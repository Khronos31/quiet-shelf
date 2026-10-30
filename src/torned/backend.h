// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_DAEMON_BACKEND_H
#define TORNE_DAEMON_BACKEND_H

#include "torned/config.h"

#include <dtv/userland/discovery_json.h>
#include <dtv/userland/error.h>
#include <dtv/userland/host_runtime/family.h>

#include <memory>

namespace torne::daemon {

// Product-owned hardware seam. The USB transport, MB86H47A bridge, DiB8096
// frontend and card UART are not part of this CLI framework; they are supplied
// by a later backend implementation. A null Backend means "no hardware
// backend", which torned reports as an error instead of inventing a state.
class Backend {
public:
    using FamilyPtr = std::unique_ptr<dtv::userland::host_runtime::EnclosureFamily>;

    virtual ~Backend() noexcept = default;

    // Read-only scan for --list-json. Must not claim the device, load FR30,
    // tune, or change card power. The returned view is owned by this Backend
    // and stays valid until the next call.
    virtual dtv::userland::Result<dtv::userland::wire::DiscoveryScan> discover(
        const DaemonConfig& config) noexcept = 0;

    // Opens the single owned enclosure. The returned family is the exact
    // dependency graph host_runtime/host_listener_context consumes.
    virtual dtv::userland::Result<FamilyPtr> open_family(
        const DaemonConfig& config) noexcept = 0;

    // True only when open_family supplied a real formal CardService backend,
    // independent of whether a card is currently inserted.
    virtual bool card_service_available() const noexcept { return false; }
};

}  // namespace torne::daemon

#endif  // TORNE_DAEMON_BACKEND_H
