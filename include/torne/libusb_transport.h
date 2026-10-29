// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <torne/product_contract.h>
#include <dtv/userland/error.h>

extern "C" {
#include "torne_core.h"
}

#include <string_view>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

namespace torne {

// Owns one libusb context and, optionally, one claimed interface 0.
// Discovery is strictly non-invasive: no interface open/claim, no vendor writes.
class LibusbTransport final {
public:
    LibusbTransport() noexcept = default;
    ~LibusbTransport() noexcept;
    LibusbTransport(const LibusbTransport&) = delete;
    LibusbTransport& operator=(const LibusbTransport&) = delete;

    dtv::userland::Result<std::vector<UsbObservation>> enumerate(bool& complete) noexcept;
    // Requires a Core-validated single USB topology selector ("usb:bus-port").
    // Does not detach kernel drivers or steal an occupied interface.
    dtv::userland::Result<void> open(std::string_view device_selector) noexcept;
    void close() noexcept;
    struct torne_ops callbacks() const noexcept;

private:
    dtv::userland::Result<void> ensure_context() noexcept;
    static int c_control(void*, u8, u16, u16, void*, u16) noexcept;
    static int c_bulk(void*, u8, void*, int, unsigned int) noexcept;
    static void c_sleep(void*, unsigned int) noexcept;
    static u64 c_now(void*) noexcept;
    static void c_log(void*, int, const char*, ...);

    libusb_context* context_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    bool claimed_ = false;
};

} // namespace torne
