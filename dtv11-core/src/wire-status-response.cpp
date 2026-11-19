/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/internal/wire-status-response.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace dtv::internal {
namespace {

bool utf8(const std::uint8_t *text, std::size_t length)
{
    for (std::size_t index = 0; index < length;) {
        auto first = text[index];
        if (first == 0) return false;
        if (first < 0x80) { ++index; continue; }
        std::size_t count = 0;
        std::uint8_t low = 0x80, high = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) count = 2;
        else if (first >= 0xe0 && first <= 0xef) {
            count = 3;
            if (first == 0xe0) low = 0xa0;
            if (first == 0xed) high = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 4;
            if (first == 0xf0) low = 0x90;
            if (first == 0xf4) high = 0x8f;
        } else return false;
        if (count > length - index || text[index + 1] < low || text[index + 1] > high) return false;
        for (std::size_t offset = 2; offset < count; ++offset) {
            if (text[index + offset] < 0x80 || text[index + offset] > 0xbf) return false;
        }
        index += count;
    }
    return true;
}

template <std::size_t N> bool text_ok(const WireText<N> &value)
{
    return value.length <= N && utf8(value.bytes.data(), value.length);
}

struct Writer {
    std::uint8_t *data;
    std::size_t size = 0;

    void u8(std::uint8_t value) { if (data) data[size] = value; ++size; }
    void u16(std::uint16_t value)
    {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value)
    {
        for (unsigned index = 0; index < 4; ++index) u8(static_cast<std::uint8_t>(value >> (8 * index)));
    }
    void u64(std::uint64_t value)
    {
        for (unsigned index = 0; index < 8; ++index) u8(static_cast<std::uint8_t>(value >> (8 * index)));
    }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void bytes(const std::uint8_t *source, std::size_t length)
    {
        if (data && length) std::copy_n(source, length, data + size);
        size += length;
    }
    template <std::size_t N> void text(const WireText<N> &value)
    {
        u16(value.length);
        bytes(value.bytes.data(), value.length);
    }
    template <typename T, typename F> void optional(const WireOptional<T> &value, F write)
    {
        u8(value.present ? 1 : 0);
        if (value.present) write(value.value);
    }
};

struct Reader {
    dtv_bytes input;
    std::size_t size = 0;
    bool ok = true;

    std::size_t remaining() const { return size <= input.size ? input.size - size : 0; }
    std::uint8_t u8()
    {
        if (!ok || size >= input.size) { ok = false; return 0; }
        return input.data[size++];
    }
    bool boolean()
    {
        auto value = u8();
        if (value > 1) ok = false;
        return value == 1;
    }
    std::uint16_t u16()
    {
        auto low = u8();
        auto high = u8();
        return static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8));
    }
    std::uint32_t u32()
    {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(u8()) << (8 * index);
        return value;
    }
    std::uint64_t u64()
    {
        std::uint64_t value = 0;
        for (unsigned index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(u8()) << (8 * index);
        return value;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    template <std::size_t N> void text(WireText<N> *value)
    {
        auto length = u16();
        if (!ok || length > N || length > remaining()) { ok = false; return; }
        if (!utf8(input.data + size, length)) { ok = false; return; }
        value->length = length;
        if (length) std::copy_n(input.data + size, length, value->bytes.begin());
        size += length;
    }
    template <typename T, typename F> void optional(WireOptional<T> *value, F read)
    {
        auto tag = u8();
        if (!ok || tag > 1) { ok = false; return; }
        value->present = tag == 1;
        if (value->present) read(&value->value);
    }
};

template <typename W> void write_model(W &writer, const ListModel &value) { writer.text(value.name); }
template <typename W> void write_error(W &writer, const ListErrorRecord &value)
{
    writer.u32(value.code);
    writer.text(value.operation);
    writer.text(value.detail);
}
template <typename W> void write_location(W &writer, const ListLocation &value)
{
    writer.optional(value.usb_path, [&](const auto &text) { writer.text(text); });
    writer.optional(value.bus, [&](auto byte) { writer.u8(byte); });
    writer.optional(value.address, [&](auto byte) { writer.u8(byte); });
}
template <typename W> void write_function(W &writer, const ListUsbFunction &value)
{
    writer.u16(value.id); writer.u16(value.vid); writer.u16(value.pid); writer.u8(value.mode);
    write_location(writer, value.location);
    writer.u8(value.serial_state);
    writer.optional(value.serial, [&](const auto &text) { writer.text(text); });
    writer.optional(value.model, [&](const auto &model) { write_model(writer, model); });
    writer.u8(value.identification_reason); writer.u8(value.present ? 1 : 0);
    writer.optional(value.last_error, [&](const auto &error) { write_error(writer, error); });
}
template <typename W> void write_binding(W &writer, const ListBinding &value)
{
    writer.u16(value.function_id); writer.u16(value.local_id);
}
template <typename W> void write_capabilities(W &writer, const ListCapabilities &value)
{
    writer.u8(value.ts_scope);
    writer.optional(value.max_pids, [&](auto count) { writer.u16(count); });
    writer.u8(value.controls);
    if (value.controls & 1) {
        writer.u16(value.lna_defaults.count);
        for (std::size_t i = 0; i < value.lna_defaults.count; ++i) {
            const auto &item = value.lna_defaults.values[i];
            writer.u8(item.system);
            writer.optional(item.enabled, [&](bool enabled) { writer.u8(enabled ? 1 : 0); });
        }
    }
    if (value.controls & 2) {
        writer.u16(value.lnb_values.count);
        for (std::size_t i = 0; i < value.lnb_values.count; ++i) writer.u8(value.lnb_values.values[i].value);
    }
    writer.u8(value.measurement_mask);
}
template <typename W> void write_receiver_descriptor(W &writer, const ListReceiver &value)
{
    writer.u16(value.id); writer.u8(value.systems);
    writer.optional(value.binding, [&](const auto &binding) { write_binding(writer, binding); });
    write_capabilities(writer, value.capabilities);
}
template <typename W> void write_reader_descriptor(W &writer, const ListReader &value)
{
    writer.u16(value.id);
    writer.optional(value.binding, [&](const auto &binding) { write_binding(writer, binding); });
}
template <typename W> void write_enclosure(W &writer, const ListEnclosure &value)
{
    writer.u16(value.id);
    writer.optional(value.model, [&](const auto &model) { write_model(writer, model); });
    writer.u8(value.identity.serial_state);
    writer.optional(value.identity.serial, [&](const auto &text) { writer.text(text); });
    writer.u8(value.identity.uniqueness);
    writer.optional(value.limits.max_concurrent_streams, [&](auto count) { writer.u16(count); });
}
template <typename W> void write_tune(W &writer, const StatusTune &value)
{
    writer.u64(value.revision); writer.u8(value.system); writer.u64(value.frequency_hz);
    writer.optional(value.programmed_hz, [&](auto hz) { writer.u64(hz); });
    writer.u8(value.selection); writer.u16(value.selection_value); writer.u8(value.verification);
}
template <typename W> void write_stream(W &writer, const StatusStream &value)
{
    writer.u64(value.id); writer.u64(value.tune_revision); writer.u8(value.phase);
}
template <typename W> void write_counter_value(W &writer, const StatusCounterValue &value)
{
    writer.u8(value.state);
    if (value.state == 1) writer.u8(value.reason);
    else if (value.state == 2) {
        writer.u64(value.value); writer.u64(value.sampled_at_ns); writer.u8(value.overflow ? 1 : 0);
    }
}
template <typename W> void write_stream_counters(W &writer, const StatusStreamCounters &value)
{
    writer.u16(value.entries.count);
    for (std::size_t i = 0; i < value.entries.count; ++i) {
        writer.u8(value.entries.values[i].id); write_counter_value(writer, value.entries.values[i].counter);
    }
}
template <typename W> void write_stream_final(W &writer, const StatusStreamFinal &value)
{
    writer.u64(value.id); writer.u64(value.tune_revision); writer.u8(value.reason);
    writer.optional(value.error, [&](const auto &error) { write_error(writer, error); });
    write_stream_counters(writer, value.statistics);
}
template <typename W> void write_measurement(W &writer, std::uint8_t id, const StatusMeasurement &value)
{
    writer.u8(value.state);
    if (value.state == 1) writer.u8(value.reason);
    else if (value.state == 2) {
        if (id == 1) writer.u8(value.boolean_value ? 1 : 0);
        else writer.i32(value.integer_value);
        writer.u64(value.sampled_at_ns);
    }
}
template <typename W> void write_measurements(W &writer, const StatusMeasurements &value)
{
    writer.u16(value.entries.count);
    for (std::size_t i = 0; i < value.entries.count; ++i) {
        writer.u8(value.entries.values[i].id);
        write_measurement(writer, value.entries.values[i].id, value.entries.values[i].measurement);
    }
}
template <typename W> void write_receiver_counters(W &writer, const StatusReceiverCounters &value)
{
    writer.u16(value.entries.count);
    for (std::size_t i = 0; i < value.entries.count; ++i) {
        writer.u8(value.entries.values[i].id); write_counter_value(writer, value.entries.values[i].counter);
    }
}
template <typename W> void write_status_receiver(W &writer, const StatusReceiver &value)
{
    write_receiver_descriptor(writer, value.descriptor);
    writer.u8(value.present ? 1 : 0); writer.u8(value.state); writer.u8(value.operation);
    writer.u8(value.leased ? 1 : 0); writer.u64(value.tune_revision);
    writer.optional(value.current_tune, [&](const auto &tune) { write_tune(writer, tune); });
    writer.optional(value.last_successful_tune, [&](const auto &tune) { write_tune(writer, tune); });
    writer.optional(value.stream, [&](const auto &stream) { write_stream(writer, stream); });
    writer.optional(value.last_stream, [&](const auto &stream) { write_stream_final(writer, stream); });
    write_measurements(writer, value.measurements);
    write_receiver_counters(writer, value.receiver_statistics);
    writer.optional(value.stream_statistics, [&](const auto &counters) { write_stream_counters(writer, counters); });
    writer.optional(value.last_error, [&](const auto &error) { write_error(writer, error); });
}
template <typename W> void write_card(W &writer, const StatusCardObservation &value)
{
    writer.u8(value.state);
    writer.optional(value.sampled_at_ns, [&](auto time) { writer.u64(time); });
    writer.u8(value.reason);
}
template <typename W> void write_status_reader(W &writer, const StatusReader &value)
{
    write_reader_descriptor(writer, value.descriptor);
    writer.u8(value.present ? 1 : 0); write_card(writer, value.card); writer.u8(value.operation);
    writer.u16(value.handles_open); writer.u8(value.transaction_active ? 1 : 0);
    writer.optional(value.last_error, [&](const auto &error) { write_error(writer, error); });
}
template <typename W> void write_status_response(W &writer, const StatusResponse &value)
{
    writer.text(value.producer.product); writer.text(value.producer.version);
    writer.optional(value.producer.core_version, [&](const auto &text) { writer.text(text); });
    writer.bytes(value.context.epoch.data(), value.context.epoch.size());
    writer.optional(value.context.instance, [&](const auto &text) { writer.text(text); });
    writer.u8(value.context.state);
    writer.optional(value.context.last_error, [&](const auto &error) { write_error(writer, error); });
    writer.u64(value.snapshot.sequence); writer.u64(value.snapshot.observed_at_ns);
    writer.u8(static_cast<std::uint8_t>(value.scope.kind)); writer.u64(value.scope.id);
    writer.optional(value.enclosure, [&](const auto &enclosure) { write_enclosure(writer, enclosure); });
    writer.optional(value.lnb_15v_enabled, [&](bool enabled) { writer.u8(enabled ? 1 : 0); });
    writer.optional(value.card_inserted, [&](bool inserted) { writer.u8(inserted ? 1 : 0); });
    writer.u8(value.topology.state);
    writer.optional(value.topology.expected_function_count, [&](auto count) { writer.u16(count); });
    writer.optional(value.functions, [&](const auto &functions) {
        writer.u16(functions.count);
        for (std::size_t i = 0; i < functions.count; ++i) write_function(writer, functions.values[i]);
    });
    writer.optional(value.receivers, [&](const auto &receivers) {
        writer.u16(receivers.count);
        for (std::size_t i = 0; i < receivers.count; ++i) write_status_receiver(writer, receivers.values[i]);
    });
    writer.optional(value.readers, [&](const auto &readers) {
        writer.u16(readers.count);
        for (std::size_t i = 0; i < readers.count; ++i) write_status_reader(writer, readers.values[i]);
    });
}

bool valid_error(const ListErrorRecord &value)
{
    return value.code != 0 && text_ok(value.operation) && text_ok(value.detail);
}
bool valid_identity(const ListIdentity &value)
{
    if (value.serial_state > 2 || value.uniqueness > 3 ||
        (value.serial_state == 1) != value.serial.present ||
        (value.serial.present && (!text_ok(value.serial.value) || value.serial.value.length == 0)) ||
        (value.serial_state != 1 && value.serial.present)) return false;
    return true;
}
bool valid_enclosure(const ListEnclosure &value)
{
    return (!value.model.present || text_ok(value.model.value.name)) && valid_identity(value.identity) &&
        (!value.limits.max_concurrent_streams.present ||
         (value.limits.max_concurrent_streams.value != 0 && value.limits.max_concurrent_streams.value <= 32));
}
bool valid_function(const ListUsbFunction &value)
{
    return value.mode <= 2 && value.serial_state <= 4 && value.identification_reason <= 3 &&
        (value.model.present || value.identification_reason != 0) &&
        ((value.serial_state == 1) == value.serial.present) &&
        (!value.serial.present || (text_ok(value.serial.value) &&
         (value.serial_state != 1 || value.serial.value.length != 0))) &&
        (value.serial_state == 1 || !value.serial.present) &&
        (!value.location.usb_path.present || text_ok(value.location.usb_path.value)) &&
        (!value.model.present || text_ok(value.model.value.name)) &&
        (!value.last_error.present || valid_error(value.last_error.value));
}
bool valid_receiver_descriptor(const ListReceiver &value)
{
    const auto &caps = value.capabilities;
    if (value.systems == 0 || (value.systems & ~3) || caps.ts_scope > 1 || (caps.controls & ~3) ||
        (caps.measurement_mask & ~7) || (caps.max_pids.present &&
         (caps.max_pids.value == 0 || caps.max_pids.value > 8192))) return false;
    if ((caps.controls & 1) ? (caps.lna_defaults.count == 0 || caps.lna_defaults.count > 2) :
                              caps.lna_defaults.count != 0) return false;
    if ((caps.controls & 2) ? (caps.lnb_values.count == 0 || caps.lnb_values.count > 2) :
                              caps.lnb_values.count != 0) return false;
    std::uint8_t systems = 0;
    for (std::size_t i = 0; i < caps.lna_defaults.count; ++i) {
        const auto &item = caps.lna_defaults.values[i];
        if ((item.system != 1 && item.system != 2) || (systems & item.system) ||
            (i && caps.lna_defaults.values[i - 1].system >= item.system)) return false;
        systems |= item.system;
    }
    for (std::size_t i = 0; i < caps.lnb_values.count; ++i) {
        auto level = caps.lnb_values.values[i].value;
        if ((level != 0 && level != 15) || (i && caps.lnb_values.values[i - 1].value >= level)) return false;
    }
    if ((caps.controls & 1) && systems != value.systems) return false;
    return true;
}
bool valid_reader_descriptor(const ListReader &) { return true; }
bool valid_tune(const StatusTune &value)
{
    constexpr std::uint64_t max_frequency = (std::uint64_t{1} << 53) - 1;
    return value.revision != 0 && (value.system == 1 || value.system == 2) &&
        value.frequency_hz >= 1 && value.frequency_hz <= max_frequency &&
        (!value.programmed_hz.present ||
         (value.programmed_hz.value >= 1 && value.programmed_hz.value <= max_frequency)) &&
        value.selection <= 2 && value.verification <= 2 &&
        (value.selection != 0 || value.selection_value == 0) &&
        (value.system != 1 || value.selection == 0);
}
bool valid_stream(const StatusStream &value)
{
    return value.id != 0 && value.tune_revision != 0 && value.phase >= 1 && value.phase <= 4;
}
bool valid_counter(const StatusCounterValue &value, bool required)
{
    if (value.state > 2) return false;
    if (value.state == 0) return !required && value.reason == 0;
    if (value.state == 1) return value.reason >= 1 && value.reason <= 5;
    return value.reason == 0 && (!value.overflow || value.value == std::numeric_limits<std::uint64_t>::max());
}
bool valid_stream_counters(const StatusStreamCounters &value)
{
    if (value.entries.count < 2 || value.entries.count > 16) return false;
    bool have5 = false, have6 = false;
    for (std::size_t i = 0; i < value.entries.count; ++i) {
        const auto &entry = value.entries.values[i];
        if (entry.id < 5 || entry.id > 32 || (i && value.entries.values[i - 1].id >= entry.id)) return false;
        if (entry.id == 5) have5 = true;
        if (entry.id == 6) have6 = true;
        if (!valid_counter(entry.counter, entry.id == 5 || entry.id == 6)) return false;
    }
    return have5 && have6;
}
bool valid_receiver_counters(const StatusReceiverCounters &value)
{
    if (value.entries.count > 4) return false;
    for (std::size_t i = 0; i < value.entries.count; ++i) {
        const auto &entry = value.entries.values[i];
        if (entry.id < 1 || entry.id > 4 || (i && value.entries.values[i - 1].id >= entry.id) ||
            !valid_counter(entry.counter, true)) return false;
    }
    return true;
}
bool valid_final(const StatusStreamFinal &value)
{
    if (value.id == 0 || value.tune_revision == 0 || value.reason > 7 ||
        (value.reason != 0 && value.reason != 7 && !value.error.present) ||
        (value.error.present && !valid_error(value.error.value))) return false;
    return valid_stream_counters(value.statistics);
}
bool valid_measurements(const StatusMeasurements &value)
{
    if (value.entries.count > 3) return false;
    for (std::size_t i = 0; i < value.entries.count; ++i) {
        const auto &entry = value.entries.values[i];
        const auto &measurement = entry.measurement;
        if (entry.id < 1 || entry.id > 3 || (i && value.entries.values[i - 1].id >= entry.id) ||
            measurement.state > 2) return false;
        if (measurement.state == 0) return false;
        if (measurement.state == 1 && (measurement.reason < 1 || measurement.reason > 5)) return false;
        if (measurement.state == 2 && measurement.reason != 0) return false;
    }
    return true;
}
bool valid_status_receiver(const StatusReceiver &value)
{
    if (!valid_receiver_descriptor(value.descriptor) || value.state > 5 || value.operation > 3 ||
        (value.current_tune.present && !valid_tune(value.current_tune.value)) ||
        (value.last_successful_tune.present && !valid_tune(value.last_successful_tune.value)) ||
        (value.stream.present && !valid_stream(value.stream.value)) ||
        (value.last_stream.present && !valid_final(value.last_stream.value)) ||
        !valid_measurements(value.measurements) || !valid_receiver_counters(value.receiver_statistics) ||
        (value.stream_statistics.present && !valid_stream_counters(value.stream_statistics.value)) ||
        (value.last_error.present && !valid_error(value.last_error.value))) return false;
    return true;
}
bool valid_card(const StatusCardObservation &value)
{
    if (value.state > 3) return false;
    if (value.state == 0) return !value.sampled_at_ns.present &&
        (value.reason == 1 || value.reason == 2);
    if (value.state == 1 || value.state == 2) return value.sampled_at_ns.present && value.reason == 0;
    return !value.sampled_at_ns.present && (value.reason == 4 || value.reason == 5);
}
bool valid_status_reader(const StatusReader &value)
{
    return valid_reader_descriptor(value.descriptor) && valid_card(value.card) &&
        value.operation <= 4 && value.handles_open <= 64 &&
        (!value.last_error.present || valid_error(value.last_error.value));
}
bool valid_local(const StatusResponse &value)
{
    if (!text_ok(value.producer.product) || !text_ok(value.producer.version) ||
        (value.producer.core_version.present && !text_ok(value.producer.core_version.value)) ||
        (value.context.instance.present && !text_ok(value.context.instance.value)) ||
        value.context.state > 4 || (value.context.last_error.present && !valid_error(value.context.last_error.value)) ||
        value.topology.state > 2 || (value.topology.expected_function_count.present &&
          (value.topology.expected_function_count.value == 0 || value.topology.expected_function_count.value > 8)) ||
        (value.enclosure.present && !valid_enclosure(value.enclosure.value))) return false;
    StatusRequest scope_value;
    scope_value.scope = value.scope;
    std::size_t ignored = 0;
    if (status_request_payload_size(scope_value, &ignored) != WireFrameCode::ok) return false;
    if (value.functions.present) {
        if (value.functions.value.count > 8) return false;
        for (std::size_t i = 0; i < value.functions.value.count; ++i) {
            if (!valid_function(value.functions.value.values[i])) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (value.functions.value.values[i].id == value.functions.value.values[j].id) return false;
        }
    }
    if (value.receivers.present) {
        if (value.receivers.value.count > 32) return false;
        for (std::size_t i = 0; i < value.receivers.value.count; ++i) {
            if (!valid_status_receiver(value.receivers.value.values[i])) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (value.receivers.value.values[i].descriptor.id == value.receivers.value.values[j].descriptor.id) return false;
        }
    }
    if (value.readers.present) {
        if (value.readers.value.count > 8) return false;
        for (std::size_t i = 0; i < value.readers.value.count; ++i) {
            if (!valid_status_reader(value.readers.value.values[i])) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (value.readers.value.values[i].descriptor.id == value.readers.value.values[j].descriptor.id) return false;
        }
    }
    return true;
}

void read_error(Reader &reader, ListErrorRecord *value)
{
    value->code = reader.u32(); reader.text(&value->operation); reader.text(&value->detail);
}
void read_location(Reader &reader, ListLocation *value)
{
    reader.optional(&value->usb_path, [&](auto *text) { reader.text(text); });
    reader.optional(&value->bus, [&](auto *byte) { *byte = reader.u8(); });
    reader.optional(&value->address, [&](auto *byte) { *byte = reader.u8(); });
}
void read_function(Reader &reader, ListUsbFunction *value)
{
    value->id = reader.u16(); value->vid = reader.u16(); value->pid = reader.u16(); value->mode = reader.u8();
    read_location(reader, &value->location);
    value->serial_state = reader.u8();
    reader.optional(&value->serial, [&](auto *text) { reader.text(text); });
    reader.optional(&value->model, [&](auto *model) { reader.text(&model->name); });
    value->identification_reason = reader.u8(); value->present = reader.boolean();
    reader.optional(&value->last_error, [&](auto *error) { read_error(reader, error); });
}
void read_binding(Reader &reader, ListBinding *value)
{
    value->function_id = reader.u16(); value->local_id = reader.u16();
}
void read_capabilities(Reader &reader, ListCapabilities *value)
{
    value->ts_scope = reader.u8();
    reader.optional(&value->max_pids, [&](auto *count) { *count = reader.u16(); });
    value->controls = reader.u8();
    if (value->controls & 1) {
        value->lna_defaults.count = reader.u16();
        if (value->lna_defaults.count > 2) { reader.ok = false; return; }
        for (std::size_t i = 0; i < value->lna_defaults.count; ++i) {
            auto &item = value->lna_defaults.values[i]; item.system = reader.u8();
            reader.optional(&item.enabled, [&](auto *enabled) { *enabled = reader.boolean(); });
        }
    }
    if (value->controls & 2) {
        value->lnb_values.count = reader.u16();
        if (value->lnb_values.count > 2) { reader.ok = false; return; }
        for (std::size_t i = 0; i < value->lnb_values.count; ++i) value->lnb_values.values[i].value = reader.u8();
    }
    value->measurement_mask = reader.u8();
}
void read_receiver_descriptor(Reader &reader, ListReceiver *value)
{
    value->id = reader.u16(); value->systems = reader.u8();
    reader.optional(&value->binding, [&](auto *binding) { read_binding(reader, binding); });
    read_capabilities(reader, &value->capabilities);
}
void read_reader_descriptor(Reader &reader, ListReader *value)
{
    value->id = reader.u16(); reader.optional(&value->binding, [&](auto *binding) { read_binding(reader, binding); });
}
void read_enclosure(Reader &reader, ListEnclosure *value)
{
    value->id = reader.u16();
    reader.optional(&value->model, [&](auto *model) { reader.text(&model->name); });
    value->identity.serial_state = reader.u8();
    reader.optional(&value->identity.serial, [&](auto *text) { reader.text(text); });
    value->identity.uniqueness = reader.u8();
    reader.optional(&value->limits.max_concurrent_streams, [&](auto *count) { *count = reader.u16(); });
}
void read_tune(Reader &reader, StatusTune *value)
{
    value->revision = reader.u64(); value->system = reader.u8(); value->frequency_hz = reader.u64();
    reader.optional(&value->programmed_hz, [&](auto *hz) { *hz = reader.u64(); });
    value->selection = reader.u8(); value->selection_value = reader.u16(); value->verification = reader.u8();
}
void read_stream(Reader &reader, StatusStream *value)
{
    value->id = reader.u64(); value->tune_revision = reader.u64(); value->phase = reader.u8();
}
void read_counter_value(Reader &reader, StatusCounterValue *value)
{
    value->state = reader.u8();
    if (value->state == 1) value->reason = reader.u8();
    else if (value->state == 2) {
        value->value = reader.u64(); value->sampled_at_ns = reader.u64(); value->overflow = reader.boolean();
    }
}
void read_stream_counters(Reader &reader, StatusStreamCounters *value)
{
    value->entries.count = reader.u16();
    if (value->entries.count > 16) { reader.ok = false; return; }
    for (std::size_t i = 0; i < value->entries.count; ++i) {
        value->entries.values[i].id = reader.u8(); read_counter_value(reader, &value->entries.values[i].counter);
    }
}
void read_stream_final(Reader &reader, StatusStreamFinal *value)
{
    value->id = reader.u64(); value->tune_revision = reader.u64(); value->reason = reader.u8();
    reader.optional(&value->error, [&](auto *error) { read_error(reader, error); });
    read_stream_counters(reader, &value->statistics);
}
void read_measurement(Reader &reader, std::uint8_t id, StatusMeasurement *value)
{
    value->state = reader.u8();
    if (value->state == 1) value->reason = reader.u8();
    else if (value->state == 2) {
        if (id == 1) value->boolean_value = reader.boolean(); else value->integer_value = reader.i32();
        value->sampled_at_ns = reader.u64();
    }
}
void read_measurements(Reader &reader, StatusMeasurements *value)
{
    value->entries.count = reader.u16();
    if (value->entries.count > 3) { reader.ok = false; return; }
    for (std::size_t i = 0; i < value->entries.count; ++i) {
        auto &item = value->entries.values[i]; item.id = reader.u8(); read_measurement(reader, item.id, &item.measurement);
    }
}
void read_receiver_counters(Reader &reader, StatusReceiverCounters *value)
{
    value->entries.count = reader.u16();
    if (value->entries.count > 4) { reader.ok = false; return; }
    for (std::size_t i = 0; i < value->entries.count; ++i) {
        value->entries.values[i].id = reader.u8(); read_counter_value(reader, &value->entries.values[i].counter);
    }
}
void read_status_receiver(Reader &reader, StatusReceiver *value)
{
    read_receiver_descriptor(reader, &value->descriptor);
    value->present = reader.boolean(); value->state = reader.u8(); value->operation = reader.u8();
    value->leased = reader.boolean(); value->tune_revision = reader.u64();
    reader.optional(&value->current_tune, [&](auto *tune) { read_tune(reader, tune); });
    reader.optional(&value->last_successful_tune, [&](auto *tune) { read_tune(reader, tune); });
    reader.optional(&value->stream, [&](auto *stream) { read_stream(reader, stream); });
    reader.optional(&value->last_stream, [&](auto *stream) { read_stream_final(reader, stream); });
    read_measurements(reader, &value->measurements);
    read_receiver_counters(reader, &value->receiver_statistics);
    reader.optional(&value->stream_statistics, [&](auto *counters) { read_stream_counters(reader, counters); });
    reader.optional(&value->last_error, [&](auto *error) { read_error(reader, error); });
}
void read_card(Reader &reader, StatusCardObservation *value)
{
    value->state = reader.u8(); reader.optional(&value->sampled_at_ns, [&](auto *time) { *time = reader.u64(); });
    value->reason = reader.u8();
}
void read_status_reader(Reader &reader, StatusReader *value)
{
    read_reader_descriptor(reader, &value->descriptor);
    value->present = reader.boolean(); read_card(reader, &value->card); value->operation = reader.u8();
    value->handles_open = reader.u16(); value->transaction_active = reader.boolean();
    reader.optional(&value->last_error, [&](auto *error) { read_error(reader, error); });
}

bool valid_header(const ListProducer &producer, const StatusContext &context, const Scope &scope,
                  const WireOptional<ListEnclosure> &enclosure, const ListTopology &topology)
{
    if (!text_ok(producer.product) || !text_ok(producer.version) ||
        (producer.core_version.present && !text_ok(producer.core_version.value)) ||
        (context.instance.present && !text_ok(context.instance.value)) || context.state > 4 ||
        (context.last_error.present && !valid_error(context.last_error.value)) || topology.state > 2 ||
        (topology.expected_function_count.present &&
         (topology.expected_function_count.value == 0 || topology.expected_function_count.value > 8)) ||
        (enclosure.present && !valid_enclosure(enclosure.value))) return false;
    StatusRequest request;
    request.scope = scope;
    std::size_t ignored = 0;
    return status_request_payload_size(request, &ignored) == WireFrameCode::ok;
}

WireFrameCode parse_status_response(dtv_bytes payload, StatusResponse *output)
{
    if (payload.size < 59 || payload.size > kWireMaxControlPayload) return WireFrameCode::protocol_error;
    Reader reader{payload};
    ListProducer producer;
    StatusContext context;
    ListSnapshot snapshot;
    Scope scope;
    WireOptional<ListEnclosure> enclosure;
    WireOptional<bool> lnb;
    WireOptional<bool> card;
    ListTopology topology;
    bool functions_present = false, receivers_present = false, readers_present = false;
    std::uint16_t function_count = 0, receiver_count = 0, reader_count = 0;
    std::uint16_t first_receiver_id = 0, first_reader_id = 0;
    bool stream_found = false;
    std::array<std::uint16_t, 8> function_ids{};
    std::array<std::uint16_t, 32> receiver_ids{};
    std::array<std::uint16_t, 8> reader_ids{};

    reader.text(&producer.product); reader.text(&producer.version);
    reader.optional(&producer.core_version, [&](auto *text) { reader.text(text); });
    for (auto &byte : context.epoch) byte = reader.u8();
    reader.optional(&context.instance, [&](auto *text) { reader.text(text); });
    context.state = reader.u8();
    reader.optional(&context.last_error, [&](auto *error) { read_error(reader, error); });
    snapshot.sequence = reader.u64(); snapshot.observed_at_ns = reader.u64();
    scope.kind = static_cast<Scope::Kind>(reader.u8()); scope.id = reader.u64();
    reader.optional(&enclosure, [&](auto *value) { read_enclosure(reader, value); });
    reader.optional(&lnb, [&](auto *value) { *value = reader.boolean(); });
    reader.optional(&card, [&](auto *value) { *value = reader.boolean(); });
    topology.state = reader.u8();
    reader.optional(&topology.expected_function_count, [&](auto *value) { *value = reader.u16(); });
    auto tag = reader.u8();
    if (tag > 1) reader.ok = false;
    functions_present = tag == 1;
    if (functions_present && reader.ok) {
        function_count = reader.u16();
        if (function_count > 8) reader.ok = false;
        if (output) { output->functions.present = true; output->functions.value.count = function_count; }
        for (std::size_t i = 0; reader.ok && i < function_count; ++i) {
            ListUsbFunction temporary;
            auto *item = output ? &output->functions.value.values[i] : &temporary;
            read_function(reader, item);
            if (reader.ok && !valid_function(*item)) return WireFrameCode::invalid_argument;
            for (std::size_t j = 0; j < i; ++j) {
                auto prior = output ? output->functions.value.values[j].id : function_ids[j];
                if (item->id == prior) return WireFrameCode::invalid_argument;
            }
            function_ids[i] = item->id;
        }
    }
    tag = reader.u8();
    if (tag > 1) reader.ok = false;
    receivers_present = tag == 1;
    if (receivers_present && reader.ok) {
        receiver_count = reader.u16();
        if (receiver_count > 32) reader.ok = false;
        if (output) { output->receivers.present = true; output->receivers.value.count = receiver_count; }
        for (std::size_t i = 0; reader.ok && i < receiver_count; ++i) {
            StatusReceiver temporary;
            auto *item = output ? &output->receivers.value.values[i] : &temporary;
            read_status_receiver(reader, item);
            if (reader.ok && !valid_status_receiver(*item)) return WireFrameCode::invalid_argument;
            auto id = item->descriptor.id;
            if (i == 0) first_receiver_id = id;
            for (std::size_t j = 0; j < i; ++j) {
                auto prior = output ? output->receivers.value.values[j].descriptor.id : receiver_ids[j];
                if (id == prior) return WireFrameCode::invalid_argument;
            }
            receiver_ids[i] = id;
            stream_found = stream_found || (item->stream.present && item->stream.value.id == scope.id) ||
                (item->last_stream.present && item->last_stream.value.id == scope.id);
        }
    }
    tag = reader.u8();
    if (tag > 1) reader.ok = false;
    readers_present = tag == 1;
    if (readers_present && reader.ok) {
        reader_count = reader.u16();
        if (reader_count > 8) reader.ok = false;
        if (output) { output->readers.present = true; output->readers.value.count = reader_count; }
        for (std::size_t i = 0; reader.ok && i < reader_count; ++i) {
            StatusReader temporary;
            auto *item = output ? &output->readers.value.values[i] : &temporary;
            read_status_reader(reader, item);
            if (reader.ok && !valid_status_reader(*item)) return WireFrameCode::invalid_argument;
            auto id = item->descriptor.id;
            if (i == 0) first_reader_id = id;
            for (std::size_t j = 0; j < i; ++j) {
                auto prior = output ? output->readers.value.values[j].descriptor.id : reader_ids[j];
                if (id == prior) return WireFrameCode::invalid_argument;
            }
            reader_ids[i] = id;
        }
    }
    if (!reader.ok || reader.size != payload.size) return WireFrameCode::protocol_error;
    if (!valid_header(producer, context, scope, enclosure, topology)) return WireFrameCode::invalid_argument;
    if (scope.kind == Scope::Kind::enclosure) {
        if (!functions_present || (topology.state == 0 ? (receivers_present || readers_present) :
            (!receivers_present || !readers_present))) return WireFrameCode::invalid_argument;
    } else if (scope.kind == Scope::Kind::receiver) {
        if (functions_present || readers_present || !receivers_present || receiver_count != 1 ||
            first_receiver_id != scope.id) return WireFrameCode::invalid_argument;
    } else if (scope.kind == Scope::Kind::reader) {
        if (functions_present || receivers_present || !readers_present || reader_count != 1 ||
            first_reader_id != scope.id) return WireFrameCode::invalid_argument;
    } else if (scope.kind == Scope::Kind::stream) {
        if (functions_present || readers_present || !receivers_present || receiver_count != 1 || !stream_found)
            return WireFrameCode::invalid_argument;
    } else return WireFrameCode::invalid_argument;
    if (output) {
        output->producer = producer; output->context = context; output->snapshot = snapshot; output->scope = scope;
        output->enclosure.present = enclosure.present;
        if (enclosure.present) output->enclosure.value = enclosure.value;
        output->lnb_15v_enabled.present = lnb.present;
        if (lnb.present) output->lnb_15v_enabled.value = lnb.value;
        output->card_inserted.present = card.present;
        if (card.present) output->card_inserted.value = card.value;
        output->topology = topology;
        output->functions.present = functions_present;
        output->receivers.present = receivers_present;
        output->readers.present = readers_present;
    }
    return WireFrameCode::ok;
}

bool equal_binding(const WireOptional<ListBinding> &left, const WireOptional<ListBinding> &right)
{
    return left.present == right.present && (!left.present ||
        (left.value.function_id == right.value.function_id && left.value.local_id == right.value.local_id));
}
bool equal_capabilities(const ListCapabilities &left, const ListCapabilities &right)
{
    if (left.ts_scope != right.ts_scope || left.max_pids.present != right.max_pids.present ||
        (left.max_pids.present && left.max_pids.value != right.max_pids.value) ||
        left.controls != right.controls || left.measurement_mask != right.measurement_mask ||
        left.lna_defaults.count != right.lna_defaults.count || left.lnb_values.count != right.lnb_values.count) return false;
    for (std::size_t i = 0; i < left.lna_defaults.count; ++i) {
        const auto &a=left.lna_defaults.values[i]; const auto &b=right.lna_defaults.values[i];
        if (a.system != b.system || a.enabled.present != b.enabled.present ||
            (a.enabled.present && a.enabled.value != b.enabled.value)) return false;
    }
    for (std::size_t i = 0; i < left.lnb_values.count; ++i)
        if (left.lnb_values.values[i].value != right.lnb_values.values[i].value) return false;
    return true;
}
bool equal_receiver_descriptor(const ListReceiver &left, const ListReceiver &right)
{
    return left.id == right.id && left.systems == right.systems && equal_binding(left.binding, right.binding) &&
        equal_capabilities(left.capabilities, right.capabilities);
}
bool equal_reader_descriptor(const ListReader &left, const ListReader &right)
{
    return left.id == right.id && equal_binding(left.binding, right.binding);
}
template <std::size_t N> bool equal_text(const WireText<N> &left, const WireText<N> &right)
{
    return left.length == right.length && std::equal(left.bytes.begin(), left.bytes.begin() + left.length,
                                                       right.bytes.begin());
}
bool equal_enclosure(const ListEnclosure &left, const ListEnclosure &right)
{
    return left.id == right.id && left.model.present == right.model.present &&
        (!left.model.present || equal_text(left.model.value.name, right.model.value.name)) &&
        left.identity.serial_state == right.identity.serial_state &&
        left.identity.serial.present == right.identity.serial.present &&
        (!left.identity.serial.present || equal_text(left.identity.serial.value, right.identity.serial.value)) &&
        left.limits.max_concurrent_streams.present == right.limits.max_concurrent_streams.present &&
        (!left.limits.max_concurrent_streams.present || left.limits.max_concurrent_streams.value ==
         right.limits.max_concurrent_streams.value);
}
const ListReceiver *find_receiver(const ListResponse &list, std::uint16_t id)
{
    if (!list.receivers.present) return nullptr;
    for (std::size_t i=0;i<list.receivers.value.count;++i)
        if (list.receivers.value.values[i].id==id) return &list.receivers.value.values[i];
    return nullptr;
}
const ListReader *find_reader(const ListResponse &list, std::uint16_t id)
{
    if (!list.readers.present) return nullptr;
    for (std::size_t i=0;i<list.readers.value.count;++i)
        if (list.readers.value.values[i].id==id) return &list.readers.value.values[i];
    return nullptr;
}
const ListUsbFunction *find_function(const ListResponse &list, std::uint16_t id)
{
    for (std::size_t i=0;i<list.functions.count;++i)
        if (list.functions.values[i].id==id) return &list.functions.values[i];
    return nullptr;
}
bool sample_times_ok(const StatusResponse &value)
{
    auto limit = value.snapshot.observed_at_ns;
    auto counters_ok = [&](const auto &entries) {
        for (std::size_t i=0;i<entries.count;++i)
            if (entries.values[i].counter.state==2 && entries.values[i].counter.sampled_at_ns>limit) return false;
        return true;
    };
    if (value.receivers.present) for (std::size_t i=0;i<value.receivers.value.count;++i) {
        const auto &receiver=value.receivers.value.values[i];
        for (std::size_t j=0;j<receiver.measurements.entries.count;++j)
            if (receiver.measurements.entries.values[j].measurement.state==2 &&
                receiver.measurements.entries.values[j].measurement.sampled_at_ns>limit) return false;
        if (!counters_ok(receiver.receiver_statistics.entries)) return false;
        if (receiver.stream_statistics.present && !counters_ok(receiver.stream_statistics.value.entries)) return false;
        if (receiver.last_stream.present && !counters_ok(receiver.last_stream.value.statistics.entries)) return false;
    }
    if (value.readers.present) for (std::size_t i=0;i<value.readers.value.count;++i) {
        const auto &card=value.readers.value.values[i].card;
        if (card.sampled_at_ns.present && card.sampled_at_ns.value>limit) return false;
    }
    return true;
}

}

WireFrameCode status_response_payload_size(const StatusResponse &value, std::size_t *size)
{
    if (!size || !valid_local(value)) return WireFrameCode::invalid_argument;
    Writer writer{nullptr};
    write_status_response(writer, value);
    if (writer.size > kWireMaxControlPayload) return WireFrameCode::invalid_argument;
    *size = writer.size;
    return WireFrameCode::ok;
}

WireFrameCode encode_status_response(const StatusResponse &value, dtv_buffer *output)
{
    if (!output || (output->capacity && !output->data)) return WireFrameCode::invalid_argument;
    std::size_t size = 0;
    auto code = status_response_payload_size(value, &size);
    if (code != WireFrameCode::ok) return code;
    if (output->capacity < size) return WireFrameCode::buffer_too_small;
    Writer writer{output->data};
    write_status_response(writer, value);
    output->size = size;
    return WireFrameCode::ok;
}

WireFrameCode decode_status_response(dtv_bytes payload, StatusResponse *value)
{
    if (!value || (payload.size && !payload.data)) return WireFrameCode::invalid_argument;
    auto code = parse_status_response(payload, nullptr);
    if (code != WireFrameCode::ok) return code;
    return parse_status_response(payload, value);
}

WireFrameCode validate_status_response_scope(const StatusResponse &value)
{
    if (!valid_local(value)) return WireFrameCode::invalid_argument;
    if (validate_scope(value.scope) != WireFrameCode::ok) return WireFrameCode::invalid_argument;
    switch (value.scope.kind) {
    case Scope::Kind::enclosure:
        if (!value.functions.present) return WireFrameCode::invalid_argument;
        if (value.topology.state == 0) {
            if (value.receivers.present || value.readers.present) return WireFrameCode::invalid_argument;
        } else if (!value.receivers.present || !value.readers.present) return WireFrameCode::invalid_argument;
        return WireFrameCode::ok;
    case Scope::Kind::receiver:
        if (value.functions.present || value.readers.present || !value.receivers.present ||
            value.receivers.value.count != 1 || value.receivers.value.values[0].descriptor.id != value.scope.id)
            return WireFrameCode::invalid_argument;
        return WireFrameCode::ok;
    case Scope::Kind::reader:
        if (value.functions.present || value.receivers.present || !value.readers.present ||
            value.readers.value.count != 1 || value.readers.value.values[0].descriptor.id != value.scope.id)
            return WireFrameCode::invalid_argument;
        return WireFrameCode::ok;
    case Scope::Kind::stream:
        if (value.functions.present || value.readers.present || !value.receivers.present ||
            value.receivers.value.count != 1) return WireFrameCode::invalid_argument;
        {
            const auto &receiver=value.receivers.value.values[0];
            bool found=(receiver.stream.present && receiver.stream.value.id==value.scope.id) ||
                (receiver.last_stream.present && receiver.last_stream.value.id==value.scope.id);
            if (!found) return WireFrameCode::invalid_argument;
        }
        return WireFrameCode::ok;
    }
    return WireFrameCode::invalid_argument;
}

WireFrameCode validate_status_response(const StatusResponse &value, const ListResponse &list)
{
    if (validate_list_response(list) != WireFrameCode::ok ||
        validate_status_response_scope(value) != WireFrameCode::ok ||
        value.context.epoch != list.epoch || !sample_times_ok(value)) return WireFrameCode::invalid_argument;
    if (value.enclosure.present) {
        const auto &identity = value.enclosure.value.identity;
        if ((identity.serial_state == 0 || identity.serial_state == 1) &&
            identity.uniqueness != 0) return WireFrameCode::invalid_argument;
        if (identity.serial_state == 2 && identity.uniqueness != 3)
            return WireFrameCode::invalid_argument;
    }
    auto binding_exists = [&](const WireOptional<ListBinding> &binding) {
        return !binding.present || find_function(list, binding.value.function_id) != nullptr;
    };
    auto match_receiver = [&](const StatusReceiver &receiver) {
        const auto *canonical = find_receiver(list, receiver.descriptor.id);
        if (!canonical || !equal_receiver_descriptor(receiver.descriptor, *canonical) ||
            !binding_exists(receiver.descriptor.binding))
            return false;
        std::uint8_t mask = 0;
        for (std::size_t i=0;i<receiver.measurements.entries.count;++i) {
            const auto &entry=receiver.measurements.entries.values[i];
            auto bit=static_cast<std::uint8_t>(1u << (entry.id - 1));
            if (!(canonical->capabilities.measurement_mask & bit) || entry.measurement.state == 0) return false;
            mask |= bit;
        }
        return mask == canonical->capabilities.measurement_mask;
    };
    auto match_reader = [&](const StatusReader &reader) {
        const auto *canonical = find_reader(list, reader.descriptor.id);
        return canonical && equal_reader_descriptor(reader.descriptor, *canonical) &&
            binding_exists(reader.descriptor.binding);
    };
    if (value.enclosure.present && value.enclosure.value.limits.max_concurrent_streams.present) {
        auto count = list.receivers.present ? list.receivers.value.count : 0;
        if (value.enclosure.value.limits.max_concurrent_streams.value > count) return WireFrameCode::invalid_argument;
    }
    switch (value.scope.kind) {
    case Scope::Kind::enclosure:
        if (value.topology.state != list.topology.state || !value.functions.present ||
            value.functions.value.count != list.functions.count ||
            value.topology.expected_function_count.present != list.topology.expected_function_count.present ||
            (value.topology.expected_function_count.present && value.topology.expected_function_count.value !=
             list.topology.expected_function_count.value) ||
            value.receivers.present != list.receivers.present || value.readers.present != list.readers.present)
            return WireFrameCode::invalid_argument;
        if (value.enclosure.present != list.enclosure.present ||
            (value.enclosure.present && !equal_enclosure(value.enclosure.value, list.enclosure.value)))
            return WireFrameCode::invalid_argument;
        for (std::size_t i=0;i<value.functions.value.count;++i) {
            auto id=value.functions.value.values[i].id;
            if (!find_function(list,id)) return WireFrameCode::invalid_argument;
        }
        if (value.receivers.present) {
            if (value.receivers.value.count != list.receivers.value.count) return WireFrameCode::invalid_argument;
            for (std::size_t i=0;i<value.receivers.value.count;++i)
                if (!match_receiver(value.receivers.value.values[i])) return WireFrameCode::invalid_argument;
        }
        if (value.readers.present) {
            if (value.readers.value.count != list.readers.value.count) return WireFrameCode::invalid_argument;
            for (std::size_t i=0;i<value.readers.value.count;++i)
                if (!match_reader(value.readers.value.values[i])) return WireFrameCode::invalid_argument;
        }
        break;
    case Scope::Kind::receiver:
        if (!match_receiver(value.receivers.value.values[0])) return WireFrameCode::invalid_argument;
        break;
    case Scope::Kind::reader:
        if (!match_reader(value.readers.value.values[0])) return WireFrameCode::invalid_argument;
        break;
    case Scope::Kind::stream:
        if (!match_receiver(value.receivers.value.values[0]) || !value.receivers.value.values[0].leased)
            return WireFrameCode::invalid_argument;
        break;
    }
    return WireFrameCode::ok;
}

}
