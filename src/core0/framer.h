// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_CORE0_FRAMER_H
#define TORNE_CORE0_FRAMER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace torne::core0 {

struct FramerStats final {
    std::uint64_t input_bytes = 0U;
    std::uint64_t packets = 0U;
    std::uint64_t sync_errors = 0U;
    std::uint64_t tei_packets = 0U;
    std::uint64_t cc_errors = 0U;
    std::uint64_t short_events = 0U;
};

// Restores 188B TS from the 192B records (4B prefix + 188B) carried by EP 0x81.
// A record can cross any read boundary. The first 4B are never interpreted as a
// timestamp. Resync drops bytes until the next record's 0x47 position.
class Framer final {
public:
    void reset() noexcept;
    void push(const std::uint8_t* data, std::size_t length,
              std::vector<std::array<std::uint8_t, 188U>>& packets) noexcept;
    const FramerStats& stats() const noexcept { return stats_; }

private:
    std::array<std::uint8_t, 384U> record_{};
    std::size_t fill_ = 0U;
    std::map<std::uint16_t, std::uint8_t> continuity_;
    FramerStats stats_{};
};

}  // namespace torne::core0

#endif  // TORNE_CORE0_FRAMER_H
