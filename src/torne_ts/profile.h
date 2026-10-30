// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_TS_PROFILE_H
#define TORNE_TS_PROFILE_H

#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/identity.h>

#include <cstdint>
#include <cstdio>
#include <optional>

namespace torne::ts {

// Serial selection is unsupported because the device has no serial descriptor.
// -f still requires --system; the product rejects ISDB-S after parsing.
inline const px4::userland::cli::TsClientProfile kProfile{
    "torne-ts",
    "torne-userland",
    dtv::userland::identity::Product::torne,
    false,
    true,
    nullptr,
    false,
};

// Applies Core's instance rules after the shared host_ts parse. The spelling is
// preserved; reserved names and invalid tokens are rejected exactly as
// canonicalize_instance decides. Kept separate so the CLI and its test use the
// same production path instead of re-deriving the rules.
bool normalize_instance(px4::userland::cli::Px4TsArguments& arguments) noexcept;

// Returns the measured queue-drop count from the final run counters, or
// nullopt when the counter availability is unknown/unsupported.
std::optional<std::uint64_t> measured_queue_drops(
    const px4::userland::ipc::CountersPayload& counters) noexcept;

// Applies the success-path queue-drop policy used by the IPC CLI. A measured
// nonzero drop is a user-visible failure; a measured zero is success; unknown
// availability is reported but never fabricated as zero.
int finalize_ipc_success(
    const px4::userland::ipc::CountersPayload& counters,
    std::FILE* error) noexcept;

}  // namespace torne::ts

#endif  // TORNE_TS_PROFILE_H
