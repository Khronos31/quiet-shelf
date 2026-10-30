// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_PRODUCT_DEVICE_H
#define TORNE_PRODUCT_DEVICE_H

#include <dtv/userland/error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace torne::product {

class Clock {
public:
    virtual ~Clock() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

struct StreamStats final {
    std::uint64_t packets = 0U;
    std::uint64_t bytes = 0U;
    std::uint64_t dropped = 0U;
};

struct Status final {
    bool locked = false;
    int fe_status = 0;
    bool has_cnr = false;
    int cnr_x100 = 0;
    bool has_strength = false;
    int strength_dbm_x1000 = 0;
};

// Product-owned device lifecycle. The hardware implementation owns the single
// USB handle; a fake implementation is used only by the entry tests.
class Device {
public:
    virtual ~Device() noexcept = default;
    virtual dtv::userland::Result<void> tune(std::uint64_t frequency_hz,
                                             std::uint32_t timeout_ms) noexcept = 0;
    virtual dtv::userland::Result<void> start_stream() noexcept = 0;
    virtual dtv::userland::Result<void> stop_stream() noexcept = 0;
    virtual dtv::userland::Result<void> read_packet(
        std::array<std::uint8_t, 188U>& packet,
        std::uint32_t timeout_ms) noexcept = 0;
    virtual dtv::userland::Result<Status> status() noexcept = 0;
    virtual dtv::userland::Result<StreamStats> stream_stats() noexcept
    {
        return dtv::userland::Result<StreamStats>::success(StreamStats{});
    }
    virtual void close() noexcept = 0;
};

// Opens the selected USB topology, accepts only Widen PRISTINE/OURS bank0,
// completes the measured startup order, tunes the DiB8096 and starts Core 0.
// It never touches USB before this call. The caller must hand the returned
// device to exactly one runner/thread.
dtv::userland::Result<std::unique_ptr<Device>> open_hardware(
    const std::string& usb_selector, Clock& clock) noexcept;

}  // namespace torne::product

#endif  // TORNE_PRODUCT_DEVICE_H
