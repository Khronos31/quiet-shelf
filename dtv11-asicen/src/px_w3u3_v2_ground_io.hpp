/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U3_V2_GROUND_IO_HPP
#define ASICEN_PX_W3U3_V2_GROUND_IO_HPP

#include <dtv/spi/device_ops.hpp>

#include <cstdint>

namespace asicen::px_w3u3_v2::detail {

struct GroundTuneIoState {
    // 同じ選局要求の各段で共有し、Coreの借用I/Oは保持しない。
    bool effect_started = false;
};

dtv_internal_result write_nmi_rf_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint32_t value, std::uint8_t value_width, GroundTuneIoState *state);

dtv_internal_result read_nmi_rf_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint8_t *value, GroundTuneIoState *state);

dtv_internal_result write_nmi_demodulator_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint32_t value, GroundTuneIoState *state);

dtv_internal_result read_nmi_demodulator_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint32_t *value, GroundTuneIoState *state);

}

#endif
