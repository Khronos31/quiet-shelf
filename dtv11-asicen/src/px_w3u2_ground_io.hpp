/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U2_GROUND_IO_HPP
#define ASICEN_PX_W3U2_GROUND_IO_HPP

#include <dtv/spi/device_ops.hpp>

#include <cstdint>

namespace asicen::px_w3u2::detail {

dtv_internal_result tune_ground_by_frequency_khz(
    dtv_context *, const dtv::spi::PublicTuneRequest *, dtv::spi::PublicTuneSession *,
    std::uint64_t frequency_khz);

}

#endif
