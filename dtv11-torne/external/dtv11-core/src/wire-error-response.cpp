// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/wire-error-response.hpp>

#include <algorithm>

namespace dtv::internal {
namespace {

bool view_valid(dtv_bytes view)
{
    return view.size == 0 || view.data != nullptr;
}

bool output_valid(const dtv_buffer *output)
{
    return output != nullptr && (output->capacity == 0 || output->data != nullptr);
}

std::uint16_t read_u16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                      static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t read_u32(const std::uint8_t *data)
{
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8 |
           static_cast<std::uint32_t>(data[2]) << 16 |
           static_cast<std::uint32_t>(data[3]) << 24;
}

std::uint64_t read_u64(const std::uint8_t *data)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(data[index]) << (8 * index);
    }
    return value;
}

void write_u16(std::uint8_t *data, std::uint16_t value)
{
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8);
}

void write_u32(std::uint8_t *data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8);
    data[2] = static_cast<std::uint8_t>(value >> 16);
    data[3] = static_cast<std::uint8_t>(value >> 24);
}

void write_u64(std::uint8_t *data, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        data[index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
}

bool utf8_without_nul(const std::uint8_t *data, std::size_t size)
{
    std::size_t index = 0;
    while (index < size) {
        const auto lead = data[index];
        if (lead == 0) return false;
        if (lead < 0x80) {
            ++index;
            continue;
        }
        std::size_t extra = 0;
        std::uint8_t second_min = 0x80;
        std::uint8_t second_max = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1;
        } else if (lead == 0xe0) {
            extra = 2;
            second_min = 0xa0;
        } else if (lead >= 0xe1 && lead <= 0xec) {
            extra = 2;
        } else if (lead == 0xed) {
            extra = 2;
            second_max = 0x9f;
        } else if (lead >= 0xee && lead <= 0xef) {
            extra = 2;
        } else if (lead == 0xf0) {
            extra = 3;
            second_min = 0x90;
        } else if (lead >= 0xf1 && lead <= 0xf3) {
            extra = 3;
        } else if (lead == 0xf4) {
            extra = 3;
            second_max = 0x8f;
        } else {
            return false;
        }
        if (index + extra >= size) return false;
        if (data[index + 1] < second_min || data[index + 1] > second_max) return false;
        for (std::size_t offset = 2; offset <= extra; ++offset) {
            if (data[index + offset] < 0x80 || data[index + offset] > 0xbf) return false;
        }
        index += extra + 1;
    }
    return true;
}

bool target_valid(std::uint8_t kind, std::uint64_t id)
{
    if (kind > 3) return false;
    if (kind == 0) return id == 0;
    if (kind == 1 || kind == 2) return id <= 0xffff;
    /* stream id 0 は純codecでは保存し、公開・振り分け前の gate で拒む。 */
    return true;
}

WireFrameCode validate_value(const ErrorResponse &value)
{
    if (value.code == 0) return WireFrameCode::invalid_argument;
    if (value.operation_length > kErrorResponseOperationCapacity ||
        value.detail_length > kErrorResponseDetailCapacity) {
        return WireFrameCode::invalid_argument;
    }
    if (!utf8_without_nul(value.operation.data(), value.operation_length) ||
        !utf8_without_nul(value.detail.data(), value.detail_length)) {
        return WireFrameCode::invalid_argument;
    }
    if (!target_valid(static_cast<std::uint8_t>(value.target_kind), value.target_id)) {
        return WireFrameCode::invalid_argument;
    }
    return WireFrameCode::ok;
}

std::size_t encoded_size(const ErrorResponse &value)
{
    return kErrorResponseMinPayloadSize + value.operation_length + value.detail_length +
           4 * (value.has_retry_after_ms ? 1 : 0) +
           4 * (value.has_required_payload_bytes ? 1 : 0);
}

}

WireFrameCode decode_error_response(dtv_bytes payload, ErrorResponse *value)
{
    if (value == nullptr || !view_valid(payload)) return WireFrameCode::invalid_argument;
    const auto size = payload.size;
    if (size < kErrorResponseMinPayloadSize || size > kErrorResponseMaxPayloadSize) {
        return WireFrameCode::protocol_error;
    }
    const auto *data = payload.data;

    ErrorResponse decoded;
    std::copy_n(data, decoded.epoch.size(), decoded.epoch.begin());
    decoded.code = read_u32(data + 16);
    const auto operation_length = read_u16(data + 20);
    if (operation_length > kErrorResponseOperationCapacity) return WireFrameCode::protocol_error;
    if (size < 24 + static_cast<std::size_t>(operation_length)) {
        return WireFrameCode::protocol_error;
    }
    std::copy_n(data + 22, operation_length, decoded.operation.begin());
    decoded.operation_length = operation_length;

    const auto detail_length = read_u16(data + 22 + operation_length);
    if (detail_length > kErrorResponseDetailCapacity) return WireFrameCode::protocol_error;
    if (size < 33 + static_cast<std::size_t>(operation_length) + detail_length) {
        return WireFrameCode::protocol_error;
    }
    std::copy_n(data + 24 + operation_length, detail_length, decoded.detail.begin());
    decoded.detail_length = detail_length;

    const auto target_kind = data[24 + operation_length + detail_length];
    decoded.target_id = read_u64(data + 25 + operation_length + detail_length);

    const auto retry_tag_offset = 33 + static_cast<std::size_t>(operation_length) + detail_length;
    if (size <= retry_tag_offset) return WireFrameCode::protocol_error;
    const auto retry_tag = data[retry_tag_offset];
    if (retry_tag > 1) return WireFrameCode::protocol_error;
    decoded.has_retry_after_ms = retry_tag == 1;

    std::size_t cursor = 34 + static_cast<std::size_t>(operation_length) + detail_length;
    if (retry_tag == 1) {
        if (size < cursor + 4) return WireFrameCode::protocol_error;
        decoded.retry_after_ms = read_u32(data + cursor);
        cursor += 4;
    }
    if (size <= cursor) return WireFrameCode::protocol_error;
    const auto required_tag = data[cursor];
    if (required_tag > 1) return WireFrameCode::protocol_error;
    decoded.has_required_payload_bytes = required_tag == 1;
    ++cursor;
    if (required_tag == 1) {
        if (size < cursor + 4) return WireFrameCode::protocol_error;
        decoded.required_payload_bytes = read_u32(data + cursor);
        cursor += 4;
    }
    if (size != cursor) return WireFrameCode::protocol_error;

    if (!utf8_without_nul(decoded.operation.data(), decoded.operation_length) ||
        !utf8_without_nul(decoded.detail.data(), decoded.detail_length)) {
        return WireFrameCode::protocol_error;
    }
    if (decoded.code == 0) return WireFrameCode::invalid_argument;
    if (!target_valid(target_kind, decoded.target_id)) return WireFrameCode::invalid_argument;
    decoded.target_kind = static_cast<ErrorTargetKind>(target_kind);
    *value = decoded;
    return WireFrameCode::ok;
}

WireFrameCode encode_error_response(const ErrorResponse &value, dtv_buffer *output)
{
    if (!output_valid(output)) return WireFrameCode::invalid_argument;
    const auto code = validate_value(value);
    if (code != WireFrameCode::ok) return code;
    const auto size = encoded_size(value);
    if (output->capacity < size) return WireFrameCode::buffer_too_small;

    auto *data = output->data;
    std::copy_n(value.epoch.begin(), value.epoch.size(), data);
    write_u32(data + 16, value.code);
    write_u16(data + 20, value.operation_length);
    std::copy_n(value.operation.begin(), value.operation_length, data + 22);
    write_u16(data + 22 + value.operation_length, value.detail_length);
    std::copy_n(value.detail.begin(), value.detail_length, data + 24 + value.operation_length);
    std::size_t cursor = 24 + static_cast<std::size_t>(value.operation_length) + value.detail_length;
    data[cursor++] = static_cast<std::uint8_t>(value.target_kind);
    write_u64(data + cursor, value.target_id);
    cursor += 8;
    data[cursor++] = value.has_retry_after_ms ? 1 : 0;
    if (value.has_retry_after_ms) {
        write_u32(data + cursor, value.retry_after_ms);
        cursor += 4;
    }
    data[cursor++] = value.has_required_payload_bytes ? 1 : 0;
    if (value.has_required_payload_bytes) {
        write_u32(data + cursor, value.required_payload_bytes);
        cursor += 4;
    }
    output->size = size;
    return WireFrameCode::ok;
}

}
