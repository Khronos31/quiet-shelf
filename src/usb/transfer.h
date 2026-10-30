// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_USB_TRANSFER_H
#define TORNE_USB_TRANSFER_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace torne::usb {

// OS-independent transport contract. The native libusb implementation and the
// tests speak these types; no Linux fd/ioctl, IOKit or WinUSB detail leaks here.

struct DeviceLocation final {
    std::uint8_t bus = 0U;
    std::array<std::uint8_t, 7U> ports{};
    std::uint8_t port_count = 0U;
};

enum class TransferCause : std::uint8_t {
    none,
    timeout,
    pipe,
    no_device,
    overflow,
    io,
    unsupported,
    access,
    busy,
    no_memory,
    invalid,
    other,
};

enum class TransferState : std::uint8_t {
    complete,
    short_transfer,
    failed,
    outcome_unknown,
};

struct TransferOutcome final {
    TransferState state = TransferState::failed;
    TransferCause cause = TransferCause::other;
    // Bulk: transferred is libusb's actual_length even when state is failed.
    // Control: libusb exposes no byte count on an error, so failed implies 0.
    std::size_t transferred = 0U;
    bool retried = false;
};

enum class OpenState : std::uint8_t {
    ok,
    not_found,
    busy,
    access,
    unsupported,
    disconnected,
    io,
    invalid,
};

struct OpenOutcome final {
    OpenState state = OpenState::io;
    TransferCause cause = TransferCause::io;
};

// The FR30 contract classifies each EP0 operation by side effect. The raw
// transport knows nothing about register meaning, so the caller must supply the
// kind; unclassified requests are refused rather than retried heuristically.
enum class ControlKind : std::uint8_t {
    pure_read,
    abs_write,
    consuming_read,
    fifo_write,
    trigger,
    unclassified,
};

struct EndpointInfo final {
    std::uint8_t address = 0U;
    std::uint8_t attributes = 0U;
    std::uint16_t max_packet = 0U;
    std::uint8_t interval = 0U;
};

struct InterfaceInfo final {
    std::uint8_t number = 0U;
    std::uint8_t alternate = 0U;
    std::uint8_t interface_class = 0U;
    std::uint8_t interface_subclass = 0U;
    std::uint8_t interface_protocol = 0U;
    std::uint8_t endpoint_count = 0U;
    std::array<EndpointInfo, 8U> endpoints{};
};

struct DeviceSummary final {
    std::uint16_t vid = 0U;
    std::uint16_t pid = 0U;
    std::uint8_t num_configurations = 0U;
    std::uint8_t configuration_value = 0U;
    std::uint8_t num_interfaces = 0U;
    InterfaceInfo interface_zero{};
};

// Single-attempt native operations. The transport adds the FR30 retry policy
// around control; bulk transfers are never retried here.
class UsbIo {
public:
    virtual ~UsbIo() noexcept = default;

    virtual OpenOutcome open(const DeviceLocation& location,
                             DeviceSummary& summary) noexcept = 0;
    virtual OpenOutcome claim_interface(std::uint8_t number) noexcept = 0;
    virtual TransferOutcome control_in(std::uint8_t request_type,
                                       std::uint8_t request,
                                       std::uint16_t value,
                                       std::uint16_t index,
                                       std::uint16_t length,
                                       std::uint8_t* data,
                                       std::uint32_t timeout_ms) noexcept = 0;
    virtual TransferOutcome control_out(std::uint8_t request_type,
                                        std::uint8_t request,
                                        std::uint16_t value,
                                        std::uint16_t index,
                                        std::uint16_t length,
                                        const std::uint8_t* data,
                                        std::uint32_t timeout_ms) noexcept = 0;
    virtual TransferOutcome bulk_in(std::uint8_t endpoint,
                                    std::uint8_t* data,
                                    std::size_t length,
                                    std::uint32_t timeout_ms) noexcept = 0;
    virtual TransferOutcome bulk_out(std::uint8_t endpoint,
                                     const std::uint8_t* data,
                                     std::size_t length,
                                     std::uint32_t timeout_ms) noexcept = 0;
    virtual void release_interface(std::uint8_t number) noexcept = 0;
    virtual void close() noexcept = 0;
};

}  // namespace torne::usb

#endif  // TORNE_USB_TRANSFER_H
