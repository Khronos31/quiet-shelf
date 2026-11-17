// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <dtv/internal/wire-envelope.hpp>

namespace {

using dtv::internal::FrameReceiver;
using dtv::internal::WireFrame;
using dtv::internal::WireFrameCode;
using dtv::internal::WireHeader;
using dtv::internal::decode_header;
using dtv::internal::encode_frame;
using dtv::internal::encode_header;

int check(bool value, int code) { return value ? 0 : code; }

std::array<std::uint8_t, 20> raw_header(std::array<char, 4> magic, std::uint16_t major,
                                        std::uint16_t minor, const WireHeader &header)
{
    std::array<std::uint8_t, 20> bytes{};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    const auto put16 = [&bytes](std::size_t offset, std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto put32 = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    put16(4, major);
    put16(6, minor);
    put16(8, header.opcode);
    put16(10, header.flags);
    put32(12, header.request_id);
    put32(16, header.payload_length);
    return bytes;
}

WireFrameCode decode(const std::array<std::uint8_t, 20> &bytes, WireHeader *header)
{
    return decode_header(dtv_bytes{bytes.data(), bytes.size()}, header);
}

bool same_header(const WireHeader &left, const WireHeader &right)
{
    return left.opcode == right.opcode && left.flags == right.flags &&
           left.request_id == right.request_id &&
           left.payload_length == right.payload_length;
}

struct Collected {
    std::vector<WireHeader> headers;
    std::vector<std::vector<std::uint8_t>> payloads;
};

WireFrameCode collect_frame(void *user, const WireFrame &frame)
{
    auto *collected = static_cast<Collected *>(user);
    collected->headers.push_back(frame.header);
    collected->payloads.push_back(
        frame.payload.size == 0
            ? std::vector<std::uint8_t>{}
            : std::vector<std::uint8_t>(frame.payload.data,
                                        frame.payload.data + frame.payload.size));
    return WireFrameCode::ok;
}

WireFrameCode refuse_frame(void *user, const WireFrame &)
{
    ++*static_cast<int *>(user);
    return WireFrameCode::protocol_error;
}

std::vector<std::uint8_t> make_frame(const WireHeader &header,
                                     const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes(dtv::internal::kWireHeaderSize + payload.size());
    dtv_buffer output{bytes.data(), bytes.size(), 0};
    if (encode_frame(header, dtv_bytes{payload.data(), payload.size()}, &output) !=
        WireFrameCode::ok) {
        return {};
    }
    bytes.resize(static_cast<std::size_t>(output.size));
    return bytes;
}

}

int main()
{
    const WireHeader request{0x0010, 0, 0x01020304, 3};
    const WireHeader response{0x0010, 1, 0x05060708, 2};
    const WireHeader error{0x0011, 3, 0x11223344, 1};
    const WireHeader event{0x80f0, 0, 0, 4};
    const std::array<WireHeader, 4> headers{request, response, error, event};
    const std::array<std::array<std::uint8_t, 20>, 4> golden{{
        {0x55, 0x44, 0x54, 0x56, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00,
         0x00, 0x00, 0x04, 0x03, 0x02, 0x01, 0x03, 0x00, 0x00, 0x00},
        {0x55, 0x44, 0x54, 0x56, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00,
         0x01, 0x00, 0x08, 0x07, 0x06, 0x05, 0x02, 0x00, 0x00, 0x00},
        {0x55, 0x44, 0x54, 0x56, 0x01, 0x00, 0x00, 0x00, 0x11, 0x00,
         0x03, 0x00, 0x44, 0x33, 0x22, 0x11, 0x01, 0x00, 0x00, 0x00},
        {0x55, 0x44, 0x54, 0x56, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x80,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00},
    }};

    for (std::size_t index = 0; index < headers.size(); ++index) {
        std::array<std::uint8_t, 20> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_header(headers[index], &output) == WireFrameCode::ok &&
                                   output.size == 20 && encoded == golden[index],
                               1)) return failed;
        WireHeader decoded;
        if (int failed = check(decode(golden[index], &decoded) == WireFrameCode::ok &&
                                   same_header(decoded, headers[index]),
                               2)) return failed;
    }

    const WireHeader unknown_request{0x1234, 0, 9, 0};
    const WireHeader unknown_event{0x8abc, 0, 0, 0};
    for (const auto &header : {unknown_request, unknown_event}) {
        const auto bytes = raw_header({'U', 'D', 'T', 'V'}, 1, 0, header);
        WireHeader decoded;
        if (int failed = check(decode(bytes, &decoded) == WireFrameCode::ok &&
                                   same_header(decoded, header),
                               3)) return failed;
        std::array<std::uint8_t, 20> encoded{};
        dtv_buffer encode_output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_header(header, &encode_output) == WireFrameCode::ok &&
                                   encoded == bytes,
                               4)) return failed;
    }

    struct BadHeader {
        std::array<char, 4> magic;
        std::uint16_t major;
        std::uint16_t minor;
        WireHeader header;
        WireFrameCode expected;
    };
    const std::array<BadHeader, 12> bad_headers{{
        {{'P', 'X', '4', 'U'}, 1, 0, request, WireFrameCode::protocol_error},
        {{'A', 'S', 'C', 'N'}, 1, 0, request, WireFrameCode::protocol_error},
        {{'U', 'D', 'T', 'V'}, 2, 0, request, WireFrameCode::version_mismatch},
        {{'U', 'D', 'T', 'V'}, 1, 1, request, WireFrameCode::version_mismatch},
        {{'U', 'D', 'T', 'V'}, 1, 0, {0x0010, 2, 1, 0}, WireFrameCode::protocol_error},
        {{'U', 'D', 'T', 'V'}, 1, 0, {0x0010, 4, 1, 0}, WireFrameCode::protocol_error},
        {{'U', 'D', 'T', 'V'}, 1, 0, {0x0010, 0, 0, 0}, WireFrameCode::protocol_error},
        {{'U', 'D', 'T', 'V'}, 1, 0, {0x0010, 1, 0, 0}, WireFrameCode::protocol_error},
        {{'U', 'D', 'T', 'V'}, 1, 0, {0x0011, 3, 0, 0}, WireFrameCode::protocol_error},
        {{'U', 'D', 'T', 'V'}, 1, 0, {0x80f0, 1, 1, 0}, WireFrameCode::protocol_error},
        {{'U', 'D', 'T', 'V'}, 1, 0, {0x80f0, 0, 1, 0}, WireFrameCode::protocol_error},
        {{'U', 'D', 'T', 'V'}, 1, 0, {0x80f0, 3, 0, 0}, WireFrameCode::protocol_error},
    }};
    for (const auto &bad : bad_headers) {
        const auto bytes = raw_header(bad.magic, bad.major, bad.minor, bad.header);
        WireHeader decoded;
        if (int failed = check(decode(bytes, &decoded) == bad.expected, 5)) return failed;
    }

    struct CapCase {
        WireHeader header;
        WireFrameCode expected;
    };
    const std::array<CapCase, 6> cap_cases{{
        {{0x8040, 0, 0, 1048504}, WireFrameCode::ok},
        {{0x8040, 0, 0, 1048505}, WireFrameCode::protocol_error},
        {{0x0010, 0, 1, 65536}, WireFrameCode::ok},
        {{0x0010, 0, 1, 65537}, WireFrameCode::protocol_error},
        {{0x8001, 0, 0, 65536}, WireFrameCode::ok},
        {{0x8001, 0, 0, 65537}, WireFrameCode::protocol_error},
    }};
    for (const auto &cap : cap_cases) {
        const auto bytes = raw_header({'U', 'D', 'T', 'V'}, 1, 0, cap.header);
        WireHeader decoded;
        if (int failed = check(decode(bytes, &decoded) == cap.expected, 6)) return failed;
        std::array<std::uint8_t, 20> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_header(cap.header, &output) == cap.expected, 7)) {
            return failed;
        }
    }

    const std::vector<std::uint8_t> request_payload{0x01, 0x02, 0x03};
    const auto request_frame = make_frame(request, request_payload);
    if (int failed = check(request_frame.size() == 23 &&
                               std::equal(golden[0].begin(), golden[0].end(),
                                          request_frame.begin()) &&
                               std::equal(request_payload.begin(), request_payload.end(),
                                          request_frame.begin() + 20),
                           8)) return failed;

    {
        std::vector<std::uint8_t> too_small(22, 0xaa);
        dtv_buffer output{too_small.data(), too_small.size(), 1234};
        if (int failed = check(encode_frame(request,
                                            dtv_bytes{request_payload.data(),
                                                      request_payload.size()},
                                            &output) == WireFrameCode::buffer_too_small &&
                                   output.size == 1234 &&
                                   std::all_of(too_small.begin(), too_small.end(),
                                               [](std::uint8_t byte) { return byte == 0xaa; }),
                               9)) return failed;
    }
    {
        std::array<std::uint8_t, 23> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        const std::vector<std::uint8_t> short_payload{0x01, 0x02};
        if (int failed = check(encode_frame(request,
                                            dtv_bytes{short_payload.data(),
                                                      short_payload.size()},
                                            &output) == WireFrameCode::invalid_argument &&
                                   output.size == 0,
                               10)) return failed;
        if (int failed = check(encode_frame(request, dtv_bytes{nullptr, 3}, &output) ==
                                   WireFrameCode::invalid_argument,
                               11)) return failed;
        dtv_buffer invalid_output{nullptr, 23, 0};
        if (int failed = check(encode_frame(request,
                                            dtv_bytes{request_payload.data(),
                                                      request_payload.size()},
                                            &invalid_output) ==
                                   WireFrameCode::invalid_argument,
                               12)) return failed;
        if (int failed = check(encode_header(request, nullptr) ==
                                   WireFrameCode::invalid_argument,
                               13)) return failed;
    }
    {
        std::array<std::uint8_t, 19> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_header(request, &output) ==
                                   WireFrameCode::buffer_too_small &&
                                   output.size == 0,
                               14)) return failed;
    }
    {
        WireHeader decoded;
        if (int failed = check(decode_header(dtv_bytes{nullptr, 20}, &decoded) ==
                                   WireFrameCode::invalid_argument,
                               15)) return failed;
        if (int failed = check(decode_header(dtv_bytes{golden[0].data(), 19}, &decoded) ==
                                   WireFrameCode::invalid_argument,
                               16)) return failed;
        if (int failed = check(decode_header(dtv_bytes{golden[0].data(), 21}, &decoded) ==
                                   WireFrameCode::invalid_argument,
                               17)) return failed;
        if (int failed = check(decode_header(dtv_bytes{golden[0].data(), golden[0].size()},
                                             nullptr) == WireFrameCode::invalid_argument,
                               18)) return failed;
    }

    std::vector<std::uint8_t> storage(1048504);
    const dtv_buffer storage_view{storage.data(), storage.size(), 0};
    const std::vector<std::uint8_t> large_payload(100, 0x5a);
    const WireHeader event_large{0x8040, 0, 0, 100};
    const WireHeader empty_response{0x0020, 1, 0x05060708, 0};
    const auto first = make_frame(request, request_payload);
    const auto second = make_frame(event_large, large_payload);
    const auto third = make_frame(empty_response, {});
    std::vector<std::uint8_t> stream;
    stream.insert(stream.end(), first.begin(), first.end());
    stream.insert(stream.end(), second.begin(), second.end());
    stream.insert(stream.end(), third.begin(), third.end());

    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        for (const auto byte : stream) {
            const std::array<std::uint8_t, 1> chunk{byte};
            if (int failed = check(receiver.feed(dtv_bytes{chunk.data(), chunk.size()},
                                                 collect_frame, &collected) ==
                                       WireFrameCode::ok,
                                   19)) return failed;
        }
        if (int failed = check(collected.headers.size() == 3 &&
                                   same_header(collected.headers[0], request) &&
                                   same_header(collected.headers[1], event_large) &&
                                   same_header(collected.headers[2], empty_response) &&
                                   collected.payloads[0] == request_payload &&
                                   collected.payloads[1] == large_payload &&
                                   collected.payloads[2].empty() &&
                                   receiver.pending_size() == 0 &&
                                   receiver.finish() == WireFrameCode::ok,
                               20)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        const std::array<std::size_t, 7> chunks{7, 1, 12, 3, 250, 1, 5};
        std::size_t offset = 0;
        for (const auto requested : chunks) {
            const auto count = std::min(requested, stream.size() - offset);
            if (int failed = check(receiver.feed(
                                       dtv_bytes{stream.data() + offset, count},
                                       collect_frame, &collected) == WireFrameCode::ok,
                                   21)) return failed;
            offset += count;
        }
        if (int failed = check(offset == stream.size() &&
                                   collected.headers.size() == 3 &&
                                   collected.payloads[1] == large_payload &&
                                   receiver.finish() == WireFrameCode::ok,
                               22)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        if (int failed = check(receiver.feed(dtv_bytes{stream.data(), stream.size()},
                                             collect_frame, &collected) ==
                                   WireFrameCode::ok &&
                                   collected.headers.size() == 3,
                               23)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        if (int failed = check(receiver.feed(dtv_bytes{first.data(), first.size()},
                                             collect_frame, &collected) ==
                                   WireFrameCode::ok &&
                                   receiver.finish() == WireFrameCode::ok &&
                                   collected.headers.size() == 1,
                               24)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        if (int failed = check(receiver.feed(dtv_bytes{first.data(), 10},
                                             collect_frame, &collected) ==
                                   WireFrameCode::ok &&
                                   receiver.finish() == WireFrameCode::protocol_error &&
                                   receiver.truncated() &&
                                   collected.headers.empty() &&
                                   receiver.feed(dtv_bytes{first.data() + 10,
                                                           first.size() - 10},
                                                 collect_frame, &collected) ==
                                       WireFrameCode::protocol_error &&
                                   collected.headers.empty(),
                               25)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        if (int failed = check(receiver.feed(dtv_bytes{first.data(), 21},
                                             collect_frame, &collected) ==
                                   WireFrameCode::ok &&
                                   receiver.finish() == WireFrameCode::protocol_error &&
                                   receiver.truncated() &&
                                   collected.headers.empty() &&
                                   receiver.feed(dtv_bytes{first.data() + 21,
                                                           first.size() - 21},
                                                 collect_frame, &collected) ==
                                       WireFrameCode::protocol_error,
                               26)) return failed;
    }

    {
        std::array<std::uint8_t, 2> small_storage{0x5a, 0x5a};
        FrameReceiver receiver(dtv_buffer{small_storage.data(), small_storage.size(), 0});
        Collected collected;
        if (int failed = check(receiver.feed(dtv_bytes{first.data(), first.size()},
                                             collect_frame, &collected) ==
                                   WireFrameCode::buffer_too_small &&
                                   collected.headers.empty() &&
                                   small_storage[0] == 0x5a && small_storage[1] == 0x5a &&
                                   receiver.feed(dtv_bytes{first.data(), first.size()},
                                                 collect_frame, &collected) ==
                                       WireFrameCode::buffer_too_small,
                               27)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        const auto too_long = raw_header({'U', 'D', 'T', 'V'}, 1, 0,
                                         WireHeader{0x0010, 0, 1, 65537});
        if (int failed = check(receiver.feed(dtv_bytes{too_long.data(), too_long.size()},
                                             collect_frame, &collected) ==
                                   WireFrameCode::protocol_error &&
                                   collected.headers.empty(),
                               28)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        int calls = 0;
        if (int failed = check(receiver.feed(dtv_bytes{first.data(), first.size()},
                                             refuse_frame, &calls) ==
                                   WireFrameCode::protocol_error &&
                                   calls == 1 &&
                                   receiver.feed(dtv_bytes{first.data(), first.size()},
                                                 refuse_frame, &calls) ==
                                       WireFrameCode::protocol_error &&
                                   calls == 1,
                               29)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        if (int failed = check(receiver.feed(dtv_bytes{nullptr, 1}, collect_frame,
                                             &collected) ==
                                   WireFrameCode::invalid_argument &&
                                   receiver.status() == WireFrameCode::ok &&
                                   receiver.feed(dtv_bytes{first.data(), first.size()},
                                                 nullptr, &collected) ==
                                       WireFrameCode::invalid_argument &&
                                   receiver.feed(dtv_bytes{first.data(), first.size()},
                                                 collect_frame, &collected) ==
                                       WireFrameCode::ok &&
                                   collected.headers.size() == 1,
                               30)) return failed;
    }
    {
        FrameReceiver receiver(storage_view);
        Collected collected;
        const auto bad_magic = raw_header({'P', 'X', '4', 'U'}, 1, 0, response);
        std::vector<std::uint8_t> mixed = first;
        mixed.insert(mixed.end(), bad_magic.begin(), bad_magic.end());
        if (int failed = check(receiver.feed(dtv_bytes{mixed.data(), mixed.size()},
                                             collect_frame, &collected) ==
                                   WireFrameCode::protocol_error &&
                                   collected.headers.size() == 1 &&
                                   same_header(collected.headers[0], request),
                               31)) return failed;
    }
    {
        const std::vector<std::uint8_t> cap_payload(1048504, 0x77);
        const WireHeader cap_header{0x8040, 0, 0, 1048504};
        const auto cap_frame = make_frame(cap_header, cap_payload);
        FrameReceiver receiver(storage_view);
        Collected collected;
        if (int failed = check(cap_frame.size() == 1048524 &&
                                   receiver.feed(dtv_bytes{cap_frame.data(), 500000},
                                                 collect_frame, &collected) ==
                                       WireFrameCode::ok &&
                                   collected.headers.empty() &&
                                   receiver.pending_size() == 500000 &&
                                   receiver.feed(dtv_bytes{cap_frame.data() + 500000,
                                                           cap_frame.size() - 500000},
                                                 collect_frame, &collected) ==
                                       WireFrameCode::ok &&
                                   collected.headers.size() == 1 &&
                                   collected.payloads[0] == cap_payload &&
                                   receiver.finish() == WireFrameCode::ok,
                               32)) return failed;
    }
    {
        FrameReceiver invalid(dtv_buffer{nullptr, 20, 0});
        if (int failed = check(!invalid.valid() &&
                                   invalid.feed(dtv_bytes{}, collect_frame, nullptr) ==
                                       WireFrameCode::invalid_argument &&
                                   invalid.finish() == WireFrameCode::invalid_argument,
                               33)) return failed;
    }
    {
        const auto bad_magic = raw_header({'X', 'D', 'T', 'V'}, 1, 0, request);
        const auto bad_version = raw_header({'U', 'D', 'T', 'V'}, 2, 0, request);
        FrameReceiver poisoned(storage_view);
        Collected collected;
        if (int failed = check(poisoned.feed(dtv_bytes{bad_magic.data(), bad_magic.size()},
                                             collect_frame, &collected) ==
                                   WireFrameCode::protocol_error,
                               34)) return failed;
        poisoned.reset();
        if (int failed = check(poisoned.feed(dtv_bytes{bad_version.data(),
                                                       bad_version.size()},
                                             collect_frame, &collected) ==
                                   WireFrameCode::version_mismatch,
                               35)) return failed;
        poisoned.reset();
        if (int failed = check(poisoned.feed(dtv_bytes{first.data(), first.size()},
                                             collect_frame, &collected) ==
                                   WireFrameCode::ok &&
                                   collected.headers.size() == 1,
                               36)) return failed;
    }

    return 0;
}
