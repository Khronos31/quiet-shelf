// SPDX-License-Identifier: GPL-2.0-only
#include "torne_ts/profile.h"

namespace torne::ts {

bool normalize_instance(px4::userland::cli::Px4TsArguments& arguments) noexcept
{
    if (arguments.instance.empty()) return true;
    const auto instance =
        dtv::userland::identity::canonicalize_instance(arguments.instance);
    if (!instance) return false;
    arguments.instance.assign(instance.value().bytes.data(), instance.value().length);
    return true;
}

}  // namespace torne::ts
