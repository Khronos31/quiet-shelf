// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/wire-status-request.hpp>

#include <algorithm>

namespace dtv::internal {
namespace {

bool view_valid(dtv_bytes input)
{
    return input.size == 0 || input.data != nullptr;
}

bool output_valid(const dtv_buffer *output)
{
    return output != nullptr && (output->capacity == 0 || output->data != nullptr);
}

bool scope_codec_valid(const Scope &scope)
{
    switch (scope.kind) {
    case Scope::Kind::enclosure:
        return scope.id == 0;
    case Scope::Kind::receiver:
    case Scope::Kind::reader:
        return scope.id <= 65535;
    case Scope::Kind::stream:
        return true;
    default:
        return false;
    }
}

std::uint64_t read_u64(const std::uint8_t *data)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(data[index]) << (8 * index);
    }
    return value;
}

void write_u64(std::uint8_t *data, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        data[index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
}

}

WireFrameCode validate_scope(const Scope &scope)
{
    if (!scope_codec_valid(scope) ||
        (scope.kind == Scope::Kind::stream && scope.id == 0)) {
        return WireFrameCode::invalid_argument;
    }
    return WireFrameCode::ok;
}

WireFrameCode status_request_payload_size(const StatusRequest &value, std::size_t *size)
{
    if (size == nullptr || !scope_codec_valid(value.scope)) {
        return WireFrameCode::invalid_argument;
    }
    *size = kStatusRequestPayloadSize;
    return WireFrameCode::ok;
}

WireFrameCode encode_status_request(const StatusRequest &value, dtv_buffer *output)
{
    if (!output_valid(output)) return WireFrameCode::invalid_argument;
    std::size_t size = 0;
    auto code = status_request_payload_size(value, &size);
    if (code != WireFrameCode::ok) return code;
    if (output->capacity < size) return WireFrameCode::buffer_too_small;

    std::copy(value.epoch.begin(), value.epoch.end(), output->data);
    output->data[16] = static_cast<std::uint8_t>(value.scope.kind);
    write_u64(output->data + 17, value.scope.id);
    output->size = size;
    return WireFrameCode::ok;
}

WireFrameCode decode_status_request(dtv_bytes payload, StatusRequest *value)
{
    if (value == nullptr || !view_valid(payload)) return WireFrameCode::invalid_argument;
    if (payload.size != kStatusRequestPayloadSize) return WireFrameCode::protocol_error;

    StatusRequest decoded;
    std::copy_n(payload.data, decoded.epoch.size(), decoded.epoch.begin());
    decoded.scope.kind = static_cast<Scope::Kind>(payload.data[16]);
    decoded.scope.id = read_u64(payload.data + 17);
    if (!scope_codec_valid(decoded.scope)) return WireFrameCode::invalid_argument;
    *value = decoded;
    return WireFrameCode::ok;
}

}
