// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <dtv/internal/wire-envelope.hpp>
#include <dtv/internal/wire-lnb-power.hpp>

namespace {

using dtv::internal::FrameReceiver;
using dtv::internal::LnbPower;
using dtv::internal::WireFrame;
using dtv::internal::WireFrameCode;
using dtv::internal::WireHeader;
using dtv::internal::decode_lnb_power;
using dtv::internal::encode_frame;
using dtv::internal::encode_lnb_power;

int check(bool value, int code) { return value ? 0 : code; }

bool same_lnb_power(const LnbPower &left, const LnbPower &right)
{
    return left.epoch == right.epoch && left.enabled == right.enabled;
}

bool unchanged(const std::array<std::uint8_t, 17> &bytes, const dtv_buffer &output,
               std::size_t size_before)
{
    return output.size == size_before &&
           std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) {
               return byte == 0xaa;
           });
}

struct Integration {
    LnbPower request{};
    LnbPower response{};
    int request_count = 0;
    int response_count = 0;
    WireFrameCode failure = WireFrameCode::ok;
};

WireFrameCode handle_frame(void *user, const WireFrame &frame)
{
    auto *state = static_cast<Integration *>(user);
    if (frame.header.opcode != dtv::internal::kWireOpcodeLnbPower) {
        state->failure = WireFrameCode::protocol_error;
        return WireFrameCode::ok;
    }
    if (frame.header.flags == 0) {
        ++state->request_count;
        const auto code = decode_lnb_power(frame.payload, &state->request);
        if (code != WireFrameCode::ok) state->failure = code;
    } else if (frame.header.flags == 1) {
        ++state->response_count;
        const auto code = decode_lnb_power(frame.payload, &state->response);
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
    const LnbPower request{
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
         0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
        true};
    const std::array<std::uint8_t, 17> request_golden{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x01,
    };
    const LnbPower response{
        {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
         0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f},
        false};
    const std::array<std::uint8_t, 17> response_golden{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x00,
    };

    {
        std::array<std::uint8_t, 17> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_lnb_power(request, &output) == WireFrameCode::ok &&
                                   output.size == 17 && encoded == request_golden,
                               1)) return failed;
        LnbPower decoded;
        if (int failed = check(
                decode_lnb_power(dtv_bytes{request_golden.data(), request_golden.size()},
                                 &decoded) == WireFrameCode::ok &&
                    same_lnb_power(decoded, request),
                2)) return failed;
    }
    {
        std::array<std::uint8_t, 17> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_lnb_power(response, &output) == WireFrameCode::ok &&
                                   output.size == 17 && encoded == response_golden,
                               3)) return failed;
        LnbPower decoded;
        if (int failed = check(
                decode_lnb_power(dtv_bytes{response_golden.data(), response_golden.size()},
                                 &decoded) == WireFrameCode::ok &&
                    same_lnb_power(decoded, response),
                4)) return failed;
    }

    {
        const LnbPower zero_epoch{{}, true};
        std::array<std::uint8_t, 17> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        LnbPower decoded;
        if (int failed = check(encode_lnb_power(zero_epoch, &output) == WireFrameCode::ok &&
                                   output.size == 17 &&
                                   decode_lnb_power(dtv_bytes{encoded.data(), encoded.size()},
                                                    &decoded) == WireFrameCode::ok &&
                                   same_lnb_power(decoded, zero_epoch),
                               5)) return failed;
    }

    {
        LnbPower decoded;
        if (int failed = check(
                decode_lnb_power(dtv_bytes{request_golden.data(), 16}, &decoded) ==
                    WireFrameCode::protocol_error &&
                decode_lnb_power(dtv_bytes{request_golden.data(), 18}, &decoded) ==
                    WireFrameCode::protocol_error,
                6)) return failed;
        if (int failed = check(decode_lnb_power(dtv_bytes{nullptr, 17}, &decoded) ==
                                   WireFrameCode::invalid_argument &&
                               decode_lnb_power(
                                   dtv_bytes{request_golden.data(), request_golden.size()},
                                   nullptr) == WireFrameCode::invalid_argument,
                               7)) return failed;
        auto bad_enabled = request_golden;
        bad_enabled[16] = 2;
        if (int failed = check(
                decode_lnb_power(dtv_bytes{bad_enabled.data(), bad_enabled.size()},
                                 &decoded) == WireFrameCode::invalid_argument,
                8)) return failed;
        bad_enabled[16] = 0xff;
        if (int failed = check(
                decode_lnb_power(dtv_bytes{bad_enabled.data(), bad_enabled.size()},
                                 &decoded) == WireFrameCode::invalid_argument,
                9)) return failed;
    }

    {
        std::array<std::uint8_t, 17> encoded{};
        std::fill(encoded.begin(), encoded.end(), 0xaa);
        dtv_buffer output{encoded.data(), 16, 777};
        if (int failed = check(encode_lnb_power(request, &output) ==
                                   WireFrameCode::buffer_too_small &&
                                   unchanged(encoded, output, 777),
                               10)) return failed;
        dtv_buffer invalid_output{nullptr, 17, 777};
        if (int failed = check(encode_lnb_power(request, &invalid_output) ==
                                   WireFrameCode::invalid_argument,
                               11)) return failed;
        if (int failed = check(encode_lnb_power(request, nullptr) ==
                                   WireFrameCode::invalid_argument,
                               12)) return failed;
    }

    {
        std::vector<std::uint8_t> request_payload(17);
        dtv_buffer request_output{request_payload.data(), request_payload.size(), 0};
        std::vector<std::uint8_t> response_payload(17);
        dtv_buffer response_output{response_payload.data(), response_payload.size(), 0};
        if (int failed = check(
                encode_lnb_power(request, &request_output) == WireFrameCode::ok &&
                    encode_lnb_power(response, &response_output) == WireFrameCode::ok,
                13)) return failed;

        const auto request_frame = frame_with_payload(
            WireHeader{dtv::internal::kWireOpcodeLnbPower, 0, 3, 17}, request_payload);
        const auto response_frame = frame_with_payload(
            WireHeader{dtv::internal::kWireOpcodeLnbPower, 1, 3, 17}, response_payload);
        if (int failed = check(request_frame.size() == 37 && response_frame.size() == 37,
                               14)) return failed;

        std::vector<std::uint8_t> stream = request_frame;
        stream.insert(stream.end(), response_frame.begin(), response_frame.end());
        std::vector<std::uint8_t> storage(64);
        FrameReceiver receiver(dtv_buffer{storage.data(), storage.size(), 0});
        Integration integration;
        if (int failed = check(
                receiver.feed(dtv_bytes{stream.data(), stream.size()}, handle_frame,
                              &integration) == WireFrameCode::ok &&
                    receiver.finish() == WireFrameCode::ok &&
                    integration.failure == WireFrameCode::ok &&
                    integration.request_count == 1 &&
                    integration.response_count == 1 &&
                    same_lnb_power(integration.request, request) &&
                    same_lnb_power(integration.response, response),
                15)) return failed;
    }

    return 0;
}
