// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_USB_TRANSPORT_H
#define TORNE_USB_TRANSPORT_H

#include "usb/transfer.h"

#include <dtv/userland/identity.h>

#include <memory>

namespace torne::usb {

bool location_from_selector(const dtv::userland::identity::Selector& selector,
                            DeviceLocation& location) noexcept;

class Transport;

struct OpenResult final {
    OpenOutcome outcome;
    std::unique_ptr<Transport> transport;
};

// One owned CECH-ZD1 USB interface. open() finds the exact bus/port path,
// verifies the fixed interface/endpoint descriptors, then claims interface 0.
// It never detaches a kernel driver, resets the device, or changes the active
// configuration. Close/reopen is represented by destroying and opening again.
class Transport final {
public:
    static OpenResult open(std::unique_ptr<UsbIo> io,
                           const DeviceLocation& location) noexcept;

    ~Transport() noexcept;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    TransferOutcome control_in(std::uint8_t request_type,
                               std::uint8_t request,
                               std::uint16_t value,
                               std::uint16_t index,
                               std::uint16_t length,
                               std::uint8_t* data,
                               std::uint32_t timeout_ms,
                               ControlKind kind) noexcept;
    TransferOutcome control_out(std::uint8_t request_type,
                                std::uint8_t request,
                                std::uint16_t value,
                                std::uint16_t index,
                                std::uint16_t length,
                                const std::uint8_t* data,
                                std::uint32_t timeout_ms,
                                ControlKind kind) noexcept;
    TransferOutcome bulk_in(std::uint8_t endpoint,
                            std::uint8_t* data,
                            std::size_t length,
                            std::uint32_t timeout_ms) noexcept;
    TransferOutcome bulk_out(std::uint8_t endpoint,
                             const std::uint8_t* data,
                             std::size_t length,
                             std::uint32_t timeout_ms) noexcept;

    void close() noexcept;
    bool claimed() const noexcept { return claimed_; }
    const DeviceSummary& summary() const noexcept { return summary_; }

private:
    Transport(std::unique_ptr<UsbIo> io, DeviceSummary summary) noexcept;

    bool endpoint_supported(std::uint8_t endpoint, std::uint8_t attributes) const noexcept;
    TransferOutcome control(std::uint8_t request_type, std::uint8_t request,
                            std::uint16_t value, std::uint16_t index,
                            std::uint16_t length, std::uint8_t* in_data,
                            const std::uint8_t* out_data,
                            std::uint32_t timeout_ms, ControlKind kind) noexcept;

    std::unique_ptr<UsbIo> io_;
    DeviceSummary summary_{};
    bool claimed_ = false;
};

}  // namespace torne::usb

#endif  // TORNE_USB_TRANSPORT_H
