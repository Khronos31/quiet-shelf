/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_s3u2_ground_tune.hpp"

namespace asicen::px_s3u2::detail {

bool calculate_ground_tune_khz(
    std::uint64_t frequency_khz,
    ::asicen::detail::ground_tune::TuneValues *out)
{
    if (out == nullptr || frequency_khz == 0 || frequency_khz > 999999) return false;
    return ::asicen::detail::ground_tune::calculate_khz(frequency_khz, out);
}

}
