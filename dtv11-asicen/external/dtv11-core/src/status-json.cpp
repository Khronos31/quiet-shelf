// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/status-json.hpp>

#include <algorithm>
#include <limits>
#include <string>

namespace dtv::internal {
namespace {

constexpr std::uint64_t kMaxJsonFrequency = (1ull << 53) - 1;
constexpr std::size_t kMaxFunctions = 8;
constexpr std::size_t kMaxReceivers = 32;
constexpr std::size_t kMaxReaders = 8;
constexpr std::size_t kMaxSerialBytes = 512;

bool utf8_ok(const char *data, std::size_t size)
{
    std::size_t index = 0;
    while (index < size) {
        const auto lead = static_cast<std::uint8_t>(data[index]);
        if (lead == 0) return false;
        if (lead < 0x80) {
            ++index;
            continue;
        }
        std::size_t extra = 0;
        std::uint8_t second_min = 0x80;
        std::uint8_t second_max = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1;
        } else if (lead == 0xe0) {
            extra = 2;
            second_min = 0xa0;
        } else if (lead >= 0xe1 && lead <= 0xec) {
            extra = 2;
        } else if (lead == 0xed) {
            extra = 2;
            second_max = 0x9f;
        } else if (lead >= 0xee && lead <= 0xef) {
            extra = 2;
        } else if (lead == 0xf0) {
            extra = 3;
            second_min = 0x90;
        } else if (lead >= 0xf1 && lead <= 0xf3) {
            extra = 3;
        } else if (lead == 0xf4) {
            extra = 3;
            second_max = 0x8f;
        } else {
            return false;
        }
        if (index + extra >= size) return false;
        const auto second = static_cast<std::uint8_t>(data[index + 1]);
        if (second < second_min || second > second_max) return false;
        for (std::size_t offset = 2; offset <= extra; ++offset) {
            const auto byte = static_cast<std::uint8_t>(data[index + offset]);
            if (byte < 0x80 || byte > 0xbf) return false;
        }
        index += extra + 1;
    }
    return true;
}

bool text_ok(const StatusText &text, std::size_t max_size, bool required)
{
    if (!text.present) return !required;
    if (text.data == nullptr) return false;
    if (text.size > max_size) return false;
    return utf8_ok(text.data, text.size);
}

template <typename T>
bool array_ok(const StatusArray<T> &array, std::size_t max_count, bool required)
{
    if (!array.present) return !required;
    if (array.count > max_count) return false;
    return array.count == 0 || array.items != nullptr;
}

bool systems_ok(std::uint8_t systems)
{
    return systems != 0 && (systems & ~0x03) == 0;
}

std::size_t bit_count(std::uint8_t value)
{
    std::size_t count = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        if ((value & (1u << bit)) != 0) ++count;
    }
    return count;
}

bool same_binding(const StatusBinding &left, const StatusBinding &right)
{
    return left.present == right.present &&
           (!left.present || (left.function_id == right.function_id &&
                              left.local_id == right.local_id));
}

bool same_lna(const StatusLna &left, const StatusLna &right)
{
    if (left.count != right.count) return false;
    for (std::size_t i = 0; i < left.count; ++i) {
        if (left.entries[i].system != right.entries[i].system) return false;
        if (left.entries[i].default_value.present !=
            right.entries[i].default_value.present) {
            return false;
        }
        if (left.entries[i].default_value.present &&
            left.entries[i].default_value.value != right.entries[i].default_value.value) {
            return false;
        }
    }
    return true;
}

bool same_capabilities(const StatusCapabilities &left, const StatusCapabilities &right)
{
    if (left.ts_scope != right.ts_scope) return false;
    if (left.max_pids.present != right.max_pids.present) return false;
    if (left.max_pids.present && left.max_pids.value != right.max_pids.value) return false;
    if (left.controls != right.controls) return false;
    if (!same_lna(left.lna, right.lna)) return false;
    if (left.lnb.count != right.lnb.count) return false;
    for (std::size_t i = 0; i < left.lnb.count; ++i) {
        if (left.lnb.values[i] != right.lnb.values[i]) return false;
    }
    return left.measurement_mask == right.measurement_mask;
}

bool same_receiver_descriptor(const StatusReceiverDescriptor &left,
                              const StatusReceiverDescriptor &right)
{
    return left.receiver == right.receiver && left.systems == right.systems &&
           same_binding(left.binding, right.binding) &&
           same_capabilities(left.capabilities, right.capabilities);
}

bool same_reader_descriptor(const StatusReaderDescriptor &left,
                            const StatusReaderDescriptor &right)
{
    return left.reader == right.reader && same_binding(left.binding, right.binding);
}

bool capabilities_ok(const StatusCapabilities &capabilities, std::uint8_t systems)
{
    if (capabilities.ts_scope > 1) return false;
    if (capabilities.max_pids.present && capabilities.max_pids.value == 0) return false;
    if ((capabilities.controls & ~0x03) != 0) return false;
    if ((capabilities.measurement_mask & ~0x07) != 0) return false;
    const bool lna = (capabilities.controls & 0x01) != 0;
    const bool lnb = (capabilities.controls & 0x02) != 0;
    if (lna) {
        if (capabilities.lna.count != bit_count(systems)) return false;
        std::uint8_t seen = 0;
        for (std::size_t i = 0; i < capabilities.lna.count; ++i) {
            const auto system = capabilities.lna.entries[i].system;
            if (system != 1 && system != 2) return false;
            const auto bit = static_cast<std::uint8_t>(1u << (system - 1));
            if ((systems & bit) == 0 || (seen & bit) != 0) return false;
            seen |= bit;
        }
        if (seen != systems) return false;
    } else if (capabilities.lna.count != 0) {
        return false;
    }
    if (lnb) {
        if (capabilities.lnb.count == 0 || capabilities.lnb.count > 2) return false;
        for (std::size_t i = 0; i < capabilities.lnb.count; ++i) {
            for (std::size_t j = i + 1; j < capabilities.lnb.count; ++j) {
                if (capabilities.lnb.values[i] == capabilities.lnb.values[j]) return false;
            }
        }
    } else if (capabilities.lnb.count != 0) {
        return false;
    }
    return true;
}

bool receiver_descriptor_ok(const StatusReceiverDescriptor &descriptor)
{
    if (!systems_ok(descriptor.systems)) return false;
    return capabilities_ok(descriptor.capabilities, descriptor.systems);
}

bool reader_descriptor_ok(const StatusReaderDescriptor &descriptor)
{
    (void)descriptor;
    return true;
}

bool error_ok(const StatusError &error)
{
    if (!error.present) return true;
    if (error.code == 0) return false;
    return text_ok(error.operation, 32, true) && text_ok(error.detail, 256, true);
}

const char *error_name(std::uint32_t code)
{
    switch (code) {
    case 1: return "INVALID_ARGUMENT";
    case 2: return "NOT_FOUND";
    case 3: return "NOT_READY";
    case 4: return "UNSUPPORTED";
    case 5: return "BUSY";
    case 6: return "TIMEOUT";
    case 7: return "VERSION_MISMATCH";
    case 8: return "PROTOCOL_ERROR";
    case 9: return "USB_IO";
    case 10: return "DISCONNECTED";
    case 11: return "TS_ERROR";
    case 12: return "NO_CARD";
    case 13: return "CARD_REMOVED";
    case 14: return "CARD_IO";
    case 15: return "BUFFER_TOO_SMALL";
    case 16: return "FIRMWARE_REJECTED";
    case 17: return "PERMISSION_DENIED";
    case 18: return "RESOURCE_LIMIT";
    case 19: return "OUTPUT_ERROR";
    case 20: return "INTERNAL";
    case 21: return "CANCELLED";
    case 22: return "SLOW_CONSUMER";
    default: return nullptr;
    }
}

bool measurement_ok(const StatusMeasurementView &measurement)
{
    if (measurement.state != 1 && measurement.state != 2) return false;
    if (measurement.state == 2) return measurement.reason == 0;
    return measurement.reason >= 1 && measurement.reason <= 5 &&
           measurement.sampled_at_ns == 0;
}

bool counter_ok(const StatusCounterView &counter)
{
    if (!measurement_ok(counter.measurement)) return false;
    if (counter.measurement.state == 2) {
        if (counter.overflow &&
            counter.measurement.counter_value != std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
    } else if (counter.overflow) {
        return false;
    }
    return true;
}

bool counters_ok(const StatusCountersView &counters, bool stream)
{
    if (!counters.entries.present) return counters.entries.count == 0;
    if (counters.entries.count > (stream ? 16 : 4)) return false;
    if (counters.entries.count != 0 && counters.entries.items == nullptr) return false;
    std::uint8_t previous = 0;
    for (std::size_t i = 0; i < counters.entries.count; ++i) {
        const auto &entry = counters.entries.items[i];
        if (!counter_ok(entry)) return false;
        if (stream) {
            if (entry.id < 5 || entry.id > 32) return false;
            if (i > 0 && entry.id <= previous) return false;
            if (i == 0 && entry.id != 5) return false;
            if (i == 1 && entry.id != 6) return false;
            previous = entry.id;
        } else {
            if (entry.id < 1 || entry.id > 4) return false;
            for (std::size_t j = 0; j < i; ++j) {
                if (counters.entries.items[j].id == entry.id) return false;
            }
        }
    }
    if (stream && counters.entries.count < 2) return false;
    return true;
}

bool measurements_ok(const StatusMeasurementsView &measurements, std::uint8_t mask)
{
    if (!measurements.entries.present) return measurements.entries.count == 0 && mask == 0;
    const auto expected = bit_count(mask);
    if (measurements.entries.count != expected) return false;
    if (measurements.entries.count != 0 && measurements.entries.items == nullptr) return false;
    std::uint8_t seen = 0;
    for (std::size_t i = 0; i < measurements.entries.count; ++i) {
        const auto &entry = measurements.entries.items[i];
        if (entry.id < 1 || entry.id > 3) return false;
        const auto bit = static_cast<std::uint8_t>(1u << (entry.id - 1));
        if ((mask & bit) == 0 || (seen & bit) != 0) return false;
        if (!measurement_ok(entry.measurement)) return false;
        seen |= bit;
    }
    return seen == mask;
}

bool tune_ok(const StatusTuneView &tune)
{
    if (tune.system != 1 && tune.system != 2) return false;
    if (tune.frequency_hz == 0 || tune.frequency_hz > kMaxJsonFrequency) return false;
    if (tune.programmed_hz.present &&
        (tune.programmed_hz.value == 0 || tune.programmed_hz.value > kMaxJsonFrequency)) {
        return false;
    }
    if (tune.selection > 2 || tune.verification > 2) return false;
    if (tune.selection == 0 && tune.selection_value != 0) return false;
    return true;
}

bool stream_ok(const StatusStreamView &stream)
{
    return stream.id != 0 && stream.phase >= 1 && stream.phase <= 4;
}

bool stream_final_ok(const StatusStreamFinalView &stream)
{
    return stream.id != 0 && stream.reason <= 7 && error_ok(stream.error) &&
           counters_ok(stream.statistics, true);
}

bool list_function_exists(const StatusListResponseView &list, std::uint16_t id)
{
    for (std::size_t i = 0; i < list.functions.count; ++i) {
        if (list.functions.items[i].id == id) return true;
    }
    return false;
}

bool binding_ok(const StatusBinding &binding, const StatusListResponseView &list)
{
    return !binding.present || list_function_exists(list, binding.function_id);
}

bool usb_function_ok(const StatusUsbFunctionView &function)
{
    if (function.mode > 2) return false;
    if (!text_ok(function.location.usb_path, 64, false)) return false;
    if (!text_ok(function.serial, kMaxSerialBytes, false)) return false;
    if (!text_ok(function.model, 128, false)) return false;
    return error_ok(function.last_error);
}

const StatusReceiverDescriptor *find_list_receiver(const StatusListResponseView &list,
                                                   std::uint16_t id)
{
    if (!list.receivers.present) return nullptr;
    for (std::size_t i = 0; i < list.receivers.count; ++i) {
        if (list.receivers.items[i].receiver == id) return &list.receivers.items[i];
    }
    return nullptr;
}

const StatusReaderDescriptor *find_list_reader(const StatusListResponseView &list,
                                               std::uint16_t id)
{
    if (!list.readers.present) return nullptr;
    for (std::size_t i = 0; i < list.readers.count; ++i) {
        if (list.readers.items[i].reader == id) return &list.readers.items[i];
    }
    return nullptr;
}

bool list_ok(const StatusListResponseView &list)
{
    if (!array_ok(list.functions, kMaxFunctions, true)) return false;
    for (std::size_t i = 0; i < list.functions.count; ++i) {
        if (!usb_function_ok(list.functions.items[i])) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (list.functions.items[j].id == list.functions.items[i].id) return false;
        }
    }
    if (list.topology.state > 2) return false;
    const bool known = list.topology.state != 0;
    if (list.receivers.present != known) return false;
    if (list.readers.present != known) return false;
    if (list.receivers.present) {
        if (!array_ok(list.receivers, kMaxReceivers, true)) return false;
        for (std::size_t i = 0; i < list.receivers.count; ++i) {
            const auto &descriptor = list.receivers.items[i];
            if (!receiver_descriptor_ok(descriptor)) return false;
            if (!binding_ok(descriptor.binding, list)) return false;
            for (std::size_t j = 0; j < i; ++j) {
                if (list.receivers.items[j].receiver == descriptor.receiver) return false;
            }
        }
    }
    if (list.readers.present) {
        if (!array_ok(list.readers, kMaxReaders, true)) return false;
        for (std::size_t i = 0; i < list.readers.count; ++i) {
            const auto &descriptor = list.readers.items[i];
            if (!reader_descriptor_ok(descriptor)) return false;
            if (!binding_ok(descriptor.binding, list)) return false;
            for (std::size_t j = 0; j < i; ++j) {
                if (list.readers.items[j].reader == descriptor.reader) return false;
            }
        }
    }
    if (list.enclosure.present) {
        if (list.enclosure.limits.max_concurrent_streams.present) {
            const auto limit = list.enclosure.limits.max_concurrent_streams.value;
            if (limit == 0) return false;
            if (list.receivers.present && limit > list.receivers.count) return false;
        }
    }
    return true;
}

bool same_topology(const StatusTopology &left, const StatusTopology &right)
{
    if (left.state != right.state) return false;
    if (left.expected_function_count.present != right.expected_function_count.present) {
        return false;
    }
    return !left.expected_function_count.present ||
           left.expected_function_count.value == right.expected_function_count.value;
}

bool receiver_ok(const StatusReceiverView &receiver, const StatusListResponseView &list,
                 std::uint64_t observed_at_ns)
{
    if (!receiver_descriptor_ok(receiver.descriptor)) return false;
    const auto *canonical = find_list_receiver(list, receiver.descriptor.receiver);
    if (canonical == nullptr || !same_receiver_descriptor(receiver.descriptor, *canonical)) {
        return false;
    }
    if (receiver.state > 5 || receiver.operation > 3) return false;
    if ((receiver.state == 5) == receiver.present) return false;
    if (receiver.current_tune.present && !tune_ok(receiver.current_tune.value)) return false;
    if (receiver.last_successful_tune.present &&
        !tune_ok(receiver.last_successful_tune.value)) {
        return false;
    }
    if (receiver.stream.present && !stream_ok(receiver.stream.value)) return false;
    if (receiver.last_stream.present) {
        if (!stream_final_ok(receiver.last_stream.value)) return false;
        if (!receiver.leased) return false;
    }
    if (receiver.stream_statistics.present && !receiver.stream.present) return false;
    if (!measurements_ok(receiver.measurements,
                         receiver.descriptor.capabilities.measurement_mask)) {
        return false;
    }
    if (!counters_ok(receiver.receiver_statistics, false)) return false;
    if (receiver.stream_statistics.present &&
        !counters_ok(receiver.stream_statistics.value, true)) {
        return false;
    }
    if (!error_ok(receiver.last_error)) return false;
    for (std::size_t i = 0; i < receiver.measurements.entries.count; ++i) {
        const auto &measurement = receiver.measurements.entries.items[i].measurement;
        if (measurement.state == 2 && measurement.sampled_at_ns > observed_at_ns) return false;
    }
    for (std::size_t i = 0; i < receiver.receiver_statistics.entries.count; ++i) {
        const auto &measurement = receiver.receiver_statistics.entries.items[i].measurement;
        if (measurement.state == 2 && measurement.sampled_at_ns > observed_at_ns) return false;
    }
    if (receiver.stream_statistics.present) {
        for (std::size_t i = 0; i < receiver.stream_statistics.value.entries.count; ++i) {
            const auto &measurement =
                receiver.stream_statistics.value.entries.items[i].measurement;
            if (measurement.state == 2 && measurement.sampled_at_ns > observed_at_ns) {
                return false;
            }
        }
    }
    if (receiver.last_stream.present) {
        for (std::size_t i = 0; i < receiver.last_stream.value.statistics.entries.count; ++i) {
            const auto &measurement =
                receiver.last_stream.value.statistics.entries.items[i].measurement;
            if (measurement.state == 2 && measurement.sampled_at_ns > observed_at_ns) {
                return false;
            }
        }
    }
    return true;
}

bool reader_ok(const StatusReaderView &reader, const StatusListResponseView &list,
               std::uint64_t observed_at_ns)
{
    if (!reader_descriptor_ok(reader.descriptor)) return false;
    const auto *canonical = find_list_reader(list, reader.descriptor.reader);
    if (canonical == nullptr || !same_reader_descriptor(reader.descriptor, *canonical)) {
        return false;
    }
    if (reader.card.state > 3 || reader.operation > 4) return false;
    if (!reader.present && reader.card.state != 3) return false;
    if (reader.card.state == 2 || reader.card.state == 1) {
        if (!reader.card.sampled_at_ns.present) return false;
        if (reader.card.reason != 0) return false;
        if (reader.card.sampled_at_ns.value > observed_at_ns) return false;
    } else if (reader.card.sampled_at_ns.present) {
        return false;
    }
    if (reader.card.state == 0) {
        if (reader.card.reason != 1 && reader.card.reason != 2) return false;
    } else if (reader.card.state == 3) {
        if (reader.card.reason != 4 && reader.card.reason != 5) return false;
    } else if (reader.card.reason != 0) {
        return false;
    }
    return error_ok(reader.last_error);
}

bool enclosure_ok(const StatusEnclosureView &enclosure, bool list_context)
{
    if (enclosure.identity.serial_state > 2) return false;
    if (!text_ok(enclosure.model, 128, false)) return false;
    if (!text_ok(enclosure.identity.serial, kMaxSerialBytes, false)) return false;
    const auto state = enclosure.identity.serial_state;
    if (state == 1) {
        if (!enclosure.identity.serial.present || enclosure.identity.serial.size == 0) {
            return false;
        }
        if (!list_context && enclosure.identity.uniqueness != 0) return false;
    } else {
        if (enclosure.identity.serial.present) return false;
        if (state == 0 && enclosure.identity.uniqueness != 0) return false;
        if (state == 2 && enclosure.identity.uniqueness != 3) return false;
    }
    if (enclosure.identity.uniqueness > 3) return false;
    if (enclosure.limits.max_concurrent_streams.present &&
        enclosure.limits.max_concurrent_streams.value == 0) {
        return false;
    }
    return true;
}

bool scope_ok(const StatusScopeView &scope)
{
    if (scope.kind > 3) return false;
    if (scope.kind == 0) return scope.id == 0;
    if (scope.kind == 3) return scope.id != 0;
    return scope.id <= 0xffff;
}

bool status_ok(const StatusJsonInput &input)
{
    const auto &status = input.status;
    const auto &list = input.canonical_list;
    if (!list_ok(list)) return false;
    if (!text_ok(input.producer.product, 32, true) ||
        !text_ok(input.producer.version, 64, true) ||
        !text_ok(input.producer.core_version, 64, false)) {
        return false;
    }
    if (!text_ok(status.context.instance, 64, false)) return false;
    if (status.context.state > 4 || !error_ok(status.context.last_error)) return false;
    if (status.context.epoch != list.epoch) return false;
    if (status.topology.state > 2) return false;
    if (!same_topology(status.topology, list.topology)) return false;
    if (status.enclosure.present && !enclosure_ok(status.enclosure, false)) return false;
    if (status.enclosure.present && list.enclosure.present &&
        status.enclosure.id != list.enclosure.id) {
        return false;
    }
    if (status.enclosure.present &&
        status.enclosure.limits.max_concurrent_streams.present &&
        list.receivers.present) {
        if (status.enclosure.limits.max_concurrent_streams.value > list.receivers.count) {
            return false;
        }
    }
    if (!scope_ok(status.scope)) return false;

    if (status.functions.present) {
        if (!array_ok(status.functions, kMaxFunctions, true)) return false;
        for (std::size_t i = 0; i < status.functions.count; ++i) {
            if (!usb_function_ok(status.functions.items[i])) return false;
            if (!list_function_exists(list, status.functions.items[i].id)) return false;
            for (std::size_t j = 0; j < i; ++j) {
                if (status.functions.items[j].id == status.functions.items[i].id) return false;
            }
        }
    }

    const bool known_topology = list.topology.state != 0;
    if (status.scope.kind == 0) {
        if (!status.functions.present) return false;
        if (status.functions.count != list.functions.count) return false;
        if (known_topology) {
            if (!status.receivers.present || !status.readers.present) return false;
        } else if (status.receivers.present || status.readers.present) {
            return false;
        }
    } else {
        if (status.functions.present) return false;
    }

    if (status.receivers.present) {
        if (!array_ok(status.receivers, kMaxReceivers, true)) return false;
        for (std::size_t i = 0; i < status.receivers.count; ++i) {
            if (!receiver_ok(status.receivers.items[i], list,
                             status.snapshot.observed_at_ns)) {
                return false;
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (status.receivers.items[j].descriptor.receiver ==
                    status.receivers.items[i].descriptor.receiver) {
                    return false;
                }
            }
        }
    }
    if (status.readers.present) {
        if (!array_ok(status.readers, kMaxReaders, true)) return false;
        for (std::size_t i = 0; i < status.readers.count; ++i) {
            if (!reader_ok(status.readers.items[i], list, status.snapshot.observed_at_ns)) {
                return false;
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (status.readers.items[j].descriptor.reader ==
                    status.readers.items[i].descriptor.reader) {
                    return false;
                }
            }
        }
    }

    switch (status.scope.kind) {
    case 0:
        if (known_topology) {
            if (status.receivers.count != list.receivers.count) return false;
            if (status.readers.count != list.readers.count) return false;
            for (std::size_t i = 0; i < list.receivers.count; ++i) {
                bool found = false;
                for (std::size_t j = 0; j < status.receivers.count; ++j) {
                    if (same_receiver_descriptor(status.receivers.items[j].descriptor,
                                                 list.receivers.items[i])) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            for (std::size_t i = 0; i < list.readers.count; ++i) {
                bool found = false;
                for (std::size_t j = 0; j < status.readers.count; ++j) {
                    if (same_reader_descriptor(status.readers.items[j].descriptor,
                                               list.readers.items[i])) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        }
        if (status.enclosure.present != list.enclosure.present) return false;
        break;
    case 1:
    case 3: {
        if (!status.receivers.present || status.receivers.count != 1) return false;
        if (status.readers.present) return false;
        if (status.scope.kind == 1) {
            if (status.receivers.items[0].descriptor.receiver != status.scope.id) return false;
        } else {
            bool found = false;
            for (std::size_t i = 0; i < status.receivers.count; ++i) {
                const auto &receiver = status.receivers.items[i];
                if (receiver.stream.present && receiver.stream.value.id == status.scope.id) {
                    found = true;
                }
                if (receiver.last_stream.present &&
                    receiver.last_stream.value.id == status.scope.id) {
                    if (!receiver.leased) return false;
                    found = true;
                }
            }
            if (!found) return false;
        }
        break;
    }
    case 2: {
        if (status.receivers.present) return false;
        if (!status.readers.present || status.readers.count != 1) return false;
        if (status.readers.items[0].descriptor.reader != status.scope.id) return false;
        break;
    }
    default:
        return false;
    }
    return true;
}

class Writer {
public:
    Writer(std::uint8_t *output, std::size_t capacity)
        : output_(output), capacity_(capacity) {}

    bool overflow() const { return overflow_; }
    std::uint64_t count() const { return count_; }

    void ch(char value)
    {
        if (overflow_) return;
        if (count_ == std::numeric_limits<std::uint64_t>::max()) {
            overflow_ = true;
            return;
        }
        if (output_ != nullptr && count_ < capacity_) {
            output_[count_] = static_cast<std::uint8_t>(value);
        }
        ++count_;
    }

    void raw(const char *data, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i) ch(data[i]);
    }

    void key(const char *name)
    {
        str_literal(name, std::char_traits<char>::length(name));
        ch(':');
    }

    void str_literal(const char *data, std::size_t size)
    {
        ch('"');
        for (std::size_t i = 0; i < size; ++i) {
            const auto byte = static_cast<std::uint8_t>(data[i]);
            switch (byte) {
            case '"': raw("\\\"", 2); break;
            case '\\': raw("\\\\", 2); break;
            case '\b': raw("\\b", 2); break;
            case '\f': raw("\\f", 2); break;
            case '\n': raw("\\n", 2); break;
            case '\r': raw("\\r", 2); break;
            case '\t': raw("\\t", 2); break;
            default:
                if (byte < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    raw("\\u00", 4);
                    ch(kHex[(byte >> 4) & 0x0f]);
                    ch(kHex[byte & 0x0f]);
                } else {
                    ch(static_cast<char>(byte));
                }
                break;
            }
        }
        ch('"');
    }

    void text(const StatusText &value)
    {
        if (!value.present) {
            null();
        } else {
            str_literal(value.data, value.size);
        }
    }

    void null()
    {
        raw("null", 4);
    }

    void boolean(bool value)
    {
        if (value) raw("true", 4);
        else raw("false", 5);
    }

    void u64(std::uint64_t value)
    {
        char buffer[20];
        std::size_t length = 0;
        do {
            buffer[length++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (length != 0) ch(buffer[--length]);
    }

    void i64(std::int64_t value)
    {
        if (value < 0) {
            ch('-');
            u64(static_cast<std::uint64_t>(-(value + 1)) + 1);
        } else {
            u64(static_cast<std::uint64_t>(value));
        }
    }

    void quoted_u64(std::uint64_t value)
    {
        ch('"');
        u64(value);
        ch('"');
    }

private:
    std::uint8_t *output_;
    std::size_t capacity_;
    std::uint64_t count_ = 0;
    bool overflow_ = false;
};

const char *context_state_name(std::uint8_t state)
{
    static constexpr const char *names[] = {"starting", "ready", "stopping", "error",
                                            "disconnected"};
    return state <= 4 ? names[state] : "error";
}

const char *topology_state_name(std::uint8_t state)
{
    static constexpr const char *names[] = {"unknown", "partial", "complete"};
    return state <= 2 ? names[state] : "unknown";
}

const char *mode_name(std::uint8_t mode)
{
    static constexpr const char *names[] = {"unknown", "cold", "runtime"};
    return mode <= 2 ? names[mode] : "unknown";
}

const char *scope_kind_name(std::uint8_t kind)
{
    static constexpr const char *names[] = {"enclosure", "receiver", "reader", "stream"};
    return kind <= 3 ? names[kind] : "enclosure";
}

const char *receiver_state_name(std::uint8_t state)
{
    static constexpr const char *names[] = {"free", "leased", "tuned", "streaming",
                                            "error", "absent"};
    return state <= 5 ? names[state] : "absent";
}

const char *operation_name(std::uint8_t operation)
{
    static constexpr const char *names[] = {"idle", "tuning", "stopping", "releasing"};
    return operation <= 3 ? names[operation] : "idle";
}

const char *system_name(std::uint8_t system)
{
    return system == 2 ? "ISDB-S" : "ISDB-T";
}

const char *verification_name(std::uint8_t verification)
{
    static constexpr const char *names[] = {"commands", "lock", "ts_sync"};
    return verification <= 2 ? names[verification] : "commands";
}

const char *selection_name(std::uint8_t selection)
{
    static constexpr const char *names[] = {"auto", "tsid", "slot"};
    return selection <= 2 ? names[selection] : "auto";
}

const char *phase_name(std::uint8_t phase)
{
    static constexpr const char *names[] = {"armed", "attaching", "active", "closing"};
    return phase >= 1 && phase <= 4 ? names[phase - 1] : "armed";
}

const char *end_reason_name(std::uint8_t reason)
{
    static constexpr const char *names[] = {"stopped", "data_closed", "owner_closed",
                                            "attach_expired", "slow_consumer",
                                            "device_lost", "backend_error", "shutdown"};
    return reason <= 7 ? names[reason] : "stopped";
}

const char *card_state_name(std::uint8_t state)
{
    static constexpr const char *names[] = {"unknown", "absent", "present", "unavailable"};
    return state <= 3 ? names[state] : "unknown";
}

const char *card_operation_name(std::uint8_t operation)
{
    static constexpr const char *names[] = {"idle", "initializing", "transmitting",
                                            "resetting", "stopping"};
    return operation <= 4 ? names[operation] : "idle";
}

const char *unknown_reason_name(std::uint8_t reason)
{
    static constexpr const char *names[] = {"not_sampled", "stale", "tune_changed",
                                            "device_lost", "read_failed"};
    return reason >= 1 && reason <= 5 ? names[reason - 1] : "not_sampled";
}

const char *metric_name(std::uint8_t id)
{
    switch (id) {
    case 1: return "lock";
    case 2: return "cnr_mdb";
    case 3: return "signal_mdbm";
    default: return nullptr;
    }
}

const char *counter_name(std::uint8_t id)
{
    switch (id) {
    case 1: return "captured_packets";
    case 2: return "usb_errors";
    case 3: return "framing_errors";
    case 4: return "upstream_lost_packets";
    case 5: return "transmitted_packets";
    case 6: return "queue_dropped_packets";
    case 7: return "stream_bytes";
    case 8: return "sync_errors";
    case 9: return "tei_packets";
    case 10: return "continuity_errors";
    case 11: return "stream_usb_errors";
    case 12: return "empty_intervals";
    default: return nullptr;
    }
}

void write_hex32(Writer &writer, const std::array<std::uint8_t, 16> &epoch)
{
    static constexpr char kHex[] = "0123456789abcdef";
    writer.ch('"');
    for (const auto byte : epoch) {
        writer.ch(kHex[(byte >> 4) & 0x0f]);
        writer.ch(kHex[byte & 0x0f]);
    }
    writer.ch('"');
}

void write_vid_pid(Writer &writer, std::uint16_t vid, std::uint16_t pid)
{
    static constexpr char kHex[] = "0123456789abcdef";
    writer.ch('"');
    for (int shift = 12; shift >= 0; shift -= 4) writer.ch(kHex[(vid >> shift) & 0x0f]);
    writer.ch(':');
    for (int shift = 12; shift >= 0; shift -= 4) writer.ch(kHex[(pid >> shift) & 0x0f]);
    writer.ch('"');
}

void write_binding(Writer &writer, const StatusBinding &binding)
{
    if (!binding.present) {
        writer.null();
        return;
    }
    writer.ch('{');
    writer.key("function_id");
    writer.u64(binding.function_id);
    writer.ch(',');
    writer.key("local_id");
    writer.u64(binding.local_id);
    writer.ch('}');
}

void write_error(Writer &writer, const StatusError &error)
{
    if (!error.present) {
        writer.null();
        return;
    }
    writer.ch('{');
    writer.key("code");
    const char *name = error_name(error.code);
    if (name != nullptr) {
        writer.str_literal(name, std::char_traits<char>::length(name));
    } else {
        writer.str_literal("UNKNOWN", 7);
        writer.ch(',');
        writer.key("numeric_code");
        writer.u64(error.code);
    }
    writer.ch(',');
    writer.key("operation");
    writer.text(error.operation);
    writer.ch(',');
    writer.key("detail");
    writer.text(error.detail);
    writer.ch('}');
}

void write_counter(Writer &writer, const StatusCounterView &counter)
{
    writer.ch('{');
    writer.key("value");
    if (counter.measurement.state == 2) writer.quoted_u64(counter.measurement.counter_value);
    else writer.null();
    writer.ch(',');
    writer.key("sampled_at_ns");
    if (counter.measurement.state == 2) writer.quoted_u64(counter.measurement.sampled_at_ns);
    else writer.null();
    writer.ch(',');
    writer.key("reason");
    if (counter.measurement.state == 2) writer.null();
    else writer.str_literal(unknown_reason_name(counter.measurement.reason),
                            std::char_traits<char>::length(unknown_reason_name(counter.measurement.reason)));
    writer.ch(',');
    writer.key("overflow");
    if (counter.measurement.state == 2) writer.boolean(counter.overflow);
    else writer.null();
    writer.ch('}');
}

void write_counter_map(Writer &writer, const StatusCountersView &counters)
{
    writer.ch('{');
    bool first = true;
    for (std::size_t i = 0; i < counters.entries.count; ++i) {
        const auto &entry = counters.entries.items[i];
        const char *name = counter_name(entry.id);
        if (name == nullptr) continue;
        if (!first) writer.ch(',');
        first = false;
        writer.key(name);
        write_counter(writer, entry);
    }
    writer.ch('}');
}

void write_measurements(Writer &writer, const StatusMeasurementsView &measurements)
{
    writer.ch('{');
    bool first = true;
    for (std::size_t i = 0; i < measurements.entries.count; ++i) {
        const auto &entry = measurements.entries.items[i];
        const char *name = metric_name(entry.id);
        if (name == nullptr) continue;
        if (!first) writer.ch(',');
        first = false;
        writer.key(name);
        if (entry.measurement.state != 2) {
            writer.ch('{');
            writer.key("value");
            writer.null();
            writer.ch(',');
            writer.key("sampled_at_ns");
            writer.null();
            writer.ch(',');
            writer.key("reason");
            const char *reason = unknown_reason_name(entry.measurement.reason);
            writer.str_literal(reason, std::char_traits<char>::length(reason));
            writer.ch('}');
        } else {
            writer.ch('{');
            writer.key("value");
            if (entry.id == 1) writer.boolean(entry.measurement.boolean_value);
            else writer.i64(entry.measurement.integer_value);
            writer.ch(',');
            writer.key("sampled_at_ns");
            writer.quoted_u64(entry.measurement.sampled_at_ns);
            writer.ch(',');
            writer.key("reason");
            writer.null();
            writer.ch('}');
        }
    }
    writer.ch('}');
}

void write_tune(Writer &writer, const StatusTuneView &tune)
{
    writer.ch('{');
    writer.key("revision");
    writer.quoted_u64(tune.revision);
    writer.ch(',');
    writer.key("system");
    const char *system = system_name(tune.system);
    writer.str_literal(system, std::char_traits<char>::length(system));
    writer.ch(',');
    writer.key("frequency_hz");
    writer.u64(tune.frequency_hz);
    writer.ch(',');
    writer.key("programmed_hz");
    if (tune.programmed_hz.present) writer.u64(tune.programmed_hz.value);
    else writer.null();
    writer.ch(',');
    writer.key("selection");
    writer.ch('{');
    writer.key("kind");
    const char *selection = selection_name(tune.selection);
    writer.str_literal(selection, std::char_traits<char>::length(selection));
    if (tune.selection != 0) {
        writer.ch(',');
        writer.key("value");
        writer.u64(tune.selection_value);
    }
    writer.ch('}');
    writer.ch(',');
    writer.key("verification");
    const char *verification = verification_name(tune.verification);
    writer.str_literal(verification, std::char_traits<char>::length(verification));
    writer.ch('}');
}

void write_stream(Writer &writer, const StatusStreamView &stream)
{
    writer.ch('{');
    writer.key("id");
    writer.quoted_u64(stream.id);
    writer.ch(',');
    writer.key("tune_revision");
    writer.quoted_u64(stream.tune_revision);
    writer.ch(',');
    writer.key("phase");
    const char *phase = phase_name(stream.phase);
    writer.str_literal(phase, std::char_traits<char>::length(phase));
    writer.ch('}');
}

void write_stream_final(Writer &writer, const StatusStreamFinalView &stream)
{
    writer.ch('{');
    writer.key("id");
    writer.quoted_u64(stream.id);
    writer.ch(',');
    writer.key("tune_revision");
    writer.quoted_u64(stream.tune_revision);
    writer.ch(',');
    writer.key("reason");
    const char *reason = end_reason_name(stream.reason);
    writer.str_literal(reason, std::char_traits<char>::length(reason));
    writer.ch(',');
    writer.key("error");
    write_error(writer, stream.error);
    writer.ch(',');
    writer.key("statistics");
    write_counter_map(writer, stream.statistics);
    writer.ch('}');
}

void write_scope(Writer &writer, const StatusScopeView &scope)
{
    writer.ch('{');
    writer.key("kind");
    const char *kind = scope_kind_name(scope.kind);
    writer.str_literal(kind, std::char_traits<char>::length(kind));
    writer.ch(',');
    writer.key("id");
    if (scope.kind == 0) writer.null();
    else if (scope.kind == 3) writer.quoted_u64(scope.id);
    else writer.u64(scope.id);
    writer.ch('}');
}

void write_enclosure(Writer &writer, const StatusEnclosureView &enclosure,
                     const StatusOptional<bool> &lnb_15v_enabled,
                     const StatusOptional<bool> &card_inserted)
{
    if (!enclosure.present) {
        writer.null();
        return;
    }
    writer.ch('{');
    writer.key("id");
    writer.u64(enclosure.id);
    writer.ch(',');
    writer.key("model");
    writer.text(enclosure.model);
    writer.ch(',');
    writer.key("serial");
    writer.text(enclosure.identity.serial);
    writer.ch(',');
    writer.key("lnb_15v_enabled");
    if (lnb_15v_enabled.present) writer.boolean(lnb_15v_enabled.value);
    else writer.null();
    writer.ch(',');
    writer.key("card_inserted");
    if (card_inserted.present) writer.boolean(card_inserted.value);
    else writer.null();
    writer.ch(',');
    writer.key("limits");
    writer.ch('{');
    writer.key("max_concurrent_streams");
    if (enclosure.limits.max_concurrent_streams.present) {
        writer.u64(enclosure.limits.max_concurrent_streams.value);
    } else {
        writer.null();
    }
    writer.ch('}');
    writer.ch('}');
}

void write_topology(Writer &writer, const StatusTopology &topology)
{
    writer.ch('{');
    writer.key("state");
    const char *state = topology_state_name(topology.state);
    writer.str_literal(state, std::char_traits<char>::length(state));
    writer.ch(',');
    writer.key("expected_function_count");
    if (topology.expected_function_count.present) {
        writer.u64(topology.expected_function_count.value);
    } else {
        writer.null();
    }
    writer.ch('}');
}

void write_context(Writer &writer, const StatusContextView &context)
{
    writer.ch('{');
    writer.key("epoch");
    write_hex32(writer, context.epoch);
    writer.ch(',');
    writer.key("instance");
    writer.text(context.instance);
    writer.ch(',');
    writer.key("state");
    const char *state = context_state_name(context.state);
    writer.str_literal(state, std::char_traits<char>::length(state));
    writer.ch(',');
    writer.key("last_error");
    write_error(writer, context.last_error);
    writer.ch('}');
}

void write_usb_function(Writer &writer, const StatusUsbFunctionView &function)
{
    writer.ch('{');
    writer.key("id");
    writer.u64(function.id);
    writer.ch(',');
    writer.key("usb");
    write_vid_pid(writer, function.vid, function.pid);
    writer.ch(',');
    writer.key("mode");
    const char *mode = mode_name(function.mode);
    writer.str_literal(mode, std::char_traits<char>::length(mode));
    writer.ch(',');
    writer.key("location");
    writer.ch('{');
    writer.key("usb_path");
    writer.text(function.location.usb_path);
    writer.ch(',');
    writer.key("bus");
    if (function.location.bus.present) writer.u64(function.location.bus.value);
    else writer.null();
    writer.ch(',');
    writer.key("address");
    if (function.location.address.present) writer.u64(function.location.address.value);
    else writer.null();
    writer.ch('}');
    writer.ch(',');
    writer.key("serial");
    writer.text(function.serial);
    writer.ch(',');
    writer.key("model");
    writer.text(function.model);
    writer.ch(',');
    writer.key("present");
    writer.boolean(function.present);
    writer.ch(',');
    writer.key("last_error");
    write_error(writer, function.last_error);
    writer.ch('}');
}

void write_receiver_descriptor(Writer &writer, const StatusReceiverDescriptor &descriptor)
{
    writer.ch('{');
    writer.key("receiver");
    writer.u64(descriptor.receiver);
    writer.ch(',');
    writer.key("systems");
    writer.ch('[');
    bool first = true;
    if ((descriptor.systems & 0x01) != 0) {
        writer.str_literal("ISDB-T", 6);
        first = false;
    }
    if ((descriptor.systems & 0x02) != 0) {
        if (!first) writer.ch(',');
        writer.str_literal("ISDB-S", 6);
    }
    writer.ch(']');
    writer.ch(',');
    writer.key("binding");
    write_binding(writer, descriptor.binding);
    writer.ch('}');
}

void write_reader_descriptor(Writer &writer, const StatusReaderDescriptor &descriptor)
{
    writer.ch('{');
    writer.key("reader");
    writer.u64(descriptor.reader);
    writer.ch(',');
    writer.key("binding");
    write_binding(writer, descriptor.binding);
    writer.ch('}');
}

void write_receiver(Writer &writer, const StatusReceiverView &receiver)
{
    writer.ch('{');
    writer.key("descriptor");
    write_receiver_descriptor(writer, receiver.descriptor);
    writer.ch(',');
    writer.key("present");
    writer.boolean(receiver.present);
    writer.ch(',');
    writer.key("state");
    const char *state = receiver_state_name(receiver.state);
    writer.str_literal(state, std::char_traits<char>::length(state));
    writer.ch(',');
    writer.key("operation");
    const char *operation = operation_name(receiver.operation);
    writer.str_literal(operation, std::char_traits<char>::length(operation));
    writer.ch(',');
    writer.key("leased");
    writer.boolean(receiver.leased);
    writer.ch(',');
    writer.key("tune_revision");
    writer.quoted_u64(receiver.tune_revision);
    writer.ch(',');
    writer.key("current_tune");
    if (receiver.current_tune.present) write_tune(writer, receiver.current_tune.value);
    else writer.null();
    writer.ch(',');
    writer.key("last_successful_tune");
    if (receiver.last_successful_tune.present) {
        write_tune(writer, receiver.last_successful_tune.value);
    } else {
        writer.null();
    }
    writer.ch(',');
    writer.key("stream");
    if (receiver.stream.present) write_stream(writer, receiver.stream.value);
    else writer.null();
    writer.ch(',');
    writer.key("last_stream");
    if (receiver.last_stream.present) write_stream_final(writer, receiver.last_stream.value);
    else writer.null();
    writer.ch(',');
    writer.key("measurements");
    write_measurements(writer, receiver.measurements);
    writer.ch(',');
    writer.key("statistics");
    writer.ch('{');
    writer.key("receiver");
    write_counter_map(writer, receiver.receiver_statistics);
    writer.ch(',');
    writer.key("stream");
    if (receiver.stream_statistics.present) {
        write_counter_map(writer, receiver.stream_statistics.value);
    } else {
        writer.null();
    }
    writer.ch('}');
    writer.ch(',');
    writer.key("last_error");
    write_error(writer, receiver.last_error);
    writer.ch('}');
}

void write_reader(Writer &writer, const StatusReaderView &reader)
{
    writer.ch('{');
    writer.key("descriptor");
    write_reader_descriptor(writer, reader.descriptor);
    writer.ch(',');
    writer.key("present");
    writer.boolean(reader.present);
    writer.ch(',');
    writer.key("card");
    writer.ch('{');
    writer.key("state");
    const char *card_state = card_state_name(reader.card.state);
    writer.str_literal(card_state, std::char_traits<char>::length(card_state));
    writer.ch(',');
    writer.key("sampled_at_ns");
    if (reader.card.sampled_at_ns.present) writer.quoted_u64(reader.card.sampled_at_ns.value);
    else writer.null();
    writer.ch(',');
    writer.key("reason");
    if (reader.card.state == 1 || reader.card.state == 2) writer.null();
    else {
        const char *reason = unknown_reason_name(reader.card.reason);
        writer.str_literal(reason, std::char_traits<char>::length(reason));
    }
    writer.ch('}');
    writer.ch(',');
    writer.key("operation");
    const char *operation = card_operation_name(reader.operation);
    writer.str_literal(operation, std::char_traits<char>::length(operation));
    writer.ch(',');
    writer.key("handles_open");
    writer.u64(reader.handles_open);
    writer.ch(',');
    writer.key("transaction_active");
    writer.boolean(reader.transaction_active);
    writer.ch(',');
    writer.key("last_error");
    write_error(writer, reader.last_error);
    writer.ch('}');
}

void emit(const StatusJsonInput &input, Writer &writer)
{
    const auto &status = input.status;
    writer.ch('{');
    writer.key("schema");
    writer.ch('{');
    writer.key("name");
    writer.str_literal("userland.status", 15);
    writer.ch(',');
    writer.key("major");
    writer.u64(1);
    writer.ch(',');
    writer.key("minor");
    writer.u64(0);
    writer.ch('}');
    writer.ch(',');
    writer.key("producer");
    writer.ch('{');
    writer.key("product");
    writer.text(input.producer.product);
    writer.ch(',');
    writer.key("version");
    writer.text(input.producer.version);
    writer.ch(',');
    writer.key("core_version");
    writer.text(input.producer.core_version);
    writer.ch('}');
    writer.ch(',');
    writer.key("context");
    write_context(writer, status.context);
    writer.ch(',');
    writer.key("snapshot");
    writer.ch('{');
    writer.key("sequence");
    writer.quoted_u64(status.snapshot.sequence);
    writer.ch(',');
    writer.key("observed_at_ns");
    writer.quoted_u64(status.snapshot.observed_at_ns);
    writer.ch('}');
    writer.ch(',');
    writer.key("scope");
    write_scope(writer, status.scope);
    writer.ch(',');
    writer.key("enclosure");
    write_enclosure(writer, status.enclosure, status.lnb_15v_enabled, status.card_inserted);
    writer.ch(',');
    writer.key("topology");
    write_topology(writer, status.topology);
    if (status.functions.present) {
        writer.ch(',');
        writer.key("usb_functions");
        writer.ch('[');
        for (std::size_t i = 0; i < status.functions.count; ++i) {
            if (i != 0) writer.ch(',');
            write_usb_function(writer, status.functions.items[i]);
        }
        writer.ch(']');
    }
    if (status.receivers.present) {
        writer.ch(',');
        writer.key("receivers");
        writer.ch('[');
        for (std::size_t i = 0; i < status.receivers.count; ++i) {
            if (i != 0) writer.ch(',');
            write_receiver(writer, status.receivers.items[i]);
        }
        writer.ch(']');
    }
    if (status.readers.present) {
        writer.ch(',');
        writer.key("readers");
        writer.ch('[');
        for (std::size_t i = 0; i < status.readers.count; ++i) {
            if (i != 0) writer.ch(',');
            write_reader(writer, status.readers.items[i]);
        }
        writer.ch(']');
    }
    writer.ch('}');
    writer.ch('\n');
}

}

StatusJsonCode status_json_size(const StatusJsonInput *input, std::uint64_t *bytes) noexcept
{
    if (input == nullptr || bytes == nullptr) return StatusJsonCode::invalid_argument;
    if (!status_ok(*input)) return StatusJsonCode::invalid_argument;
    Writer writer(nullptr, 0);
    emit(*input, writer);
    if (writer.overflow()) return StatusJsonCode::invalid_argument;
    *bytes = writer.count();
    return StatusJsonCode::ok;
}

StatusJsonCode encode_status_json(const StatusJsonInput *input, dtv_buffer *output) noexcept
{
    if (input == nullptr || output == nullptr ||
        (output->capacity != 0 && output->data == nullptr)) {
        return StatusJsonCode::invalid_argument;
    }
    if (!status_ok(*input)) return StatusJsonCode::invalid_argument;
    Writer counter(nullptr, 0);
    emit(*input, counter);
    if (counter.overflow()) return StatusJsonCode::invalid_argument;
    if (static_cast<std::uint64_t>(output->capacity) < counter.count()) {
        return StatusJsonCode::buffer_too_small;
    }
    Writer writer(output->data, static_cast<std::size_t>(output->capacity));
    emit(*input, writer);
    if (writer.overflow()) return StatusJsonCode::buffer_too_small;
    output->size = counter.count();
    return StatusJsonCode::ok;
}

}
