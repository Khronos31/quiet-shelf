/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_S3U2_GROUND_TUNE_HPP
#define ASICEN_PX_S3U2_GROUND_TUNE_HPP

#include "ground_io_common.hpp"

#include <cstdint>

namespace asicen::px_s3u2::detail {

bool calculate_ground_tune_khz(std::uint64_t frequency_khz,
                               ::asicen::detail::ground_tune::TuneValues *out);

dtv_internal_result tune_ground_by_frequency_khz(
    const ::asicen::detail::ground_io::IoContext &context,
    std::uint8_t demodulator, std::uint64_t frequency_khz,
    const char *length_diagnostic, const char *status_diagnostic);

dtv_internal_result read_ground_lock(
    const ::asicen::detail::ground_io::IoContext &context,
    const char *length_diagnostic, const char *status_diagnostic,
    ::asicen::detail::ground_tune::Lock *out);

}

#endif
