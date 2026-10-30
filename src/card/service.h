// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_CARD_SERVICE_H
#define TORNE_CARD_SERVICE_H

#include "card/uart.h"

#include <dtv/userland/host_card_t1/card_t1.h>
#include <dtv/userland/host_daemon/card_service.h>

namespace torne::card {

// The formal bridge from the product card line to the publicly adopted Core
// CardHardware/CardTime seam. No ATR/T=1 parser or retry policy is added here.
class CoreTime final : public dtv::userland::host_card_t1::CardTime {
public:
    explicit CoreTime(Clock* clock) noexcept : clock_(clock) {}
    std::uint64_t monotonic_ms() noexcept override
    {
        return clock_ != nullptr ? clock_->monotonic_ms() : 0U;
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        if (clock_ != nullptr) clock_->sleep_ms(milliseconds);
    }

private:
    Clock* clock_;
};

class Hardware final : public dtv::userland::host_card_t1::CardHardware {
public:
    explicit Hardware(Line* line) noexcept : line_(line) {}

    dtv::userland::Result<bool> detect_card() noexcept override;
    dtv::userland::Result<void> reset_card(
        dtv::userland::host_card_t1::CardDelay& delay) noexcept override;
    dtv::userland::Result<bool> data_ready() noexcept override;
    dtv::userland::Result<std::size_t> read_data(
        dtv::userland::MutableByteView output) noexcept override;
    dtv::userland::Result<void> write_data(
        dtv::userland::ByteView input) noexcept override;
    dtv::userland::Result<void> set_baud_rate(
        dtv::userland::host_card_t1::CardBaudRate baud_rate) noexcept override;

private:
    Line* line_;
};

class Backend final : public px4::userland::CardServiceBackend {
public:
    explicit Backend(Line* line) noexcept : line_(line) {}

    dtv::userland::Result<void> set_power(bool on) noexcept override;
    dtv::userland::Result<void> initialize_uart() noexcept override;
    dtv::userland::Result<bool> detect_card() noexcept override;
    void request_stop() noexcept override;

private:
    Line* line_;
};

}  // namespace torne::card

#endif  // TORNE_CARD_SERVICE_H
