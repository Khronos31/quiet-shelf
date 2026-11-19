// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/wire-hello-list.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

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
    std::size_t offset = 0;
    while (offset < size) {
        const auto first = data[offset];
        if (first == 0) return false;
        if (first <= 0x7f) {
            ++offset;
            continue;
        }

        std::size_t count = 0;
        std::uint8_t second_min = 0x80;
        std::uint8_t second_max = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 3;
            if (first == 0xe0) second_min = 0xa0;
            if (first == 0xed) second_max = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 4;
            if (first == 0xf0) second_min = 0x90;
            if (first == 0xf4) second_max = 0x8f;
        } else {
            return false;
        }
        if (count > size - offset) return false;
        const auto second = data[offset + 1];
        if (second < second_min || second > second_max) return false;
        for (std::size_t index = 2; index < count; ++index) {
            const auto continuation = data[offset + index];
            if (continuation < 0x80 || continuation > 0xbf) return false;
        }
        offset += count;
    }
    return true;
}

bool text_valid(const std::string &value, std::size_t maximum)
{
    return value.size() <= maximum &&
           utf8_without_nul(reinterpret_cast<const std::uint8_t *>(value.data()),
                            value.size());
}

bool hello_response_valid(const HelloResponse &value)
{
    return text_valid(value.product, kHelloResponseProductMaxSize) &&
           text_valid(value.version, kHelloResponseVersionMaxSize) &&
           (!value.core_version ||
            text_valid(*value.core_version, kHelloResponseVersionMaxSize));
}

bool take_bytes(dtv_bytes payload, std::size_t *offset, std::size_t size,
                const std::uint8_t **data)
{
    if (*offset > payload.size || size > payload.size - *offset) return false;
    *data = payload.data + *offset;
    *offset += size;
    return true;
}

bool take_u16(dtv_bytes payload, std::size_t *offset, std::uint16_t *value)
{
    const std::uint8_t *data = nullptr;
    if (!take_bytes(payload, offset, 2, &data)) return false;
    *value = read_u16(data);
    return true;
}

bool take_u32(dtv_bytes payload, std::size_t *offset, std::uint32_t *value)
{
    const std::uint8_t *data = nullptr;
    if (!take_bytes(payload, offset, 4, &data)) return false;
    *value = read_u32(data);
    return true;
}

bool take_text(dtv_bytes payload, std::size_t *offset, std::size_t maximum,
               std::string *value)
{
    std::uint16_t size = 0;
    if (!take_u16(payload, offset, &size) || size > maximum) return false;
    const std::uint8_t *data = nullptr;
    if (!take_bytes(payload, offset, size, &data) || !utf8_without_nul(data, size)) {
        return false;
    }
    value->assign(reinterpret_cast<const char *>(data), size);
    return true;
}

void write_text(const std::string &value, std::uint8_t *output, std::size_t *offset)
{
    write_u16(output + *offset, static_cast<std::uint16_t>(value.size()));
    *offset += 2;
    if (!value.empty()) std::memcpy(output + *offset, value.data(), value.size());
    *offset += value.size();
}

}

WireFrameCode hello_response_payload_size(const HelloResponse &value, std::size_t *size)
{
    if (size == nullptr) return WireFrameCode::invalid_argument;
    if (!hello_response_valid(value)) return WireFrameCode::invalid_argument;
    const auto core_size = value.core_version ? 2 + value.core_version->size() : 0;
    *size = kHelloResponseMinSize + value.product.size() + value.version.size() + core_size;
    return WireFrameCode::ok;
}

WireFrameCode decode_hello_response(dtv_bytes payload, HelloResponse *value)
{
    if (value == nullptr || !view_valid(payload)) return WireFrameCode::invalid_argument;
    if (payload.size < kHelloResponseMinSize || payload.size > kHelloResponseMaxSize) {
        return WireFrameCode::protocol_error;
    }

    std::size_t offset = 0;
    const std::uint8_t *fixed = nullptr;
    HelloResponse decoded;
    if (!take_bytes(payload, &offset, 4, &fixed)) return WireFrameCode::protocol_error;
    decoded.major = read_u16(fixed);
    decoded.minor = read_u16(fixed + 2);
    if (!take_bytes(payload, &offset, 8, &fixed)) return WireFrameCode::protocol_error;
    decoded.features = read_u64(fixed);
    if (!take_bytes(payload, &offset, decoded.epoch.size(), &fixed)) {
        return WireFrameCode::protocol_error;
    }
    std::copy_n(fixed, decoded.epoch.size(), decoded.epoch.begin());

    if (!take_text(payload, &offset, kHelloResponseProductMaxSize, &decoded.product) ||
        !take_text(payload, &offset, kHelloResponseVersionMaxSize, &decoded.version)) {
        return WireFrameCode::protocol_error;
    }
    if (!take_bytes(payload, &offset, 1, &fixed)) return WireFrameCode::protocol_error;
    if (fixed[0] > 1) return WireFrameCode::protocol_error;
    if (fixed[0] == 1) {
        std::string core_version;
        if (!take_text(payload, &offset, kHelloResponseVersionMaxSize, &core_version)) {
            return WireFrameCode::protocol_error;
        }
        decoded.core_version = std::move(core_version);
    }
    if (!take_u32(payload, &offset, &decoded.max_control_payload) ||
        !take_u32(payload, &offset, &decoded.max_ts_bytes) || offset != payload.size) {
        return WireFrameCode::protocol_error;
    }
    *value = std::move(decoded);
    return WireFrameCode::ok;
}

WireFrameCode encode_hello_response(const HelloResponse &value, dtv_buffer *output)
{
    if (!output_valid(output)) return WireFrameCode::invalid_argument;
    std::size_t size = 0;
    const auto valid = hello_response_payload_size(value, &size);
    if (valid != WireFrameCode::ok) return valid;
    if (output->capacity < size) return WireFrameCode::buffer_too_small;

    std::size_t offset = 0;
    write_u16(output->data + offset, value.major);
    offset += 2;
    write_u16(output->data + offset, value.minor);
    offset += 2;
    write_u64(output->data + offset, value.features);
    offset += 8;
    std::copy(value.epoch.begin(), value.epoch.end(), output->data + offset);
    offset += value.epoch.size();
    write_text(value.product, output->data, &offset);
    write_text(value.version, output->data, &offset);
    output->data[offset++] = value.core_version ? 1 : 0;
    if (value.core_version) write_text(*value.core_version, output->data, &offset);
    write_u32(output->data + offset, value.max_control_payload);
    offset += 4;
    write_u32(output->data + offset, value.max_ts_bytes);
    offset += 4;
    output->size = offset;
    return WireFrameCode::ok;
}

WireFrameCode decode_list_request(dtv_bytes payload, ListRequest *value)
{
    if (value == nullptr || !view_valid(payload)) return WireFrameCode::invalid_argument;
    if (payload.size != kListRequestSize) return WireFrameCode::protocol_error;
    ListRequest decoded;
    std::copy_n(payload.data, decoded.epoch.size(), decoded.epoch.begin());
    *value = decoded;
    return WireFrameCode::ok;
}

WireFrameCode encode_list_request(const ListRequest &value, dtv_buffer *output)
{
    if (!output_valid(output)) return WireFrameCode::invalid_argument;
    if (output->capacity < kListRequestSize) return WireFrameCode::buffer_too_small;
    std::copy(value.epoch.begin(), value.epoch.end(), output->data);
    output->size = kListRequestSize;
    return WireFrameCode::ok;
}

}
