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

struct WriteResult final {
    bool ok = false;
    usb::TransferOutcome transfer{};
};

struct BlockReadResult final {
    bool ok = false;
    std::size_t length = 0U;
    std::array<std::uint8_t, 64U> bytes{};
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

// Single-owner EP0 view for the Widen transaction. read32() is restricted to
// the pre-Widen 24bit/4B path; the wide calls are only issued after all five
// patch words have been read back. No implementation retries by itself: the
// Transport owns the contract retry for PURE_READ/ABS_WRITE.
class Ep0Io : public Ep0Reader {
public:
    ~Ep0Io() noexcept override = default;
    virtual ReadResult read32_wide(std::uint32_t address) noexcept = 0;
    virtual WriteResult write32(std::uint32_t address, std::uint32_t value,
                                bool wide) noexcept = 0;
    // Shared-memory writes are ABS_WRITE; a trigger such as Core RUN must not
    // be retried. The transport owns the per-kind retry policy.
    virtual WriteResult write32_trigger(std::uint32_t address,
                                        std::uint32_t value,
                                        bool wide) noexcept = 0;
    virtual BlockReadResult read_block_wide(std::uint32_t address,
                                            std::size_t length) noexcept = 0;
    virtual WriteResult write_block_wide(std::uint32_t address,
                                         const std::uint8_t* data,
                                         std::size_t length) noexcept = 0;
};

// Native bridge over the existing Transport. Only 24bit, 4B-aligned reads are
// issued before Widen is active. The Transport owns the single PURE_READ retry;
// this class neither retries nor issues writes on the pre-Widen path.
class Bridge final : public Ep0Io {
public:
    explicit Bridge(usb::Transport& transport) noexcept : transport_(transport) {}
    ReadResult read32(std::uint32_t address) noexcept override;
    ReadResult read32_wide(std::uint32_t address) noexcept override;
    WriteResult write32(std::uint32_t address, std::uint32_t value,
                        bool wide) noexcept override;
    WriteResult write32_trigger(std::uint32_t address, std::uint32_t value,
                                bool wide) noexcept override;
    BlockReadResult read_block_wide(std::uint32_t address,
                                    std::size_t length) noexcept override;
    WriteResult write_block_wide(std::uint32_t address,
                                 const std::uint8_t* data,
                                 std::size_t length) noexcept override;

private:
    usb::Transport& transport_;
};

}  // namespace torne::widen

#endif  // TORNE_WIDEN_BRIDGE_H
