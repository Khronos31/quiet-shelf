// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <dtv/internal/wire-envelope.hpp>
#include <dtv/internal/wire-subset.hpp>

namespace {

using dtv::internal::AcquireRequest;
using dtv::internal::AcquireResponse;
using dtv::internal::FrameReceiver;
using dtv::internal::HelloRequest;
using dtv::internal::WireFrame;
using dtv::internal::WireFrameCode;
using dtv::internal::WireHeader;
using dtv::internal::WireVersion;
using dtv::internal::decode_acquire_request;
using dtv::internal::decode_acquire_response;
using dtv::internal::decode_hello_request;
using dtv::internal::encode_acquire_request;
using dtv::internal::encode_acquire_response;
using dtv::internal::encode_frame;
using dtv::internal::encode_hello_request;
using dtv::internal::negotiate_hello_version;

int check(bool value, int code) { return value ? 0 : code; }

bool same_hello(const HelloRequest &left, const HelloRequest &right)
{
    return left.min_major == right.min_major && left.min_minor == right.min_minor &&
           left.max_major == right.max_major && left.max_minor == right.max_minor &&
           left.required_features == right.required_features &&
           left.optional_features == right.optional_features;
}

bool same_acquire_request(const AcquireRequest &left, const AcquireRequest &right)
{
    return left.epoch == right.epoch && left.receiver_id == right.receiver_id;
}

bool same_acquire_response(const AcquireResponse &left, const AcquireResponse &right)
{
    return left.epoch == right.epoch && left.lease_id == right.lease_id;
}

bool unchanged(const std::array<std::uint8_t, 24> &bytes, const dtv_buffer &output,
               std::size_t size_before)
{
    return output.size == size_before &&
           std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) {
               return byte == 0xaa;
           });
}

struct Integration {
    HelloRequest hello{};
    AcquireRequest acquire{};
    AcquireResponse response{};
    int hello_count = 0;
    int acquire_count = 0;
    int response_count = 0;
    WireFrameCode failure = WireFrameCode::ok;
};

WireFrameCode handle_frame(void *user, const WireFrame &frame)
{
    auto *state = static_cast<Integration *>(user);
    if (frame.header.opcode == 0x0001) {
        ++state->hello_count;
        const auto code = decode_hello_request(frame.payload, &state->hello);
        if (code != WireFrameCode::ok) state->failure = code;
    } else if (frame.header.opcode == 0x0020 && frame.header.flags == 0) {
        ++state->acquire_count;
        const auto code = decode_acquire_request(frame.payload, &state->acquire);
        if (code != WireFrameCode::ok) state->failure = code;
    } else if (frame.header.opcode == 0x0020 && frame.header.flags == 1) {
        ++state->response_count;
        const auto code = decode_acquire_response(frame.payload, &state->response);
        if (code != WireFrameCode::ok) state->failure = code;
    } else {
        state->failure = WireFrameCode::protocol_error;
    }
    return WireFrameCode::ok;
}

std::vector<std::uint8_t> frame_with_payload(const WireHeader &header,
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
    const HelloRequest hello{1, 0, 1, 0, 0x0102030405060708ull, 0x1112131415161718ull};
    const std::array<std::uint8_t, 24> hello_golden{
        0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
    };
    {
        std::array<std::uint8_t, 24> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_hello_request(hello, &output) == WireFrameCode::ok &&
                                   output.size == 24 && encoded == hello_golden,
                               1)) return failed;
        HelloRequest decoded;
        if (int failed = check(
                decode_hello_request(dtv_bytes{hello_golden.data(), hello_golden.size()},
                                     &decoded) == WireFrameCode::ok &&
                    same_hello(decoded, hello),
                2)) return failed;
        if (int failed = check(decode_hello_request(
                                   dtv_bytes{hello_golden.data(), 23}, &decoded) ==
                                   WireFrameCode::protocol_error &&
                               decode_hello_request(
                                   dtv_bytes{hello_golden.data(), 25}, &decoded) ==
                                   WireFrameCode::protocol_error,
                               3)) return failed;
        if (int failed = check(decode_hello_request(dtv_bytes{nullptr, 24}, &decoded) ==
                                   WireFrameCode::invalid_argument &&
                               decode_hello_request(
                                   dtv_bytes{hello_golden.data(), hello_golden.size()},
                                   nullptr) == WireFrameCode::invalid_argument,
                               4)) return failed;
    }

    const HelloRequest wide{0, 9, 1, 1, 0xffffffffffffffffull, 0x8000000000000001ull};
    {
        std::array<std::uint8_t, 24> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        HelloRequest decoded;
        if (int failed = check(encode_hello_request(wide, &output) == WireFrameCode::ok &&
                                   decode_hello_request(
                                       dtv_bytes{encoded.data(), encoded.size()},
                                       &decoded) == WireFrameCode::ok &&
                                   same_hello(decoded, wide),
                               5)) return failed;
    }

    {
        WireVersion version;
        if (int failed = check(
                negotiate_hello_version(HelloRequest{1, 0, 1, 0}, &version) ==
                        WireFrameCode::ok &&
                    version.major == 1 && version.minor == 0,
                6)) return failed;
        if (int failed = check(negotiate_hello_version(HelloRequest{0, 9, 1, 1}, &version) ==
                                   WireFrameCode::ok &&
                               version.major == 1 && version.minor == 0,
                               7)) return failed;
        if (int failed = check(negotiate_hello_version(HelloRequest{2, 0, 3, 0}, &version) ==
                                   WireFrameCode::version_mismatch &&
                               negotiate_hello_version(HelloRequest{0, 1, 0, 9}, &version) ==
                                   WireFrameCode::version_mismatch &&
                               negotiate_hello_version(HelloRequest{1, 1, 2, 0}, &version) ==
                                   WireFrameCode::version_mismatch,
                               8)) return failed;
        if (int failed = check(negotiate_hello_version(HelloRequest{2, 0, 1, 0}, &version) ==
                                   WireFrameCode::invalid_argument &&
                               negotiate_hello_version(HelloRequest{1, 1, 1, 0}, &version) ==
                                   WireFrameCode::invalid_argument,
                               9)) return failed;
        if (int failed = check(negotiate_hello_version(HelloRequest{1, 0, 1, 0}, nullptr) ==
                                   WireFrameCode::invalid_argument,
                               10)) return failed;
    }

    {
        std::array<std::uint8_t, 24> encoded{};
        std::fill(encoded.begin(), encoded.end(), 0xaa);
        dtv_buffer output{encoded.data(), 23, 777};
        if (int failed = check(encode_hello_request(hello, &output) ==
                                   WireFrameCode::buffer_too_small &&
                                   unchanged(encoded, output, 777),
                               11)) return failed;
        std::fill(encoded.begin(), encoded.end(), 0xaa);
        dtv_buffer invalid_output{nullptr, 24, 777};
        if (int failed = check(encode_hello_request(hello, &invalid_output) ==
                                   WireFrameCode::invalid_argument,
                               12)) return failed;
        if (int failed = check(encode_hello_request(hello, nullptr) ==
                                   WireFrameCode::invalid_argument,
                               13)) return failed;
        std::fill(encoded.begin(), encoded.end(), 0xaa);
        dtv_buffer inverted_output{encoded.data(), encoded.size(), 777};
        if (int failed = check(encode_hello_request(HelloRequest{2, 0, 1, 0},
                                                    &inverted_output) ==
                                   WireFrameCode::invalid_argument &&
                                   unchanged(encoded, inverted_output, 777),
                               14)) return failed;
    }

    {
        const std::array<std::uint8_t, 24> inverted{
            0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        HelloRequest decoded;
        if (int failed = check(
                decode_hello_request(dtv_bytes{inverted.data(), inverted.size()},
                                     &decoded) == WireFrameCode::invalid_argument,
                15)) return failed;
    }

    const AcquireRequest acquire{{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
                                 0x1234};
    const std::array<std::uint8_t, 18> acquire_golden{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x34, 0x12,
    };
    {
        std::array<std::uint8_t, 18> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(
                encode_acquire_request(acquire, &output) == WireFrameCode::ok &&
                    output.size == 18 && encoded == acquire_golden,
                16)) return failed;
        AcquireRequest decoded;
        if (int failed = check(
                decode_acquire_request(dtv_bytes{acquire_golden.data(),
                                                 acquire_golden.size()},
                                       &decoded) == WireFrameCode::ok &&
                    same_acquire_request(decoded, acquire),
                17)) return failed;
        if (int failed = check(
                decode_acquire_request(dtv_bytes{acquire_golden.data(), 17}, &decoded) ==
                    WireFrameCode::protocol_error &&
                decode_acquire_request(dtv_bytes{acquire_golden.data(), 19}, &decoded) ==
                    WireFrameCode::protocol_error,
                18)) return failed;
        std::fill(encoded.begin(), encoded.end(), 0xaa);
        dtv_buffer short_output{encoded.data(), 17, 777};
        if (int failed = check(encode_acquire_request(acquire, &short_output) ==
                                   WireFrameCode::buffer_too_small,
                               19)) return failed;
    }

    for (const std::uint16_t receiver : {std::uint16_t{0}, std::uint16_t{0x00ff},
                                         std::uint16_t{0x0100}, std::uint16_t{0xffff}}) {
        const AcquireRequest value{{}, receiver};
        std::array<std::uint8_t, 18> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        AcquireRequest decoded;
        if (int failed = check(
                encode_acquire_request(value, &output) == WireFrameCode::ok &&
                    decode_acquire_request(dtv_bytes{encoded.data(), encoded.size()},
                                           &decoded) == WireFrameCode::ok &&
                    same_acquire_request(decoded, value),
                20)) return failed;
    }

    const AcquireResponse response{
        {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
         0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f},
        0x0102030405060708ull};
    const std::array<std::uint8_t, 24> response_golden{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    };
    {
        std::array<std::uint8_t, 24> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(
                encode_acquire_response(response, &output) == WireFrameCode::ok &&
                    output.size == 24 && encoded == response_golden,
                21)) return failed;
        AcquireResponse decoded;
        if (int failed = check(
                decode_acquire_response(dtv_bytes{response_golden.data(),
                                                  response_golden.size()},
                                        &decoded) == WireFrameCode::ok &&
                    same_acquire_response(decoded, response),
                22)) return failed;
        if (int failed = check(
                decode_acquire_response(dtv_bytes{response_golden.data(), 23}, &decoded) ==
                    WireFrameCode::protocol_error &&
                decode_acquire_response(dtv_bytes{response_golden.data(), 25}, &decoded) ==
                    WireFrameCode::protocol_error,
                23)) return failed;
        if (int failed = check(
                decode_acquire_response(dtv_bytes{nullptr, 24}, &decoded) ==
                    WireFrameCode::invalid_argument &&
                decode_acquire_response(dtv_bytes{response_golden.data(),
                                                  response_golden.size()},
                                        nullptr) == WireFrameCode::invalid_argument,
                24)) return failed;
    }

    {
        const AcquireResponse zero_lease{{}, 0};
        std::array<std::uint8_t, 24> encoded{};
        std::fill(encoded.begin(), encoded.end(), 0xaa);
        dtv_buffer output{encoded.data(), encoded.size(), 777};
        if (int failed = check(encode_acquire_response(zero_lease, &output) ==
                                   WireFrameCode::invalid_argument &&
                                   unchanged(encoded, output, 777),
                               25)) return failed;
        std::array<std::uint8_t, 24> zero_bytes{};
        AcquireResponse decoded;
        if (int failed = check(
                decode_acquire_response(dtv_bytes{zero_bytes.data(), zero_bytes.size()},
                                        &decoded) == WireFrameCode::invalid_argument,
                26)) return failed;
        const AcquireResponse saturated{
            {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
             0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
            0xffffffffffffffffull};
        dtv_buffer full_output{encoded.data(), encoded.size(), 0};
        if (int failed = check(
                encode_acquire_response(saturated, &full_output) == WireFrameCode::ok &&
                    decode_acquire_response(dtv_bytes{encoded.data(), encoded.size()},
                                            &decoded) == WireFrameCode::ok &&
                    same_acquire_response(decoded, saturated),
                27)) return failed;
        std::fill(encoded.begin(), encoded.end(), 0xaa);
        dtv_buffer short_output{encoded.data(), 23, 777};
        if (int failed = check(encode_acquire_response(response, &short_output) ==
                                   WireFrameCode::buffer_too_small &&
                                   unchanged(encoded, short_output, 777),
                               28)) return failed;
    }

    {
        std::vector<std::uint8_t> hello_payload(24);
        dtv_buffer hello_output{hello_payload.data(), hello_payload.size(), 0};
        std::vector<std::uint8_t> acquire_payload(18);
        dtv_buffer acquire_output{acquire_payload.data(), acquire_payload.size(), 0};
        std::vector<std::uint8_t> response_payload(24);
        dtv_buffer response_output{response_payload.data(), response_payload.size(), 0};
        if (int failed = check(
                encode_hello_request(hello, &hello_output) == WireFrameCode::ok &&
                    encode_acquire_request(acquire, &acquire_output) == WireFrameCode::ok &&
                    encode_acquire_response(response, &response_output) == WireFrameCode::ok,
                29)) return failed;

        const auto hello_frame =
            frame_with_payload(WireHeader{0x0001, 0, 1, 24}, hello_payload);
        const auto acquire_frame =
            frame_with_payload(WireHeader{0x0020, 0, 2, 18}, acquire_payload);
        const auto response_frame =
            frame_with_payload(WireHeader{0x0020, 1, 2, 24}, response_payload);
        if (int failed = check(hello_frame.size() == 44 && acquire_frame.size() == 38 &&
                                   response_frame.size() == 44,
                               30)) return failed;

        std::vector<std::uint8_t> stream = hello_frame;
        stream.insert(stream.end(), acquire_frame.begin(), acquire_frame.end());
        stream.insert(stream.end(), response_frame.begin(), response_frame.end());
        std::vector<std::uint8_t> storage(128);
        FrameReceiver receiver(dtv_buffer{storage.data(), storage.size(), 0});
        Integration integration;
        if (int failed = check(
                receiver.feed(dtv_bytes{stream.data(), stream.size()}, handle_frame,
                              &integration) == WireFrameCode::ok &&
                    receiver.finish() == WireFrameCode::ok &&
                    integration.failure == WireFrameCode::ok &&
                    integration.hello_count == 1 && integration.acquire_count == 1 &&
                    integration.response_count == 1 &&
                    same_hello(integration.hello, hello) &&
                    same_acquire_request(integration.acquire, acquire) &&
                    same_acquire_response(integration.response, response),
                31)) return failed;
    }

    return 0;
}
