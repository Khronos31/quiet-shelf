// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_WIDEN_LOADER_H
#define TORNE_WIDEN_LOADER_H

#include "widen/recognition.h"

#include <cstddef>
#include <cstdint>

namespace torne::widen {

enum class LoaderStage : std::uint8_t {
    recognition,
    recheck,
    image_write,
    image_readback,
    patch_write,
    patch_readback,
    functional_check,
    done,
};

enum class LoaderError : std::uint8_t {
    none,
    invalid_argument,
    read_failed,
    unknown_state,
    partial_state,
    introduction_not_allowed,
    missing_image,
    state_changed,
    write_failed,
    outcome_unknown,
    device_gone,
    readback_mismatch,
    image_verified_stop,
    functional_check_not_allowed,
    functional_check_failed,
};

// The caller must set these only after an explicit introduction decision.
// Defaults are deliberately closed: recognition never becomes a write permit.
struct IntroductionPolicy final {
    bool allow_bank_write = false;
    bool allow_functional_check = false;
    // Explicit staging break after the image readback, before any patch word.
    bool stop_after_image_readback = false;
    std::uint32_t core0_hold_address = 0x01600000U;
    std::uint32_t core1_hold_address = 0x01700000U;
    std::uint32_t hold_value = 0xFFFFFFFFU;
    std::uint32_t functional_probe_address = 0x017A3FC0U;
    std::uint16_t functional_probe_length = 64U;
    std::uint32_t legacy_probe_address = 0x0F1010CU;
    std::uint8_t functional_pattern_seed = 0xA5U;
};

struct LoaderResult final {
    bool ok = false;
    WidenState observed = WidenState::unknown;
    std::uint32_t target_bank_base = 0U;
    LoaderStage stage = LoaderStage::recognition;
    LoaderError error = LoaderError::none;
    usb::TransferOutcome failure{};
    bool wrote_any = false;
    bool image_written = false;
    bool patches_applied = false;
    bool functional_checked = false;
    bool legacy_24bit_checked = false;
    std::size_t image_words_written = 0U;
    std::size_t patch_words_written = 0U;
};

// Runs the contract v0.2.2 §6.3 transaction over one EP0 owner. It never opens
// USB, retries itself, or permits writes unless the caller's policy allows
// them. images contains the TWDN candidate for each bank; current is the one
// current image passed to recognition and known is the explicit registration
// list. A missing target image or an incomplete/unknown observation stops
// before any write.
LoaderResult introduce_widen(Ep0Io& io, const RegisteredImage* current,
                             const RegisteredImage* known,
                             std::size_t known_count,
                             const RegisteredImage* images,
                             std::size_t image_count,
                             const IntroductionPolicy& policy = {}) noexcept;

}  // namespace torne::widen

#endif  // TORNE_WIDEN_LOADER_H
