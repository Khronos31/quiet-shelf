// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_PRODUCT_PACKET_QUEUE_H
#define TORNE_PRODUCT_PACKET_QUEUE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace torne::product {

// Bounded handoff between the TsPump callback and the reader. Overflow drops
// the oldest packet and is counted so the caller can report it instead of
// silently presenting a gap as a normal stream.
class PacketQueue final {
public:
    explicit PacketQueue(std::size_t limit) noexcept : limit_(limit) {}

    void push(const std::uint8_t* data, std::size_t length) noexcept
    {
        if (data == nullptr || length != 188U) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= limit_) {
            queue_.pop_front();
            ++dropped_;
        }
        std::array<std::uint8_t, 188U> packet{};
        for (std::size_t index = 0U; index < packet.size(); ++index)
            packet[index] = data[index];
        queue_.push_back(packet);
    }

    bool pop(std::array<std::uint8_t, 188U>& packet) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        packet = queue_.front();
        queue_.pop_front();
        return true;
    }

    std::uint64_t dropped() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    std::size_t size() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    void reset() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        dropped_ = 0U;
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::array<std::uint8_t, 188U>> queue_;
    std::size_t limit_ = 0U;
    std::uint64_t dropped_ = 0U;
};

}  // namespace torne::product

#endif  // TORNE_PRODUCT_PACKET_QUEUE_H
