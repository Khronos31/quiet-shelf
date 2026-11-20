// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <dtv/internal/wire-envelope.hpp>
#include <dtv/internal/wire-error-response.hpp>

namespace {

using dtv::internal::ErrorResponse;
using dtv::internal::ErrorTargetKind;
using dtv::internal::FrameReceiver;
using dtv::internal::WireFrame;
using dtv::internal::WireFrameCode;
using dtv::internal::WireHeader;
using dtv::internal::decode_error_response;
using dtv::internal::encode_error_response;
using dtv::internal::encode_frame;

int check(bool value, int code) { return value ? 0 : code; }

bool same_error(const ErrorResponse &left, const ErrorResponse &right)
{
    return left.epoch == right.epoch && left.code == right.code &&
           left.operation_length == right.operation_length &&
           std::equal(left.operation.begin(), left.operation.begin() + left.operation_length,
                      right.operation.begin()) &&
           left.detail_length == right.detail_length &&
           std::equal(left.detail.begin(), left.detail.begin() + left.detail_length,
                      right.detail.begin()) &&
           left.target_kind == right.target_kind && left.target_id == right.target_id &&
           left.has_retry_after_ms == right.has_retry_after_ms &&
           left.retry_after_ms == right.retry_after_ms &&
           left.has_required_payload_bytes == right.has_required_payload_bytes &&
           left.required_payload_bytes == right.required_payload_bytes;
}

void append_u16(std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
    }
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
    }
}

std::vector<std::uint8_t> raw_error(
    const std::array<std::uint8_t, 16> &epoch, std::uint32_t code,
    const std::vector<std::uint8_t> &operation, const std::vector<std::uint8_t> &detail,
    std::uint8_t kind, std::uint64_t id, bool has_retry, std::uint32_t retry,
    bool has_required, std::uint32_t required)
{
    std::vector<std::uint8_t> bytes(epoch.begin(), epoch.end());
    append_u32(bytes, code);
    append_u16(bytes, static_cast<std::uint16_t>(operation.size()));
    bytes.insert(bytes.end(), operation.begin(), operation.end());
    append_u16(bytes, static_cast<std::uint16_t>(detail.size()));
    bytes.insert(bytes.end(), detail.begin(), detail.end());
    bytes.push_back(kind);
    append_u64(bytes, id);
    bytes.push_back(has_retry ? 1 : 0);
    if (has_retry) append_u32(bytes, retry);
    bytes.push_back(has_required ? 1 : 0);
    if (has_required) append_u32(bytes, required);
    return bytes;
}

std::vector<std::uint8_t> make_error_frame(const WireHeader &header,
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

struct Integration {
    ErrorResponse value{};
    bool header_ok = false;
    WireFrameCode failure = WireFrameCode::ok;
};

WireFrameCode handle_frame(void *user, const WireFrame &frame)
{
    auto *state = static_cast<Integration *>(user);
    state->header_ok = frame.header.opcode == 0x0020 && frame.header.flags == 3 &&
                       frame.header.request_id == 5;
    const auto code = decode_error_response(frame.payload, &state->value);
    if (code != WireFrameCode::ok) state->failure = code;
    return WireFrameCode::ok;
}

}

int main()
{
    ErrorResponse golden{};
    for (std::size_t index = 0; index < golden.epoch.size(); ++index) {
        golden.epoch[index] = static_cast<std::uint8_t>(index);
    }
    golden.code = 18;
    golden.operation[0] = 'o';
    golden.operation[1] = 'p';
    golden.operation_length = 2;
    golden.target_kind = ErrorTargetKind::receiver;
    golden.target_id = 2;
    golden.has_required_payload_bytes = true;
    golden.required_payload_bytes = 4096;
    const std::array<std::uint8_t, 41> golden_bytes{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x12, 0x00, 0x00, 0x00, 0x02, 0x00, 0x6f, 0x70, 0x00, 0x00, 0x01,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x10, 0x00, 0x00,
    };
    {
        std::array<std::uint8_t, 41> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_error_response(golden, &output) == WireFrameCode::ok &&
                                   output.size == 41 && encoded == golden_bytes,
                               1)) return failed;
        ErrorResponse decoded;
        if (int failed = check(
                decode_error_response(dtv_bytes{golden_bytes.data(), golden_bytes.size()},
                                      &decoded) == WireFrameCode::ok &&
                    same_error(decoded, golden),
                2)) return failed;
    }

    ErrorResponse minimal{};
    minimal.code = 1;
    const std::array<std::uint8_t, 35> minimal_bytes{
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    {
        std::array<std::uint8_t, 35> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_error_response(minimal, &output) == WireFrameCode::ok &&
                                   output.size == 35 && encoded == minimal_bytes,
                               3)) return failed;
        ErrorResponse decoded;
        if (int failed = check(
                decode_error_response(dtv_bytes{minimal_bytes.data(), minimal_bytes.size()},
                                      &decoded) == WireFrameCode::ok &&
                    same_error(decoded, minimal),
                4)) return failed;
    }

    ErrorResponse maximum{};
    for (std::size_t index = 0; index < maximum.epoch.size(); ++index) {
        maximum.epoch[index] = static_cast<std::uint8_t>(0x10 + index);
    }
    maximum.code = 0x11223344;
    maximum.operation_length = 32;
    std::fill(maximum.operation.begin(), maximum.operation.end(), 'a');
    maximum.detail_length = 256;
    for (std::size_t index = 0; index < maximum.detail_length; index += 2) {
        maximum.detail[index] = 0xc3;
        maximum.detail[index + 1] = 0xa9;
    }
    maximum.target_kind = ErrorTargetKind::stream;
    maximum.target_id = 0x0102030405060708ull;
    maximum.has_retry_after_ms = true;
    maximum.retry_after_ms = 0x0a0b0c0d;
    maximum.has_required_payload_bytes = true;
    maximum.required_payload_bytes = 0x01020304;
    {
        std::array<std::uint8_t, 331> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        if (int failed = check(encode_error_response(maximum, &output) == WireFrameCode::ok &&
                                   output.size == 331,
                               5)) return failed;
        if (int failed = check(encoded[20] == 32 && encoded[21] == 0 &&
                                   encoded[22] == 'a' && encoded[54] == 0x00 &&
                                   encoded[55] == 0x01 && encoded[312] == 0x03,
                               6)) return failed;
        ErrorResponse decoded;
        if (int failed = check(
                decode_error_response(dtv_bytes{encoded.data(), encoded.size()}, &decoded) ==
                        WireFrameCode::ok &&
                    same_error(decoded, maximum),
                7)) return failed;
    }

    for (const auto flags : {std::array<bool, 2>{false, false}, std::array<bool, 2>{true, false},
                             std::array<bool, 2>{false, true}, std::array<bool, 2>{true, true}}) {
        auto value = golden;
        value.has_retry_after_ms = flags[0];
        value.retry_after_ms = flags[0] ? 0x55667788 : 0;
        value.has_required_payload_bytes = flags[1];
        value.required_payload_bytes = flags[1] ? 0x99aabbcc : 0;
        const auto expected = 35 + 2 + 0 + 4 * (flags[0] ? 1 : 0) + 4 * (flags[1] ? 1 : 0);
        std::array<std::uint8_t, 331> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        ErrorResponse decoded;
        if (int failed = check(
                encode_error_response(value, &output) == WireFrameCode::ok &&
                    output.size == expected &&
                    decode_error_response(dtv_bytes{encoded.data(), output.size}, &decoded) ==
                        WireFrameCode::ok &&
                    same_error(decoded, value),
                8)) return failed;
    }

    {
        ErrorResponse unknown{};
        unknown.code = 0xffffffff;
        unknown.target_kind = ErrorTargetKind::stream;
        std::array<std::uint8_t, 35> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        ErrorResponse decoded;
        if (int failed = check(
                encode_error_response(unknown, &output) == WireFrameCode::ok &&
                    decode_error_response(dtv_bytes{encoded.data(), output.size}, &decoded) ==
                        WireFrameCode::ok &&
                    same_error(decoded, unknown),
                9)) return failed;
    }

    {
        ErrorResponse decoded;
        if (int failed = check(
                decode_error_response(dtv_bytes{nullptr, 35}, &decoded) ==
                    WireFrameCode::invalid_argument &&
                decode_error_response(dtv_bytes{minimal_bytes.data(), minimal_bytes.size()},
                                      nullptr) == WireFrameCode::invalid_argument,
                10)) return failed;
        std::vector<std::uint8_t> short_payload(minimal_bytes.begin(), minimal_bytes.end() - 1);
        std::vector<std::uint8_t> long_payload(minimal_bytes.begin(), minimal_bytes.end());
        long_payload.push_back(0);
        std::vector<std::uint8_t> short_golden(golden_bytes.begin(), golden_bytes.end() - 1);
        std::vector<std::uint8_t> long_golden(golden_bytes.begin(), golden_bytes.end());
        long_golden.push_back(0);
        if (int failed = check(
                decode_error_response(dtv_bytes{short_payload.data(), short_payload.size()},
                                      &decoded) == WireFrameCode::protocol_error &&
                decode_error_response(dtv_bytes{long_payload.data(), long_payload.size()},
                                      &decoded) == WireFrameCode::protocol_error &&
                decode_error_response(dtv_bytes{short_golden.data(), short_golden.size()},
                                      &decoded) == WireFrameCode::protocol_error &&
                decode_error_response(dtv_bytes{long_golden.data(), long_golden.size()},
                                      &decoded) == WireFrameCode::protocol_error,
                11)) return failed;
    }

    {
        const auto long_operation = raw_error({}, 1, std::vector<std::uint8_t>(33, 'a'), {},
                                             0, 0, false, 0, false, 0);
        const auto long_detail = raw_error({}, 1, {}, std::vector<std::uint8_t>(257, 'd'),
                                           0, 0, false, 0, false, 0);
        ErrorResponse decoded;
        if (int failed = check(
                decode_error_response(dtv_bytes{long_operation.data(),
                                                long_operation.size()}, &decoded) ==
                    WireFrameCode::protocol_error &&
                decode_error_response(dtv_bytes{long_detail.data(), long_detail.size()},
                                      &decoded) == WireFrameCode::protocol_error,
                12)) return failed;
    }

    {
        auto checked = golden_bytes;
        ErrorResponse decoded;
        checked[35] = 2;
        const auto bad_retry = checked;
        checked = golden_bytes;
        checked[36] = 2;
        const auto bad_required = checked;
        checked = golden_bytes;
        checked[16] = checked[17] = checked[18] = checked[19] = 0;
        const auto zero_code = checked;
        checked = golden_bytes;
        checked[26] = 4;
        const auto bad_kind = checked;
        if (int failed = check(
                decode_error_response(dtv_bytes{bad_retry.data(), bad_retry.size()},
                                      &decoded) == WireFrameCode::protocol_error &&
                decode_error_response(dtv_bytes{bad_required.data(), bad_required.size()},
                                      &decoded) == WireFrameCode::protocol_error &&
                decode_error_response(dtv_bytes{zero_code.data(), zero_code.size()},
                                      &decoded) == WireFrameCode::invalid_argument &&
                decode_error_response(dtv_bytes{bad_kind.data(), bad_kind.size()},
                                      &decoded) == WireFrameCode::invalid_argument,
                13)) return failed;
    }

    {
        const std::array<std::array<std::uint8_t, 2>, 5> invalid_utf8{{
            {{0xff, 0x41}},
            {{0xc0, 0x80}},
            {{0xe3, 0x81}},
            {{0xed, 0xa0}},
            {{0x00, 0x41}},
        }};
        for (const auto &item : invalid_utf8) {
            auto bytes = golden_bytes;
            bytes[22] = item[0];
            bytes[23] = item[1];
            ErrorResponse decoded;
            if (int failed = check(
                    decode_error_response(dtv_bytes{bytes.data(), bytes.size()}, &decoded) ==
                        WireFrameCode::protocol_error,
                    14)) return failed;
        }
        const auto nul_detail = raw_error({}, 1, {}, std::vector<std::uint8_t>{0x00},
                                          0, 0, false, 0, false, 0);
        ErrorResponse decoded;
        if (int failed = check(
                decode_error_response(dtv_bytes{nul_detail.data(), nul_detail.size()},
                                      &decoded) == WireFrameCode::protocol_error,
                15)) return failed;
    }

    {
        const auto enclosure_nonzero = raw_error({}, 1, {}, {}, 0, 1, false, 0, false, 0);
        const auto receiver_too_large = raw_error({}, 1, {}, {}, 1, 0x10000, false, 0, false, 0);
        const auto reader_too_large = raw_error({}, 1, {}, {}, 2, 0x10000, false, 0, false, 0);
        const auto receiver_max = raw_error({}, 1, {}, {}, 1, 0xffff, false, 0, false, 0);
        const auto reader_max = raw_error({}, 1, {}, {}, 2, 0xffff, false, 0, false, 0);
        const auto stream_zero = raw_error({}, 1, {}, {}, 3, 0, false, 0, false, 0);
        ErrorResponse decoded;
        if (int failed = check(
                decode_error_response(dtv_bytes{enclosure_nonzero.data(),
                                                enclosure_nonzero.size()}, &decoded) ==
                    WireFrameCode::invalid_argument &&
                decode_error_response(dtv_bytes{receiver_too_large.data(),
                                                receiver_too_large.size()}, &decoded) ==
                    WireFrameCode::invalid_argument &&
                decode_error_response(dtv_bytes{reader_too_large.data(),
                                                reader_too_large.size()}, &decoded) ==
                    WireFrameCode::invalid_argument &&
                decode_error_response(dtv_bytes{receiver_max.data(), receiver_max.size()},
                                      &decoded) == WireFrameCode::ok &&
                decode_error_response(dtv_bytes{reader_max.data(), reader_max.size()},
                                      &decoded) == WireFrameCode::ok &&
                decode_error_response(dtv_bytes{stream_zero.data(), stream_zero.size()},
                                      &decoded) == WireFrameCode::ok &&
                decoded.target_kind == ErrorTargetKind::stream && decoded.target_id == 0,
                16)) return failed;
    }

    {
        ErrorResponse sentinel{};
        sentinel.code = 0xdeadbeef;
        sentinel.epoch.fill(0xaa);
        sentinel.operation.fill(0xbb);
        sentinel.operation_length = 3;
        sentinel.detail.fill(0xcc);
        sentinel.detail_length = 4;
        sentinel.target_kind = ErrorTargetKind::stream;
        sentinel.target_id = 0x1234;
        sentinel.has_retry_after_ms = true;
        sentinel.retry_after_ms = 7;
        sentinel.has_required_payload_bytes = true;
        sentinel.required_payload_bytes = 8;
        auto bad = golden_bytes;
        bad[35] = 2;
        auto unchanged_value = sentinel;
        if (int failed = check(
                decode_error_response(dtv_bytes{bad.data(), bad.size()}, &unchanged_value) ==
                        WireFrameCode::protocol_error &&
                    same_error(unchanged_value, sentinel),
                17)) return failed;
    }

    {
        std::array<std::uint8_t, 331> encoded{};
        std::fill(encoded.begin(), encoded.end(), 0xaa);
        dtv_buffer output{encoded.data(), 40, 777};
        if (int failed = check(
                encode_error_response(golden, &output) == WireFrameCode::buffer_too_small &&
                    output.size == 777 &&
                    std::all_of(encoded.begin(), encoded.end(),
                                [](std::uint8_t byte) { return byte == 0xaa; }),
                18)) return failed;

        auto invalid = golden;
        invalid.code = 0;
        dtv_buffer invalid_output{encoded.data(), encoded.size(), 777};
        if (int failed = check(
                encode_error_response(invalid, &invalid_output) ==
                        WireFrameCode::invalid_argument &&
                    invalid_output.size == 777,
                19)) return failed;

        dtv_buffer null_output{nullptr, 331, 0};
        if (int failed = check(encode_error_response(golden, &null_output) ==
                                   WireFrameCode::invalid_argument &&
                               encode_error_response(golden, nullptr) ==
                                   WireFrameCode::invalid_argument,
                               20)) return failed;

        const std::array<ErrorResponse, 6> invalid_values{{
            [] { ErrorResponse value; value.code = 0; return value; }(),
            [] { ErrorResponse value; value.code = 1; value.operation_length = 33; return value; }(),
            [] { ErrorResponse value; value.code = 1; value.detail_length = 257; return value; }(),
            [] { ErrorResponse value; value.code = 1; value.operation[0] = 0xff;
                 value.operation_length = 1; return value; }(),
            [] { ErrorResponse value; value.code = 1; value.operation[0] = 0x00;
                 value.operation_length = 1; return value; }(),
            [] { ErrorResponse value; value.code = 1;
                 value.target_kind = static_cast<ErrorTargetKind>(4); return value; }(),
        }};
        for (const auto &value : invalid_values) {
            dtv_buffer output{encoded.data(), encoded.size(), 777};
            if (int failed = check(encode_error_response(value, &output) ==
                                       WireFrameCode::invalid_argument &&
                                   output.size == 777,
                                   21)) return failed;
        }
    }

    {
        std::vector<std::uint8_t> payload(41);
        dtv_buffer payload_output{payload.data(), payload.size(), 0};
        if (int failed = check(encode_error_response(golden, &payload_output) ==
                                   WireFrameCode::ok && payload_output.size == 41,
                               22)) return failed;
        const auto frame = make_error_frame(WireHeader{0x0020, 3, 5, 41}, payload);
        if (int failed = check(frame.size() == 61, 23)) return failed;
        std::array<std::uint8_t, 64> storage{};
        FrameReceiver receiver(dtv_buffer{storage.data(), storage.size(), 0});
        Integration integration;
        if (int failed = check(
                receiver.feed(dtv_bytes{frame.data(), frame.size()}, handle_frame,
                              &integration) == WireFrameCode::ok &&
                    receiver.finish() == WireFrameCode::ok &&
                    integration.header_ok && integration.failure == WireFrameCode::ok &&
                    same_error(integration.value, golden),
                24)) return failed;
    }

    return 0;
}
