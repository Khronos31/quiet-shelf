/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_USB_TRANSFER_BACKEND_HPP
#define DTV_INTERNAL_USB_TRANSFER_BACKEND_HPP

#include <memory>
#include <cstdint>
#include <vector>

#include <libusb.h>

#include <dtv/internal/usb.hpp>

namespace dtv::internal {

class UsbTransferBackend {
public:
    virtual ~UsbTransferBackend() = default;
    virtual int init_context(libusb_context **) = 0;
    virtual libusb_transfer *allocate_transfer() = 0;
    virtual void free_transfer(libusb_transfer *) = 0;
    virtual int submit_transfer(libusb_transfer *) = 0;
    virtual int handle_events(libusb_context *, timeval *) = 0;
    virtual int get_device_descriptor(libusb_device *, libusb_device_descriptor *) = 0;
    virtual std::uint8_t get_bus_number(libusb_device *) = 0;
    virtual std::uint8_t get_device_address(libusb_device *) = 0;
    virtual int get_active_config_descriptor(libusb_device *, libusb_config_descriptor **) = 0;
    virtual int get_config_descriptor(libusb_device *, std::uint8_t,
                                      libusb_config_descriptor **) = 0;
    virtual void free_config_descriptor(libusb_config_descriptor *) = 0;
    virtual int open_device(libusb_device *, libusb_device_handle **) = 0;
    virtual int claim_interface(libusb_device_handle *, int) = 0;
    virtual int set_interface_alt_setting(libusb_device_handle *, int, int) = 0;
    virtual int wrap_sys_device(libusb_context *, std::intptr_t, libusb_device_handle **) = 0;
    virtual libusb_device *get_device(libusb_device_handle *) = 0;
    virtual libusb_device *ref_device(libusb_device *) = 0;
    virtual void unref_device(libusb_device *) = 0;
    virtual int release_interface(libusb_device_handle *, int) = 0;
    virtual void close_handle(libusb_device_handle *) = 0;
    virtual void exit_context(libusb_context *) = 0;
};

std::shared_ptr<UsbTransferBackend> make_libusb_transfer_backend();

struct UsbTestDeviceFactory {
    static UsbContext context(std::shared_ptr<UsbTransferBackend>);
    static UsbDeviceSnapshot snapshot(const UsbContext &);
    static std::unique_ptr<UsbDevice> open_usb_fd(
        int descriptor, int interface_number, dtv_internal_result *,
        std::shared_ptr<UsbTransferBackend>);
    static std::unique_ptr<UsbDevice> open_usb_fd_impl(
        int descriptor, int interface_number, dtv_internal_result *,
        std::shared_ptr<UsbTransferBackend>);
};

}

#endif
