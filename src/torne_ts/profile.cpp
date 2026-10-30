// SPDX-License-Identifier: GPL-2.0-only
#include "torne_ts/profile.h"

namespace torne::ts {

std::optional<std::uint64_t> measured_queue_drops(
    const px4::userland::ipc::CountersPayload& counters) noexcept
{
    if (counters.availability[5U] != dtv::userland::wire::MeasureState::valid)
        return std::nullopt;
    return counters.queue_drops;
}

int finalize_ipc_success(
    const px4::userland::ipc::CountersPayload& counters,
    std::FILE* error) noexcept
{
    const auto drops = measured_queue_drops(counters);
    if (!drops) {
        std::fprintf(error, "torne-ts: queue drop statistic unavailable\n");
        return 0;
    }
    if (drops.value() != 0U) {
        std::fprintf(error, "torne-ts: queue drops: %llu\n",
                     static_cast<unsigned long long>(drops.value()));
        return px4::userland::cli::px4_ts_exit_status(
            dtv::userland::Error::SLOW_CONSUMER);
    }
    return 0;
}

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
