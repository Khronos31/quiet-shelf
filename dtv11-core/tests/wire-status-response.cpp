/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <dtv/internal/wire-status-response.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

using namespace dtv::internal;

namespace {

const std::array<std::uint8_t, 59> global_unknown_golden{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00
};

const std::array<std::uint8_t, 63> global_partial_empty_golden{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x01,0x00,0x01,0x00,0x00,0x01,0x00,0x00,
    0x01,0x00,0x00
};

StatusResponse response;
StatusResponse decoded;
ListResponse canonical_list;
std::array<std::uint8_t, 58780> maximum_payload{};
std::array<std::uint8_t, 58780> roundtrip_payload{};

template <std::size_t N> void fill_text(WireText<N> *value)
{
    value->length = static_cast<std::uint16_t>(N);
    value->bytes.fill('x');
}

void fill_error(ListErrorRecord *value)
{
    value->code = 0xffffffff;
    fill_text(&value->operation);
    fill_text(&value->detail);
}

void fill_maximum_response()
{
    response = StatusResponse{};
    fill_text(&response.producer.product);
    fill_text(&response.producer.version);
    response.producer.core_version.present = true;
    fill_text(&response.producer.core_version.value);
    response.context.instance.present = true;
    fill_text(&response.context.instance.value);
    response.context.last_error.present = true;
    fill_error(&response.context.last_error.value);
    response.scope.kind = Scope::Kind::enclosure;
    response.enclosure.present = true;
    response.enclosure.value.model.present = true;
    fill_text(&response.enclosure.value.model.value.name);
    auto &identity = response.enclosure.value.identity;
    identity.serial_state = 1;
    identity.serial.present = true;
    fill_text(&identity.serial.value);
    identity.uniqueness = 0;
    response.enclosure.value.limits.max_concurrent_streams.present = true;
    response.enclosure.value.limits.max_concurrent_streams.value = 32;
    response.lnb_15v_enabled.present = true;
    response.card_inserted.present = true;
    response.topology.state = 2;
    response.topology.expected_function_count.present = true;
    response.topology.expected_function_count.value = 8;
    response.functions.present = true;
    response.functions.value.count = 8;
    for (std::size_t i = 0; i < response.functions.value.count; ++i) {
        auto &function = response.functions.value.values[i];
        function.id = static_cast<std::uint16_t>(i + 1);
        function.location.usb_path.present = true;
        fill_text(&function.location.usb_path.value);
        function.location.bus.present = true;
        function.location.address.present = true;
        function.serial_state = 1;
        function.serial.present = true;
        fill_text(&function.serial.value);
        function.model.present = true;
        fill_text(&function.model.value.name);
        function.last_error.present = true;
        fill_error(&function.last_error.value);
    }
    response.receivers.present = true;
    response.receivers.value.count = 32;
    for (std::size_t i = 0; i < response.receivers.value.count; ++i) {
        auto &receiver = response.receivers.value.values[i];
        receiver.descriptor.id = static_cast<std::uint16_t>(i + 1);
        receiver.descriptor.systems = 3;
        receiver.descriptor.binding.present = true;
        receiver.descriptor.binding.value.function_id = static_cast<std::uint16_t>((i % 8) + 1);
        receiver.descriptor.binding.value.local_id = static_cast<std::uint16_t>(i + 1);
        auto &capabilities = receiver.descriptor.capabilities;
        capabilities.ts_scope = 1;
        capabilities.max_pids.present = true;
        capabilities.max_pids.value = 8192;
        capabilities.controls = 3;
        capabilities.lna_defaults.count = 2;
        for (std::size_t i = 0; i < 2; ++i) {
            capabilities.lna_defaults.values[i].system = static_cast<std::uint8_t>(i + 1);
            capabilities.lna_defaults.values[i].enabled.present = true;
        }
        capabilities.lnb_values.count = 2;
        capabilities.lnb_values.values[0].value = 0;
        capabilities.lnb_values.values[1].value = 15;
        capabilities.measurement_mask = 7;
        receiver.present = true;
        receiver.state = 3;
        receiver.operation = 3;
        receiver.leased = true;
        receiver.tune_revision = 1;
        receiver.current_tune.present = true;
        receiver.current_tune.value.revision = 1;
        receiver.current_tune.value.system = 2;
        receiver.current_tune.value.frequency_hz = 1;
        receiver.current_tune.value.programmed_hz.present = true;
        receiver.current_tune.value.programmed_hz.value = 1;
        receiver.current_tune.value.selection = 2;
        receiver.current_tune.value.selection_value = 1;
        receiver.last_successful_tune = receiver.current_tune;
        receiver.stream.present = true;
        receiver.stream.value.id = 1;
        receiver.stream.value.tune_revision = 1;
        receiver.stream.value.phase = 4;
        receiver.last_stream.present = true;
        receiver.last_stream.value.id = 1;
        receiver.last_stream.value.tune_revision = 1;
        receiver.last_stream.value.reason = 1;
        receiver.last_stream.value.error.present = true;
        fill_error(&receiver.last_stream.value.error.value);
        receiver.last_stream.value.statistics.entries.count = 16;
        for (std::size_t i = 0; i < 16; ++i) {
            auto &entry = receiver.last_stream.value.statistics.entries.values[i];
            entry.id = static_cast<std::uint8_t>(i + 5);
            entry.counter.state = 2;
        }
        receiver.measurements.entries.count = 3;
        for (std::size_t i = 0; i < 3; ++i) {
            auto &entry = receiver.measurements.entries.values[i];
            entry.id = static_cast<std::uint8_t>(i + 1);
            entry.measurement.state = 2;
        }
        receiver.receiver_statistics.entries.count = 4;
        for (std::size_t i = 0; i < 4; ++i) {
            auto &entry = receiver.receiver_statistics.entries.values[i];
            entry.id = static_cast<std::uint8_t>(i + 1);
            entry.counter.state = 2;
        }
        receiver.stream_statistics.present = true;
        receiver.stream_statistics.value.entries.count = 16;
        for (std::size_t i = 0; i < 16; ++i) {
            auto &entry = receiver.stream_statistics.value.entries.values[i];
            entry.id = static_cast<std::uint8_t>(i + 5);
            entry.counter.state = 2;
        }
        receiver.last_error.present = true;
        fill_error(&receiver.last_error.value);
    }
    response.readers.present = true;
    response.readers.value.count = 8;
    for (std::size_t i = 0; i < response.readers.value.count; ++i) {
        auto &reader = response.readers.value.values[i];
        reader.descriptor.id = static_cast<std::uint16_t>(i + 1);
        reader.descriptor.binding.present = true;
        reader.descriptor.binding.value.function_id = static_cast<std::uint16_t>(i + 1);
        reader.descriptor.binding.value.local_id = static_cast<std::uint16_t>(i + 1);
        reader.present = true;
        reader.card.state = 2;
        reader.card.sampled_at_ns.present = true;
        reader.operation = 4;
        reader.handles_open = 64;
        reader.transaction_active = true;
        reader.last_error.present = true;
        fill_error(&reader.last_error.value);
    }
}

void check_golden(const std::uint8_t *golden, std::size_t length, std::uint8_t topology)
{
    response.scope.kind = Scope::Kind::enclosure;
    response.topology.state = topology;
    response.functions.present = true;
    canonical_list.topology.state = topology;
    if (topology == 1) {
        response.receivers.present = true;
        response.readers.present = true;
        canonical_list.receivers.present = true;
        canonical_list.readers.present = true;
    }

    std::size_t size = 0;
    assert(status_response_payload_size(response, &size) == WireFrameCode::ok);
    assert(size == length);
    std::array<std::uint8_t, 63> bytes{};
    dtv_buffer output{bytes.data(), bytes.size(), 0};
    assert(encode_status_response(response, &output) == WireFrameCode::ok);
    assert(output.size == length);
    assert(std::equal(bytes.begin(), bytes.begin() + length, golden));
    assert(decode_status_response({golden, length}, &decoded) == WireFrameCode::ok);
    std::array<std::uint8_t, 63> roundtrip{};
    dtv_buffer roundtrip_output{roundtrip.data(), roundtrip.size(), 0};
    assert(encode_status_response(decoded, &roundtrip_output) == WireFrameCode::ok);
    assert(roundtrip_output.size == length);
    assert(std::equal(roundtrip.begin(), roundtrip.begin() + length, golden));
    assert(validate_status_response_scope(decoded) == WireFrameCode::ok);
    assert(validate_status_response(decoded, canonical_list) == WireFrameCode::ok);
}

}

int main()
{
    check_golden(global_unknown_golden.data(), global_unknown_golden.size(), 0);
    check_golden(global_partial_empty_golden.data(), global_partial_empty_golden.size(), 1);

    fill_maximum_response();
    std::size_t size = 0;
    assert(status_response_payload_size(response, &size) == WireFrameCode::ok);
    assert(size == maximum_payload.size());
    dtv_buffer maximum_output{maximum_payload.data(), maximum_payload.size(), 0};
    assert(encode_status_response(response, &maximum_output) == WireFrameCode::ok);
    assert(maximum_output.size == size);
    assert(decode_status_response({maximum_payload.data(), size}, &decoded) == WireFrameCode::ok);
    dtv_buffer roundtrip_output{roundtrip_payload.data(), roundtrip_payload.size(), 0};
    assert(encode_status_response(decoded, &roundtrip_output) == WireFrameCode::ok);
    assert(roundtrip_output.size == size);
    assert(std::equal(maximum_payload.begin(), maximum_payload.begin() + size, roundtrip_payload.begin()));

    canonical_list = ListResponse{};
    canonical_list.topology = response.topology;
    canonical_list.enclosure = response.enclosure;
    canonical_list.functions.count = response.functions.value.count;
    for (std::size_t i = 0; i < canonical_list.functions.count; ++i)
        canonical_list.functions.values[i] = response.functions.value.values[i];
    canonical_list.receivers.present = true;
    canonical_list.receivers.value.count = response.receivers.value.count;
    for (std::size_t i = 0; i < canonical_list.receivers.value.count; ++i)
        canonical_list.receivers.value.values[i] = response.receivers.value.values[i].descriptor;
    canonical_list.readers.present = true;
    canonical_list.readers.value.count = response.readers.value.count;
    for (std::size_t i = 0; i < canonical_list.readers.value.count; ++i)
        canonical_list.readers.value.values[i] = response.readers.value.values[i].descriptor;
    assert(validate_status_response(response, canonical_list) == WireFrameCode::ok);
    canonical_list.epoch[0] = 1;
    assert(validate_status_response(response, canonical_list) == WireFrameCode::invalid_argument);
    canonical_list.epoch[0] = 0;

    response.receivers.value.values[0].descriptor.capabilities.measurement_mask = 0;
    assert(validate_status_response(response, canonical_list) == WireFrameCode::invalid_argument);
    response.receivers.value.values[0].descriptor.capabilities.measurement_mask = 7;
    response.enclosure.value.identity.uniqueness = 1;
    assert(validate_status_response_scope(response) == WireFrameCode::ok);
    assert(validate_status_response(response, canonical_list) == WireFrameCode::invalid_argument);
    response.enclosure.value.identity.uniqueness = 0;

    /* F1: unknown/present は uniqueness unknown、absent は not_applicable。 */
    auto set_identity = [&](std::uint8_t serial_state, bool present, std::uint8_t uniqueness) {
        auto &identity = response.enclosure.value.identity;
        identity.serial_state = serial_state;
        identity.serial.present = present;
        identity.uniqueness = uniqueness;
        canonical_list.enclosure.value.identity = identity;
    };
    set_identity(0, false, 0);
    assert(validate_status_response(response, canonical_list) == WireFrameCode::ok);
    set_identity(2, false, 3);
    assert(validate_status_response(response, canonical_list) == WireFrameCode::ok);
    set_identity(0, false, 3);
    assert(validate_status_response(response, canonical_list) == WireFrameCode::invalid_argument);
    set_identity(2, false, 0);
    assert(validate_status_response(response, canonical_list) == WireFrameCode::invalid_argument);
    set_identity(1, true, 0);

    /* F2/F3: 宣言済み measurement は unknown か valid。unsupported は拒否。 */
    auto &metric = response.receivers.value.values[0].measurements.entries.values[0].measurement;
    metric.state = 0;
    metric.reason = 0;
    assert(status_response_payload_size(response, &size) == WireFrameCode::invalid_argument);
    assert(validate_status_response_scope(response) == WireFrameCode::invalid_argument);
    assert(validate_status_response(response, canonical_list) == WireFrameCode::invalid_argument);
    metric.state = 1;
    metric.reason = 1;
    assert(status_response_payload_size(response, &size) == WireFrameCode::ok);
    assert(validate_status_response_scope(response) == WireFrameCode::ok);
    assert(validate_status_response(response, canonical_list) == WireFrameCode::ok);
    metric.state = 2;
    metric.reason = 0;

    response.receivers.value.values[0].measurements.entries.values[0].measurement.sampled_at_ns = 1;
    assert(validate_status_response(response, canonical_list) == WireFrameCode::invalid_argument);
    response.receivers.value.values[0].measurements.entries.values[0].measurement.sampled_at_ns = 0;
    response.receivers.value.values[0].stream_statistics.value.entries.values[0].counter.overflow = true;
    size = 31;
    assert(status_response_payload_size(response, &size) == WireFrameCode::invalid_argument);
    assert(size == 31);
    response.receivers.value.values[0].stream_statistics.value.entries.values[0].counter.overflow = false;

    response.scope.kind = Scope::Kind::stream;
    response.scope.id = 1;
    response.functions.present = false;
    response.readers.present = false;
    response.receivers.value.count = 1;
    assert(validate_status_response_scope(response) == WireFrameCode::ok);
    response.receivers.value.values[0].leased = false;
    assert(validate_status_response(response, canonical_list) == WireFrameCode::invalid_argument);
    response.receivers.value.values[0].leased = true;
    assert(validate_status_response(response, canonical_list) == WireFrameCode::ok);

    decoded.scope.id = 91;
    assert(decode_status_response({maximum_payload.data(), size - 1}, &decoded) ==
           WireFrameCode::protocol_error);
    assert(decoded.scope.id == 91);
    maximum_payload[size] = 0;
    assert(decode_status_response({maximum_payload.data(), size + 1}, &decoded) == WireFrameCode::protocol_error);
    assert(decoded.scope.id == 91);
    std::array<std::uint8_t, 4> guarded{{0xaa, 0xaa, 0xaa, 0xaa}};
    dtv_buffer small_output{guarded.data(), guarded.size(), 23};
    assert(encode_status_response(response, &small_output) == WireFrameCode::buffer_too_small);
    assert(small_output.size == 23 && guarded[0] == 0xaa);

    response.scope.kind = Scope::Kind::enclosure;
    response.scope.id = 1;
    size = 77;
    assert(status_response_payload_size(response, &size) == WireFrameCode::invalid_argument);
    assert(size == 77);
    small_output.size = 29;
    assert(encode_status_response(response, &small_output) == WireFrameCode::invalid_argument);
    assert(small_output.size == 29 && guarded[0] == 0xaa);
    return 0;
}
