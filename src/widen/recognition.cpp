// SPDX-License-Identifier: GPL-2.0-only
#include "widen/recognition.h"

#include <dtv/userland/detail/sha256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace torne::widen {
namespace {

constexpr std::array<std::uint32_t, 5U> kPatchAddresses{
    0x0E9AC0U, 0x0E9AC4U, 0x0E9E0CU, 0x0E9E10U, 0x0E9D94U};
constexpr std::array<std::uint32_t, 5U> kPristineWords{
    0x9B800000U, 0xAA04E40FU, 0x9B800000U, 0xAA04E40DU, 0xB4279F8DU};
constexpr std::uint32_t kTrampolineWord = 0x970C9FA0U;
constexpr std::uint32_t kTailPatchWord = 0x9FA09F8DU;
constexpr std::uint32_t kTwdnMagic = 0x5457444EU;
constexpr std::uint32_t kTwdnAbi = 0x00010000U;
constexpr std::uint32_t kMaxImageBytes = 0x800U;
constexpr std::uint32_t kHeaderBytes = 0x20U;

enum class Verify { match, no_match, read_failure };

bool is_widen_bank_base(std::uint32_t base) noexcept
{
    return base == 0x0E1000U || base == 0x0E1800U;
}

bool record_entries_in_bank(const RegisteredImage& image,
                            std::uint32_t lower_bound,
                            std::uint32_t alignment) noexcept
{
    const std::uint32_t end = image.bank_base + image.image_bytes;
    if (!is_widen_bank_base(image.bank_base) ||
        image.image_bytes == 0U ||
        image.image_bytes > kMaxImageBytes || image.image_bytes % 4U != 0U)
        return false;
    for (const std::uint32_t entry : {image.entry_read, image.entry_write}) {
        if (entry < image.bank_base + lower_bound || entry >= end ||
            entry % alignment != 0U)
            return false;
    }
    return true;
}

bool valid_legacy_record(const RegisteredImage& image) noexcept
{
    if (image.format != ImageFormat::legacy_coax ||
        !record_entries_in_bank(image, 0U, 2U))
        return false;
    for (std::size_t index = 0U; index < kPatchAddresses.size(); ++index) {
        if (image.patches[index].address != kPatchAddresses[index]) return false;
    }
    return image.patches[0].value == make_jump_word(image.entry_read) &&
           image.patches[1].value == kTrampolineWord &&
           image.patches[2].value == make_jump_word(image.entry_write) &&
           image.patches[3].value == kTrampolineWord &&
           image.patches[4].value == kTailPatchWord;
}

bool valid_twdn_record(const RegisteredImage& image) noexcept
{
    return image.format == ImageFormat::twdn &&
           record_entries_in_bank(image, kHeaderBytes, 4U) &&
           image.expected_image != nullptr;
}

bool matches_legacy_five(const std::vector<std::uint32_t>& observed,
                         const RegisteredImage& image) noexcept
{
    if (observed.size() != 5U) return false;
    for (std::size_t index = 0U; index < observed.size(); ++index) {
        if (observed[index] != image.patches[index].value) return false;
    }
    return true;
}

bool matches_twdn_five(const std::vector<std::uint32_t>& observed,
                       const RegisteredImage& image) noexcept
{
    if (observed.size() != 5U) return false;
    return observed[0] == make_jump_word(image.entry_read) &&
           observed[1] == kTrampolineWord &&
           observed[2] == make_jump_word(image.entry_write) &&
           observed[3] == kTrampolineWord &&
           observed[4] == kTailPatchWord;
}

bool read_words(Ep0Reader& reader, std::uint32_t base, std::size_t count,
                std::vector<std::uint32_t>& output,
                RecognitionResult& result, RecognitionStage stage) noexcept
{
    result.stage = stage;
    output.clear();
    output.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        const ReadResult read = reader.read32(base + static_cast<std::uint32_t>(index) * 4U);
        if (!read.ok) {
            result.classified = false;
            result.failure = read.transfer;
            return false;
        }
        output.push_back(read.value);
    }
    return true;
}

bool read_image(Ep0Reader& reader, const RegisteredImage& image,
                std::vector<std::uint8_t>& output,
                RecognitionResult& result) noexcept
{
    result.stage = RecognitionStage::image;
    output.clear();
    output.reserve(image.image_bytes);
    for (std::uint32_t offset = 0U; offset < image.image_bytes; offset += 4U) {
        const ReadResult read = reader.read32(image.bank_base + offset);
        if (!read.ok) {
            result.classified = false;
            result.failure = read.transfer;
            return false;
        }
        output.push_back(static_cast<std::uint8_t>(read.value >> 24U));
        output.push_back(static_cast<std::uint8_t>(read.value >> 16U));
        output.push_back(static_cast<std::uint8_t>(read.value >> 8U));
        output.push_back(static_cast<std::uint8_t>(read.value));
    }
    return true;
}

std::array<std::uint8_t, 32U> image_sha256(
    const std::vector<std::uint8_t>& bytes) noexcept
{
    dtv::userland::detail::Sha256 digest;
    digest.update(bytes.data(), bytes.size());
    return digest.finish();
}

Verify verify_legacy(Ep0Reader& reader, const RegisteredImage& image,
                     RecognitionResult& result) noexcept
{
    std::vector<std::uint8_t> bytes;
    if (!read_image(reader, image, bytes, result)) return Verify::read_failure;
    return image_sha256(bytes) == image.sha256 ? Verify::match : Verify::no_match;
}

Verify verify_twdn(Ep0Reader& reader, const RegisteredImage& image,
                   RecognitionResult& result) noexcept
{
    std::vector<std::uint32_t> header;
    if (!read_words(reader, image.bank_base, 8U, header, result,
                    RecognitionStage::header))
        return Verify::read_failure;
    if (header[0] != kTwdnMagic || header[1] != kTwdnAbi ||
        header[3] == 0U || (header[3] & 0x01U) == 0U ||
        header[4] != image.image_bytes || header[5] != image.entry_read ||
        header[6] != image.entry_write || header[7] != 0U)
        return Verify::no_match;
    std::vector<std::uint8_t> bytes;
    if (!read_image(reader, image, bytes, result)) return Verify::read_failure;
    if (bytes.size() != image.image_bytes) return Verify::no_match;
    return std::memcmp(bytes.data(), image.expected_image, image.image_bytes) == 0
               ? Verify::match
               : Verify::no_match;
}

}  // namespace

std::uint32_t make_jump_word(std::uint32_t entry) noexcept
{
    return ((0x9B00U | (((entry >> 16U) & 0x0FU) << 4U) | 0x0CU) << 16U) |
           (entry & 0xFFFFU);
}

const RegisteredImage* legacy_registry(std::size_t& count) noexcept
{
    // Contract v0.2.2 / torne_drv fc662902a3e4ad56566b00cae4a0ead922e3dd02
    // driver/torne_fw_gen.h torne_addressing0_code[85]. Only the hash, entries,
    // and five patch words are kept here; the image bytes stay in the GPL
    // reference source and in the read-only observation report.
    static const RegisteredImage kLegacyBank0{
        ImageFormat::legacy_coax,
        0x0E1000U,
        340U,
        0x0E1000U,
        0x0E1076U,
        {0x69U, 0x37U, 0x80U, 0x5EU, 0x74U, 0xBFU, 0x0EU, 0xCCU,
         0x8FU, 0x2AU, 0x9EU, 0x76U, 0x75U, 0x0DU, 0x4BU, 0xD8U,
         0x7EU, 0xD2U, 0xD5U, 0x3EU, 0x85U, 0xDBU, 0xFBU, 0x29U,
         0x37U, 0x6CU, 0x2BU, 0xC1U, 0x14U, 0x52U, 0x68U, 0xE7U},
        {{{0x0E9AC0U, 0x9BEC1000U},
          {0x0E9AC4U, 0x970C9FA0U},
          {0x0E9E0CU, 0x9BEC1076U},
          {0x0E9E10U, 0x970C9FA0U},
          {0x0E9D94U, 0x9FA09F8DU}}},
        nullptr,
    };
    count = 1U;
    return &kLegacyBank0;
}

RecognitionResult recognize_read_only(Ep0Reader& reader,
                                      const RegisteredImage* current,
                                      const RegisteredImage* known,
                                      std::size_t known_count) noexcept
{
    RecognitionResult result;
    std::vector<std::uint32_t> five;
    result.stage = RecognitionStage::five_words;
    five.reserve(kPatchAddresses.size());
    for (std::size_t index = 0U; index < kPatchAddresses.size(); ++index) {
        const ReadResult read = reader.read32(kPatchAddresses[index]);
        if (!read.ok) {
            result.classified = false;
            result.stage = RecognitionStage::five_words;
            result.failure = read.transfer;
            return result;
        }
        five.push_back(read.value);
    }
    for (std::size_t index = 0U; index < five.size(); ++index)
        result.five[index] = five[index];

    bool pristine = true;
    for (std::size_t index = 0U; index < five.size(); ++index) {
        if (five[index] != kPristineWords[index]) pristine = false;
    }
    if (pristine) {
        result.classified = true;
        result.state = WidenState::pristine;
        return result;
    }

    if (known_count != 0U && known == nullptr) {
        result.classified = true;
        result.state = WidenState::unknown;
        return result;
    }
    std::size_t matches = 0U;
    WidenState matched_state = WidenState::unknown;
    const RegisteredImage* matched_image = nullptr;
    if (current != nullptr && valid_twdn_record(*current) &&
        matches_twdn_five(five, *current)) {
        const Verify verified = verify_twdn(reader, *current, result);
        if (verified == Verify::read_failure) return result;
        if (verified == Verify::match) {
            ++matches;
            matched_state = WidenState::ours_active;
            matched_image = current;
        }
    }
    for (std::size_t index = 0U; index < known_count; ++index) {
        const RegisteredImage& image = known[index];
        if (image.format == ImageFormat::legacy_coax) {
            if (!valid_legacy_record(image) || !matches_legacy_five(five, image))
                continue;
            const Verify verified = verify_legacy(reader, image, result);
            if (verified == Verify::read_failure) return result;
            if (verified == Verify::match) {
                ++matches;
                matched_state = WidenState::foreign_known;
                matched_image = &image;
            }
        } else if (image.format == ImageFormat::twdn) {
            if (!valid_twdn_record(image) || !matches_twdn_five(five, image))
                continue;
            const Verify verified = verify_twdn(reader, image, result);
            if (verified == Verify::read_failure) return result;
            if (verified == Verify::match) {
                ++matches;
                matched_state = WidenState::foreign_known;
                matched_image = &image;
            }
        }
    }

    result.classified = true;
    result.state = matches == 1U ? matched_state : WidenState::unknown;
    result.matched = matches == 1U ? matched_image : nullptr;
    result.stage = RecognitionStage::image;
    result.failure = usb::TransferOutcome{};
    return result;
}

}  // namespace torne::widen
