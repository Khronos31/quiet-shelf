/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_s3u2_ground_tune.hpp"

#include "px_s3u2_ground_io.hpp"

#include <array>

namespace asicen::px_s3u2::detail {

namespace {

constexpr std::uint8_t ground_demodulator = 0x30;
constexpr std::uint8_t ground_lock_status_register = 0xb0;
constexpr std::uint8_t ground_lock_read_mode = 0x01;

}

bool calculate_ground_tune_khz(
    std::uint64_t frequency_khz,
    ::asicen::detail::ground_tune::TuneValues *out)
{
    if (out == nullptr || frequency_khz == 0 || frequency_khz > 999999) return false;
    return ::asicen::detail::ground_tune::calculate_khz(frequency_khz, out);
}

dtv_internal_result tune_ground_by_frequency_khz(
    const ::asicen::detail::ground_io::IoContext &context,
    std::uint8_t demodulator, std::uint64_t frequency_khz,
    const char *length_diagnostic, const char *status_diagnostic)
{
    ::asicen::detail::ground_tune::TuneValues values{};
    if (!calculate_ground_tune_khz(frequency_khz, &values))
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);

    auto result = ::asicen::px_s3u2::detail::write_ground_tune_prepare(
        context, demodulator, length_diagnostic, status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    const std::array<std::uint8_t, 6> registers{
        values.tuner.reg1, values.tuner.reg2, values.tuner.reg3,
        values.tuner.reg4, values.tuner.reg5, values.tuner.reg6};
    result = write_ground_tuner_registers(
        context, demodulator, std::span<const std::uint8_t, 6>(registers),
        length_diagnostic, status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    return ::asicen::px_s3u2::detail::write_ground_tune_finish(
        context, demodulator, length_diagnostic, status_diagnostic);
}

dtv_internal_result read_ground_lock(
    const ::asicen::detail::ground_io::IoContext &context,
    const char *length_diagnostic, const char *status_diagnostic,
    ::asicen::detail::ground_tune::Lock *out)
{
    if (out == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);

    std::uint8_t status = 0;
    const auto result = ::asicen::detail::ground_io::read_demodulator(
        context, ground_demodulator, ground_lock_status_register, ground_lock_read_mode,
        &status, length_diagnostic, status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    *out = ::asicen::detail::ground_tune::classify_lock(status);
    return result;
}

}
