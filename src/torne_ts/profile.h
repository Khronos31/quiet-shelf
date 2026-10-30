// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_TS_PROFILE_H
#define TORNE_TS_PROFILE_H

#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/identity.h>

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

}  // namespace torne::ts

#endif  // TORNE_TS_PROFILE_H
