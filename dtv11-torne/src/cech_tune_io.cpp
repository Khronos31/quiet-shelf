/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cech_tune_io.hpp"
#include "cech_tune_signal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace torne::cech_zd1j {
namespace {

constexpr std::uint32_t kWideRequest = 0xBC;
constexpr std::uint32_t kWideWriteType = 0x40;
constexpr std::uint32_t kWideReadType = 0xC0;
constexpr std::uint32_t kControlAddress = 0x017A3840;
constexpr std::uint32_t kProducerAddress = 0x017A3850;
constexpr std::uint32_t kProgressAddress = 0x017A389C;
constexpr std::uint32_t kRequestAddress = 0x017A3900;
constexpr std::uint32_t kCompletionAddress = 0x017A3B00;
constexpr std::size_t kRecordSize = 64;
constexpr std::size_t kSlotCount = 8;

dtv_internal_result result(std::int32_t code, std::uint32_t detail = 0)
{
    return dtv_internal_result_make(code, detail);
}

std::uint16_t packed_value(std::uint32_t address)
{
    return static_cast<std::uint16_t>((((address >> 16) & 0xFF) << 8) |
                                      ((address >> 24) & 0xFF));
}

std::uint32_t read_be32(const std::uint8_t *bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

void write_be32(std::uint8_t *bytes, std::uint32_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void write_be16(std::uint8_t *bytes, std::uint16_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 8);
    bytes[1] = static_cast<std::uint8_t>(value);
}

std::uint16_t read_be16(const std::uint8_t *bytes)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      bytes[1]);
}

dtv_internal_result control_read(const dtv_usb_io *io, dtv_timeout timeout,
                                 std::uint32_t address, std::span<std::uint8_t> bytes)
{
    if (io == nullptr || io->control == nullptr || bytes.empty() || (address & 3) != 0 ||
        address < 0x01000000 || address > 0x01FFFFFF ||
        bytes.size() > 0x02000000 - address)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    dtv_buffer buffer{bytes.data(), bytes.size(), 0};
    const auto transferred = io->control(io->user, kWideReadType, kWideRequest,
        packed_value(address), static_cast<std::uint16_t>(address), &buffer, timeout);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if (buffer.size != bytes.size())
        return result(DTV_INTERNAL_RESULT_USB_IO, static_cast<std::uint32_t>(buffer.size));
    return transferred;
}

dtv_internal_result control_write(const dtv_usb_io *io, dtv_timeout timeout,
                                  std::uint32_t address,
                                  std::span<const std::uint8_t> bytes)
{
    if (io == nullptr || io->control == nullptr || bytes.empty() || (address & 3) != 0 ||
        address < 0x01000000 || address > 0x01FFFFFF ||
        bytes.size() > 0x02000000 - address)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    dtv_buffer buffer{const_cast<std::uint8_t *>(bytes.data()), bytes.size(), bytes.size()};
    return io->control(io->user, kWideWriteType, kWideRequest, packed_value(address),
        static_cast<std::uint16_t>(address), &buffer, timeout);
}

dtv_internal_result map_i2c_status(std::uint32_t status)
{
    const auto code = status & 0xFF;
    switch (code) {
    case 0: return result(DTV_INTERNAL_RESULT_OK);
    case 4:
    case 32: return result(DTV_INTERNAL_RESULT_TIMEOUT, code);
    case 5: return result(DTV_INTERNAL_RESULT_BUSY, code);
    case 16: return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, code);
    case 17: return result(DTV_INTERNAL_RESULT_NOT_READY, code);
    case 18:
    case 33: return result(DTV_INTERNAL_RESULT_DISCONNECTED, code);
    case 1:
    case 2:
    case 3:
    case 34: return result(DTV_INTERNAL_RESULT_USB_IO);
    default: return result(DTV_INTERNAL_RESULT_USB_IO);
    }
}

}

CechI2cIo::CechI2cIo(const dtv_usb_io *io, dtv_timeout timeout,
                     const std::atomic<bool> *cancelled, I2cCursor *cursor)
    : io_(io), timeout_(timeout), cancelled_(cancelled), cursor_(cursor)
{
}

dtv_internal_result CechI2cIo::read_demodulator(std::uint16_t reg, std::uint16_t *value)
{
    if (value == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    std::uint8_t address[2]{};
    std::uint8_t response[2]{};
    write_be16(address, reg);
    const auto transferred = transact(0x24, 3, address, response);
    if (transferred.code == DTV_INTERNAL_RESULT_OK) *value = read_be16(response);
    return transferred;
}

dtv_internal_result CechI2cIo::write_demodulator(std::uint16_t reg, std::uint16_t value)
{
    std::uint8_t bytes[4]{};
    write_be16(bytes, reg);
    write_be16(bytes + 2, value);
    return transact(0x24, 1, bytes, {});
}

dtv_internal_result CechI2cIo::observe_cnr(CnrObservation *observation)
{
    if (observation == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    *observation = {};

    auto transferred = read_demodulator(568, &observation->lock_register);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    observation->lock_observed = true;
    if (!demod_fec_mpeg_locked(observation->lock_register)) {
        observation->state = CnrObservationState::invalid_unlocked;
        return result(DTV_INTERNAL_RESULT_OK);
    }

    transferred = read_demodulator(542, &observation->noise_register);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    observation->noise_observed = true;
    transferred = read_demodulator(543, &observation->signal_register);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    observation->signal_observed = true;

    if (!calculate_demod_cnr(observation->noise_register, observation->signal_register,
            &observation->noise_q16_16, &observation->signal_q16_16,
            &observation->hundredth_db)) {
        observation->state = ((observation->noise_register >> 6) & 0x00FF) == 0
            ? CnrObservationState::invalid_noise_zero
            : CnrObservationState::numeric_unresolved;
        return result(DTV_INTERNAL_RESULT_OK);
    }
    observation->state = CnrObservationState::valid;
    return result(DTV_INTERNAL_RESULT_OK);
}

DemodSyncIoResult CechI2cIo::begin_sync(DemodTransmissionMode mode, std::uint8_t guard,
                                        bool differential,
                                        bool wait_mpeg,
                                        const DemodLayerConfig (&layers)[3],
                                        DemodSyncIoState *state)
{
    DemodSyncIoResult sync_result{};
    sync_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->started) {
        sync_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return sync_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->started = true;
        state->machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
        return sync_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->started = true;
        state->machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        return sync_result;
    }

    const auto now_ms = io_->clock->monotonic_now_ns(io_->clock->user) / 1000000;
    DemodInitPlan plan{};
    if (!torne::cech_zd1j::begin_demod_sync(mode, guard, differential, wait_mpeg,
            layers, now_ms, &state->machine, &plan, &sync_result.poll_interval_ms)) {
        state->started = true;
        state->machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return sync_result;
    }
    state->started = true;
    sync_result.status = DemodSyncStatus::pending;
    for (std::size_t index = 0; index < plan.step_count; ++index) {
        const auto &step = plan.steps[index];
        if (step.target != InitTarget::demodulator ||
            (step.kind != InitStepKind::write && step.kind != InitStepKind::masked_write) ||
            step.reg > 0xFFFF || step.value > 0xFFFF || step.mask > 0xFFFF) {
            state->machine.stage = DemodSyncStage::failed;
            sync_result.status = DemodSyncStatus::failed;
            sync_result.io_result = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            return sync_result;
        }
        if (step.kind == InitStepKind::write) {
            sync_result.io_result = write_demodulator(static_cast<std::uint16_t>(step.reg),
                static_cast<std::uint16_t>(step.value));
        } else {
            std::uint16_t current = 0;
            sync_result.io_result = read_demodulator(static_cast<std::uint16_t>(step.reg),
                &current);
            if (sync_result.io_result.code == DTV_INTERNAL_RESULT_OK)
                sync_result.io_result = write_demodulator(static_cast<std::uint16_t>(step.reg),
                    static_cast<std::uint16_t>((current & step.mask) | step.value));
        }
        if (sync_result.io_result.code != DTV_INTERNAL_RESULT_OK) {
            state->machine.stage = DemodSyncStage::failed;
            sync_result.status = DemodSyncStatus::failed;
            return sync_result;
        }
        ++sync_result.plan_steps_completed;
    }
    state->machine.coff_started_ms = io_->clock->monotonic_now_ns(io_->clock->user) / 1000000;
    sync_result.immediate_plan_applied = plan.step_count != 0;
    return sync_result;
}

DemodSyncIoResult CechI2cIo::begin_sync_from_search_result(
    const DemodSearchIoState *search_state,
    const DemodSearchResultIo *search_result,
    DemodSyncIoState *state)
{
    DemodSyncIoResult sync_result{};
    sync_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->started || search_state == nullptr ||
        search_state->phase != DemodSearchIoPhase::complete || search_result == nullptr ||
        search_result->status != DemodSearchResultIoStatus::complete ||
        !search_result->raw_complete || !search_result->decoded_valid) {
        sync_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return sync_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->started = true;
        state->machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
        return sync_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->started = true;
        state->machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        return sync_result;
    }

    const auto &decoded = search_result->decoded;
    std::uint16_t register_1291 = 0;
    sync_result.io_result = read_demodulator(1291, &register_1291);
    if (sync_result.io_result.code != DTV_INTERNAL_RESULT_OK) {
        state->started = true;
        state->machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        return sync_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->started = true;
        state->machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
        return sync_result;
    }

    bool differential = false;
    if (decoded.partial_reception) {
        differential = decoded.layers[0].constellation == DemodConstellation::dqpsk ||
            (decoded.layers[1].constellation == DemodConstellation::dqpsk &&
                decoded.layers[1].segments != 0) ||
            (decoded.layers[2].constellation == DemodConstellation::dqpsk &&
                decoded.layers[2].segments != 0);
    } else {
        for (const auto &layer : decoded.layers)
            differential = differential ||
                (layer.constellation == DemodConstellation::dqpsk && layer.segments != 0);
    }
    return begin_sync(decoded.mode, decoded.guard, differential,
        (register_1291 & 0x0200) != 0, decoded.layers, state);
}

DemodSyncIoResult CechI2cIo::poll_sync(DemodSyncIoState *state)
{
    DemodSyncIoResult sync_result{};
    sync_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || !state->started) {
        sync_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return sync_result;
    }

    auto &machine = state->machine;
    if (machine.stage == DemodSyncStage::complete ||
        machine.stage == DemodSyncStage::failed ||
        machine.stage == DemodSyncStage::recovery_failed) {
        DemodInitPlan plan{};
        sync_result.status = torne::cech_zd1j::advance_demod_sync(
            &machine, true, true, true, 0, 0, nullptr, &plan,
            &sync_result.poll_interval_ms);
        return sync_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
        return sync_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        return sync_result;
    }

    const auto now_ms = io_->clock->monotonic_now_ns(io_->clock->user) / 1000000;
    if (machine.stage == DemodSyncStage::await_coff_cpil &&
        now_ms < machine.coff_started_ms + 10U) {
        sync_result.status = DemodSyncStatus::pending;
        sync_result.poll_interval_ms = 10;
        return sync_result;
    }
    if (machine.stage == DemodSyncStage::viterbi_start ||
        machine.stage == DemodSyncStage::failure_recovery) {
        DemodInitPlan plan{};
        sync_result.status = torne::cech_zd1j::advance_demod_sync(
            &machine, true, true, true, now_ms, 0, nullptr, &plan,
            &sync_result.poll_interval_ms);
        return sync_result;
    }

    std::uint16_t register_568 = 0;
    sync_result.io_result = read_demodulator(568, &register_568);
    if (sync_result.io_result.code != DTV_INTERNAL_RESULT_OK) {
        DemodInitPlan plan{};
        sync_result.status = torne::cech_zd1j::advance_demod_sync(
            &machine, false, false, false,
            io_->clock->monotonic_now_ns(io_->clock->user) / 1000000,
            0, nullptr, &plan,
            &sync_result.poll_interval_ms);
        return sync_result;
    }

    const std::uint16_t viterbi_registers[3]{};
    LockObservation observation{};
    if (observe_lock(true, register_568, viterbi_registers, &observation) ==
        LockReadResult::read_error) {
        machine.stage = DemodSyncStage::failed;
        sync_result.status = DemodSyncStatus::failed;
        sync_result.io_result = result(DTV_INTERNAL_RESULT_INTERNAL);
        return sync_result;
    }

    std::uint16_t register_188 = 0;
    bool register_188_read_succeeded = true;
    if (machine.stage == DemodSyncStage::await_coff_cpil && observation.coff_cpil &&
        !machine.differential && !observation.lmod4) {
        sync_result.io_result = read_demodulator(188, &register_188);
        register_188_read_succeeded = sync_result.io_result.code == DTV_INTERNAL_RESULT_OK;
    }

    const auto observation_now_ms =
        io_->clock->monotonic_now_ns(io_->clock->user) / 1000000;
    DemodInitPlan plan{};
    sync_result.status = torne::cech_zd1j::advance_demod_sync(
        &machine, true, register_188_read_succeeded, true, observation_now_ms, register_188,
        &observation, &plan, &sync_result.poll_interval_ms);
    if (!register_188_read_succeeded) return sync_result;
    if (plan.step_count == 0) return sync_result;

    sync_result.recovery_attempted = machine.stage == DemodSyncStage::failure_recovery;
    for (std::size_t index = 0; index < plan.step_count; ++index) {
        const auto &step = plan.steps[index];
        if (step.target != InitTarget::demodulator || step.reg > 0xFFFF ||
            step.value > 0xFFFF || step.mask > 0xFFFF) {
            sync_result.io_result = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            break;
        }
        if (step.kind == InitStepKind::write) {
            sync_result.io_result = write_demodulator(static_cast<std::uint16_t>(step.reg),
                static_cast<std::uint16_t>(step.value));
        } else if (step.kind == InitStepKind::masked_write) {
            std::uint16_t current = 0;
            sync_result.io_result = read_demodulator(static_cast<std::uint16_t>(step.reg),
                &current);
            if (sync_result.io_result.code == DTV_INTERNAL_RESULT_OK)
                sync_result.io_result = write_demodulator(static_cast<std::uint16_t>(step.reg),
                    static_cast<std::uint16_t>((current & step.mask) | step.value));
        } else {
            sync_result.io_result = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        }
        if (sync_result.io_result.code != DTV_INTERNAL_RESULT_OK) break;
        ++sync_result.plan_steps_completed;
    }
    if (sync_result.io_result.code != DTV_INTERNAL_RESULT_OK) {
        DemodInitPlan recovery_plan{};
        sync_result.status = torne::cech_zd1j::advance_demod_sync(
            &machine, true, true, false, observation_now_ms, 0, nullptr, &recovery_plan,
            &sync_result.poll_interval_ms);
        return sync_result;
    }
    sync_result.immediate_plan_applied = true;
    if (sync_result.recovery_attempted) sync_result.recovery_succeeded = true;
    if (machine.stage == DemodSyncStage::viterbi_start ||
        machine.stage == DemodSyncStage::failure_recovery) {
        const auto plan_applied_now_ms =
            io_->clock->monotonic_now_ns(io_->clock->user) / 1000000;
        DemodInitPlan completed_plan{};
        sync_result.status = torne::cech_zd1j::advance_demod_sync(
            &machine, true, true, true, plan_applied_now_ms, 0, &observation, &completed_plan,
            &sync_result.poll_interval_ms);
    }
    return sync_result;
}

DemodSearchIoResult CechI2cIo::begin_search(std::uint16_t revision, bool mode_auto,
                                            bool guard_auto,
                                            DemodTransmissionMode requested_mode,
                                            std::uint8_t requested_guard,
                                            const DemodChannelConfig *channel,
                                            DemodSearchIoState *state)
{
    DemodSearchIoResult search_result{};
    search_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (channel == nullptr || state == nullptr || state->phase != DemodSearchIoPhase::idle) {
        search_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return search_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DemodSearchIoPhase::failed;
        search_result.status = DemodSearchIoStatus::failed;
        search_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
        return search_result;
    }
    if (!torne::cech_zd1j::select_demod_search(revision, mode_auto, guard_auto,
            requested_mode, requested_guard, &state->selection)) {
        state->phase = DemodSearchIoPhase::failed;
        search_result.status = DemodSearchIoStatus::failed;
        search_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return search_result;
    }
    state->channel = *channel;
    bool plan_built = false;
    if (state->selection.strategy == DemodSearchStrategy::two_pass) {
        plan_built = torne::cech_zd1j::build_demod_fft_search_plan(&state->plan);
        state->phase = DemodSearchIoPhase::fft_plan;
        if (plan_built) {
            state->fft_read_start_index = state->plan.step_count;
            for (std::size_t index = 0; index < state->plan.step_count; ++index) {
                const auto &step = state->plan.steps[index];
                if (step.kind == InitStepKind::read && step.reg == 594) {
                    state->fft_read_start_index = index;
                    break;
                }
            }
            if (state->fft_read_start_index == state->plan.step_count)
                plan_built = false;
        }
    } else {
        plan_built = torne::cech_zd1j::build_demod_one_pass_search_plan(
            &state->selection, &state->channel, &state->plan);
        state->phase = DemodSearchIoPhase::one_pass_plan;
    }
    if (!plan_built) {
        state->phase = DemodSearchIoPhase::failed;
        search_result.status = DemodSearchIoStatus::failed;
        search_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return search_result;
    }
    return poll_search(state);
}

DemodSearchIoResult CechI2cIo::poll_search(DemodSearchIoState *state)
{
    DemodSearchIoResult search_result{};
    search_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->phase == DemodSearchIoPhase::idle) {
        search_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return search_result;
    }
    if (state->phase == DemodSearchIoPhase::complete) {
        search_result.status = DemodSearchIoStatus::complete;
        return search_result;
    }
    if (state->phase == DemodSearchIoPhase::guard_plan_applied) {
        search_result.status = DemodSearchIoStatus::guard_plan_applied;
        return search_result;
    }
    if (state->phase == DemodSearchIoPhase::failed) {
        search_result.status = DemodSearchIoStatus::failed;
        return search_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DemodSearchIoPhase::failed;
        search_result.status = DemodSearchIoStatus::failed;
        search_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
        return search_result;
    }

    while (state->plan_step_index < state->plan.step_count) {
        const auto &step = state->plan.steps[state->plan_step_index];
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
            state->phase = DemodSearchIoPhase::failed;
            search_result.status = DemodSearchIoStatus::failed;
            search_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
            return search_result;
        }
        if (step.kind == InitStepKind::delay_ms) {
            ++state->plan_step_index;
            search_result.status = DemodSearchIoStatus::pending;
            search_result.poll_interval_ms = step.value;
            search_result.plan_steps_completed = state->plan_steps_completed;
            search_result.immediate_plan_applied = state->plan_steps_completed != 0;
            return search_result;
        }
        if (step.target != InitTarget::demodulator || step.reg > 0xFFFF ||
            step.value > 0xFFFF || step.mask > 0xFFFF) {
            state->phase = DemodSearchIoPhase::failed;
            search_result.status = DemodSearchIoStatus::failed;
            search_result.io_result = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            return search_result;
        }

        if (step.kind == InitStepKind::read) {
            std::uint16_t value = 0;
            search_result.io_result = read_demodulator(static_cast<std::uint16_t>(step.reg),
                &value);
            if (search_result.io_result.code == DTV_INTERNAL_RESULT_OK) {
                state->saved_register = static_cast<std::uint16_t>(step.reg);
                state->saved_value = value;
                state->saved_value_valid = true;
                if (step.reg == 0) {
                    state->restart_register = static_cast<std::uint16_t>(step.reg);
                    state->restart_value = value;
                    state->restart_value_valid = true;
                }
                if (state->phase == DemodSearchIoPhase::fft_plan &&
                    step.reg >= 594 && step.reg <= 599)
                    state->fft_correlation_words[step.reg - 594] = value;
                if (state->phase == DemodSearchIoPhase::fft_plan && step.reg == 1284)
                    state->fft_status_register = value;
            }
        } else if (step.kind == InitStepKind::write) {
            search_result.io_result = write_demodulator(static_cast<std::uint16_t>(step.reg),
                static_cast<std::uint16_t>(step.value));
        } else if (step.kind == InitStepKind::masked_write) {
            std::uint16_t current = 0;
            search_result.io_result = read_demodulator(static_cast<std::uint16_t>(step.reg),
                &current);
            if (search_result.io_result.code == DTV_INTERNAL_RESULT_OK && cancelled_ != nullptr &&
                cancelled_->load(std::memory_order_acquire))
                search_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
            if (search_result.io_result.code == DTV_INTERNAL_RESULT_OK)
                search_result.io_result = write_demodulator(static_cast<std::uint16_t>(step.reg),
                    static_cast<std::uint16_t>((current & step.mask) | step.value));
        } else if (step.kind == InitStepKind::write_saved) {
            if (!state->restart_value_valid || state->restart_register != step.reg) {
                search_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
            } else {
                search_result.io_result = write_demodulator(state->restart_register,
                    state->restart_value);
            }
        } else {
            search_result.io_result = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        }
        if (search_result.io_result.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = DemodSearchIoPhase::failed;
            search_result.status = DemodSearchIoStatus::failed;
            search_result.plan_steps_completed = state->plan_steps_completed;
            return search_result;
        }
        ++state->plan_step_index;
        ++state->plan_steps_completed;
    }

    search_result.immediate_plan_applied = state->plan_steps_completed != 0;
    search_result.plan_steps_completed = state->plan_steps_completed;
    if (state->phase == DemodSearchIoPhase::fft_plan) {
        if ((state->fft_status_register & 0x0001) == 0) {
            state->plan_step_index = state->fft_read_start_index;
            search_result.status = DemodSearchIoStatus::pending;
            search_result.poll_interval_ms = 10;
            return search_result;
        }
        const std::uint32_t correlations[3] = {
            (static_cast<std::uint32_t>(state->fft_correlation_words[4]) << 16) |
                state->fft_correlation_words[5],
            (static_cast<std::uint32_t>(state->fft_correlation_words[2]) << 16) |
                state->fft_correlation_words[3],
            (static_cast<std::uint32_t>(state->fft_correlation_words[0]) << 16) |
                state->fft_correlation_words[1]
        };
        const auto resolution = torne::cech_zd1j::resolve_demod_fft_search(true,
            correlations);
        switch (resolution) {
        case DemodFftSearchResult::mode_2k:
            state->selection.mode = DemodTransmissionMode::mode_2k;
            break;
        case DemodFftSearchResult::mode_4k:
            state->selection.mode = DemodTransmissionMode::mode_4k;
            break;
        case DemodFftSearchResult::mode_8k:
            state->selection.mode = DemodTransmissionMode::mode_8k;
            break;
        case DemodFftSearchResult::pending:
            state->phase = DemodSearchIoPhase::failed;
            search_result.status = DemodSearchIoStatus::failed;
            search_result.io_result = result(DTV_INTERNAL_RESULT_NOT_READY);
            return search_result;
        }
        if (!torne::cech_zd1j::build_demod_guard_search_plan(state->selection.mode,
                &state->channel, &state->plan)) {
            state->phase = DemodSearchIoPhase::failed;
            search_result.status = DemodSearchIoStatus::failed;
            search_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
            return search_result;
        }
        state->phase = DemodSearchIoPhase::guard_plan;
        state->plan_step_index = 0;
        return poll_search(state);
    }
    if (state->phase == DemodSearchIoPhase::guard_plan) {
        if (!state->saved_value_valid || state->saved_register != 570) {
            state->phase = DemodSearchIoPhase::failed;
            search_result.status = DemodSearchIoStatus::failed;
            search_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
            return search_result;
        }
        state->selection.guard = static_cast<std::uint8_t>(state->saved_value & 0x0003);
        state->phase = DemodSearchIoPhase::guard_plan_applied;
        search_result.status = DemodSearchIoStatus::guard_plan_applied;
        search_result.plan_steps_completed = state->plan_steps_completed;
        return search_result;
    }

    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DemodSearchIoPhase::failed;
        search_result.status = DemodSearchIoStatus::failed;
        search_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
        return search_result;
    }
    std::uint16_t status_register = 0;
    search_result.io_result = read_demodulator(1284, &status_register);
    if (search_result.io_result.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = DemodSearchIoPhase::failed;
        search_result.status = DemodSearchIoStatus::failed;
        return search_result;
    }
    if ((status_register & 0x0001) != 0) {
        state->phase = DemodSearchIoPhase::failed;
        search_result.status = DemodSearchIoStatus::failed;
        return search_result;
    }
    if ((status_register & 0x0002) == 0) {
        search_result.status = DemodSearchIoStatus::pending;
        return search_result;
    }
    state->phase = DemodSearchIoPhase::complete;
    search_result.status = DemodSearchIoStatus::complete;
    return search_result;
}

DemodSearchResultIo CechI2cIo::read_search_result(const DemodSearchIoState *state)
{
    DemodSearchResultIo search_result{};
    search_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->phase != DemodSearchIoPhase::complete) {
        search_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return search_result;
    }

    auto read_register = [&](std::uint16_t reg, std::uint16_t *value) {
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
            search_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
            return false;
        }
        search_result.io_result = read_demodulator(reg, value);
        if (search_result.io_result.code != DTV_INTERNAL_RESULT_OK) return false;
        ++search_result.read_count;
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
            search_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
            return false;
        }
        return true;
    };

    if (!read_register(568, &search_result.register_568)) return search_result;
    if ((search_result.register_568 & 0x001E) != 0x001E) {
        search_result.status = DemodSearchResultIoStatus::tmcc_not_established;
        search_result.io_result = result(DTV_INTERNAL_RESULT_NOT_READY);
        return search_result;
    }
    if (!read_register(508, &search_result.register_508) ||
        !read_register(570, &search_result.register_570) ||
        !read_register(505, &search_result.register_505))
        return search_result;
    for (std::size_t index = 0; index < 3; ++index) {
        if (!read_register(static_cast<std::uint16_t>(493 + index),
                &search_result.register_493[index]) ||
            !read_register(static_cast<std::uint16_t>(499 + index),
                &search_result.register_499[index]) ||
            !read_register(static_cast<std::uint16_t>(481 + index),
                &search_result.register_481[index]) ||
            !read_register(static_cast<std::uint16_t>(487 + index),
                &search_result.register_487[index]))
            return search_result;
    }
    search_result.raw_complete = true;
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        search_result.io_result = result(DTV_INTERNAL_RESULT_CANCELLED);
        return search_result;
    }

    torne::cech_zd1j::decode_demod_search_result(search_result.register_570,
        search_result.register_505, search_result.register_493,
        search_result.register_499, search_result.register_481,
        search_result.register_487, &search_result.decoded);
    search_result.decoded_valid = true;
    search_result.status = DemodSearchResultIoStatus::complete;
    return search_result;
}

dtv_internal_result CechI2cIo::read_tuner(std::uint8_t reg, std::uint16_t *value)
{
    if (value == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    const std::uint8_t address[1]{reg};
    std::uint8_t response[2]{};
    const auto transferred = transact(0xC0, 3, address, response);
    if (transferred.code == DTV_INTERNAL_RESULT_OK) *value = read_be16(response);
    return transferred;
}

dtv_internal_result CechI2cIo::write_tuner(std::uint8_t reg, std::uint16_t value)
{
    std::uint8_t bytes[3]{reg, 0, 0};
    write_be16(bytes + 1, value);
    return transact(0xC0, 1, bytes, {});
}

dtv_internal_result CechI2cIo::apply_tune_plan(const TunePlan &plan)
{
    // 公開tune caller未接合。呼出し側は最初のI/O前にbegin_effectを呼ぶ。
    if (plan.write_count > sizeof(plan.writes) / sizeof(plan.writes[0]))
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    for (std::size_t index = 0; index < plan.write_count; ++index) {
        const auto &write = plan.writes[index];
        const auto transferred = write_tuner(write.address, write.value);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }
    return result(DTV_INTERNAL_RESULT_OK);
}

WbdCalibrationIoResult CechI2cIo::begin_wbd_calibration(
    std::uint64_t frequency_hz, std::uint16_t previous_gain,
    std::uint16_t previous_offset, WbdCalibrationIoState *state)
{
    WbdCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->phase != WbdCalibrationIoPhase::idle) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    if (!build_wbd_calibration_plan(frequency_hz, previous_gain, &state->plan)) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    state->previous_offset = previous_offset;
    if (state->plan.complete) {
        if (!complete_wbd_calibration(&state->plan, false, 0, previous_offset,
                                      &state->calibration)) {
            state->phase = WbdCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
            calibration_result.io_result = state->failure;
            return calibration_result;
        }
        state->phase = WbdCalibrationIoPhase::complete;
        calibration_result.status = WbdCalibrationIoStatus::complete;
        calibration_result.calibration_valid = true;
        calibration_result.calibration = state->calibration;
        return calibration_result;
    }

    if (!state->plan.wbd_calibration || !state->plan.needs_measurement ||
        state->plan.measurement_register != 0x1D || io_ == nullptr || io_->clock == nullptr ||
        io_->clock->monotonic_now_ns == nullptr) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    for (std::size_t index = 0; index < state->plan.step_count; ++index) {
        const auto &step = state->plan.steps[index];
        if (step.target != InitTarget::tuner || step.kind != InitStepKind::write ||
            step.reg > 0xFF || step.value > 0xFFFF) {
            state->phase = WbdCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            calibration_result.io_result = state->failure;
            return calibration_result;
        }
        state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
            static_cast<std::uint16_t>(step.value));
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = WbdCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    const auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    const auto wait_ms = static_cast<std::uint64_t>(10U *
        (static_cast<std::uint32_t>(state->plan.wait_tenth_ms) + 99U) / 100U);
    const auto wait_ns = wait_ms * 1000000ULL;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    state->measurement_ready_ns = now_ns > maximum - wait_ns ? maximum : now_ns + wait_ns;
    state->phase = WbdCalibrationIoPhase::waiting_measurement;
    calibration_result.status = WbdCalibrationIoStatus::pending;
    calibration_result.poll_interval_ms = static_cast<std::uint32_t>(wait_ms);
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    return calibration_result;
}

WbdCalibrationIoResult CechI2cIo::poll_wbd_calibration(WbdCalibrationIoState *state)
{
    WbdCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (state->phase == WbdCalibrationIoPhase::complete) {
        calibration_result.status = WbdCalibrationIoStatus::complete;
        calibration_result.calibration_valid = true;
        calibration_result.calibration = state->calibration;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (state->phase == WbdCalibrationIoPhase::failed) {
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (state->phase != WbdCalibrationIoPhase::waiting_measurement) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    const auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (now_ns < state->measurement_ready_ns) {
        const auto remaining_ns = state->measurement_ready_ns - now_ns;
        auto remaining_ms = remaining_ns / 1000000U +
            (remaining_ns % 1000000U != 0 ? 1U : 0U);
        if (timeout_.operation_deadline_ns != 0) {
            const auto remaining_deadline_ns = timeout_.operation_deadline_ns - now_ns;
            const auto remaining_deadline_ms = remaining_deadline_ns / 1000000U +
                (remaining_deadline_ns % 1000000U != 0 ? 1U : 0U);
            remaining_ms = std::min(remaining_ms, remaining_deadline_ms);
        }
        calibration_result.status = WbdCalibrationIoStatus::pending;
        calibration_result.poll_interval_ms = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining_ms,
                std::numeric_limits<std::uint32_t>::max()));
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    std::uint16_t measurement = 0;
    state->failure = read_tuner(static_cast<std::uint8_t>(state->plan.measurement_register),
        &measurement);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = WbdCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (!complete_wbd_calibration(&state->plan, true, measurement,
                                  state->previous_offset, &state->calibration)) {
        state->phase = WbdCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    state->phase = WbdCalibrationIoPhase::complete;
    calibration_result.status = WbdCalibrationIoStatus::complete;
    calibration_result.calibration_valid = true;
    calibration_result.calibration = state->calibration;
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    return calibration_result;
}

DcCalibrationIoResult CechI2cIo::begin_dc_calibration_io(
    TunerVariant variant, DcCalibrationIoState *state)
{
    DcCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->phase != DcCalibrationIoPhase::idle ||
        (variant != TunerVariant::standard && variant != TunerVariant::p1g)) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    std::uint16_t register_24 = 0;
    state->failure = read_tuner(0x24, &register_24);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = DcCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    std::uint16_t wbd_mux = 0;
    state->failure = read_tuner(0x10, &wbd_mux);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = DcCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    std::uint16_t register_23 = 0;
    state->failure = read_tuner(0x23, &register_23);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = DcCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    TunerCalibrationPlan plan{};
    if (!torne::cech_zd1j::begin_dc_calibration(variant, register_24, wbd_mux,
            register_23, &state->machine, &plan) ||
        plan.step_count > sizeof(plan.steps) / sizeof(plan.steps[0])) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    for (std::size_t index = 0; index < plan.step_count; ++index) {
        const auto &step = plan.steps[index];
        if (step.target != InitTarget::tuner || step.kind != InitStepKind::write ||
            step.reg > 0xFF || step.value > 0xFFFF) {
            state->phase = DcCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
            static_cast<std::uint16_t>(step.value));
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = DcCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    const auto wait_ms = static_cast<std::uint64_t>(10U *
        (static_cast<std::uint32_t>(plan.wait_tenth_ms) + 99U) / 100U);
    const auto wait_ns = wait_ms * 1000000ULL;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    state->next_stage_ns = now_ns > maximum - wait_ns ? maximum : now_ns + wait_ns;
    state->phase = DcCalibrationIoPhase::waiting;
    calibration_result.status = DcCalibrationIoStatus::pending;
    calibration_result.poll_interval_ms = static_cast<std::uint32_t>(wait_ms);
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    return calibration_result;
}

DcCalibrationIoResult CechI2cIo::poll_dc_calibration_io(DcCalibrationIoState *state)
{
    DcCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (state->phase == DcCalibrationIoPhase::complete) {
        calibration_result.status = DcCalibrationIoStatus::complete;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        calibration_result.calibration_flags_to_clear = state->calibration_flags_to_clear;
        return calibration_result;
    }
    if (state->phase == DcCalibrationIoPhase::failed) {
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (state->phase != DcCalibrationIoPhase::waiting) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (now_ns < state->next_stage_ns) {
        const auto remaining_ns = state->next_stage_ns - now_ns;
        auto remaining_ms = remaining_ns / 1000000U +
            (remaining_ns % 1000000U != 0 ? 1U : 0U);
        if (timeout_.operation_deadline_ns != 0) {
            const auto remaining_deadline_ns = timeout_.operation_deadline_ns - now_ns;
            const auto remaining_deadline_ms = remaining_deadline_ns / 1000000U +
                (remaining_deadline_ns % 1000000U != 0 ? 1U : 0U);
            remaining_ms = std::min(remaining_ms, remaining_deadline_ms);
        }
        calibration_result.status = DcCalibrationIoStatus::pending;
        calibration_result.poll_interval_ms = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining_ms,
                std::numeric_limits<std::uint32_t>::max()));
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    bool measurement_available = false;
    std::uint16_t measurement = 0;
    if (state->machine.stage == 3 || state->machine.stage == 4) {
        state->failure = read_tuner(0x1D, &measurement);
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = DcCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        measurement_available = true;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    TunerCalibrationPlan plan{};
    const auto status = torne::cech_zd1j::advance_dc_calibration(&state->machine,
        measurement_available, measurement, &plan);
    if (status == DcCalibrationStatus::failed ||
        plan.step_count > sizeof(plan.steps) / sizeof(plan.steps[0])) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if ((status == DcCalibrationStatus::complete) != plan.complete) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    for (std::size_t index = 0; index < plan.step_count; ++index) {
        const auto &step = plan.steps[index];
        if (step.target != InitTarget::tuner || step.kind != InitStepKind::write ||
            step.reg > 0xFF || step.value > 0xFFFF) {
            state->phase = DcCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
            static_cast<std::uint16_t>(step.value));
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = DcCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
    }
    if (status == DcCalibrationStatus::complete) {
        state->phase = DcCalibrationIoPhase::complete;
        state->calibration_flags_to_clear = plan.calibration_flags_to_clear;
        calibration_result.status = DcCalibrationIoStatus::complete;
        calibration_result.calibration_flags_to_clear = state->calibration_flags_to_clear;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    std::uint16_t wait_tenth_ms = 0;
    if (!dc_calibration_wait(state->machine.stage, &wait_tenth_ms)) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = DcCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    const auto wait_ms = static_cast<std::uint64_t>(10U *
        (static_cast<std::uint32_t>(wait_tenth_ms) + 99U) / 100U);
    const auto wait_ns = wait_ms * 1000000ULL;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    state->next_stage_ns = now_ns > maximum - wait_ns ? maximum : now_ns + wait_ns;
    calibration_result.status = DcCalibrationIoStatus::pending;
    calibration_result.poll_interval_ms = static_cast<std::uint32_t>(wait_ms);
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    return calibration_result;
}

TemperatureCalibrationIoResult CechI2cIo::begin_temperature_calibration_io(
    TemperatureCalibrationIoState *state)
{
    TemperatureCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->phase != TemperatureCalibrationIoPhase::idle) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    std::uint16_t wbd_mux = 0;
    state->failure = read_tuner(0x10, &wbd_mux);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    const auto temperature_mux = static_cast<std::uint16_t>(
        (wbd_mux & ~0x07F8) | 0x0040);
    state->failure = write_tuner(0x10, temperature_mux);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    ++state->plan_steps_completed;
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    std::uint16_t register_13 = 0;
    state->failure = read_tuner(0x13, &register_13);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    if (!torne::cech_zd1j::begin_temperature_calibration(wbd_mux, register_13,
            &state->machine, &state->plan) ||
        state->plan.step_count > sizeof(state->plan.steps) / sizeof(state->plan.steps[0]) ||
        state->plan.step_count != 2 || state->plan.steps[0].target != InitTarget::tuner ||
        state->plan.steps[0].kind != InitStepKind::write ||
        state->plan.steps[0].reg != 0x10 || state->plan.steps[0].value != temperature_mux ||
        state->plan.steps[1].target != InitTarget::tuner ||
        state->plan.steps[1].kind != InitStepKind::write ||
        state->plan.steps[1].reg != 0x13 ||
        !state->plan.needs_measurement || state->plan.measurement_register != 0x1D ||
        state->plan.wait_tenth_ms != 15) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    for (std::size_t index = 1; index < state->plan.step_count; ++index) {
        const auto &step = state->plan.steps[index];
        if (step.target != InitTarget::tuner || step.kind != InitStepKind::write ||
            step.reg > 0xFF || step.value > 0xFFFF) {
            state->phase = TemperatureCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
            static_cast<std::uint16_t>(step.value));
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = TemperatureCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    const auto wait_ms = static_cast<std::uint64_t>(10U *
        (static_cast<std::uint32_t>(state->plan.wait_tenth_ms) + 99U) / 100U);
    const auto wait_ns = wait_ms * 1000000ULL;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    state->measurement_ready_ns = now_ns > maximum - wait_ns ? maximum : now_ns + wait_ns;
    state->phase = TemperatureCalibrationIoPhase::waiting_first_measurement;
    calibration_result.status = TemperatureCalibrationIoStatus::pending;
    calibration_result.poll_interval_ms = static_cast<std::uint32_t>(wait_ms);
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    return calibration_result;
}

TemperatureCalibrationIoResult CechI2cIo::poll_temperature_calibration_io(
    TemperatureCalibrationIoState *state)
{
    TemperatureCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (state->phase == TemperatureCalibrationIoPhase::complete) {
        calibration_result.status = TemperatureCalibrationIoStatus::complete;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        calibration_result.temperature_valid = true;
        calibration_result.temperature = state->temperature;
        calibration_result.calibration_flags_to_clear = state->calibration_flags_to_clear;
        return calibration_result;
    }
    if (state->phase == TemperatureCalibrationIoPhase::failed) {
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (state->phase != TemperatureCalibrationIoPhase::waiting_first_measurement &&
        state->phase != TemperatureCalibrationIoPhase::waiting_second_measurement) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (now_ns < state->measurement_ready_ns) {
        const auto remaining_ns = state->measurement_ready_ns - now_ns;
        auto remaining_ms = remaining_ns / 1000000U +
            (remaining_ns % 1000000U != 0 ? 1U : 0U);
        if (timeout_.operation_deadline_ns != 0) {
            const auto remaining_deadline_ns = timeout_.operation_deadline_ns - now_ns;
            const auto remaining_deadline_ms = remaining_deadline_ns / 1000000U +
                (remaining_deadline_ns % 1000000U != 0 ? 1U : 0U);
            remaining_ms = std::min(remaining_ms, remaining_deadline_ms);
        }
        calibration_result.status = TemperatureCalibrationIoStatus::pending;
        calibration_result.poll_interval_ms = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining_ms,
                std::numeric_limits<std::uint32_t>::max()));
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    std::uint16_t measurement = 0;
    state->failure = read_tuner(static_cast<std::uint8_t>(state->plan.measurement_register),
        &measurement);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    std::int16_t completed_temperature = 0;
    if (state->phase == TemperatureCalibrationIoPhase::waiting_first_measurement) {
        if (!torne::cech_zd1j::continue_temperature_calibration(&state->machine,
                static_cast<std::int16_t>(measurement), &state->plan) ||
            state->plan.step_count > sizeof(state->plan.steps) / sizeof(state->plan.steps[0]) ||
            state->plan.step_count != 1 || state->plan.steps[0].target != InitTarget::tuner ||
            state->plan.steps[0].kind != InitStepKind::write ||
            state->plan.steps[0].reg != 0x13 ||
            !state->plan.needs_measurement || state->plan.measurement_register != 0x1D ||
            state->plan.wait_tenth_ms != 15) {
            state->phase = TemperatureCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        for (std::size_t index = 0; index < state->plan.step_count; ++index) {
            const auto &step = state->plan.steps[index];
            if (step.target != InitTarget::tuner || step.kind != InitStepKind::write ||
                step.reg > 0xFF || step.value > 0xFFFF) {
                state->phase = TemperatureCalibrationIoPhase::failed;
                state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
                calibration_result.io_result = state->failure;
                calibration_result.plan_steps_completed = state->plan_steps_completed;
                return calibration_result;
            }
            state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
                static_cast<std::uint16_t>(step.value));
            if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
                state->phase = TemperatureCalibrationIoPhase::failed;
                calibration_result.io_result = state->failure;
                calibration_result.plan_steps_completed = state->plan_steps_completed;
                return calibration_result;
            }
            ++state->plan_steps_completed;
        }
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
            state->phase = TemperatureCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
        if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
            state->phase = TemperatureCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        const auto wait_ms = static_cast<std::uint64_t>(10U *
            (static_cast<std::uint32_t>(state->plan.wait_tenth_ms) + 99U) / 100U);
        const auto wait_ns = wait_ms * 1000000ULL;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        state->measurement_ready_ns = now_ns > maximum - wait_ns ? maximum : now_ns + wait_ns;
        state->phase = TemperatureCalibrationIoPhase::waiting_second_measurement;
        calibration_result.status = TemperatureCalibrationIoStatus::pending;
        calibration_result.poll_interval_ms = static_cast<std::uint32_t>(wait_ms);
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    if (!torne::cech_zd1j::complete_temperature_calibration(&state->machine, true,
            static_cast<std::int16_t>(measurement), &completed_temperature, &state->plan) ||
        state->plan.step_count > sizeof(state->plan.steps) / sizeof(state->plan.steps[0]) ||
        state->plan.step_count != 2 || state->plan.steps[0].target != InitTarget::tuner ||
        state->plan.steps[0].kind != InitStepKind::write ||
        state->plan.steps[0].reg != 0x13 ||
        state->plan.steps[0].value != state->machine.saved_register_13 ||
        state->plan.steps[1].target != InitTarget::tuner ||
        state->plan.steps[1].kind != InitStepKind::write ||
        state->plan.steps[1].reg != 0x10 ||
        state->plan.steps[1].value != state->machine.wbd_mux ||
        !state->plan.complete || state->plan.calibration_flags_to_clear !=
            tuner_calibration_temp) {
        state->phase = TemperatureCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    for (std::size_t index = 0; index < state->plan.step_count; ++index) {
        const auto &step = state->plan.steps[index];
        if (step.target != InitTarget::tuner || step.kind != InitStepKind::write ||
            step.reg > 0xFF || step.value > 0xFFFF) {
            state->phase = TemperatureCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
            static_cast<std::uint16_t>(step.value));
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = TemperatureCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
    }

    state->temperature = completed_temperature;
    state->calibration_flags_to_clear = state->plan.calibration_flags_to_clear;
    state->phase = TemperatureCalibrationIoPhase::complete;
    calibration_result.status = TemperatureCalibrationIoStatus::complete;
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    calibration_result.temperature_valid = true;
    calibration_result.temperature = state->temperature;
    calibration_result.calibration_flags_to_clear = state->calibration_flags_to_clear;
    return calibration_result;
}

NormalCapTrimCalibrationIoResult CechI2cIo::begin_normal_captrim_calibration_io(
    bool pll_fraction_zero, NormalCapTrimCalibrationIoState *state)
{
    NormalCapTrimCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->phase != NormalCapTrimCalibrationIoPhase::idle) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    if (!torne::cech_zd1j::begin_captrim_calibration(false, 0, &state->machine) ||
        state->machine.retune || state->machine.candidate != 64 ||
        state->machine.step != 64 || state->machine.best_candidate != 64 ||
        state->machine.minimum_difference != 3000 ||
        !torne::cech_zd1j::build_captrim_start_plan(&state->plan) ||
        !state->plan.complete || state->plan.step_count != 2 ||
        state->plan.steps[0].target != InitTarget::tuner ||
        state->plan.steps[0].kind != InitStepKind::write ||
        state->plan.steps[0].reg != 0x10 || state->plan.steps[0].value != 0x02B1 ||
        state->plan.steps[1].target != InitTarget::tuner ||
        state->plan.steps[1].kind != InitStepKind::write ||
        state->plan.steps[1].reg != 0x1E || state->plan.steps[1].value != 0x0032) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    state->pll_fraction_zero = pll_fraction_zero;
    for (std::size_t index = 0; index < state->plan.step_count; ++index) {
        const auto &step = state->plan.steps[index];
        state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
            static_cast<std::uint16_t>(step.value));
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = NormalCapTrimCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    state->failure = write_tuner(0x18,
        torne::cech_zd1j::captrim_register_value(state->machine.candidate));
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    ++state->plan_steps_completed;
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    std::uint16_t measurement = 0;
    state->failure = read_tuner(0x1D, &measurement);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    if (!torne::cech_zd1j::complete_captrim_measurement(&state->machine, true,
            measurement, state->pll_fraction_zero, &state->calibration) ||
        !state->calibration.needs_measurement || state->calibration.complete ||
        state->calibration.wait_tenth_ms != 25 ||
        state->calibration.calibration_flags_to_clear != 0) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    state->failure = write_tuner(0x18, state->calibration.register_value);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    ++state->plan_steps_completed;
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    const auto wait_ms = static_cast<std::uint64_t>(10U *
        (static_cast<std::uint32_t>(state->calibration.wait_tenth_ms) + 99U) / 100U);
    const auto wait_ns = wait_ms * 1000000ULL;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    state->measurement_ready_ns = now_ns > maximum - wait_ns ? maximum : now_ns + wait_ns;
    state->phase = NormalCapTrimCalibrationIoPhase::waiting_measurement;
    calibration_result.status = NormalCapTrimCalibrationIoStatus::pending;
    calibration_result.poll_interval_ms = static_cast<std::uint32_t>(wait_ms);
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    return calibration_result;
}

NormalCapTrimCalibrationIoResult CechI2cIo::poll_normal_captrim_calibration_io(
    NormalCapTrimCalibrationIoState *state)
{
    NormalCapTrimCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (state->phase == NormalCapTrimCalibrationIoPhase::complete) {
        calibration_result.status = NormalCapTrimCalibrationIoStatus::complete;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        calibration_result.register_value = state->calibration.register_value;
        calibration_result.calibration_valid = true;
        calibration_result.calibration_flags_to_clear =
            state->calibration.calibration_flags_to_clear;
        return calibration_result;
    }
    if (state->phase == NormalCapTrimCalibrationIoPhase::failed) {
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (state->phase != NormalCapTrimCalibrationIoPhase::waiting_measurement) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (now_ns < state->measurement_ready_ns) {
        const auto remaining_ns = state->measurement_ready_ns - now_ns;
        auto remaining_ms = remaining_ns / 1000000U +
            (remaining_ns % 1000000U != 0 ? 1U : 0U);
        if (timeout_.operation_deadline_ns != 0) {
            const auto remaining_deadline_ns = timeout_.operation_deadline_ns - now_ns;
            const auto remaining_deadline_ms = remaining_deadline_ns / 1000000U +
                (remaining_deadline_ns % 1000000U != 0 ? 1U : 0U);
            remaining_ms = std::min(remaining_ms, remaining_deadline_ms);
        }
        calibration_result.status = NormalCapTrimCalibrationIoStatus::pending;
        calibration_result.poll_interval_ms = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining_ms,
                std::numeric_limits<std::uint32_t>::max()));
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    std::uint16_t measurement = 0;
    state->failure = read_tuner(0x1D, &measurement);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (!torne::cech_zd1j::complete_captrim_measurement(&state->machine, true,
            measurement, state->pll_fraction_zero, &state->calibration)) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    if (state->calibration.needs_measurement) {
        if (state->calibration.complete || state->calibration.wait_tenth_ms != 25 ||
            state->calibration.calibration_flags_to_clear != 0) {
            state->phase = NormalCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        state->failure = write_tuner(0x18, state->calibration.register_value);
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = NormalCapTrimCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
            state->phase = NormalCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
        if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
            state->phase = NormalCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        const auto wait_ms = static_cast<std::uint64_t>(10U *
            (static_cast<std::uint32_t>(state->calibration.wait_tenth_ms) + 99U) / 100U);
        const auto wait_ns = wait_ms * 1000000ULL;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        state->measurement_ready_ns = now_ns > maximum - wait_ns ? maximum : now_ns + wait_ns;
        calibration_result.status = NormalCapTrimCalibrationIoStatus::pending;
        calibration_result.poll_interval_ms = static_cast<std::uint32_t>(wait_ms);
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    if (!state->calibration.complete || state->calibration.needs_measurement ||
        state->calibration.calibration_flags_to_clear != tuner_calibration_captrim) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    state->failure = write_tuner(0x18, state->calibration.register_value);
    if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
        state->phase = NormalCapTrimCalibrationIoPhase::failed;
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    ++state->plan_steps_completed;
    state->phase = NormalCapTrimCalibrationIoPhase::complete;
    calibration_result.status = NormalCapTrimCalibrationIoStatus::complete;
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    calibration_result.register_value = state->calibration.register_value;
    calibration_result.calibration_valid = true;
    calibration_result.calibration_flags_to_clear = state->calibration.calibration_flags_to_clear;
    return calibration_result;
}

P1gCapTrimCalibrationIoResult CechI2cIo::begin_p1g_captrim_calibration_io(
    P1gCapTrimCalibrationIoState *state)
{
    P1gCapTrimCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr || state->phase != P1gCapTrimCalibrationIoPhase::idle) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }

    auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    if (!torne::cech_zd1j::build_captrim_start_plan(&state->plan) ||
        !state->plan.complete || state->plan.step_count != 2 ||
        state->plan.steps[0].target != InitTarget::tuner ||
        state->plan.steps[0].kind != InitStepKind::write ||
        state->plan.steps[0].reg != 0x10 || state->plan.steps[0].value != 0x02B1 ||
        state->plan.steps[1].target != InitTarget::tuner ||
        state->plan.steps[1].kind != InitStepKind::write ||
        state->plan.steps[1].reg != 0x1E || state->plan.steps[1].value != 0x0032) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        return calibration_result;
    }
    for (std::size_t index = 0; index < state->plan.step_count; ++index) {
        const auto &step = state->plan.steps[index];
        state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
            static_cast<std::uint16_t>(step.value));
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
    }

    if (!torne::cech_zd1j::build_p1g_captrim_calibration_plan(&state->plan) ||
        state->plan.complete || state->plan.step_count != 2 ||
        state->plan.steps[0].target != InitTarget::tuner ||
        state->plan.steps[0].kind != InitStepKind::write ||
        state->plan.steps[0].reg != 0x40 || state->plan.steps[0].value != 0x01FF ||
        state->plan.steps[1].target != InitTarget::tuner ||
        state->plan.steps[1].kind != InitStepKind::read ||
        state->plan.steps[1].reg != 0x40 || state->plan.wait_tenth_ms != 50) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    for (std::size_t index = 0; index < state->plan.step_count; ++index) {
        const auto &step = state->plan.steps[index];
        if (step.target != InitTarget::tuner || step.reg > 0xFF) {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        if (step.kind == InitStepKind::write) {
            state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
                static_cast<std::uint16_t>(step.value));
        } else if (step.kind == InitStepKind::read) {
            std::uint16_t ignored = 0;
            state->failure = read_tuner(static_cast<std::uint8_t>(step.reg), &ignored);
        } else {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
    }

    now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    const auto wait_ms = static_cast<std::uint64_t>(10U *
        (static_cast<std::uint32_t>(state->plan.wait_tenth_ms) + 99U) / 100U);
    const auto wait_ns = wait_ms * 1000000ULL;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    state->measurement_ready_ns = now_ns > maximum - wait_ns ? maximum : now_ns + wait_ns;
    state->phase = P1gCapTrimCalibrationIoPhase::waiting_hardware_search;
    calibration_result.status = P1gCapTrimCalibrationIoStatus::pending;
    calibration_result.poll_interval_ms = static_cast<std::uint32_t>(wait_ms);
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    return calibration_result;
}

P1gCapTrimCalibrationIoResult CechI2cIo::poll_p1g_captrim_calibration_io(
    P1gCapTrimCalibrationIoState *state)
{
    P1gCapTrimCalibrationIoResult calibration_result{};
    calibration_result.io_result = result(DTV_INTERNAL_RESULT_OK);
    if (state == nullptr) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (state->phase == P1gCapTrimCalibrationIoPhase::complete) {
        calibration_result.status = P1gCapTrimCalibrationIoStatus::complete;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        calibration_result.calibration_valid = true;
        calibration_result.captrim_value = state->captrim_value;
        calibration_result.calibration_flags_to_clear = state->calibration_flags_to_clear;
        return calibration_result;
    }
    if (state->phase == P1gCapTrimCalibrationIoPhase::failed) {
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (state->phase != P1gCapTrimCalibrationIoPhase::waiting_hardware_search) {
        calibration_result.io_result = result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        return calibration_result;
    }
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (io_ == nullptr || io_->clock == nullptr || io_->clock->monotonic_now_ns == nullptr) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    const auto now_ns = io_->clock->monotonic_now_ns(io_->clock->user);
    if (timeout_.operation_deadline_ns != 0 && now_ns >= timeout_.operation_deadline_ns) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_TIMEOUT);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    if (now_ns < state->measurement_ready_ns) {
        const auto remaining_ns = state->measurement_ready_ns - now_ns;
        auto remaining_ms = remaining_ns / 1000000U +
            (remaining_ns % 1000000U != 0 ? 1U : 0U);
        if (timeout_.operation_deadline_ns != 0) {
            const auto remaining_deadline_ns = timeout_.operation_deadline_ns - now_ns;
            const auto remaining_deadline_ms = remaining_deadline_ns / 1000000U +
                (remaining_deadline_ns % 1000000U != 0 ? 1U : 0U);
            remaining_ms = std::min(remaining_ms, remaining_deadline_ms);
        }
        calibration_result.status = P1gCapTrimCalibrationIoStatus::pending;
        calibration_result.poll_interval_ms = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining_ms,
                std::numeric_limits<std::uint32_t>::max()));
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }

    if (!torne::cech_zd1j::continue_p1g_captrim_calibration(true, &state->plan) ||
        !state->plan.complete || state->plan.step_count != 3 ||
        state->plan.steps[0].target != InitTarget::tuner ||
        state->plan.steps[0].kind != InitStepKind::write ||
        state->plan.steps[0].reg != 0x40 || state->plan.steps[0].value != 0x018C ||
        state->plan.steps[1].target != InitTarget::tuner ||
        state->plan.steps[1].kind != InitStepKind::read ||
        state->plan.steps[1].reg != 0x40 ||
        state->plan.steps[2].target != InitTarget::tuner ||
        state->plan.steps[2].kind != InitStepKind::read ||
        state->plan.steps[2].reg != 0x18 || state->plan.steps[2].mask != 0x007F ||
        state->plan.calibration_flags_to_clear != tuner_calibration_captrim) {
        state->phase = P1gCapTrimCalibrationIoPhase::failed;
        state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
        calibration_result.io_result = state->failure;
        calibration_result.plan_steps_completed = state->plan_steps_completed;
        return calibration_result;
    }
    for (std::size_t index = 0; index < state->plan.step_count; ++index) {
        const auto &step = state->plan.steps[index];
        if (step.target != InitTarget::tuner || step.reg > 0xFF) {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        if (step.kind == InitStepKind::write) {
            state->failure = write_tuner(static_cast<std::uint8_t>(step.reg),
                static_cast<std::uint16_t>(step.value));
        } else if (step.kind == InitStepKind::read) {
            std::uint16_t observed = 0;
            state->failure = read_tuner(static_cast<std::uint8_t>(step.reg), &observed);
            if (state->failure.code == DTV_INTERNAL_RESULT_OK && step.mask != 0) {
                state->captrim_value = static_cast<std::uint16_t>(observed & step.mask);
            }
        } else {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_INTERNAL);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        if (state->failure.code != DTV_INTERNAL_RESULT_OK) {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
        ++state->plan_steps_completed;
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire)) {
            state->phase = P1gCapTrimCalibrationIoPhase::failed;
            state->failure = result(DTV_INTERNAL_RESULT_CANCELLED);
            calibration_result.io_result = state->failure;
            calibration_result.plan_steps_completed = state->plan_steps_completed;
            return calibration_result;
        }
    }

    state->calibration_flags_to_clear = state->plan.calibration_flags_to_clear;
    state->phase = P1gCapTrimCalibrationIoPhase::complete;
    calibration_result.status = P1gCapTrimCalibrationIoStatus::complete;
    calibration_result.plan_steps_completed = state->plan_steps_completed;
    calibration_result.calibration_valid = true;
    calibration_result.captrim_value = state->captrim_value;
    calibration_result.calibration_flags_to_clear = state->calibration_flags_to_clear;
    return calibration_result;
}

dtv_internal_result CechI2cIo::initialize_cursor()
{
    if (cursor_ == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (cursor_->pending) return result(DTV_INTERNAL_RESULT_NOT_READY);
    std::uint8_t bytes[4]{};
    auto transferred = control_read(io_, timeout_, kControlAddress, bytes);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    const auto session = read_be32(bytes) & 0xFFFF0000;
    if (session == 0) return result(DTV_INTERNAL_RESULT_NOT_READY);
    transferred = control_read(io_, timeout_, kProducerAddress, bytes);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    cursor_->session = session;
    cursor_->sequence = static_cast<std::uint16_t>(read_be32(bytes));
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result CechI2cIo::finish_pending(std::span<std::uint8_t> input)
{
    if (cursor_ == nullptr || !cursor_->pending)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (io_ == nullptr || io_->clock == nullptr ||
        io_->clock->monotonic_now_ns == nullptr || io_->clock->delay_until == nullptr)
        return result(DTV_INTERNAL_RESULT_UNSUPPORTED);

    std::uint8_t session_bytes[4]{};
    auto transferred = control_read(io_, timeout_, kControlAddress, session_bytes);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if ((read_be32(session_bytes) & 0xFFFF0000) != cursor_->session) {
        cursor_->pending = false;
        return result(DTV_INTERNAL_RESULT_DISCONNECTED);
    }

    const auto address = kCompletionAddress +
        static_cast<std::uint32_t>(cursor_->pending_slot * kRecordSize);
    for (;;) {
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire))
            return result(DTV_INTERNAL_RESULT_CANCELLED);
        std::uint8_t progress_bytes[4]{};
        transferred = control_read(io_, timeout_, kProgressAddress, progress_bytes);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        const auto progress = read_be32(progress_bytes);
        if ((progress & 0x80000000) != 0)
            return result(DTV_INTERNAL_RESULT_NOT_READY, progress);
        const auto completed = static_cast<std::uint16_t>(progress);
        if (completed == cursor_->pending_completion_count) {
            const auto now = io_->clock->monotonic_now_ns(io_->clock->user);
            if (timeout_.operation_deadline_ns != 0 && now >= timeout_.operation_deadline_ns)
                return result(DTV_INTERNAL_RESULT_TIMEOUT);
            // Core-clock pacing only; this is not a device timing requirement.
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            const auto wake = now > maximum - 1000000 ? maximum : now + 1000000;
            std::uint32_t cancelled = 0;
            transferred = io_->clock->delay_until(io_->clock->user, wake,
                timeout_.operation_deadline_ns, nullptr, &cancelled);
            if (cancelled != 0) return result(DTV_INTERNAL_RESULT_CANCELLED);
            if (transferred.code != DTV_INTERNAL_RESULT_OK &&
                transferred.code != DTV_INTERNAL_RESULT_TIMEOUT)
                return transferred;
            continue;
        }
        std::uint8_t record[kRecordSize]{};
        transferred = control_read(io_, timeout_, address, record);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        const auto first_tag = read_be32(record);
        const auto last_tag = read_be32(record + 60);
        if (first_tag != cursor_->pending_tag || last_tag != cursor_->pending_tag) {
            const auto now = io_->clock->monotonic_now_ns(io_->clock->user);
            if (timeout_.operation_deadline_ns != 0 && now >= timeout_.operation_deadline_ns)
                return result(DTV_INTERNAL_RESULT_USB_IO);
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            const auto wake = now > maximum - 1000000 ? maximum : now + 1000000;
            std::uint32_t cancelled = 0;
            transferred = io_->clock->delay_until(io_->clock->user, wake,
                timeout_.operation_deadline_ns, nullptr, &cancelled);
            if (cancelled != 0) return result(DTV_INTERNAL_RESULT_CANCELLED);
            if (transferred.code != DTV_INTERNAL_RESULT_OK &&
                transferred.code != DTV_INTERNAL_RESULT_TIMEOUT)
                return transferred;
            continue;
        }
        const auto read_length = read_be32(record + 8);
        const auto status = read_be32(record + 4);
        const auto completion = map_i2c_status(status);
        cursor_->pending = false;
        cursor_->sequence = static_cast<std::uint16_t>(cursor_->sequence + 1);
        if (completion.code != DTV_INTERNAL_RESULT_OK) return completion;
        if (read_length != cursor_->pending_read_length ||
            (!input.empty() && read_length != input.size()))
            return result(DTV_INTERNAL_RESULT_USB_IO);
        if (!input.empty())
            std::copy_n(record + 16, input.size(), input.data());
        return result(DTV_INTERNAL_RESULT_OK);
    }
}

dtv_internal_result CechI2cIo::transact(std::uint8_t address, std::uint16_t operation,
                                        std::span<const std::uint8_t> output,
                                        std::span<std::uint8_t> input)
{
    if (io_ == nullptr || io_->control == nullptr || cursor_ == nullptr ||
        address == 0 || (address & 1) != 0 || output.size() > 32 || input.size() > 32 ||
        (operation == 1 && (output.empty() || !input.empty())) ||
        (operation == 2 && (!output.empty() || input.empty())) ||
        (operation == 3 && (output.empty() || input.empty())) ||
        (operation < 1 || operation > 3))
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire))
        return result(DTV_INTERNAL_RESULT_CANCELLED);
    if (cursor_->pending) {
        const auto pending_result = finish_pending({});
        if (pending_result.code != DTV_INTERNAL_RESULT_OK) return pending_result;
    }
    auto transferred = initialize_cursor();
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;

    std::uint8_t progress_bytes[4]{};
    transferred = control_read(io_, timeout_, kProgressAddress, progress_bytes);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    const auto progress = read_be32(progress_bytes);
    if ((progress & 0x80000000) != 0)
        return result(DTV_INTERNAL_RESULT_NOT_READY, progress);
    const auto outstanding = static_cast<std::uint16_t>(cursor_->sequence -
        static_cast<std::uint16_t>(progress));
    if (outstanding >= kSlotCount)
        return result(DTV_INTERNAL_RESULT_BUSY, outstanding);

    const auto slot = static_cast<std::size_t>(cursor_->sequence % kSlotCount);
    const auto tag = cursor_->session | cursor_->sequence;
    std::uint8_t request[kRecordSize]{};
    write_be32(request, tag);
    write_be32(request + 4,
        (static_cast<std::uint32_t>(address) << 16) | operation);
    write_be32(request + 8,
        (static_cast<std::uint32_t>(input.size()) << 24) |
        static_cast<std::uint32_t>(output.size()));
    std::copy(output.begin(), output.end(), request + 16);
    write_be32(request + 60, tag);
    transferred = control_write(io_, timeout_, kRequestAddress +
        static_cast<std::uint32_t>(slot * kRecordSize), request);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;

    cursor_->pending_tag = tag;
    cursor_->pending_slot = slot;
    cursor_->pending_read_length = input.size();
    cursor_->pending_completion_count = static_cast<std::uint16_t>(progress);
    cursor_->pending = true;
    std::uint8_t producer[4]{};
    write_be32(producer, static_cast<std::uint16_t>(cursor_->sequence + 1));
    transferred = control_write(io_, timeout_, kProducerAddress, producer);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    return finish_pending(input);
}

}
