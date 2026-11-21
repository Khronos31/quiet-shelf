/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <dtv/internal/wire-status-request.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

using namespace dtv::internal;

int main()
{
    const std::array<std::uint8_t, 25> enclosure_golden{};
    StatusRequest request;
    std::size_t size = 99;
    assert(status_request_payload_size(request, &size) == WireFrameCode::ok);
    assert(size == enclosure_golden.size());
    std::array<std::uint8_t, 25> encoded{};
    dtv_buffer output{encoded.data(), encoded.size(), 77};
    assert(encode_status_request(request, &output) == WireFrameCode::ok);
    assert(output.size == encoded.size() && encoded == enclosure_golden);
    assert(decode_status_request({enclosure_golden.data(), enclosure_golden.size()}, &request) ==
           WireFrameCode::ok);
    assert(request.scope.kind == Scope::Kind::enclosure && request.scope.id == 0);
    assert(validate_scope(request.scope) == WireFrameCode::ok);

    std::array<std::uint8_t, 25> receiver_golden{};
    receiver_golden[16] = 1;
    receiver_golden[17] = 7;
    assert(decode_status_request({receiver_golden.data(), receiver_golden.size()}, &request) ==
           WireFrameCode::ok);
    assert(request.scope.kind == Scope::Kind::receiver && request.scope.id == 7);
    output = {encoded.data(), encoded.size(), 0};
    assert(encode_status_request(request, &output) == WireFrameCode::ok);
    assert(encoded == receiver_golden);

    request = {};
    request.scope = {Scope::Kind::receiver, 0};
    assert(status_request_payload_size(request, &size) == WireFrameCode::ok);
    request.scope = {Scope::Kind::receiver, 65535};
    assert(status_request_payload_size(request, &size) == WireFrameCode::ok);
    assert(validate_scope(request.scope) == WireFrameCode::ok);
    request.scope = {Scope::Kind::reader, 0};
    assert(status_request_payload_size(request, &size) == WireFrameCode::ok);
    request.scope = {Scope::Kind::reader, 65535};
    assert(status_request_payload_size(request, &size) == WireFrameCode::ok);
    assert(validate_scope(request.scope) == WireFrameCode::ok);
    request.scope = {Scope::Kind::stream, 0};
    assert(status_request_payload_size(request, &size) == WireFrameCode::ok);
    assert(validate_scope(request.scope) == WireFrameCode::invalid_argument);
    output = {encoded.data(), encoded.size(), 0};
    assert(encode_status_request(request, &output) == WireFrameCode::ok);
    assert(decode_status_request({encoded.data(), encoded.size()}, &request) == WireFrameCode::ok);
    assert(request.scope.id == 0 && validate_scope(request.scope) == WireFrameCode::invalid_argument);
    request.scope.id = 1;
    assert(validate_scope(request.scope) == WireFrameCode::ok);
    request.scope.id = std::numeric_limits<std::uint64_t>::max();
    assert(status_request_payload_size(request, &size) == WireFrameCode::ok);
    assert(validate_scope(request.scope) == WireFrameCode::ok);
    output = {encoded.data(), encoded.size(), 0};
    assert(encode_status_request(request, &output) == WireFrameCode::ok);
    assert(encoded[17] == 0xff && encoded[24] == 0xff);
    request.scope.id = 0x0102030405060708ULL;
    output = {encoded.data(), encoded.size(), 0};
    assert(encode_status_request(request, &output) == WireFrameCode::ok);
    assert(encoded[17] == 0x08 && encoded[18] == 0x07 && encoded[19] == 0x06 &&
           encoded[20] == 0x05 && encoded[21] == 0x04 && encoded[22] == 0x03 &&
           encoded[23] == 0x02 && encoded[24] == 0x01);
    StatusRequest le_decoded;
    assert(decode_status_request({encoded.data(), encoded.size()}, &le_decoded) ==
           WireFrameCode::ok);
    assert(le_decoded.scope.id == 0x0102030405060708ULL);

    request.scope = {Scope::Kind::enclosure, 1};
    size = 123;
    assert(status_request_payload_size(request, &size) == WireFrameCode::invalid_argument);
    assert(size == 123);
    request.scope = {Scope::Kind::receiver, 65536};
    assert(status_request_payload_size(request, &size) == WireFrameCode::invalid_argument);
    assert(validate_scope(request.scope) == WireFrameCode::invalid_argument);
    request.scope = {Scope::Kind::reader, 65536};
    assert(status_request_payload_size(request, &size) == WireFrameCode::invalid_argument);
    assert(validate_scope(request.scope) == WireFrameCode::invalid_argument);
    request.scope = {static_cast<Scope::Kind>(4), 0};
    assert(status_request_payload_size(request, &size) == WireFrameCode::invalid_argument);
    assert(size == 123);

    const auto unchanged = request;
    assert(decode_status_request({receiver_golden.data(), 24}, &request) ==
           WireFrameCode::protocol_error);
    assert(request.scope.kind == unchanged.scope.kind && request.scope.id == unchanged.scope.id);
    std::array<std::uint8_t, 26> extra{};
    assert(decode_status_request({extra.data(), extra.size()}, &request) ==
           WireFrameCode::protocol_error);
    auto bad_kind = receiver_golden;
    bad_kind[16] = 4;
    assert(decode_status_request({bad_kind.data(), bad_kind.size()}, &request) ==
           WireFrameCode::invalid_argument);
    assert(request.scope.kind == unchanged.scope.kind && request.scope.id == unchanged.scope.id);
    auto bad_enclosure = enclosure_golden;
    bad_enclosure[17] = 1;
    assert(decode_status_request({bad_enclosure.data(), bad_enclosure.size()}, &request) ==
           WireFrameCode::invalid_argument);
    assert(request.scope.kind == unchanged.scope.kind && request.scope.id == unchanged.scope.id);
    auto bad_receiver = receiver_golden;
    bad_receiver.fill(0);
    bad_receiver[16] = 1;
    bad_receiver[19] = 1;
    assert(decode_status_request({bad_receiver.data(), bad_receiver.size()}, &request) ==
           WireFrameCode::invalid_argument);
    assert(request.scope.kind == unchanged.scope.kind && request.scope.id == unchanged.scope.id);
    auto bad_reader = bad_receiver;
    bad_reader[16] = 2;
    assert(decode_status_request({bad_reader.data(), bad_reader.size()}, &request) ==
           WireFrameCode::invalid_argument);
    assert(request.scope.kind == unchanged.scope.kind && request.scope.id == unchanged.scope.id);

    request = {};
    std::array<std::uint8_t, 25> epoch_bytes{};
    for (std::size_t index = 0; index < epoch_bytes.size(); ++index) {
        epoch_bytes[index] = static_cast<std::uint8_t>(index + 1);
    }
    epoch_bytes[16] = 3;
    assert(decode_status_request({epoch_bytes.data(), epoch_bytes.size()}, &request) ==
           WireFrameCode::ok);
    for (std::size_t index = 0; index < request.epoch.size(); ++index) {
        assert(request.epoch[index] == index + 1);
    }
    assert(request.scope.kind == Scope::Kind::stream && request.scope.id != 0);

    request = {};
    std::array<std::uint8_t, 24> short_bytes{};
    short_bytes.fill(0xa5);
    output = {short_bytes.data(), short_bytes.size(), 19};
    assert(encode_status_request(request, &output) == WireFrameCode::buffer_too_small);
    assert(output.size == 19 && std::all_of(short_bytes.begin(), short_bytes.end(),
                                            [](auto byte) { return byte == 0xa5; }));
    request.scope = {static_cast<Scope::Kind>(4), 0};
    output = {encoded.data(), encoded.size(), 19};
    encoded.fill(0xa5);
    assert(encode_status_request(request, &output) == WireFrameCode::invalid_argument);
    assert(output.size == 19 && std::all_of(encoded.begin(), encoded.end(),
                                            [](auto byte) { return byte == 0xa5; }));
    assert(encode_status_request(StatusRequest{}, nullptr) == WireFrameCode::invalid_argument);
    dtv_buffer null_data{nullptr, 25, 19};
    assert(encode_status_request(StatusRequest{}, &null_data) == WireFrameCode::invalid_argument);
    dtv_buffer no_data{nullptr, 0, 19};
    assert(encode_status_request(StatusRequest{}, &no_data) == WireFrameCode::buffer_too_small);
    assert(no_data.size == 19);
    assert(status_request_payload_size(StatusRequest{}, nullptr) == WireFrameCode::invalid_argument);
    assert(decode_status_request({nullptr, 25}, &request) == WireFrameCode::invalid_argument);
    assert(decode_status_request({nullptr, 0}, &request) == WireFrameCode::protocol_error);
    assert(decode_status_request({enclosure_golden.data(), enclosure_golden.size()}, nullptr) ==
           WireFrameCode::invalid_argument);
    return 0;
}
