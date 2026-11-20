/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_s3u2_ground_io.hpp"

namespace asicen::px_s3u2::detail {

dtv_internal_result write_ground_tune_prepare(
    const ::asicen::detail::ground_io::IoContext &context,
    std::uint8_t demodulator, const char *length_diagnostic,
    const char *status_diagnostic)
{
    return ::asicen::detail::ground_io::write_ground_tune_prepare(
        context, demodulator, length_diagnostic, status_diagnostic);
}

dtv_internal_result write_ground_tuner_registers(
    const ::asicen::detail::ground_io::IoContext &context,
    std::uint8_t demodulator, std::span<const std::uint8_t, 6> values,
    const char *length_diagnostic, const char *status_diagnostic)
{
    return ::asicen::detail::ground_io::write_rf_tuner_registers(
        context, demodulator, values, length_diagnostic, status_diagnostic);
}

dtv_internal_result write_ground_tune_finish(
    const ::asicen::detail::ground_io::IoContext &context,
    std::uint8_t demodulator, const char *length_diagnostic,
    const char *status_diagnostic)
{
    return ::asicen::detail::ground_io::write_ground_tune_finish(
        context, demodulator, length_diagnostic, status_diagnostic);
}

}
