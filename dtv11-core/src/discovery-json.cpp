// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/discovery-json.hpp>

#include <algorithm>
#include <limits>
#include <string>

namespace dtv::internal {
namespace {

constexpr std::uint32_t kSystemsKnownMask = (1u << 1) | (1u << 2);

bool utf8_ok(const char *data)
{
    if (data == nullptr) return false;
    const auto size = std::char_traits<char>::length(data);
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

bool text_ok(const char *value, bool required, bool nonempty)
{
    if (value == nullptr) return !required;
    if (!utf8_ok(value)) return false;
    return !nonempty || value[0] != '\0';
}

bool location_ok(const DiscoveryJsonLocation &location)
{
    return text_ok(location.port, false, false);
}

bool systems_ok(std::uint32_t systems)
{
    return systems != 0 && (systems & ~kSystemsKnownMask) == 0;
}

bool receiver_ok(const DiscoveryJsonReceiver &receiver)
{
    return systems_ok(receiver.systems) && receiver.ts_scrambling <= 2;
}

bool device_ok(const DiscoveryJsonDevice &device, std::size_t device_index,
               const DiscoveryJsonDevice *devices, std::size_t device_count)
{
    if (!text_ok(device.serial, false, false) || !location_ok(device.location)) {
        return false;
    }
    if (device.receivers_known) {
        if (device.receiver_count != 0 && device.receivers == nullptr) return false;
        for (std::size_t i = 0; i < device.receiver_count; ++i) {
            if (device.receivers[i].device != device.id) return false;
            if (!receiver_ok(device.receivers[i])) return false;
            for (std::size_t j = 0; j < i; ++j) {
                if (device.receivers[j].receiver == device.receivers[i].receiver) {
                    return false;
                }
            }
        }
    } else if (device.receiver_count != 0 || device.receivers != nullptr) {
        return false;
    }
    (void)device_index;
    (void)devices;
    (void)device_count;
    return true;
}

bool enclosure_ok(const DiscoveryJsonEnclosure &enclosure)
{
    if (!text_ok(enclosure.model, false, false) ||
        !text_ok(enclosure.serial, false, false)) {
        return false;
    }
    if (enclosure.serial_unique > 2) return false;
    if (enclosure.status != 0 && enclosure.status != 1 &&
        enclosure.status != 3 && enclosure.status != 4) {
        return false;
    }
    if (enclosure.lnb_15v_supported > 2 || enclosure.card_reader_supported > 2) {
        return false;
    }
    if (enclosure.device_count != 0 && enclosure.devices == nullptr) return false;
    if (enclosure.candidate_count != 0 && enclosure.candidates == nullptr) return false;
    bool card_device_found = false;
    for (std::size_t i = 0; i < enclosure.device_count; ++i) {
        const auto &device = enclosure.devices[i];
        if (!device_ok(device, i, enclosure.devices, enclosure.device_count)) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (enclosure.devices[j].id == device.id) return false;
        }
        if (enclosure.has_card_reader_device && device.id == enclosure.card_reader_device) {
            card_device_found = true;
        }
    }
    if (enclosure.has_card_reader_device && !card_device_found) return false;
    for (std::size_t device_index = 0; device_index < enclosure.device_count; ++device_index) {
        const auto &device = enclosure.devices[device_index];
        if (!device.receivers_known) continue;
        for (std::size_t receiver_index = 0; receiver_index < device.receiver_count;
             ++receiver_index) {
            const auto receiver_id = device.receivers[receiver_index].receiver;
            for (std::size_t previous_device = 0; previous_device < device_index;
                 ++previous_device) {
                const auto &previous = enclosure.devices[previous_device];
                if (!previous.receivers_known) continue;
                for (std::size_t previous_receiver = 0;
                     previous_receiver < previous.receiver_count; ++previous_receiver) {
                    if (previous.receivers[previous_receiver].receiver == receiver_id) {
                        return false;
                    }
                }
            }
        }
    }
    for (std::size_t i = 0; i < enclosure.candidate_count; ++i) {
        const auto &candidate = enclosure.candidates[i];
        if (!text_ok(candidate.serial, false, false) ||
            !location_ok(candidate.location)) {
            return false;
        }
    }
    return true;
}

bool input_ok(const DiscoveryJsonInput &input)
{
    if (!text_ok(input.product, true, true) ||
        !text_ok(input.version, true, true)) {
        return false;
    }
    if (input.enclosure_count != 0 && input.enclosures == nullptr) return false;
    if (input.ungrouped_count != 0 && input.ungrouped == nullptr) return false;
    for (std::size_t i = 0; i < input.enclosure_count; ++i) {
        if (!enclosure_ok(input.enclosures[i])) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (input.enclosures[j].id == input.enclosures[i].id) return false;
        }
    }
    for (std::size_t i = 0; i < input.ungrouped_count; ++i) {
        const auto &ungrouped = input.ungrouped[i];
        if (!text_ok(ungrouped.serial, false, false) ||
            !text_ok(ungrouped.model, false, false) ||
            !location_ok(ungrouped.location) || ungrouped.status > 6) {
            return false;
        }
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
        str_literal(name);
        ch(':');
    }

    void str_literal(const char *data)
    {
        const auto size = std::char_traits<char>::length(data);
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

    void null() { raw("null", 4); }
    void boolean(bool value) { raw(value ? "true" : "false", value ? 4 : 5); }

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

    void text(const char *value)
    {
        if (value == nullptr) null();
        else str_literal(value);
    }

    void tribool(std::uint32_t value)
    {
        if (value == 2) boolean(true);
        else if (value == 1) boolean(false);
        else null();
    }

    void usb(std::uint16_t vid, std::uint16_t pid)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        ch('"');
        for (int shift = 12; shift >= 0; shift -= 4) ch(kHex[(vid >> shift) & 0x0f]);
        ch(':');
        for (int shift = 12; shift >= 0; shift -= 4) ch(kHex[(pid >> shift) & 0x0f]);
        ch('"');
    }

private:
    std::uint8_t *output_;
    std::size_t capacity_;
    std::uint64_t count_ = 0;
    bool overflow_ = false;
};

const char *enclosure_status_name(std::uint32_t status)
{
    switch (status) {
    case 1: return "ready";
    case 3: return "duplicate";
    case 4: return "invalid_observation";
    default: return "unknown";
    }
}

const char *ungrouped_status_name(std::uint32_t status)
{
    switch (status) {
    case 1: return "usable";
    case 2: return "unsupported";
    case 3: return "invalid_serial";
    case 4: return "insufficient_speed";
    case 5: return "invalid_topology";
    case 6: return "open_failed";
    default: return "unknown";
    }
}

void write_location(Writer &writer, const DiscoveryJsonLocation &location)
{
    writer.key("bus");
    if (location.bus != nullptr) writer.u64(*location.bus);
    else writer.null();
    writer.ch(',');
    writer.key("address");
    if (location.address != nullptr) writer.u64(*location.address);
    else writer.null();
    writer.ch(',');
    writer.key("port");
    writer.text(location.port);
}

void write_receiver(Writer &writer, const DiscoveryJsonReceiver &receiver)
{
    writer.ch('{');
    writer.key("receiver");
    writer.u64(receiver.receiver);
    writer.ch(',');
    writer.key("device");
    writer.u64(receiver.device);
    writer.ch(',');
    writer.key("local");
    writer.u64(receiver.local);
    writer.ch(',');
    writer.key("systems");
    writer.ch('[');
    bool first = true;
    if ((receiver.systems & (1u << 1)) != 0) {
        writer.str_literal("ISDB-T");
        first = false;
    }
    if ((receiver.systems & (1u << 2)) != 0) {
        if (!first) writer.ch(',');
        writer.str_literal("ISDB-S");
    }
    writer.ch(']');
    writer.ch(',');
    writer.key("ts_scrambling");
    if (receiver.ts_scrambling == 1) writer.str_literal("scrambled");
    else if (receiver.ts_scrambling == 2) writer.str_literal("descrambled");
    else writer.null();
    writer.ch('}');
}

void write_device(Writer &writer, const DiscoveryJsonDevice &device)
{
    writer.ch('{');
    writer.key("id");
    writer.u64(device.id);
    writer.ch(',');
    writer.key("usb");
    writer.usb(device.vid, device.pid);
    writer.ch(',');
    writer.key("serial");
    writer.text(device.serial);
    writer.ch(',');
    write_location(writer, device.location);
    if (device.receivers_known) {
        writer.ch(',');
        writer.key("receivers");
        writer.ch('[');
        for (std::size_t i = 0; i < device.receiver_count; ++i) {
            if (i != 0) writer.ch(',');
            write_receiver(writer, device.receivers[i]);
        }
        writer.ch(']');
    }
    writer.ch('}');
}

void write_candidate(Writer &writer, const DiscoveryJsonCandidate &candidate)
{
    writer.ch('{');
    writer.key("device");
    writer.u64(candidate.device);
    writer.ch(',');
    writer.key("serial");
    writer.text(candidate.serial);
    writer.ch(',');
    write_location(writer, candidate.location);
    writer.ch('}');
}

void write_enclosure(Writer &writer, const DiscoveryJsonEnclosure &enclosure)
{
    writer.ch('{');
    writer.key("id");
    writer.u64(enclosure.id);
    writer.ch(',');
    writer.key("model");
    writer.text(enclosure.model);
    writer.ch(',');
    writer.key("serial");
    writer.text(enclosure.serial);
    writer.ch(',');
    writer.key("serial_unique");
    writer.tribool(enclosure.serial_unique);
    writer.ch(',');
    writer.key("status");
    writer.str_literal(enclosure_status_name(enclosure.status));
    writer.ch(',');
    writer.key("lnb_15v_supported");
    writer.tribool(enclosure.lnb_15v_supported);
    writer.ch(',');
    writer.key("card_reader_supported");
    writer.tribool(enclosure.card_reader_supported);
    writer.ch(',');
    writer.key("card_reader_device");
    if (enclosure.has_card_reader_device) writer.u64(enclosure.card_reader_device);
    else writer.null();
    writer.ch(',');
    writer.key("devices");
    writer.ch('[');
    for (std::size_t i = 0; i < enclosure.device_count; ++i) {
        if (i != 0) writer.ch(',');
        write_device(writer, enclosure.devices[i]);
    }
    writer.ch(']');
    writer.ch(',');
    writer.key("candidates");
    writer.ch('[');
    for (std::size_t i = 0; i < enclosure.candidate_count; ++i) {
        if (i != 0) writer.ch(',');
        write_candidate(writer, enclosure.candidates[i]);
    }
    writer.ch(']');
    writer.ch('}');
}

void write_ungrouped(Writer &writer, const DiscoveryJsonUngrouped &ungrouped)
{
    writer.ch('{');
    writer.key("serial");
    writer.text(ungrouped.serial);
    writer.ch(',');
    writer.key("model");
    writer.text(ungrouped.model);
    writer.ch(',');
    writer.key("usb");
    writer.usb(ungrouped.vid, ungrouped.pid);
    writer.ch(',');
    writer.key("status");
    writer.str_literal(ungrouped_status_name(ungrouped.status));
    writer.ch(',');
    write_location(writer, ungrouped.location);
    writer.ch('}');
}

void emit(const DiscoveryJsonInput &input, Writer &writer)
{
    writer.ch('{');
    writer.key("schema");
    writer.ch('{');
    writer.key("name");
    writer.str_literal("userland.discovery");
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
    writer.text(input.product);
    writer.ch(',');
    writer.key("version");
    writer.text(input.version);
    writer.ch('}');
    writer.ch(',');
    writer.key("scan");
    writer.ch('{');
    writer.key("complete");
    writer.boolean(input.complete);
    writer.ch(',');
    writer.key("issues");
    writer.ch('[');
    writer.ch(']');
    writer.ch('}');
    writer.ch(',');
    writer.key("enclosures");
    writer.ch('[');
    for (std::size_t i = 0; i < input.enclosure_count; ++i) {
        if (i != 0) writer.ch(',');
        write_enclosure(writer, input.enclosures[i]);
    }
    writer.ch(']');
    writer.ch(',');
    writer.key("ungrouped_usb_devices");
    writer.ch('[');
    for (std::size_t i = 0; i < input.ungrouped_count; ++i) {
        if (i != 0) writer.ch(',');
        write_ungrouped(writer, input.ungrouped[i]);
    }
    writer.ch(']');
    writer.ch('}');
    writer.ch('\n');
}

}

DiscoveryJsonCode discovery_json_size(const DiscoveryJsonInput *input,
                                      std::uint64_t *bytes) noexcept
{
    if (input == nullptr || bytes == nullptr) return DiscoveryJsonCode::invalid_argument;
    if (!input_ok(*input)) return DiscoveryJsonCode::invalid_argument;
    Writer writer(nullptr, 0);
    emit(*input, writer);
    if (writer.overflow()) return DiscoveryJsonCode::invalid_argument;
    *bytes = writer.count();
    return DiscoveryJsonCode::ok;
}

DiscoveryJsonCode encode_discovery_json(const DiscoveryJsonInput *input,
                                        dtv_buffer *output) noexcept
{
    if (input == nullptr || output == nullptr ||
        (output->capacity != 0 && output->data == nullptr)) {
        return DiscoveryJsonCode::invalid_argument;
    }
    if (!input_ok(*input)) return DiscoveryJsonCode::invalid_argument;
    Writer counter(nullptr, 0);
    emit(*input, counter);
    if (counter.overflow()) return DiscoveryJsonCode::invalid_argument;
    if (static_cast<std::uint64_t>(output->capacity) < counter.count()) {
        return DiscoveryJsonCode::buffer_too_small;
    }
    Writer writer(output->data, static_cast<std::size_t>(output->capacity));
    emit(*input, writer);
    if (writer.overflow()) return DiscoveryJsonCode::buffer_too_small;
    output->size = counter.count();
    return DiscoveryJsonCode::ok;
}

}
