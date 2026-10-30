// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_CARD_UART_H
#define TORNE_CARD_UART_H

#include "usb/transport.h"

#include <cstddef>
#include <cstdint>

namespace torne::card {

// The CECH-ZD1 J card slot is a smart-card UART in the MB86H47A. This type
// owns only the line/register operation. ATR syntax, T=1 and APDU handling
// belong to the shared Core when its final pin and RESULT_UNKNOWN boundary are
// settled.
enum class Step : std::uint8_t {
    none,
    presence,
    power_lines_reset,
    power_fifo,
    power_enable,
    power_lines,
    power_irq_mask,
    power,
    reset_sleep_10,
    reset_irq_clear,
    reset_power,
    reset_lines_reset,
    reset_fifo,
    reset_enable_zero,
    reset_enable_seven,
    reset_lines_vcc,
    reset_baud,
    reset_sleep_2,
    reset_lines_clock,
    reset_recv_lrc,
    reset_cwt,
    reset_bwt,
    reset_irq_mask,
    reset_sleep_1,
    reset_lines_release,
    fifo_count,
    read_rx,
    write_lrc,
    write_tx,
    write_irq,
    write_control,
    send,
    set_baud,
};

enum class Error : std::uint8_t {
    none,
    invalid_argument,
    session_invalid,
    no_card,
    unsupported_baud,
    short_transfer,
    io_failed,
    outcome_unknown,
};

struct Result final {
    bool ok = false;
    Step step = Step::none;
    Error error = Error::none;
    usb::TransferOutcome transfer{};
};

struct PresenceResult final {
    Result result{};
    bool present = false;
};

struct CountResult final {
    Result result{};
    std::uint32_t count = 0U;
};

struct ReadResult final {
    Result result{};
    std::size_t bytes_read = 0U;
};

// Injected time boundary. The line-reset waits are contract candidates taken
// from the fixed source; the upper layer owns the clock, not this class.
class Clock {
public:
    virtual ~Clock() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

// One card UART on the existing single-owner Transport. It never opens,
// claims or resets USB, and it never retries by itself: Transport owns the
// per-ControlKind retry policy.
class Uart final {
public:
    Uart(usb::Transport& transport, Clock& clock) noexcept;
    Uart(const Uart&) = delete;
    Uart& operator=(const Uart&) = delete;

    bool valid() const noexcept { return valid_; }
    void invalidate() noexcept { valid_ = false; }

    PresenceResult detect_card() noexcept;
    Result reset_card() noexcept;
    Result reset_card(Clock& clock) noexcept;
    Result power_down() noexcept;
    CountResult data_ready() noexcept;
    ReadResult read_data(std::uint8_t* buffer, std::size_t length) noexcept;
    Result write_data(const std::uint8_t* data, std::size_t length) noexcept;
    Result set_baud_rate(std::uint32_t rate) noexcept;

private:
    Result read(std::uint32_t address, usb::ControlKind kind,
                Step step, std::uint32_t& value) noexcept;
    Result write(std::uint32_t address, std::uint32_t value,
                 usb::ControlKind kind, Step step) noexcept;
    Result read_modify(std::uint32_t address, std::uint32_t mask, bool set,
                       Step step) noexcept;

    usb::Transport& transport_;
    Clock& clock_;
    bool valid_ = true;
};

// Narrow electrical seam used by the formal Core CardHardware adapter. The
// formal path takes the Core clock for the reset waits; tests can substitute
// this interface without emulating EP0 registers.
class Line {
public:
    virtual ~Line() noexcept = default;
    virtual PresenceResult detect_card() noexcept = 0;
    virtual Result reset_card(Clock& clock) noexcept = 0;
    virtual Result power_down() noexcept = 0;
    virtual CountResult data_ready() noexcept = 0;
    virtual ReadResult read_data(std::uint8_t* buffer,
                                 std::size_t length) noexcept = 0;
    virtual Result write_data(const std::uint8_t* data,
                              std::size_t length) noexcept = 0;
    virtual Result set_baud_rate(std::uint32_t rate) noexcept = 0;
    virtual void invalidate() noexcept = 0;
    virtual bool valid() const noexcept = 0;
};

class UartLine final : public Line {
public:
    explicit UartLine(Uart& uart) noexcept : uart_(uart) {}
    PresenceResult detect_card() noexcept override { return uart_.detect_card(); }
    Result reset_card(Clock& clock) noexcept override
    {
        return uart_.reset_card(clock);
    }
    Result power_down() noexcept override { return uart_.power_down(); }
    CountResult data_ready() noexcept override { return uart_.data_ready(); }
    ReadResult read_data(std::uint8_t* buffer,
                         std::size_t length) noexcept override
    {
        return uart_.read_data(buffer, length);
    }
    Result write_data(const std::uint8_t* data,
                      std::size_t length) noexcept override
    {
        return uart_.write_data(data, length);
    }
    Result set_baud_rate(std::uint32_t rate) noexcept override
    {
        return uart_.set_baud_rate(rate);
    }
    void invalidate() noexcept override { uart_.invalidate(); }
    bool valid() const noexcept override { return uart_.valid(); }

private:
    Uart& uart_;
};

}  // namespace torne::card

#endif  // TORNE_CARD_UART_H
