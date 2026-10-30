// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_CORE1_HOST_RUNTIME_H
#define TORNE_CORE1_HOST_RUNTIME_H

#include "usb/transfer.h"
#include "widen/bridge.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace torne::core1 {

// The image is injected explicitly; the host runtime does not depend on a
// generated C1-A header. bytes are the FR30 image in memory order, and sha256
// is the full-image hash used to reject a mismatched injection before I/O.
struct Image final {
    const std::uint8_t* bytes = nullptr;
    std::size_t image_bytes = 0U;
    std::uint32_t build_id = 0U;
    std::uint32_t entry = 0x000A0000U;
    std::uint32_t capabilities = 0x00000001U;
    std::array<std::uint8_t, 32U> sha256{};
};

enum class Stage : std::uint8_t {
    validate,
    hold,
    vector,
    zero_data,
    ctrl,
    code_write,
    code_readback,
    run,
    info,
    heartbeat,
    nop,
    done,
};

enum class Error : std::uint8_t {
    none,
    invalid_argument,
    widen_not_verified,
    write_failed,
    read_failed,
    readback_mismatch,
    outcome_unknown,
    run_outcome_unknown,
    info_timeout,
    abi_mismatch,
    build_id_mismatch,
    session_mismatch,
    image_mismatch,
    data_base_mismatch,
    capability_mismatch,
    heartbeat_stalled,
    nop_timeout,
    nop_failed,
    device_gone,
};

struct Result final {
    bool ok = false;
    Stage stage = Stage::validate;
    Error error = Error::none;
    usb::TransferOutcome failure{};
    std::uint32_t session = 0U;
};

class Clock {
public:
    virtual ~Clock() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

// The next session has a nonzero high 16 bits and differs from previous.
std::uint32_t next_session(std::uint32_t previous) noexcept;

// C1-B host side. The caller must have completed Widen verification (S2) and
// must own the single EP0 path. The runtime never opens USB, never retries
// itself, and uses the Transport's per-kind policy for every transfer.
class Runtime final {
public:
    explicit Runtime(std::uint32_t previous_session = 0U) noexcept
        : last_session_(previous_session)
    {
    }

    Result start(widen::Ep0Io& io, const Image& image, bool widen_verified,
                 Clock& clock) noexcept;
    Result stop(widen::Ep0Io& io) noexcept;
    std::uint32_t last_session() const noexcept { return last_session_; }

private:
    std::uint32_t last_session_ = 0U;
};

}  // namespace torne::core1

#endif  // TORNE_CORE1_HOST_RUNTIME_H
