// SPDX-License-Identifier: GPL-2.0-only
#include <torne/product_contract.h>

#include <dtv/userland/discovery_json.h>
#include <dtv/userland/identity.h>
#include <dtv/userland/tuning.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace torne {
namespace {

namespace wire = dtv::userland::wire;
namespace identity = dtv::userland::identity;
using dtv::userland::Error;
using dtv::userland::Result;

template <std::size_t N>
bool set_text(wire::WString<N>& target, std::string_view text) noexcept
{
    if (text.size() > target.bytes.size()) return false;
    target.length = static_cast<std::uint16_t>(text.size());
    std::copy(text.begin(), text.end(), target.bytes.begin());
    return true;
}

} // namespace

Result<std::uint32_t> resolve_terrestrial_hz(std::string_view channel) noexcept
{
    dtv::tuning::Request request{};
    request.channel = channel;
    const auto resolved = dtv::tuning::resolve(request);
    if (!resolved || resolved.value().system != wire::System::isdb_t ||
        resolved.value().frequency_hz > std::numeric_limits<std::uint32_t>::max()) {
        return Result<std::uint32_t>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<std::uint32_t>::success(
        static_cast<std::uint32_t>(resolved.value().frequency_hz));
}

Result<std::string> discovery_json(const std::vector<UsbObservation>& devices,
                                   bool complete, std::string_view version)
{
    if (devices.size() > std::numeric_limits<std::uint16_t>::max())
        return Result<std::string>::failure(Error::RESOURCE_LIMIT);

    const auto count = devices.size();
    // Stable order independent of libusb iteration order.
    std::vector<UsbObservation> ordered = devices;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.bus != b.bus) return a.bus < b.bus;
        if (a.port != b.port) return a.port < b.port;
        return a.address < b.address;
    });
    for (std::size_t i = 0; i < count; ++i) {
        if (ordered[i].port.empty()) continue;
        const auto selector =
            identity::parse_device_selector(std::string("usb:") + ordered[i].port);
        if (!selector || selector.value().kind != identity::SelectorKind::usb_topology ||
            selector.value().topology.count != 1U)
            return Result<std::string>::failure(Error::INVALID_ARGUMENT);
        if (i != 0U && ordered[i].port == ordered[i-1].port)
            return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    }

    std::vector<wire::DiscoveryEnclosure> enclosures(count);
    std::vector<wire::DiscoveryDevice> functions(count);
    std::vector<wire::DiscoveryReceiver> receivers(count);
    std::vector<wire::DiscoveryReader> readers(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t id = static_cast<std::uint16_t>(i + 1U);
        auto& enclosure = enclosures[i];
        enclosure.enclosure = id;
        enclosure.status = ordered[i].port.empty() ?
            wire::DiscoveryEnclosureStatus::incomplete :
            wire::DiscoveryEnclosureStatus::ready;
        enclosure.model.present = true;
        if (!set_text(enclosure.model.value, "Sony CECH-ZD1 J"))
            return Result<std::string>::failure(Error::INTERNAL);
        enclosure.lnb_15v_supported.present = true;
        enclosure.lnb_15v_supported.value = false;
        enclosure.readers_known = true;
        enclosure.reader_count = 1U;
        enclosure.readers = &readers[i];
        enclosure.device_count = 1U;
        enclosure.devices = &functions[i];

        readers[i].reader = 0U;
        readers[i].device = id;

        auto& function = functions[i];
        function.device = id;
        function.vid = 0x054cU;
        function.pid = 0x04b2U;
        function.location.bus.present = true;
        function.location.bus.value = ordered[i].bus;
        function.location.address.present = true;
        function.location.address.value = ordered[i].address;
        if (!ordered[i].port.empty()) {
            function.location.port.present = true;
            if (!set_text(function.location.port.value, ordered[i].port))
                return Result<std::string>::failure(Error::INVALID_ARGUMENT);
        }
        function.receivers_known = true;
        function.receiver_count = 1U;
        function.receivers = &receivers[i];

        receivers[i].receiver = 0U;
        receivers[i].device = id;
        receivers[i].local = 0U;
        receivers[i].systems.mask = 0x01U; // ISDB-T only
    }

    wire::DiscoveryScan scan{};
    scan.complete = complete;
    if (!set_text(scan.product, "torne") || !set_text(scan.version, version))
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    scan.enclosure_count = count;
    scan.enclosures = enclosures.data();
    const auto measured = wire::discovery_json_size(scan);
    if (!measured) return Result<std::string>::failure(measured.error());
    std::vector<std::uint8_t> bytes(measured.value());
    const auto written = wire::encode_discovery_json(scan, {bytes.data(), bytes.size()});
    if (!written) return Result<std::string>::failure(written.error());
    return Result<std::string>::success(
        std::string(reinterpret_cast<const char*>(bytes.data()), written.value()));
}

} // namespace torne
