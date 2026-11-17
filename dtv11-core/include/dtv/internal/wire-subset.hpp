/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_WIRE_SUBSET_HPP
#define DTV_INTERNAL_WIRE_SUBSET_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include <dtv/internal/wire-envelope.hpp>
#include <dtv/types.h>

namespace dtv::internal {

inline constexpr std::size_t kHelloRequestSize = 24;
inline constexpr std::size_t kAcquireRequestSize = 18;
inline constexpr std::size_t kAcquireResponseSize = 24;

struct HelloRequest {
    std::uint16_t min_major = 0;
    std::uint16_t min_minor = 0;
    std::uint16_t max_major = 0;
    std::uint16_t max_minor = 0;
    std::uint64_t required_features = 0;
    std::uint64_t optional_features = 0;
};

struct AcquireRequest {
    std::array<std::uint8_t, 16> epoch{};
    std::uint16_t receiver_id = 0;
};

struct AcquireResponse {
    std::array<std::uint8_t, 16> epoch{};
    std::uint64_t lease_id = 0;
};

struct WireVersion {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
};

/*
 * 固定長 payload の byte codec。整数はリトルエンディアンで、構造体の
 * メモリ表現は wire へ流さない。decode は正確な payload 長を要求し、
 * encode は失敗時に出力先を変更しない。
 */
WireFrameCode decode_hello_request(dtv_bytes payload, HelloRequest *value);
WireFrameCode encode_hello_request(const HelloRequest &value, dtv_buffer *output);
WireFrameCode decode_acquire_request(dtv_bytes payload, AcquireRequest *value);
WireFrameCode encode_acquire_request(const AcquireRequest &value, dtv_buffer *output);
WireFrameCode decode_acquire_response(dtv_bytes payload, AcquireResponse *value);
WireFrameCode encode_acquire_response(const AcquireResponse &value, dtv_buffer *output);
/* min <= 1.0 <= max のときだけ 1.0 を返す。逆転した範囲は INVALID_ARGUMENT。 */
WireFrameCode negotiate_hello_version(const HelloRequest &value, WireVersion *version);

}

#endif
