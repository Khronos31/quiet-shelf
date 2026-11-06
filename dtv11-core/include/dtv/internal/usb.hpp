/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_USB_HPP
#define DTV_INTERNAL_USB_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include <dtv/spi/types.h>

namespace dtv::internal {

struct UsbDeviceSnapshot {
    struct State;
    struct State;
    std::uint16_t vendor_id;
    std::uint16_t product_id;
    std::uint16_t device_version;
    std::uint8_t device_class;
    std::uint8_t device_subclass;
    std::uint8_t device_protocol;
    std::uint8_t bus_number;
    std::uint8_t device_address;
    std::uint8_t configuration_count;

private:
    std::shared_ptr<State> state_;
    friend class UsbContext;
};

class UsbDevice;

class UsbContext {
public:
    struct State;
    UsbContext();
    ~UsbContext();
    UsbContext(const UsbContext &) = delete;
    UsbContext &operator=(const UsbContext &) = delete;
    UsbContext(UsbContext &&) noexcept;
    UsbContext &operator=(UsbContext &&) noexcept;

    dtv_result result() const;
    std::vector<UsbDeviceSnapshot> enumerate(dtv_result *result = nullptr) const;
    std::unique_ptr<UsbDevice> open(const UsbDeviceSnapshot &, int interface_number) const;

private:
    std::shared_ptr<State> state_;
};

class UsbDevice {
public:
    struct State;
    ~UsbDevice();
    UsbDevice(const UsbDevice &) = delete;
    UsbDevice &operator=(const UsbDevice &) = delete;

    dtv_result result() const;
    const dtv_usb_io *io() const;

private:
    explicit UsbDevice(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
    friend class UsbContext;
    friend std::unique_ptr<UsbDevice> open_usb_fd(int, int, dtv_result *);
};

/* The input descriptor is borrowed; the Core duplicates and owns its wrapper FD. */
std::unique_ptr<UsbDevice> open_usb_fd(int descriptor, int interface_number,
                                       dtv_result *result);

}

#endif
