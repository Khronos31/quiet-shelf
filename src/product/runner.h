// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_PRODUCT_RUNNER_H
#define TORNE_PRODUCT_RUNNER_H

#include "product/device.h"

#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/error.h>

#include <cstdint>

namespace torne::product {

struct RunOptions final {
    std::uint64_t frequency_hz = 0U;
    std::uint32_t tune_timeout_ms = 10000U;
    std::uint64_t duration_seconds = 0U;
    std::uint64_t packet_count = 0U;
    bool duration_set = false;
    bool packet_count_set = false;
};

// Drives one tuned Device to an output until the signal stops, the duration
// expires, or the packet count is reached. It owns no USB and never writes
// diagnostics to the output stream.
dtv::userland::Result<void> run(
    Device& device, const RunOptions& options,
    px4::userland::cli::Px4TsOutput& output,
    px4::userland::cli::Px4TsClock& clock,
    const px4::userland::cli::Px4TsSignal& signal) noexcept;

}  // namespace torne::product

#endif  // TORNE_PRODUCT_RUNNER_H
