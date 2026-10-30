// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_PRODUCT_DAEMON_BACKEND_H
#define TORNE_PRODUCT_DAEMON_BACKEND_H

#include "torned/backend.h"
#include "usb/backend.h"

namespace torne::product {

// Product Backend for torned. Discovery is the existing read-only USB scan;
// open_family creates exactly one HardwareDevice and passes it to the Core
// EnclosureFamily.
class DaemonBackend final : public torne::daemon::Backend {
public:
    dtv::userland::Result<dtv::userland::wire::DiscoveryScan> discover(
        const torne::daemon::DaemonConfig& config) noexcept override;
    dtv::userland::Result<FamilyPtr> open_family(
        const torne::daemon::DaemonConfig& config) noexcept override;
    bool card_service_available() const noexcept override
    {
        return card_service_available_;
    }

private:
    torne::usb::UsbBackend discovery_;
    bool has_scan_ = false;
    bool card_service_available_ = false;
    dtv::userland::wire::DiscoveryScan scan_{};
};

}  // namespace torne::product

#endif  // TORNE_PRODUCT_DAEMON_BACKEND_H
