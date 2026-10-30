// SPDX-License-Identifier: GPL-2.0-only
#include "card/uart.h"

#include <array>

namespace torne::card {
namespace {

constexpr std::uint32_t card_enable = 0x00FEA000U;
constexpr std::uint32_t card_lines = 0x00FEA004U;
constexpr std::uint32_t card_baud = 0x00FEA008U;
constexpr std::uint32_t card_send = 0x00FEA00CU;
constexpr std::uint32_t card_control = 0x00FEA010U;
constexpr std::uint32_t card_fifo = 0x00FEA014U;
constexpr std::uint32_t card_irq_mask = 0x00FEA018U;
constexpr std::uint32_t card_irq = 0x00FEA01CU;
constexpr std::uint32_t card_cwt = 0x00FEA020U;
constexpr std::uint32_t card_bwt = 0x00FEA024U;
constexpr std::uint32_t card_tx = 0x00FEA028U;
constexpr std::uint32_t card_rx = 0x00FEA02CU;
constexpr std::uint32_t card_power = 0x00FEA034U;
constexpr std::uint32_t card_sent_lrc = 0x00FEA038U;
constexpr std::uint32_t card_recv_lrc = 0x00FEA03CU;

constexpr std::uint32_t card_no_card = 1U << 10U;
constexpr std::uint32_t reset_baud = 0x0000BA7FU;
constexpr std::uint32_t reset_cwt = 9600U;
constexpr std::uint32_t reset_recv_lrc = 0x3BU;
constexpr std::uint32_t reset_irq_mask = 0x1FFU;
constexpr std::uint32_t send_command = 0x200U;
constexpr std::uint32_t send_command_long = 0x280U;
constexpr std::uint32_t control_timeout_ms = 1000U;

std::uint16_t address_value(std::uint32_t address) noexcept
{
    return static_cast<std::uint16_t>(((address >> 8U) & 0xFF00U) |
                                      ((address >> 24U) & 0xFFU));
}

std::uint16_t address_index(std::uint32_t address) noexcept
{
    return static_cast<std::uint16_t>(address & 0xFFFFU);
}

bool valid_address(std::uint32_t address) noexcept
{
    return address < 0x01000000U && (address & 0x03U) == 0U;
}

std::array<std::uint8_t, 4U> encode_be32(std::uint32_t value) noexcept
{
    return {static_cast<std::uint8_t>(value >> 24U),
            static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value)};
}

std::uint32_t decode_be32(const std::array<std::uint8_t, 4U>& bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

Result make_result(Step step, const usb::TransferOutcome& transfer) noexcept
{
    Result result;
    result.ok = true;
    result.step = step;
    result.transfer = transfer;
    return result;
}

Result make_error(Step step, Error error,
                  const usb::TransferOutcome& transfer) noexcept
{
    Result result;
    result.step = step;
    result.error = error;
    result.transfer = transfer;
    return result;
}

}  // namespace

Uart::Uart(usb::Transport& transport, Clock& clock) noexcept
    : transport_(transport), clock_(clock)
{
}

Result Uart::read(std::uint32_t address, usb::ControlKind kind, Step step,
                  std::uint32_t& value) noexcept
{
    if (!valid_)
        return make_error(step, Error::session_invalid, {});
    if (!valid_address(address))
        return make_error(step, Error::invalid_argument, {});
    std::array<std::uint8_t, 4U> raw{};
    const usb::TransferOutcome transfer = transport_.control_in(
        0xC0U, 0xBCU, address_value(address), address_index(address),
        static_cast<std::uint16_t>(raw.size()), raw.data(), control_timeout_ms,
        kind);
    if (transfer.state == usb::TransferState::complete &&
        transfer.transferred == raw.size()) {
        value = decode_be32(raw);
        return make_result(step, transfer);
    }
    valid_ = false;
    if (transfer.state == usb::TransferState::outcome_unknown)
        return make_error(step, Error::outcome_unknown, transfer);
    if (transfer.state == usb::TransferState::short_transfer)
        return make_error(step, Error::short_transfer, transfer);
    return make_error(step, Error::io_failed, transfer);
}

Result Uart::write(std::uint32_t address, std::uint32_t value,
                   usb::ControlKind kind, Step step) noexcept
{
    if (!valid_)
        return make_error(step, Error::session_invalid, {});
    if (!valid_address(address))
        return make_error(step, Error::invalid_argument, {});
    const std::array<std::uint8_t, 4U> raw = encode_be32(value);
    const usb::TransferOutcome transfer = transport_.control_out(
        0x40U, 0xBCU, address_value(address), address_index(address),
        static_cast<std::uint16_t>(raw.size()), raw.data(), control_timeout_ms,
        kind);
    if (transfer.state == usb::TransferState::complete)
        return make_result(step, transfer);
    valid_ = false;
    if (transfer.state == usb::TransferState::outcome_unknown)
        return make_error(step, Error::outcome_unknown, transfer);
    if (transfer.state == usb::TransferState::short_transfer)
        return make_error(step, Error::short_transfer, transfer);
    return make_error(step, Error::io_failed, transfer);
}

Result Uart::read_modify(std::uint32_t address, std::uint32_t mask, bool set,
                         Step step) noexcept
{
    std::uint32_t value = 0U;
    const Result read_result = read(address, usb::ControlKind::pure_read, step,
                                    value);
    if (!read_result.ok)
        return read_result;
    const std::uint32_t next = set ? (value | mask) : (value & ~mask);
    return write(address, next, usb::ControlKind::abs_write, step);
}

PresenceResult Uart::detect_card() noexcept
{
    PresenceResult result;
    std::uint32_t irq = 0U;
    result.result = read(card_irq, usb::ControlKind::pure_read, Step::presence,
                         irq);
    if (result.result.ok)
        result.present = (irq & card_no_card) == 0U;
    return result;
}

Result Uart::power_down() noexcept
{
    Result result = read_modify(card_lines, 1U << 2U, false,
                                Step::power_lines_reset);
    if (!result.ok) return result;
    result = write(card_fifo, 0U, usb::ControlKind::trigger, Step::power_fifo);
    if (!result.ok) return result;
    result = read_modify(card_enable, 1U << 2U, false, Step::power_enable);
    if (!result.ok) return result;
    result = read_modify(card_lines, 0x0AU, false, Step::power_lines);
    if (!result.ok) return result;
    result = write(card_irq_mask, 1U, usb::ControlKind::abs_write,
                   Step::power_irq_mask);
    if (!result.ok) return result;
    return write(card_power, 0U, usb::ControlKind::abs_write, Step::power);
}

Result Uart::reset_card() noexcept
{
    const PresenceResult presence = detect_card();
    if (!presence.result.ok)
        return presence.result;
    if (!presence.present)
        return make_error(Step::presence, Error::no_card, presence.result.transfer);

    Result result = power_down();
    if (!result.ok) return result;

    clock_.sleep_ms(10U);
    result = write(card_irq, 0x3FFU, usb::ControlKind::trigger,
                   Step::reset_irq_clear);
    if (!result.ok) return result;
    result = write(card_power, 1U, usb::ControlKind::abs_write,
                   Step::reset_power);
    if (!result.ok) return result;
    result = read_modify(card_lines, 1U << 2U, false,
                         Step::reset_lines_reset);
    if (!result.ok) return result;
    result = write(card_fifo, 0U, usb::ControlKind::trigger, Step::reset_fifo);
    if (!result.ok) return result;
    result = write(card_enable, 0U, usb::ControlKind::abs_write,
                   Step::reset_enable_zero);
    if (!result.ok) return result;
    result = write(card_enable, 7U, usb::ControlKind::abs_write,
                   Step::reset_enable_seven);
    if (!result.ok) return result;
    result = read_modify(card_lines, 1U << 3U, true, Step::reset_lines_vcc);
    if (!result.ok) return result;
    result = write(card_baud, reset_baud, usb::ControlKind::abs_write,
                   Step::reset_baud);
    if (!result.ok) return result;

    clock_.sleep_ms(2U);
    result = read_modify(card_lines, 1U << 1U, true, Step::reset_lines_clock);
    if (!result.ok) return result;
    result = write(card_recv_lrc, reset_recv_lrc, usb::ControlKind::abs_write,
                   Step::reset_recv_lrc);
    if (!result.ok) return result;
    result = write(card_cwt, reset_cwt, usb::ControlKind::abs_write,
                   Step::reset_cwt);
    if (!result.ok) return result;
    result = write(card_bwt, 0U, usb::ControlKind::abs_write,
                   Step::reset_bwt);
    if (!result.ok) return result;
    result = write(card_irq_mask, reset_irq_mask, usb::ControlKind::abs_write,
                   Step::reset_irq_mask);
    if (!result.ok) return result;

    clock_.sleep_ms(1U);
    return read_modify(card_lines, 1U << 2U, true, Step::reset_lines_release);
}

CountResult Uart::data_ready() noexcept
{
    CountResult result;
    std::uint32_t fifo = 0U;
    result.result = read(card_fifo, usb::ControlKind::pure_read,
                         Step::fifo_count, fifo);
    if (result.result.ok)
        result.count = fifo >> 16U;
    return result;
}

ReadResult Uart::read_data(std::uint8_t* buffer, std::size_t length) noexcept
{
    ReadResult result;
    if (!valid_) {
        result.result = make_error(Step::read_rx, Error::session_invalid, {});
        return result;
    }
    if (length != 0U && buffer == nullptr) {
        result.result = make_error(Step::read_rx, Error::invalid_argument, {});
        return result;
    }
    const CountResult ready = data_ready();
    if (!ready.result.ok) {
        result.result = ready.result;
        return result;
    }
    const std::size_t count =
        ready.count < length ? ready.count : length;
    usb::TransferOutcome last = ready.result.transfer;
    for (std::size_t i = 0U; i < count; ++i) {
        std::uint32_t value = 0U;
        const Result read_result = read(card_rx,
                                        usb::ControlKind::consuming_read,
                                        Step::read_rx, value);
        if (!read_result.ok) {
            result.result = read_result;
            return result;
        }
        last = read_result.transfer;
        buffer[i] = static_cast<std::uint8_t>(value);
        result.bytes_read = i + 1U;
    }
    result.result = make_result(Step::read_rx, last);
    return result;
}

Result Uart::write_data(const std::uint8_t* data, std::size_t length) noexcept
{
    if (!valid_)
        return make_error(Step::write_tx, Error::session_invalid, {});
    if (length == 0U || data == nullptr)
        return make_error(Step::write_tx, Error::invalid_argument, {});

    Result result = write(card_recv_lrc, 0U, usb::ControlKind::abs_write,
                          Step::write_lrc);
    if (!result.ok) return result;
    result = write(card_sent_lrc, 0U, usb::ControlKind::abs_write,
                   Step::write_lrc);
    if (!result.ok) return result;
    for (std::size_t i = 0U; i < length; ++i) {
        result = write(card_tx, data[i], usb::ControlKind::fifo_write,
                       Step::write_tx);
        if (!result.ok) return result;
    }
    result = write(card_irq, 0x3FFU, usb::ControlKind::trigger,
                   Step::write_irq);
    if (!result.ok) return result;
    const std::uint32_t command =
        length >= 0x100U ? send_command_long : send_command;
    result = write(card_control, command, usb::ControlKind::abs_write,
                   Step::write_control);
    if (!result.ok) return result;
    return write(card_send, 1U, usb::ControlKind::trigger, Step::send);
}

Result Uart::set_baud_rate(std::uint32_t rate) noexcept
{
    if (!valid_)
        return make_error(Step::set_baud, Error::session_invalid, {});
    std::uint32_t baud = 0U;
    std::uint32_t cwt = 0U;
    std::uint32_t bwt = 0U;
    std::uint32_t control = 0U;
    switch (rate) {
    case 9600U:
        baud = 0xBA7FU;
        cwt = 9600U;
        bwt = 0U;
        control = 0U;
        break;
    case 19200U:
        baud = 0x5D5DU;
        cwt = 107U;
        bwt = 36559U;
        control = 1U << 16U;
        break;
    case 38400U:
        baud = 0x2EAEU;
        cwt = 107U;
        bwt = 0xFFFFU;
        control = 1U << 16U;
        break;
    default:
        return make_error(Step::set_baud, Error::unsupported_baud, {});
    }

    Result result = write(card_baud, baud, usb::ControlKind::abs_write,
                          Step::set_baud);
    if (!result.ok) return result;
    result = write(card_control, control, usb::ControlKind::abs_write,
                   Step::set_baud);
    if (!result.ok) return result;
    result = write(card_cwt, cwt, usb::ControlKind::abs_write,
                   Step::set_baud);
    if (!result.ok) return result;
    return write(card_bwt, bwt, usb::ControlKind::abs_write, Step::set_baud);
}

}  // namespace torne::card
