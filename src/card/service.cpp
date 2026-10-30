// SPDX-License-Identifier: GPL-2.0-only
#include "card/service.h"

namespace torne::card {
namespace {

dtv::userland::Error map_error(const card::Result& result) noexcept
{
    switch (result.error) {
    case card::Error::none:
        return dtv::userland::Error::INTERNAL;
    case card::Error::invalid_argument:
        return dtv::userland::Error::INVALID_ARGUMENT;
    case card::Error::session_invalid:
        return dtv::userland::Error::NOT_READY;
    case card::Error::no_card:
        return dtv::userland::Error::NO_CARD;
    case card::Error::unsupported_baud:
        return dtv::userland::Error::UNSUPPORTED;
    case card::Error::short_transfer:
    case card::Error::outcome_unknown:
    case card::Error::io_failed:
        return result.transfer.cause == usb::TransferCause::no_device
                   ? dtv::userland::Error::DISCONNECTED
                   : dtv::userland::Error::USB_IO;
    }
    return dtv::userland::Error::INTERNAL;
}

class DelayClock final : public Clock {
public:
    explicit DelayClock(
        dtv::userland::host_card_t1::CardDelay& delay) noexcept
        : delay_(delay) {}
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        delay_.sleep_ms(milliseconds);
    }

private:
    dtv::userland::host_card_t1::CardDelay& delay_;
};

std::uint32_t baud_value(
    dtv::userland::host_card_t1::CardBaudRate baud_rate) noexcept
{
    using dtv::userland::host_card_t1::CardBaudRate;
    switch (baud_rate) {
    case CardBaudRate::baud_9600: return 9600U;
    case CardBaudRate::baud_19200: return 19200U;
    case CardBaudRate::baud_38400: return 38400U;
    }
    return 0U;
}

}  // namespace

dtv::userland::Result<bool> Hardware::detect_card() noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<bool>::failure(dtv::userland::Error::UNSUPPORTED);
    const PresenceResult detected = line_->detect_card();
    if (!detected.result.ok)
        return dtv::userland::Result<bool>::failure(map_error(detected.result));
    return dtv::userland::Result<bool>::success(detected.present);
}

dtv::userland::Result<void> Hardware::reset_card(
    dtv::userland::host_card_t1::CardDelay& delay) noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<void>::failure(dtv::userland::Error::UNSUPPORTED);
    DelayClock clock(delay);
    const card::Result reset = line_->reset_card(clock);
    return reset.ok ? dtv::userland::Result<void>::success()
                    : dtv::userland::Result<void>::failure(map_error(reset));
}

dtv::userland::Result<bool> Hardware::data_ready() noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<bool>::failure(dtv::userland::Error::UNSUPPORTED);
    const CountResult ready = line_->data_ready();
    if (!ready.result.ok)
        return dtv::userland::Result<bool>::failure(map_error(ready.result));
    return dtv::userland::Result<bool>::success(ready.count != 0U);
}

dtv::userland::Result<std::size_t> Hardware::read_data(
    dtv::userland::MutableByteView output) noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<std::size_t>::failure(dtv::userland::Error::UNSUPPORTED);
    const ReadResult read = line_->read_data(output.data, output.size);
    if (!read.result.ok)
        return dtv::userland::Result<std::size_t>::failure(map_error(read.result));
    if (read.bytes_read > output.size)
        return dtv::userland::Result<std::size_t>::failure(dtv::userland::Error::PROTOCOL_ERROR);
    return dtv::userland::Result<std::size_t>::success(read.bytes_read);
}

dtv::userland::Result<void> Hardware::write_data(dtv::userland::ByteView input) noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<void>::failure(dtv::userland::Error::UNSUPPORTED);
    const card::Result written = line_->write_data(input.data, input.size);
    return written.ok ? dtv::userland::Result<void>::success()
                      : dtv::userland::Result<void>::failure(map_error(written));
}

dtv::userland::Result<void> Hardware::set_baud_rate(
    dtv::userland::host_card_t1::CardBaudRate baud_rate) noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<void>::failure(dtv::userland::Error::UNSUPPORTED);
    const std::uint32_t rate = baud_value(baud_rate);
    if (rate == 0U) return dtv::userland::Result<void>::failure(dtv::userland::Error::UNSUPPORTED);
    const card::Result baud = line_->set_baud_rate(rate);
    return baud.ok ? dtv::userland::Result<void>::success()
                   : dtv::userland::Result<void>::failure(map_error(baud));
}

dtv::userland::Result<void> Backend::set_power(bool on) noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<void>::failure(dtv::userland::Error::UNSUPPORTED);
    if (on) return dtv::userland::Result<void>::success();
    const card::Result off = line_->power_down();
    return off.ok ? dtv::userland::Result<void>::success()
                  : dtv::userland::Result<void>::failure(map_error(off));
}

dtv::userland::Result<void> Backend::initialize_uart() noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<void>::failure(dtv::userland::Error::UNSUPPORTED);
    return dtv::userland::Result<void>::success();
}

dtv::userland::Result<bool> Backend::detect_card() noexcept
{
    if (line_ == nullptr) return dtv::userland::Result<bool>::failure(dtv::userland::Error::UNSUPPORTED);
    const PresenceResult detected = line_->detect_card();
    if (!detected.result.ok)
        return dtv::userland::Result<bool>::failure(map_error(detected.result));
    return dtv::userland::Result<bool>::success(detected.present);
}

void Backend::request_stop() noexcept
{
    if (line_ != nullptr) line_->invalidate();
}

}  // namespace torne::card
