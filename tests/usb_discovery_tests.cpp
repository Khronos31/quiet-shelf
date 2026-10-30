// SPDX-License-Identifier: GPL-2.0-only
#include "usb/discovery.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

namespace wire = dtv::userland::wire;
using torne::usb::RawDevice;
using torne::usb::ScanStorage;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

template <std::size_t N>
std::string text(const wire::WString<N>& value)
{
    return std::string(value.bytes.data(), value.length);
}

RawDevice make_device(std::uint8_t bus, std::uint8_t address,
                      std::initializer_list<std::uint8_t> ports,
                      std::uint16_t vid = 0x054cU,
                      std::uint16_t pid = 0x04b2U)
{
    RawDevice device;
    device.vid = vid;
    device.pid = pid;
    device.bus = bus;
    device.address = address;
    device.port_count = static_cast<std::uint8_t>(ports.size());
    std::size_t index = 0U;
    for (const std::uint8_t port : ports) device.ports[index++] = port;
    device.descriptor_ok = true;
    return device;
}

void test_empty()
{
    ScanStorage storage;
    const auto scan = torne::usb::build_scan(nullptr, 0U, storage);
    CHECK(scan.has_value());
    CHECK(scan.value().complete);
    CHECK(scan.value().enclosure_count == 0U);
    CHECK(scan.value().issue_count == 0U);
    CHECK(text(scan.value().product) == "torne-userland");
    CHECK(!text(scan.value().version).empty());
}

void test_ignores_other_products_and_reports_descriptor_failure()
{
    RawDevice devices[3] = {
        make_device(1U, 4U, {2U}, 0x1234U, 0x5678U),
        make_device(1U, 5U, {3U}),
        RawDevice{},
    };
    devices[2].descriptor_ok = false;
    ScanStorage storage;
    const auto scan = torne::usb::build_scan(devices, 3U, storage);
    CHECK(scan.has_value());
    CHECK(!scan.value().complete);
    CHECK(scan.value().enclosure_count == 1U);
    CHECK(scan.value().issue_count == 1U);
    CHECK(text(scan.value().issues[0].code) == "descriptor_unavailable");
    CHECK(text(scan.value().enclosures[0].devices[0].location.port.value) == "1-3");
}

void test_single_device()
{
    const RawDevice device = make_device(1U, 111U, {2U});
    ScanStorage storage;
    const auto scan = torne::usb::build_scan(&device, 1U, storage);
    CHECK(scan.has_value());
    CHECK(scan.value().complete);
    CHECK(scan.value().enclosure_count == 1U);
    const auto& enclosure = scan.value().enclosures[0];
    CHECK(enclosure.status == wire::DiscoveryEnclosureStatus::ready);
    CHECK(text(enclosure.model.value) == "CECH-ZD1 J");
    CHECK(!enclosure.serial.present);
    CHECK(enclosure.lnb_15v_supported.present);
    CHECK(!enclosure.lnb_15v_supported.value);
    CHECK(enclosure.readers_known);
    CHECK(enclosure.reader_count == 1U);
    CHECK(enclosure.readers[0].reader == 0U);
    CHECK(enclosure.devices[0].vid == 0x054cU);
    CHECK(enclosure.devices[0].pid == 0x04b2U);
    CHECK(!enclosure.devices[0].serial.present);
    CHECK(enclosure.devices[0].receivers_known);
    CHECK(enclosure.devices[0].receivers[0].systems.mask == 0x01U);
    CHECK(text(enclosure.devices[0].location.port.value) == "1-2");
    CHECK(enclosure.devices[0].location.address.value == 111U);
}

void test_multiple_devices_use_distinct_paths()
{
    const RawDevice devices[2] = {
        make_device(1U, 7U, {2U}),
        make_device(1U, 8U, {4U, 2U}),
    };
    ScanStorage storage;
    const auto scan = torne::usb::build_scan(devices, 2U, storage);
    CHECK(scan.has_value());
    CHECK(scan.value().complete);
    CHECK(scan.value().enclosure_count == 2U);
    CHECK(scan.value().enclosures[0].enclosure != scan.value().enclosures[1].enclosure);
    CHECK(text(scan.value().enclosures[0].devices[0].location.port.value) == "1-2");
    CHECK(text(scan.value().enclosures[1].devices[0].location.port.value) == "1-4.2");
}

void test_invalid_topology_is_not_invented()
{
    RawDevice device = make_device(1U, 9U, {});
    ScanStorage storage;
    const auto scan = torne::usb::build_scan(&device, 1U, storage);
    CHECK(scan.has_value());
    CHECK(!scan.value().complete);
    CHECK(scan.value().enclosure_count == 0U);
    CHECK(scan.value().issue_count == 1U);
    CHECK(text(scan.value().issues[0].code) == "invalid_topology");
    CHECK(scan.value().ungrouped_usb_device_count == 1U);
    CHECK(scan.value().ungrouped_usb_devices[0].status ==
          wire::DiscoveryObservationStatus::invalid_topology);
    CHECK(!scan.value().ungrouped_usb_devices[0].location.port.present);
}

void test_path_format()
{
    wire::WString<64> path;
    const std::uint8_t ports[2] = {4U, 2U};
    CHECK(torne::usb::format_usb_path(1U, ports, 2U, path));
    CHECK(text(path) == "1-4.2");
    CHECK(!torne::usb::format_usb_path(1U, ports, 0U, path));
}

}  // namespace

int main()
{
    test_empty();
    test_ignores_other_products_and_reports_descriptor_failure();
    test_single_device();
    test_multiple_devices_use_distinct_paths();
    test_invalid_topology_is_not_invented();
    test_path_format();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
