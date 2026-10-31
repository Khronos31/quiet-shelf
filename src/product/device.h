// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_PRODUCT_DEVICE_H
#define TORNE_PRODUCT_DEVICE_H

#include <dtv/userland/error.h>
#include <dtv/userland/identity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace torne::card {
class Clock;
class Line;
}

namespace torne::product {

class Clock {
public:
    virtual ~Clock() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

struct CardPort final {
    card::Line* line = nullptr;
    card::Clock* clock = nullptr;
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
    // Stops only the Core0 capture. Frontend/Core1 remain available for a
    // later tune/start on the same long-lived family.
    virtual dtv::userland::Result<void> stop_capture() noexcept
    {
        return stop_stream();
    }
    virtual dtv::userland::Result<void> read_packet(
        std::array<std::uint8_t, 188U>& packet,
        std::uint32_t timeout_ms) noexcept = 0;
    // Fills a caller-owned bounded buffer with currently available 188B
    // packets. The first wait uses timeout_ms; draining the ready queue does
    // not wait again. Returns the number of bytes written.
    virtual dtv::userland::Result<std::size_t> read_packets(
        std::uint8_t* output, std::size_t capacity,
        std::uint32_t timeout_ms) noexcept
    {
        if (output == nullptr || capacity < 188U)
            return dtv::userland::Result<std::size_t>::failure(
                dtv::userland::Error::BUFFER_TOO_SMALL);
        std::size_t bytes = 0U;
        while (bytes + 188U <= capacity) {
            std::array<std::uint8_t, 188U> packet{};
            const auto read = read_packet(packet, bytes == 0U ? timeout_ms : 0U);
            if (!read) {
                if (bytes == 0U)
                    return dtv::userland::Result<std::size_t>::failure(read.error());
                break;
            }
            std::memcpy(output + bytes, packet.data(), packet.size());
            bytes += packet.size();
        }
        return dtv::userland::Result<std::size_t>::success(bytes);
    }
    virtual dtv::userland::Result<Status> status() noexcept = 0;
    virtual dtv::userland::Result<StreamStats> stream_stats() noexcept
    {
        return dtv::userland::Result<StreamStats>::success(StreamStats{});
    }
    // The product UART and the formal Core card adapter share this single USB
    // owner. A device without a card slot returns an empty port.
    virtual CardPort card_port() noexcept { return {}; }
    virtual void close() noexcept = 0;
};

// Opens the selected USB topology, accepts only Widen PRISTINE/OURS bank0,
// completes the measured startup order, tunes the DiB8096 and starts Core 0.
// It never touches USB before this call. The caller must hand the returned
// device to exactly one runner/thread.
dtv::userland::Result<std::unique_ptr<Device>> open_hardware(
    const std::string& usb_selector, Clock& clock) noexcept;
dtv::userland::Result<std::unique_ptr<Device>> open_hardware(
    const dtv::userland::identity::Selector& selector, Clock& clock) noexcept;
// Device owns its monotonic clock; use this for a returned long-lived family.
dtv::userland::Result<std::unique_ptr<Device>> open_hardware(
    const dtv::userland::identity::Selector& selector) noexcept;
// Opens the selected enclosure from a caller-owned native descriptor. The
// transport duplicates the descriptor and closes only its duplicate; the
// caller (for example termux-usb) keeps ownership of the original.
dtv::userland::Result<std::unique_ptr<Device>> open_hardware_fd(int fd) noexcept;

}  // namespace torne::product

#endif  // TORNE_PRODUCT_DEVICE_H
