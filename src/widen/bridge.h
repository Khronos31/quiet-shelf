// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_WIDEN_BRIDGE_H
#define TORNE_WIDEN_BRIDGE_H

#include "usb/transport.h"

#include <array>
#include <cstdint>

namespace torne::widen {

struct ReadResult final {
    bool ok = false;
    std::uint32_t value = 0U;
    usb::TransferOutcome transfer{};
};

std::uint32_t decode_be32(const std::array<std::uint8_t, 4U>& bytes) noexcept;
bool is_readable_24bit_address(std::uint32_t address) noexcept;

// Single-owner, read-only EP0 view. Implementations expose no write method.
class Ep0Reader {
public:
    virtual ~Ep0Reader() noexcept = default;
    virtual ReadResult read32(std::uint32_t address) noexcept = 0;
};

// Native bridge over the existing Transport. Only 24bit, 4B-aligned reads are
// issued before Widen is active. The Transport owns the single PURE_READ retry;
// this class neither retries nor issues writes.
class Bridge final : public Ep0Reader {
public:
    explicit Bridge(usb::Transport& transport) noexcept : transport_(transport) {}
    ReadResult read32(std::uint32_t address) noexcept override;

private:
    usb::Transport& transport_;
};

}  // namespace torne::widen

#endif  // TORNE_WIDEN_BRIDGE_H
