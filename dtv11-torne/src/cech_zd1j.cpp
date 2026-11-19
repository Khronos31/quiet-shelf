/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <vector>

#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>
#include <dtv/spi/firmware_session.hpp>

#include "torne_firmware.hpp"

namespace {

constexpr std::uint32_t kFunction = 0;
constexpr std::uint32_t kWideRequest = 0xBC;
constexpr std::uint32_t kNarrowRequest = 0xBC;
constexpr std::uint32_t kWideWriteType = 0x40;
constexpr std::uint32_t kWideReadType = 0xC0;
constexpr std::uint32_t kNarrowWriteType = 0x40;
constexpr std::uint32_t kNarrowReadType = 0xC0;
constexpr std::uint32_t kWidenBase = 0x000E1000;
constexpr std::size_t kWidenSize = 172;
constexpr std::uint32_t kCore0Base = 0x016A0000;
constexpr std::uint32_t kCore0Hold = 0x01600000;
constexpr std::uint32_t kCore1Base = 0x017A0000;
constexpr std::uint32_t kCore1Hold = 0x01700000;
constexpr std::uint32_t kInfoAddress = 0x017A3800;
constexpr std::uint32_t kControlAddress = 0x017A3840;
constexpr std::uint32_t kStatusAddress = 0x017A3880;
constexpr std::uint32_t kSharedAddress = 0x017A3800;
constexpr std::uint32_t kCore1ImageOffset = 1504;
constexpr std::size_t kCore1ImageSize = 1924;
constexpr std::uint32_t kCore0ImageOffset = 172;
constexpr std::size_t kCore0ImageSize = 1332;
struct AddressValue {
    std::uint32_t address;
    std::uint32_t data;
};

constexpr std::uint32_t kPatchAddresses[]{
    0x000E9AC0, 0x000E9AC4, 0x000E9E0C, 0x000E9E10, 0x000E9D94};
constexpr std::uint32_t kPristinePatchWords[]{
    0x9B800000, 0xAA04E40F, 0x9B800000, 0xAA04E40D, 0xB4279F8D};
constexpr std::uint32_t kCandidatePatchWords[]{
    0x9BEC1020, 0x970C9FA0, 0x9BEC1064, 0x970C9FA0, 0x9FA09F8D};

struct CeChState {
    const dtv_usb_io *io = nullptr;
    std::vector<std::uint8_t> candidate_widen;
};

dtv_internal_result result(int code, std::uint32_t detail = 0)
{
    return dtv_internal_result_make(code, detail);
}

dtv_internal_result create_state(dtv_device_id, dtv_context **out)
{
    if (out == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    auto *state = new (std::nothrow) CeChState;
    if (state == nullptr) return result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
    *out = reinterpret_cast<dtv_context *>(state);
    return result(DTV_INTERNAL_RESULT_OK);
}

void destroy_state(dtv_context *context)
{
    delete reinterpret_cast<CeChState *>(context);
}

dtv_internal_result open_device(dtv_context *context, const dtv_usb_io *io,
                                const dtv_open_args *args)
{
    auto *state = reinterpret_cast<CeChState *>(context);
    if (state == nullptr || io == nullptr || io->control == nullptr || args == nullptr ||
        args->function != kFunction)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    state->io = io;
    state->candidate_widen.clear();
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result close_device(dtv_context *context, dtv_timeout)
{
    auto *state = reinterpret_cast<CeChState *>(context);
    if (state == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    state->io = nullptr;
    state->candidate_widen.clear();
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_timeout io_timeout(const dtv_usb_io *io)
{
    dtv_timeout timeout{};
    timeout.timeout_ms = 1000;
    if (io != nullptr && io->clock != nullptr && io->clock->monotonic_now_ns != nullptr)
        timeout.operation_deadline_ns = io->clock->monotonic_now_ns(io->clock->user) + 1000000000;
    return timeout;
}

std::uint16_t packed_value(std::uint32_t address)
{
    return static_cast<std::uint16_t>((((address >> 16) & 0xFF) << 8) |
                                      ((address >> 24) & 0xFF));
}

dtv_internal_result control_read(CeChState *state, std::uint32_t address,
                                 std::span<std::uint8_t> bytes, bool wide)
{
    if (state == nullptr || state->io == nullptr || state->io->control == nullptr ||
        bytes.empty() || address > 0x01FFFFFF || bytes.size() > 0x02000000 - address)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    dtv_buffer buffer{bytes.data(), bytes.size(), 0};
    const auto transferred = state->io->control(state->io->user,
        wide ? kWideReadType : kNarrowReadType,
        wide ? kWideRequest : kNarrowRequest, packed_value(address),
        static_cast<std::uint16_t>(address & 0xFFFF), &buffer, io_timeout(state->io));
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if (buffer.size != bytes.size()) return result(DTV_INTERNAL_RESULT_USB_IO, buffer.size);
    return transferred;
}

dtv_internal_result control_write(CeChState *state, std::uint32_t address,
                                  std::span<const std::uint8_t> bytes, bool wide)
{
    if (state == nullptr || state->io == nullptr || state->io->control == nullptr ||
        bytes.empty() || address > 0x01FFFFFF || bytes.size() > 0x02000000 - address)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    dtv_buffer buffer{const_cast<std::uint8_t *>(bytes.data()), bytes.size(), bytes.size()};
    return state->io->control(state->io->user,
        wide ? kWideWriteType : kNarrowWriteType,
        wide ? kWideRequest : kNarrowRequest, packed_value(address),
        static_cast<std::uint16_t>(address & 0xFFFF), &buffer, io_timeout(state->io));
}

std::uint32_t read_be32(const std::uint8_t *bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

void write_be32(std::uint8_t *bytes, std::uint32_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}

dtv_internal_result read_word(CeChState *state, std::uint32_t address, std::uint32_t *value)
{
    std::uint8_t bytes[4]{};
    auto transferred = control_read(state, address, bytes, address >= 0x01000000);
    if (transferred.code == DTV_INTERNAL_RESULT_OK) *value = read_be32(bytes);
    return transferred;
}

dtv_internal_result write_word(CeChState *state, std::uint32_t address, std::uint32_t value)
{
    std::uint8_t bytes[4]{};
    write_be32(bytes, value);
    return control_write(state, address, bytes, address >= 0x01000000);
}

dtv_internal_result write_wide_image(CeChState *state, std::uint32_t address,
                                     std::span<const std::uint8_t> image, bool verify)
{
    if (image.empty() || (image.size() & 3) != 0 || address < 0x01000000 ||
        address > 0x01FFFFFF || image.size() > 0x02000000 - address)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    for (std::size_t offset = 0; offset < image.size();) {
        const auto count = std::min<std::size_t>(64, image.size() - offset);
        const auto block = image.subspan(offset, count);
        auto transferred = control_write(state, address + static_cast<std::uint32_t>(offset), block, true);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        if (verify) {
            std::uint8_t readback[64]{};
            transferred = control_read(state, address + static_cast<std::uint32_t>(offset),
                std::span<std::uint8_t>(readback, count), true);
            if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
            if (!std::equal(block.begin(), block.end(), readback))
                return result(DTV_INTERNAL_RESULT_USB_IO);
        }
        offset += count;
    }
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result read_widen_patches(CeChState *state,
                                       std::uint32_t values[5])
{
    for (std::size_t index = 0; index < sizeof(kPatchAddresses) / sizeof(kPatchAddresses[0]); ++index) {
        auto transferred = read_word(state, kPatchAddresses[index], &values[index]);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result read_widen_image(CeChState *state,
                                     std::uint8_t image[kWidenSize])
{
    for (std::size_t offset = 0; offset < kWidenSize; offset += 4) {
        auto transferred = control_read(state, kWidenBase + static_cast<std::uint32_t>(offset),
            std::span<std::uint8_t>(image + offset, 4), false);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }
    return result(DTV_INTERNAL_RESULT_OK);
}

std::span<const std::uint8_t> default_widen()
{
    return {dtv::spi::firmware_assets::torne_fr30_v1::data, kWidenSize};
}

dtv_internal_result recognize_widen(CeChState *state, bool *pristine, bool *candidate)
{
    std::uint32_t words[5]{};
    auto transferred = read_widen_patches(state, words);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    *pristine = std::equal(words, words + 5, kPristinePatchWords);
    *candidate = false;
    if (*pristine) return result(DTV_INTERNAL_RESULT_OK);
    if (!std::equal(words, words + 5, kCandidatePatchWords)) return result(DTV_INTERNAL_RESULT_OK);

    const auto expected = state->candidate_widen.empty()
        ? default_widen()
        : std::span<const std::uint8_t>(state->candidate_widen);
    if (expected.size() != kWidenSize) return result(DTV_INTERNAL_RESULT_OK);
    std::uint8_t readback[kWidenSize]{};
    transferred = read_widen_image(state, readback);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    *candidate = std::equal(expected.begin(), expected.end(), readback);
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result read_core1_info(CeChState *state, std::uint8_t info[64])
{
    return control_read(state, kInfoAddress, std::span<std::uint8_t>(info, 64), true);
}

bool valid_core1_info(const std::uint8_t info[64])
{
    return read_be32(info) == 0x54493243 &&
           (read_be32(info + 4) >> 16) == 1 &&
           read_be32(info + 8) == 0xFC662BD9 &&
           read_be32(info + 12) == 1 &&
           read_be32(info + 16) != 0 &&
           read_be32(info + 20) == kCore1ImageSize &&
           read_be32(info + 24) == 0x000A3800 &&
           read_be32(info + 28) == 0x00040000;
}

dtv_internal_result identify_device(dtv_context *context, dtv_identify_info *identity,
                                    dtv_timeout)
{
    auto *state = reinterpret_cast<CeChState *>(context);
    if (state == nullptr || state->io == nullptr || identity == nullptr)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    identity->state = DTV_IDENTIFY_UNKNOWN;
    identity->firmware_key = 0;
    bool pristine = false;
    bool candidate = false;
    auto transferred = recognize_widen(state, &pristine, &candidate);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if (pristine) {
        identity->state = DTV_IDENTIFY_LOADER;
        return result(DTV_INTERNAL_RESULT_OK);
    }
    if (!candidate) return result(DTV_INTERNAL_RESULT_OK);
    std::uint8_t info[64]{};
    transferred = read_core1_info(state, info);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    identity->state = valid_core1_info(info) ? DTV_IDENTIFY_RUNTIME : DTV_IDENTIFY_LOADER;
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result delay_ms(CeChState *state, std::uint64_t wake, std::uint64_t deadline)
{
    if (state == nullptr || state->io == nullptr || state->io->clock == nullptr ||
        state->io->clock->delay_until == nullptr)
        return result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    std::uint32_t cancelled = 0;
    return state->io->clock->delay_until(state->io->clock->user, wake, deadline, nullptr, &cancelled);
}

std::uint64_t now_ns(CeChState *state)
{
    if (state == nullptr || state->io == nullptr || state->io->clock == nullptr ||
        state->io->clock->monotonic_now_ns == nullptr)
        return 0;
    return state->io->clock->monotonic_now_ns(state->io->clock->user);
}

dtv_internal_result wait_core1(CeChState *state, std::uint32_t expected_session)
{
    const auto deadline = now_ns(state) + 100000000;
    std::uint8_t info[64]{};
    for (;;) {
        auto transferred = read_core1_info(state, info);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        if (read_be32(info) == 0x54493243) {
            if (!valid_core1_info(info) || read_be32(info + 16) != expected_session)
                return result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            break;
        }
        const auto now = now_ns(state);
        if (now >= deadline) return result(DTV_INTERNAL_RESULT_TIMEOUT);
        transferred = delay_ms(state, std::min(now + 1000000, deadline), deadline);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }

    std::uint32_t heartbeat = 0;
    auto transferred = read_word(state, kStatusAddress + 4, &heartbeat);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    const auto heartbeat_deadline = now_ns(state) + 100000000;
    for (;;) {
        std::uint32_t next = 0;
        transferred = read_word(state, kStatusAddress + 4, &next);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        if (next != heartbeat) break;
        const auto now = now_ns(state);
        if (now >= heartbeat_deadline) return result(DTV_INTERNAL_RESULT_TIMEOUT);
        transferred = delay_ms(state, std::min(now + 1000000, heartbeat_deadline), heartbeat_deadline);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }

    std::uint8_t control[64]{};
    write_be32(control, expected_session);
    write_be32(control + 4, 1);
    write_be32(control + 8, 0);
    transferred = control_write(state, kControlAddress, control, true);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    const auto nop_deadline = now_ns(state) + 100000000;
    for (;;) {
        std::uint32_t ack = 0;
        std::uint32_t command_result = 0;
        transferred = read_word(state, kStatusAddress + 0x10, &ack);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        transferred = read_word(state, kStatusAddress + 0x14, &command_result);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        if (ack == 1) return command_result == 0 ? result(DTV_INTERNAL_RESULT_OK)
                                                 : result(DTV_INTERNAL_RESULT_USB_IO, command_result);
        const auto now = now_ns(state);
        if (now >= nop_deadline) return result(DTV_INTERNAL_RESULT_TIMEOUT);
        transferred = delay_ms(state, std::min(now + 1000000, nop_deadline), nop_deadline);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }
}

dtv_internal_result initialize_i2c_engine(CeChState *state)
{
    std::uint32_t value = 0;
    auto transferred = read_word(state, 0x00FD8010, &value);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    transferred = write_word(state, 0x00FD8010, value | 0x10);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    transferred = read_word(state, 0x00FD8014, &value);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    transferred = write_word(state, 0x00FD8014, value & ~0x10u);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    constexpr AddressValue writes[]{
        {0x00FD80C0, 1}, {0x00FD80C0, 0}, {0x00FD80C4, 0}, {0x00090114, 0x80080000},
        {0x00FD8FF8, 6}, {0x00FD8FF4, 8}, {0x00FD8FE8, 0x3C}};
    for (const auto &[address, data] : writes) {
        transferred = write_word(state, address, data);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }
    return write_word(state, 0x00FD8FE4, 0x40);
}

dtv_internal_result initialize_clocks(CeChState *state)
{
    constexpr AddressValue writes[]{
        {0x00F1010C, 0x82010000}, {0x00F10114, 0x80080000},
        {0x00F10118, 0xFFFF0000}, {0x00F1011C, 0x00060000},
        {0x00F10128, 0x60000000}, {0x00F1012C, 0x40070000}};
    for (const auto &[address, data] : writes) {
        auto transferred = write_word(state, address, data);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result install_widen(CeChState *state, std::span<const std::uint8_t> image)
{
    bool pristine = false;
    bool candidate = false;
    auto transferred = recognize_widen(state, &pristine, &candidate);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if (candidate) {
        std::uint32_t unchanged[5]{};
        transferred = read_widen_patches(state, unchanged);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        if (!std::equal(unchanged, unchanged + 5, kCandidatePatchWords))
            return result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        const auto expected = state->candidate_widen.empty()
            ? default_widen()
            : std::span<const std::uint8_t>(state->candidate_widen);
        state->candidate_widen.assign(expected.begin(), expected.end());
        return result(DTV_INTERNAL_RESULT_OK);
    }
    if (!pristine) return result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    if (image.size() != kWidenSize) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);

    for (std::size_t offset = 0; offset < image.size(); offset += 4) {
        transferred = control_write(state, kWidenBase + static_cast<std::uint32_t>(offset),
            image.subspan(offset, 4), false);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }
    std::uint8_t readback[kWidenSize]{};
    transferred = read_widen_image(state, readback);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if (!std::equal(image.begin(), image.end(), readback))
        return result(DTV_INTERNAL_RESULT_USB_IO);
    std::uint32_t unchanged[5]{};
    transferred = read_widen_patches(state, unchanged);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if (!std::equal(unchanged, unchanged + 5, kPristinePatchWords))
        return result(DTV_INTERNAL_RESULT_UNSUPPORTED);

    for (std::size_t index = 0; index < sizeof(kPatchAddresses) / sizeof(kPatchAddresses[0]); ++index) {
        transferred = write_word(state, kPatchAddresses[index], kCandidatePatchWords[index]);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        std::uint32_t observed = 0;
        transferred = read_word(state, kPatchAddresses[index], &observed);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        if (observed != kCandidatePatchWords[index]) return result(DTV_INTERNAL_RESULT_USB_IO);
    }
    state->candidate_widen.assign(image.begin(), image.end());
    return result(DTV_INTERNAL_RESULT_OK);
}

std::int32_t public_error(const dtv_internal_result &failure)
{
    switch (failure.code) {
    case DTV_INTERNAL_RESULT_INVALID_ARGUMENT: return DTV_ERROR_INVALID_ARGUMENT;
    case DTV_INTERNAL_RESULT_UNSUPPORTED: return DTV_ERROR_UNSUPPORTED;
    case DTV_INTERNAL_RESULT_TIMEOUT: return DTV_ERROR_TIMEOUT;
    case DTV_INTERNAL_RESULT_CANCELLED: return DTV_ERROR_CANCELLED;
    case DTV_INTERNAL_RESULT_DISCONNECTED: return DTV_ERROR_DISCONNECTED;
    case DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED: return DTV_ERROR_RESOURCE_LIMIT;
    case DTV_INTERNAL_RESULT_USB_IO: return DTV_ERROR_USB_IO;
    default: return DTV_ERROR_INTERNAL;
    }
}

dtv_internal_result boot_core1(CeChState *state, std::span<const std::uint8_t> code)
{
    auto transferred = write_word(state, kCore0Hold, 0xFFFFFFFF);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    transferred = write_word(state, kCore1Hold, 0xFFFFFFFF);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;

    std::uint8_t probe[64]{};
    for (std::size_t index = 0; index < 64; ++index)
        probe[index] = static_cast<std::uint8_t>(0xA5 + index);
    transferred = control_write(state, 0x017A3FC0, probe, true);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    std::uint8_t probe_readback[64]{};
    transferred = control_read(state, 0x017A3FC0, probe_readback, true);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if (!std::equal(probe, probe + 64, probe_readback))
        return result(DTV_INTERNAL_RESULT_USB_IO);
    std::uint32_t ignored = 0;
    transferred = read_word(state, 0x00F1010C, &ignored);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;

    std::uint32_t previous_session = 0;
    transferred = read_word(state, kControlAddress, &previous_session);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    std::uint32_t generation = (previous_session >> 16) + 1;
    generation &= 0xFFFF;
    if (generation == 0) generation = 1;
    const auto session = generation << 16;

    transferred = write_word(state, kCore1Hold, 0xFFFFFFFF);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    transferred = write_word(state, 0x017FFFFC, 0x000A0000);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    std::uint8_t zeroes[64]{};
    for (std::size_t offset = 0; offset < 0x800; offset += sizeof(zeroes)) {
        transferred = control_write(state, kSharedAddress + static_cast<std::uint32_t>(offset), zeroes, true);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    }
    std::uint8_t control[64]{};
    write_be32(control, session);
    transferred = control_write(state, kControlAddress, control, true);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    transferred = write_wide_image(state, kCore1Base, code, true);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    // 実行制御portは、コード配置先のCore1領域とは別の0x01700000。
    transferred = write_word(state, kCore1Hold, 0xFFFCFFFF);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    return wait_core1(state, session);
}

std::int32_t firmware_transfer(dtv_context *context,
    const dtv::spi::FirmwareSession *session, dtv_bytes package, dtv_error *)
{
    auto *state = reinterpret_cast<CeChState *>(context);
    constexpr std::size_t package_size = 3428;
    if (state == nullptr || session == nullptr || state->io == nullptr || package.data == nullptr ||
        package.size < package_size)
        return DTV_ERROR_INVALID_ARGUMENT;
    const std::span<const std::uint8_t> bytes(package.data, package_size);
    state->candidate_widen.assign(bytes.begin(), bytes.begin() + kWidenSize);
    auto transferred = install_widen(state, bytes.first(kWidenSize));
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return public_error(transferred);
    transferred = initialize_i2c_engine(state);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return public_error(transferred);
    transferred = initialize_clocks(state);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return public_error(transferred);
    const auto core1 = bytes.subspan(kCore1ImageOffset, kCore1ImageSize);
    transferred = boot_core1(state, core1);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return public_error(transferred);
    const auto core0 = bytes.subspan(kCore0ImageOffset, kCore0ImageSize);
    transferred = write_wide_image(state, kCore0Base, core0, true);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return public_error(transferred);
    return DTV_ERROR_OK;
}

constexpr dtv::spi::model_information::receiver_information receiver_info{
    0, 0, DTV_SYSTEM_ISDB_T, 192, 4, 0, 0, DTV_TS_UNKNOWN};
constexpr dtv::spi::model_information::device_receiver_information device_receivers[]{
    {kFunction, true, &receiver_info, 1}};
constexpr dtv::spi::model_information cech_info{
    "CECH-ZD1J", 0x054C, 0x04B2, device_receivers, 1};

const dtv::spi::DeviceOps bootstrap_ops{
    .open = open_device,
    .identify = identify_device,
    .close = close_device};
const dtv::spi::FunctionEntry functions[]{
    {.function = kFunction,
     .bootstrap_ops = &bootstrap_ops,
     .firmware = nullptr,
     .firmware_count = 0,
     .images = nullptr,
     .image_count = 0,
     .stages = nullptr,
     .stage_count = 0,
     .create_state = create_state,
     .destroy_state = destroy_state}};

}

extern "C" int32_t dtv_model_impl_send_firmware_torne_cech_zd1j(
    dtv_device *device, const uint8_t *firmware, uint64_t size,
    const dtv_call_options *options, dtv_error *error)
{
    const dtv_bytes package{firmware, size};
    return dtv::spi::with_firmware_session(device, package, options, firmware_transfer, error);
}

DTV_PRODUCT(torne, DTV_FEATURES());
DTV_MODEL(torne, cech_zd1j, DTV_FEATURES(), DTV_FIRMWARE(torne_fr30_v1), cech_info);
DTV_DEFINE_MODEL(torne, cech_zd1j);

namespace dtv::spi {

const ModelEntry cech_zd1j_model{
    .model = {"CECH-ZD1J", 0x054C, 0x04B2},
    .firmware = nullptr,
    .firmware_count = 0,
    .create_state = create_state,
    .destroy_state = destroy_state,
    .bootstrap_ops = &bootstrap_ops,
    .functions = functions,
    .function_count = 1,
    .source_stride_bytes = 192,
    .source_sync_offset_bytes = 4,
    .runtime_device_receivers = device_receivers,
    .runtime_device_receiver_count = 1};

}
