// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dtv/internal/wire-hello-list.hpp>

namespace {

using dtv::internal::HelloResponse;
using dtv::internal::ListRequest;
using dtv::internal::WireFrameCode;
using dtv::internal::decode_hello_response;
using dtv::internal::decode_list_request;
using dtv::internal::encode_hello_response;
using dtv::internal::encode_list_request;
using dtv::internal::hello_response_payload_size;

int check(bool value, int code) { return value ? 0 : code; }

bool same_hello_response(const HelloResponse &left, const HelloResponse &right)
{
    return left.major == right.major && left.minor == right.minor &&
           left.features == right.features && left.epoch == right.epoch &&
           left.product == right.product && left.version == right.version &&
           left.core_version == right.core_version &&
           left.max_control_payload == right.max_control_payload &&
           left.max_ts_bytes == right.max_ts_bytes;
}

bool unchanged(const std::vector<std::uint8_t> &bytes, const dtv_buffer &output,
               std::size_t size_before)
{
    return output.size == size_before &&
           std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) {
               return byte == 0xaa;
           });
}

struct HelloCase {
    HelloResponse value;
    std::vector<std::uint8_t> golden;
};

}

int main()
{
    HelloResponse response;
    response.major = 1;
    response.minor = 1;
    response.features = 5;
    response.epoch = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                      0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
    response.product = "px4";
    response.version = "1.2.3";
    response.max_control_payload = 65536;
    response.max_ts_bytes = 1048476;
    const std::array<std::uint8_t, 49> tag_zero_golden{
        0x01, 0x00, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xa0, 0xa1, 0xa2, 0xa3,
        0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab,
        0xac, 0xad, 0xae, 0xaf, 0x03, 0x00, 0x70, 0x78,
        0x34, 0x05, 0x00, 0x31, 0x2e, 0x32, 0x2e, 0x33,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x9c, 0xff, 0x0f,
        0x00,
    };

    HelloResponse with_core;
    with_core.major = 1;
    with_core.minor = 1;
    with_core.features = 0x8877665544332211ull;
    with_core.epoch = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                       0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    with_core.product = "x";
    with_core.version = "v";
    with_core.core_version = "c";
    with_core.max_control_payload = 0x01020304;
    with_core.max_ts_bytes = 0xa1b2c3d4;
    const std::array<std::uint8_t, 46> tag_one_golden{
        0x01, 0x00, 0x01, 0x00, 0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88, 0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f, 0x01, 0x00, 0x78, 0x01,
        0x00, 0x76, 0x01, 0x01, 0x00, 0x63, 0x04, 0x03,
        0x02, 0x01, 0xd4, 0xc3, 0xb2, 0xa1,
    };
    const std::array<HelloCase, 2> cases{{
        {response, std::vector<std::uint8_t>(tag_zero_golden.begin(), tag_zero_golden.end())},
        {with_core, std::vector<std::uint8_t>(tag_one_golden.begin(), tag_one_golden.end())},
    }};
    for (const auto &item : cases) {
        std::size_t size = 999;
        if (int failed = check(hello_response_payload_size(item.value, &size) ==
                                   WireFrameCode::ok && size == item.golden.size(),
                               1)) return failed;
        std::vector<std::uint8_t> encoded(dtv::internal::kHelloResponseMaxSize, 0xaa);
        dtv_buffer output{encoded.data(), encoded.size(), 777};
        if (int failed = check(encode_hello_response(item.value, &output) ==
                                   WireFrameCode::ok && output.size == item.golden.size() &&
                                   std::equal(item.golden.begin(), item.golden.end(),
                                              encoded.begin()),
                               2)) return failed;
        HelloResponse decoded;
        if (int failed = check(decode_hello_response(
                                   dtv_bytes{item.golden.data(), item.golden.size()},
                                   &decoded) == WireFrameCode::ok &&
                                   same_hello_response(decoded, item.value),
                               3)) return failed;
    }

    {
        HelloResponse empty;
        empty.major = 1;
        empty.minor = 1;
        empty.core_version = std::string{};
        std::size_t size = 999;
        std::vector<std::uint8_t> encoded(dtv::internal::kHelloResponseMaxSize);
        dtv_buffer output{encoded.data(), encoded.size(), 0};
        HelloResponse decoded;
        if (int failed = check(hello_response_payload_size(empty, &size) ==
                                   WireFrameCode::ok && size == 43 &&
                                   encode_hello_response(empty, &output) ==
                                       WireFrameCode::ok && output.size == 43 &&
                                   encoded[28] == 0 && encoded[29] == 0 &&
                                   encoded[30] == 0 && encoded[31] == 0 &&
                                   encoded[32] == 1 && encoded[33] == 0 &&
                                   encoded[34] == 0 &&
                                   decode_hello_response(dtv_bytes{encoded.data(), output.size},
                                                         &decoded) == WireFrameCode::ok &&
                                   same_hello_response(decoded, empty),
                               4)) return failed;
    }

    {
        HelloResponse maximum;
        maximum.major = 1;
        maximum.minor = 1;
        maximum.features = 0xffffffffffffffffull;
        maximum.product = std::string(32, 'p');
        maximum.version = std::string(64, 'v');
        maximum.core_version = std::string(64, 'c');
        maximum.max_control_payload = 0xffffffff;
        maximum.max_ts_bytes = 0xffffffff;
        std::vector<std::uint8_t> encoded(dtv::internal::kHelloResponseMaxSize, 0xaa);
        dtv_buffer output{encoded.data(), encoded.size(), 777};
        HelloResponse decoded;
        if (int failed = check(hello_response_payload_size(maximum, &output.size) ==
                                   WireFrameCode::ok && output.size == 203 &&
                                   encode_hello_response(maximum, &output) ==
                                       WireFrameCode::ok && output.size == 203 &&
                                   encoded[28] == 32 && encoded[29] == 0 &&
                                   encoded[62] == 64 && encoded[63] == 0 &&
                                   encoded[128] == 1 && encoded[129] == 64 &&
                                   encoded[130] == 0 && encoded[195] == 0xff &&
                                   encoded[202] == 0xff &&
                                   decode_hello_response(dtv_bytes{encoded.data(), output.size},
                                                         &decoded) == WireFrameCode::ok &&
                                   same_hello_response(decoded, maximum),
                               5)) return failed;
    }

    {
        HelloResponse utf8;
        utf8.major = 1;
        utf8.minor = 1;
        utf8.product = "製品";
        utf8.version = "é";
        utf8.core_version = "é";
        std::size_t size = 0;
        if (int failed = check(hello_response_payload_size(utf8, &size) ==
                                   WireFrameCode::ok && size == 53,
                               6)) return failed;
    }

    {
        auto bad_product_nul = std::vector<std::uint8_t>(tag_zero_golden.begin(),
                                                         tag_zero_golden.end());
        bad_product_nul[30] = 0;
        auto bad_product_utf8 = bad_product_nul;
        bad_product_utf8[30] = 0xff;
        auto bad_product_length = std::vector<std::uint8_t>(tag_zero_golden.begin(),
                                                            tag_zero_golden.end());
        bad_product_length[28] = 33;
        auto bad_version_length = std::vector<std::uint8_t>(tag_zero_golden.begin(),
                                                            tag_zero_golden.end());
        bad_version_length[33] = 65;
        auto bad_tag = std::vector<std::uint8_t>(tag_zero_golden.begin(),
                                                 tag_zero_golden.end());
        bad_tag[40] = 2;
        auto bad_core_length = std::vector<std::uint8_t>(tag_one_golden.begin(),
                                                         tag_one_golden.end());
        bad_core_length[35] = 65;
        auto extra = std::vector<std::uint8_t>(tag_zero_golden.begin(),
                                              tag_zero_golden.end());
        extra.push_back(0);
        auto short_payload = std::vector<std::uint8_t>(tag_zero_golden.begin(),
                                                       tag_zero_golden.end() - 1);
        const std::array<std::vector<std::uint8_t>, 8> invalid{
            bad_product_nul, bad_product_utf8, bad_product_length, bad_version_length,
            bad_tag, bad_core_length, extra, short_payload,
        };
        for (const auto &bytes : invalid) {
            HelloResponse decoded;
            if (int failed = check(decode_hello_response(
                                       dtv_bytes{bytes.data(), bytes.size()}, &decoded) ==
                                       WireFrameCode::protocol_error,
                                   7)) return failed;
        }
        HelloResponse decoded;
        if (int failed = check(decode_hello_response(dtv_bytes{nullptr, 41}, &decoded) ==
                                   WireFrameCode::invalid_argument &&
                               decode_hello_response(
                                   dtv_bytes{tag_zero_golden.data(), tag_zero_golden.size()},
                                   nullptr) == WireFrameCode::invalid_argument,
                               8)) return failed;
    }

    {
        std::array<HelloResponse, 4> invalid{};
        invalid[0].product = std::string(33, 'p');
        invalid[1].version = std::string(65, 'v');
        invalid[2].core_version = std::string(65, 'c');
        invalid[3].product = std::string("a\0b", 3);
        std::vector<std::uint8_t> bytes(dtv::internal::kHelloResponseMaxSize, 0xaa);
        for (const auto &value : invalid) {
            std::size_t size = 777;
            dtv_buffer output{bytes.data(), bytes.size(), 777};
            if (int failed = check(hello_response_payload_size(value, &size) ==
                                       WireFrameCode::invalid_argument && size == 777 &&
                                   encode_hello_response(value, &output) ==
                                       WireFrameCode::invalid_argument &&
                                   unchanged(bytes, output, 777),
                                   9)) return failed;
        }
        HelloResponse invalid_utf8;
        invalid_utf8.product = std::string("\xc0\x80", 2);
        std::size_t size = 777;
        dtv_buffer output{bytes.data(), bytes.size(), 777};
        if (int failed = check(hello_response_payload_size(invalid_utf8, &size) ==
                                   WireFrameCode::invalid_argument && size == 777 &&
                               encode_hello_response(invalid_utf8, &output) ==
                                   WireFrameCode::invalid_argument &&
                               unchanged(bytes, output, 777),
                               10)) return failed;
        dtv_buffer small{bytes.data(), 202, 777};
        HelloResponse maximum;
        maximum.product = std::string(32, 'p');
        maximum.version = std::string(64, 'v');
        maximum.core_version = std::string(64, 'c');
        if (int failed = check(encode_hello_response(maximum, &small) ==
                                   WireFrameCode::buffer_too_small &&
                               unchanged(bytes, small, 777),
                               11)) return failed;
        dtv_buffer invalid_output{nullptr, 203, 777};
        if (int failed = check(encode_hello_response(response, &invalid_output) ==
                                   WireFrameCode::invalid_argument &&
                               encode_hello_response(response, nullptr) ==
                                   WireFrameCode::invalid_argument,
                               12)) return failed;
        if (int failed = check(hello_response_payload_size(response, nullptr) ==
                                   WireFrameCode::invalid_argument,
                               13)) return failed;
    }

    {
        const ListRequest request{{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                   0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f}};
        const std::array<std::uint8_t, 16> golden{
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        };
        std::array<std::uint8_t, 16> encoded{};
        dtv_buffer output{encoded.data(), encoded.size(), 777};
        ListRequest decoded;
        if (int failed = check(dtv::internal::kListRequestSize == 16 &&
                                   encode_list_request(request, &output) ==
                                       WireFrameCode::ok && output.size == 16 &&
                                   encoded == golden &&
                                   decode_list_request(dtv_bytes{golden.data(), golden.size()},
                                                       &decoded) == WireFrameCode::ok &&
                                   decoded.epoch == request.epoch,
                               14)) return failed;
        auto changed = golden;
        if (int failed = check(decode_list_request(dtv_bytes{changed.data(), changed.size()},
                                                   &decoded) == WireFrameCode::ok,
                               15)) return failed;
        changed[0] = 0xff;
        if (int failed = check(decoded.epoch == request.epoch, 21)) return failed;
        std::array<std::uint8_t, 16> zero{};
        if (int failed = check(decode_list_request(dtv_bytes{zero.data(), zero.size()},
                                                   &decoded) == WireFrameCode::ok &&
                                   decoded.epoch == zero,
                               16)) return failed;
        if (int failed = check(decode_list_request(dtv_bytes{golden.data(), 15}, &decoded) ==
                                   WireFrameCode::protocol_error &&
                               decode_list_request(dtv_bytes{golden.data(), 17}, &decoded) ==
                                   WireFrameCode::protocol_error,
                               17)) return failed;
        if (int failed = check(decode_list_request(dtv_bytes{nullptr, 16}, &decoded) ==
                                   WireFrameCode::invalid_argument &&
                               decode_list_request(dtv_bytes{golden.data(), golden.size()},
                                                   nullptr) == WireFrameCode::invalid_argument,
                               18)) return failed;
        std::array<std::uint8_t, 16> unchanged_bytes{};
        unchanged_bytes.fill(0xaa);
        dtv_buffer small{unchanged_bytes.data(), 15, 777};
        if (int failed = check(encode_list_request(request, &small) ==
                                   WireFrameCode::buffer_too_small &&
                                   small.size == 777 &&
                                   std::all_of(unchanged_bytes.begin(), unchanged_bytes.end(),
                                               [](std::uint8_t byte) {
                                                   return byte == 0xaa;
                                               }),
                               19)) return failed;
        dtv_buffer invalid_output{nullptr, 16, 777};
        if (int failed = check(encode_list_request(request, &invalid_output) ==
                                   WireFrameCode::invalid_argument &&
                               encode_list_request(request, nullptr) ==
                                   WireFrameCode::invalid_argument,
                               20)) return failed;
    }

    return 0;
}
