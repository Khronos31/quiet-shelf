/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_USB_HPP
#define DTV_INTERNAL_USB_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <dtv/spi/types.h>

namespace dtv::internal {

struct UsbTestDeviceFactory;

struct UsbInterfaceSnapshot {
    dtv_usb_interface_info info{};
    std::vector<dtv_usb_endpoint_info> endpoints;
};

struct UsbInterfaceClaim {
    std::uint8_t interface_number;
    std::uint8_t alternate_setting;
};

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
    std::uint64_t candidate_id = 0;
    std::vector<std::uint8_t> port_path;
    std::vector<UsbInterfaceSnapshot> interfaces;

private:
    std::shared_ptr<State> state_;
    friend class UsbContext;
    friend struct UsbTestDeviceFactory;
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

    dtv_internal_result result() const;
    std::vector<UsbDeviceSnapshot> enumerate(dtv_internal_result *result = nullptr) const;
    std::unique_ptr<UsbDevice> open(const UsbDeviceSnapshot &, int interface_number,
                                    const dtv_runtime_clock *clock = nullptr) const;
    std::unique_ptr<UsbDevice> open(const UsbDeviceSnapshot &,
                                    std::span<const UsbInterfaceClaim>,
                                    const dtv_runtime_clock *clock = nullptr) const;

private:
    explicit UsbContext(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
    friend struct UsbTestDeviceFactory;
};

class UsbDevice {
public:
    struct State;
    ~UsbDevice();
    UsbDevice(const UsbDevice &) = delete;
    UsbDevice &operator=(const UsbDevice &) = delete;

    dtv_internal_result result() const;
    const dtv_usb_io *io() const;

private:
    explicit UsbDevice(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
    friend class UsbContext;
    friend struct UsbTestDeviceFactory;
    friend std::unique_ptr<UsbDevice> open_usb_fd(int, int, dtv_internal_result *);
};

/* The input descriptor is borrowed; the Core duplicates and owns its wrapper FD. */
std::unique_ptr<UsbDevice> open_usb_fd(int descriptor, int interface_number,
                                       dtv_internal_result *result);

}

#endif
