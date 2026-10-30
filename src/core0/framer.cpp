// SPDX-License-Identifier: GPL-2.0-only
#include "core0/framer.h"

#include <cstring>

namespace torne::core0 {
namespace {
constexpr std::uint16_t kNullPid = 0x1FFFU;
}

void Framer::reset() noexcept
{
    fill_ = 0U;
    continuity_.clear();
    stats_ = FramerStats{};
}

void Framer::push(const std::uint8_t* data, std::size_t length,
                  std::vector<std::array<std::uint8_t, 188U>>& packets) noexcept
{
    if (data == nullptr && length != 0U) return;
    stats_.input_bytes += length;
    std::size_t offset = 0U;
    while (offset < length) {
        const std::size_t available = record_.size() - fill_;
        const std::size_t count = length - offset < available ? length - offset : available;
        std::memcpy(record_.data() + fill_, data + offset, count);
        fill_ += count;
        offset += count;
        while (fill_ >= 2U * 192U) {
            const bool first_sync = record_[4] == 0x47U;
            const bool next_sync = record_[192U + 4U] == 0x47U;
            if (first_sync && next_sync) {
                std::array<std::uint8_t, 188U> packet{};
                std::memcpy(packet.data(), record_.data() + 4U, packet.size());
                packets.push_back(packet);
                ++stats_.packets;
                if ((packet[1] & 0x80U) != 0U) ++stats_.tei_packets;
                const std::uint16_t pid = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(packet[1] & 0x1FU) << 8U) | packet[2]);
                const std::uint8_t afc = static_cast<std::uint8_t>((packet[3] >> 4U) & 0x03U);
                const std::uint8_t cc = packet[3] & 0x0FU;
                if ((afc & 0x01U) != 0U && pid != kNullPid) {
                    const auto previous = continuity_.find(pid);
                    if (previous != continuity_.end() &&
                        ((previous->second + 1U) & 0x0FU) != cc)
                        ++stats_.cc_errors;
                    continuity_[pid] = cc;
                }
                std::memmove(record_.data(), record_.data() + 192U,
                             record_.size() - 192U);
                fill_ -= 192U;
            } else {
                ++stats_.sync_errors;
                std::memmove(record_.data(), record_.data() + 1U,
                             record_.size() - 1U);
                --fill_;
            }
        }
    }
    if (length == 0U) ++stats_.short_events;
}

}  // namespace torne::core0
