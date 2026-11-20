/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_WIRE_STATUS_REQUEST_HPP
#define DTV_INTERNAL_WIRE_STATUS_REQUEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include <dtv/internal/wire-envelope.hpp>

namespace dtv::internal {

inline constexpr std::size_t kStatusRequestPayloadSize = 25;

struct Scope {
    enum class Kind : std::uint8_t {
        enclosure = 0,
        receiver = 1,
        reader = 2,
        stream = 3
    };

    Kind kind = Kind::enclosure;
    std::uint64_t id = 0;
};

struct StatusRequest {
    std::array<std::uint8_t, 16> epoch{};
    Scope scope;
};

/* stream の id=0 はcodecでは保持し、利用前のscope検証で拒否する。 */
WireFrameCode validate_scope(const Scope &scope);
WireFrameCode status_request_payload_size(const StatusRequest &value, std::size_t *size);
WireFrameCode encode_status_request(const StatusRequest &value, dtv_buffer *output);
WireFrameCode decode_status_request(dtv_bytes payload, StatusRequest *value);

}

#endif
