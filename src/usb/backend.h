// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_USB_BACKEND_H
#define TORNE_USB_BACKEND_H

#include "torned/backend.h"
#include "usb/discovery.h"

namespace torne::usb {

// Read-only Backend for this increment. Discovery never opens or claims a
// device; open_family stays unsupported until the transport increment.
class UsbBackend final : public torne::daemon::Backend {
public:
    dtv::userland::Result<dtv::userland::wire::DiscoveryScan> discover(
        const torne::daemon::DaemonConfig& config) noexcept override;
    dtv::userland::Result<FamilyPtr> open_family(
        const torne::daemon::DaemonConfig& config) noexcept override;

private:
    ScanStorage storage_;
};

}  // namespace torne::usb

#endif  // TORNE_USB_BACKEND_H
