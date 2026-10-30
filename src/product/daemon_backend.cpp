// SPDX-License-Identifier: GPL-2.0-only
#include "product/daemon_backend.h"

#include "product/core_family.h"
#include "product/device.h"

#include <dtv/userland/error.h>
#include <dtv/userland/identity.h>

#include <cstdint>
#include <memory>
#include <string>

namespace torne::product {
namespace {

using dtv::userland::Error;
using dtv::userland::Result;

}  // namespace

Result<dtv::userland::wire::DiscoveryScan> DaemonBackend::discover(
    const torne::daemon::DaemonConfig& config) noexcept
{
    auto result = discovery_.discover(config);
    if (result) {
        scan_ = result.value();
        has_scan_ = true;
    } else {
        has_scan_ = false;
    }
    return result;
}

Result<DaemonBackend::FamilyPtr> DaemonBackend::open_family(
    const torne::daemon::DaemonConfig& config) noexcept
{
    if (!config.has_device) {
        if (!has_scan_) {
            const auto discovered = discover(config);
            if (!discovered)
                return Result<FamilyPtr>::failure(discovered.error());
        }
        if (scan_.enclosure_count == 0U ||
            scan_.enclosures[0U].device_count == 0U)
            return Result<FamilyPtr>::failure(Error::NOT_FOUND);
        const auto& location = scan_.enclosures[0U].devices[0U].location;
        if (!location.port.present)
            return Result<FamilyPtr>::failure(Error::NOT_FOUND);
        std::string selector_text = "usb:";
        selector_text.append(location.port.value.bytes.data(),
                             location.port.value.length);
        const auto selector =
            dtv::userland::identity::parse_device_selector(selector_text);
        if (!selector)
            return Result<FamilyPtr>::failure(selector.error());
        auto device = open_hardware(selector.value());
        if (!device) {
            card_service_available_ = false;
            return Result<FamilyPtr>::failure(device.error());
        }
        card_service_available_ =
            device.value()->card_port().line != nullptr;
        auto family = make_family(std::move(device.value()));
        if (!family) return Result<FamilyPtr>::failure(Error::INTERNAL);
        return Result<FamilyPtr>::success(std::move(family));
    }
    auto device = open_hardware(config.device);
    if (!device) {
        card_service_available_ = false;
        return Result<FamilyPtr>::failure(device.error());
    }
    card_service_available_ = device.value()->card_port().line != nullptr;
    std::unique_ptr<dtv::userland::host_runtime::EnclosureFamily> family =
        make_family(std::move(device.value()));
    if (!family)
        return Result<FamilyPtr>::failure(Error::INTERNAL);
    return Result<FamilyPtr>::success(std::move(family));
}

}  // namespace torne::product
