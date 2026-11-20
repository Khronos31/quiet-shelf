/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U3_SATELLITE_IO_HPP
#define ASICEN_PX_W3U3_SATELLITE_IO_HPP

#include <dtv/spi/device_ops.hpp>

#include "px_w3u3_satellite.hpp"

namespace asicen::px_w3u3::detail {

struct SatelliteTuneIoState {
    // 同じ選局要求の各段で共有し、Coreの借用I/Oは保持しない。
    bool effect_started = false;
};

dtv_internal_result write_satellite_tune(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint64_t if_frequency_khz,
    std::uint16_t initial_tsid, SatelliteTuneIoState *state);

}

#endif
