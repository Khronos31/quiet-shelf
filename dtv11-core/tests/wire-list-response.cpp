/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/internal/wire-list-response.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

using namespace dtv::internal;

int main()
{
    const std::array<std::uint8_t, 44> empty{};
    ListResponse value;
    assert(decode_list_response({empty.data(), empty.size()}, &value) == WireFrameCode::ok);
    assert(validate_list_response(value) == WireFrameCode::ok);
    std::size_t size = 0;
    assert(list_response_payload_size(value, &size) == WireFrameCode::ok && size == empty.size());
    std::array<std::uint8_t, 44> encoded{};
    dtv_buffer output{encoded.data(), encoded.size(), 99};
    assert(encode_list_response(value, &output) == WireFrameCode::ok);
    assert(output.size == empty.size() && encoded == empty);

    const std::array<std::uint8_t, 50> partial{
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        1,0,112,1,0,118,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,1,0,0
    };
    assert(decode_list_response({partial.data(), partial.size()}, &value) == WireFrameCode::ok);
    assert(validate_list_response(value) == WireFrameCode::ok);
    std::array<std::uint8_t, 50> encoded_partial{};
    output = {encoded_partial.data(), encoded_partial.size(), 0};
    assert(encode_list_response(value, &output) == WireFrameCode::ok);
    assert(encoded_partial == partial);

    std::array<std::uint8_t, 51> enclosure{};
    enclosure[39] = 1;
    enclosure[40] = 7;
    assert(decode_list_response({enclosure.data(), enclosure.size()}, &value) == WireFrameCode::ok);
    assert(validate_list_response(value) == WireFrameCode::ok);
    assert(list_response_payload_size(value, &size) == WireFrameCode::ok && size == enclosure.size());
    std::array<std::uint8_t, 51> encoded_enclosure{};
    output = {encoded_enclosure.data(), encoded_enclosure.size(), 0};
    assert(encode_list_response(value, &output) == WireFrameCode::ok);
    assert(encoded_enclosure == enclosure);
    std::array<std::uint8_t, 20 + 51> frame{};
    dtv_buffer frame_output{frame.data(), frame.size(), 0};
    assert(encode_frame({0x0010, 1, 1, 51}, {encoded_enclosure.data(), 51}, &frame_output) == WireFrameCode::ok);
    assert(frame_output.size == frame.size());

    std::array<std::uint8_t, 45> extra{};
    value.producer.product.length = 3;
    value.producer.product.bytes[0] = 'x';
    const auto before = value;
    assert(decode_list_response({extra.data(), extra.size()}, &value) == WireFrameCode::protocol_error);
    assert(value.producer.product.length == before.producer.product.length);
    assert(decode_list_response({empty.data(), empty.size() - 1}, &value) == WireFrameCode::protocol_error);
    auto malformed = empty;
    malformed[20] = 2;
    assert(decode_list_response({malformed.data(), malformed.size()}, &value) == WireFrameCode::protocol_error);
    malformed = empty;
    malformed[37] = 3;
    assert(decode_list_response({malformed.data(), malformed.size()}, &value) == WireFrameCode::invalid_argument);
    malformed = empty;
    malformed[16] = 1;
    malformed[18] = 0xff;
    assert(decode_list_response({malformed.data(), malformed.size()}, &value) == WireFrameCode::protocol_error);
    malformed[18] = 0;
    assert(decode_list_response({malformed.data(), malformed.size()}, &value) == WireFrameCode::protocol_error);
    malformed = empty;
    malformed[40] = 9;
    assert(decode_list_response({malformed.data(), malformed.size()}, &value) == WireFrameCode::protocol_error);
    value.topology.state = 0;
    value.receivers.present = true;
    value.readers.present = true;
    assert(validate_list_response(value) == WireFrameCode::invalid_argument);

    value = {};
    value.functions.count = 2;
    value.functions.values[0].id = 4;
    value.functions.values[1].id = 4;
    assert(validate_list_response(value) == WireFrameCode::invalid_argument);
    value = {};
    value.topology.state = 1;
    value.receivers.present = true;
    value.readers.present = true;
    value.receivers.value.count = 1;
    value.receivers.value.values[0].systems = 1;
    value.receivers.value.values[0].binding.present = true;
    value.receivers.value.values[0].binding.value.function_id = 7;
    assert(validate_list_response(value) == WireFrameCode::invalid_argument);
    value = {};
    value.topology.state = 1;
    value.receivers.present = true;
    value.readers.present = true;
    value.receivers.value.count = 1;
    value.receivers.value.values[0].systems = 1;
    value.receivers.value.values[0].capabilities.controls = 1;
    value.receivers.value.values[0].capabilities.lna_defaults.count = 1;
    value.receivers.value.values[0].capabilities.lna_defaults.values[0].system = 2;
    assert(validate_list_response(value) == WireFrameCode::invalid_argument);
    value.receivers.value.count = 0;
    value.enclosure.present = true;
    value.enclosure.value.limits.max_concurrent_streams.present = true;
    value.enclosure.value.limits.max_concurrent_streams.value = 1;
    assert(validate_list_response(value) == WireFrameCode::invalid_argument);

    value = {};
    value.functions.count = 1;
    value.functions.values[0].id = 0;
    value.functions.values[0].identification_reason = 2;
    value.functions.values[0].present = true;
    value.functions.values[0].last_error.present = true;
    value.functions.values[0].last_error.value.code = 0xf0000001;
    value.producer.product.length = 2;
    value.producer.product.bytes[0] = 0xc3;
    value.producer.product.bytes[1] = 0xa9;
    assert(list_response_payload_size(value, &size) == WireFrameCode::ok);
    std::vector<std::uint8_t> owned_payload(size);
    output = {owned_payload.data(), owned_payload.size(), 0};
    assert(encode_list_response(value, &output) == WireFrameCode::ok);
    ListResponse owned;
    assert(decode_list_response({owned_payload.data(), output.size}, &owned) == WireFrameCode::ok);
    owned_payload.clear();
    assert(owned.producer.product.length == 2 && owned.producer.product.bytes[0] == 0xc3);
    assert(owned.functions.values[0].id == 0 && owned.functions.values[0].last_error.value.code == 0xf0000001);

    value = {};
    std::array<std::uint8_t, 43> short_output{};
    short_output.fill(0xa5);
    output = {short_output.data(), short_output.size(), 17};
    assert(encode_list_response(value, &output) == WireFrameCode::buffer_too_small);
    assert(output.size == 17 && short_output[0] == 0xa5 && short_output.back() == 0xa5);

    value = {};
    value.functions.count = 1;
    value.functions.values[0].identification_reason = 0;
    assert(validate_list_response(value) == WireFrameCode::invalid_argument);
    std::array<std::uint8_t, 128> sentinel_bytes{};
    sentinel_bytes.fill(0xa5);
    output = {sentinel_bytes.data(), sentinel_bytes.size(), 17};
    assert(encode_list_response(value, &output) == WireFrameCode::invalid_argument);
    assert(output.size == 17 && sentinel_bytes[0] == 0xa5 && sentinel_bytes.back() == 0xa5);

    value.functions.values[0].identification_reason = 2;
    assert(validate_list_response(value) == WireFrameCode::ok);
    assert(list_response_payload_size(value, &size) == WireFrameCode::ok);
    std::vector<std::uint8_t> absent_model(size);
    output = {absent_model.data(), absent_model.size(), 0};
    assert(encode_list_response(value, &output) == WireFrameCode::ok);
    assert(absent_model[55] == 2);
    absent_model[55] = 0;
    ListResponse unchanged{};
    unchanged.producer.product.length = 4;
    unchanged.producer.product.bytes[0] = 'k';
    const auto original = unchanged;
    assert(decode_list_response({absent_model.data(), absent_model.size()}, &unchanged) == WireFrameCode::invalid_argument);
    assert(unchanged.producer.product.length == original.producer.product.length);
    assert(unchanged.producer.product.bytes == original.producer.product.bytes);

    value.functions.values[0].model.present = true;
    value.functions.values[0].identification_reason = 1;
    assert(validate_list_response(value) == WireFrameCode::ok);
    value.functions.values[0].identification_reason = 0;
    assert(validate_list_response(value) == WireFrameCode::ok);
    return 0;
}
