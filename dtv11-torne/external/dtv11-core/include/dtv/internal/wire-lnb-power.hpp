/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_WIRE_LNB_POWER_HPP
#define DTV_INTERNAL_WIRE_LNB_POWER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include <dtv/internal/wire-envelope.hpp>
#include <dtv/types.h>

namespace dtv::internal {

inline constexpr std::size_t kLnbPowerPayloadSize = 17;
inline constexpr std::uint16_t kWireOpcodeLnbPower = 0x0039;

/* LNB_POWER の要求と応答は同じ17 byte配置（epoch16 + enabled 0/1）を共有する。 */
struct LnbPower {
    std::array<std::uint8_t, 16> epoch{};
    bool enabled = false;
};

/*
 * 固定長 payload の byte codec。epoch は不透明な16 byteとして往復し、
 * enabled は0/1だけを受け付ける。decode は正確な17 byteを要求する。
 */
WireFrameCode decode_lnb_power(dtv_bytes payload, LnbPower *value);
WireFrameCode encode_lnb_power(const LnbPower &value, dtv_buffer *output);

}

#endif
