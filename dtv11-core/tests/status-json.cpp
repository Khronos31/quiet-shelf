// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include <dtv/internal/status-json.hpp>

namespace {

using dtv::internal::StatusArray;
using dtv::internal::StatusContextView;
using dtv::internal::StatusCounterView;
using dtv::internal::StatusCountersView;
using dtv::internal::StatusEnclosureView;
using dtv::internal::StatusError;
using dtv::internal::StatusIdentity;
using dtv::internal::StatusJsonCode;
using dtv::internal::StatusJsonInput;
using dtv::internal::StatusListEnclosureView;
using dtv::internal::StatusListResponseView;
using dtv::internal::StatusMeasurementView;
using dtv::internal::StatusMeasurementsView;
using dtv::internal::StatusMetricView;
using dtv::internal::StatusOptional;
using dtv::internal::StatusProducerView;
using dtv::internal::StatusReceiverDescriptor;
using dtv::internal::StatusReceiverView;
using dtv::internal::StatusReaderDescriptor;
using dtv::internal::StatusReaderView;
using dtv::internal::StatusScopeView;
using dtv::internal::StatusSnapshotView;
using dtv::internal::StatusText;
using dtv::internal::StatusTopology;
using dtv::internal::StatusUsbFunctionView;
using dtv::internal::encode_status_json;
using dtv::internal::status_json_size;

int check(bool value, int code) { return value ? 0 : code; }

StatusText text(const char *value)
{
    return StatusText{value, std::strlen(value), true};
}

StatusText missing() { return StatusText{}; }

struct Storage {
    std::array<StatusMetricView, 3> metrics{};
    std::array<StatusCounterView, 4> counters{};
    std::array<StatusCounterView, 2> stream_counters{};
    StatusUsbFunctionView function{};
    StatusReceiverDescriptor receiver_descriptor{};
    StatusReceiverView receiver{};
    StatusReaderDescriptor reader_descriptor{};
    StatusReaderView reader{};
    StatusReceiverDescriptor list_receiver{};
    StatusReaderDescriptor list_reader{};
    StatusArray<StatusUsbFunctionView> status_functions{};
    StatusArray<StatusReceiverView> status_receivers{};
    StatusArray<StatusReaderView> status_readers{};
    StatusArray<StatusUsbFunctionView> list_functions{};
    StatusArray<StatusReceiverDescriptor> list_receivers{};
    StatusArray<StatusReaderDescriptor> list_readers{};
    StatusJsonInput input{};
};

void make_base(Storage &s)
{
    s = Storage{};
    s.metrics[0] = StatusMetricView{1, StatusMeasurementView{2, 0, true, 0, 0, 500}};
    s.metrics[1] = StatusMetricView{2, StatusMeasurementView{2, 0, false, -123, 0, 500}};
    s.counters[0] = StatusCounterView{1, StatusMeasurementView{2, 0, false, 0, 10, 500}, false};
    s.counters[1] = StatusCounterView{2, StatusMeasurementView{1, 1, false, 0, 0, 0}, false};
    s.stream_counters[0] =
        StatusCounterView{5, StatusMeasurementView{2, 0, false, 0, 10, 500}, false};
    s.stream_counters[1] =
        StatusCounterView{6, StatusMeasurementView{1, 1, false, 0, 0, 0}, false};

    s.function.id = 2;
    s.function.vid = 0x0511;
    s.function.pid = 0x084a;
    s.function.mode = 2;
    s.function.location.usb_path = text("1-2.3");
    s.function.location.bus = {true, 1};
    s.function.location.address = {true, 6};
    s.function.serial = text("usb-serial");
    s.function.model = text("PX-Q3U4");
    s.function.present = true;

    s.receiver_descriptor.receiver = 3;
    s.receiver_descriptor.systems = 3;
    s.receiver_descriptor.binding = {true, 2, 0};
    s.receiver_descriptor.capabilities.ts_scope = 0;
    s.receiver_descriptor.capabilities.controls = 0;
    s.receiver_descriptor.capabilities.measurement_mask = 3;

    s.receiver.descriptor = s.receiver_descriptor;
    s.receiver.present = true;
    s.receiver.state = 0;
    s.receiver.operation = 0;
    s.receiver.leased = false;
    s.receiver.tune_revision = 0;
    s.receiver.measurements =
        StatusMeasurementsView{StatusArray<StatusMetricView>{s.metrics.data(), 2, true}};
    s.receiver.receiver_statistics =
        StatusCountersView{StatusArray<StatusCounterView>{s.counters.data(), 2, true}};

    s.reader_descriptor.reader = 0;
    s.reader_descriptor.binding = {true, 2, 0};
    s.reader.descriptor = s.reader_descriptor;
    s.reader.present = true;
    s.reader.card.state = 2;
    s.reader.card.sampled_at_ns = {true, 500};
    s.reader.card.reason = 0;
    s.reader.operation = 0;

    s.list_receiver = s.receiver_descriptor;
    s.list_reader = s.reader_descriptor;
    s.status_functions = StatusArray<StatusUsbFunctionView>{&s.function, 1, true};
    s.status_receivers = StatusArray<StatusReceiverView>{&s.receiver, 1, true};
    s.status_readers = StatusArray<StatusReaderView>{&s.reader, 1, true};
    s.list_functions = StatusArray<StatusUsbFunctionView>{&s.function, 1, true};
    s.list_receivers = StatusArray<StatusReceiverDescriptor>{&s.list_receiver, 1, true};
    s.list_readers = StatusArray<StatusReaderDescriptor>{&s.list_reader, 1, true};

    std::array<std::uint8_t, 16> epoch{};
    for (std::size_t i = 0; i < epoch.size(); ++i) {
        epoch[i] = static_cast<std::uint8_t>(i);
    }
    s.input.producer = StatusProducerView{text("px4"), text("1.0.0"), text("1.0.0")};
    s.input.status.context = StatusContextView{epoch, text("default"), 1, StatusError{}};
    s.input.status.snapshot = StatusSnapshotView{100, 1000};
    s.input.status.scope = StatusScopeView{0, 0};
    s.input.status.enclosure.present = true;
    s.input.status.enclosure.id = 1;
    s.input.status.enclosure.model = text("PX-Q3U4");
    s.input.status.enclosure.identity = StatusIdentity{1, text("serial-value"), 0};
    s.input.status.enclosure.limits.max_concurrent_streams = {true, 1};
    s.input.status.lnb_15v_enabled = {true, false};
    s.input.status.card_inserted = {true, true};
    s.input.status.topology = StatusTopology{2, {true, 1}};
    s.input.status.functions = s.status_functions;
    s.input.status.receivers = s.status_receivers;
    s.input.status.readers = s.status_readers;
    s.input.canonical_list.epoch = epoch;
    s.input.canonical_list.topology = StatusTopology{2, {true, 1}};
    s.input.canonical_list.enclosure.present = true;
    s.input.canonical_list.enclosure.id = 1;
    s.input.canonical_list.enclosure.limits.max_concurrent_streams = {true, 1};
    s.input.canonical_list.functions = s.list_functions;
    s.input.canonical_list.receivers = s.list_receivers;
    s.input.canonical_list.readers = s.list_readers;
}

const char *kBaseJson =
    "{\"schema\":{\"name\":\"userland.status\",\"major\":1,\"minor\":0},"
    "\"producer\":{\"product\":\"px4\",\"version\":\"1.0.0\","
    "\"core_version\":\"1.0.0\"},"
    "\"context\":{\"epoch\":\"000102030405060708090a0b0c0d0e0f\","
    "\"instance\":\"default\",\"state\":\"ready\",\"last_error\":null},"
    "\"snapshot\":{\"sequence\":\"100\",\"observed_at_ns\":\"1000\"},"
    "\"scope\":{\"kind\":\"enclosure\",\"id\":null},"
    "\"enclosure\":{\"id\":1,\"model\":\"PX-Q3U4\",\"serial\":\"serial-value\","
    "\"lnb_15v_enabled\":false,\"card_inserted\":true,"
    "\"limits\":{\"max_concurrent_streams\":1}},"
    "\"topology\":{\"state\":\"complete\",\"expected_function_count\":1},"
    "\"usb_functions\":[{\"id\":2,\"usb\":\"0511:084a\",\"mode\":\"runtime\","
    "\"location\":{\"usb_path\":\"1-2.3\",\"bus\":1,\"address\":6},"
    "\"serial\":\"usb-serial\",\"model\":\"PX-Q3U4\",\"present\":true,"
    "\"last_error\":null}],"
    "\"receivers\":[{\"descriptor\":{\"receiver\":3,\"systems\":[\"ISDB-T\",\"ISDB-S\"],"
    "\"binding\":{\"function_id\":2,\"local_id\":0}},\"present\":true,"
    "\"state\":\"free\",\"operation\":\"idle\",\"leased\":false,"
    "\"tune_revision\":\"0\",\"current_tune\":null,\"last_successful_tune\":null,"
    "\"stream\":null,\"last_stream\":null,"
    "\"measurements\":{\"lock\":{\"value\":true,\"sampled_at_ns\":\"500\","
    "\"reason\":null},\"cnr_mdb\":{\"value\":-123,\"sampled_at_ns\":\"500\","
    "\"reason\":null}},"
    "\"statistics\":{\"receiver\":{\"captured_packets\":{\"value\":\"10\","
    "\"sampled_at_ns\":\"500\",\"reason\":null,\"overflow\":false},"
    "\"usb_errors\":{\"value\":null,\"sampled_at_ns\":null,"
    "\"reason\":\"not_sampled\",\"overflow\":null}},\"stream\":null},"
    "\"last_error\":null}],"
    "\"readers\":[{\"descriptor\":{\"reader\":0,\"binding\":{\"function_id\":2,"
    "\"local_id\":0}},\"present\":true,\"card\":{\"state\":\"present\","
    "\"sampled_at_ns\":\"500\",\"reason\":null},\"operation\":\"idle\","
    "\"handles_open\":0,\"transaction_active\":false,\"last_error\":null}]}";

bool encode_matches(const StatusJsonInput &input, const char *expected)
{
    std::array<std::uint8_t, 4096> buffer{};
    dtv_buffer output{buffer.data(), buffer.size(), 0};
    if (encode_status_json(&input, &output) != StatusJsonCode::ok) return false;
    const auto expected_size = std::strlen(expected);
    if (output.size != expected_size + 1) return false;
    if (std::memcmp(buffer.data(), expected, expected_size) != 0) return false;
    return buffer[expected_size] == '\n';
}

}

int main()
{
    Storage base;
    make_base(base);
    {
        std::uint64_t bytes = 0;
        if (int failed = check(status_json_size(&base.input, &bytes) == StatusJsonCode::ok &&
                                   bytes == std::strlen(kBaseJson) + 1,
                               1)) return failed;
        if (int failed = check(encode_matches(base.input, kBaseJson), 2)) return failed;
    }
    {
        const auto needed = static_cast<std::size_t>(std::strlen(kBaseJson) + 1);
        std::array<std::uint8_t, 4096> buffer{};
        std::fill(buffer.begin(), buffer.end(), 0xaa);
        dtv_buffer output{buffer.data(), needed - 1, 777};
        if (int failed = check(encode_status_json(&base.input, &output) ==
                                   StatusJsonCode::buffer_too_small &&
                               output.size == 777 &&
                               std::all_of(buffer.begin(), buffer.end(),
                                           [](std::uint8_t byte) { return byte == 0xaa; }),
                               3)) return failed;
        if (int failed = check(encode_status_json(nullptr, &output) ==
                                   StatusJsonCode::invalid_argument &&
                               encode_status_json(&base.input, nullptr) ==
                                   StatusJsonCode::invalid_argument &&
                               status_json_size(nullptr, nullptr) ==
                                   StatusJsonCode::invalid_argument,
                               4)) return failed;
        dtv_buffer invalid_output{nullptr, 100, 0};
        if (int failed = check(encode_status_json(&base.input, &invalid_output) ==
                                   StatusJsonCode::invalid_argument,
                               5)) return failed;
    }
    {
        Storage value;
        make_base(value);
        value.input.canonical_list.epoch[0] = 0x7f;
        std::uint64_t bytes = 0;
        if (int failed = check(status_json_size(&value.input, &bytes) ==
                                   StatusJsonCode::invalid_argument,
                               6)) return failed;
        make_base(value);
        value.input.status.context.state = 5;
        if (int failed = check(status_json_size(&value.input, &bytes) ==
                                   StatusJsonCode::invalid_argument,
                               7)) return failed;
        make_base(value);
        value.input.status.topology.state = 1;
        if (int failed = check(status_json_size(&value.input, &bytes) ==
                                   StatusJsonCode::invalid_argument,
                               8)) return failed;
        make_base(value);
        value.reader.card.sampled_at_ns.present = false;
        if (int failed = check(status_json_size(&value.input, &bytes) ==
                                   StatusJsonCode::invalid_argument,
                               9)) return failed;
        make_base(value);
        value.metrics[1].measurement.state = 0;
        if (int failed = check(status_json_size(&value.input, &bytes) ==
                                   StatusJsonCode::invalid_argument,
                               10)) return failed;
        make_base(value);
        value.counters[0].overflow = true;
        value.counters[0].measurement.counter_value = 1;
        if (int failed = check(status_json_size(&value.input, &bytes) ==
                                   StatusJsonCode::invalid_argument,
                               11)) return failed;
    }
    {
        Storage value;
        make_base(value);
        value.input.status.context.last_error =
            StatusError{true, 99, text("op"), text("detail")};
        if (int failed = check(encode_matches(
                                   value.input,
                                   R"({"schema":{"name":"userland.status","major":1,"minor":0},"producer":{"product":"px4","version":"1.0.0","core_version":"1.0.0"},"context":{"epoch":"000102030405060708090a0b0c0d0e0f","instance":"default","state":"ready","last_error":{"code":"UNKNOWN","numeric_code":99,"operation":"op","detail":"detail"}},"snapshot":{"sequence":"100","observed_at_ns":"1000"},"scope":{"kind":"enclosure","id":null},"enclosure":{"id":1,"model":"PX-Q3U4","serial":"serial-value","lnb_15v_enabled":false,"card_inserted":true,"limits":{"max_concurrent_streams":1}},"topology":{"state":"complete","expected_function_count":1},"usb_functions":[{"id":2,"usb":"0511:084a","mode":"runtime","location":{"usb_path":"1-2.3","bus":1,"address":6},"serial":"usb-serial","model":"PX-Q3U4","present":true,"last_error":null}],"receivers":[{"descriptor":{"receiver":3,"systems":["ISDB-T","ISDB-S"],"binding":{"function_id":2,"local_id":0}},"present":true,"state":"free","operation":"idle","leased":false,"tune_revision":"0","current_tune":null,"last_successful_tune":null,"stream":null,"last_stream":null,"measurements":{"lock":{"value":true,"sampled_at_ns":"500","reason":null},"cnr_mdb":{"value":-123,"sampled_at_ns":"500","reason":null}},"statistics":{"receiver":{"captured_packets":{"value":"10","sampled_at_ns":"500","reason":null,"overflow":false},"usb_errors":{"value":null,"sampled_at_ns":null,"reason":"not_sampled","overflow":null}},"stream":null},"last_error":null}],"readers":[{"descriptor":{"reader":0,"binding":{"function_id":2,"local_id":0}},"present":true,"card":{"state":"present","sampled_at_ns":"500","reason":null},"operation":"idle","handles_open":0,"transaction_active":false,"last_error":null}]})"),
                               12)) return failed;
        make_base(value);
        value.input.status.context.last_error =
            StatusError{true, 18, text("op"), text("")};
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_status_json(&value.input, &output) ==
                                   StatusJsonCode::ok &&
                               std::string(reinterpret_cast<char *>(buffer.data()),
                                           output.size)
                                       .find("\"code\":\"RESOURCE_LIMIT\"") !=
                                   std::string::npos &&
                               std::string(reinterpret_cast<char *>(buffer.data()),
                                           output.size)
                                       .find("numeric_code") == std::string::npos,
                               13)) return failed;
    }
    {
        Storage value;
        make_base(value);
        value.input.status.scope = StatusScopeView{1, 3};
        value.input.status.functions.present = false;
        value.input.status.readers.present = false;
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_status_json(&value.input, &output) ==
                                   StatusJsonCode::ok,
                               14)) return failed;
        const std::string encoded(reinterpret_cast<char *>(buffer.data()), output.size);
        if (int failed = check(encoded.find("\"scope\":{\"kind\":\"receiver\",\"id\":3}") !=
                                   std::string::npos &&
                               encoded.find("usb_functions") == std::string::npos &&
                               encoded.find("readers") == std::string::npos,
                               15)) return failed;

        make_base(value);
        value.input.status.scope = StatusScopeView{2, 0};
        value.input.status.functions.present = false;
        value.input.status.receivers.present = false;
        dtv_buffer reader_output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_status_json(&value.input, &reader_output) ==
                                   StatusJsonCode::ok &&
                               std::string(reinterpret_cast<char *>(buffer.data()),
                                           reader_output.size)
                                       .find("\"scope\":{\"kind\":\"reader\",\"id\":0}") !=
                                   std::string::npos,
                               16)) return failed;

        make_base(value);
        value.input.status.scope = StatusScopeView{3, 7};
        value.input.status.functions.present = false;
        value.input.status.readers.present = false;
        value.receiver.stream = {true, {7, 1, 3}};
        dtv_buffer stream_output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_status_json(&value.input, &stream_output) ==
                                   StatusJsonCode::ok &&
                               std::string(reinterpret_cast<char *>(buffer.data()),
                                           stream_output.size)
                                       .find("\"scope\":{\"kind\":\"stream\",\"id\":\"7\"}") !=
                                   std::string::npos &&
                               std::string(reinterpret_cast<char *>(buffer.data()),
                                           stream_output.size)
                                       .find("\"stream\":{\"id\":\"7\",\"tune_revision\":\"1\",\"phase\":\"active\"}") !=
                                   std::string::npos,
                               17)) return failed;
    }
    {
        Storage value;
        make_base(value);
        value.input.status.topology = StatusTopology{0, {false, 0}};
        value.input.canonical_list.topology = StatusTopology{0, {false, 0}};
        value.input.canonical_list.receivers.present = false;
        value.input.canonical_list.readers.present = false;
        value.input.status.receivers.present = false;
        value.input.status.readers.present = false;
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_status_json(&value.input, &output) ==
                                   StatusJsonCode::ok,
                               18)) return failed;
        const std::string encoded(reinterpret_cast<char *>(buffer.data()), output.size);
        if (int failed = check(encoded.find("\"topology\":{\"state\":\"unknown\","
                                            "\"expected_function_count\":null}") !=
                                   std::string::npos &&
                               encoded.find("\"receivers\"") == std::string::npos &&
                               encoded.find("\"readers\"") == std::string::npos,
                               19)) return failed;
    }
    {
        Storage value;
        make_base(value);
        value.receiver.stream = {true, {7, 1, 3}};
        value.receiver.stream_statistics =
            StatusOptional<StatusCountersView>{true,
                StatusCountersView{StatusArray<StatusCounterView>{
                    value.stream_counters.data(), 2, true}}};
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_status_json(&value.input, &output) ==
                                   StatusJsonCode::ok,
                               20)) return failed;
        const std::string encoded(reinterpret_cast<char *>(buffer.data()), output.size);
        if (int failed = check(encoded.find("\"transmitted_packets\":{\"value\":\"10\"") !=
                                   std::string::npos &&
                               encoded.find("\"queue_dropped_packets\":{\"value\":null") !=
                                   std::string::npos,
                               21)) return failed;
    }
    {
        Storage value;
        make_base(value);
        const char escaped[] = "a\"b\\c\n\x01";
        value.input.status.context.instance =
            StatusText{escaped, sizeof(escaped) - 1, true};
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_status_json(&value.input, &output) ==
                                   StatusJsonCode::ok,
                               22)) return failed;
        const std::string encoded(reinterpret_cast<char *>(buffer.data()), output.size);
        if (int failed = check(encoded.find("\"instance\":\"a\\\"b\\\\c\\n\\u0001\"") !=
                                   std::string::npos,
                               23)) return failed;
        const char invalid[] = {static_cast<char>(0xff)};
        value.input.status.context.instance = StatusText{invalid, 1, true};
        if (int failed = check(status_json_size(&value.input, &output.size) ==
                                   StatusJsonCode::invalid_argument,
                               24)) return failed;
    }
    {
        Storage value;
        make_base(value);
        value.receiver.last_stream = {true, {8, 1, 0, StatusError{},
                                              StatusCountersView{StatusArray<StatusCounterView>{
                                                  value.stream_counters.data(), 2, true}}}};
        value.receiver.leased = false;
        std::uint64_t bytes = 0;
        if (int failed = check(status_json_size(&value.input, &bytes) ==
                                   StatusJsonCode::invalid_argument,
                               25)) return failed;
        value.receiver.leased = true;
        if (int failed = check(status_json_size(&value.input, &bytes) ==
                                   StatusJsonCode::ok,
                               26)) return failed;
    }
    {
        Storage value;
        make_base(value);
        value.input.status.functions.present = true;
        value.input.status.functions.count = 0;
        value.input.status.functions.items = nullptr;
        value.input.status.receivers.count = 0;
        value.input.status.receivers.items = nullptr;
        value.input.status.readers.count = 0;
        value.input.status.readers.items = nullptr;
        value.input.canonical_list.functions.count = 0;
        value.input.canonical_list.functions.items = nullptr;
        value.input.canonical_list.receivers.count = 0;
        value.input.canonical_list.receivers.items = nullptr;
        value.input.canonical_list.readers.count = 0;
        value.input.canonical_list.readers.items = nullptr;
        value.input.canonical_list.enclosure.limits.max_concurrent_streams.present = false;
        value.input.status.enclosure.limits.max_concurrent_streams.present = false;
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_status_json(&value.input, &output) ==
                                   StatusJsonCode::ok,
                               27)) return failed;
        const std::string encoded(reinterpret_cast<char *>(buffer.data()), output.size);
        if (int failed = check(encoded.find("\"usb_functions\":[]") != std::string::npos &&
                                   encoded.find("\"receivers\":[]") != std::string::npos &&
                                   encoded.find("\"readers\":[]") != std::string::npos,
                               28)) return failed;
    }

    return 0;
}
