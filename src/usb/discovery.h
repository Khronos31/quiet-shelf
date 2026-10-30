// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_USB_DISCOVERY_H
#define TORNE_USB_DISCOVERY_H

#include <dtv/userland/discovery_json.h>
#include <dtv/userland/error.h>
#include <dtv/userland/records.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace torne::usb {

// One device observed through public libusb calls. No open, claim, reset or
// transfer is performed; descriptors and topology only.
struct RawDevice final {
    std::uint16_t vid = 0U;
    std::uint16_t pid = 0U;
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    std::array<std::uint8_t, 7U> ports{};
    std::uint8_t port_count = 0U;
    bool descriptor_ok = false;
};

// Owns every array pointed to by the returned DiscoveryScan. The scan view is
// valid while this storage is unchanged and not rebuilt.
struct ScanStorage final {
    std::vector<dtv::userland::wire::DiscoveryEnclosure> enclosures;
    std::vector<dtv::userland::wire::DiscoveryDevice> devices;
    std::vector<dtv::userland::wire::DiscoveryReceiver> receivers;
    std::vector<dtv::userland::wire::DiscoveryReader> readers;
    std::vector<dtv::userland::wire::DiscoveryUsbObservation> ungrouped;
    std::vector<dtv::userland::wire::DiscoveryScanIssue> issues;
};

bool is_torne_device(std::uint16_t vid, std::uint16_t pid) noexcept;

// Builds the canonical `bus-port[.port...]` selector text. Returns false when
// there is no usable topology or the text does not fit.
bool format_usb_path(std::uint8_t bus, const std::uint8_t* ports,
                     std::size_t port_count,
                     dtv::userland::wire::WString<64>& output) noexcept;

// Pure mapping from observed devices to the Core discovery schema. Used by the
// libusb enumerator and by offline tests.
dtv::userland::Result<dtv::userland::wire::DiscoveryScan> build_scan(
    const RawDevice* devices, std::size_t count, ScanStorage& storage) noexcept;

// Initializes a fresh libusb context, reads the device list and descriptors,
// then releases everything. No device handle is opened.
dtv::userland::Result<dtv::userland::wire::DiscoveryScan> enumerate(
    ScanStorage& storage) noexcept;

}  // namespace torne::usb

#endif  // TORNE_USB_DISCOVERY_H
