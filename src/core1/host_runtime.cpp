// SPDX-License-Identifier: GPL-2.0-only
#include "core1/host_runtime.h"

#include <dtv/userland/detail/sha256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace torne::core1 {
namespace {

constexpr std::uint32_t kBase = 0x01700000U;
constexpr std::uint32_t kCodeLocal = 0x000A0000U;
constexpr std::uint32_t kDataLocal = 0x000A3800U;
constexpr std::uint32_t kCtrlLocal = 0x000A3840U;
constexpr std::uint32_t kStatLocal = 0x000A3880U;
constexpr std::uint32_t kVectorLocal = 0x000FFFFCU;
constexpr std::uint32_t kHoldValue = 0xFFFFFFFFU;
constexpr std::uint32_t kRunValue = 0xFFFCFFFFU;
constexpr std::uint32_t kInfoMagic = 0x54493243U;
constexpr std::uint32_t kAbiMajor = 1U;
constexpr std::uint32_t kCapabilityI2cRing = 0x00000001U;
constexpr std::size_t kDataBytes = 0x800U;
constexpr std::size_t kMaxCodeBytes = 0x3000U;
constexpr std::uint32_t kDeadlineMs = 100U;
constexpr std::uint32_t kHeartbeatGapMs = 1U;

std::uint32_t be32(const std::uint8_t* bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

void put_be32(std::uint8_t* bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void fail_transfer(Result& result, const usb::TransferOutcome& transfer,
                   bool write) noexcept
{
    result.failure = transfer;
    if (transfer.cause == usb::TransferCause::no_device)
        result.error = Error::device_gone;
    else if (transfer.state == usb::TransferState::outcome_unknown)
        result.error = Error::outcome_unknown;
    else
        result.error = write ? Error::write_failed : Error::read_failed;
}

bool valid_image(const Image& image) noexcept
{
    if (image.bytes == nullptr || image.image_bytes == 0U ||
        image.image_bytes > kMaxCodeBytes || (image.image_bytes % 4U) != 0U ||
        image.entry != kCodeLocal ||
        (image.capabilities & kCapabilityI2cRing) == 0U)
        return false;
    dtv::userland::detail::Sha256 digest;
    digest.update(image.bytes, image.image_bytes);
    return digest.finish() == image.sha256;
}

bool read_block(widen::Ep0Io& io, std::uint32_t address, std::size_t length,
                std::array<std::uint8_t, 64U>& output, Result& result,
                Stage stage) noexcept
{
    result.stage = stage;
    const widen::BlockReadResult block = io.read_block_wide(address, length);
    if (!block.ok || block.length != length) {
        fail_transfer(result, block.transfer, false);
        return false;
    }
    std::memcpy(output.data(), block.bytes.data(), length);
    return true;
}

bool write_block(widen::Ep0Io& io, std::uint32_t address,
                 const std::uint8_t* data, std::size_t length, Result& result,
                 Stage stage) noexcept
{
    result.stage = stage;
    const widen::WriteResult write = io.write_block_wide(address, data, length);
    if (!write.ok) {
        fail_transfer(result, write.transfer, true);
        return false;
    }
    return true;
}

bool write_word(widen::Ep0Io& io, std::uint32_t address, std::uint32_t value,
                Result& result, Stage stage) noexcept
{
    result.stage = stage;
    const widen::WriteResult write = io.write32(address, value, true);
    if (!write.ok) {
        fail_transfer(result, write.transfer, true);
        return false;
    }
    return true;
}

bool write_ctrl(widen::Ep0Io& io, std::uint32_t session, std::uint32_t cmd_seq,
                Result& result, Stage stage) noexcept
{
    std::array<std::uint8_t, 64U> block{};
    put_be32(block.data(), session);
    put_be32(block.data() + 4U, cmd_seq);
    put_be32(block.data() + 8U, 0U);
    return write_block(io, kBase + kCtrlLocal, block.data(), block.size(),
                       result, stage);
}

bool load_code(widen::Ep0Io& io, const Image& image, Result& result) noexcept
{
    for (std::size_t offset = 0U; offset < image.image_bytes; offset += 64U) {
        const std::size_t length =
            image.image_bytes - offset < 64U ? image.image_bytes - offset : 64U;
        if (!write_block(io, kBase + kCodeLocal + static_cast<std::uint32_t>(offset),
                         image.bytes + offset, length, result, Stage::code_write))
            return false;
    }
    for (std::size_t offset = 0U; offset < image.image_bytes; offset += 64U) {
        const std::size_t length =
            image.image_bytes - offset < 64U ? image.image_bytes - offset : 64U;
        std::array<std::uint8_t, 64U> readback{};
        if (!read_block(io, kBase + kCodeLocal + static_cast<std::uint32_t>(offset),
                        length, readback, result, Stage::code_readback))
            return false;
        if (std::memcmp(readback.data(), image.bytes + offset, length) != 0) {
            result.error = Error::readback_mismatch;
            return false;
        }
    }
    return true;
}

bool read_info(widen::Ep0Io& io, std::array<std::uint32_t, 8U>& words,
               Result& result) noexcept
{
    std::array<std::uint8_t, 64U> block{};
    if (!read_block(io, kBase + kDataLocal, block.size(), block, result,
                    Stage::info))
        return false;
    for (std::size_t index = 0U; index < words.size(); ++index)
        words[index] = be32(block.data() + index * 4U);
    return true;
}

bool read_stat(widen::Ep0Io& io, std::array<std::uint32_t, 8U>& words,
               Result& result, Stage stage) noexcept
{
    std::array<std::uint8_t, 64U> block{};
    if (!read_block(io, kBase + kStatLocal, block.size(), block, result, stage))
        return false;
    for (std::size_t index = 0U; index < words.size(); ++index)
        words[index] = be32(block.data() + index * 4U);
    return true;
}

bool validate_info(const std::array<std::uint32_t, 8U>& info,
                   const Image& image, std::uint32_t session,
                   Result& result) noexcept
{
    if ((info[1] >> 16U) != kAbiMajor) {
        result.error = Error::abi_mismatch;
        return false;
    }
    if (info[2] != image.build_id) {
        result.error = Error::build_id_mismatch;
        return false;
    }
    if (info[4] != session) {
        result.error = Error::session_mismatch;
        return false;
    }
    if (info[5] != image.image_bytes) {
        result.error = Error::image_mismatch;
        return false;
    }
    if (info[6] != kDataLocal) {
        result.error = Error::data_base_mismatch;
        return false;
    }
    if (info[3] != image.capabilities) {
        result.error = Error::capability_mismatch;
        return false;
    }
    return true;
}

bool wait_for_info(widen::Ep0Io& io, const Image& image, std::uint32_t session,
                   Clock& clock, Result& result) noexcept
{
    const std::uint64_t start = clock.monotonic_ms();
    while (true) {
        std::array<std::uint32_t, 8U> info{};
        if (!read_info(io, info, result)) return false;
        if (clock.monotonic_ms() - start >= kDeadlineMs) {
            result.error = Error::info_timeout;
            return false;
        }
        if (info[0] == kInfoMagic) {
            return validate_info(info, image, session, result);
        }
        clock.sleep_ms(1U);
    }
}

bool wait_for_heartbeat(widen::Ep0Io& io, Clock& clock, Result& result) noexcept
{
    std::array<std::uint32_t, 8U> stat{};
    if (!read_stat(io, stat, result, Stage::heartbeat)) return false;
    const std::uint32_t first = stat[1];
    const std::uint64_t start = clock.monotonic_ms();
    while (true) {
        if (clock.monotonic_ms() - start >= kDeadlineMs) {
            result.error = Error::heartbeat_stalled;
            return false;
        }
        clock.sleep_ms(kHeartbeatGapMs);
        if (!read_stat(io, stat, result, Stage::heartbeat)) return false;
        if (stat[1] != first) {
            if (clock.monotonic_ms() - start >= kDeadlineMs) {
                result.error = Error::heartbeat_stalled;
                return false;
            }
            return true;
        }
    }
}

bool run_nop(widen::Ep0Io& io, std::uint32_t session, Clock& clock,
             Result& result) noexcept
{
    constexpr std::uint32_t kCmdSeq = 1U;
    if (!write_ctrl(io, session, kCmdSeq, result, Stage::nop)) return false;
    const std::uint64_t start = clock.monotonic_ms();
    while (true) {
        std::array<std::uint32_t, 8U> stat{};
        if (!read_stat(io, stat, result, Stage::nop)) return false;
        if (stat[4] == kCmdSeq) {
            if (stat[5] != 0U) {
                result.error = Error::nop_failed;
                return false;
            }
            if (clock.monotonic_ms() - start >= kDeadlineMs) {
                result.error = Error::nop_timeout;
                return false;
            }
            return true;
        }
        if (clock.monotonic_ms() - start >= kDeadlineMs) {
            result.error = Error::nop_timeout;
            return false;
        }
        clock.sleep_ms(1U);
    }
}

}  // namespace

std::uint32_t next_session(std::uint32_t previous) noexcept
{
    const std::uint32_t high = previous >> 16U;
    const std::uint32_t next = high == 0xFFFFU ? 1U : high + 1U;
    return next << 16U;
}

Result Runtime::start(widen::Ep0Io& io, const Image& image,
                      bool widen_verified, Clock& clock) noexcept
{
    Result result;
    result.stage = Stage::validate;
    if (!widen_verified) {
        result.error = Error::widen_not_verified;
        return result;
    }
    if (!valid_image(image)) {
        result.error = Error::invalid_argument;
        return result;
    }
    const std::uint32_t session = next_session(last_session_);
    last_session_ = session;
    result.session = session;

    if (!write_word(io, kBase, kHoldValue, result, Stage::hold)) return result;
    if (!write_word(io, kBase + kVectorLocal, kCodeLocal, result, Stage::vector))
        return result;
    std::array<std::uint8_t, 64U> zeros{};
    for (std::size_t offset = 0U; offset < kDataBytes; offset += zeros.size()) {
        if (!write_block(io, kBase + kDataLocal + static_cast<std::uint32_t>(offset),
                         zeros.data(), zeros.size(), result, Stage::zero_data))
            return result;
    }
    if (!write_ctrl(io, session, 0U, result, Stage::ctrl)) return result;
    if (!load_code(io, image, result)) return result;

    result.stage = Stage::run;
    const widen::WriteResult run = io.write32_trigger(kBase, kRunValue, true);
    if (!run.ok) {
        fail_transfer(result, run.transfer, true);
        if (run.transfer.state == usb::TransferState::outcome_unknown)
            result.error = Error::run_outcome_unknown;
        return result;
    }
    if (!wait_for_info(io, image, session, clock, result)) return result;
    if (!wait_for_heartbeat(io, clock, result)) return result;
    if (!run_nop(io, session, clock, result)) return result;

    result.ok = true;
    result.stage = Stage::done;
    return result;
}

Result Runtime::stop(widen::Ep0Io& io) noexcept
{
    Result result;
    if (!write_word(io, kBase, kHoldValue, result, Stage::hold)) return result;
    result.ok = true;
    result.stage = Stage::hold;
    return result;
}

}  // namespace torne::core1
