// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_CORE0_HOST_RUNTIME_H
#define TORNE_CORE0_HOST_RUNTIME_H

#include "core0/ts_pump.h"
#include "usb/transport.h"
#include "widen/bridge.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace torne::core0 {

struct Image final {
    const std::uint8_t* bytes = nullptr;
    std::size_t image_bytes = 0U;
    std::uint32_t build_id = 0U;
    std::uint32_t entry = 0x000A0000U;
    std::uint32_t capabilities = 0x00000001U;
    std::uint32_t param0 = 0x00000840U;
    std::array<std::uint8_t, 32U> sha256{};
};

class I2cFlush {
public:
    virtual ~I2cFlush() noexcept = default;
    virtual bool flush() noexcept = 0;
};

enum class Stage : std::uint8_t {
    validate,
    i2c_flush,
    engine_setup,
    hold,
    vector,
    zero_data,
    ctrl,
    code_write,
    code_readback,
    usb_setup,
    run,
    info,
    stat,
    bulk_start,
    input_open,
    done,
    hold_stop,
    input_stop,
    bulk_stop,
};

enum class Error : std::uint8_t {
    none,
    invalid_argument,
    widen_not_verified,
    i2c_flush_failed,
    write_failed,
    read_failed,
    outcome_unknown,
    readback_mismatch,
    run_outcome_unknown,
    info_timeout,
    abi_mismatch,
    build_id_mismatch,
    session_mismatch,
    image_mismatch,
    data_base_mismatch,
    capability_mismatch,
    state_mismatch,
    epoch_mismatch,
    heartbeat_stalled,
    bulk_start_failed,
    bulk_stop_failed,
    device_gone,
};

struct StartOptions final {
    bool open_input = true;
};

struct Result final {
    bool ok = false;
    Stage stage = Stage::validate;
    Error error = Error::none;
    usb::TransferOutcome failure{};
    std::uint32_t session = 0U;
    std::uint32_t epoch = 0U;
    bool input_open = false;
};

std::uint32_t next_session(std::uint32_t previous) noexcept;

class Runtime final {
public:
    explicit Runtime(std::uint32_t previous_session = 0U) noexcept
        : last_session_(previous_session)
    {
    }

    Result start(widen::Ep0Io& io, usb::Transport& transport,
                 const Image& image, std::uint32_t epoch,
                 bool widen_verified, I2cFlush& flush, Clock& clock,
                 TsPump& pump, const StartOptions& options = {}) noexcept;
    Result stop(widen::Ep0Io& io, TsPump& pump) noexcept;
    std::uint32_t last_session() const noexcept { return last_session_; }

private:
    std::uint32_t last_session_ = 0U;
};

}  // namespace torne::core0

#endif  // TORNE_CORE0_HOST_RUNTIME_H
