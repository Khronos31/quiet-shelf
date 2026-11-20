/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_s3u_ground_io.hpp"

#include "ground_io_common.hpp"
#include "px_s3u_tune.hpp"

#include <array>
#include <atomic>
#include <limits>

namespace asicen::px_s3u::detail {

namespace ground_io = ::asicen::detail::ground_io;

namespace {

dtv_internal_result make_result(std::int32_t code)
{
    return dtv_internal_result_make(code, 0);
}

bool valid_context(const dtv::spi::PublicTuneRequest &request,
                   const dtv::spi::PublicTuneSession &session,
                   const GroundTuneIoState *state)
{
    return state != nullptr && request.system == DTV_SYSTEM_ISDB_T &&
           session.begin_effect != nullptr && session.io != nullptr &&
           session.io->control != nullptr && session.io->clock != nullptr &&
           session.io->clock->monotonic_now_ns != nullptr;
}

dtv_internal_result begin_effect_if_needed(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, GroundTuneIoState *state)
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

dtv_internal_result write_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, const DemodulatorWrite &write,
    GroundTuneIoState *state)
{
    if (!valid_context(request, session, state))
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    const auto started = begin_effect_if_needed(request, session, state);
    if (started.code != DTV_INTERNAL_RESULT_OK) return started;
    return ground_io::write_demodulator(request, session, write.slave_address, write.reg,
        write.value, "PX-S3U I2C応答長が不正です", "PX-S3U I2C statusが不正です");
}

dtv_internal_result write_registers(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session,
    const DemodulatorWrite *writes, std::size_t count, GroundTuneIoState *state)
{
    for (std::size_t i = 0; i < count; ++i) {
        const auto result = write_register(request, session, writes[i], state);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result write_tuner_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint8_t reg, std::uint8_t value,
    GroundTuneIoState *state)
{
    if (!valid_context(request, session, state))
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    const auto started = begin_effect_if_needed(request, session, state);
    if (started.code != DTV_INTERNAL_RESULT_OK) return started;
    const std::array<std::uint8_t, 4> bytes{0xfe, 0xc6, reg, value};
    return ground_io::write_stage(request, session, 0x30, false, bytes,
        "PX-S3U tuner bridge応答長が不正です", "PX-S3U tuner bridge statusが不正です");
}

dtv_internal_result wait_calibration(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session)
{
    if (session.io == nullptr || session.io->clock == nullptr ||
        session.io->clock->monotonic_now_ns == nullptr ||
        session.io->clock->delay_until == nullptr)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);

    constexpr std::uint64_t wait_ns = UINT64_C(1000000);
    const auto now = session.io->clock->monotonic_now_ns(session.io->clock->user);
    const auto wake_at = now > std::numeric_limits<std::uint64_t>::max() - wait_ns
                             ? std::numeric_limits<std::uint64_t>::max()
                             : now + wait_ns;
    std::uint32_t cancelled = 0;
    const auto result = session.io->clock->delay_until(
        session.io->clock->user, wake_at, request.timeout.operation_deadline_ns,
        nullptr, &cancelled);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if (cancelled != 0 || (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire)))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);
    return result;
}

dtv_internal_result read_calibration(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint8_t *value,
    GroundTuneIoState *state)
{
    if (value == nullptr || !valid_context(request, session, state))
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    auto started = begin_effect_if_needed(request, session, state);
    if (started.code != DTV_INTERNAL_RESULT_OK) return started;
    constexpr char length_diagnostic[] = "PX-S3U calibration read応答長が不正です";
    constexpr char status_diagnostic[] = "PX-S3U calibration read statusが不正です";
    auto result = ground_io::write_demodulator_bytes(request, session, 0x30, 0xfe,
        std::array<std::uint8_t, 2>{0xc6, 0x0e}, true,
        length_diagnostic, status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = ground_io::write_demodulator_bytes(request, session, 0x30, 0xfe,
        std::array<std::uint8_t, 1>{0xc7}, false,
        length_diagnostic, status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    std::array<std::uint8_t, 2> response{};
    result = ground_io::transfer(request, session, 0x19, 0x30, 0, response.data(),
        response.size(), true, length_diagnostic, status_diagnostic);
    if (result.code == DTV_INTERNAL_RESULT_OK) *value = response[1];
    return result;
}

}

dtv_internal_result write_ground_tune_prepare(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, GroundTuneIoState *state)
{
    return write_registers(request, session, ground_tune_prepare,
                           sizeof(ground_tune_prepare) / sizeof(ground_tune_prepare[0]), state);
}

dtv_internal_result write_ground_rf_band(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint64_t center_frequency_khz,
    GroundTuneIoState *state)
{
    DemodulatorWrite write{};
    if (!ground_rf_band_write(center_frequency_khz, &write))
        return make_result(DTV_INTERNAL_RESULT_OK);
    return write_register(request, session, write, state);
}

dtv_internal_result write_ground_tuner(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, const GroundTuneValues &values,
    GroundTuneIoState *state)
{
    const std::array<std::uint8_t, 6> registers{
        values.tuner.reg1, values.tuner.reg2, values.tuner.reg3,
        values.tuner.reg4, values.tuner.reg5, values.tuner.reg6};
    for (std::size_t i = 0; i < registers.size(); ++i) {
        const auto result = write_tuner_register(request, session,
            static_cast<std::uint8_t>(i + 1), registers[i], state);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }

    auto result = write_tuner_register(request, session, 0x0e, 0x80, state);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_tuner_register(request, session, 0x0e, 0x00, state);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = wait_calibration(request, session);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_tuner_register(request, session, 0x0e, 0x00, state);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    std::uint8_t calibration_value = 0;
    result = read_calibration(request, session, &calibration_value, state);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if ((calibration_value & 0x3f) < 0x02) {
        std::uint8_t reg6 = 0;
        if (!calculate_calibrated_reg6(values.tuner.reg6, calibration_value, &reg6))
            return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        result = write_tuner_register(request, session, 0x06, reg6, state);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = write_tuner_register(request, session, 0x0e, 0x80, state);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = write_tuner_register(request, session, 0x0e, 0x00, state);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = wait_calibration(request, session);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result write_ground_tune_finish(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, GroundTuneIoState *state)
{
    return write_registers(request, session, ground_tune_finish,
                           sizeof(ground_tune_finish) / sizeof(ground_tune_finish[0]), state);
}

}
