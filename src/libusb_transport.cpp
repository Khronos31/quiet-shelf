// SPDX-License-Identifier: GPL-2.0-only
#include <torne/libusb_transport.h>
#include <dtv/userland/identity.h>

#include <libusb.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace torne {
namespace {
using dtv::userland::Error;
using dtv::userland::Result;

Error map_libusb(int code) noexcept
{
    switch (code) {
    case LIBUSB_ERROR_NO_DEVICE: return Error::DISCONNECTED;
    case LIBUSB_ERROR_BUSY: return Error::BUSY;
    case LIBUSB_ERROR_TIMEOUT: return Error::TIMEOUT;
    case LIBUSB_ERROR_NOT_FOUND: return Error::NOT_FOUND;
    case LIBUSB_ERROR_INVALID_PARAM: return Error::INVALID_ARGUMENT;
    case LIBUSB_ERROR_PIPE: return Error::PROTOCOL_ERROR;
    default: return Error::USB_IO;
    }
}

// The contract is portable bus-port[.port...], not volatile USB device address.
std::string usb_port(libusb_device* device)
{
    std::array<std::uint8_t, 8> ports{};
    const int count = libusb_get_port_numbers(device, ports.data(),
                                               static_cast<int>(ports.size()));
    if (count <= 0) return {};
    std::string result = std::to_string(libusb_get_bus_number(device));
    result.push_back('-');
    for (int i = 0; i < count; ++i) {
        if (i != 0) result.push_back('.');
        result += std::to_string(ports[static_cast<std::size_t>(i)]);
    }
    return result;
}

int errno_from_libusb(int code) noexcept
{
    switch (code) {
    case LIBUSB_ERROR_TIMEOUT: return -ETIMEDOUT;
    case LIBUSB_ERROR_NO_DEVICE: return -ENODEV;
    case LIBUSB_ERROR_BUSY: return -EBUSY;
    case LIBUSB_ERROR_ACCESS: return -EACCES;
    case LIBUSB_ERROR_INVALID_PARAM: return -EINVAL;
    case LIBUSB_ERROR_PIPE: return -EPIPE;
    case LIBUSB_ERROR_INTERRUPTED: return -EINTR;
    case LIBUSB_ERROR_OVERFLOW: return -EOVERFLOW;
    default: return -EIO;
    }
}
} // namespace

LibusbTransport::~LibusbTransport() noexcept
{
    close();
    if (context_) libusb_exit(context_);
}

Result<void> LibusbTransport::ensure_context() noexcept
{
    if (context_) return Result<void>::success();
    const int ret = libusb_init(&context_);
    return ret == 0 ? Result<void>::success()
                    : Result<void>::failure(map_libusb(ret));
}

Result<std::vector<UsbObservation>> LibusbTransport::enumerate(bool& complete) noexcept
{
    complete = false;
    const auto initialized = ensure_context();
    if (!initialized)
        return Result<std::vector<UsbObservation>>::failure(initialized.error());

    libusb_device** list = nullptr;
    const auto count = libusb_get_device_list(context_, &list);
    if (count < 0)
        return Result<std::vector<UsbObservation>>::failure(
            map_libusb(static_cast<int>(count)));
    std::vector<UsbObservation> found;
    complete = true;
    for (std::ptrdiff_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        const int rc = libusb_get_device_descriptor(list[i], &desc);
        if (rc < 0) {
            complete = false;
            continue;
        }
        if (desc.idVendor != 0x054cU || desc.idProduct != 0x04b2U) continue;
        UsbObservation one;
        one.bus = libusb_get_bus_number(list[i]);
        one.address = libusb_get_device_address(list[i]);
        one.port = usb_port(list[i]);
        if (one.port.empty()) complete = false;
        found.push_back(std::move(one));
    }
    libusb_free_device_list(list, 1);
    return Result<std::vector<UsbObservation>>::success(std::move(found));
}

Result<void> LibusbTransport::open(std::string_view selector) noexcept
{
    if (handle_) return Result<void>::failure(Error::BUSY);
    const auto parsed = dtv::userland::identity::parse_device_selector(selector);
    if (!parsed ||
        parsed.value().kind != dtv::userland::identity::SelectorKind::usb_topology ||
        parsed.value().topology.count != 1U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    const auto& path = parsed.value().topology.paths[0];
    const std::string wanted(path.bytes.data(), path.length);
    const auto initialized = ensure_context();
    if (!initialized) return initialized;

    libusb_device** list = nullptr;
    const auto count = libusb_get_device_list(context_, &list);
    if (count < 0) return Result<void>::failure(map_libusb(static_cast<int>(count)));
    int rc = LIBUSB_ERROR_NOT_FOUND;
    for (std::ptrdiff_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) < 0) continue;
        if (desc.idVendor != 0x054cU || desc.idProduct != 0x04b2U ||
            usb_port(list[i]) != wanted) continue;
        rc = libusb_open(list[i], &handle_);
        break;
    }
    libusb_free_device_list(list, 1);
    if (rc < 0 || !handle_) return Result<void>::failure(map_libusb(rc));

    // Never silently detach the kernel module / another client.
    const int active = libusb_kernel_driver_active(handle_, 0);
    if (active == 1) {
        close();
        return Result<void>::failure(Error::BUSY);
    }
    rc = libusb_claim_interface(handle_, 0);
    if (rc < 0) {
        close();
        return Result<void>::failure(map_libusb(rc));
    }
    claimed_ = true;
    rc = libusb_set_interface_alt_setting(handle_, 0, 0);
    if (rc < 0) {
        close();
        return Result<void>::failure(map_libusb(rc));
    }
    return Result<void>::success();
}

void LibusbTransport::close() noexcept
{
    if (handle_) {
        if (claimed_) libusb_release_interface(handle_, 0);
        libusb_close(handle_);
    }
    handle_ = nullptr;
    claimed_ = false;
}

struct torne_ops LibusbTransport::callbacks() const noexcept
{
    struct torne_ops ops{};
    ops.control = &LibusbTransport::c_control;
    ops.bulk = &LibusbTransport::c_bulk;
    ops.msleep = &LibusbTransport::c_sleep;
    ops.now_ms = &LibusbTransport::c_now;
    ops.log = &LibusbTransport::c_log;
    return ops;
}

int LibusbTransport::c_control(void* context, u8 request_type, u16 value,
                                u16 index, void* buffer, u16 length) noexcept
{
    auto* usb = static_cast<LibusbTransport*>(context);
    if (!usb->handle_) return -ENODEV;
    const int n = libusb_control_transfer(
        usb->handle_, request_type, 0xBC, value, index,
        static_cast<unsigned char*>(buffer), length, 1000);
    return n < 0 ? errno_from_libusb(n) : n;
}

int LibusbTransport::c_bulk(void* context, u8 endpoint, void* buffer,
                             int length, unsigned int timeout_ms) noexcept
{
    auto* usb = static_cast<LibusbTransport*>(context);
    if (!usb->handle_) return -ENODEV;
    int n = 0;
    const int rc = libusb_bulk_transfer(
        usb->handle_, endpoint, static_cast<unsigned char*>(buffer),
        length, &n, timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT && n > 0) return n;
    return rc < 0 ? errno_from_libusb(rc) : n;
}

void LibusbTransport::c_sleep(void*, unsigned int ms) noexcept
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

u64 LibusbTransport::c_now(void*) noexcept
{
    const auto n = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    return static_cast<u64>(n.count());
}

void LibusbTransport::c_log(void*, int, const char* format, ...)
{
    std::fprintf(stderr, "[MB86H47A] ");
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
}

} // namespace torne
