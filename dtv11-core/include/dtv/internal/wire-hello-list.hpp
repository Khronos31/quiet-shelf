/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_WIRE_HELLO_LIST_HPP
#define DTV_INTERNAL_WIRE_HELLO_LIST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <dtv/internal/wire-envelope.hpp>
#include <dtv/types.h>

namespace dtv::internal {

inline constexpr std::size_t kListRequestSize = 16;
inline constexpr std::size_t kHelloResponseMinSize = 41;
inline constexpr std::size_t kHelloResponseMaxSize = 203;
inline constexpr std::size_t kHelloResponseProductMaxSize = 32;
inline constexpr std::size_t kHelloResponseVersionMaxSize = 64;

struct HelloResponse {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint64_t features = 0;
    std::array<std::uint8_t, 16> epoch{};
    std::string product;
    std::string version;
    std::optional<std::string> core_version;
    std::uint32_t max_control_payload = 0;
    std::uint32_t max_ts_bytes = 0;
};

struct ListRequest {
    std::array<std::uint8_t, 16> epoch{};
};

/* major/minor の選択と epoch 照合は上位層が行い、ここでは値を保つ。 */
WireFrameCode hello_response_payload_size(const HelloResponse &value, std::size_t *size);
WireFrameCode decode_hello_response(dtv_bytes payload, HelloResponse *value);
WireFrameCode encode_hello_response(const HelloResponse &value, dtv_buffer *output);
WireFrameCode decode_list_request(dtv_bytes payload, ListRequest *value);
WireFrameCode encode_list_request(const ListRequest &value, dtv_buffer *output);

}

#endif
