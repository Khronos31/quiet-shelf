/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_S3U2_GROUND_TUNE_HPP
#define ASICEN_PX_S3U2_GROUND_TUNE_HPP

#include "ground_tune_common.hpp"

#include <cstdint>

namespace asicen::px_s3u2::detail {

bool calculate_ground_tune_khz(std::uint64_t frequency_khz,
                               ::asicen::detail::ground_tune::TuneValues *out);

}

#endif
