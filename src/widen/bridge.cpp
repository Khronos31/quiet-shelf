// SPDX-License-Identifier: GPL-2.0-only
#include "widen/bridge.h"

namespace torne::widen {

std::uint32_t decode_be32(const std::array<std::uint8_t, 4U>& bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

bool is_readable_24bit_address(std::uint32_t address) noexcept
{
    return address < 0x01000000U && (address & 0x03U) == 0U;
}

ReadResult Bridge::read32(std::uint32_t address) noexcept
{
    if (!is_readable_24bit_address(address)) {
        return ReadResult{false, 0U,
                          usb::TransferOutcome{usb::TransferState::failed,
                                               usb::TransferCause::invalid,
                                               0U, false}};
    }
    std::array<std::uint8_t, 4U> raw{};
    const std::uint16_t value = static_cast<std::uint16_t>(
        ((address >> 8U) & 0xFF00U) | ((address >> 24U) & 0xFFU));
    const std::uint16_t index = static_cast<std::uint16_t>(address & 0xFFFFU);
    const usb::TransferOutcome transfer = transport_.control_in(
        0xC0U, 0xBCU, value, index, static_cast<std::uint16_t>(raw.size()),
        raw.data(), 1000U, usb::ControlKind::pure_read);
    if (transfer.state != usb::TransferState::complete ||
        transfer.transferred != raw.size())
        return ReadResult{false, 0U, transfer};
    return ReadResult{true, decode_be32(raw), transfer};
}

}  // namespace torne::widen
