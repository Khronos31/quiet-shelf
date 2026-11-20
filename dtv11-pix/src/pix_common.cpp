/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix_dt295_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

#include <dtv/internal/firmware.hpp>

namespace dtv::pix::detail {

namespace {

constexpr int32_t kOk = DTV_INTERNAL_RESULT_OK;
constexpr int32_t kInvalid = DTV_INTERNAL_RESULT_INVALID_ARGUMENT;
constexpr int32_t kUnsupported = DTV_INTERNAL_RESULT_UNSUPPORTED;
constexpr int32_t kUsbIo = DTV_INTERNAL_RESULT_USB_IO;
constexpr int32_t kTimeout = DTV_INTERNAL_RESULT_TIMEOUT;
constexpr int32_t kCancelled = DTV_INTERNAL_RESULT_CANCELLED;
constexpr int32_t kInternal = DTV_INTERNAL_RESULT_INTERNAL;
constexpr int32_t kNotReady = DTV_INTERNAL_RESULT_NOT_READY;
constexpr uint32_t kFirmwareKey = 25;

constexpr uint32_t kMainMarker = 0xE1800;
constexpr uint32_t kMainHeartbeat = 0xE1804;
constexpr uint32_t kMainCommand = 0xE1808;
constexpr uint32_t kMainArgument = 0xE180C;
constexpr uint32_t kMainResult = 0xE1810;
constexpr uint32_t kMainAddress = 0xE1814;
constexpr uint32_t kSecondaryMarker = 0xDA0000;
constexpr uint32_t kSecondaryHeartbeat = 0xDA0004;
constexpr uint32_t kSecondaryCommand = 0xDA0008;
constexpr uint32_t kSecondaryArgument = 0xDA000C;
constexpr uint32_t kSecondaryStatus = 0xDA0018;

dtv_internal_result make_result(int32_t code, uint32_t detail = 0)
{
    return dtv_internal_result_make(code, detail);
}

bool succeeded(dtv_internal_result result)
{
    return result.code == kOk;
}

uint32_t load_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint16_t load_be16(const uint8_t *data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

void store_be32(uint8_t *data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

void store_be16(uint8_t *data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

dtv_internal_result now_ns(const dtv_usb_io &io, uint64_t *value)
{
    if (io.clock == nullptr || io.clock->monotonic_now_ns == nullptr || value == nullptr) {
        return make_result(kUnsupported);
    }
    *value = io.clock->monotonic_now_ns(io.clock->user);
    return make_result(kOk);
}

dtv_internal_result bounded_timeout(const dtv_usb_io &io, dtv_timeout input,
                                    uint64_t budget_ms, dtv_timeout *output)
{
    uint64_t now = 0;
    auto result = now_ns(io, &now);
    if (!succeeded(result) || output == nullptr) {
        return succeeded(result) ? make_result(kInvalid) : result;
    }
    const uint64_t budget_ns = budget_ms > (UINT64_MAX - now) / 1000000
        ? UINT64_MAX : now + budget_ms * 1000000;
    uint64_t deadline = budget_ns;
    if (input.operation_deadline_ns != 0 && input.operation_deadline_ns < deadline) {
        deadline = input.operation_deadline_ns;
    }
    if (input.timeout_ms != 0 && input.timeout_ms != UINT64_MAX) {
        const uint64_t input_ns = input.timeout_ms > (UINT64_MAX - now) / 1000000
            ? UINT64_MAX : now + input.timeout_ms * 1000000;
        if (input_ns < deadline) {
            deadline = input_ns;
        }
    }
    if (now >= deadline) {
        return make_result(kTimeout);
    }
    const uint64_t remaining = deadline - now;
    output->timeout_ms = remaining / 1000000 + (remaining % 1000000 != 0 ? 1 : 0);
    output->operation_deadline_ns = deadline;
    return make_result(kOk);
}

dtv_internal_result delay_ms(const dtv_usb_io &io, dtv_timeout timeout, uint64_t delay_ms)
{
    if (io.clock == nullptr || io.clock->delay_until == nullptr) {
        return make_result(kUnsupported);
    }
    dtv_timeout bounded{};
    auto result = bounded_timeout(io, timeout, delay_ms + 1, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    uint64_t now = 0;
    result = now_ns(io, &now);
    if (!succeeded(result)) {
        return result;
    }
    const uint64_t wake = delay_ms > (UINT64_MAX - now) / 1000000
        ? UINT64_MAX : now + delay_ms * 1000000;
    uint32_t cancelled = 0;
    result = io.clock->delay_until(io.clock->user, wake, bounded.operation_deadline_ns,
                                   nullptr, &cancelled);
    if (!succeeded(result)) {
        return result;
    }
    if (cancelled != 0) {
        return make_result(kTimeout);
    }
    result = now_ns(io, &now);
    if (!succeeded(result)) {
        return result;
    }
    return now >= wake ? make_result(kOk) : make_result(kTimeout);
}

dtv_internal_result read_memory(const dtv_usb_io &io, uint32_t address, uint8_t *data,
                                size_t size, dtv_timeout timeout)
{
    if (address > 0xFFFFFF || size > 0x1000000 - address ||
        (size != 0 && data == nullptr) || io.control == nullptr) {
        return make_result(kInvalid);
    }
    while (size != 0) {
        const auto length = size > 64 ? size_t{64} : size;
        dtv_buffer buffer{data, length, 0};
        const auto current = address;
        const auto request_value = static_cast<uint16_t>(((current >> 16) & 0xFF) << 8);
        const auto request_index = static_cast<uint16_t>(current & 0xFFFF);
        const auto result = io.control(io.user, 0xC0, 0xBC, request_value,
                                       request_index, &buffer, timeout);
        if (!succeeded(result)) {
            return result;
        }
        if (buffer.size != length) {
            return make_result(kUsbIo, static_cast<uint32_t>(buffer.size));
        }
        address += static_cast<uint32_t>(length);
        data += length;
        size -= length;
    }
    return make_result(kOk);
}

dtv_internal_result write_memory(const dtv_usb_io &io, uint32_t address,
                                 const uint8_t *data, size_t size, dtv_timeout timeout)
{
    if (address > 0xFFFFFF || size > 0x1000000 - address ||
        (size != 0 && data == nullptr) || io.control == nullptr) {
        return make_result(kInvalid);
    }
    while (size != 0) {
        const auto length = size > 64 ? size_t{64} : size;
        auto *mutable_data = const_cast<uint8_t *>(data);
        dtv_buffer buffer{mutable_data, length, length};
        const auto current = address;
        const auto request_value = static_cast<uint16_t>(((current >> 16) & 0xFF) << 8);
        const auto request_index = static_cast<uint16_t>(current & 0xFFFF);
        const auto result = io.control(io.user, 0x40, 0xBC, request_value,
                                       request_index, &buffer, timeout);
        if (!succeeded(result)) {
            return result;
        }
        if (buffer.size != length) {
            return make_result(kUsbIo, static_cast<uint32_t>(buffer.size));
        }
        address += static_cast<uint32_t>(length);
        data += length;
        size -= length;
    }
    return make_result(kOk);
}

dtv_internal_result read_u16(const dtv_usb_io &io, uint32_t address, uint16_t *value,
                             dtv_timeout timeout)
{
    uint8_t bytes[2]{};
    const auto result = read_memory(io, address, bytes, sizeof(bytes), timeout);
    if (succeeded(result) && value != nullptr) {
        *value = load_be16(bytes);
    }
    return value == nullptr ? make_result(kInvalid) : result;
}

dtv_internal_result read_u32(const dtv_usb_io &io, uint32_t address, uint32_t *value,
                             dtv_timeout timeout)
{
    uint8_t bytes[4]{};
    const auto result = read_memory(io, address, bytes, sizeof(bytes), timeout);
    if (succeeded(result) && value != nullptr) {
        *value = load_be32(bytes);
    }
    return value == nullptr ? make_result(kInvalid) : result;
}

dtv_internal_result write_u16(const dtv_usb_io &io, uint32_t address, uint16_t value,
                              dtv_timeout timeout)
{
    uint8_t bytes[2]{};
    store_be16(bytes, value);
    return write_memory(io, address, bytes, sizeof(bytes), timeout);
}

dtv_internal_result write_u32(const dtv_usb_io &io, uint32_t address, uint32_t value,
                              dtv_timeout timeout)
{
    uint8_t bytes[4]{};
    store_be32(bytes, value);
    return write_memory(io, address, bytes, sizeof(bytes), timeout);
}

dtv_internal_result wait_zero(const dtv_usb_io &io, uint32_t address,
                              dtv_timeout timeout, uint64_t budget_ms)
{
    dtv_timeout bounded{};
    auto result = bounded_timeout(io, timeout, budget_ms, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    for (;;) {
        uint32_t value = 0;
        result = read_u32(io, address, &value, bounded);
        if (!succeeded(result)) {
            return result;
        }
        if (value == 0) {
            return make_result(kOk);
        }
        uint64_t now = 0;
        result = now_ns(io, &now);
        if (!succeeded(result)) {
            return result;
        }
        if (now >= bounded.operation_deadline_ns) {
            return make_result(kTimeout, value);
        }
    }
}

dtv_internal_result main_command(const dtv_usb_io &io, uint32_t command,
                                 bool has_argument, uint32_t argument,
                                 bool has_address, uint32_t address,
                                 dtv_timeout timeout, uint64_t budget_ms,
                                 uint32_t *command_result)
{
    dtv_timeout bounded{};
    auto result = bounded_timeout(io, timeout, budget_ms, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    uint32_t current_command = 0;
    result = read_u32(io, kMainCommand, &current_command, bounded);
    if (!succeeded(result)) {
        return result;
    }
    if (current_command != 0) {
        return make_result(kTimeout, current_command);
    }
    if (has_argument) {
        result = write_u32(io, kMainArgument, argument, bounded);
        if (!succeeded(result)) {
            return result;
        }
    }
    if (has_address) {
        result = write_u32(io, kMainAddress, address, bounded);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = write_u32(io, kMainCommand, command, bounded);
    if (!succeeded(result)) {
        return result;
    }
    result = wait_zero(io, kMainCommand, bounded, budget_ms);
    if (!succeeded(result)) {
        return result;
    }
    uint32_t value = 0;
    result = read_u32(io, kMainResult, &value, bounded);
    if (succeeded(result) && command_result != nullptr) {
        *command_result = value;
    }
    return result;
}

dtv_internal_result native16_write(const dtv_usb_io &io, uint32_t address,
                                   uint16_t value, dtv_timeout timeout)
{
    uint32_t result_value = 0;
    // 200msは仕様書記載の検証観測値に基づくホスト側上限で、機器の完了保証ではない。
    auto result = main_command(io, 3, true, value, true, address, timeout, 200,
                               &result_value);
    if (!succeeded(result)) {
        return result;
    }
    return result_value == value ? make_result(kOk) : make_result(kInternal, result_value);
}

dtv_internal_result i2c_transfer(const dtv_usb_io &io, uint8_t address,
                                 std::span<const uint8_t> write_data,
                                 uint8_t *read_data, size_t read_size,
                                 dtv_timeout timeout)
{
    if ((write_data.empty() && read_size == 0) || write_data.size() > 16 ||
        read_size > 16 || (read_size != 0 && read_data == nullptr)) {
        return make_result(kInvalid);
    }
    dtv_timeout bounded{};
    // 250msは仕様書記載の検証観測値に基づくホスト側上限で、機器の完了保証ではない。
    auto result = bounded_timeout(io, timeout, 250, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    uint32_t current_command = 0;
    result = read_u32(io, kMainCommand, &current_command, bounded);
    if (!succeeded(result)) {
        return result;
    }
    if (current_command != 0) {
        return make_result(kTimeout, current_command);
    }
    if (!write_data.empty()) {
        result = write_memory(io, 0xE1F20, write_data.data(), write_data.size(), bounded);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = write_u32(io, 0xE1F00, address, bounded);
    if (succeeded(result)) result = write_u32(io, 0xE1F04, static_cast<uint32_t>(write_data.size()), bounded);
    if (succeeded(result)) result = write_u32(io, 0xE1F08, static_cast<uint32_t>(read_size), bounded);
    if (succeeded(result)) result = write_u32(io, kMainCommand, 9, bounded);
    if (!succeeded(result)) {
        return result;
    }
    result = wait_zero(io, kMainCommand, bounded, 250);
    if (!succeeded(result)) {
        return result;
    }
    uint32_t status = 0;
    result = read_u32(io, 0xE1F0C, &status, bounded);
    if (!succeeded(result)) {
        return result;
    }
    if (status != 0) {
        return make_result(kInternal, status);
    }
    return read_size == 0 ? make_result(kOk)
        : read_memory(io, 0xE1F40, read_data, read_size, bounded);
}

dtv_internal_result demod_write(const dtv_usb_io &io, uint8_t reg, uint8_t value,
                                dtv_timeout timeout)
{
    const uint8_t bytes[]{reg, value};
    return i2c_transfer(io, 0x20, bytes, nullptr, 0, timeout);
}

dtv_internal_result demod_read(const dtv_usb_io &io, uint8_t reg, uint8_t *data,
                               size_t size, dtv_timeout timeout)
{
    return i2c_transfer(io, 0x20, std::span<const uint8_t>(&reg, 1), data, size, timeout);
}

dtv_internal_result rf_write(const dtv_usb_io &io, uint8_t reg, uint8_t value,
                             dtv_timeout timeout)
{
    const uint8_t bytes[]{0xFE, 0xC0, reg, value};
    return i2c_transfer(io, 0x20, bytes, nullptr, 0, timeout);
}

dtv_internal_result rf_read(const dtv_usb_io &io, uint8_t reg, uint8_t *value,
                            dtv_timeout timeout)
{
    const uint8_t set_register[]{0xFE, 0xC0, 0xFB, reg};
    auto result = i2c_transfer(io, 0x20, set_register, nullptr, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const uint8_t read_register[]{0xFE, 0xC1};
    return i2c_transfer(io, 0x20, read_register, value, 1, timeout);
}

dtv_internal_result rf_mask_write(const dtv_usb_io &io, uint8_t reg, uint8_t mask,
                                  uint8_t value, dtv_timeout timeout)
{
    uint8_t old = 0;
    auto result = rf_read(io, reg, &old, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return rf_write(io, reg, static_cast<uint8_t>((old & mask) | value), timeout);
}

dtv_internal_result apply_pairs(const dtv_usb_io &io, uint8_t address,
                                 std::span<const std::array<uint8_t, 2>> pairs,
                                 dtv_timeout timeout)
{
    for (const auto &pair : pairs) {
        const uint8_t bytes[]{pair[0], pair[1]};
        const auto result = i2c_transfer(io, address, bytes, nullptr, 0, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    return make_result(kOk);
}

dtv_internal_result apply_rf_pairs(const dtv_usb_io &io,
                                   std::span<const std::array<uint8_t, 2>> pairs,
                                   dtv_timeout timeout)
{
    for (const auto &pair : pairs) {
        const auto result = rf_write(io, pair[0], pair[1], timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    return make_result(kOk);
}

dtv_internal_result demod_mask_write(const dtv_usb_io &io, uint8_t reg,
                                     uint8_t mask, uint8_t value,
                                     dtv_timeout timeout)
{
    uint8_t old = 0;
    auto result = demod_read(io, reg, &old, 1, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(io, reg, static_cast<uint8_t>((old & mask) | value), timeout);
}

}

namespace {

constexpr std::array<std::array<uint8_t, 2>, 22> kTerrestrialDemodInit{{
    {0x01, 0x00}, {0x03, 0x90}, {0x06, 0x80}, {0x1C, 0x20},
    {0x1D, 0x56}, {0x1E, 0x6A}, {0x1F, 0x9A}, {0x23, 0x4D},
    {0x25, 0x00}, {0x26, 0x00}, {0x32, 0x21}, {0x3B, 0x21},
    {0x3C, 0x3F}, {0x70, 0x38}, {0x73, 0x45}, {0x74, 0x11},
    {0x75, 0x02}, {0x76, 0x00}, {0x77, 0x01}, {0x7D, 0xD2},
    {0xE4, 0xF4}, {0xF4, 0x80},
}};

constexpr std::array<std::array<uint8_t, 2>, 26> kTerrestrialRfInit{{
    {0x00, 0x01}, {0x96, 0x00}, {0x00, 0x00}, {0x14, 0x13},
    {0x6D, 0x8A}, {0x6D, 0x0A}, {0xDF, 0x19}, {0x45, 0x1B},
    {0xA9, 0x59}, {0xAA, 0x6A}, {0xBE, 0x4C}, {0xCF, 0x2A},
    {0xD2, 0xF5}, {0xD0, 0x34}, {0x77, 0xE7}, {0x78, 0xE3},
    {0x6F, 0x51}, {0x7B, 0x84}, {0x7C, 0x9F}, {0x56, 0x41},
    {0xCD, 0x64}, {0xC3, 0x2C}, {0x9D, 0x61}, {0xF7, 0x52},
    {0x58, 0x81}, {0xB1, 0x30},
}};

constexpr std::array<std::array<uint8_t, 2>, 28> kTerrestrialRfExtra{{
    {0x0C, 0x00}, {0x13, 0x04}, {0x53, 0xFE}, {0x57, 0x91},
    {0x5A, 0xF9}, {0x5B, 0x18}, {0x5C, 0xF1}, {0x62, 0xC2},
    {0x6E, 0x01}, {0x6F, 0x51}, {0x87, 0x77}, {0x88, 0x55},
    {0x93, 0x22}, {0x97, 0x02}, {0xBA, 0x30}, {0x98, 0xAF},
    {0x9B, 0x20}, {0x9C, 0x1E}, {0xA0, 0x18}, {0xA5, 0x09},
    {0xC2, 0xA9}, {0xC5, 0x7C}, {0xCD, 0xE8}, {0xCE, 0x7F},
    {0xD5, 0x03}, {0xD9, 0x04}, {0xEA, 0x0D}, {0xDC, 0x1C},
}};

constexpr std::array<std::array<uint8_t, 2>, 18> kSatelliteDemodInit{{
    {0x03, 0x00}, {0x04, 0x02}, {0x07, 0x33}, {0x08, 0x11},
    {0x10, 0xB0}, {0x11, 0x00}, {0x13, 0x80}, {0x15, 0x00},
    {0x17, 0x01}, {0x1C, 0x59}, {0x1D, 0xAA}, {0x1E, 0x20},
    {0x1F, 0x19}, {0x85, 0x7A}, {0x87, 0x04}, {0x8E, 0x26},
    {0xA3, 0x91}, {0xA5, 0x40},
}};

dtv_internal_result write_i2c_register(const dtv_usb_io &io, uint8_t address,
                                       uint8_t reg, uint8_t value, dtv_timeout timeout)
{
    const uint8_t bytes[]{reg, value};
    return i2c_transfer(io, address, bytes, nullptr, 0, timeout);
}

dtv_internal_result write_terrestrial_pll(const dtv_usb_io &io, uint32_t hz,
                                          dtv_timeout timeout)
{
    const uint32_t divider = static_cast<uint32_t>(
        (static_cast<uint64_t>(hz) * 64) / 1000000);
    auto result = rf_mask_write(io, 0x0D, 0xFE, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x0F, 0x20, timeout);
    if (succeeded(result)) result = rf_write(io, 0x10, static_cast<uint8_t>(divider), timeout);
    if (succeeded(result)) result = rf_write(io, 0x11, static_cast<uint8_t>(divider >> 8), timeout);
    if (succeeded(result)) result = rf_write(io, 0x00, 0x01, timeout);
    if (succeeded(result)) result = rf_write(io, 0x31, hz < 700000000 ? 0x81 : 0x91, timeout);
    if (succeeded(result)) result = rf_write(io, 0x00, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x7C, hz < 700000000 ? 0x1F : 0x9F, timeout);
    return result;
}

dtv_internal_result i2c_controller_prepare(const dtv_usb_io &io, dtv_timeout timeout)
{
    uint32_t old = 0;
    auto result = read_u32(io, 0xFD8010, &old, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if ((old & 0x10) == 0) {
        uint16_t old_native = 0;
        result = read_u16(io, 0xF10114, &old_native, timeout);
        if (succeeded(result)) {
            result = native16_write(io, 0x00F10114,
                                    static_cast<uint16_t>((old_native & ~0x0030) | 0x0020),
                                    timeout);
        }
        if (succeeded(result)) result = native16_write(io, 0x00F10090, 0, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD8010, old | 0x10, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD8014, old & ~0x10u, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD80C0, 1, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD80C0, 0, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD80C4, 0, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD8FF8, 6, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD8FF4, 0x10, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD8FE4, 0x40, timeout);
        if (succeeded(result)) result = write_u32(io, 0xFD8FE8, 0x3C, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    uint32_t value = 0;
    result = read_u32(io, 0xFD8FE8, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (value != 0xBC) {
        return make_result(kInternal, value);
    }
    result = read_u32(io, 0xFD8FE0, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return (value & 0x80) == 0 ? make_result(kOk) : make_result(kInternal, value);
}

dtv_internal_result initial_rf_reset(const dtv_usb_io &io, dtv_timeout timeout)
{
    uint16_t old_14 = 0;
    uint16_t old_18 = 0;
    auto result = read_u16(io, 0xF10014, &old_14, timeout);
    if (succeeded(result)) result = read_u16(io, 0xF10018, &old_18, timeout);
    if (succeeded(result)) result = native16_write(io, 0x00F10014, old_14 | 0x0002, timeout);
    if (succeeded(result)) result = native16_write(io, 0x00F10018, old_18 & ~0x0002, timeout);
    if (succeeded(result)) result = delay_ms(io, timeout, 20);
    if (succeeded(result)) result = native16_write(io, 0x00F10018, old_18 | 0x0002, timeout);
    if (succeeded(result)) result = delay_ms(io, timeout, 5);
    return result;
}

dtv_internal_result terrestrial_initialization(const dtv_usb_io &io, dtv_timeout timeout)
{
    auto result = apply_pairs(io, 0x20, kTerrestrialDemodInit, timeout);
    if (succeeded(result)) {
        const uint8_t rf_reset[]{0xFE, 0xC0, 0xFF};
        result = i2c_transfer(io, 0x20, rf_reset, nullptr, 0, timeout);
    }
    if (succeeded(result)) result = apply_rf_pairs(io, kTerrestrialRfInit, timeout);
    if (succeeded(result)) result = rf_write(io, 0x00, 0x01, timeout);
    if (succeeded(result)) result = rf_mask_write(io, 0x31, 0x2F, 0xD0, timeout);
    if (succeeded(result)) result = rf_write(io, 0x00, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x01, 0x89, timeout);
    if (succeeded(result)) result = rf_write(io, 0x02, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x6D, 0x0A, timeout);
    if (succeeded(result)) result = rf_write(io, 0x0B, 0x01, timeout);
    if (succeeded(result)) result = rf_mask_write(io, 0x04, 0xC0, 0x01, timeout);
    if (succeeded(result)) result = rf_write(io, 0x05, 0x1B, timeout);
    if (succeeded(result)) result = rf_mask_write(io, 0x08, 0xF2, 0x05, timeout);
    if (succeeded(result)) result = rf_mask_write(io, 0x09, 0x80, 0x42, timeout);
    if (succeeded(result)) result = rf_mask_write(io, 0x5E, 0xEF, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x10, 0x00, timeout);
    if (succeeded(result)) result = apply_rf_pairs(io, kTerrestrialRfExtra, timeout);
    if (succeeded(result)) result = rf_write(io, 0x6D, 0x0A, timeout);
    if (succeeded(result)) result = demod_write(io, 0x03, 0x00, timeout);
    if (succeeded(result)) result = demod_write(io, 0x03, 0x01, timeout);
    if (succeeded(result)) result = delay_ms(io, timeout, 100);
    if (succeeded(result)) result = write_terrestrial_pll(io, 473142857, timeout);
    if (succeeded(result)) result = rf_write(io, 0x0B, 0x01, timeout);
    if (succeeded(result)) result = rf_write(io, 0x12, 0x01, timeout);
    if (succeeded(result)) result = delay_ms(io, timeout, 20);
    if (succeeded(result)) result = rf_write(io, 0x12, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x0B, 0x00, timeout);
    if (succeeded(result)) result = demod_write(io, 0x25, 0x00, timeout);
    if (succeeded(result)) result = demod_write(io, 0x23, 0x4D, timeout);
    if (succeeded(result)) result = demod_write(io, 0x03, 0x90, timeout);
    return result;
}

dtv_internal_result satellite_initialization_and_sleep(const dtv_usb_io &io,
                                                       dtv_timeout timeout)
{
    // 地上波だけを使う場合も、共有フロントエンドを仕様所定の衛星sleep状態にする。
    auto result = write_i2c_register(io, 0x22, 0x01, 0x90, timeout);
    if (succeeded(result)) result = apply_pairs(io, 0x22, kSatelliteDemodInit, timeout);
    if (succeeded(result)) result = delay_ms(io, timeout, 4);
    const uint8_t rf_init[]{0xFE, 0xC0, 0x2A, 0xB9};
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_init, nullptr, 0, timeout);
    const uint8_t rf_block[]{0xFE, 0xC0, 0x06, 0x10, 0x10, 0x00};
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_block, nullptr, 0, timeout);
    const uint8_t rf_lo[]{0xFE, 0xC0, 0x0A, 0x00};
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_lo, nullptr, 0, timeout);
    const uint8_t rf_sleep[]{0xFE, 0xC0, 0x0E, 0x00};
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_sleep, nullptr, 0, timeout);
    const uint8_t rf_sleep_29[]{0xFE, 0xC0, 0x29, 0x70};
    const uint8_t rf_sleep_28[]{0xFE, 0xC0, 0x28, 0x3E};
    const uint8_t rf_sleep_2a[]{0xFE, 0xC0, 0x2A, 0x19};
    const uint8_t rf_sleep_1c[]{0xFE, 0xC0, 0x1C, 0x00};
    const uint8_t rf_sleep_block[]{0xFE, 0xC0, 0x11, 0xC0, 0xA7};
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_init, nullptr, 0, timeout);
    if (succeeded(result)) {
        result = i2c_transfer(io, 0x22, rf_sleep_29, nullptr, 0, timeout);
    }
    if (succeeded(result)) {
        result = i2c_transfer(io, 0x22, rf_sleep_28, nullptr, 0, timeout);
    }
    if (succeeded(result)) {
        result = i2c_transfer(io, 0x22, rf_sleep_2a, nullptr, 0, timeout);
    }
    if (succeeded(result)) {
        result = i2c_transfer(io, 0x22, rf_sleep_1c, nullptr, 0, timeout);
    }
    if (succeeded(result)) {
        result = i2c_transfer(io, 0x22, rf_sleep_block, nullptr, 0, timeout);
    }
    // 6msは仕様の初回準備シーケンスで指定された固定待機時間。
    if (succeeded(result)) result = delay_ms(io, timeout, 6);
    if (succeeded(result)) result = write_i2c_register(io, 0x22, 0x1E, 0x28, timeout);
    if (succeeded(result)) result = write_i2c_register(io, 0x22, 0x0A, 0x00, timeout);
    if (succeeded(result)) result = write_i2c_register(io, 0x22, 0x03, 0x01, timeout);
    if (succeeded(result)) result = write_i2c_register(io, 0x22, 0x17, 0x01, timeout);
    uint8_t reg_1e = 0;
    if (succeeded(result)) {
        const uint8_t reg = 0x1E;
        result = i2c_transfer(io, 0x22, std::span<const uint8_t>(&reg, 1), &reg_1e, 1, timeout);
    }
    if (succeeded(result)) {
        result = write_i2c_register(io, 0x22, 0x1E,
                                    static_cast<uint8_t>((reg_1e & 0xCF) | 0x30), timeout);
    }
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_init, nullptr, 0, timeout);
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_sleep_29, nullptr, 0, timeout);
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_sleep_28, nullptr, 0, timeout);
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_sleep_2a, nullptr, 0, timeout);
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_sleep_1c, nullptr, 0, timeout);
    if (succeeded(result)) result = i2c_transfer(io, 0x22, rf_sleep_block, nullptr, 0, timeout);
    return result;
}

dtv_internal_result issue_lease_command(const dtv_usb_io &io, uint32_t command,
                                        uint32_t argument, dtv_timeout timeout);

dtv_internal_result recover_timed_out_lease_acquire(State &state,
                                                     const dtv_usb_io &io)
{
    // 呼び出し元deadline後もcommand 10の成否を判定するため、新たに最大1秒だけ観測する。
    const dtv_timeout recovery_timeout{1000, 0};
    auto result = wait_zero(io, kMainCommand, recovery_timeout, 1000);
    if (!succeeded(result)) {
        return result;
    }
    uint32_t status = 0;
    result = read_u32(io, kMainResult, &status, recovery_timeout);
    if (!succeeded(result) || status != 0) {
        return succeeded(result) ? make_result(kInternal, status) : result;
    }
    uint32_t owner = 0;
    result = read_u32(io, 0xE1F60, &owner, recovery_timeout);
    if (!succeeded(result) || owner != 1) {
        return succeeded(result) ? make_result(kInternal, owner) : result;
    }

    // command 10の成功とowner=1を確認した場合だけ、解放command 11を一度発行する。
    state.lease = true;
    result = write_u32(io, kMainCommand, 11, recovery_timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = wait_zero(io, kMainCommand, recovery_timeout, 1000);
    if (result.code == kTimeout) {
        // 最初の待機が尽きた後に独立した最大1秒を設け、command 11は再発行せず完了だけ観測する。
        const dtv_timeout recheck_timeout{1000, 0};
        result = wait_zero(io, kMainCommand, recheck_timeout, 1000);
    }
    if (!succeeded(result)) {
        return result;
    }
    // command 11完了後に、元deadlineとは独立した最大1秒でstatusとownerを読み確定する。
    const dtv_timeout observe_timeout{1000, 0};
    result = read_u32(io, kMainResult, &status, observe_timeout);
    if (!succeeded(result) || status != 0) {
        return succeeded(result) ? make_result(kInternal, status) : result;
    }
    result = read_u32(io, 0xE1F60, &owner, observe_timeout);
    if (succeeded(result) && owner == 0) {
        state.lease = false;
    }
    return result;
}

dtv_internal_result acquire_lease(State &state, const dtv_usb_io &io,
                                  dtv_timeout timeout)
{
    const auto result = issue_lease_command(io, 10, 0, timeout);
    if (succeeded(result)) {
        state.lease = true;
    } else if (result.code == kTimeout) {
        // Coreの失敗cleanupに所有状態を伝え、確認済みleaseの解放を続けられるようにする。
        (void)recover_timed_out_lease_acquire(state, io);
    }
    return result;
}

dtv_internal_result issue_lease_command(const dtv_usb_io &io, uint32_t command,
                                        uint32_t argument, dtv_timeout timeout)
{
    dtv_timeout bounded{};
    // 1000msは仕様書の検証観測値に基づくホスト側上限で、完了時間の保証ではない。
    auto result = bounded_timeout(io, timeout, 1000, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    uint32_t current = 0;
    result = read_u32(io, kMainCommand, &current, bounded);
    if (!succeeded(result)) {
        return result;
    }
    if (current != 0) {
        return make_result(kTimeout, current);
    }
    if (command == 10) {
        result = write_u32(io, kMainArgument, argument, bounded);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = write_u32(io, kMainCommand, command, bounded);
    if (succeeded(result)) result = wait_zero(io, kMainCommand, bounded, 1000);
    if (!succeeded(result)) {
        return result;
    }
    uint32_t status = 0;
    result = read_u32(io, kMainResult, &status, bounded);
    return succeeded(result) && status != 0 ? make_result(kInternal, status) : result;
}

dtv_internal_result prepare_hardware(State &state, const dtv_usb_io &io,
                                     dtv_timeout timeout,
                                     const std::atomic<bool> *cancel)
{
    auto result = i2c_controller_prepare(io, timeout);
    if (succeeded(result) && cancel != nullptr && cancel->load()) {
        return make_result(kCancelled);
    }
    if (succeeded(result)) result = initial_rf_reset(io, timeout);
    if (succeeded(result) && cancel != nullptr && cancel->load()) {
        return make_result(kCancelled);
    }
    if (succeeded(result)) result = terrestrial_initialization(io, timeout);
    if (succeeded(result) && cancel != nullptr && cancel->load()) {
        return make_result(kCancelled);
    }
    if (succeeded(result)) result = satellite_initialization_and_sleep(io, timeout);
    if (succeeded(result) && cancel != nullptr && cancel->load()) {
        return make_result(kCancelled);
    }
    if (succeeded(result)) result = acquire_lease(state, io, timeout);
    return result;
}

}

dtv_internal_result plan_tune(dtv_context *context,
                              const dtv::spi::PublicTuneRequest *request,
                              dtv::spi::PublicTunePlan *plan)
{
    if (context == nullptr || request == nullptr || plan == nullptr) {
        return make_result(kInvalid);
    }
    *plan = {};
    if (request->system != DTV_SYSTEM_ISDB_T) {
        return make_result(kOk);
    }
    plan->supported = !request->has_tune_options ||
        (request->tune_options.selector == DTV_TUNE_SELECTOR_NONE &&
         request->tune_options.lna == DTV_LNA_DEFAULT);
    const auto parameters = terrestrial_tune_parameters(request->frequency_hz);
    plan->arguments_valid = parameters.has_value();
    auto *state = static_cast<State *>(context);
    plan->ready = state->device_state == DeviceState::runtime &&
                  state->prepared && state->lease && !state->failed;
    plan->handle_index = 0;
    return make_result(kOk);
}

dtv_internal_result tune_by_frequency(dtv_context *context,
                                      const dtv::spi::PublicTuneRequest *request,
                                      dtv::spi::PublicTuneSession *session)
{
    if (context == nullptr || request == nullptr || session == nullptr ||
        session->io == nullptr || session->io->control == nullptr ||
        session->begin_effect == nullptr) {
        return make_result(kInvalid);
    }
    auto *state = static_cast<State *>(context);
    if (request->system != DTV_SYSTEM_ISDB_T ||
        (request->has_tune_options &&
         (request->tune_options.selector != DTV_TUNE_SELECTOR_NONE ||
          request->tune_options.lna != DTV_LNA_DEFAULT))) {
        return make_result(kUnsupported);
    }
    const auto parameters = terrestrial_tune_parameters(request->frequency_hz);
    if (!parameters.has_value()) {
        return make_result(kInvalid);
    }
    if (state->device_state != DeviceState::runtime || !state->prepared ||
        !state->lease || state->failed) {
        return make_result(kNotReady);
    }

    uint64_t operation_epoch = 0;
    auto result = session->begin_effect(session->user, &operation_epoch);
    if (!succeeded(result)) {
        return result;
    }
    (void)operation_epoch;
    const auto &io = *session->io;
    const auto timeout = request->timeout;
    result = rf_write(io, 0x0B, 0x01, timeout);
    if (succeeded(result)) result = rf_write(io, 0x12, 0x01, timeout);
    if (succeeded(result)) result = delay_ms(io, timeout, 20);
    if (succeeded(result)) result = demod_write(io, 0x03, 0x00, timeout);
    if (succeeded(result)) result = demod_write(io, 0x1D, 0xAA, timeout);
    if (succeeded(result)) result = demod_write(io, 0x1E, 0xAA, timeout);
    if (succeeded(result)) result = demod_write(io, 0x1F, 0xAA, timeout);
    if (succeeded(result)) result = demod_write(io, 0x25, 0x00, timeout);
    if (succeeded(result)) result = demod_write(io, 0x23, 0x4D, timeout);
    if (succeeded(result)) result = rf_mask_write(io, 0x0D, 0xFE, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x0F, 0x20, timeout);
    if (succeeded(result)) result = rf_write(io, 0x10,
        static_cast<uint8_t>(parameters->pll_divider), timeout);
    if (succeeded(result)) result = rf_write(io, 0x11,
        static_cast<uint8_t>(parameters->pll_divider >> 8), timeout);
    if (succeeded(result)) result = rf_write(io, 0x00, 0x01, timeout);
    if (succeeded(result)) result = rf_write(io, 0x31,
        parameters->high_band ? 0x91 : 0x81, timeout);
    if (succeeded(result)) result = rf_write(io, 0x00, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x7C,
        parameters->high_band ? 0x9F : 0x1F, timeout);
    if (succeeded(result)) result = rf_write(io, 0x12, 0x00, timeout);
    if (succeeded(result)) result = rf_mask_write(io, 0x6E, 0xFF, 0x80, timeout);
    if (succeeded(result)) result = rf_mask_write(io, 0x14, 0xDF, 0x00, timeout);
    if (succeeded(result)) result = rf_write(io, 0x12, 0x01, timeout);
    if (succeeded(result)) result = delay_ms(io, timeout, 20);
    if (succeeded(result)) result = rf_mask_write(io, 0x6E, 0x7F, 0x00, timeout);
    if (succeeded(result)) result = demod_mask_write(io, 0x0E, 0x70, 0x01, timeout);
    if (succeeded(result)) result = demod_write(io, 0x0F, 0x10, timeout);
    if (succeeded(result)) result = demod_write(io, 0x01, 0x40, timeout);
    if (succeeded(result)) result = demod_write(io, 0x23, 0x4C, timeout);
    if (succeeded(result)) result = delay_ms(io, timeout, 100);
    if (succeeded(result)) result = demod_write(io, 0x1D, 0x02, timeout);
    if (succeeded(result)) result = demod_write(io, 0x1E, 0x2A, timeout);
    if (succeeded(result)) result = demod_write(io, 0x1F, 0x8A, timeout);
    return result;
}

namespace {

dtv_internal_result read_descriptor(const dtv_usb_io &io, dtv_timeout timeout)
{
    uint32_t command = 0;
    auto result = read_u32(io, kSecondaryCommand, &command, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (command != 0) {
        return make_result(kTimeout, command);
    }
    result = write_u32(io, kSecondaryArgument, 1, timeout);
    if (succeeded(result)) result = write_u32(io, kSecondaryCommand, 3, timeout);
    if (succeeded(result)) result = wait_zero(io, kSecondaryCommand, timeout, 250);
    if (!succeeded(result)) {
        return result;
    }
    uint32_t status = 0;
    result = read_u32(io, kSecondaryStatus, &status, timeout);
    if (!succeeded(result) || status != 0) {
        return succeeded(result) ? make_result(kUnsupported, status) : result;
    }
    std::array<uint8_t, 256> descriptor{};
    result = read_memory(io, 0xE1900, descriptor.data(), descriptor.size(), timeout);
    if (!succeeded(result)) {
        return result;
    }
    const uint32_t expected[]{0x50584331, 256, 1, 7, 25, 295, 0x0FE0FFFF, 0x001F0000};
    for (size_t index = 0; index < std::size(expected); ++index) {
        if (load_be32(descriptor.data() + index * 4) != expected[index]) {
            return make_result(kUnsupported, static_cast<uint32_t>(index));
        }
    }
    return make_result(kOk);
}

struct GuardSpec {
    uint32_t address;
    size_t size;
    const char *sha256;
};

constexpr std::array<GuardSpec, 4> kColdGuards{{
    {0xE0000, 128, "20436911b09fcd010038939a89b3f03be1453166d959653a05e5ad0c54f75ade"},
    {0xE9000, 1088, "ed4141f58ec23aaffcddfbe9f58e247dd5846a8172a76db694240fc61781e5c6"},
    {0xE9E60, 272, "fb3f0b20a8e020543803d6e66070d952b5d2ded2a954f99a3c93ee51449cc5b7"},
    {0xEA3BE, 34, "6a8c4e7309cce353126f84de39f605dcc6415f60e1f27133ee5a2847b7ba23e1"},
}};

dtv_internal_result observe_state(const dtv_usb_io &io, dtv_timeout timeout,
                                  ReadOnlyObservation *observation)
{
    if (observation == nullptr) return make_result(kInvalid);
    *observation = {};
    auto result = read_u16(io, 0x82008, &observation->status, timeout);
    if (!succeeded(result)) return result;

    observation->guards_match = true;
    std::array<uint8_t, 1088> guard{};
    for (const auto &spec : kColdGuards) {
        result = read_memory(io, spec.address, guard.data(), spec.size, timeout);
        if (!succeeded(result)) return result;
        const dtv::internal::FirmwarePackageImageSpec image{0, spec.size, spec.sha256};
        dtv::internal::VerifiedFirmwarePackage verified;
        const auto checked = dtv::internal::load_firmware_package(
            std::span<const uint8_t>(guard.data(), spec.size),
            std::span<const dtv::internal::FirmwarePackageImageSpec>(&image, 1), &verified);
        if (!checked) observation->guards_match = false;
    }

    result = read_u32(io, 0x80020, &observation->source, timeout);
    if (succeeded(result)) result = read_u32(io, 0xC491C, &observation->pointer, timeout);
    if (!succeeded(result)) return result;

    constexpr uint32_t main_addresses[]{kMainMarker, kMainHeartbeat,
                                         kSecondaryMarker, kSecondaryHeartbeat};
    for (size_t sample = 0; sample < 2; ++sample) {
        if (sample == 1) {
            result = delay_ms(io, timeout, 20);
            if (!succeeded(result)) return result;
        }
        uint32_t *values[]{&observation->main_marker[sample],
                           &observation->main_heartbeat[sample],
                           &observation->secondary_marker[sample],
                           &observation->secondary_heartbeat[sample]};
        for (size_t index = 0; index < std::size(main_addresses); ++index) {
            result = read_u32(io, main_addresses[index], values[index], timeout);
            if (!succeeded(result)) return result;
        }
    }
    return make_result(kOk);
}

#if defined(DTV_PIX_TESTING)
thread_local OpenObservationTestHook g_open_observation_test_hook = nullptr;
thread_local void *g_open_observation_test_user = nullptr;
#endif

dtv_internal_result observe_open_state(const dtv_usb_io &io, dtv_timeout timeout,
                                        ReadOnlyObservation *observation)
{
#if defined(DTV_PIX_TESTING)
    if (g_open_observation_test_hook != nullptr) {
        return g_open_observation_test_hook(g_open_observation_test_user, timeout, observation);
    }
#endif
    return observe_state(io, timeout, observation);
}

bool is_runtime_observation(const ReadOnlyObservation &observation)
{
    return observation.main_marker[0] == 0x50495831 &&
           observation.main_marker[1] == 0x50495831 &&
           observation.main_heartbeat[0] != observation.main_heartbeat[1] &&
           observation.secondary_marker[0] == 0x44413031 &&
           observation.secondary_marker[1] == 0x44413031 &&
           observation.secondary_heartbeat[0] != observation.secondary_heartbeat[1];
}

bool is_cold_observation(const ReadOnlyObservation &observation)
{
    return observation.status == 0 && observation.guards_match &&
           observation.source == 0x82000000 &&
           (observation.pointer == 0x82000000 || observation.pointer == 0x82400000) &&
           observation.main_marker[0] == observation.main_marker[1] &&
           observation.main_heartbeat[0] == observation.main_heartbeat[1] &&
           observation.secondary_marker[0] == 0 && observation.secondary_marker[1] == 0 &&
           observation.secondary_heartbeat[0] == 0 && observation.secondary_heartbeat[1] == 0 &&
           observation.main_marker[0] != 0x50495831;
}

dtv_internal_result fail_state(State &state, uint32_t phase, dtv_internal_result cause)
{
    state.failed = true;
    state.failure_phase = phase;
    state.failure_result = cause;
    state.prepared = false;
    auto result = make_result(DTV_INTERNAL_RESULT_NOT_READY,
                              static_cast<uint32_t>(cause.code));
    const char *diagnostic = state.stage_started
        ? "PIX-DT295 firmware stage failed" : "PIX-DT295 state UNKNOWN";
    const size_t diagnostic_size = state.stage_started
        ? sizeof("PIX-DT295 firmware stage failed") - 1
        : sizeof("PIX-DT295 state UNKNOWN") - 1;
    dtv_internal_result_set_diagnostic(&result, diagnostic, diagnostic_size);
    return result;
}

dtv_internal_result fail_open_initialize(State &state, uint32_t phase,
                                         dtv_internal_result cause)
{
    state.failed = true;
    state.failure_phase = phase;
    state.failure_result = cause;
    state.prepared = false;
    return cause;
}

dtv_internal_result identify_runtime(State &state, dtv_identify_info *info,
                                      dtv_timeout timeout)
{
    if (info == nullptr || !state.opened) return make_result(kInvalid);
    info->firmware_key = 0;
    info->state = DTV_IDENTIFY_UNKNOWN;
    if (state.failed) return make_result(DTV_INTERNAL_RESULT_NOT_READY);

    if (state.device_state == DeviceState::runtime) {
        auto result = read_descriptor(state.io, timeout);
        if (!succeeded(result)) return fail_state(state, 2, result);
        info->firmware_key = kFirmwareKey;
        info->state = DTV_IDENTIFY_RUNTIME;
        return make_result(kOk);
    }

    if (state.device_state == DeviceState::cold) {
        info->state = DTV_IDENTIFY_LOADER;
        return make_result(kOk);
    }
    return fail_state(state, 5, make_result(DTV_INTERNAL_RESULT_NOT_READY));
}

dtv_internal_result write_bits_verified(const dtv_usb_io &io, uint32_t address,
                                        uint16_t mask, uint16_t value,
                                        dtv_timeout timeout)
{
    uint16_t current = 0;
    auto result = read_u16(io, address, &current, timeout);
    if (!succeeded(result)) return result;
    const auto next = static_cast<uint16_t>((current & ~mask) | (value & mask));
    result = write_u16(io, address, next, timeout);
    if (!succeeded(result)) return result;
    result = read_u16(io, address, &current, timeout);
    if (!succeeded(result)) return result;
    return (current & mask) == (value & mask) ? make_result(kOk)
                                               : make_result(kInternal, current);
}

dtv_internal_result write_register_verified(const dtv_usb_io &io, uint32_t address,
                                            uint16_t value, dtv_timeout timeout)
{
    auto result = write_u16(io, address, value, timeout);
    if (!succeeded(result)) return result;
    uint16_t readback = 0;
    result = read_u16(io, address, &readback, timeout);
    if (!succeeded(result)) return result;
    return readback == value ? make_result(kOk) : make_result(kInternal, readback);
}

dtv_internal_result wait_bits_set(const dtv_usb_io &io, uint32_t address,
                                  uint16_t mask, dtv_timeout timeout,
                                  uint64_t budget_ms, uint16_t *last_value)
{
    dtv_timeout bounded{};
    auto result = bounded_timeout(io, timeout, budget_ms, &bounded);
    if (!succeeded(result)) return result;
    for (;;) {
        uint16_t value = 0;
        result = read_u16(io, address, &value, bounded);
        if (!succeeded(result)) return result;
        if ((value & mask) == mask) {
            if (last_value != nullptr) *last_value = value;
            return make_result(kOk);
        }
        uint64_t now = 0;
        result = now_ns(io, &now);
        if (!succeeded(result)) return result;
        if (now >= bounded.operation_deadline_ns) {
            if (last_value != nullptr) *last_value = value;
            return make_result(kTimeout, value);
        }
    }
}

dtv_internal_result set_register_bits_sequentially(const dtv_usb_io &io,
                                                   uint32_t address,
                                                   uint8_t highest_bit,
                                                   uint16_t set_mask,
                                                   dtv_timeout timeout)
{
    for (int bit = highest_bit; bit >= 0; --bit) {
        const auto mask = static_cast<uint16_t>(1u << bit);
        auto result = write_bits_verified(io, address, mask,
            (set_mask & mask) != 0 ? mask : 0, timeout);
        if (!succeeded(result)) return result;
    }
    return make_result(kOk);
}

dtv_internal_result verify_cold_before_stage(State &state, dtv_timeout timeout)
{
    ReadOnlyObservation observation{};
    auto result = observe_state(state.io, timeout, &observation);
    if (!succeeded(result)) return result;
    if (!is_cold_observation(observation) ||
        observation.main_marker[0] == 0x50495831 ||
        observation.main_marker[1] == 0x50495831 ||
        observation.secondary_marker[0] == 0x44413031 ||
        observation.secondary_marker[1] == 0x44413031) {
        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    }
    return make_result(kOk);
}

dtv_internal_result confirm_started_firmware(State &state, dtv_timeout timeout)
{
    const auto &io = state.io;
    auto result = delay_ms(io, timeout, 150);
    uint32_t marker = 0;
    if (succeeded(result)) result = read_u32(io, kMainMarker, &marker, timeout);
    if (!succeeded(result)) return result;
    if (marker != 0x50495831) return make_result(kNotReady, marker);

    uint32_t main_heartbeat_before = 0;
    uint32_t secondary_marker = 0;
    uint32_t secondary_heartbeat_before = 0;
    result = read_u32(io, kMainHeartbeat, &main_heartbeat_before, timeout);
    if (succeeded(result)) result = read_u32(io, kSecondaryMarker, &secondary_marker, timeout);
    if (succeeded(result)) result = read_u32(io, kSecondaryHeartbeat,
                                              &secondary_heartbeat_before, timeout);
    if (!succeeded(result)) return result;
    if (secondary_marker != 0x44413031) return make_result(kNotReady, secondary_marker);

    uint32_t echo = 0;
    result = main_command(io, 1, true, 0x12345678, false, 0, timeout, 250, &echo);
    if (!succeeded(result)) return result;
    result = delay_ms(io, timeout, 50);
    uint32_t command = 0;
    uint32_t echo_heartbeat = 0;
    if (succeeded(result)) result = read_u32(io, kMainCommand, &command, timeout);
    if (succeeded(result)) result = read_u32(io, kMainHeartbeat, &echo_heartbeat, timeout);
    if (!succeeded(result)) return result;
    if (command != 0 || echo != 0x2B640E31 ||
        echo_heartbeat == main_heartbeat_before) {
        return make_result(kNotReady, echo);
    }

    result = delay_ms(io, timeout, 20);
    uint32_t main_marker = 0;
    uint32_t main_heartbeat_after = 0;
    uint32_t secondary_marker_after = 0;
    uint32_t secondary_heartbeat_after = 0;
    if (succeeded(result)) result = read_u32(io, kMainMarker, &main_marker, timeout);
    if (succeeded(result)) result = read_u32(io, kMainHeartbeat,
                                              &main_heartbeat_after, timeout);
    if (succeeded(result)) result = read_u32(io, kSecondaryMarker,
                                              &secondary_marker_after, timeout);
    if (succeeded(result)) result = read_u32(io, kSecondaryHeartbeat,
                                              &secondary_heartbeat_after, timeout);
    if (!succeeded(result)) return result;
    if (main_marker != 0x50495831 || secondary_marker_after != 0x44413031 ||
        main_heartbeat_after == echo_heartbeat ||
        secondary_heartbeat_after == secondary_heartbeat_before) {
        return make_result(kNotReady);
    }

    std::array<uint8_t, 0x900> workspace{};
    result = read_memory(io, 0xDA0100, workspace.data(), workspace.size(), timeout);
    if (!succeeded(result)) return result;
    if (std::any_of(workspace.begin(), workspace.end(), [](uint8_t value) { return value != 0; })) {
        return make_result(kNotReady);
    }
    uint32_t dirty = 0;
    result = read_u32(io, 0xDA007C, &dirty, timeout);
    if (!succeeded(result)) return result;
    if (dirty != 0) return make_result(kNotReady, dirty);
    return read_descriptor(io, timeout);
}

bool is_pix_dt295_candidate(const dtv_usb_candidate_info &candidate)
{
    if (candidate.vendor_id != 0x06B8 || candidate.product_id != 0x103B ||
        candidate.interfaces == nullptr) {
        return false;
    }
    const dtv_usb_interface_info *target = nullptr;
    for (uint32_t index = 0; index < candidate.interface_count; ++index) {
        const auto &interface = candidate.interfaces[index];
        if (interface.interface_number != 0 || interface.alternate_setting != 0) continue;
        if (target != nullptr) return false;
        target = &interface;
    }
    if (target == nullptr || target->endpoints == nullptr) return false;
    bool bulk_out = false;
    bool bulk_in = false;
    for (uint32_t index = 0; index < target->endpoint_count; ++index) {
        const auto &endpoint = target->endpoints[index];
        if ((endpoint.attributes & 0x03) != 0x02) continue;
        if (endpoint.address == 0x02) bulk_out = true;
        if (endpoint.address == 0x81) bulk_in = true;
    }
    return bulk_out && bulk_in;
}

dtv_internal_result firmware_stage(dtv_context *context, dtv_bytes image,
                                   dtv_timeout timeout)
{
    auto *state = static_cast<State *>(context);
    if (state == nullptr || !state->opened ||
        (image.size != 0 && image.data == nullptr)) {
        return make_result(kInvalid);
    }
    if (state->failed) return make_result(kNotReady);
    if (image.size == 0 || image.data == nullptr || state->stage_started ||
        image.size > static_cast<uint64_t>(SIZE_MAX - 32) ||
        image.size > static_cast<uint64_t>(UINT32_MAX - 32)) {
        return make_result(kInvalid);
    }
    if (state->device_state == DeviceState::runtime) {
        return make_result(kOk);
    }
    if (state->device_state != DeviceState::cold) return make_result(kNotReady);
    state->stage_started = true;
    uint32_t phase = 1;
    auto stop = [&](dtv_internal_result result) { return fail_state(*state, phase, result); };

    auto result = verify_cold_before_stage(*state, timeout);
    if (!succeeded(result)) return stop(result);

    phase = 2;
    uint32_t pointer = 0;
    result = read_u32(state->io, 0xC491C, &pointer, timeout);
    if (!succeeded(result)) return stop(result);
    if (pointer == 0x82400000) {
        result = write_u32(state->io, 0xC491C, 0x82000000, timeout);
        if (succeeded(result)) result = read_u32(state->io, 0xC491C, &pointer, timeout);
        if (!succeeded(result) || pointer != 0x82000000) {
            return stop(succeeded(result) ? make_result(kNotReady, pointer) : result);
        }
    } else if (pointer != 0x82000000) {
        return stop(make_result(kNotReady, pointer));
    }
    uint32_t source = 0;
    result = read_u32(state->io, 0x80020, &source, timeout);
    if (!succeeded(result) || source != 0x82000000) {
        return stop(succeeded(result) ? make_result(kNotReady, source) : result);
    }

    phase = 3;
    result = write_bits_verified(state->io, 0x90194, 0x7000, 0x1000, timeout);
    if (succeeded(result)) result = write_register_verified(state->io, 0x90198, 0x0040, timeout);
    if (succeeded(result)) result = set_register_bits_sequentially(
        state->io, 0x90078, 13, 0, timeout);
    if (succeeded(result)) result = set_register_bits_sequentially(
        state->io, 0x901B4, 11, 0, timeout);
    if (!succeeded(result)) return stop(result);
    uint16_t ack = 0;
    result = wait_bits_set(state->io, 0x90074, 0x0004, timeout, 1000, &ack);
    if (!succeeded(result)) return stop(result);
    result = write_u16(state->io, 0x90074, static_cast<uint16_t>(ack | 0x0004), timeout);
    if (!succeeded(result)) return stop(result);

    phase = 4;
    std::array<uint8_t, 40> saved_window{};
    result = read_memory(state->io, 0xE9EA4, saved_window.data(), saved_window.size(), timeout);
    if (!succeeded(result)) return stop(result);
    const uint32_t saved_window_address = load_be32(saved_window.data() + (0xE9EBC - 0xE9EA4));
    if (saved_window_address != 0x00F00000) {
        return stop(make_result(kNotReady, saved_window_address));
    }
    result = write_u32(state->io, 0xE9EBC, 0x82000000, timeout);
    uint32_t window_address = 0;
    if (succeeded(result)) result = read_u32(state->io, 0xE9EBC, &window_address, timeout);
    if (!succeeded(result) || window_address != 0x82000000) {
        return stop(succeeded(result) ? make_result(kNotReady, window_address) : result);
    }

    std::vector<uint8_t> transfer(static_cast<size_t>(image.size) + 32, 0);
    std::copy_n(image.data, image.size, transfer.data());
    if (state->io.bulk_out == nullptr) return stop(make_result(kUnsupported));
    result = state->io.bulk_out(state->io.user, 0x02,
                                {transfer.data(), transfer.size()}, timeout);
    if (!succeeded(result) || result.detail != transfer.size()) {
        return stop(succeeded(result) ? make_result(kUsbIo, result.detail) : result);
    }

    std::vector<uint8_t> readback(static_cast<size_t>(image.size));
    result = read_memory(state->io, 0x82000000, readback.data(), readback.size(), timeout);
    if (!succeeded(result)) return stop(result);
    if (!std::equal(readback.begin(), readback.end(), image.data)) {
        return stop(make_result(kNotReady));
    }

    phase = 5;
    result = write_u32(state->io, 0xE9EBC, saved_window_address, timeout);
    if (!succeeded(result)) return stop(result);
    result = read_u32(state->io, 0xE9EBC, &window_address, timeout);
    if (!succeeded(result) || window_address != saved_window_address) {
        return stop(succeeded(result) ? make_result(kNotReady, window_address) : result);
    }
    std::array<uint8_t, 40> restored_window{};
    result = read_memory(state->io, 0xE9EA4, restored_window.data(), restored_window.size(), timeout);
    if (!succeeded(result)) return stop(result);
    if (restored_window != saved_window) return stop(make_result(kNotReady));

    phase = 6;
    result = set_register_bits_sequentially(state->io, 0x90078, 13, 0x3FEF, timeout);
    if (succeeded(result)) result = set_register_bits_sequentially(
        state->io, 0x901B4, 11, 0x0FFF, timeout);
    if (!succeeded(result)) return stop(result);
    uint16_t trigger = 0;
    result = read_u16(state->io, 0x90070, &trigger, timeout);
    if (!succeeded(result)) return stop(result);
    state->trigger_attempted = true;
    result = write_u16(state->io, 0x90070, static_cast<uint16_t>(trigger | 0x0004), timeout);
    if (!succeeded(result)) return stop(result);

    phase = 7;
    result = confirm_started_firmware(*state, timeout);
    if (!succeeded(result)) return stop(result);
    state->device_state = DeviceState::runtime;
    return make_result(kOk);
}
}
dtv_internal_result open(dtv_context *context, const dtv_usb_io *io,
                         const dtv_open_args *args)
{
    auto *state = static_cast<State *>(context);
    if (state == nullptr || io == nullptr || args == nullptr || io->control == nullptr ||
        io->bulk_out == nullptr || io->clock == nullptr ||
        io->clock->monotonic_now_ns == nullptr || io->clock->delay_until == nullptr ||
        args->usb_observation == nullptr || args->model == nullptr || state->opened ||
        args->interface_number != 0 || args->alternate_setting != 0 ||
        args->model->vendor_id != 0x06B8 || args->model->product_id != 0x103B ||
        (args->usb_observation->port_path_length != 0 &&
         args->usb_observation->port_path == nullptr) ||
        !is_pix_dt295_candidate(*args->usb_observation)) {
        return make_result(kInvalid);
    }
    state->io = *io;
    state->open_timeout = args->timeout;
    state->opened = true;
    ReadOnlyObservation observation{};
    auto result = observe_state(state->io, args->timeout, &observation);
    if (!succeeded(result)) return fail_state(*state, 1, result);

    if (is_runtime_observation(observation)) {
        state->device_state = DeviceState::runtime;
        return make_result(kOk);
    }
    const bool runtime_marker_evidence =
        observation.main_marker[0] == 0x50495831 || observation.main_marker[1] == 0x50495831 ||
        observation.secondary_marker[0] == 0x44413031 ||
        observation.secondary_marker[1] == 0x44413031;
    if (is_cold_observation(observation) && !runtime_marker_evidence) {
        state->device_state = DeviceState::cold;
        return make_result(kOk);
    }
    return fail_state(*state, 5, make_result(DTV_INTERNAL_RESULT_NOT_READY));
}

dtv_internal_result initialize_open(
    dtv_context *context, std::span<const dtv::spi::PublicOpenIo> ios,
    dtv_timeout timeout, const std::atomic<bool> *cancel)
{
    auto *state = static_cast<State *>(context);
    if (state == nullptr || state->opened || state->failed || ios.size() != 1 ||
        ios[0].handle_index != 0 || ios[0].io == nullptr ||
        ios[0].io->control == nullptr || ios[0].io->clock == nullptr ||
        ios[0].io->clock->monotonic_now_ns == nullptr ||
        ios[0].io->clock->delay_until == nullptr) {
        return make_result(kInvalid);
    }
    state->prepared = false;
    const auto &io = *ios[0].io;
    if (cancel != nullptr && cancel->load()) {
        return fail_open_initialize(*state, 1, make_result(kCancelled));
    }

    ReadOnlyObservation observation{};
    auto result = observe_state(io, timeout, &observation);
    if (!succeeded(result)) return fail_open_initialize(*state, 1, result);
    if (cancel != nullptr && cancel->load()) {
        return fail_open_initialize(*state, 1, make_result(kCancelled));
    }
    if (!is_runtime_observation(observation)) {
        return fail_open_initialize(*state, 5, make_result(kNotReady));
    }
    result = read_descriptor(io, timeout);
    if (!succeeded(result)) return fail_open_initialize(*state, 2, result);
    if (cancel != nullptr && cancel->load()) {
        return fail_open_initialize(*state, 2, make_result(kCancelled));
    }

    state->device_state = DeviceState::runtime;
    state->opened = true;
    result = prepare_hardware(*state, io, timeout, cancel);
    if (!succeeded(result)) return fail_open_initialize(*state, 6, result);
    state->prepared = true;
    return make_result(kOk);
}

bool identify_requires_initial_data(dtv_context *)
{
    // DT295はcontrol観測で識別する方針。今回のcontextやopen観測には依存しない。
    return false;
}

dtv_internal_result identify_after_initialize(
    dtv_context *context, std::span<const unsigned char>, dtv_timeout,
    const std::atomic<bool> *)
{
    if (context == nullptr) return make_result(kInvalid);
    const auto &state = *static_cast<const State *>(context);
    if (state.failed || state.device_state != DeviceState::runtime ||
        !state.opened || !state.prepared || !state.lease) {
        return make_result(kNotReady);
    }
    return make_result(kOk);
}

dtv_internal_result plan_open(
    std::span<const dtv_usb_candidate_info> candidates,
    std::span<const dtv_function> functions,
    std::span<dtv::spi::PublicOpenPlan> plans, std::size_t *plan_count)
{
    if (plan_count == nullptr) return make_result(kInvalid);
    *plan_count = 0;
    if (plans.empty()) return make_result(kInvalid);
    if (candidates.size() != 1 || functions.size() != 1 || functions[0] != 1) {
        return make_result(kUnsupported);
    }
    if (!is_pix_dt295_candidate(candidates[0])) return make_result(kUnsupported);
    plans[0] = {0, 0x81, 16384, 1, 65536, 1, false, false};
    *plan_count = 1;
    return make_result(kOk);
}

dtv_internal_result identify_shared(
    dtv_context *context, std::span<const unsigned char>, dtv_timeout timeout,
    const std::atomic<bool> *cancel,
    dtv::spi::PublicOpenSession *session)
{
    if (session == nullptr) return make_result(kInvalid);
    // Core92のdata不要経路では未呼出し。sessionや初回TSを作らずStateだけを確認する。
    return identify_after_initialize(context, {}, timeout, cancel);
}

dtv_internal_result select_usb_group(const dtv_usb_candidate_info *candidates,
                                     uint32_t candidate_count, uint32_t anchor,
                                     dtv_usb_function_selection *out,
                                     uint32_t capacity, uint32_t *out_count,
                                     dtv_usb_group_match *match)
{
    if (candidates == nullptr || out_count == nullptr || match == nullptr ||
        anchor >= candidate_count) {
        return make_result(kInvalid);
    }
    *out_count = 0;
    *match = DTV_USB_GROUP_NO_MATCH;
    if (!is_pix_dt295_candidate(candidates[anchor])) return make_result(kOk);
    if (out == nullptr || capacity < 1) return make_result(kInvalid);
    out[0] = {anchor, 1, 0, 0};
    *out_count = 1;
    *match = DTV_USB_GROUP_COMPLETE;
    return make_result(kOk);
}

dtv_internal_result identify(dtv_context *context, dtv_identify_info *info,
                              dtv_timeout timeout)
{
    auto *state = static_cast<State *>(context);
    return state == nullptr ? make_result(kInvalid) : identify_runtime(*state, info, timeout);
}

dtv_internal_result close(dtv_context *context, dtv_timeout timeout)
{
    auto *state = static_cast<State *>(context);
    if (state == nullptr) return make_result(kInvalid);
    if (!state->lease) return make_result(kOk);

    const auto result = issue_lease_command(state->io, 11, 0, timeout);
    if (succeeded(result)) state->lease = false;
    return result;
}

dtv_internal_result firmware(dtv_context *context, dtv_bytes image,
                             dtv_firmware_result *firmware_result, dtv_timeout timeout)
{
    if (firmware_result == nullptr) return make_result(kInvalid);
    const auto result = firmware_stage(context, image, timeout);
    if (succeeded(result)) {
        firmware_result->firmware_key = kFirmwareKey;
        firmware_result->transition = DTV_FIRMWARE_SAME_HANDLE;
    }
    return result;
}

dtv_internal_result create_state(dtv_device_id device, dtv_context **out_context)
{
    if (out_context == nullptr) return make_result(kInvalid);
    *out_context = new State{device};
    return make_result(kOk);
}

void destroy_state(dtv_context *context)
{
    delete static_cast<State *>(context);
}

}
