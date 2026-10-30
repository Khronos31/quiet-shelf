// SPDX-License-Identifier: GPL-2.0-only
#include "widen/loader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace torne::widen {
namespace {

constexpr std::array<std::uint32_t, 5U> kPatchAddresses{
    0x0E9AC0U, 0x0E9AC4U, 0x0E9E0CU, 0x0E9E10U, 0x0E9D94U};
constexpr std::uint32_t kTrampolineWord = 0x970C9FA0U;
constexpr std::uint32_t kTailPatchWord = 0x9FA09F8DU;
constexpr std::uint32_t kBank0 = 0x0E1000U;
constexpr std::uint32_t kBank1 = 0x0E1800U;
constexpr std::uint32_t kHeaderBytes = 0x20U;
constexpr std::uint32_t kMaxImageBytes = 0x800U;
constexpr std::uint32_t kCore0HoldAddress = 0x01600000U;
constexpr std::uint32_t kCore1HoldAddress = 0x01700000U;
constexpr std::uint32_t kHoldValue = 0xFFFFFFFFU;
constexpr std::uint32_t kFunctionalProbeAddress = 0x017A3FC0U;
constexpr std::uint16_t kFunctionalProbeLength = 64U;

bool valid_functional_policy(const IntroductionPolicy& policy) noexcept
{
    return policy.core0_hold_address == kCore0HoldAddress &&
           policy.core1_hold_address == kCore1HoldAddress &&
           policy.hold_value == kHoldValue &&
           policy.functional_probe_address == kFunctionalProbeAddress &&
           policy.functional_probe_length == kFunctionalProbeLength &&
           policy.legacy_probe_address < 0x01000000U &&
           (policy.legacy_probe_address & 0x03U) == 0U;
}

bool is_bank_base(std::uint32_t base) noexcept
{
    return base == kBank0 || base == kBank1;
}

std::uint32_t other_bank(std::uint32_t base) noexcept
{
    return base == kBank0 ? kBank1 : kBank0;
}

std::uint32_t read_be32(const std::uint8_t* bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

bool valid_twdn_image(const RegisteredImage& image) noexcept
{
    if (image.format != ImageFormat::twdn || image.expected_image == nullptr ||
        !is_bank_base(image.bank_base) || image.image_bytes <= kHeaderBytes ||
        image.image_bytes > kMaxImageBytes || (image.image_bytes % 4U) != 0U)
        return false;
    const std::uint32_t end = image.bank_base + image.image_bytes;
    for (const std::uint32_t entry : {image.entry_read, image.entry_write}) {
        if (entry < image.bank_base + kHeaderBytes || entry >= end ||
            (entry & 0x03U) != 0U)
            return false;
    }
    const std::uint8_t* header = image.expected_image;
    return read_be32(header) == 0x5457444EU &&
           read_be32(header + 4U) == 0x00010000U &&
           (read_be32(header + 12U) & 0x01U) != 0U &&
           read_be32(header + 16U) == image.image_bytes &&
           read_be32(header + 20U) == image.entry_read &&
           read_be32(header + 24U) == image.entry_write &&
           read_be32(header + 28U) == 0U;
}

std::array<std::uint32_t, 5U> five_for_record(
    const RegisteredImage& image) noexcept
{
    if (image.format == ImageFormat::legacy_coax) {
        return {image.patches[0].value, image.patches[1].value,
                image.patches[2].value, image.patches[3].value,
                image.patches[4].value};
    }
    return {make_jump_word(image.entry_read), kTrampolineWord,
            make_jump_word(image.entry_write), kTrampolineWord,
            kTailPatchWord};
}

bool same_five(const std::array<std::uint32_t, 5U>& left,
               const std::array<std::uint32_t, 5U>& right) noexcept
{
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

const RegisteredImage* find_image(const RegisteredImage* images,
                                  std::size_t count,
                                  std::uint32_t base) noexcept
{
    const RegisteredImage* found = nullptr;
    for (std::size_t index = 0U; index < count; ++index) {
        if (images[index].bank_base != base) continue;
        if (found != nullptr) return nullptr;
        found = &images[index];
    }
    return found;
}

void set_transfer_failure(LoaderResult& result,
                          const usb::TransferOutcome& transfer,
                          bool write) noexcept
{
    result.failure = transfer;
    if (transfer.cause == usb::TransferCause::no_device) {
        result.error = LoaderError::device_gone;
    } else if (transfer.state == usb::TransferState::outcome_unknown) {
        result.error = LoaderError::outcome_unknown;
    } else {
        result.error = write ? LoaderError::write_failed
                             : LoaderError::read_failed;
    }
}

bool read_five(Ep0Io& io, std::array<std::uint32_t, 5U>& five,
               LoaderResult& result) noexcept
{
    for (std::size_t index = 0U; index < kPatchAddresses.size(); ++index) {
        const ReadResult read = io.read32(kPatchAddresses[index]);
        if (!read.ok) {
            set_transfer_failure(result, read.transfer, false);
            return false;
        }
        five[index] = read.value;
    }
    return true;
}

bool write_24(Ep0Io& io, std::uint32_t address, std::uint32_t value,
              LoaderResult& result) noexcept
{
    result.wrote_any = true;
    const WriteResult write = io.write32(address, value, false);
    if (!write.ok) {
        set_transfer_failure(result, write.transfer, true);
        return false;
    }
    return true;
}

bool write_wide32(Ep0Io& io, std::uint32_t address, std::uint32_t value,
                  LoaderResult& result) noexcept
{
    result.wrote_any = true;
    const WriteResult write = io.write32(address, value, true);
    if (!write.ok) {
        set_transfer_failure(result, write.transfer, true);
        return false;
    }
    return true;
}

bool write_wide_block(Ep0Io& io, std::uint32_t address,
                      const std::uint8_t* data, std::size_t length,
                      LoaderResult& result) noexcept
{
    result.wrote_any = true;
    const WriteResult write = io.write_block_wide(address, data, length);
    if (!write.ok) {
        set_transfer_failure(result, write.transfer, true);
        return false;
    }
    return true;
}

bool run_functional_check(Ep0Io& io, const IntroductionPolicy& policy,
                          LoaderResult& result) noexcept
{
    result.stage = LoaderStage::functional_check;
    if (!policy.allow_functional_check) {
        result.error = LoaderError::functional_check_not_allowed;
        return false;
    }
    if (!valid_functional_policy(policy)) {
        result.error = LoaderError::functional_check_failed;
        return false;
    }

    const std::uint32_t cores[] = {policy.core0_hold_address,
                                   policy.core1_hold_address};
    for (const std::uint32_t address : cores) {
        if (!write_wide32(io, address, policy.hold_value, result)) return false;
    }

    std::array<std::uint8_t, 64U> pattern{};
    for (std::size_t index = 0U; index < policy.functional_probe_length;
         ++index) {
        pattern[index] = static_cast<std::uint8_t>(
            policy.functional_pattern_seed + static_cast<std::uint8_t>(index));
    }
    if (!write_wide_block(io, policy.functional_probe_address, pattern.data(),
                          policy.functional_probe_length, result))
        return false;
    const BlockReadResult block = io.read_block_wide(
        policy.functional_probe_address, policy.functional_probe_length);
    if (!block.ok) {
        set_transfer_failure(result, block.transfer, false);
        return false;
    }
    if (block.length != policy.functional_probe_length ||
        std::memcmp(block.bytes.data(), pattern.data(),
                    policy.functional_probe_length) != 0) {
        result.error = LoaderError::functional_check_failed;
        return false;
    }

    // The legacy 24bit path must still work through the widened handler. This
    // only proves the request succeeds; it does not prove a particular value.
    const ReadResult legacy = io.read32(policy.legacy_probe_address);
    if (!legacy.ok) {
        set_transfer_failure(result, legacy.transfer, false);
        return false;
    }
    result.legacy_24bit_checked = true;
    result.functional_checked = true;
    result.ok = true;
    result.error = LoaderError::none;
    result.stage = LoaderStage::done;
    return true;
}

bool load_and_patch(Ep0Io& io, const RegisteredImage& target,
                    const std::array<std::uint32_t, 5U>& source_five,
                    const IntroductionPolicy& policy,
                    LoaderResult& result) noexcept
{
    result.target_bank_base = target.bank_base;
    result.stage = LoaderStage::image_write;
    for (std::uint32_t offset = 0U; offset < target.image_bytes; offset += 4U) {
        const std::uint32_t value = read_be32(target.expected_image + offset);
        if (!write_24(io, target.bank_base + offset, value, result))
            return false;
        ++result.image_words_written;
    }
    result.image_written = true;

    result.stage = LoaderStage::image_readback;
    for (std::uint32_t offset = 0U; offset < target.image_bytes; offset += 4U) {
        const ReadResult read = io.read32(target.bank_base + offset);
        if (!read.ok) {
            set_transfer_failure(result, read.transfer, false);
            return false;
        }
        if (read.value != read_be32(target.expected_image + offset)) {
            result.error = LoaderError::readback_mismatch;
            return false;
        }
    }

    result.stage = LoaderStage::recheck;
    std::array<std::uint32_t, 5U> five{};
    if (!read_five(io, five, result)) return false;
    if (!same_five(five, source_five)) {
        result.error = LoaderError::state_changed;
        return false;
    }
    if (policy.stop_after_image_readback) {
        result.error = LoaderError::image_verified_stop;
        return false;
    }

    result.stage = LoaderStage::patch_write;
    const std::array<std::uint32_t, 5U> new_five = five_for_record(target);
    for (std::size_t index = 0U; index < kPatchAddresses.size(); ++index) {
        if (!write_24(io, kPatchAddresses[index], new_five[index], result))
            return false;
        ++result.patch_words_written;
    }
    result.patches_applied = true;

    result.stage = LoaderStage::patch_readback;
    if (!read_five(io, five, result)) return false;
    if (!same_five(five, new_five)) {
        result.error = LoaderError::readback_mismatch;
        return false;
    }

    return run_functional_check(io, policy, result);
}

}  // namespace

LoaderResult introduce_widen(Ep0Io& io, const RegisteredImage* current,
                             const RegisteredImage* known,
                             std::size_t known_count,
                             const RegisteredImage* images,
                             std::size_t image_count,
                             const IntroductionPolicy& policy) noexcept
{
    LoaderResult result;
    if (images == nullptr || image_count == 0U ||
        (known_count != 0U && known == nullptr) ||
        (current != nullptr && !valid_twdn_image(*current)) ||
        (policy.allow_functional_check && !valid_functional_policy(policy))) {
        result.error = LoaderError::invalid_argument;
        return result;
    }
    for (std::size_t index = 0U; index < image_count; ++index) {
        if (!valid_twdn_image(images[index])) {
            result.error = LoaderError::invalid_argument;
            return result;
        }
        for (std::size_t other = index + 1U; other < image_count; ++other) {
            if (images[index].bank_base == images[other].bank_base) {
                result.error = LoaderError::invalid_argument;
                return result;
            }
        }
    }

    const RecognitionResult recognition =
        recognize_read_only(io, current, known, known_count);
    result.observed = recognition.state;
    if (!recognition.classified) {
        set_transfer_failure(result, recognition.failure, false);
        return result;
    }

    if (recognition.state == WidenState::unknown) {
        result.error = LoaderError::unknown_state;
        return result;
    }
    if (recognition.state == WidenState::partial_ours) {
        result.error = LoaderError::partial_state;
        return result;
    }

    if (recognition.state == WidenState::ours_active) {
        if (current == nullptr) {
            result.error = LoaderError::invalid_argument;
            return result;
        }
        result.target_bank_base = current->bank_base;
        result.stage = LoaderStage::recheck;
        std::array<std::uint32_t, 5U> five{};
        if (!read_five(io, five, result)) return result;
        if (!same_five(five, recognition.five)) {
            result.error = LoaderError::state_changed;
            return result;
        }
        result.patches_applied = true;
        run_functional_check(io, policy, result);
        return result;
    }

    result.stage = LoaderStage::recheck;
    std::array<std::uint32_t, 5U> source_five{};
    if (!read_five(io, source_five, result)) return result;

    if (!same_five(source_five, recognition.five)) {
        result.error = LoaderError::state_changed;
        return result;
    }

    std::uint32_t target_base = 0U;
    if (recognition.state == WidenState::pristine) {
        target_base = kBank0;
    } else if (recognition.state == WidenState::foreign_known) {
        if (recognition.matched == nullptr ||
            !is_bank_base(recognition.matched->bank_base)) {
            result.error = LoaderError::unknown_state;
            return result;
        }
        target_base = other_bank(recognition.matched->bank_base);
    } else {
        result.error = LoaderError::unknown_state;
        return result;
    }

    const RegisteredImage* target = find_image(images, image_count, target_base);
    if (target == nullptr) {
        result.target_bank_base = target_base;
        result.error = LoaderError::missing_image;
        return result;
    }
    result.target_bank_base = target_base;
    if (!policy.allow_bank_write) {
        result.error = LoaderError::introduction_not_allowed;
        return result;
    }

    load_and_patch(io, *target, source_five, policy, result);
    return result;
}

}  // namespace torne::widen
