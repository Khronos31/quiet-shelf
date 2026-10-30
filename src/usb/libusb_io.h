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

}  // namespace torne::usb

#endif  // TORNE_USB_LIBUSB_IO_H
