// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_WIDEN_RECOGNITION_H
#define TORNE_WIDEN_RECOGNITION_H

#include "widen/bridge.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace torne::widen {

enum class ImageFormat : std::uint8_t {
    legacy_coax,
    twdn,
};

enum class WidenState : std::uint8_t {
    pristine,
    ours_active,
    foreign_known,
    // Reserved for a future switch context. Recognition never invents it from
    // a read-only observation.
    partial_ours,
    unknown,
};

enum class RecognitionStage : std::uint8_t {
    five_words,
    header,
    image,
};

struct PatchWord final {
    std::uint32_t address = 0U;
    std::uint32_t value = 0U;
};

// A registered identity record. Legacy records carry the full-image SHA-256 and
// patch words; TWDN records additionally need the expected image bytes injected
// by the future firmware build. Neither record stores a loadable firmware image
// in this increment.
struct RegisteredImage final {
    ImageFormat format = ImageFormat::legacy_coax;
    std::uint32_t bank_base = 0U;
    std::uint32_t image_bytes = 0U;
    std::uint32_t entry_read = 0U;
    std::uint32_t entry_write = 0U;
    std::array<std::uint8_t, 32U> sha256{};
    std::array<PatchWord, 5U> patches{};
    const std::uint8_t* expected_image = nullptr;
};

struct RecognitionResult final {
    bool classified = false;
    WidenState state = WidenState::unknown;
    RecognitionStage stage = RecognitionStage::five_words;
    usb::TransferOutcome failure{};
    // Introduction and identification are separate gates. This increment has
    // no write path, so every classification keeps this false.
    bool introduction_allowed = false;
    // The five words observed during this recognition. The loader rechecks
    // these exact words instead of selecting another registered image.
    std::array<std::uint32_t, 5U> five{};
    // Set only when exactly one full image matched. It points into the caller's
    // current or known record storage and remains valid only while that storage
    // lives.
    const RegisteredImage* matched = nullptr;
};

std::uint32_t make_jump_word(std::uint32_t entry) noexcept;

// The fixed legacy bank0 identity from the contract v0.2.2. It contains only
// the hash/entry/patch tuple, never the reference image itself.
const RegisteredImage* legacy_registry(std::size_t& count) noexcept;

// Reads five patch words, then only the registered candidate's fixed-length
// image. Content mismatch returns UNKNOWN; a read failure returns
// classified=false with the transfer failure and never UNKNOWN.
//
// current is the image for this build (TWDN only, null allowed). known is the
// explicit registry of legacy/other-build images. A current match is
// OURS_ACTIVE; a known match (including TWDN) is FOREIGN_KNOWN. Two or more
// total matches are UNKNOWN.
RecognitionResult recognize_read_only(Ep0Reader& reader,
                                      const RegisteredImage* current,
                                      const RegisteredImage* known,
                                      std::size_t known_count) noexcept;

}  // namespace torne::widen

#endif  // TORNE_WIDEN_RECOGNITION_H
