// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include <dtv/internal/discovery-json.hpp>

namespace {

using dtv::internal::DiscoveryJsonCandidate;
using dtv::internal::DiscoveryJsonCode;
using dtv::internal::DiscoveryJsonDevice;
using dtv::internal::DiscoveryJsonEnclosure;
using dtv::internal::DiscoveryJsonInput;
using dtv::internal::DiscoveryJsonReceiver;
using dtv::internal::DiscoveryJsonUngrouped;
using dtv::internal::encode_discovery_json;
using dtv::internal::discovery_json_size;

int check(bool value, int code) { return value ? 0 : code; }

bool encode_matches(const DiscoveryJsonInput &input, const char *expected)
{
    std::array<std::uint8_t, 4096> buffer{};
    dtv_buffer output{buffer.data(), buffer.size(), 0};
    if (encode_discovery_json(&input, &output) != DiscoveryJsonCode::ok) return false;
    const auto expected_size = std::strlen(expected);
    if (output.size != expected_size + 1) return false;
    if (std::memcmp(buffer.data(), expected, expected_size) != 0) return false;
    return buffer[expected_size] == '\n';
}

}

int main()
{
    DiscoveryJsonInput empty{};
    empty.product = "fixture";
    empty.version = "1.2.3";
    empty.complete = true;
    const char *empty_json =
        R"({"schema":{"name":"userland.discovery","major":1,"minor":0},"producer":{"product":"fixture","version":"1.2.3"},"scan":{"complete":true,"issues":[]},"enclosures":[],"ungrouped_usb_devices":[]})";
    {
        std::uint64_t bytes = 0;
        if (int failed = check(discovery_json_size(&empty, &bytes) == DiscoveryJsonCode::ok &&
                                   bytes == std::strlen(empty_json) + 1,
                               1)) return failed;
        if (int failed = check(encode_matches(empty, empty_json), 2)) return failed;
    }

    std::uint32_t bus = 1;
    std::uint32_t address = 6;
    std::uint32_t ungrouped_bus = 1;
    std::uint32_t ungrouped_address = 7;
    DiscoveryJsonReceiver receiver{};
    receiver.receiver = 3;
    receiver.device = 2;
    receiver.local = 0;
    receiver.systems = (1u << 1) | (1u << 2);
    receiver.ts_scrambling = 1;
    DiscoveryJsonDevice device{};
    device.id = 2;
    device.vid = 0x0511;
    device.pid = 0x084a;
    device.serial = "usb-serial";
    device.location.bus = &bus;
    device.location.address = &address;
    device.location.port = "1-2.3";
    device.receivers_known = true;
    device.receivers = &receiver;
    device.receiver_count = 1;
    DiscoveryJsonCandidate candidate{};
    candidate.device = 9;
    DiscoveryJsonEnclosure enclosure{};
    enclosure.id = 1;
    enclosure.model = "PX-Q3U4";
    enclosure.serial = "serial-value";
    enclosure.serial_unique = 2;
    enclosure.status = 1;
    enclosure.lnb_15v_supported = 2;
    enclosure.card_reader_supported = 2;
    enclosure.has_card_reader_device = true;
    enclosure.card_reader_device = 2;
    enclosure.devices = &device;
    enclosure.device_count = 1;
    enclosure.candidates = &candidate;
    enclosure.candidate_count = 1;
    DiscoveryJsonUngrouped ungrouped{};
    ungrouped.serial = nullptr;
    ungrouped.model = "PX";
    ungrouped.vid = 0x1234;
    ungrouped.pid = 0x0001;
    ungrouped.status = 1;
    ungrouped.location.bus = &ungrouped_bus;
    ungrouped.location.address = &ungrouped_address;
    ungrouped.location.port = "1-1.7";
    DiscoveryJsonInput nonempty{};
    nonempty.product = "fixture";
    nonempty.version = "1.2.3";
    nonempty.complete = true;
    nonempty.enclosures = &enclosure;
    nonempty.enclosure_count = 1;
    nonempty.ungrouped = &ungrouped;
    nonempty.ungrouped_count = 1;
    const char *nonempty_json =
        R"({"schema":{"name":"userland.discovery","major":1,"minor":0},"producer":{"product":"fixture","version":"1.2.3"},"scan":{"complete":true,"issues":[]},"enclosures":[{"id":1,"model":"PX-Q3U4","serial":"serial-value","serial_unique":true,"status":"ready","lnb_15v_supported":true,"card_reader_supported":true,"card_reader_device":2,"devices":[{"id":2,"usb":"0511:084a","serial":"usb-serial","bus":1,"address":6,"port":"1-2.3","receivers":[{"receiver":3,"device":2,"local":0,"systems":["ISDB-T","ISDB-S"],"ts_scrambling":"scrambled"}]}],"candidates":[{"device":9,"serial":null,"bus":null,"address":null,"port":null}]}],"ungrouped_usb_devices":[{"serial":null,"model":"PX","usb":"1234:0001","status":"usable","bus":1,"address":7,"port":"1-1.7"}]})";
    if (int failed = check(encode_matches(nonempty, nonempty_json), 3)) return failed;

    {
        auto value = nonempty;
        auto changed_receiver = receiver;
        changed_receiver.systems = (1u << 2);
        changed_receiver.ts_scrambling = 0;
        auto changed_device = device;
        changed_device.serial = nullptr;
        changed_device.location.bus = nullptr;
        changed_device.location.address = nullptr;
        changed_device.location.port = nullptr;
        changed_device.receivers = &changed_receiver;
        changed_device.receiver_count = 1;
        auto changed_enclosure = enclosure;
        changed_enclosure.serial = nullptr;
        changed_enclosure.serial_unique = 0;
        changed_enclosure.lnb_15v_supported = 0;
        changed_enclosure.card_reader_supported = 1;
        changed_enclosure.has_card_reader_device = false;
        changed_enclosure.devices = &changed_device;
        auto changed_candidate = candidate;
        changed_candidate.serial = nullptr;
        changed_enclosure.candidates = &changed_candidate;
        auto changed_ungrouped = ungrouped;
        changed_ungrouped.model = nullptr;
        changed_ungrouped.status = 0;
        changed_ungrouped.location.bus = nullptr;
        changed_ungrouped.location.address = nullptr;
        changed_ungrouped.location.port = nullptr;
        value.enclosures = &changed_enclosure;
        value.ungrouped = &changed_ungrouped;
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_discovery_json(&value, &output) ==
                                   DiscoveryJsonCode::ok,
                               4)) return failed;
        const std::string encoded(reinterpret_cast<char *>(buffer.data()), output.size);
        if (int failed = check(
                encoded.find("\"serial_unique\":null") != std::string::npos &&
                    encoded.find("\"card_reader_supported\":false") != std::string::npos &&
                    encoded.find("\"card_reader_device\":null") != std::string::npos &&
                    encoded.find("\"systems\":[\"ISDB-S\"]") != std::string::npos &&
                    encoded.find("\"ts_scrambling\":null") != std::string::npos &&
                    encoded.find("\"status\":\"unknown\"") != std::string::npos &&
                    encoded.find("\"bus\":null") != std::string::npos,
                5)) return failed;
    }

    {
        auto value = nonempty;
        auto changed_device = device;
        changed_device.receivers_known = false;
        changed_device.receivers = nullptr;
        changed_device.receiver_count = 0;
        auto changed_enclosure = enclosure;
        changed_enclosure.devices = &changed_device;
        value.enclosures = &changed_enclosure;
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_discovery_json(&value, &output) ==
                                   DiscoveryJsonCode::ok &&
                               std::string(reinterpret_cast<char *>(buffer.data()), output.size)
                                       .find("\"receivers\"") == std::string::npos,
                               6)) return failed;

        changed_device.receivers_known = true;
        changed_device.receivers = nullptr;
        changed_device.receiver_count = 0;
        changed_enclosure.devices = &changed_device;
        value.enclosures = &changed_enclosure;
        dtv_buffer empty_output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_discovery_json(&value, &empty_output) ==
                                   DiscoveryJsonCode::ok &&
                               std::string(reinterpret_cast<char *>(buffer.data()),
                                           empty_output.size)
                                       .find("\"receivers\":[]") != std::string::npos,
                               7)) return failed;
    }

    {
        std::uint32_t maximum = 0xffffffffu;
        auto value = nonempty;
        auto max_device = device;
        max_device.location.bus = &maximum;
        max_device.location.address = &maximum;
        auto max_enclosure = enclosure;
        max_enclosure.devices = &max_device;
        value.enclosures = &max_enclosure;
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_discovery_json(&value, &output) ==
                                   DiscoveryJsonCode::ok &&
                               std::string(reinterpret_cast<char *>(buffer.data()),
                                           output.size)
                                       .find("\"bus\":4294967295,\"address\":4294967295") !=
                                   std::string::npos,
                               19)) return failed;
    }

    {
        auto value = nonempty;
        const char escaped[] = "a\"b\\c\n\x01";
        auto escaped_enclosure = enclosure;
        escaped_enclosure.model = escaped;
        value.enclosures = &escaped_enclosure;
        std::array<std::uint8_t, 4096> buffer{};
        dtv_buffer output{buffer.data(), buffer.size(), 0};
        if (int failed = check(encode_discovery_json(&value, &output) ==
                                   DiscoveryJsonCode::ok,
                               14)) return failed;
        const std::string encoded(reinterpret_cast<char *>(buffer.data()), output.size);
        if (int failed = check(
                encoded.find("\"model\":\"a\\\"b\\\\c\\n\\u0001\"") !=
                    std::string::npos,
                15)) return failed;
    }

    {
        auto value = nonempty;
        const char invalid[] = {static_cast<char>(0xff)};
        auto changed_enclosure = enclosure;
        changed_enclosure.model = invalid;
        value.enclosures = &changed_enclosure;
        std::uint64_t bytes = 0;
        if (int failed = check(discovery_json_size(&value, &bytes) ==
                                   DiscoveryJsonCode::invalid_argument,
                               8)) return failed;
        value = nonempty;
        auto bad_systems = receiver;
        bad_systems.systems = 1;
        auto bad_device = device;
        bad_device.receivers = &bad_systems;
        auto bad_enclosure = enclosure;
        bad_enclosure.devices = &bad_device;
        value.enclosures = &bad_enclosure;
        if (int failed = check(discovery_json_size(&value, &bytes) ==
                                   DiscoveryJsonCode::invalid_argument,
                               9)) return failed;
        value = nonempty;
        auto duplicate = device;
        duplicate.id = 2;
        std::array<DiscoveryJsonDevice, 2> duplicate_devices{{device, duplicate}};
        auto dup_enclosure = enclosure;
        dup_enclosure.devices = duplicate_devices.data();
        dup_enclosure.device_count = 2;
        value.enclosures = &dup_enclosure;
        if (int failed = check(discovery_json_size(&value, &bytes) ==
                                   DiscoveryJsonCode::invalid_argument,
                               10)) return failed;
        value = nonempty;
        auto missing_card = enclosure;
        missing_card.card_reader_device = 99;
        value.enclosures = &missing_card;
        if (int failed = check(discovery_json_size(&value, &bytes) ==
                                   DiscoveryJsonCode::invalid_argument,
                               11)) return failed;
        value = nonempty;
        auto bad_unique = enclosure;
        bad_unique.serial_unique = 3;
        value.enclosures = &bad_unique;
        if (int failed = check(discovery_json_size(&value, &bytes) ==
                                   DiscoveryJsonCode::invalid_argument,
                               16)) return failed;
        value = nonempty;
        auto bad_status = enclosure;
        bad_status.status = 2;
        value.enclosures = &bad_status;
        if (int failed = check(discovery_json_size(&value, &bytes) ==
                                   DiscoveryJsonCode::invalid_argument,
                               17)) return failed;
        value = nonempty;
        auto bad_ungrouped = ungrouped;
        bad_ungrouped.status = 7;
        value.ungrouped = &bad_ungrouped;
        if (int failed = check(discovery_json_size(&value, &bytes) ==
                                   DiscoveryJsonCode::invalid_argument,
                               18)) return failed;
    }

    {
        std::array<std::uint8_t, 4096> buffer{};
        std::fill(buffer.begin(), buffer.end(), 0xaa);
        dtv_buffer output{buffer.data(), 10, 777};
        if (int failed = check(encode_discovery_json(&nonempty, &output) ==
                                   DiscoveryJsonCode::buffer_too_small &&
                               output.size == 777 &&
                               std::all_of(buffer.begin(), buffer.end(),
                                           [](std::uint8_t byte) { return byte == 0xaa; }),
                               12)) return failed;
        if (int failed = check(encode_discovery_json(nullptr, &output) ==
                                   DiscoveryJsonCode::invalid_argument &&
                               discovery_json_size(nullptr, nullptr) ==
                                   DiscoveryJsonCode::invalid_argument,
                               13)) return failed;
    }

    return 0;
}
