// SPDX-License-Identifier: GPL-2.0-only
#include "usb/backend.h"

namespace torne::usb {

dtv::userland::Result<dtv::userland::wire::DiscoveryScan> UsbBackend::discover(
    const torne::daemon::DaemonConfig& config) noexcept
{
    (void)config;
    return enumerate(storage_);
}

dtv::userland::Result<UsbBackend::FamilyPtr> UsbBackend::open_family(
    const torne::daemon::DaemonConfig& config) noexcept
{
    (void)config;
    return dtv::userland::Result<FamilyPtr>::failure(
        dtv::userland::Error::UNSUPPORTED);
}

}  // namespace torne::usb
