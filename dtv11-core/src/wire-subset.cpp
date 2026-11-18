// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/wire-subset.hpp>

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

WireFrameCode check_payload(dtv_bytes payload, std::size_t expected)
{
    if (!view_valid(payload)) return WireFrameCode::invalid_argument;
    if (payload.size != expected) return WireFrameCode::protocol_error;
    return WireFrameCode::ok;
}

std::uint16_t read_u16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                      static_cast<std::uint16_t>(data[1]) << 8);
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

void write_u64(std::uint8_t *data, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        data[index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
}

/* (major, minor) の順で a < b を判定する。 */
bool version_less(std::uint16_t a_major, std::uint16_t a_minor,
                  std::uint16_t b_major, std::uint16_t b_minor)
{
    return a_major < b_major || (a_major == b_major && a_minor < b_minor);
}

bool hello_range_valid(const HelloRequest &value)
{
    return !version_less(value.max_major, value.max_minor,
                         value.min_major, value.min_minor);
}

void read_epoch(const std::uint8_t *data, std::array<std::uint8_t, 16> &epoch)
{
    std::copy_n(data, epoch.size(), epoch.begin());
}

void write_epoch(const std::array<std::uint8_t, 16> &epoch, std::uint8_t *data)
{
    std::copy_n(epoch.begin(), epoch.size(), data);
}

}

WireFrameCode decode_hello_request(dtv_bytes payload, HelloRequest *value)
{
    if (value == nullptr) return WireFrameCode::invalid_argument;
    const auto code = check_payload(payload, kHelloRequestSize);
    if (code != WireFrameCode::ok) return code;
    const auto *data = payload.data;
    HelloRequest decoded;
    decoded.min_major = read_u16(data);
    decoded.min_minor = read_u16(data + 2);
    decoded.max_major = read_u16(data + 4);
    decoded.max_minor = read_u16(data + 6);
    decoded.required_features = read_u64(data + 8);
    decoded.optional_features = read_u64(data + 16);
    if (!hello_range_valid(decoded)) return WireFrameCode::invalid_argument;
    *value = decoded;
    return WireFrameCode::ok;
}

WireFrameCode encode_hello_request(const HelloRequest &value, dtv_buffer *output)
{
    if (!output_valid(output)) return WireFrameCode::invalid_argument;
    if (!hello_range_valid(value)) return WireFrameCode::invalid_argument;
    if (output->capacity < kHelloRequestSize) return WireFrameCode::buffer_too_small;
    write_u16(output->data, value.min_major);
    write_u16(output->data + 2, value.min_minor);
    write_u16(output->data + 4, value.max_major);
    write_u16(output->data + 6, value.max_minor);
    write_u64(output->data + 8, value.required_features);
    write_u64(output->data + 16, value.optional_features);
    output->size = kHelloRequestSize;
    return WireFrameCode::ok;
}

WireFrameCode decode_acquire_request(dtv_bytes payload, AcquireRequest *value)
{
    if (value == nullptr) return WireFrameCode::invalid_argument;
    const auto code = check_payload(payload, kAcquireRequestSize);
    if (code != WireFrameCode::ok) return code;
    AcquireRequest decoded;
    read_epoch(payload.data, decoded.epoch);
    decoded.receiver_id = read_u16(payload.data + 16);
    *value = decoded;
    return WireFrameCode::ok;
}

WireFrameCode encode_acquire_request(const AcquireRequest &value, dtv_buffer *output)
{
    if (!output_valid(output)) return WireFrameCode::invalid_argument;
    if (output->capacity < kAcquireRequestSize) return WireFrameCode::buffer_too_small;
    write_epoch(value.epoch, output->data);
    write_u16(output->data + 16, value.receiver_id);
    output->size = kAcquireRequestSize;
    return WireFrameCode::ok;
}

WireFrameCode decode_acquire_response(dtv_bytes payload, AcquireResponse *value)
{
    if (value == nullptr) return WireFrameCode::invalid_argument;
    const auto code = check_payload(payload, kAcquireResponseSize);
    if (code != WireFrameCode::ok) return code;
    AcquireResponse decoded;
    read_epoch(payload.data, decoded.epoch);
    decoded.lease_id = read_u64(payload.data + 16);
    if (decoded.lease_id == 0) return WireFrameCode::invalid_argument;
    *value = decoded;
    return WireFrameCode::ok;
}

WireFrameCode encode_acquire_response(const AcquireResponse &value, dtv_buffer *output)
{
    if (!output_valid(output)) return WireFrameCode::invalid_argument;
    if (value.lease_id == 0) return WireFrameCode::invalid_argument;
    if (output->capacity < kAcquireResponseSize) return WireFrameCode::buffer_too_small;
    write_epoch(value.epoch, output->data);
    write_u64(output->data + 16, value.lease_id);
    output->size = kAcquireResponseSize;
    return WireFrameCode::ok;
}

WireFrameCode negotiate_hello_version(const HelloRequest &value, WireVersion *version)
{
    if (version == nullptr) return WireFrameCode::invalid_argument;
    if (!hello_range_valid(value)) return WireFrameCode::invalid_argument;
    const bool includes =
        !version_less(1, 0, value.min_major, value.min_minor) &&
        !version_less(value.max_major, value.max_minor, 1, 0);
    if (!includes) return WireFrameCode::version_mismatch;
    *version = WireVersion{1, 0};
    return WireFrameCode::ok;
}

}
