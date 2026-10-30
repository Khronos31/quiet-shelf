// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_CORE0_TS_PUMP_H
#define TORNE_CORE0_TS_PUMP_H

#include "core0/framer.h"
#include "usb/transport.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace torne::core0 {

class Clock {
public:
    virtual ~Clock() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

struct PumpStats final {
    std::uint64_t transfers = 0U;
    std::uint64_t bytes = 0U;
    std::uint64_t errors = 0U;
    std::uint64_t discarded_old_epoch = 0U;
};

// Bounded asynchronous Bulk IN pump for EP 0x81. It never owns a second USB
// handle; all transfers use the existing Transport claim. The caller must keep
// pump()/stop() on the same owner/event path as EP0.
class TsPump final {
public:
    using PacketSink = void (*)(void* context, const std::uint8_t* packet,
                                std::size_t length) noexcept;

    TsPump(usb::Transport& transport, Framer& framer, Clock& clock,
           std::size_t depth = 16U, std::size_t buffer_size = 65536U,
           PacketSink sink = nullptr, void* sink_context = nullptr) noexcept;
    ~TsPump() noexcept;

    int start(std::uint32_t epoch) noexcept;
    // max_completions=0 preserves the unbounded drain used by tests/direct
    // callers. A nonzero limit bounds one poll to that many callbacks so a
    // consumer can drain its queue between bursts.
    int pump(std::size_t max_completions = 0U) noexcept;
    int stop() noexcept;
    bool running() const noexcept { return running_; }
    std::uint32_t epoch() const noexcept { return epoch_; }
    const PumpStats& stats() const noexcept { return stats_; }
    std::size_t depth() const noexcept { return buffers_.size(); }
    std::size_t active() const noexcept { return id_to_index_.size(); }
    std::size_t buffer_size() const noexcept { return buffer_size_; }

private:
    int submit(std::size_t index) noexcept;

    usb::Transport& transport_;
    Framer& framer_;
    Clock& clock_;
    std::size_t buffer_size_ = 65536U;
    std::vector<std::vector<std::uint8_t>> buffers_;
    std::vector<std::uint64_t> ids_;
    std::vector<std::uint32_t> epochs_;
    std::vector<bool> active_;
    std::vector<std::array<std::uint8_t, 188U>> packets_;
    std::map<std::uint64_t, std::size_t> id_to_index_;
    PacketSink sink_ = nullptr;
    void* sink_context_ = nullptr;
    std::uint32_t epoch_ = 0U;
    bool running_ = false;
    bool stopping_ = false;
    bool failed_ = false;
    PumpStats stats_{};
};

}  // namespace torne::core0

#endif  // TORNE_CORE0_TS_PUMP_H
