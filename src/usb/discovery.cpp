// SPDX-License-Identifier: GPL-2.0-only
#include "usb/discovery.h"

#include <libusb.h>

#include <cstring>
#include <string_view>

namespace torne::usb {
namespace {

namespace wire = dtv::userland::wire;
using dtv::userland::Error;
using dtv::userland::Result;

constexpr std::uint16_t kSonyVendorId = 0x054cU;
constexpr std::uint16_t kCechZd1ProductId = 0x04b2U;
constexpr const char* kProductSlug = "torne-userland";
constexpr const char* kModelName = "CECH-ZD1 J";

template <std::size_t N>
bool set_text(wire::WString<N>& output, std::string_view value) noexcept
{
    if (value.size() > N) return false;
    output.length = static_cast<std::uint16_t>(value.size());
    if (!value.empty()) std::memcpy(output.bytes.data(), value.data(), value.size());
    return true;
}

void add_issue(ScanStorage& storage, const char* code, const char* detail) noexcept
{
    wire::DiscoveryScanIssue issue;
    (void)set_text(issue.code, code);
    (void)set_text(issue.detail, detail);
    storage.issues.push_back(issue);
}

void set_location(const RawDevice& device, const wire::WString<64>& path,
                  wire::DiscoveryUsbLocation& location) noexcept
{
    location.bus.present = true;
    location.bus.value = device.bus;
    location.address.present = true;
    location.address.value = device.address;
    location.port.present = true;
    location.port.value = path;
}

void add_invalid_topology(ScanStorage& storage, const RawDevice& device) noexcept
{
    add_issue(storage, "invalid_topology",
              "a matching device has no usable USB bus-port topology");
    wire::DiscoveryUsbObservation observation;
    observation.vid = device.vid;
    observation.pid = device.pid;
    observation.status = wire::DiscoveryObservationStatus::invalid_topology;
    observation.location.bus.present = true;
    observation.location.bus.value = device.bus;
    observation.location.address.present = true;
    observation.location.address.value = device.address;
    storage.ungrouped.push_back(observation);
}

struct MatchedDevice final {
    RawDevice device;
    wire::WString<64> path;
};

}  // namespace

bool is_torne_device(std::uint16_t vid, std::uint16_t pid) noexcept
{
    return vid == kSonyVendorId && pid == kCechZd1ProductId;
}

bool format_usb_path(std::uint8_t bus, const std::uint8_t* ports,
                     std::size_t port_count,
                     wire::WString<64>& output) noexcept
{
    if (port_count == 0U || port_count > 7U || ports == nullptr) return false;
    std::array<char, 64U> text{};
    std::size_t used = 0U;
    const auto append_number = [&](std::uint32_t value) noexcept {
        char digits[4] = {};
        std::size_t count = 0U;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        if (used + count > text.size()) return false;
        while (count != 0U) text[used++] = digits[--count];
        return true;
    };
    if (!append_number(bus)) return false;
    for (std::size_t index = 0U; index < port_count; ++index) {
        if (used + 1U > text.size()) return false;
        text[used++] = index == 0U ? '-' : '.';
        if (!append_number(ports[index])) return false;
    }
    return set_text(output, std::string_view(text.data(), used));
}

Result<wire::DiscoveryScan> build_scan(const RawDevice* devices,
                                       std::size_t count,
                                       ScanStorage& storage) noexcept
{
    storage.enclosures.clear();
    storage.devices.clear();
    storage.receivers.clear();
    storage.readers.clear();
    storage.ungrouped.clear();
    storage.issues.clear();

    bool complete = true;
    std::vector<MatchedDevice> matched;
    for (std::size_t index = 0U; index < count; ++index) {
        const RawDevice& device = devices[index];
        if (!device.descriptor_ok) {
            add_issue(storage, "descriptor_unavailable",
                      "a USB device descriptor could not be read");
            complete = false;
            continue;
        }
        if (!is_torne_device(device.vid, device.pid)) continue;
        wire::WString<64> path;
        if (!format_usb_path(device.bus, device.ports.data(), device.port_count,
                             path)) {
            add_invalid_topology(storage, device);
            complete = false;
            continue;
        }
        matched.push_back(MatchedDevice{device, path});
    }
    if (matched.size() > 0xffffU) {
        add_issue(storage, "too_many_devices",
                  "the scan contains more devices than the schema can number");
        complete = false;
        matched.resize(0xffffU);
    }

    storage.enclosures.resize(matched.size());
    storage.devices.resize(matched.size());
    storage.receivers.resize(matched.size());
    storage.readers.resize(matched.size());
    for (std::size_t index = 0U; index < matched.size(); ++index) {
        const MatchedDevice& entry = matched[index];
        const auto id = static_cast<std::uint16_t>(index);

        auto& enclosure = storage.enclosures[index];
        enclosure.enclosure = id;
        enclosure.status = wire::DiscoveryEnclosureStatus::ready;
        enclosure.model.present = true;
        (void)set_text(enclosure.model.value, kModelName);
        enclosure.lnb_15v_supported.present = true;
        enclosure.lnb_15v_supported.value = false;
        enclosure.readers_known = true;
        enclosure.reader_count = 1U;
        enclosure.readers = &storage.readers[index];
        storage.readers[index].reader = 0U;
        storage.readers[index].device = id;

        auto& device = storage.devices[index];
        device.device = id;
        device.vid = entry.device.vid;
        device.pid = entry.device.pid;
        set_location(entry.device, entry.path, device.location);
        device.receivers_known = true;
        device.receiver_count = 1U;
        device.receivers = &storage.receivers[index];
        storage.receivers[index].receiver = 0U;
        storage.receivers[index].device = id;
        storage.receivers[index].local = 0U;
        storage.receivers[index].systems.mask = 0x01U;

        enclosure.device_count = 1U;
        enclosure.devices = &storage.devices[index];
    }

    wire::DiscoveryScan scan;
    scan.complete = complete;
    (void)set_text(scan.product, kProductSlug);
    (void)set_text(scan.version, TORNE_USERLAND_PRODUCT_VERSION);
    scan.issue_count = storage.issues.size();
    scan.issues = storage.issues.empty() ? nullptr : storage.issues.data();
    scan.enclosure_count = storage.enclosures.size();
    scan.enclosures = storage.enclosures.empty() ? nullptr : storage.enclosures.data();
    scan.ungrouped_usb_device_count = storage.ungrouped.size();
    scan.ungrouped_usb_devices =
        storage.ungrouped.empty() ? nullptr : storage.ungrouped.data();
    return Result<wire::DiscoveryScan>::success(scan);
}

Result<wire::DiscoveryScan> enumerate(ScanStorage& storage) noexcept
{
    libusb_context* context = nullptr;
    if (libusb_init(&context) != LIBUSB_SUCCESS || context == nullptr)
        return Result<wire::DiscoveryScan>::failure(Error::USB_IO);

    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        libusb_exit(context);
        return Result<wire::DiscoveryScan>::failure(Error::USB_IO);
    }

    std::vector<RawDevice> raw;
    raw.reserve(static_cast<std::size_t>(count));
    for (ssize_t index = 0; index < count; ++index) {
        libusb_device* const device = devices[index];
        RawDevice observation;
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(device, &descriptor) == LIBUSB_SUCCESS) {
            observation.descriptor_ok = true;
            observation.vid = descriptor.idVendor;
            observation.pid = descriptor.idProduct;
        }
        observation.bus = libusb_get_bus_number(device);
        observation.address = libusb_get_device_address(device);
        std::array<std::uint8_t, 7U> ports{};
        const int port_count =
            libusb_get_port_numbers(device, ports.data(),
                                    static_cast<int>(ports.size()));
        if (port_count > 0) {
            observation.port_count = static_cast<std::uint8_t>(port_count);
            observation.ports = ports;
        }
        raw.push_back(observation);
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);

    return build_scan(raw.data(), raw.size(), storage);
}

}  // namespace torne::usb
