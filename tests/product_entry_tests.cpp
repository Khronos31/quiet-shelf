// SPDX-License-Identifier: GPL-2.0-only
#include "product/packet_queue.h"
#include "product/runner.h"

#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace cli = px4::userland::cli;
using dtv::userland::Error;
using dtv::userland::Result;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

class Clock final : public cli::Px4TsClock {
public:
    std::uint64_t now = 0U;
    std::uint32_t slept = 0U;

    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t ms) noexcept override
    {
        now += ms;
        slept += ms;
    }
};

class Signal final : public cli::Px4TsSignal {
public:
    bool stop = false;
    bool stop_requested() const noexcept override { return stop; }
};

class Output final : public cli::Px4TsOutput {
public:
    std::vector<std::array<std::uint8_t, 188U>> packets;
    int fail_after = -1;
    bool timeout_on_stop = false;

    Result<void> write(dtv::userland::ByteView bytes,
                       const cli::Px4TsSignal& signal) noexcept override
    {
        if (timeout_on_stop && signal.stop_requested())
            return Result<void>::failure(Error::TIMEOUT);
        if (fail_after >= 0 && static_cast<int>(packets.size()) >= fail_after)
            return Result<void>::failure(Error::INTERNAL);
        if (bytes.size != 188U)
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        std::array<std::uint8_t, 188U> packet{};
        std::memcpy(packet.data(), bytes.data, packet.size());
        packets.push_back(packet);
        return Result<void>::success();
    }
};

class FakeDevice final : public torne::product::Device {
public:
    Clock* clock = nullptr;
    std::vector<std::array<std::uint8_t, 188U>> pending;
    int tune_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;
    bool tune_ok = true;
    bool read_ok = true;
    bool start_ok = true;
    bool stop_ok = true;
    std::uint64_t dropped = 0U;
    Signal* signal = nullptr;
    bool stop_on_read = false;

    Result<void> tune(std::uint64_t hz, std::uint32_t) noexcept override
    {
        ++tune_calls;
        if (!tune_ok) return Result<void>::failure(Error::UNSUPPORTED);
        if (hz == 0U) return Result<void>::failure(Error::INVALID_ARGUMENT);
        return Result<void>::success();
    }
    Result<void> start_stream() noexcept override
    {
        ++start_calls;
        return start_ok ? Result<void>::success()
                        : Result<void>::failure(Error::PROTOCOL_ERROR);
    }
    Result<void> stop_stream() noexcept override
    {
        ++stop_calls;
        return stop_ok ? Result<void>::success()
                       : Result<void>::failure(Error::USB_IO);
    }
    Result<void> read_packet(std::array<std::uint8_t, 188U>& packet,
                             std::uint32_t) noexcept override
    {
        if (!read_ok) return Result<void>::failure(Error::USB_IO);
        if (!pending.empty()) {
            packet = pending.front();
            pending.erase(pending.begin());
            if (stop_on_read && signal != nullptr) signal->stop = true;
            return Result<void>::success();
        }
        if (clock) clock->sleep_ms(100U);
        return Result<void>::failure(Error::TIMEOUT);
    }
    Result<torne::product::StreamStats> stream_stats() noexcept override
    {
        torne::product::StreamStats stats;
        stats.dropped = dropped;
        return Result<torne::product::StreamStats>::success(stats);
    }
    Result<torne::product::Status> status() noexcept override
    {
        torne::product::Status status;
        status.locked = true;
        return Result<torne::product::Status>::success(status);
    }
    void close() noexcept override {}
};

std::array<std::uint8_t, 188U> packet(std::uint8_t marker)
{
    std::array<std::uint8_t, 188U> p{};
    p[0] = 0x47U;
    p[1] = marker;
    return p;
}

void test_packets_and_packet_count()
{
    Clock clock;
    Signal signal;
    Output output;
    FakeDevice device;
    device.clock = &clock;
    device.pending = {packet(1U), packet(2U), packet(3U)};

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    options.packet_count = 2U;
    options.packet_count_set = true;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(result);
    CHECK(device.tune_calls == 1);
    CHECK(device.start_calls == 1);
    CHECK(device.stop_calls == 1);
    CHECK(output.packets.size() == 2U);
    CHECK(output.packets[0][1] == 1U && output.packets[1][1] == 2U);
}

void test_duration_uses_clock()
{
    Clock clock;
    Signal signal;
    Output output;
    FakeDevice device;
    device.clock = &clock;

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    options.duration_seconds = 1U;
    options.duration_set = true;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(result);
    CHECK(clock.now >= 1000U);
    CHECK(device.stop_calls == 1);
    CHECK(output.packets.empty());
}

void test_output_failure_stops()
{
    Clock clock;
    Signal signal;
    Output output;
    output.fail_after = 0;
    FakeDevice device;
    device.clock = &clock;
    device.pending = {packet(1U)};

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    options.packet_count = 1U;
    options.packet_count_set = true;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(!result);
    CHECK(result.error() == Error::INTERNAL);
    CHECK(device.stop_calls == 1);
}

void test_read_failure_stops()
{
    Clock clock;
    Signal signal;
    Output output;
    FakeDevice device;
    device.clock = &clock;
    device.read_ok = false;

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    options.packet_count = 1U;
    options.packet_count_set = true;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(!result);
    CHECK(result.error() == Error::USB_IO);
    CHECK(device.stop_calls == 1);
}

void test_tune_failure_does_not_start()
{
    Clock clock;
    Signal signal;
    Output output;
    FakeDevice device;
    device.clock = &clock;
    device.tune_ok = false;

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(!result);
    CHECK(result.error() == Error::UNSUPPORTED);
    CHECK(device.start_calls == 0);
    CHECK(device.stop_calls == 0);
}

void test_packet_queue_is_bounded_and_counts_drops()
{
    torne::product::PacketQueue queue{4U};
    std::array<std::uint8_t, 188U> input{};
    for (std::uint8_t marker = 0U; marker < 6U; ++marker) {
        input[0] = marker;
        queue.push(input.data(), input.size());
    }
    CHECK(queue.dropped() == 2U);
    std::array<std::uint8_t, 188U> output{};
    CHECK(queue.pop(output));
    CHECK(output[0] == 2U);
    CHECK(queue.pop(output));
    CHECK(output[0] == 3U);
    CHECK(queue.pop(output));
    CHECK(output[0] == 4U);
    CHECK(queue.pop(output));
    CHECK(output[0] == 5U);
    CHECK(!queue.pop(output));
    queue.reset();
    CHECK(queue.dropped() == 0U);
    CHECK(!queue.pop(output));
}

void test_start_failure_stops()
{
    Clock clock;
    Signal signal;
    Output output;
    FakeDevice device;
    device.clock = &clock;
    device.start_ok = false;

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(!result);
    CHECK(result.error() == Error::PROTOCOL_ERROR);
    CHECK(device.stop_calls == 1);
}

void test_stop_failure_is_not_success()
{
    Clock clock;
    Signal signal;
    Output output;
    FakeDevice device;
    device.clock = &clock;
    device.pending = {packet(1U)};
    device.stop_ok = false;

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    options.packet_count = 1U;
    options.packet_count_set = true;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(!result);
    CHECK(result.error() == Error::USB_IO);
    CHECK(device.stop_calls == 1);
}

void test_dropped_packets_are_reported()
{
    Clock clock;
    Signal signal;
    Output output;
    FakeDevice device;
    device.clock = &clock;
    device.pending = {packet(1U)};
    device.dropped = 1U;

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    options.packet_count = 1U;
    options.packet_count_set = true;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(!result);
    CHECK(result.error() == Error::SLOW_CONSUMER);
}

void test_stop_read_timeout_is_normal()
{
    Clock clock;
    Signal signal;
    Output output;
    output.timeout_on_stop = true;
    FakeDevice device;
    device.clock = &clock;
    device.signal = &signal;
    device.stop_on_read = true;
    device.pending = {packet(1U)};

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    options.packet_count = 1U;
    options.packet_count_set = true;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(result);
    CHECK(device.stop_calls == 1);
    CHECK(output.packets.empty());
}

void test_signal_stop_finishes_cleanly()
{
    Clock clock;
    Signal signal;
    Output output;
    FakeDevice device;
    device.clock = &clock;
    signal.stop = true;

    torne::product::RunOptions options;
    options.frequency_hz = 557142857U;
    const auto result = torne::product::run(device, options, output, clock, signal);
    CHECK(result);
    CHECK(device.start_calls == 1);
    CHECK(device.stop_calls == 1);
    CHECK(output.packets.empty());
}

}  // namespace

int main()
{
    test_packets_and_packet_count();
    test_duration_uses_clock();
    test_output_failure_stops();
    test_read_failure_stops();
    test_tune_failure_does_not_start();
    test_packet_queue_is_bounded_and_counts_drops();
    test_start_failure_stops();
    test_stop_failure_is_not_success();
    test_dropped_packets_are_reported();
    test_stop_read_timeout_is_normal();
    test_signal_stop_finishes_cleanly();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("product entry tests passed\n");
    return 0;
}
