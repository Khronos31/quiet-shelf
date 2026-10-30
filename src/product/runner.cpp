// SPDX-License-Identifier: GPL-2.0-only
#include "product/runner.h"

#include <array>
#include <cstdint>

namespace torne::product {
namespace {

using dtv::userland::Error;
using dtv::userland::Result;

Result<void> failure(Error error) noexcept
{
    return Result<void>::failure(error);
}

}  // namespace

Result<void> run(Device& device, const RunOptions& options,
                 px4::userland::cli::Px4TsOutput& output,
                 px4::userland::cli::Px4TsClock& clock,
                 const px4::userland::cli::Px4TsSignal& signal) noexcept
{
    if (options.frequency_hz == 0U) return failure(Error::INVALID_ARGUMENT);
    if (options.duration_set && options.packet_count_set)
        return failure(Error::INVALID_ARGUMENT);
    if (options.duration_set && options.duration_seconds == 0U)
        return failure(Error::INVALID_ARGUMENT);

    const auto tuned = device.tune(options.frequency_hz, options.tune_timeout_ms);
    if (!tuned) return tuned;

    const auto started = device.start_stream();
    if (!started) {
        (void)device.stop_stream();
        return started;
    }

    const std::uint64_t started_ms = clock.monotonic_ms();
    const std::uint64_t duration_ms =
        options.duration_set ? options.duration_seconds * 1000U : 0U;
    std::uint64_t packets = 0U;
    std::array<std::uint8_t, 188U> packet{};
    Result<void> result = Result<void>::success();

    for (;;) {
        if (signal.stop_requested()) break;
        if (options.duration_set &&
            clock.monotonic_ms() - started_ms >= duration_ms)
            break;
        if (options.packet_count_set && packets >= options.packet_count)
            break;

        const auto read = device.read_packet(packet, 100U);
        if (!read) {
            if (read.error() == Error::TIMEOUT) continue;
            result = read;
            break;
        }
        const auto written = output.write(
            dtv::userland::ByteView{packet.data(), packet.size()}, signal);
        if (!written) {
            if (written.error() == Error::TIMEOUT && signal.stop_requested())
                break;
            result = written;
            break;
        }
        ++packets;
    }

    const auto stopped = device.stop_stream();
    if (result) result = stopped;
    if (result) {
        const auto stats = device.stream_stats();
        if (stats && stats.value().dropped != 0U)
            result = failure(Error::SLOW_CONSUMER);
    }
    return result;
}

}  // namespace torne::product
