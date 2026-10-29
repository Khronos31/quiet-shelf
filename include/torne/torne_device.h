// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <dtv/userland/error.h>
#include <dtv/userland/transport.h>

extern "C" {
#include "torne_core.h"
#include "torne_dib.h"
}

#include <array>
#include <cstddef>
#include <cstdint>

namespace torne {

dtv::userland::Error map_driver_error(int code) noexcept;

// Owns the borrowed GPL C bridge/demodulator state, not the USB device.
// The supplied callback context MUST remain valid until shutdown completes.
class Device final {
public:
    explicit Device(const struct torne_ops& callbacks, void* context) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() noexcept;

    dtv::userland::Result<void> initialize() noexcept;
    dtv::userland::Result<void> tune(std::uint32_t frequency_hz) noexcept;
    dtv::userland::Result<struct torne_dib_status> status() noexcept;
    dtv::userland::Result<void> start_stream() noexcept;
    dtv::userland::Result<void> stop_stream() noexcept;
    // Produces whole 188-byte TS packets. output requires at least 2 packets.
    dtv::userland::Result<std::size_t> read_ts(
        dtv::userland::MutableByteView output, std::uint32_t timeout_ms) noexcept;
    dtv::userland::Result<void> shutdown() noexcept;
    bool initialized() const noexcept { return initialized_; }
    bool streaming() const noexcept { return streaming_; }

private:
    struct torne_ops callbacks_{};
    void* context_ = nullptr;
    struct torne_bridge bridge_{};
    struct torne_dib demod_{};
    struct torne_framer framer_{};
    // Maximum raw read: 320 whole 192-byte records, with carry between calls.
    std::array<std::uint8_t, 61440> raw_{};
    bool initialized_ = false;
    bool tuned_ = false;
    bool streaming_ = false;
};

} // namespace torne
