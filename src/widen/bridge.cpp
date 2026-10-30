// SPDX-License-Identifier: GPL-2.0-only
#include "widen/bridge.h"

namespace torne::widen {
namespace {

std::uint16_t address_value(std::uint32_t address) noexcept
{
    return static_cast<std::uint16_t>(((address >> 8U) & 0xFF00U) |
                                      ((address >> 24U) & 0xFFU));
}

std::uint16_t address_index(std::uint32_t address) noexcept
{
    return static_cast<std::uint16_t>(address & 0xFFFFU);
}

bool is_aligned_address(std::uint32_t address) noexcept
{
    return (address & 0x03U) == 0U;
}

bool valid_block_length(std::size_t length) noexcept
{
    return length >= 4U && length <= 64U && (length % 4U) == 0U;
}

// The initial host uses only the legacy high byte 0x00 and the core high byte
// 0x01. 0xF0 and above are reserved for future non-memory requests. A block
// must also end inside that domain (no 32-bit wrap into another high byte).
bool valid_wide_range(std::uint32_t address, std::size_t length) noexcept
{
    if (!is_aligned_address(address) || !valid_block_length(length))
        return false;
    const std::uint32_t high = address >> 24U;
    if (high != 0x00U && high != 0x01U) return false;
    return address <= 0x02000000U - static_cast<std::uint32_t>(length);
}

usb::TransferOutcome invalid_transfer() noexcept
{
    return usb::TransferOutcome{usb::TransferState::failed,
                                usb::TransferCause::invalid, 0U, false};
}

std::array<std::uint8_t, 4U> encode_be32(std::uint32_t value) noexcept
{
    return {static_cast<std::uint8_t>(value >> 24U),
            static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value)};
}

ReadResult read_block_value(Bridge& bridge, std::uint32_t address,
                            std::size_t length) noexcept
{
    const BlockReadResult block = bridge.read_block_wide(address, length);
    if (!block.ok || block.length != 4U)
        return ReadResult{false, 0U, block.transfer};
    std::array<std::uint8_t, 4U> raw{block.bytes[0], block.bytes[1],
                                     block.bytes[2], block.bytes[3]};
    return ReadResult{true, decode_be32(raw), block.transfer};
}

}  // namespace

std::uint32_t decode_be32(const std::array<std::uint8_t, 4U>& bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

bool is_readable_24bit_address(std::uint32_t address) noexcept
{
    return address < 0x01000000U && is_aligned_address(address);
}

ReadResult Bridge::read32(std::uint32_t address) noexcept
{
    if (!is_readable_24bit_address(address))
        return ReadResult{false, 0U, invalid_transfer()};
    std::array<std::uint8_t, 4U> raw{};
    const usb::TransferOutcome transfer = transport_.control_in(
        0xC0U, 0xBCU, address_value(address), address_index(address),
        static_cast<std::uint16_t>(raw.size()), raw.data(), 1000U,
        usb::ControlKind::pure_read);
    if (transfer.state != usb::TransferState::complete ||
        transfer.transferred != raw.size())
        return ReadResult{false, 0U, transfer};
    return ReadResult{true, decode_be32(raw), transfer};
}

ReadResult Bridge::read32_wide(std::uint32_t address) noexcept
{
    return read_block_value(*this, address, 4U);
}

WriteResult Bridge::write32(std::uint32_t address, std::uint32_t value,
                            bool wide) noexcept
{
    if (wide ? !valid_wide_range(address, 4U)
             : !is_readable_24bit_address(address))
        return WriteResult{false, invalid_transfer()};
    const std::array<std::uint8_t, 4U> raw = encode_be32(value);
    const usb::TransferOutcome transfer = transport_.control_out(
        0x40U, 0xBCU, address_value(address), address_index(address),
        static_cast<std::uint16_t>(raw.size()), raw.data(), 1000U,
        usb::ControlKind::abs_write);
    return WriteResult{transfer.state == usb::TransferState::complete, transfer};
}

WriteResult Bridge::write32_trigger(std::uint32_t address,
                                    std::uint32_t value,
                                    bool wide) noexcept
{
    if (wide ? !valid_wide_range(address, 4U)
             : !is_readable_24bit_address(address))
        return WriteResult{false, invalid_transfer()};
    const std::array<std::uint8_t, 4U> raw = encode_be32(value);
    const usb::TransferOutcome transfer = transport_.control_out(
        0x40U, 0xBCU, address_value(address), address_index(address),
        static_cast<std::uint16_t>(raw.size()), raw.data(), 1000U,
        usb::ControlKind::trigger);
    return WriteResult{transfer.state == usb::TransferState::complete, transfer};
}

BlockReadResult Bridge::read_block_wide(std::uint32_t address,
                                        std::size_t length) noexcept
{
    BlockReadResult result;
    if (!valid_wide_range(address, length)) {
        result.transfer = invalid_transfer();
        return result;
    }
    const usb::TransferOutcome transfer = transport_.control_in(
        0xC0U, 0xBCU, address_value(address), address_index(address),
        static_cast<std::uint16_t>(length), result.bytes.data(), 1000U,
        usb::ControlKind::pure_read);
    result.transfer = transfer;
    if (transfer.state != usb::TransferState::complete ||
        transfer.transferred != length)
        return result;
    result.ok = true;
    result.length = length;
    return result;
}

WriteResult Bridge::write_block_wide(std::uint32_t address,
                                     const std::uint8_t* data,
                                     std::size_t length) noexcept
{
    if (!valid_wide_range(address, length) || data == nullptr)
        return WriteResult{false, invalid_transfer()};
    const usb::TransferOutcome transfer = transport_.control_out(
        0x40U, 0xBCU, address_value(address), address_index(address),
        static_cast<std::uint16_t>(length), data, 1000U,
        usb::ControlKind::abs_write);
    return WriteResult{transfer.state == usb::TransferState::complete, transfer};
}

}  // namespace torne::widen
