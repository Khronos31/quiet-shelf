// SPDX-License-Identifier: GPL-2.0-only
#include "core0/host_runtime.h"

#include <dtv/userland/detail/sha256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace torne::core0 {
namespace {

constexpr std::uint32_t kBase = 0x01600000U;
constexpr std::uint32_t kCodeLocal = 0x000A0000U;
constexpr std::uint32_t kDataLocal = 0x000A3800U;
constexpr std::uint32_t kCtrlLocal = 0x000A3840U;
constexpr std::uint32_t kStatLocal = 0x000A3880U;
constexpr std::uint32_t kVectorLocal = 0x000FFFFCU;
constexpr std::uint32_t kHold = 0xFFFFFFFFU;
constexpr std::uint32_t kRun = 0xFFFCFFFFU;
constexpr std::uint32_t kInfoMagic = 0x54545330U;
constexpr std::uint32_t kAbiMajor = 1U;
constexpr std::uint32_t kTsRing = 0x00000001U;
constexpr std::size_t kDataBytes = 0x800U;
constexpr std::size_t kMaxCodeBytes = 0x3000U;
constexpr std::uint32_t kDeadlineMs = 100U;
constexpr std::uint32_t kHeartbeatGapMs = 1U;
constexpr std::uint32_t kRingBase = 0x00AE4000U;
constexpr std::uint32_t kRingSize = 0x00600000U;
constexpr std::uint32_t kUnitSize = 192U;
constexpr std::uint32_t kWriterOffsetReg = 0x00D02038U;
constexpr std::uint32_t kReadLimitReg = 0x00D0202CU;
constexpr std::uint32_t kMargin = 8U;
constexpr std::uint32_t kReportEvery = 10U;

std::uint32_t be32(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

void put32(std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v >> 24U);
    p[1] = static_cast<std::uint8_t>(v >> 16U);
    p[2] = static_cast<std::uint8_t>(v >> 8U);
    p[3] = static_cast<std::uint8_t>(v);
}

void fail_transfer(Result& result, const usb::TransferOutcome& transfer,
                   bool write) noexcept
{
    result.failure = transfer;
    if (transfer.cause == usb::TransferCause::no_device)
        result.error = Error::device_gone;
    else if (transfer.state == usb::TransferState::outcome_unknown)
        result.error = write ? Error::write_failed : Error::read_failed;
    else
        result.error = write ? Error::write_failed : Error::read_failed;
}

bool valid_image(const Image& image) noexcept
{
    if (image.bytes == nullptr || image.image_bytes == 0U ||
        image.image_bytes > kMaxCodeBytes || (image.image_bytes % 4U) != 0U ||
        image.entry != kCodeLocal || (image.capabilities & kTsRing) == 0U)
        return false;
    dtv::userland::detail::Sha256 digest;
    digest.update(image.bytes, image.image_bytes);
    return digest.finish() == image.sha256;
}

bool write32(widen::Ep0Io& io, std::uint32_t address, std::uint32_t value,
             Result& result, Stage stage, bool wide = true) noexcept
{
    result.stage = stage;
    const widen::WriteResult write = io.write32(address, value, wide);
    if (!write.ok) {
        fail_transfer(result, write.transfer, true);
        return false;
    }
    return true;
}

bool write_trigger(widen::Ep0Io& io, std::uint32_t address,
                   std::uint32_t value, Result& result,
                   Stage stage) noexcept
{
    result.stage = stage;
    const widen::WriteResult write = io.write32_trigger(address, value, false);
    if (!write.ok) {
        fail_transfer(result, write.transfer, true);
        if (write.transfer.state == usb::TransferState::outcome_unknown)
            result.error = Error::outcome_unknown;
        return false;
    }
    return true;
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

bool setup_engine(widen::Ep0Io& io, Result& result) noexcept
{
    const std::pair<std::uint32_t, std::uint32_t> values[] = {
        {0x00D000C4U, 0x00000000U}, {0x00D00000U, 0xFFFFFFFFU},
        {0x00D00004U, 0x001F33FFU}, {0x00D00000U, 0x30030FFFU},
        {0x00D00000U, 0x00030000U}, {0x00D00004U, 0x00003305U},
        {0x00D01040U, 0x00000000U}, {0x00D01064U, 0x00000000U},
        {0x00D010A0U, 0x00000000U}, {0x00D010A4U, 0x00000000U},
        {0x00D010A8U, 0x00000000U}, {0x00D01020U, 0x02000000U},
        {0x00D01004U, 0x00000000U}, {0x00D01044U, 0x00BC0000U},
        {0x00D01048U, 0x47000000U}, {0x00D0104CU, 0x00FFFFFFU},
        {0x00D01050U, 0x00000000U}, {0x00D01060U, 0x00000000U},
        {0x00D01000U, 0x06070000U}, {0x00D01080U, 0x01000000U},
        {0x00D01088U, 0x00000000U}, {0x00D0108CU, 0x000000FFU},
        {0x00D01084U, 0x00000000U}, {0x00D01090U, 0x01000000U},
        {0x00D02028U, 0x00000000U}, {0x00D0202CU, 0x00000000U},
        {0x00D02030U, 0x00000000U}, {0x00D02038U, 0x00000000U},
        {0x00D02000U, 0x03020300U}, {0x00D02020U, kRingBase},
        {0x00D02024U, kRingSize}, {0x00D00004U, 0x00043305U}};
    for (const auto& value : values) {
        if (!write32(io, value.first, value.second, result, Stage::engine_setup,
                     false))
            return false;
    }
    return true;
}

bool write_ctrl(widen::Ep0Io& io, std::uint32_t session, std::uint32_t epoch,
                Result& result) noexcept
{
    std::array<std::uint8_t, 64U> block{};
    put32(block.data() + 0x00U, session);
    put32(block.data() + 0x04U, 0U);
    put32(block.data() + 0x08U, 0U);
    put32(block.data() + 0x0CU, 0U);
    put32(block.data() + 0x10U, epoch);
    put32(block.data() + 0x14U, kRingBase);
    put32(block.data() + 0x18U, kRingSize);
    put32(block.data() + 0x1CU, kUnitSize);
    put32(block.data() + 0x20U, kWriterOffsetReg);
    put32(block.data() + 0x24U, kReadLimitReg);
    put32(block.data() + 0x28U, kMargin);
    put32(block.data() + 0x2CU, kReportEvery);
    return write_block(io, kBase + kCtrlLocal, block.data(), block.size(),
                       result, Stage::ctrl);
}

bool setup_usb(widen::Ep0Io& io, Clock& clock, Result& result) noexcept
{
    const widen::ReadResult enable = io.read32(0x00FD8010U);
    if (!enable.ok) {
        fail_transfer(result, enable.transfer, false);
        return false;
    }
    if (!write32(io, 0x00FD8010U, enable.value | 0x000F0000U, result,
                 Stage::usb_setup, false))
        return false;
    const widen::ReadResult reset = io.read32(0x00FD8014U);
    if (!reset.ok) {
        fail_transfer(result, reset.transfer, false);
        return false;
    }
    if (!write32(io, 0x00FD8014U, reset.value & ~0x000F0000U, result,
                 Stage::usb_setup, false))
        return false;
    if (!write_trigger(io, 0x00FD8200U, 1U, result, Stage::usb_setup) ||
        !write_trigger(io, 0x00FD8220U, 1U, result, Stage::usb_setup) ||
        !write_trigger(io, 0x00FD8200U, 0U, result, Stage::usb_setup) ||
        !write32(io, 0x00FD8204U, 0xB09F0000U, result, Stage::usb_setup, false) ||
        !write_trigger(io, 0x00FD8200U, 2U, result, Stage::usb_setup) ||
        !write_trigger(io, 0x00FD8220U, 0U, result, Stage::usb_setup) ||
        !write32(io, 0x00FD8224U, 0xB09F2000U, result, Stage::usb_setup, false) ||
        !write_trigger(io, 0x00FD8220U, 2U, result, Stage::usb_setup))
        return false;
    if (!write_trigger(io, 0x00FDBD08U, 0x00FF6001U, result, Stage::usb_setup))
        return false;
    const std::uint64_t start = clock.monotonic_ms();
    while (true) {
        const widen::ReadResult state = io.read32(0x00FDBD08U);
        if (!state.ok) {
            fail_transfer(result, state.transfer, false);
            return false;
        }
        if ((state.value & 1U) == 0U) break;
        if (clock.monotonic_ms() - start >= kDeadlineMs) {
            result.error = Error::read_failed;
            return false;
        }
        clock.sleep_ms(1U);
    }
    if (!write32(io, 0x00FDBD08U, 0x00FF6000U, result, Stage::usb_setup, false))
        return false;
    return write32(io, 0x00FDBC14U, 0x00060030U, result, Stage::usb_setup,
                   false);
}

bool open_input(widen::Ep0Io& io, Result& result) noexcept
{
    const std::pair<std::uint32_t, std::uint32_t> values[] = {
        {0x00D01020U, 0U}, {0x00D01040U, 0x03000000U},
        {0x00D010A4U, 0x13170400U}, {0x00D00484U, 0x01000000U},
        {0x00D00488U, 0x206C0000U}};
    for (const auto& value : values) {
        if (!write32(io, value.first, value.second, result, Stage::input_open,
                     false))
            return false;
    }
    return write_trigger(io, 0x00D02010U, 0x02000000U, result,
                         Stage::input_open);
    return true;
}

bool close_input(widen::Ep0Io& io, Result& result) noexcept
{
    const std::pair<std::uint32_t, std::uint32_t> values[] = {
        {0x00D01040U, 0U}, {0x00D01020U, 0x02000000U}};
    for (const auto& value : values) {
        if (!write32(io, value.first, value.second, result, Stage::input_stop,
                     false))
            return false;
    }
    return write_trigger(io, 0x00D02010U, 0x04000000U, result,
                         Stage::input_stop);
}

bool read_info(widen::Ep0Io& io, std::array<std::uint32_t, 8U>& info,
               Result& result) noexcept
{
    std::array<std::uint8_t, 64U> block{};
    if (!read_block(io, kBase + kDataLocal, block.size(), block, result,
                    Stage::info))
        return false;
    for (std::size_t i = 0U; i < info.size(); ++i)
        info[i] = be32(block.data() + i * 4U);
    return true;
}

bool read_stat(widen::Ep0Io& io, std::array<std::uint32_t, 8U>& stat,
               Result& result, Stage stage) noexcept
{
    std::array<std::uint8_t, 64U> block{};
    if (!read_block(io, kBase + kStatLocal, block.size(), block, result, stage))
        return false;
    for (std::size_t i = 0U; i < stat.size(); ++i)
        stat[i] = be32(block.data() + i * 4U);
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
            if (info[7] != image.param0) {
                result.error = Error::capability_mismatch;
                return false;
            }
            return true;
        }
        clock.sleep_ms(1U);
    }
}

bool wait_for_stat(widen::Ep0Io& io, std::uint32_t epoch, Clock& clock,
                   Result& result) noexcept
{
    std::array<std::uint32_t, 8U> stat{};
    const std::uint64_t start = clock.monotonic_ms();
    bool have_heartbeat = false;
    std::uint32_t first = 0U;
    while (true) {
        if (!read_stat(io, stat, result, Stage::stat)) return false;
        if (stat[0] == 0xE0U) {
            result.error = Error::state_mismatch;
            return false;
        }
        if (stat[0] == 3U) {
            if (stat[6] != epoch) {
                result.error = Error::epoch_mismatch;
                return false;
            }
            if (!have_heartbeat) {
                first = stat[1];
                have_heartbeat = true;
            } else if (stat[1] != first) {
                return true;
            }
        }
        if (clock.monotonic_ms() - start >= kDeadlineMs) {
            result.error = Error::state_mismatch;
            return false;
        }
        clock.sleep_ms(kHeartbeatGapMs);
    }
}

}  // namespace

std::uint32_t next_session(std::uint32_t previous) noexcept
{
    const std::uint32_t high = previous >> 16U;
    const std::uint32_t next = high == 0xFFFFU ? 1U : high + 1U;
    return next << 16U;
}

Result Runtime::start(widen::Ep0Io& io, usb::Transport& transport,
                      const Image& image, std::uint32_t epoch,
                      bool widen_verified, I2cFlush& flush, Clock& clock,
                      TsPump& pump, const StartOptions& options) noexcept
{
    (void)transport;
    Result result;
    result.stage = Stage::validate;
    result.epoch = epoch;
    if (!widen_verified) {
        result.error = Error::widen_not_verified;
        return result;
    }
    if (!valid_image(image)) {
        result.error = Error::invalid_argument;
        return result;
    }
    result.stage = Stage::i2c_flush;
    if (!flush.flush()) {
        result.error = Error::i2c_flush_failed;
        return result;
    }
    if (!setup_engine(io, result)) return result;
    const std::uint32_t session = next_session(last_session_);
    last_session_ = session;
    result.session = session;
    if (!write32(io, kBase, kHold, result, Stage::hold)) return result;
    if (!write32(io, kBase + kVectorLocal, kCodeLocal, result, Stage::vector))
        return result;
    std::array<std::uint8_t, 64U> zeros{};
    for (std::size_t offset = 0U; offset < kDataBytes; offset += zeros.size()) {
        if (!write_block(io, kBase + kDataLocal + static_cast<std::uint32_t>(offset),
                         zeros.data(), zeros.size(), result, Stage::zero_data))
            return result;
    }
    if (!write_ctrl(io, session, epoch, result)) return result;
    if (!load_code(io, image, result)) return result;
    if (!setup_usb(io, clock, result)) return result;
    result.stage = Stage::run;
    const widen::WriteResult run = io.write32_trigger(kBase, kRun, true);
    if (!run.ok) {
        fail_transfer(result, run.transfer, true);
        if (run.transfer.state == usb::TransferState::outcome_unknown)
            result.error = Error::run_outcome_unknown;
        return result;
    }
    if (!wait_for_info(io, image, session, clock, result)) return result;
    if (!wait_for_stat(io, epoch, clock, result)) return result;
    result.stage = Stage::bulk_start;
    if (pump.start(epoch) != 0) {
        result.error = Error::bulk_start_failed;
        return result;
    }
    if (!options.open_input) {
        result.ok = true;
        result.stage = Stage::bulk_start;
        return result;
    }
    if (!open_input(io, result)) {
        (void)pump.stop();
        return result;
    }
    result.input_open = true;
    result.ok = true;
    result.stage = Stage::done;
    return result;
}

Result Runtime::stop(widen::Ep0Io& io, TsPump& pump) noexcept
{
    Result result;
    Error first_error = Error::none;
    usb::TransferOutcome first_failure{};
    Stage first_stage = Stage::hold_stop;
    const auto remember = [&]() noexcept {
        if (first_error == Error::none) {
            first_error = result.error;
            first_failure = result.failure;
            first_stage = result.stage;
        }
    };
    if (!write32(io, kBase, kHold, result, Stage::hold_stop)) remember();
    if (!close_input(io, result)) remember();
    result.stage = Stage::bulk_stop;
    if (pump.stop() != 0) {
        result.error = Error::bulk_stop_failed;
        remember();
    }
    if (first_error != Error::none) {
        result.ok = false;
        result.error = first_error;
        result.failure = first_failure;
        result.stage = first_stage;
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace torne::core0
