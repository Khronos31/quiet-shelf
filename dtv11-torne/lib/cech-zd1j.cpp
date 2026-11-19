// SPDX-License-Identifier: GPL-2.0-or-later

#include <dtv/spi/device_ops.hpp>

#include "../firmware/fr30/generated/torne-fr30-v1-metadata.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace {

constexpr uint8_t kRequestTypeIn = 0xC0;
constexpr uint8_t kRequestTypeOut = 0x40;
constexpr uint8_t kBridgeRequest = 0xBC;
constexpr uint32_t kFirmwareKey = 0x544F524E;
constexpr uint32_t kFirmwareStageWiden = 1;
constexpr uint32_t kFirmwareStageCore1 = 2;
constexpr uint32_t kFirmwareStageFrontendPower = 3;
constexpr dtv_function kReceiverFunction = 1;
constexpr dtv_spi_receiver_id kReceiverId = 0;
constexpr std::size_t kWidenSize = 172;
constexpr std::size_t kCore0Size = 1332;
constexpr std::size_t kCore1Size = 1924;

struct DeviceState {
    dtv_device_id device_id = 0;
    dtv_usb_io io{};
    std::array<uint8_t, kCore0Size> core0_image{};
    uint32_t session = 0x00010000;
    uint32_t core0_session = 0;
    uint32_t core0_epoch = 0x00040000;
    uint16_t i2c_sequence = 0;
    uint8_t tuner_revision = 0;
    uint8_t tuner_package = 0;
    uint8_t tuner_calibration_flags = 0;
    uint16_t demod_revision = 0;
    uint16_t tuner_wbd_offset = 0;
    uint8_t tuner_wbd_gain = 0xFF;
    int16_t tuner_temperature = 55;
    uint32_t timf = 20199727;
    uint32_t tuner_rf_khz = 0;
    bool i2c_ready = false;
    bool core1_running = false;
    bool frontend_ready = false;
    bool tuner_p1g = false;
    bool tuner_tuned = false;
    bool core0_stream_active = false;
    bool core0_image_ready = false;
    bool opened = false;
};

dtv_internal_result make_result(int32_t code, uint32_t detail = 0)
{
    return dtv_internal_result_make(code, detail);
}

bool succeeded(dtv_internal_result result)
{
    return result.code == DTV_INTERNAL_RESULT_OK;
}

dtv_internal_result bounded_timeout(DeviceState &state, dtv_timeout timeout,
                                    dtv_timeout *bounded)
{
    if (bounded == nullptr || state.io.clock == nullptr ||
        state.io.clock->monotonic_now_ns == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    *bounded = timeout;
    if (timeout.operation_deadline_ns == 0) {
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    const auto now = state.io.clock->monotonic_now_ns(state.io.clock->user);
    if (now >= timeout.operation_deadline_ns) {
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    const auto remaining_ns = timeout.operation_deadline_ns - now;
    const auto remaining_ms = (remaining_ns + 999999) / 1000000;
    if (bounded->timeout_ms == 0 || bounded->timeout_ms > remaining_ms) {
        bounded->timeout_ms = remaining_ms;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result delay_ms(DeviceState &state, uint64_t delay_ms, dtv_timeout timeout)
{
    if (state.io.clock == nullptr || state.io.clock->monotonic_now_ns == nullptr ||
        state.io.clock->delay_until == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    const auto now = state.io.clock->monotonic_now_ns(state.io.clock->user);
    const auto wake = now + delay_ms * 1000000;
    uint32_t cancelled = 0;
    const auto result = state.io.clock->delay_until(
        state.io.clock->user, wake, timeout.operation_deadline_ns, nullptr, &cancelled);
    if (!succeeded(result)) {
        return result;
    }
    if (cancelled != 0) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    }
    if (timeout.operation_deadline_ns != 0 &&
        state.io.clock->monotonic_now_ns(state.io.clock->user) >= timeout.operation_deadline_ns) {
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    return result;
}

DeviceState *state_from(dtv_context *context)
{
    return static_cast<DeviceState *>(context);
}

uint16_t address_value(uint32_t address)
{
    return static_cast<uint16_t>(((address >> 16) & 0xFF) << 8) |
           static_cast<uint16_t>((address >> 24) & 0xFF);
}

dtv_internal_result read_word(DeviceState &state, uint32_t address, uint32_t *value,
                              dtv_timeout timeout)
{
    if (value == nullptr || (address & 3) != 0 || state.io.control == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }

    std::array<uint8_t, 4> bytes{};
    dtv_buffer buffer{bytes.data(), bytes.size(), 0};
    dtv_timeout bounded{};
    auto result = bounded_timeout(state, timeout, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    result = state.io.control(state.io.user, kRequestTypeIn, kBridgeRequest,
                                         address_value(address),
                                         static_cast<uint16_t>(address & 0xFFFF),
                                         &buffer, bounded);
    if (!succeeded(result)) {
        return result;
    }
    if (buffer.size != bytes.size()) {
        return make_result(DTV_INTERNAL_RESULT_USB_IO,
                           static_cast<uint32_t>(buffer.size));
    }
    *value = (static_cast<uint32_t>(bytes[0]) << 24) |
             (static_cast<uint32_t>(bytes[1]) << 16) |
             (static_cast<uint32_t>(bytes[2]) << 8) |
             static_cast<uint32_t>(bytes[3]);
    return result;
}

constexpr std::array<uint32_t, 5> kPristinePatchWords{
    0x9B800000, 0xAA04E40F, 0x9B800000, 0xAA04E40D, 0xB4279F8D,
};
constexpr std::array<uint32_t, 5> kActivePatchWords{
    0x9BEC1020, 0x970C9FA0, 0x9BEC1064, 0x970C9FA0, 0x9FA09F8D,
};
constexpr std::array<uint32_t, 5> kPatchAddresses{
    0x000E9AC0, 0x000E9AC4, 0x000E9E0C, 0x000E9E10, 0x000E9D94,
};

constexpr std::array<uint32_t, 64> kSha256RoundConstants{
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

uint32_t rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32 - count));
}

void sha256_block(std::array<uint32_t, 8> &hash, const uint8_t *block)
{
    std::array<uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 4;
        words[index] = (static_cast<uint32_t>(block[offset]) << 24) |
                       (static_cast<uint32_t>(block[offset + 1]) << 16) |
                       (static_cast<uint32_t>(block[offset + 2]) << 8) |
                       static_cast<uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const uint32_t x = words[index - 15];
        const uint32_t y = words[index - 2];
        const uint32_t s0 = rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
        const uint32_t s1 = rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
        const uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + sum1 + choose + kSha256RoundConstants[index] + words[index];
        const uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
}

std::array<uint8_t, 32> sha256(const uint8_t *data, std::size_t size)
{
    std::array<uint32_t, 8> hash{
        0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
        0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
    };
    const auto block_count = size / 64;
    for (std::size_t index = 0; index < block_count; ++index) {
        sha256_block(hash, data + index * 64);
    }

    std::array<uint8_t, 128> tail{};
    const auto remainder = size % 64;
    for (std::size_t index = 0; index < remainder; ++index) {
        tail[index] = data[block_count * 64 + index];
    }
    tail[remainder] = 0x80;
    const auto tail_size = remainder < 56 ? 64 : 128;
    const uint64_t bit_size = static_cast<uint64_t>(size) * 8;
    for (std::size_t index = 0; index < 8; ++index) {
        tail[tail_size - 1 - index] = static_cast<uint8_t>(bit_size >> (index * 8));
    }
    sha256_block(hash, tail.data());
    if (tail_size == 128) {
        sha256_block(hash, tail.data() + 64);
    }

    std::array<uint8_t, 32> output{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
        output[index * 4] = static_cast<uint8_t>(hash[index] >> 24);
        output[index * 4 + 1] = static_cast<uint8_t>(hash[index] >> 16);
        output[index * 4 + 2] = static_cast<uint8_t>(hash[index] >> 8);
        output[index * 4 + 3] = static_cast<uint8_t>(hash[index]);
    }
    return output;
}

constexpr std::array<uint8_t, 32> kWidenSha256{
    0x8F, 0xD0, 0x35, 0xBC, 0xE7, 0x38, 0xA9, 0x1F,
    0xA7, 0x89, 0x44, 0x23, 0xB2, 0xE7, 0x38, 0x3E,
    0x39, 0x23, 0x7F, 0xAE, 0xC0, 0x79, 0xC9, 0xF8,
    0x2B, 0xD5, 0x1B, 0xAF, 0x42, 0x2F, 0xA5, 0x3D,
};
constexpr std::array<uint8_t, 32> kCore0Sha256{
    0xB5, 0x9B, 0x82, 0xDD, 0x81, 0xF3, 0x90, 0x8C,
    0xF9, 0x5B, 0xB2, 0x90, 0xD5, 0xBD, 0xD2, 0x29,
    0xB1, 0x2B, 0x5C, 0x9D, 0xE1, 0x31, 0x48, 0xBD,
    0x44, 0xBE, 0x1E, 0x54, 0x49, 0x52, 0x22, 0x74,
};
constexpr std::array<uint8_t, 32> kCore1Sha256{
    0x0E, 0x02, 0xC9, 0x46, 0x89, 0x36, 0xBB, 0x44,
    0x13, 0x23, 0x93, 0x93, 0xBA, 0xCA, 0x42, 0xE1,
    0xDC, 0x85, 0x59, 0xE2, 0x9D, 0xA6, 0x32, 0xCC,
    0x18, 0x69, 0x98, 0xE6, 0xA6, 0x0E, 0xA2, 0x02,
};
bool hash_matches(const uint8_t *data, std::size_t size,
                  const std::array<uint8_t, 32> &expected)
{
    return sha256(data, size) == expected;
}

dtv_internal_result read_widen_image(DeviceState &state, std::array<uint8_t, kWidenSize> &image,
                                     dtv_timeout timeout)
{
    for (std::size_t offset = 0; offset < image.size(); offset += 4) {
        uint32_t word = 0;
        const auto result = read_word(state, 0x000E1000 + static_cast<uint32_t>(offset),
                                      &word, timeout);
        if (!succeeded(result)) {
            return result;
        }
        image[offset] = static_cast<uint8_t>(word >> 24);
        image[offset + 1] = static_cast<uint8_t>(word >> 16);
        image[offset + 2] = static_cast<uint8_t>(word >> 8);
        image[offset + 3] = static_cast<uint8_t>(word);
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result read_patch_words(DeviceState &state,
                                     std::array<uint32_t, kPatchAddresses.size()> &words,
                                     dtv_timeout timeout)
{
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto result = read_word(state, kPatchAddresses[index], &words[index], timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result write_word(DeviceState &state, uint32_t address, uint32_t value,
                               dtv_timeout timeout)
{
    if ((address & 3) != 0 || state.io.control == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value),
    };
    dtv_buffer buffer{bytes.data(), bytes.size(), bytes.size()};
    dtv_timeout bounded{};
    auto result = bounded_timeout(state, timeout, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    result = state.io.control(state.io.user, kRequestTypeOut, kBridgeRequest,
                                         address_value(address),
                                         static_cast<uint16_t>(address & 0xFFFF),
                                         &buffer, bounded);
    if (!succeeded(result)) {
        return result;
    }
    return result;
}

dtv_internal_result transfer_block(DeviceState &state, bool write, uint32_t address,
                                   uint8_t *data, std::size_t size, dtv_timeout timeout)
{
    if (data == nullptr || size < 4 || size > 64 || (size & 3) != 0 ||
        address < 0x01000000 || address + size >= 0x02000000 ||
        (address & 3) != 0 || state.io.control == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    dtv_timeout bounded{};
    auto result = bounded_timeout(state, timeout, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    dtv_buffer buffer{data, size, write ? size : 0};
    result = state.io.control(state.io.user,
                              write ? kRequestTypeOut : kRequestTypeIn,
                              kBridgeRequest, address_value(address),
                              static_cast<uint16_t>(address & 0xFFFF), &buffer, bounded);
    if (!succeeded(result)) {
        return result;
    }
    if (buffer.size != size) {
        return make_result(DTV_INTERNAL_RESULT_USB_IO, static_cast<uint32_t>(buffer.size));
    }
    return result;
}

bool equals(const std::array<uint32_t, kPatchAddresses.size()> &left,
            const std::array<uint32_t, kPatchAddresses.size()> &right)
{
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

bool core1_is_running(DeviceState &state, dtv_timeout timeout, dtv_internal_result *result)
{
    constexpr std::array<uint32_t, 8> expected{
        0x54493243, 0, 0xFC662BD9, 1, 0, 1924, 0x000A3800, 0,
    };
    std::array<uint32_t, 8> status{};
    for (std::size_t index = 0; index < status.size(); ++index) {
        *result = read_word(state, 0x017A3800 + static_cast<uint32_t>(index * 4),
                            &status[index], timeout);
        if (!succeeded(*result)) {
            return false;
        }
    }
    return status[0] == expected[0] && (status[1] >> 16) == 1 &&
           status[2] == expected[2] && status[3] == expected[3] &&
           status[4] == state.session && status[5] == expected[5] &&
           status[6] == expected[6];
}

template <typename Predicate>
dtv_internal_result wait_core1(DeviceState &state,
                               std::array<uint32_t, 8> &status,
                               Predicate condition, dtv_timeout timeout,
                               uint64_t wait_ms)
{
    const auto started = state.io.clock->monotonic_now_ns(state.io.clock->user);
    const auto deadline = started + wait_ms * 1000000;
    for (;;) {
        for (std::size_t index = 0; index < status.size(); ++index) {
            auto result = read_word(state, 0x017A3800 + static_cast<uint32_t>(index * 4),
                                    &status[index], timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        if (condition(status)) {
            return make_result(DTV_INTERNAL_RESULT_OK);
        }
        const auto now = state.io.clock->monotonic_now_ns(state.io.clock->user);
        if (now >= deadline) {
            return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
        }
        auto result = delay_ms(state, 1, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
}

dtv_internal_result start_core1(DeviceState &state, const uint8_t *image,
                                dtv_timeout timeout)
{
    auto result = write_word(state, 0x01700000, 0xFFFFFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x017FFFFC, 0x000A0000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    std::array<uint8_t, 64> block{};
    std::array<uint8_t, 64> readback{};
    for (uint32_t offset = 0; offset < 0x800; offset += block.size()) {
        result = transfer_block(state, true, 0x017A3800 + offset, block.data(),
                                block.size(), timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    block.fill(0);
    block[0] = static_cast<uint8_t>(state.session >> 24);
    block[1] = static_cast<uint8_t>(state.session >> 16);
    block[2] = static_cast<uint8_t>(state.session >> 8);
    block[3] = static_cast<uint8_t>(state.session);
    result = transfer_block(state, true, 0x017A3840, block.data(), block.size(), timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (std::size_t offset = 0; offset < kCore1Size; offset += block.size()) {
        block.fill(0);
        const auto remaining = kCore1Size - offset;
        const auto count = remaining < block.size() ? remaining : block.size();
        for (std::size_t index = 0; index < count; ++index) {
            block[index] = image[offset + index];
        }
        result = transfer_block(state, true,
                                0x017A0000 + static_cast<uint32_t>(offset),
                                block.data(), block.size(), timeout);
        if (!succeeded(result)) {
            return result;
        }
        readback.fill(0);
        result = transfer_block(state, false,
                                0x017A0000 + static_cast<uint32_t>(offset),
                                readback.data(), readback.size(), timeout);
        if (!succeeded(result)) {
            return result;
        }
        if (readback != block) {
            return make_result(DTV_INTERNAL_RESULT_USB_IO,
                               static_cast<uint32_t>(offset));
        }
    }
    result = write_word(state, 0x017A0000, 0xFFFCFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    std::array<uint32_t, 8> status{};
    result = wait_core1(state, status, [&](const auto &current) {
        return current[0] == 0x54493243 && (current[1] >> 16) == 1 &&
               current[2] == 0xFC662BD9 && current[3] == 1 &&
               current[4] == state.session && current[5] == kCore1Size &&
               current[6] == 0x000A3800;
    }, timeout, 100);
    if (!succeeded(result)) {
        return result;
    }
    const auto first_heartbeat = status[1];
    result = wait_core1(state, status, [&](const auto &current) {
        return (current[1] >> 16) == 1 && current[1] != first_heartbeat;
    }, timeout, 100);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x017A3844, 1, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = wait_core1(state, status, [](const auto &current) {
        return current[4] == 1 && current[5] == 0;
    }, timeout, 100);
    if (!succeeded(result)) {
        return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result validate_firmware_image(uint32_t stage, dtv_bytes firmware)
{
    std::size_t expected_size = 0;
    const std::array<uint8_t, 32> *expected_hash = nullptr;
    if (stage == kFirmwareStageWiden) {
        expected_size = kWidenSize;
        expected_hash = &kWidenSha256;
    } else if (stage == kFirmwareStageCore1) {
        expected_size = kCore1Size;
        expected_hash = &kCore1Sha256;
    } else if (stage == kFirmwareStageFrontendPower) {
        expected_size = kCore0Size;
        expected_hash = &kCore0Sha256;
    }
    if (firmware.data == nullptr || expected_hash == nullptr ||
        firmware.size != expected_size ||
        !hash_matches(firmware.data, expected_size, *expected_hash)) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result install_widen(DeviceState &state, const uint8_t *image,
                                  dtv_timeout timeout)
{
    std::array<uint32_t, kPatchAddresses.size()> patches{};
    auto result = read_patch_words(state, patches, timeout);
    if (!succeeded(result)) {
        return result;
    }

    std::array<uint8_t, kWidenSize> current_image{};
    if (equals(patches, kPristinePatchWords)) {
        for (std::size_t offset = 0; offset < kWidenSize; offset += 4) {
            const uint32_t word = (static_cast<uint32_t>(image[offset]) << 24) |
                                  (static_cast<uint32_t>(image[offset + 1]) << 16) |
                                  (static_cast<uint32_t>(image[offset + 2]) << 8) |
                                  static_cast<uint32_t>(image[offset + 3]);
            result = write_word(state, 0x000E1000 + static_cast<uint32_t>(offset), word, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        result = read_widen_image(state, current_image, timeout);
        if (!succeeded(result)) {
            return result;
        }
        for (std::size_t index = 0; index < current_image.size(); ++index) {
            if (current_image[index] != image[index]) {
                return make_result(DTV_INTERNAL_RESULT_USB_IO,
                                   static_cast<uint32_t>(index));
            }
        }
        for (std::size_t index = 0; index < kPatchAddresses.size(); ++index) {
            result = write_word(state, kPatchAddresses[index], kActivePatchWords[index], timeout);
            if (!succeeded(result)) {
                return result;
            }
            uint32_t readback = 0;
            result = read_word(state, kPatchAddresses[index], &readback, timeout);
            if (!succeeded(result)) {
                return result;
            }
            if (readback != kActivePatchWords[index]) {
                return make_result(DTV_INTERNAL_RESULT_USB_IO,
                                   static_cast<uint32_t>(readback));
            }
        }
        return make_result(DTV_INTERNAL_RESULT_OK);
    }

    if (!equals(patches, kActivePatchWords)) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    }
    result = read_widen_image(state, current_image, timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (std::size_t index = 0; index < current_image.size(); ++index) {
        if (current_image[index] != image[index]) {
            return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        }
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result create_state(dtv_device_id device_id, dtv_context **out_context)
{
    if (out_context == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    *out_context = nullptr;
    auto *state = new DeviceState;
    state->device_id = device_id;
    *out_context = state;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

void destroy_state(dtv_context *context)
{
    delete state_from(context);
}

dtv_internal_result open_device(dtv_context *context, const dtv_usb_io *io,
                                const dtv_open_args *args)
{
    auto *state = state_from(context);
    if (state == nullptr || io == nullptr || args == nullptr || args->model == nullptr ||
        args->model->vendor_id != 0x054C || args->model->product_id != 0x04B2 ||
        io->control == nullptr || io->bulk_in == nullptr || io->clock == nullptr ||
        io->clock->monotonic_now_ns == nullptr || io->clock->delay_until == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    state->io = *io;
    state->opened = true;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result identify_device(dtv_context *context, dtv_identify_info *info,
                                    dtv_timeout timeout)
{
    auto *state = state_from(context);
    if (state == nullptr || !state->opened || info == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }

    info->firmware_key = 0;
    info->state = DTV_IDENTIFY_UNKNOWN;
    std::array<uint32_t, kPatchAddresses.size()> patches{};
    auto result = read_patch_words(*state, patches, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (equals(patches, kPristinePatchWords)) {
        info->state = DTV_IDENTIFY_LOADER;
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    if (!equals(patches, kActivePatchWords)) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    }

    std::array<uint8_t, kWidenSize> image{};
    result = read_widen_image(*state, image, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (sha256(image.data(), image.size()) != kWidenSha256) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    }

    dtv_internal_result status_result = make_result(DTV_INTERNAL_RESULT_OK);
    if (!core1_is_running(*state, timeout, &status_result)) {
        if (!succeeded(status_result)) {
            return status_result;
        }
        info->state = DTV_IDENTIFY_LOADER;
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    info->firmware_key = kFirmwareKey;
    info->state = DTV_IDENTIFY_RUNTIME;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result initialize_bridge(DeviceState &state, dtv_timeout timeout)
{
    uint32_t value = 0;
    auto result = read_word(state, 0x00FD8010, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x00FD8010, value | 0x10, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = read_word(state, 0x00FD8014, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x00FD8014, value & ~0x10u, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint32_t, uint32_t>, 10> setup{
        std::pair{0x00FD80C0, 1}, std::pair{0x00FD80C0, 0},
        std::pair{0x00FD80C4, 0}, std::pair{0x00090114, 0x80080000},
        std::pair{0x00FD8FF8, 6}, std::pair{0x00FD8FF4, 8},
        std::pair{0x00FD8FE8, 0x3C}, std::pair{0x00FD8FE4, 0x40},
        std::pair{0x0F1010C, 0x82010000}, std::pair{0x0F10114, 0x80080000},
    };
    for (const auto &[address, setting] : setup) {
        result = write_word(state, address, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    constexpr std::array<std::pair<uint32_t, uint32_t>, 4> remaining_gates{
        std::pair{0x0F10118, 0xFFFF0000}, std::pair{0x0F1011C, 0x00060000},
        std::pair{0x0F10128, 0x60000000}, std::pair{0x0F1012C, 0x40070000},
    };
    for (const auto &[address, setting] : remaining_gates) {
        result = write_word(state, address, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = write_word(state, 0x01600000, 0xFFFFFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x01700000, 0xFFFFFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    std::array<uint8_t, 64> probe{};
    std::array<uint8_t, 64> readback{};
    for (std::size_t index = 0; index < probe.size(); ++index) {
        probe[index] = static_cast<uint8_t>(0xA5 + index);
    }
    result = transfer_block(state, true, 0x017A3FC0, probe.data(), probe.size(), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = transfer_block(state, false, 0x017A3FC0, readback.data(),
                            readback.size(), timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (readback != probe) {
        return make_result(DTV_INTERNAL_RESULT_USB_IO);
    }
    result = read_word(state, 0x00F1010C, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

void store_be32(uint8_t *data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

uint32_t load_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

dtv_internal_result initialize_i2c_ring(DeviceState &state, dtv_timeout timeout)
{
    uint32_t producer = 0;
    const auto result = read_word(state, 0x017A3850, &producer, timeout);
    if (!succeeded(result)) {
        return result;
    }
    state.i2c_sequence = static_cast<uint16_t>(producer);
    state.i2c_ready = true;
    return result;
}

dtv_internal_result i2c_transfer(DeviceState &state, uint8_t address, uint16_t operation,
                                const uint8_t *write_data, uint8_t write_size,
                                uint8_t *read_data, uint8_t read_size,
                                dtv_timeout timeout)
{
    const bool lengths_valid =
        (operation == 1 && write_size > 0 && write_size <= 32 && read_size == 0) ||
        (operation == 2 && write_size == 0 && read_size > 0 && read_size <= 32) ||
        (operation == 3 && write_size > 0 && write_size <= 32 &&
         read_size > 0 && read_size <= 32);
    if (!state.i2c_ready || !lengths_valid ||
        (write_size > 0 && write_data == nullptr) ||
        (read_size > 0 && read_data == nullptr)) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }

    uint32_t progress = 0;
    auto result = read_word(state, 0x017A389C, &progress, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if ((progress & 0x80000000) != 0) {
        return make_result(DTV_INTERNAL_RESULT_INTERNAL, progress);
    }
    const auto sequence = state.i2c_sequence;
    const auto tag = (state.session & 0xFFFF0000) | sequence;
    const auto slot = static_cast<uint32_t>(sequence & 7);
    std::array<uint8_t, 64> request{};
    store_be32(request.data(), tag);
    store_be32(request.data() + 4,
               (static_cast<uint32_t>(address) << 16) | operation);
    store_be32(request.data() + 8,
               (static_cast<uint32_t>(read_size) << 24) | write_size);
    for (std::size_t index = 0; index < write_size; ++index) {
        request[0x10 + index] = write_data[index];
    }
    store_be32(request.data() + 0x3C, tag);
    result = transfer_block(state, true, 0x017A3900 + slot * 64,
                            request.data(), request.size(), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x017A3850,
                        static_cast<uint16_t>(sequence + 1), timeout);
    if (!succeeded(result)) {
        state.i2c_ready = false;
        return result;
    }
    state.i2c_sequence = static_cast<uint16_t>(sequence + 1);

    const auto started = state.io.clock->monotonic_now_ns(state.io.clock->user);
    const auto local_deadline = started + 1000000000ULL;
    auto wait_timeout = timeout;
    if (wait_timeout.operation_deadline_ns == 0 ||
        local_deadline < wait_timeout.operation_deadline_ns) {
        wait_timeout.operation_deadline_ns = local_deadline;
    }
    const auto expected_count = static_cast<uint16_t>((progress + 1) & 0xFFFF);
    std::array<uint8_t, 64> completion{};
    for (;;) {
        result = read_word(state, 0x017A389C, &progress, wait_timeout);
        if (!succeeded(result)) {
            state.i2c_ready = false;
            return result;
        }
        if ((progress & 0x80000000) != 0) {
            state.i2c_ready = false;
            return make_result(DTV_INTERNAL_RESULT_INTERNAL, progress);
        }
        if (static_cast<uint16_t>(progress) == expected_count) {
            result = transfer_block(state, false, 0x017A3B00 + slot * 64,
                                    completion.data(), completion.size(), wait_timeout);
            if (!succeeded(result)) {
                state.i2c_ready = false;
                return result;
            }
            const auto first_tag = load_be32(completion.data());
            const auto last_tag = load_be32(completion.data() + 0x3C);
            if (first_tag != tag || last_tag != tag) {
                state.i2c_ready = false;
                return make_result(DTV_INTERNAL_RESULT_INTERNAL, first_tag);
            }
            const auto status = load_be32(completion.data() + 4);
            const auto completion_code = status & 0xFF;
            const auto actual_read_size = load_be32(completion.data() + 8);
            if (completion_code == 0) {
                if (actual_read_size != read_size) {
                    state.i2c_ready = false;
                    return make_result(DTV_INTERNAL_RESULT_USB_IO, actual_read_size);
                }
                for (std::size_t index = 0; index < read_size; ++index) {
                    read_data[index] = completion[0x10 + index];
                }
                return make_result(DTV_INTERNAL_RESULT_OK);
            }
            if (completion_code == 4) {
                return make_result(DTV_INTERNAL_RESULT_TIMEOUT, status);
            }
            if (completion_code == 32 || completion_code == 33 || completion_code == 34) {
                state.i2c_ready = false;
            }
            if (completion_code == 32) {
                return make_result(DTV_INTERNAL_RESULT_TIMEOUT, status);
            }
            if (completion_code == 16) {
                return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, status);
            }
            if (completion_code == 34) {
                return make_result(DTV_INTERNAL_RESULT_INTERNAL, status);
            }
            return make_result(DTV_INTERNAL_RESULT_USB_IO, status);
        }
        const auto now = state.io.clock->monotonic_now_ns(state.io.clock->user);
        if (now >= local_deadline) {
            state.i2c_ready = false;
            return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
        }
        result = delay_ms(state, 1, wait_timeout);
        if (!succeeded(result)) {
            state.i2c_ready = false;
            return result;
        }
    }
}

dtv_internal_result demod_read(DeviceState &state, uint16_t reg,
                               uint16_t *value, dtv_timeout timeout)
{
    if (value == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    const std::array<uint8_t, 2> index{
        static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg),
    };
    std::array<uint8_t, 2> bytes{};
    auto result = i2c_transfer(state, 0x24, 3, index.data(), index.size(),
                               bytes.data(), bytes.size(), timeout);
    if (!succeeded(result)) {
        return result;
    }
    *value = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
    return result;
}

dtv_internal_result demod_write(DeviceState &state, uint16_t reg,
                                uint16_t value, dtv_timeout timeout)
{
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value),
    };
    return i2c_transfer(state, 0x24, 1, bytes.data(), bytes.size(), nullptr, 0, timeout);
}

dtv_internal_result tuner_read(DeviceState &state, uint8_t reg,
                               uint16_t *value, dtv_timeout timeout)
{
    if (value == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    std::array<uint8_t, 1> index{reg};
    std::array<uint8_t, 2> bytes{};
    auto result = i2c_transfer(state, 0xC0, 3, index.data(), index.size(),
                               bytes.data(), bytes.size(), timeout);
    if (!succeeded(result)) {
        return result;
    }
    *value = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
    return result;
}

dtv_internal_result tuner_write(DeviceState &state, uint8_t reg,
                                uint16_t value, dtv_timeout timeout)
{
    const std::array<uint8_t, 3> bytes{
        reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value),
    };
    return i2c_transfer(state, 0xC0, 1, bytes.data(), bytes.size(), nullptr, 0, timeout);
}

dtv_internal_result demod_power(DeviceState &state, bool interface_only,
                                dtv_timeout timeout)
{
    uint16_t value = 0;
    const std::array<std::pair<uint16_t, uint16_t>, 3> fixed{
        std::pair<uint16_t, uint16_t>{774, interface_only ? 0x3FFF : 0},
        std::pair<uint16_t, uint16_t>{775, interface_only ? 0xFFFF : 0},
        std::pair<uint16_t, uint16_t>{776, interface_only ? 0xFFFF : 0},
    };
    for (const auto &[reg, setting] : fixed) {
        auto result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    auto result = demod_read(state, 900, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 900,
                         static_cast<uint16_t>((value & 0xFFFC) |
                                               (interface_only ? 3 : 0)), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 1280, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, 1280, value & 0x00FF, timeout);
}

dtv_internal_result demod_adc_off(DeviceState &state, dtv_timeout timeout)
{
    auto result = demod_write(state, 907, 0x3000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, 908, 0x007A, timeout);
}

dtv_internal_result demod_adc_on_slow(DeviceState &state, dtv_timeout timeout)
{
    uint16_t adc = 0;
    uint16_t slow = 0;
    auto result = demod_read(state, 907, &adc, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 908, &slow, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 907, adc & 0x0FFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 908, slow & 0x0003, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 908, slow | 0x0003, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 907, adc & 0x0FFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, 908, (slow & ~0x0002u) | 0x0001, timeout);
}

dtv_internal_result demod_initialize(DeviceState &state, dtv_timeout timeout)
{
    constexpr std::array<uint8_t, 4> start_command{0x05, 0x07, 0x00, 0x03};
    auto result = i2c_transfer(state, 0x24, 1, start_command.data(), start_command.size(),
                               nullptr, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t value = 0;
    result = demod_read(state, 896, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 896, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (value != 0x01B3) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED, value);
    }
    result = demod_read(state, 897, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (value != 0x8000 && value != 0x8001 && value != 0x8002) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED, value);
    }
    state.demod_revision = value;

    result = demod_write(state, 1287, 3, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1025, 0x0100, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1028, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_power(state, false, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_adc_off(state, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 13> reset_assert{
        std::pair<uint16_t, uint16_t>{770, 0xFFFF}, {771, 0xFFFF}, {772, 0xFFFC},
        {898, 0x000C}, {1280, 0x004D}, {1281, 0x000C},
        {770, 0}, {771, 0}, {772, 0}, {898, 0x0004}, {1280, 0}, {1281, 0},
        {906, 0x2D98},
    };
    for (const auto &[reg, setting] : reset_assert) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 10> pll{
        std::pair<uint16_t, uint16_t>{901, 0x0112}, {902, 0x050F}, {902, 0x0507},
        {904, 0x1E80}, {23, 0x0337}, {24, 0xF980}, {27, 0}, {28, 0}, {26, 0},
        {922, 0xD257},
    };
    for (const auto &[reg, setting] : pll) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = demod_write(state, 898, 0x0004, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 4> gpio_reset{
        std::pair<uint16_t, uint16_t>{1029, 0xFFFF}, {1030, 0},
        {1032, 0xFFFF}, {1037, 0},
    };
    for (const auto &[reg, setting] : gpio_reset) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = demod_read(state, 299, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 299, (value & 0x0050) | 0x0022, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 300, 0x0700, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1286, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1291, 0x0205, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 40, 0x0755, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 14> defaults{
        std::pair<uint16_t, uint16_t>{7, 4}, {8, 1024}, {9, 2068},
        {11, 27}, {12, 30528}, {13, 91}, {14, 36224}, {15, 457},
        {16, 50048}, {17, 0}, {18, 128}, {19, 0}, {20, 144}, {21, 1},
    };
    for (const auto &[reg, setting] : defaults) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 23> rest_defaults{
        std::pair<uint16_t, uint16_t>{22, 54464},
        {80, 7250}, {81, 7238}, {82, 7264}, {83, 7309}, {84, 7338},
        {85, 7382}, {86, 7427}, {87, 7456}, {88, 7500}, {89, 7544},
        {90, 7574}, {108, 0}, {109, 0}, {110, 0}, {111, 0},
        {175, 1040}, {179, 8192}, {181, 10240}, {182, 10240},
        {183, 10240}, {184, 10240}, {185, 10240},
    };
    for (const auto &[reg, setting] : rest_defaults) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 18> tail_defaults{
        std::pair<uint16_t, uint16_t>{186, 10240}, {193, 1638}, {194, 0},
        {205, 8207}, {206, 15}, {215, 573}, {216, 164}, {217, 164},
        {218, 32752}, {219, 15564}, {230, 0}, {263, 2048}, {268, 1063},
        {270, 1}, {285, 32}, {299, 98}, {338, 5217}, {903, 0x0031},
    };
    for (const auto &[reg, setting] : tail_defaults) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = demod_read(state, 1285, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1285, value & 0xFFFD, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 29, 0x0134, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 30, 0x392F, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t slow = 0;
    uint16_t adc = 0;
    result = demod_read(state, 908, &slow, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 907, &adc, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 908, slow | 0x0003, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 907, adc, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 908, (slow & ~0x0002u) | 0x0001, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 923, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 924, 0x0308, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 923, 1, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 923, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 1, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 907, adc, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 908, slow | 0x0003, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 285, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 285, (value & 0xFF9F) | 0x0060, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_power(state, true, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 565, &value, timeout);
    return result;
}

dtv_internal_result tuner_gpio(DeviceState &state, uint16_t bit, bool high,
                               dtv_timeout timeout)
{
    uint16_t value = 0;
    auto result = demod_read(state, 1030, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (high) {
        value |= bit;
    } else {
        value &= static_cast<uint16_t>(~bit);
    }
    return demod_write(state, 1030, value, timeout);
}

dtv_internal_result tuner_gpio6_output(DeviceState &state, bool high,
                                       dtv_timeout timeout)
{
    uint16_t direction = 0;
    auto result = demod_read(state, 1029, &direction, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1029, direction & ~0x0040, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return tuner_gpio(state, 0x0040, high, timeout);
}

dtv_internal_result tuner_initialize(DeviceState &state, dtv_timeout timeout)
{
    auto result = demod_write(state, 1025, 0xC000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1028, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t identity = 0;
    result = tuner_read(state, 0x1A, &identity, timeout);
    if (!succeeded(result)) {
        return result;
    }
    identity &= static_cast<uint16_t>(~0x0800);
    const auto product = (identity >> 8) & 0xF;
    const auto version = identity & 0xFF;
    state.tuner_package = static_cast<uint8_t>((version >> 5) & 7);
    state.tuner_revision = static_cast<uint8_t>(version & 0x1F);
    state.tuner_p1g = state.tuner_revision == 7 || state.tuner_revision == 15;
    if (product != 2 || (version & 3) == 2 ||
        (state.tuner_package != 1 && state.tuner_package != 4 &&
         state.tuner_package != 6 && state.tuner_package != 7) ||
        (state.tuner_revision != 1 && state.tuner_revision != 3 &&
         state.tuner_revision != 7 && state.tuner_revision != 15)) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED, identity);
    }
    result = tuner_gpio(state, 0x0001, false, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 10, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_gpio(state, 0x0020, false, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 10, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_gpio(state, 0x0020, true, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 10, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint8_t, uint16_t>, 4> digital{
        std::pair<uint8_t, uint16_t>{0x24, 0x0022}, {0x1B, 0x00A2},
        {0x20, 0x3B00}, {0x23, 0x0A30},
    };
    for (const auto &[reg, value] : digital) {
        result = tuner_write(state, reg, value, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    uint16_t pll = 0;
    result = tuner_read(state, 0x21, &pll, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x21, pll | 0x8000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_read(state, 0x1A, &identity, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x1B, (identity & 0x0040) ? 0x00EB : 0x00A2, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<uint16_t, 25> common_regs{
        0x0000, 0x99A0, 0x6008, 0x0000, 0x8BCB, 0x0000, 0x0405,
        0x0000, 0x0000, 0x0000, 0xB802, 0x0300, 0x2D12, 0xBAC0,
        0x7C00, 0xDBB9, 0x0954, 0x0743, 0x8000, 0x0001, 0x0040,
        0x0100, 0x0000, 0xE910, 0x149E,
    };
    for (std::size_t index = 0; index < common_regs.size(); ++index) {
        result = tuner_write(state, static_cast<uint8_t>(index + 1), common_regs[index], timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    constexpr std::array<std::pair<uint8_t, uint16_t>, 9> defaults{
        std::pair<uint8_t, uint16_t>{0x1C, 0xFF2D}, {0x39, 0},
        {0x1E, 0x07FF}, {0x1F, 0x0007}, {0x24, 0x22EB},
        {0x3C, 0x03FF}, {0x3D, 0x0111}, {0x14, 2}, {0x23, 0x0A30},
    };
    for (const auto &[reg, value] : defaults) {
        result = tuner_write(state, reg, value, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    if (state.tuner_p1g) {
        constexpr std::array<std::pair<uint8_t, uint16_t>, 4> p1g_defaults{
            std::pair<uint8_t, uint16_t>{0x05, 0xABCD}, {0x11, 0x00B4},
            {0x1C, 0xFFFD}, {0x40, 0x0108},
        };
        for (const auto &[reg, value] : p1g_defaults) {
            result = tuner_write(state, reg, value, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
    }
    uint16_t fuse_a = 0;
    uint16_t fuse_b = 0;
    result = tuner_read(state, 0x26, &fuse_a, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_read(state, 0x28, &fuse_b, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (state.tuner_revision >= 3 && fuse_a == 0xFFFF) {
        result = tuner_write(state, 0x22, 0x0010, timeout);
        if (!succeeded(result)) {
            return result;
        }
        uint16_t measured = 0;
        result = tuner_read(state, 0x22, &measured, timeout);
        if (!succeeded(result)) {
            return result;
        }
        measured = (measured >> 6) & 0x03FF;
        if (measured < 670 || measured == 1023) {
            measured = 850;
        }
        const auto poly = static_cast<uint16_t>(165 - ((measured * 10) >> 6));
        fuse_a = fuse_b = static_cast<uint16_t>((3 << 12) | (34 << 6) | poly);
    }
    if (fuse_a != fuse_b) {
        fuse_a &= fuse_b;
    }
    if (fuse_a != 0xFFFF) {
        auto cap = static_cast<uint16_t>(fuse_a & 0x3F);
        auto poly = static_cast<uint16_t>((fuse_a >> 12) & 0xF);
        auto resistor = static_cast<uint16_t>((fuse_a >> 6) & 0x3F);
        cap = (cap <= 9 || cap >= 40) ? 32 : cap + 14;
        resistor = (resistor <= 25 || resistor >= 40) ? 34 : resistor;
        poly = (poly == 0 || poly >= 8) ? 3 : poly;
        result = tuner_write(state, 0x13, resistor << 10, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = tuner_write(state, 0x02,
                             (poly << 11) | ((resistor >> 2) << 6) | cap, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = tuner_write(state, 0x14, 2, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t value = 0;
    result = demod_power(state, false, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_adc_on_slow(state, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_gpio(state, 0x0001, false, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_read(state, 0x23, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x23, value | 0x4000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    state.tuner_calibration_flags = 7;
    result = demod_read(state, 299, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 299, (value & 0x0050) | 0x0022, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 300, 1792, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1286, 0x0400, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, 1291, 0x0205, timeout);
}

struct TunerPllBand {
    uint32_t upper_khz;
    uint8_t vco_band;
    uint8_t hf_code;
    uint8_t hf_div;
    uint8_t prescaler;
};

struct TunerTuneBand {
    uint32_t upper_khz;
    uint16_t switch_trim;
    uint16_t lna_tune;
    uint16_t lna_bias;
    uint16_t v_to_i;
    uint16_t mixer;
    uint16_t load;
    uint16_t enable;
};

constexpr std::array<TunerTuneBand, 10> kTunerTuneBands{{
    {170000, 4, 1, 15, 0x280, 0x2912, 0xB94E, 0x88E9},
    {184000, 1, 1, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {227000, 1, 3, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {380000, 1, 7, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {520000, 2, 0, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {550000, 2, 2, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {650000, 2, 3, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {750000, 2, 5, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {850000, 2, 6, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {900000, 2, 7, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
}};

constexpr std::array<TunerTuneBand, 11> kTunerP1gTuneBands{{
    {170000, 4, 1, 33295, 0x300, 0x2D22, 0x82CB, 0x88E9},
    {184000, 1, 1, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {227000, 1, 3, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {380000, 1, 7, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {510000, 2, 0, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {540000, 2, 1, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {600000, 2, 3, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {630000, 2, 4, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {680000, 2, 5, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {720000, 2, 6, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {900000, 2, 7, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
}};

constexpr std::array<TunerTuneBand, 9> kTunerForcedTuneBands{{
    {184000, 4, 1, 15, 0x280, 0x2912, 0xB94E, 0x88E9},
    {227000, 4, 3, 15, 0x280, 0x2912, 0xB94E, 0x88E9},
    {380000, 4, 7, 15, 0x280, 0x2912, 0xB94E, 0x88E9},
    {520000, 2, 0, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {550000, 2, 2, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {650000, 2, 3, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {750000, 2, 5, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {850000, 2, 6, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {900000, 2, 7, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
}};

constexpr std::array<TunerTuneBand, 9> kTunerP1gForcedTuneBands{{
    {184000, 4, 3, 16775, 0x2C0, 0x2D22, 0x81CB, 0x88E9},
    {227000, 4, 3, 16775, 0x2C0, 0x2D22, 0x81CB, 0x88E9},
    {380000, 4, 3, 16775, 0x2C0, 0x2D22, 0x81CB, 0x88E9},
    {520000, 2, 0, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {550000, 2, 2, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {650000, 2, 3, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {750000, 2, 5, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {850000, 2, 6, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {900000, 2, 7, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
}};

constexpr std::array<TunerPllBand, 16> kTunerPllBands{{
    {56000, 0, 9, 48, 6}, {70000, 1, 9, 48, 6},
    {87000, 0, 8, 32, 4}, {105000, 1, 8, 32, 4},
    {115000, 0, 7, 24, 6}, {140000, 1, 7, 24, 6},
    {170000, 0, 6, 16, 4}, {200000, 1, 6, 16, 4},
    {230000, 0, 5, 12, 6}, {280000, 1, 5, 12, 6},
    {340000, 0, 4, 8, 4}, {380000, 1, 4, 8, 4},
    {450000, 0, 3, 6, 6}, {580000, 1, 3, 6, 6},
    {700000, 0, 2, 4, 4}, {860000, 1, 2, 4, 4},
}};

constexpr std::array<TunerPllBand, 16> kTunerP1gPllBands{{
    {57000, 0, 11, 48, 6}, {70000, 1, 11, 48, 6},
    {86000, 0, 10, 32, 4}, {105000, 1, 10, 32, 4},
    {115000, 0, 9, 24, 6}, {140000, 1, 9, 24, 6},
    {170000, 0, 8, 16, 4}, {200000, 1, 8, 16, 4},
    {230000, 0, 7, 12, 6}, {280000, 1, 7, 12, 6},
    {340000, 0, 6, 8, 4}, {380000, 1, 6, 8, 4},
    {455000, 0, 5, 6, 6}, {580000, 1, 5, 6, 6},
    {680000, 0, 4, 4, 4}, {860000, 1, 4, 4, 4},
}};

dtv_internal_result tuner_set_pll(DeviceState &state, uint32_t frequency_hz,
                                  bool *has_fraction, dtv_timeout timeout)
{
    const auto frequency_khz = frequency_hz / 1000;
    const auto offset_khz = frequency_khz > 250000 ? 63u : 143u;
    const auto rf_khz = frequency_khz - offset_khz;
    const auto &bands = state.tuner_p1g ? kTunerP1gPllBands : kTunerPllBands;
    const TunerPllBand *band = nullptr;
    for (const auto &candidate : bands) {
        if (rf_khz <= candidate.upper_khz) {
            band = &candidate;
            break;
        }
    }
    if (band == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }

    const auto vco_khz = static_cast<uint64_t>(band->hf_div) * rf_khz * 2;
    const auto intermediate = vco_khz / band->prescaler;
    auto feedback = static_cast<uint32_t>(intermediate / 2000);
    auto fraction = static_cast<uint32_t>(intermediate - feedback * 2000);
    if (fraction < 100) {
        fraction = 0;
    } else if (fraction < 200) {
        fraction = 200;
    } else if (fraction > 1900) {
        fraction = 0;
        ++feedback;
    } else if (fraction > 1800) {
        fraction = 1800;
    }
    fraction = fraction * 6528 / 200;
    if (has_fraction != nullptr) {
        *has_fraction = fraction != 0;
    }
    const auto denominator = fraction == 0 ? 1 : 255;
    uint16_t control = 0;
    if (state.tuner_p1g) {
        control = 0x042C;
    } else if (fraction == 0) {
        control = band->vco_band ? 0x049F : 0x041F;
    } else {
        control = band->vco_band ? 0x049E : 0x041D;
    }
    control |= static_cast<uint16_t>(band->hf_code << 11);
    control |= static_cast<uint16_t>(band->vco_band << 7);
    auto loop_filter = static_cast<uint16_t>(state.tuner_p1g || fraction == 0
                                                 ? 0xFFF8
                                                 : 0xFF28);
    if (fraction != 0) {
        loop_filter |= 0x0006;
    }

    auto result = tuner_write(state, 0x15, static_cast<uint16_t>(feedback), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x16, static_cast<uint16_t>((denominator << 8) | 6), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x17, static_cast<uint16_t>(fraction), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x19, control, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return tuner_write(state, 0x1C, loop_filter, timeout);
}

struct TunerDcStep {
    uint8_t reg;
    uint8_t bit;
    bool pga;
    bool q_signal;
    uint16_t bb1;
    uint16_t bb7;
};

dtv_internal_result tuner_dc_calibrate(DeviceState &state, dtv_timeout timeout)
{
    uint16_t value = 0;
    auto result = tuner_read(state, 0x24, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x24, value & 0x0FFB, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_read(state, 0x10, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x10, (value & ~0x07F8u) | 0x003B, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_read(state, 0x23, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x23, value & 0xBFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }

    const std::array<TunerDcStep, 4> normal{{
        {0x06, 5, true, false, 0x20D0, 0x040D},
        {0x07, 11, true, true, 0x20D0, 0x040D},
        {0x06, 0, false, false, 0x3DD0, 0x040D},
        {0x06, 10, false, true, 0x3DD0, 0x040D},
    }};
    const std::array<TunerDcStep, 4> p1g{{
        {0x06, 5, true, false, 0x2078, 0x040D},
        {0x07, 11, true, true, 0x2078, 0x040D},
        {0x06, 0, false, false, 0x3D78, 0x040D},
        {0x06, 10, false, true, 0x3D78, 0x040D},
    }};
    const auto &steps = state.tuner_p1g ? p1g : normal;
    for (const auto &step : steps) {
        result = tuner_write(state, 0x01, step.bb1, timeout);
        if (!succeeded(result)) {
            return result;
        }
        const auto bb7 = static_cast<uint16_t>(step.bb7 | (step.q_signal ? 0x0080 : 0));
        result = tuner_write(state, 0x07, bb7, timeout);
        if (!succeeded(result)) {
            return result;
        }
        uint8_t search = 0;
        int32_t minimum = 1023;
        result = delay_ms(state, 14, timeout);
        if (!succeeded(result)) {
            return result;
        }
        for (;;) {
            const auto mask = static_cast<uint16_t>(0x1F << step.bit);
            auto trim = static_cast<uint16_t>((search & 0x1F) << step.bit);
            uint16_t base = step.reg == 0x06 ? 0 : bb7;
            if (step.reg == 0x06) {
                result = tuner_write(state, 0x06, (base & ~mask) | trim, timeout);
            } else {
                result = tuner_write(state, 0x07, (base & ~mask) | trim, timeout);
            }
            if (!succeeded(result)) {
                return result;
            }
            result = tuner_write(state, 0x1F, 7, timeout);
            if (!succeeded(result)) {
                return result;
            }
            result = delay_ms(state, 19, timeout);
            if (!succeeded(result)) {
                return result;
            }
            uint16_t positive = 0;
            result = tuner_read(state, 0x1D, &positive, timeout);
            if (!succeeded(result)) {
                return result;
            }
            result = tuner_write(state, 0x1F, 4, timeout);
            if (!succeeded(result)) {
                return result;
            }
            result = delay_ms(state, 19, timeout);
            if (!succeeded(result)) {
                return result;
            }
            uint16_t negative = 0;
            result = tuner_read(state, 0x1D, &negative, timeout);
            if (!succeeded(result)) {
                return result;
            }
            result = delay_ms(state, 9, timeout);
            if (!succeeded(result)) {
                return result;
            }
            const auto difference = static_cast<int32_t>(positive) - negative;
            if (search == 0 && difference < 0) {
                minimum = -1023;
            }
            if (search == 0 && ((step.pga && difference < 0) ||
                                (!step.pga && difference > 0))) {
                search = 0x10;
            }
            const auto folded = search >= 0x10 ? search - 0x10 : search;
            if ((difference < 0) == (minimum < 0) && folded < 15) {
                ++search;
                minimum = difference;
                result = delay_ms(state, 14, timeout);
                if (!succeeded(result)) {
                    return result;
                }
                continue;
            }
            const auto abs_difference = difference < 0 ? -difference : difference;
            const auto abs_minimum = minimum < 0 ? -minimum : minimum;
            if (abs_difference > abs_minimum && search != 0) {
                --search;
            }
            trim = static_cast<uint16_t>((search & 0x1F) << step.bit);
            if (step.reg == 0x06) {
                result = tuner_write(state, 0x06, (base & ~mask) | trim, timeout);
            } else {
                result = tuner_write(state, 0x07, (base & ~mask) | trim, timeout);
            }
            if (!succeeded(result)) {
                return result;
            }
            break;
        }
    }
    result = tuner_write(state, 0x07, 0x0405, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x1F, 7, timeout);
    if (!succeeded(result)) {
        return result;
    }
    state.tuner_calibration_flags &= static_cast<uint8_t>(~1u);
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result tuner_cap_trim(DeviceState &state, bool has_fraction, bool retune,
                                   dtv_timeout timeout)
{
    auto result = tuner_write(state, 0x10, 0x02B1, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x1E, 0x0032, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t cap = 0;
    if (state.tuner_p1g) {
        result = tuner_write(state, 0x40, 0x01FF, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = tuner_read(state, 0x40, &cap, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = delay_ms(state, 14, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = tuner_write(state, 0x40, 0x018C, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = tuner_read(state, 0x40, &cap, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = tuner_read(state, 0x18, &cap, timeout);
        if (!succeeded(result)) {
            return result;
        }
        cap &= 0x007F;
    } else {
        uint16_t candidate = 64;
        uint16_t step = 64;
        if (retune) {
            result = tuner_read(state, 0x18, &candidate, timeout);
            if (!succeeded(result)) {
                return result;
            }
            candidate &= 0x007F;
            step = 4;
        }
        uint16_t best = candidate;
        uint16_t best_difference = 3000;
        const auto target = static_cast<uint16_t>(has_fraction ? 400 : 200);
        while (step != 0) {
            step /= 2;
            result = tuner_write(state, 0x18, 0xE900 | candidate, timeout);
            if (!succeeded(result)) {
                return result;
            }
            uint16_t adc = 0;
            result = tuner_read(state, 0x1D, &adc, timeout);
            if (!succeeded(result)) {
                return result;
            }
            const auto difference = static_cast<uint16_t>(adc >= target ? adc - target
                                                                        : target - adc);
            const auto direction = adc >= target ? -1 : 1;
            if (difference < best_difference) {
                best_difference = difference;
                best = candidate;
            }
            candidate = static_cast<uint16_t>(candidate + direction * step);
            if (step != 0) {
                result = delay_ms(state, 12, timeout);
                if (!succeeded(result)) {
                    return result;
                }
            }
        }
        cap = best;
    }
    result = tuner_write(state, 0x18, 0xE900 | cap, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result tuner_wbd_calibrate(DeviceState &state, uint32_t frequency_hz,
                                        dtv_timeout timeout)
{
    if ((state.tuner_calibration_flags & 2) == 0) {
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    struct WbdBand {
        uint32_t upper_mhz;
        uint8_t gain;
    };
    constexpr std::array<WbdBand, 5> bands{{
        {120, 4}, {170, 4}, {380, 6}, {860, 6}, {65535, 6},
    }};
    const auto current_frequency_hz = state.tuner_tuned
                                          ? static_cast<uint64_t>(state.tuner_rf_khz) * 1000
                                          : frequency_hz;
    const auto frequency_mhz = current_frequency_hz / 1000000;
    const WbdBand *selected = &bands.back();
    for (const auto &band : bands) {
        if (frequency_mhz <= band.upper_mhz) {
            selected = &band;
            break;
        }
    }
    if (selected->gain == state.tuner_wbd_gain) {
        state.tuner_calibration_flags &= static_cast<uint8_t>(~2u);
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    auto result = tuner_write(state, 0x10,
                              static_cast<uint16_t>((selected->gain << 13) | 0x1F89), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x24, 0x02EB, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 18, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_read(state, 0x1D, &state.tuner_wbd_offset, timeout);
    if (!succeeded(result)) {
        return result;
    }
    state.tuner_wbd_gain = selected->gain;
    state.tuner_calibration_flags &= static_cast<uint8_t>(~2u);
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result tuner_temperature_calibrate(DeviceState &state, dtv_timeout timeout)
{
    if ((state.tuner_calibration_flags & 4) == 0) {
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    uint16_t mux = 0;
    auto result = tuner_read(state, 0x10, &mux, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x10, (mux & ~0x07F8u) | 0x0040, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t register13 = 0;
    result = tuner_read(state, 0x13, &register13, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x13, register13 | 0x0300, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 11, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t first = 0;
    result = tuner_read(state, 0x1D, &first, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x13,
                         static_cast<uint16_t>((register13 & ~0x0300u) | 0x0200), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 11, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t second = 0;
    result = tuner_read(state, 0x1D, &second, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x13, register13, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x10, mux, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto temperature = static_cast<int32_t>(second - first) * 180 / 256 + 55;
    state.tuner_temperature = static_cast<int16_t>(std::clamp(temperature, 0, 128));
    state.tuner_calibration_flags &= static_cast<uint8_t>(~4u);
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result tuner_apply_steps(DeviceState &state, const TunerTuneBand &band,
                                      uint32_t frequency_hz, dtv_timeout timeout)
{
    auto result = tuner_write(state, 0x1E, 0x07FF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto gain = frequency_hz / 1000000 <= 170 ? 4 : 6;
    result = tuner_write(state, 0x10, static_cast<uint16_t>((gain << 13) | 0x1F83), timeout);
    if (!succeeded(result)) {
        return result;
    }
    const bool p1g_c_band = state.tuner_p1g && band.enable == 0x88E9;
    result = tuner_write(state, 0x09, p1g_c_band
                                          ? band.lna_bias
                                          : static_cast<uint16_t>((band.lna_tune << 5) |
                                                                  band.lna_bias),
                         timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (p1g_c_band) {
        result = tuner_write(state, 0x0B,
                             static_cast<uint16_t>(0xB800 | (band.lna_tune << 6) |
                                                   band.switch_trim), timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = tuner_write(state, 0x0C, band.v_to_i, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x0D, band.mixer, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x0E, band.load, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint8_t, uint16_t>, 5> filter{
        std::pair<uint8_t, uint16_t>{0x01, 0x8000}, {0x03, 0x6008}, {0x04, 1},
        {0x02, 0x2A16}, {0x05, 0xABCD},
    };
    for (const auto &[reg, value] : filter) {
        result = tuner_write(state, reg, value, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    state.tuner_calibration_flags = 6;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result tuner_reset_pwm_gain(DeviceState &state, uint32_t frequency_hz,
                                         dtv_timeout timeout)
{
    constexpr std::array<uint16_t, 11> rf_c_p1g{
        280, 18, 504, 30060, 504, 61668, 364, 34925, 228, 37888, 109,
    };
    constexpr std::array<uint16_t, 11> rf_c{
        314, 33, 1023, 8935, 1023, 15829, 742, 9450, 468, 9216, 233,
    };
    constexpr std::array<uint16_t, 11> rf_vhf_uhf{
        398, 24, 954, 7168, 290, 17083, 954, 17988, 699, 7458, 580,
    };
    constexpr std::array<uint16_t, 7> bb_normal{
        500, 8, 400, 1024, 168, 1192, 400,
    };
    constexpr std::array<uint16_t, 7> bb_soc{
        550, 520, 440, 2048, 208, 2256, 440,
    };
    const auto frequency_mhz = frequency_hz / 1000000;
    const bool c_band = frequency_mhz <= 170;
    const bool vhf = frequency_mhz <= 250;
    const auto &rf = c_band ? (state.tuner_p1g ? rf_c_p1g : rf_c) : rf_vhf_uhf;
    const bool use_soc = state.tuner_p1g && (c_band || vhf);
    const auto &bb = use_soc ? bb_soc : bb_normal;
    auto result = tuner_write(state, 0x2A, 0xFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (uint8_t index = 0; index < 6; ++index) {
        result = tuner_write(state, static_cast<uint8_t>(0x2C + index), rf[3 + index], timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = tuner_write(state, 0x3E, rf[9], timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x3F, rf[10], timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x33, 0xFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (uint8_t index = 0; index < 4; ++index) {
        result = tuner_write(state, static_cast<uint8_t>(0x35 + index), bb[3 + index], timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = tuner_write(state, 0x01,
                         static_cast<uint16_t>(0x8000 | (bb[0] > 500 ? 0x2000 : 0)), timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto mux = rf[0] == 0 || (c_band && state.tuner_revision <= 3) ? 0 : 1;
    result = tuner_write(state, 0x32, static_cast<uint16_t>((mux << 12) | (mux << 11)), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x04, 1, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return tuner_write(state, 0x39, 0x0400, timeout);
}

int32_t tuner_wbd_db(const DeviceState &state, uint16_t raw)
{
    auto value = static_cast<int32_t>(raw & 0x03FF);
    if (value < state.tuner_wbd_offset) {
        value = 0;
    } else {
        value -= state.tuner_wbd_offset;
    }
    constexpr std::array<int32_t, 3> widths{66, 600, 170};
    constexpr std::array<int32_t, 3> slopes{120, 170, 250};
    auto correction = 0;
    for (std::size_t index = 0; index < widths.size(); ++index) {
        const auto used = std::min(value, widths[index]);
        correction += used * slopes[index] / widths[index];
        value -= used;
    }
    return -640 + correction;
}

dtv_internal_result demod_read_adc_power(DeviceState &state, int32_t *power_db256,
                                         dtv_timeout timeout)
{
    if (power_db256 == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    uint16_t high = 0;
    uint16_t low = 0;
    auto result = demod_read(state, 384, &high, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 385, &low, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto bits = (static_cast<uint32_t>(high) << 16) | low;
    const int64_t raw = bits <= 0x7FFFFFFF ? bits : static_cast<int64_t>(bits) - 0x100000000LL;
    if (raw <= 0) {
        *power_db256 = raw == 0 ? -3371 : 2128;
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    uint32_t exponent = 0;
    for (auto value = static_cast<uint64_t>(raw); value > 1; value >>= 1) {
        ++exponent;
    }
    const auto mantissa = static_cast<uint32_t>(static_cast<uint64_t>(raw) * 1000 /
                                                (1ULL << exponent));
    auto index = static_cast<int32_t>((mantissa - 1000) / 100);
    index = std::clamp(index, 0, 10);
    constexpr std::array<int32_t, 11> logarithm{
        908, 7003, 7090, 7170, 7244, 7313, 7377, 7438, 7495, 7549, 7600,
    };
    const auto numerator = logarithm[index] + 693 * (static_cast<int32_t>(exponent) - 20) - 6908;
    *power_db256 = numerator * 256 / 1000;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

uint16_t tuner_gain_stage(const uint16_t *ramp, std::size_t stage, int32_t gain)
{
    if (stage >= 3) {
        return 0;
    }
    const auto offset = 1 + stage * 3;
    const auto width = ramp[offset];
    const auto upper = ramp[offset + 1];
    const auto maximum = ramp[offset + 2];
    if (width == 0 || gain < static_cast<int32_t>(upper) - width) {
        return 0;
    }
    if (gain >= upper) {
        return maximum;
    }
    return static_cast<uint16_t>((gain - (static_cast<int32_t>(upper) - width)) *
                                 maximum / width);
}

dtv_internal_result tuner_apply_c_band_gain(DeviceState &state,
                                            const std::array<uint16_t, 11> &rf,
                                            const std::array<uint16_t, 7> &bb,
                                            int32_t rf_gain, int32_t bb_gain,
                                            uint16_t low_tracking, dtv_timeout timeout)
{
    std::array<uint16_t, 5> rf_stages{};
    for (std::size_t stage = 0; stage < rf_stages.size(); ++stage) {
        rf_stages[stage] = tuner_gain_stage(rf.data(), stage, rf_gain);
    }
    const std::array<uint16_t, 2> bb_stages{
        tuner_gain_stage(bb.data(), 0, bb_gain), tuner_gain_stage(bb.data(), 1, bb_gain),
    };
    auto result = tuner_write(state, 0x08,
                              rf_stages[0] | static_cast<uint16_t>(rf_stages[1] << 7), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x0A,
                         rf_stages[2] | static_cast<uint16_t>(rf_stages[3] << 7), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x0F, rf_stages[4] | low_tracking, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto fractional = (bb_gain % 10) * 100 / 125;
    const auto bb1 = 0x8000 | (bb[0] > 500 ? 0x2000 : 0) |
                     (bb_stages[0] << 3) | (bb_stages[1] << 8) | fractional;
    return tuner_write(state, 0x01, static_cast<uint16_t>(bb1), timeout);
}

dtv_internal_result tuner_software_c_band_agc(DeviceState &state,
                                              uint32_t frequency_hz,
                                              dtv_timeout timeout)
{
    constexpr std::array<uint16_t, 11> rf_normal{
        314, 33, 1023, 8935, 1023, 15829, 742, 9450, 468, 9216, 233,
    };
    constexpr std::array<uint16_t, 11> rf_p1g{
        280, 18, 504, 30060, 504, 61668, 364, 34925, 228, 37888, 109,
    };
    constexpr std::array<uint16_t, 7> bb_normal{500, 8, 400, 1024, 168, 1192, 400};
    constexpr std::array<uint16_t, 7> bb_soc{550, 520, 440, 2048, 208, 2256, 440};
    const auto &rf = state.tuner_p1g ? rf_p1g : rf_normal;
    const auto &bb = state.tuner_p1g ? bb_soc : bb_normal;
    auto result = tuner_write(state, 0x04, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x32, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(state, 0x39, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto target = tuner_wbd_db(state, state.tuner_wbd_offset);
    const auto rf_limit = static_cast<int32_t>(rf[0]) << 6;
    auto total_gain = (static_cast<int32_t>(rf[0] + bb[0]) / 2) << 5;
    uint16_t low_tracking = 0x7C00;
    for (;;) {
        int32_t wbd_sum = 0;
        for (uint8_t sample = 0; sample < 5; ++sample) {
            uint16_t raw = 0;
            result = tuner_read(state, 0x1D, &raw, timeout);
            if (!succeeded(result)) {
                return result;
            }
            wbd_sum += tuner_wbd_db(state, raw);
        }
        const auto wbd_error = target - wbd_sum / 5;
        auto gain2 = (low_tracking >> 10) & 7;
        const bool reduce_low_tracking = !state.tuner_p1g && wbd_error < 0 &&
                                         rf_limit != 0 && gain2 != 0;
        if (reduce_low_tracking) {
            gain2 >>= 1;
            low_tracking &= static_cast<uint16_t>(gain2 << 10);
        }
        result = delay_ms(state, 10, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = tuner_gpio6_output(state, true, timeout);
        if (!succeeded(result)) {
            return result;
        }
        if (!reduce_low_tracking) {
            break;
        }
    }
    if (static_cast<uint16_t>(rf_limit) < 2000) {
        result = delay_ms(state, 10, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = tuner_gpio6_output(state, false, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    uint8_t coarse_steps = 0;
    for (;;) {
        int32_t power = 0;
        result = demod_read_adc_power(state, &power, timeout);
        if (!succeeded(result)) {
            return result;
        }
        const auto numerator = static_cast<int64_t>(power) * 355774 + (1 << 20);
        auto adc = numerator / (1 << 21);
        if (numerator < 0 && numerator % (1 << 21) != 0) {
            --adc;
        }
        const auto error = static_cast<int32_t>(-220 - adc);
        const auto abs_error = error < 0 ? -error : error;
        if (abs_error < 50 || coarse_steps > 5) {
            result = tuner_write(state, 0x02, 0x99A0, timeout);
            if (!succeeded(result)) {
                return result;
            }
            return tuner_write(state, 0x04, 1, timeout);
        }
        ++coarse_steps;
        const auto desired_gain = std::clamp(total_gain + error, 0,
                                             rf_limit + (static_cast<int32_t>(bb[0]) << 5));
        const auto rf_gain = std::min(desired_gain, rf_limit);
        const auto bb_gain = desired_gain - rf_gain;
        result = tuner_apply_c_band_gain(state, rf, bb, rf_gain, bb_gain,
                                         low_tracking, timeout);
        if (!succeeded(result)) {
            return result;
        }
        total_gain = desired_gain;
        result = delay_ms(state, 10, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
}

dtv_internal_result demod_set_timing(DeviceState &state, uint32_t frequency_hz,
                                     dtv_timeout timeout)
{
    constexpr std::array<uint32_t, 4> ratios{17, 18, 19, 20};
    constexpr std::array<uint32_t, 4> timf{
        21387946, 20199727, 19136583, 18179756,
    };
    const auto dividend = (frequency_hz / 1000 * 256) / 1000;
    uint32_t selected = 3;
    uint32_t best_remainder = 717;
    for (uint32_t index = 0; index < ratios.size(); ++index) {
        const auto divisor = 192 * ratios[index];
        auto remainder = dividend % divisor;
        if (remainder > divisor / 2) {
            remainder = divisor - remainder;
        }
        if (remainder > best_remainder) {
            best_remainder = remainder;
            selected = index;
        }
    }
    auto result = demod_write(state, 29, static_cast<uint16_t>(timf[selected] >> 16), timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, 30, static_cast<uint16_t>(timf[selected]), timeout);
}

dtv_internal_result demod_set_wbd_target(DeviceState &state, uint32_t frequency_hz,
                                         dtv_timeout timeout)
{
    struct WbdTargetBand {
        uint32_t upper_mhz;
        int32_t low_slope;
        int32_t low_offset;
        int32_t high_slope;
        int32_t high_offset;
    };
    constexpr std::array<WbdTargetBand, 5> targets{{
        {120, 0, 500, 0, 500}, {170, 0, 450, 0, 450},
        {380, 48, 373, 28, 259}, {860, 34, 700, 36, 616},
        {65535, 34, 700, 36, 616},
    }};
    const auto frequency_mhz = frequency_hz / 1000000;
    const WbdTargetBand *target_band = &targets.back();
    for (const auto &candidate : targets) {
        if (frequency_mhz <= candidate.upper_mhz) {
            target_band = &candidate;
            break;
        }
    }
    const auto low = target_band->low_offset -
                     ((target_band->low_slope * static_cast<int32_t>(frequency_mhz)) >> 6);
    const auto high = target_band->high_offset -
                      ((target_band->high_slope * static_cast<int32_t>(frequency_mhz)) >> 6);
    const auto interpolated = low + ((high - low) * state.tuner_temperature >> 7);
    const auto raw = static_cast<uint16_t>(state.tuner_wbd_offset + interpolated);
    const auto calculated = ((static_cast<uint32_t>(raw) * 8 * 18 / 33) + 1) / 2;
    return demod_write(state, 106,
                       static_cast<uint16_t>(std::min<uint32_t>(calculated, 4095)), timeout);
}

dtv_internal_result demod_apply_agc(DeviceState &state, bool c_band, dtv_timeout timeout)
{
    const std::array<std::pair<uint16_t, uint16_t>, 16> profile{
        std::pair<uint16_t, uint16_t>{76, 10346}, {77, 787}, {78, 10},
        {101, 0x0076}, {102, 922}, {103, 2035}, {107, 0x0A00},
        {108, c_band ? 0 : 0xFFFF}, {109, 0}, {110, 0xFFFF}, {111, 0}, {112, 0x0020},
        {113, 0x8F90}, {114, 0x72E3}, {115, 0x7475}, {75, 114},
    };
    auto result = demod_adc_on_slow(state, timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (const auto &[reg, setting] : profile) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    uint16_t value = 0;
    result = demod_read(state, 923, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 923, (value & 0xFFE3) | 4, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 16, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 770, 0x0A00, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 770, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 14, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return delay_ms(state, 16, timeout);
}

dtv_internal_result demod_configure_channel(DeviceState &state, uint16_t sequence,
                                            uint16_t mode, uint16_t guard,
                                            dtv_timeout timeout)
{
    auto result = demod_write(state, 10, sequence << 4, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t value = 0;
    result = demod_read(state, 1, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1, (value & 0xFFFC) | guard, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 274, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto partial = sequence == 0 ? 0 : 1;
    result = demod_write(state, 274, (value & 0xFFCF) | (partial << 5), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 268, 1063, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr uint16_t layer_a = (3 << 10) | (13 << 6) | (2 << 3);
    constexpr std::array<uint16_t, 3> layers{layer_a, 0, 0};
    for (uint16_t layer = 0; layer < layers.size(); ++layer) {
        result = demod_write(state, 2 + layer, layers[layer], timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    const auto mode_number = mode == 1 ? 1 : mode == 2 ? 2 : 3;
    result = demod_write(state, 0, mode_number << 13, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 5> adp{
        std::pair<uint16_t, uint16_t>{215, 328}, {216, 65520}, {217, 164}, {218, 65528},
        {116, 7},
    };
    for (const auto &[reg, setting] : adp) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    constexpr std::array<uint16_t, 10> adc_target{
        7250, 7238, 7264, 7309, 7338, 7382, 7427, 7456, 7500, 7544,
    };
    for (uint16_t index = 0; index < adc_target.size(); ++index) {
        result = demod_write(state, 80 + index, adc_target[index], timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    constexpr std::array<int16_t, 24> filters{
        396, 305, 105, -51, -77, -12, 41, 31, -11, -30, -11, 14,
        15, -2, -13, -7, 5, 8, 1, -6, -7, -3, 0, 1,
    };
    for (uint16_t index = 0; index < filters.size(); ++index) {
        result = demod_write(state, 117 + index, static_cast<uint16_t>(filters[index]), timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 14> fixed{
        std::pair<uint16_t, uint16_t>{180, 0x0409}, {187, 0x4102},
        {181, 0x2800}, {182, 0x2800}, {183, 0x2800}, {184, 0x2800},
        {185, 0x2800}, {186, 0x2800}, {338, 0x1461}, {340, 0x0206},
        {341, 0x0027}, {228, 0}, {265, 31}, {205, 0x200F},
    };
    for (const auto &[reg, setting] : fixed) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = demod_write(state, 266, 0xE040, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 352, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 353, 0x1FFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 351, 0x00D5, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 1, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1, 0x3FFC | (value & 3), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 222, 1, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 223, 0x1000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (const auto reg : {189, 192, 225}) {
        result = demod_write(state, reg, 0xE000, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = demod_write(state, 288, sequence == 0 ? 0 : 0x1FFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 211, 0x1FFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 287, 0xF000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 178, 32, timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (const auto reg : {290, 291, 292}) {
        result = demod_write(state, reg, 1664, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    return demod_write(state, 250, 3285, timeout);
}

dtv_internal_result demod_set_pair(DeviceState &state, uint16_t high_reg,
                                   uint16_t high, uint16_t low,
                                   dtv_timeout timeout)
{
    auto result = demod_write(state, high_reg, high, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, high_reg + 1, low, timeout);
}

dtv_internal_result demod_set_diversity(DeviceState &state, bool internal_only,
                                        dtv_timeout timeout)
{
    auto result = demod_write(state, 1286, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 272, 0x0200, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t value = 0;
    result = demod_read(state, 273, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 273, (value & 0xFFF0) | 0x0006, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 270, internal_only ? 1 : 6, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 271, internal_only ? 0 : 6, timeout);
    if (!succeeded(result) || state.demod_revision != 0x8002) {
        return result;
    }
    result = demod_read(state, 903, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto original = value;
    result = demod_write(state, 903, value & ~8u, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = delay_ms(state, 30, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, 903, original, timeout);
}

dtv_internal_result demod_start_search(DeviceState &state, dtv_timeout timeout)
{
    uint16_t original = 0;
    auto result = demod_read(state, 0, &original, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 0, original | 0x8000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t value = 0;
    result = demod_read(state, 1284, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, 0, original, timeout);
}

dtv_internal_result demod_wait_search(DeviceState &state, uint64_t timeout_ms,
                                      uint16_t *status, dtv_timeout timeout)
{
    if (status == nullptr || state.io.clock == nullptr ||
        state.io.clock->monotonic_now_ns == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    const auto started = state.io.clock->monotonic_now_ns(state.io.clock->user);
    for (;;) {
        auto result = delay_ms(state, 10, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_read(state, 1284, status, timeout);
        if (!succeeded(result)) {
            return result;
        }
        if ((*status & 3) != 0) {
            return make_result(DTV_INTERNAL_RESULT_OK);
        }
        const auto now = state.io.clock->monotonic_now_ns(state.io.clock->user);
        if (now - started >= timeout_ms * 1000000) {
            return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
        }
    }
}

dtv_internal_result demod_search(DeviceState &state, uint16_t *mode,
                                 uint16_t *guard, dtv_timeout timeout)
{
    if (mode == nullptr || guard == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    uint16_t value = 0;
    if (state.demod_revision == 0x8002) {
        auto result = demod_write(state, 37, 0x0065, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 116, 0, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_read(state, 0, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 0, (value & 0x1FFF) | 0x8000, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_read(state, 1, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 1, value & 0xFFFC, timeout);
        if (!succeeded(result)) {
            return result;
        }
        for (const auto reg : {6, 7, 8}) {
            result = demod_write(state, reg, 0, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        result = demod_read(state, 10, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 10, (value & 0x0200) | 0x0100, timeout);
        if (!succeeded(result)) {
            return result;
        }
        for (const auto reg : {11, 13, 15}) {
            result = demod_set_pair(state, reg, 0x0010, 0x7AC0, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        constexpr std::array<std::pair<uint16_t, uint16_t>, 4> fft_periods{
            std::pair<uint16_t, uint16_t>{17, 0}, {18, 0x00C8}, {19, 0}, {20, 0x0190},
        };
        for (const auto &[reg, setting] : fft_periods) {
            result = demod_write(state, reg, setting, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        result = demod_set_pair(state, 21, 0x0010, 0x7AC0, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_read(state, 32, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 32, (value & 0xF0FF) | 0x0900, timeout);
        if (!succeeded(result)) {
            return result;
        }
        constexpr std::array<std::pair<uint16_t, uint16_t>, 3> fft_control{
            std::pair<uint16_t, uint16_t>{355, 2}, {356, 0}, {357, 0x0111},
        };
        for (const auto &[reg, setting] : fft_control) {
            result = demod_write(state, reg, setting, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        result = demod_read(state, 770, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 770, (value & 0xDFFF) | 0x2000, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_read(state, 770, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 770, value & 0xDFFF, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_read(state, 0, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 0, (value & 0x07FF) | 0x2000, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = delay_ms(state, 10, timeout);
        if (!succeeded(result)) {
            return result;
        }
        std::array<uint32_t, 3> correlations{};
        constexpr std::array<uint16_t, 3> high_regs{598, 596, 594};
        for (std::size_t index = 0; index < high_regs.size(); ++index) {
            uint16_t high = 0;
            uint16_t low = 0;
            result = demod_read(state, high_regs[index], &high, timeout);
            if (!succeeded(result)) {
                return result;
            }
            result = demod_read(state, high_regs[index] + 1, &low, timeout);
            if (!succeeded(result)) {
                return result;
            }
            correlations[index] = (static_cast<uint32_t>(high) << 16) | low;
        }
        *mode = 1;
        if (correlations[1] > correlations[0]) {
            *mode = 2;
        }
        if (correlations[2] > correlations[*mode - 1]) {
            *mode = 3;
        }
        uint16_t status = 0;
        result = demod_read(state, 1284, &status, timeout);
        if (!succeeded(result) || (status & 1) == 0) {
            return succeeded(result) ? make_result(DTV_INTERNAL_RESULT_TIMEOUT) : result;
        }
        result = demod_configure_channel(state, 16, *mode, 2, timeout);
        if (!succeeded(result)) {
            return result;
        }
        constexpr std::array<std::pair<uint16_t, uint16_t>, 3> guard_control{
            std::pair<uint16_t, uint16_t>{6, 4}, {7, 8}, {8, 0x1000},
        };
        for (const auto &[reg, setting] : guard_control) {
            result = demod_write(state, reg, setting, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        result = demod_set_pair(state, 11, 0x0029, 0x32E0, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_set_pair(state, 13, 0x00A4, 0xCB80, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_set_pair(state, 15, 0x0337, 0xF980, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 355, 3, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 356, 0, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_write(state, 357, 0x000F, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_start_search(state, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_wait_search(state, 1250, &status, timeout);
        if (!succeeded(result) || (status & 1) != 0 || (status & 2) == 0) {
            return succeeded(result) ? make_result(DTV_INTERNAL_RESULT_TIMEOUT) : result;
        }
        result = demod_read(state, 570, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        *guard = value & 3;
        return make_result(DTV_INTERNAL_RESULT_OK);
    }

    const auto list = state.demod_revision == 0x8000 || state.demod_revision == 0x8001
                          ? 7
                          : 0;
    *mode = 3;
    *guard = 2;
    auto result = demod_configure_channel(state, list, *mode, *guard, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint16_t, uint16_t>, 3> search_control{
        std::pair<uint16_t, uint16_t>{6, 4}, {7, 8}, {8, 0x1000},
    };
    for (const auto &[reg, setting] : search_control) {
        result = demod_write(state, reg, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = demod_set_pair(state, 11, 0x0029, 0x32E0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_set_pair(state, 13, 0x0052, 0x65C0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_set_pair(state, 15, 0x0337, 0xF980, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 355, 3, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 356, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 357, 0x000F, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_start_search(state, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t status = 0;
    result = demod_wait_search(state, 1200, &status, timeout);
    if (!succeeded(result) || (status & 1) != 0 || (status & 2) == 0) {
        return succeeded(result) ? make_result(DTV_INTERNAL_RESULT_TIMEOUT) : result;
    }
    result = demod_read(state, 570, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    *mode = (value >> 4) & 3;
    if (*mode < 1 || *mode > 2) {
        *mode = 3;
    }
    *guard = value & 3;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result demod_set_loops(DeviceState &state, uint16_t mode,
                                    bool tracking, dtv_timeout timeout)
{
    const auto mode_number = mode == 1 ? 1 : mode == 2 ? 2 : 3;
    const auto reg32 = tracking ? (((11 - mode_number) << 12) | 0x0680)
                                : (((9 - mode_number) << 12) | 0x0680);
    const auto reg37 = tracking ? (((5 + mode_number) << 5) | (10 - mode_number))
                                : ((3 << 5) | (8 - mode_number));
    auto result = demod_write(state, 32, static_cast<uint16_t>(reg32), timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_write(state, 37, static_cast<uint16_t>(reg37), timeout);
}

dtv_internal_result demod_poll_lock(DeviceState &state, uint64_t timeout_ms,
                                    uint16_t mask, bool wait_before_first,
                                    uint16_t *status,
                                    dtv_timeout timeout)
{
    if (status == nullptr || state.io.clock == nullptr ||
        state.io.clock->monotonic_now_ns == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    const auto started = state.io.clock->monotonic_now_ns(state.io.clock->user);
    const auto deadline = started + timeout_ms * 1000000;
    bool first = true;
    for (;;) {
        if (first && wait_before_first) {
            auto result = delay_ms(state, 10, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        auto result = demod_read(state, 568, status, timeout);
        if (!succeeded(result)) {
            return result;
        }
        if ((*status & mask) != 0) {
            return make_result(DTV_INTERNAL_RESULT_OK);
        }
        if (state.io.clock->monotonic_now_ns(state.io.clock->user) >= deadline) {
            return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
        }
        first = false;
        result = delay_ms(state, 10, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
}

dtv_internal_result demod_recover_tracking(DeviceState &state, uint16_t mode,
                                           dtv_timeout timeout)
{
    uint16_t value = 0;
    auto result = demod_read(state, 771, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 771, value & ~2u, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return demod_set_loops(state, mode, true, timeout);
}

dtv_internal_result demod_lock(DeviceState &state, uint32_t frequency_hz,
                               uint16_t mode, uint16_t guard, dtv_timeout timeout)
{
    uint16_t value = 0;
    auto result = demod_read(state, 565, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 771, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 771, value | 2, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 285, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 285, value & 0x0060, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto offset_khz = frequency_hz / 1000 > 250000 ? 63u : 143u;
    const uint32_t dds = offset_khz == 63 ? 0x01FECE5A : 0x01FD4A3A;
    result = demod_write(state, 26, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 27, static_cast<uint16_t>((dds >> 16) & 0x01FF), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 28, static_cast<uint16_t>(dds), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_set_loops(state, mode, false, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_configure_channel(state, 0, mode, guard, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 1286, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 273, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto base = mode == 1 ? 64 : mode == 2 ? 128 : 256;
    const auto sync_wait = base * (1 << guard) * 3 / 2 + 48;
    result = demod_write(state, 273,
                         static_cast<uint16_t>((value & 0x000F) | (sync_wait << 4)), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 770, 0x4000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 770, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_poll_lock(state, 4, 0x1800, true, &value, timeout);
    if (!succeeded(result)) {
        const auto recovery = demod_recover_tracking(state, mode, timeout);
        if (!succeeded(recovery)) {
            return recovery;
        }
        return result;
    }
    uint16_t timf_high = 0;
    uint16_t timf_low = 0;
    result = demod_read(state, 435, &timf_high, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_read(state, 436, &timf_low, timeout);
    if (!succeeded(result)) {
        return result;
    }
    state.timf = (static_cast<uint32_t>(timf_high) << 16) | timf_low;
    result = demod_write(state, 29, static_cast<uint16_t>(state.timf >> 16), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 30, static_cast<uint16_t>(state.timf), timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t window = 0;
    result = demod_read(state, 188, &window, timeout);
    if (!succeeded(result)) {
        return result;
    }
    const auto lmod4_timeout = ((window >> 5) & 0x1F) / 4;
    result = demod_poll_lock(state, lmod4_timeout, 0x0400, false, &value, timeout);
    if (!succeeded(result)) {
        const auto recovery = demod_recover_tracking(state, mode, timeout);
        if (!succeeded(recovery)) {
            return recovery;
        }
        return result;
    }
    result = demod_read(state, 771, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_write(state, 771, value & ~2u, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_set_loops(state, mode, true, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t wait_config = 0;
    result = demod_read(state, 1291, &wait_config, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t segments_a = 0;
    uint16_t interleave_a = 0;
    uint16_t target_layer = 0;
    for (uint16_t layer = 0; layer < 3; ++layer) {
        uint16_t segments = 0;
        uint16_t interleave = 0;
        result = demod_read(state, 493 + layer, &segments, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_read(state, 499 + layer, &interleave, timeout);
        if (!succeeded(result)) {
            return result;
        }
        segments &= 0xF;
        interleave &= 3;
        if (interleave == 3) {
            interleave = 4;
        }
        if (layer == 0) {
            segments_a = segments;
            interleave_a = interleave;
        }
        if (segments >= 1 && interleave >= interleave_a) {
            target_layer = layer;
        }
    }
    auto interleave_timeout = interleave_a == 0 ? 2
                              : interleave_a == 3 ? 8
                                                  : 2 * interleave_a;
    const auto mpeg_timeout = 200 * interleave_timeout;
    uint16_t status = 0;
    if ((wait_config & 0x0200) != 0) {
        result = demod_poll_lock(state, mpeg_timeout,
                                 static_cast<uint16_t>(1 << (7 - target_layer)),
                                 true, &status, timeout);
    } else {
        result = demod_read(state, 568, &status, timeout);
    }
    if (!succeeded(result)) {
        if (result.code != DTV_INTERNAL_RESULT_TIMEOUT || (status & 0x00E0) == 0) {
            return result;
        }
    }
    if (segments_a == 0) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    }
    if ((status & 0x1000) == 0 || (status & 0x00E0) == 0) {
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    result = demod_write(state, 1286, 0x0400, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result tune_device(dtv_context *context, dtv_spi_receiver_id,
                                uint32_t system, uint64_t frequency_hz,
                                dtv_spi_tune_result *tune_result, dtv_timeout timeout)
{
    auto *state = state_from(context);
    if (state == nullptr || !state->opened || !state->i2c_ready) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    if (tune_result == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    if (system != 1 || frequency_hz < 45000000 || frequency_hz > 860000000) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    auto result = demod_set_diversity(*state, false, timeout);
    if (!succeeded(result)) {
        return result;
    }
    if ((state->tuner_calibration_flags & 1) != 0) {
        result = tuner_dc_calibrate(*state, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    const auto frequency_khz = static_cast<uint32_t>(frequency_hz / 1000);
    const auto rf_khz = frequency_khz - (frequency_khz > 250000 ? 63u : 143u);
    const bool forced = (state->tuner_package == 4 || state->tuner_package == 7) &&
                        frequency_khz <= 250000;
    const TunerTuneBand *tune_band = nullptr;
    auto select_band = [&](const auto &bands) {
        for (const auto &candidate : bands) {
            if (rf_khz <= candidate.upper_khz) {
                tune_band = &candidate;
                return;
            }
        }
    };
    if (forced) {
        if (state->tuner_p1g) {
            select_band(kTunerP1gForcedTuneBands);
        } else {
            select_band(kTunerForcedTuneBands);
        }
    } else if (state->tuner_p1g) {
        select_band(kTunerP1gTuneBands);
    } else {
        select_band(kTunerTuneBands);
    }
    if (tune_band == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    result = tuner_gpio(*state, 0x0008, frequency_khz <= 250000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(*state, 0x0B, 0xB800 | tune_band->switch_trim, timeout);
    if (!succeeded(result)) {
        return result;
    }
    bool has_fraction = false;
    result = tuner_set_pll(*state, static_cast<uint32_t>(frequency_hz), &has_fraction, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_write(*state, 0x24,
                         static_cast<uint16_t>((tune_band->enable & 0xFF9F) | 0x0006), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_cap_trim(*state, has_fraction, state->tuner_tuned, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_wbd_calibrate(*state, static_cast<uint32_t>(frequency_hz), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_temperature_calibrate(*state, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_apply_steps(*state, *tune_band, static_cast<uint32_t>(frequency_hz), timeout);
    if (!succeeded(result)) {
        return result;
    }
    state->tuner_rf_khz = rf_khz;
    state->tuner_tuned = true;
    result = demod_set_timing(*state, static_cast<uint32_t>(frequency_hz), timeout);
    if (!succeeded(result)) {
        return result;
    }
    if (frequency_khz <= 170000) {
        result = tuner_software_c_band_agc(*state, static_cast<uint32_t>(frequency_hz), timeout);
    } else {
        result = demod_set_wbd_target(*state, static_cast<uint32_t>(frequency_hz), timeout);
    }
    if (!succeeded(result)) {
        return result;
    }
    result = demod_set_diversity(*state, true, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = tuner_reset_pwm_gain(*state, static_cast<uint32_t>(frequency_hz), timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_apply_agc(*state, frequency_khz <= 170000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    uint16_t mode = 0;
    uint16_t guard = 0;
    result = demod_search(*state, &mode, &guard, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = demod_lock(*state, static_cast<uint32_t>(frequency_hz), mode, guard, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result select_usb_group(const dtv_usb_candidate_info *candidates,
                                     uint32_t candidate_count, uint32_t anchor,
                                     dtv_usb_function_selection *out, uint32_t capacity,
                                     uint32_t *out_count, dtv_usb_group_match *match)
{
    if (candidates == nullptr || anchor >= candidate_count || out_count == nullptr ||
        match == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    *out_count = 0;
    *match = DTV_USB_GROUP_NO_MATCH;
    const auto &candidate = candidates[anchor];
    if (candidate.vendor_id != 0x054C || candidate.product_id != 0x04B2) {
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    if (candidate.interface_count != 1 || candidate.interfaces == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    const auto &interface = candidate.interfaces[0];
    if (interface.configuration != 1 || interface.interface_number != 0 ||
        interface.alternate_setting != 0 || interface.class_code != 0xFF ||
        interface.subclass_code != 0 || interface.protocol_code != 0 ||
        interface.endpoint_count != 4 || interface.endpoints == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    constexpr std::array<dtv_usb_endpoint_info, 4> expected_endpoints{{
        {0x81, 2, 512, 0}, {0x02, 2, 512, 0},
        {0x83, 3, 64, 1}, {0x84, 2, 512, 0},
    }};
    for (const auto &expected : expected_endpoints) {
        bool found = false;
        for (uint32_t index = 0; index < interface.endpoint_count; ++index) {
            const auto &endpoint = interface.endpoints[index];
            if (endpoint.address == expected.address &&
                endpoint.attributes == expected.attributes &&
                endpoint.max_packet_size == expected.max_packet_size &&
                endpoint.interval == expected.interval) {
                found = true;
                break;
            }
        }
        if (!found) {
            return make_result(DTV_INTERNAL_RESULT_OK);
        }
    }
    if (out == nullptr || capacity < 1) {
        return make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
    }
    out[0] = {anchor, kReceiverFunction, 0, 0};
    *out_count = 1;
    *match = DTV_USB_GROUP_COMPLETE;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result transform_ts(dtv_context *context, dtv_spi_receiver_id,
                                  dtv_bytes source, dtv_buffer *output)
{
    auto *state = state_from(context);
    if (output != nullptr) {
        output->size = 0;
    }
    if (state == nullptr || !state->opened || source.data == nullptr || source.size == 0 ||
        source.size % 192 != 0 || source.size > std::numeric_limits<std::size_t>::max() ||
        output == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    const auto output_size = (source.size / 192) * 188;
    if (output->data == nullptr || output->capacity < output_size ||
        output_size > std::numeric_limits<std::size_t>::max()) {
        return make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
    }
    const auto units = static_cast<std::size_t>(source.size / 192);
    for (std::size_t index = 0; index < units; ++index) {
        std::copy_n(source.data + index * 192 + 4, 188, output->data + index * 188);
    }
    output->size = output_size;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result timeout_after(DeviceState &state, dtv_timeout timeout,
                                   uint64_t duration_ms, dtv_timeout *bounded)
{
    if (bounded == nullptr || state.io.clock == nullptr ||
        state.io.clock->monotonic_now_ns == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    *bounded = timeout;
    const auto now = state.io.clock->monotonic_now_ns(state.io.clock->user);
    const auto duration_ns = duration_ms * 1000000;
    const auto deadline = now > std::numeric_limits<uint64_t>::max() - duration_ns
                              ? std::numeric_limits<uint64_t>::max()
                              : now + duration_ns;
    if (bounded->operation_deadline_ns == 0 ||
        deadline < bounded->operation_deadline_ns) {
        bounded->operation_deadline_ns = deadline;
    }
    if (bounded->timeout_ms == 0 || bounded->timeout_ms > duration_ms) {
        bounded->timeout_ms = duration_ms;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result flush_i2c(DeviceState &state, dtv_timeout timeout)
{
    if (!state.i2c_ready) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    dtv_timeout bounded{};
    auto result = timeout_after(state, timeout, 1000, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    for (;;) {
        uint32_t producer = 0;
        uint32_t progress = 0;
        result = read_word(state, 0x017A3850, &producer, bounded);
        if (!succeeded(result)) {
            return result;
        }
        result = read_word(state, 0x017A389C, &progress, bounded);
        if (!succeeded(result)) {
            return result;
        }
        if ((progress & 0x80000000) != 0) {
            state.i2c_ready = false;
            return make_result(DTV_INTERNAL_RESULT_INTERNAL, progress);
        }
        if (static_cast<uint16_t>(producer) == static_cast<uint16_t>(progress)) {
            return make_result(DTV_INTERNAL_RESULT_OK);
        }
        result = delay_ms(state, 1, bounded);
        if (!succeeded(result)) {
            return result;
        }
    }
}

dtv_internal_result configure_usb_input_engine(DeviceState &state, dtv_timeout timeout)
{
    constexpr std::array<std::pair<uint32_t, uint32_t>, 32> setup{{
        {0x00D000C4, 0}, {0x00D00000, 0xFFFFFFFF}, {0x00D00004, 0x001F33FF},
        {0x00D00000, 0x30030FFF}, {0x00D00000, 0x00030000}, {0x00D00004, 0x00003305},
        {0x00D01040, 0}, {0x00D01064, 0}, {0x00D010A0, 0}, {0x00D010A4, 0},
        {0x00D010A8, 0}, {0x00D01020, 0x02000000}, {0x00D01004, 0},
        {0x00D01044, 0x00BC0000}, {0x00D01048, 0x47000000},
        {0x00D0104C, 0x00FFFFFF}, {0x00D01050, 0}, {0x00D01060, 0},
        {0x00D01000, 0x06070000}, {0x00D01080, 0x01000000}, {0x00D01088, 0},
        {0x00D0108C, 0x000000FF}, {0x00D01084, 0}, {0x00D01090, 0x01000000},
        {0x00D02028, 0}, {0x00D0202C, 0}, {0x00D02030, 0}, {0x00D02038, 0},
        {0x00D02000, 0x03020300}, {0x00D02020, 0x00AE4000},
        {0x00D02024, 0x00600000}, {0x00D00004, 0x00043305},
    }};
    for (const auto &[address, value] : setup) {
        auto result = write_word(state, address, value, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    uint32_t value = 0;
    auto result = read_word(state, 0x00FD8010, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x00FD8010, value | 0x000F0000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = read_word(state, 0x00FD8014, &value, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x00FD8014, value & ~0x000F0000u, timeout);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<std::pair<uint32_t, uint32_t>, 9> triggers{{
        {0x00FD8200, 1}, {0x00FD8220, 1}, {0x00FD8200, 0},
        {0x00FD8204, 0xB09F0000}, {0x00FD8200, 2}, {0x00FD8220, 0},
        {0x00FD8224, 0xB09F2000}, {0x00FD8220, 2}, {0x00FDBD08, 0x00FF6001},
    }};
    for (const auto &[address, setting] : triggers) {
        result = write_word(state, address, setting, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    dtv_timeout wait_timeout{};
    result = timeout_after(state, timeout, 100, &wait_timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (;;) {
        result = read_word(state, 0x00FDBD08, &value, wait_timeout);
        if (!succeeded(result)) {
            return result;
        }
        if ((value & 1) == 0) {
            break;
        }
        result = delay_ms(state, 1, wait_timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    result = write_word(state, 0x00FDBD08, 0x00FF6000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    return write_word(state, 0x00FDBC14, 0x00060030, timeout);
}

dtv_internal_result load_core0(DeviceState &state, dtv_timeout timeout)
{
    auto result = write_word(state, 0x01600000, 0xFFFFFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x016FFFFC, 0x000A0000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    std::array<uint8_t, 64> block{};
    std::array<uint8_t, 64> readback{};
    for (uint32_t offset = 0; offset < 0x800; offset += block.size()) {
        result = transfer_block(state, true, 0x016A3800 + offset,
                                block.data(), block.size(), timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    auto session_high = static_cast<uint16_t>((state.core0_session >> 16) + 1);
    if (session_high == 0) {
        session_high = 1;
    }
    state.core0_session = static_cast<uint32_t>(session_high) << 16;
    auto epoch_sequence = static_cast<uint16_t>(state.core0_epoch) + 1;
    if (epoch_sequence == 0) {
        epoch_sequence = 1;
    }
    state.core0_epoch = 0x00040000 | epoch_sequence;
    block.fill(0);
    store_be32(block.data(), state.core0_session);
    store_be32(block.data() + 0x10, state.core0_epoch);
    store_be32(block.data() + 0x14, 0x00AE4000);
    store_be32(block.data() + 0x18, 0x00600000);
    store_be32(block.data() + 0x1C, 192);
    store_be32(block.data() + 0x20, 0x00D02038);
    store_be32(block.data() + 0x24, 0x00D0202C);
    store_be32(block.data() + 0x28, 8);
    store_be32(block.data() + 0x2C, 10);
    result = transfer_block(state, true, 0x016A3840,
                            block.data(), block.size(), timeout);
    if (!succeeded(result)) {
        return result;
    }
    for (std::size_t offset = 0; offset < state.core0_image.size(); offset += block.size()) {
        block.fill(0);
        const auto remaining = state.core0_image.size() - offset;
        const auto count = remaining < block.size() ? remaining : block.size();
        std::copy_n(state.core0_image.data() + offset, count, block.data());
        result = transfer_block(state, true,
                                0x016A0000 + static_cast<uint32_t>(offset),
                                block.data(), block.size(), timeout);
        if (!succeeded(result)) {
            return result;
        }
        readback.fill(0);
        result = transfer_block(state, false,
                                0x016A0000 + static_cast<uint32_t>(offset),
                                readback.data(), readback.size(), timeout);
        if (!succeeded(result)) {
            return result;
        }
        if (readback != block) {
            return make_result(DTV_INTERNAL_RESULT_USB_IO,
                               static_cast<uint32_t>(offset));
        }
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result start_usb_input(DeviceState &state, dtv_timeout timeout)
{
    constexpr std::array<std::pair<uint32_t, uint32_t>, 5> setup{{
        {0x00D01020, 0}, {0x00D01040, 0x03000000},
        {0x00D010A4, 0x13170400}, {0x00D00484, 0x01000000},
        {0x00D00488, 0x206C0000},
    }};
    for (const auto &[address, value] : setup) {
        auto result = write_word(state, address, value, timeout);
        if (!succeeded(result)) {
            return result;
        }
    }
    return write_word(state, 0x00D02010, 0x02000000, timeout);
}

dtv_internal_result wait_core0_running(DeviceState &state, dtv_timeout timeout)
{
    dtv_timeout bounded{};
    auto result = timeout_after(state, timeout, 100, &bounded);
    if (!succeeded(result)) {
        return result;
    }
    constexpr std::array<uint32_t, 8> expected_info{
        0x54545330, 0x00010000, 0x07E65E29, 1, 0, 1332, 0x000A3800, 0x00000840,
    };
    uint32_t first_heartbeat = 0;
    bool have_heartbeat = false;
    for (;;) {
        std::array<uint32_t, 8> info{};
        for (std::size_t index = 0; index < info.size(); ++index) {
            result = read_word(state, 0x016A3800 + static_cast<uint32_t>(index * 4),
                               &info[index], bounded);
            if (!succeeded(result)) {
                return result;
            }
        }
        uint32_t status = 0;
        uint32_t heartbeat = 0;
        uint32_t fault = 0;
        uint32_t epoch = 0;
        result = read_word(state, 0x016A3880, &status, bounded);
        if (!succeeded(result)) {
            return result;
        }
        result = read_word(state, 0x016A3884, &heartbeat, bounded);
        if (!succeeded(result)) {
            return result;
        }
        result = read_word(state, 0x016A3888, &fault, bounded);
        if (!succeeded(result)) {
            return result;
        }
        result = read_word(state, 0x016A3898, &epoch, bounded);
        if (!succeeded(result)) {
            return result;
        }
        if (status == 0xE0) {
            return make_result(DTV_INTERNAL_RESULT_INTERNAL, fault);
        }
        if (!have_heartbeat) {
            first_heartbeat = heartbeat;
            have_heartbeat = true;
        }
        const bool info_ready = info[0] == expected_info[0] &&
                                info[1] == expected_info[1] &&
                                info[2] == expected_info[2] &&
                                info[3] == expected_info[3] &&
                                info[4] == state.core0_session &&
                                info[5] == expected_info[5] &&
                                info[6] == expected_info[6] &&
                                info[7] == expected_info[7];
        if (status == 3 && info_ready && epoch == state.core0_epoch &&
            heartbeat != first_heartbeat) {
            return make_result(DTV_INTERNAL_RESULT_OK);
        }
        result = delay_ms(state, 1, bounded);
        if (!succeeded(result)) {
            return result;
        }
    }
}

dtv_internal_result stop_core0(DeviceState &state, dtv_timeout timeout)
{
    if (!state.core0_stream_active) {
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    auto result = write_word(state, 0x01600000, 0xFFFFFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x00D01040, 0, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x00D01020, 0x02000000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    result = write_word(state, 0x00D02010, 0x04000000, timeout);
    if (!succeeded(result)) {
        return result;
    }
    state.core0_stream_active = false;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result abort_core0_start(DeviceState &state,
                                      dtv_internal_result start_result,
                                      dtv_timeout timeout)
{
    stop_core0(state, timeout);
    return start_result;
}

dtv_internal_result prepare_ts(dtv_context *context, dtv_spi_receiver_id,
                               uint64_t, const dtv_stream_program *, dtv_timeout timeout)
{
    auto *state = state_from(context);
    if (state == nullptr || !state->opened || !state->i2c_ready ||
        !state->frontend_ready || !state->tuner_tuned || !state->core0_image_ready) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    if (state->core0_stream_active) {
        return make_result(DTV_INTERNAL_RESULT_BUSY);
    }
    auto result = flush_i2c(*state, timeout);
    if (!succeeded(result)) {
        return result;
    }
    state->core0_stream_active = true;
    result = configure_usb_input_engine(*state, timeout);
    if (!succeeded(result)) {
        return abort_core0_start(*state, result, timeout);
    }
    result = load_core0(*state, timeout);
    if (!succeeded(result)) {
        return abort_core0_start(*state, result, timeout);
    }
    result = start_usb_input(*state, timeout);
    if (!succeeded(result)) {
        return abort_core0_start(*state, result, timeout);
    }
    result = write_word(*state, 0x016A0000, 0xFFFCFFFF, timeout);
    if (!succeeded(result)) {
        return abort_core0_start(*state, result, timeout);
    }
    result = wait_core0_running(*state, timeout);
    if (!succeeded(result)) {
        return abort_core0_start(*state, result, timeout);
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result stop_ts_device(dtv_context *context, dtv_spi_receiver_id,
                                   dtv_timeout timeout)
{
    auto *state = state_from(context);
    if (state == nullptr || !state->opened) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    return stop_core0(*state, timeout);
}

dtv_internal_result close_device(dtv_context *context, dtv_timeout timeout)
{
    auto *state = state_from(context);
    if (state == nullptr || !state->opened) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    if (state->core0_stream_active) {
        return make_result(DTV_INTERNAL_RESULT_BUSY);
    }
    auto result = write_word(*state, 0x01600000, 0xFFFFFFFF, timeout);
    if (!succeeded(result)) {
        return result;
    }
    auto first_failure = make_result(DTV_INTERNAL_RESULT_OK);
    if (state->frontend_ready) {
        if (!state->i2c_ready) {
            first_failure = make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        } else {
            result = tuner_gpio(*state, 0x0001, true, timeout);
            if (succeeded(result)) {
                result = demod_write(*state, 1286, 0, timeout);
            }
            if (succeeded(result)) {
                result = demod_power(*state, true, timeout);
            }
            uint16_t slow = 0;
            if (succeeded(result)) {
                result = demod_read(*state, 908, &slow, timeout);
            }
            if (succeeded(result)) {
                result = demod_write(*state, 908, slow | 0x0003, timeout);
            }
            if (succeeded(result)) {
                result = demod_adc_off(*state, timeout);
            }
            if (succeeded(result)) {
                state->frontend_ready = false;
            } else {
                first_failure = result;
            }
        }
    }
    if (state->core1_running) {
        result = write_word(*state, 0x01700000, 0xFFFFFFFF, timeout);
        if (succeeded(result)) {
            state->core1_running = false;
            state->i2c_ready = false;
        } else if (succeeded(first_failure)) {
            first_failure = result;
        }
    }
    if (!succeeded(first_failure)) {
        return first_failure;
    }
    state->i2c_ready = false;
    state->core0_image_ready = false;
    state->opened = false;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result install_firmware(dtv_context *context, uint32_t stage, dtv_bytes firmware,
                                     dtv_firmware_result *firmware_result,
                                     dtv_timeout timeout)
{
    auto *state = state_from(context);
    if (state == nullptr || !state->opened || firmware_result == nullptr ||
        stage < kFirmwareStageWiden || stage > kFirmwareStageFrontendPower) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    auto result = validate_firmware_image(stage, firmware);
    if (!succeeded(result)) {
        return result;
    }
    if (stage == kFirmwareStageWiden) {
        result = install_widen(*state, firmware.data, timeout);
        if (!succeeded(result)) {
            return result;
        }
    } else if (stage == kFirmwareStageCore1) {
        result = initialize_bridge(*state, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = start_core1(*state, firmware.data, timeout);
        if (!succeeded(result)) {
            return result;
        }
        state->core1_running = true;
    } else {
        state->core0_image_ready = false;
        state->frontend_ready = false;
        uint32_t value = 0;
        for (const auto address : {0x00090018u, 0x00090014u}) {
            result = read_word(*state, address, &value, timeout);
            if (!succeeded(result)) {
                return result;
            }
            const auto updated = address == 0x00090018
                                    ? ((value & 0xFFFF) | 0x00400000)
                                    : ((value & 0xFFFF) | 0x00410000);
            result = write_word(*state, address, updated, timeout);
            if (!succeeded(result)) {
                return result;
            }
        }
        result = delay_ms(*state, 50, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = read_word(*state, 0x00090018, &value, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = write_word(*state, 0x00090018, (value & 0xFFFF) | 0x00410000, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = delay_ms(*state, 10, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = initialize_i2c_ring(*state, timeout);
        if (!succeeded(result)) {
            return result;
        }
        result = demod_initialize(*state, timeout);
        if (!succeeded(result)) {
            state->i2c_ready = false;
            return result;
        }
        result = tuner_initialize(*state, timeout);
        if (!succeeded(result)) {
            state->i2c_ready = false;
            return result;
        }
        std::copy_n(firmware.data, kCore0Size, state->core0_image.begin());
        state->core0_image_ready = true;
        state->frontend_ready = true;
    }
    firmware_result->firmware_key = 0;
    firmware_result->transition = DTV_FIRMWARE_SAME_HANDLE;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

const dtv::spi::DeviceOps bootstrap_ops{
    .open = open_device,
    .identify = identify_device,
    .firmware = install_firmware,
};

const dtv::spi::DeviceOps runtime_ops{
    .open = open_device,
    .identify = identify_device,
    .tune_t = tune_device,
    .ts_prepare = prepare_ts,
    .ts_transform = transform_ts,
    .ts_stop_device = stop_ts_device,
    .close = close_device,
};

const dtv::spi::FirmwareEntry firmware_entries[]{
    {kFirmwareKey, &runtime_ops},
};

const dtv::spi::FirmwareManifestImage firmware_images[]{
    {TORNE_FR30_WIDEN_BANK0_OFFSET, TORNE_FR30_WIDEN_BANK0_BYTES,
     TORNE_FR30_WIDEN_BANK0_SHA256},
    {TORNE_FR30_CORE0_OFFSET, TORNE_FR30_CORE0_BYTES, TORNE_FR30_CORE0_SHA256},
    {TORNE_FR30_CORE1_OFFSET, TORNE_FR30_CORE1_BYTES, TORNE_FR30_CORE1_SHA256},
};

const dtv::spi::FunctionFirmwareStage firmware_stages[]{
    {kFirmwareStageWiden, 0, DTV_IDENTIFY_LOADER, DTV_IDENTIFY_LOADER},
    {kFirmwareStageCore1, 2, DTV_IDENTIFY_LOADER, DTV_IDENTIFY_RUNTIME},
    {kFirmwareStageFrontendPower, 1, DTV_IDENTIFY_RUNTIME, DTV_IDENTIFY_RUNTIME},
};

const dtv_usb_identity cold_identities[]{{0x054C, 0x04B2}};

const dtv::spi::FunctionEntry function_entries[]{
    {
        .function = kReceiverFunction,
        .bootstrap_ops = &bootstrap_ops,
        .firmware = firmware_entries,
        .firmware_count = 1,
        .images = firmware_images,
        .image_count = std::size(firmware_images),
        .stages = firmware_stages,
        .stage_count = std::size(firmware_stages),
        .create_state = create_state,
        .destroy_state = destroy_state,
        .expected_package_size = TORNE_FR30_FIRMWARE_BYTES,
        .expected_package_sha256 = TORNE_FR30_FIRMWARE_SHA256,
    },
};

const dtv_function receiver_functions[]{kReceiverFunction};

const dtv::spi::OperationRequirement operation_requirements[]{
    {kReceiverId, DTV_SPI_OPERATION_TUNE_ISDB_T,
     receiver_functions, std::size(receiver_functions)},
};

}

namespace dtv::spi {

extern const ModelEntry cech_zd1j_model{
    .model = {"CECH-ZD1J", 0x054C, 0x04B2},
    .firmware = firmware_entries,
    .firmware_count = 1,
    .create_state = create_state,
    .destroy_state = destroy_state,
    .bootstrap_ops = &bootstrap_ops,
    .cold_identities = cold_identities,
    .cold_identity_count = std::size(cold_identities),
    .select_usb_group = select_usb_group,
    .functions = function_entries,
    .function_count = std::size(function_entries),
    .operation_requirements = operation_requirements,
    .operation_requirement_count = std::size(operation_requirements),
};

}
