/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_w3u3_satellite_io.hpp"

#include "ground_io_common.hpp"

#include <atomic>
#include <limits>
#include <span>

namespace asicen::px_w3u3::detail {

namespace ground_io = ::asicen::detail::ground_io;

namespace {

dtv_internal_result make_result(std::int32_t code)
{
    return dtv_internal_result_make(code, 0);
}

bool valid_context(const dtv::spi::PublicTuneRequest &request,
                   const dtv::spi::PublicTuneSession &session,
                   const SatelliteTuneIoState *state)
{
    return state != nullptr && request.system == DTV_SYSTEM_ISDB_S &&
           (request.receiver_index == 0 || request.receiver_index == 2) &&
           session.begin_effect != nullptr && session.io != nullptr &&
           session.io->control != nullptr && session.io->clock != nullptr &&
           session.io->clock->monotonic_now_ns != nullptr &&
           session.io->clock->delay_until != nullptr;
}

dtv_internal_result begin_effect_if_needed(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, SatelliteTuneIoState *state)
{
    if (state->effect_started) return make_result(DTV_INTERNAL_RESULT_OK);
    if (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);

    std::uint64_t operation_epoch = 0;
    const auto result = session.begin_effect(session.user, &operation_epoch);
    if (result.code == DTV_INTERNAL_RESULT_OK) state->effect_started = true;
    return result;
}

dtv_internal_result wait_ms(const dtv::spi::PublicTuneRequest &request,
                            const dtv::spi::PublicTuneSession &session,
                            std::uint16_t delay_ms)
{
    if (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);

    const auto *clock = session.io->clock;
    const auto now = clock->monotonic_now_ns(clock->user);
    constexpr std::uint64_t ns_per_ms = UINT64_C(1000000);
    const auto delay_ns = static_cast<std::uint64_t>(delay_ms) * ns_per_ms;
    if (now > std::numeric_limits<std::uint64_t>::max() - delay_ns)
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);

    const auto operation_deadline = request.timeout.operation_deadline_ns == 0
        ? std::numeric_limits<std::uint64_t>::max()
        : request.timeout.operation_deadline_ns;
    std::uint32_t cancelled = 0;
    const auto result = clock->delay_until(clock->user, now + delay_ns,
                                           operation_deadline, nullptr, &cancelled);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if (cancelled != 0 || (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire)))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);
    return result;
}

dtv_internal_result write_demodulator(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session,
    const SatelliteDemodulatorWrite &write)
{
    return ground_io::write_demodulator(request, session, write.address, write.reg,
        write.value, "PX-W3U3衛星I2C応答長が不正です",
        "PX-W3U3衛星I2C statusが不正です");
}

}

dtv_internal_result write_satellite_tune(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint64_t if_frequency_khz,
    std::uint16_t initial_tsid, SatelliteTuneIoState *state)
{
    if (!valid_context(request, session, state))
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);

    SatelliteTunePlan plan{};
    if (!make_satellite_tune_plan(if_frequency_khz, &plan, initial_tsid))
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);

    auto result = begin_effect_if_needed(request, session, state);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    for (const auto &write : satellite_tune_prepare) {
        result = write_demodulator(request, session, write);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }

    result = ground_io::write_demodulator_bytes(request, session,
        0x32, plan.initial_tsid_register,
        std::span<const std::uint8_t>(plan.initial_tsid_bytes, 2), false,
        "PX-W3U3衛星初期TSID応答長が不正です",
        "PX-W3U3衛星初期TSID statusが不正です");
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    for (std::uint8_t index = 0; index < 4; ++index) {
        const auto &message = plan.tuner_messages[index];
        result = ground_io::write_stage(request, session, 0x32, message.stop != 0,
            std::span<const std::uint8_t>(message.data, message.size),
            "PX-W3U3衛星チューナ応答長が不正です",
            "PX-W3U3衛星チューナ statusが不正です");
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        if (index == plan.wait_after_message_index) {
            result = wait_ms(request, session, plan.wait_after_message_ms);
            if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        }
    }

    for (const auto &write : satellite_tune_finish) {
        result = write_demodulator(request, session, write);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    return result;
}

}
