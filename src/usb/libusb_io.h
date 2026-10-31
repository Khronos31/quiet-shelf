// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_USB_LIBUSB_IO_H
#define TORNE_USB_LIBUSB_IO_H

#include "usb/transfer.h"

#include <memory>

namespace torne::usb {

// Pure conversion used by the native bulk path. The status is a libusb error
// code; transferred is libusb's actual_length (valid on error paths too).
TransferOutcome bulk_transfer_outcome(int status, int transferred,
                                      std::size_t requested) noexcept;

// Native implementation over the public libusb API. Returns null when a
// libusb context cannot be initialized; otherwise open/claim map every libusb
// failure into the OS-independent contract.
std::unique_ptr<UsbIo> create_libusb_io() noexcept;

// FD-only context: on Linux/Android it initializes libusb with
// LIBUSB_OPTION_NO_DEVICE_DISCOVERY so a borrowed descriptor does not require
// general USB enumeration or hotplug authority. Other platforms fall back to
// the normal context; their open_fd remains unsupported.
std::unique_ptr<UsbIo> create_libusb_io_fd() noexcept;

}  // namespace torne::usb

#endif  // TORNE_USB_LIBUSB_IO_H
