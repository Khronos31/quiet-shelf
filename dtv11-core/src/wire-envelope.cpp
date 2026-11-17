// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/wire-envelope.hpp>

#include <algorithm>

namespace dtv::internal {
namespace {

bool view_valid(dtv_bytes view)
{
    return view.size == 0 || view.data != nullptr;
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

std::uint32_t payload_limit(const WireHeader &header)
{
    return header.opcode == kWireOpcodeTsData ? kWireMaxTsDataPayload : kWireMaxControlPayload;
}

WireFrameCode validate_header(const WireHeader &header)
{
    const bool event = (header.opcode & kWireOpcodeEventBit) != 0;
    if (event) {
        if (header.flags != 0 || header.request_id != 0) return WireFrameCode::protocol_error;
    } else {
        if (header.flags != 0 && header.flags != 1 && header.flags != 3) {
            return WireFrameCode::protocol_error;
        }
        if (header.request_id == 0) return WireFrameCode::protocol_error;
    }
    if (header.payload_length > payload_limit(header)) return WireFrameCode::protocol_error;
    return WireFrameCode::ok;
}

void write_header(const WireHeader &header, std::uint8_t *output)
{
    output[0] = 'U';
    output[1] = 'D';
    output[2] = 'T';
    output[3] = 'V';
    write_u16(output + 4, 1);
    write_u16(output + 6, 0);
    write_u16(output + 8, header.opcode);
    write_u16(output + 10, header.flags);
    write_u32(output + 12, header.request_id);
    write_u32(output + 16, header.payload_length);
}

}

WireFrameCode decode_header(dtv_bytes input, WireHeader *header)
{
    if (header == nullptr || !view_valid(input)) return WireFrameCode::invalid_argument;
    if (input.size != kWireHeaderSize) return WireFrameCode::invalid_argument;
    const auto *data = input.data;
    const bool magic = data[0] == 'U' && data[1] == 'D' && data[2] == 'T' && data[3] == 'V';
    if (!magic) return WireFrameCode::protocol_error;
    if (read_u16(data + 4) != 1 || read_u16(data + 6) != 0) {
        return WireFrameCode::version_mismatch;
    }
    WireHeader decoded;
    decoded.opcode = read_u16(data + 8);
    decoded.flags = read_u16(data + 10);
    decoded.request_id = read_u32(data + 12);
    decoded.payload_length = read_u32(data + 16);
    const auto code = validate_header(decoded);
    if (code != WireFrameCode::ok) return code;
    *header = decoded;
    return WireFrameCode::ok;
}

WireFrameCode encode_header(const WireHeader &header, dtv_buffer *output)
{
    if (output == nullptr || (output->capacity != 0 && output->data == nullptr)) {
        return WireFrameCode::invalid_argument;
    }
    const auto code = validate_header(header);
    if (code != WireFrameCode::ok) return code;
    if (output->capacity < kWireHeaderSize) return WireFrameCode::buffer_too_small;
    write_header(header, output->data);
    output->size = kWireHeaderSize;
    return WireFrameCode::ok;
}

WireFrameCode encode_frame(const WireHeader &header, dtv_bytes payload, dtv_buffer *output)
{
    if (output == nullptr || (output->capacity != 0 && output->data == nullptr) ||
        !view_valid(payload)) {
        return WireFrameCode::invalid_argument;
    }
    const auto code = validate_header(header);
    if (code != WireFrameCode::ok) return code;
    if (payload.size != header.payload_length) return WireFrameCode::invalid_argument;
    const auto total = kWireHeaderSize + static_cast<std::size_t>(header.payload_length);
    if (output->capacity < total) return WireFrameCode::buffer_too_small;
    write_header(header, output->data);
    if (payload.size != 0) {
        std::copy_n(payload.data, payload.size, output->data + kWireHeaderSize);
    }
    output->size = total;
    return WireFrameCode::ok;
}

FrameReceiver::FrameReceiver(dtv_buffer payload_storage)
    : storage_(payload_storage),
      storage_valid_(!(payload_storage.capacity != 0 && payload_storage.data == nullptr))
{
}

WireFrameCode FrameReceiver::fail(WireFrameCode code)
{
    status_ = code;
    return code;
}

WireFrameCode FrameReceiver::read_header()
{
    const auto code = decode_header(
        dtv_bytes{header_data_.data(), kWireHeaderSize}, &header_);
    if (code != WireFrameCode::ok) return fail(code);
    if (static_cast<std::uint64_t>(header_.payload_length) > storage_.capacity) {
        return fail(WireFrameCode::buffer_too_small);
    }
    return WireFrameCode::ok;
}

WireFrameCode FrameReceiver::deliver(FrameHandler handler, void *user)
{
    const WireFrame frame{header_, dtv_bytes{storage_.data, header_.payload_length}};
    header_bytes_ = 0;
    payload_bytes_ = 0;
    header_ = WireHeader{};
    const auto code = handler(user, frame);
    if (code != WireFrameCode::ok) return fail(code);
    return WireFrameCode::ok;
}

WireFrameCode FrameReceiver::feed(dtv_bytes input, FrameHandler handler, void *user)
{
    if (!storage_valid_) return WireFrameCode::invalid_argument;
    if (status_ != WireFrameCode::ok) return status_;
    if (handler == nullptr || !view_valid(input)) return WireFrameCode::invalid_argument;

    std::size_t consumed = 0;
    while (consumed < input.size) {
        if (header_bytes_ < kWireHeaderSize) {
            const auto count = std::min(kWireHeaderSize - header_bytes_,
                                        input.size - consumed);
            std::copy_n(input.data + consumed, count,
                        header_data_.begin() + static_cast<std::ptrdiff_t>(header_bytes_));
            header_bytes_ += count;
            consumed += count;
            if (header_bytes_ < kWireHeaderSize) break;
            const auto code = read_header();
            if (code != WireFrameCode::ok) return code;
            if (header_.payload_length != 0) continue;
            const auto delivered = deliver(handler, user);
            if (delivered != WireFrameCode::ok) return delivered;
            continue;
        }

        const auto need = static_cast<std::size_t>(header_.payload_length) - payload_bytes_;
        const auto count = std::min(need, input.size - consumed);
        std::copy_n(input.data + consumed, count, storage_.data + payload_bytes_);
        payload_bytes_ += count;
        consumed += count;
        if (payload_bytes_ != header_.payload_length) break;
        const auto delivered = deliver(handler, user);
        if (delivered != WireFrameCode::ok) return delivered;
    }
    return WireFrameCode::ok;
}

WireFrameCode FrameReceiver::finish()
{
    if (!storage_valid_) return WireFrameCode::invalid_argument;
    if (status_ != WireFrameCode::ok) return status_;
    if (pending_size() == 0) return WireFrameCode::ok;
    truncated_ = true;
    return fail(WireFrameCode::protocol_error);
}

void FrameReceiver::reset()
{
    status_ = WireFrameCode::ok;
    truncated_ = false;
    header_bytes_ = 0;
    payload_bytes_ = 0;
    header_ = WireHeader{};
}

}
