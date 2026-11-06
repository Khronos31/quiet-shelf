/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_USB_TRANSFER_BACKEND_HPP
#define DTV_INTERNAL_USB_TRANSFER_BACKEND_HPP

#include <memory>
#include <vector>

#include <libusb.h>

#include <dtv/internal/usb.hpp>

namespace dtv::internal {

class UsbTransferBackend {
public:
    virtual ~UsbTransferBackend() = default;
    virtual libusb_transfer *allocate_transfer() = 0;
    virtual void free_transfer(libusb_transfer *) = 0;
    virtual int submit_transfer(libusb_transfer *) = 0;
    virtual int handle_events(libusb_context *, timeval *) = 0;
    virtual int release_interface(libusb_device_handle *, int) = 0;
    virtual void close_handle(libusb_device_handle *) = 0;
    virtual void exit_context(libusb_context *) = 0;
};

std::shared_ptr<UsbTransferBackend> make_libusb_transfer_backend();

struct UsbTestDeviceFactory {
    static std::unique_ptr<UsbDevice> create(
        std::shared_ptr<UsbTransferBackend>, libusb_device_handle *,
        std::vector<int> claimed_interfaces, int owned_descriptor = -1);
};

}

#endif
