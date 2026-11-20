/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_S3U2_SATELLITE_IO_HPP
#define ASICEN_PX_S3U2_SATELLITE_IO_HPP

#include <dtv/spi/device_ops.hpp>

#include <cstdint>

namespace asicen::px_s3u2::detail {

struct SatelliteTuneIoRequest {
    std::uint32_t rf_center_frequency_khz;
    std::uint16_t initial_tsid;
    std::uint16_t target_tsid;
    std::uint32_t slot_poll_interval_ms;
    std::uint32_t slot_poll_attempts;
};

struct SatelliteTuneIoOutcome {
    std::uint16_t selected_tsid;
    std::uint8_t lock_register;
    bool locked;
};

dtv_internal_result tune_satellite_io(
    const dtv::spi::PublicTuneRequest &, const dtv::spi::PublicTuneSession &,
    const SatelliteTuneIoRequest &, SatelliteTuneIoOutcome *);

}

#endif
