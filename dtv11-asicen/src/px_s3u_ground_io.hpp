/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_S3U_GROUND_IO_HPP
#define ASICEN_PX_S3U_GROUND_IO_HPP

#include <dtv/spi/device_ops.hpp>

#include <cstdint>

namespace asicen::px_s3u::detail {

struct GroundTuneValues;

struct GroundTuneIoState {
    // 同じ選局処理の各段で共有し、Coreの借用I/Oは保持しない。
    bool effect_started = false;
};

dtv_internal_result write_ground_tune_prepare(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, GroundTuneIoState *state);
dtv_internal_result write_ground_rf_band(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint64_t center_frequency_khz,
    GroundTuneIoState *state);
dtv_internal_result write_ground_tuner(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, const GroundTuneValues &values,
    GroundTuneIoState *state);
dtv_internal_result write_ground_tune_finish(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, GroundTuneIoState *state);

}

#endif
