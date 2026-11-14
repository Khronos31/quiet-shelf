/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstdlib>
#include <memory>
#include <unordered_map>

#include <dtv/internal/l3-registry.hpp>
#include <dtv/spi/device_ops.hpp>
#include "usb-transfer-backend.hpp"

void set_scan_group_case(unsigned);

namespace {

libusb_device *device(std::uintptr_t value)
{
    return reinterpret_cast<libusb_device *>(value);
}

unsigned wraps;
unsigned closed;
unsigned fail_wrap;
bool fail_config;
bool duplicate_second;
bool reinserted;
int last_descriptor;
unsigned wrap_serial;
std::unordered_map<std::uintptr_t, unsigned> wrapped_logical;

unsigned logical_device(libusb_device *value)
{
    const auto raw = reinterpret_cast<std::uintptr_t>(value);
    if (raw <= 2) return static_cast<unsigned>(raw);
    return wrapped_logical[raw];
}

class FakeFdBackend final : public dtv::internal::UsbTransferBackend {
public:
    int init_context(libusb_context **context) override
    {
        *context = reinterpret_cast<libusb_context *>(0x100);
        return LIBUSB_SUCCESS;
    }
    ssize_t get_device_list(libusb_context *, libusb_device ***out) override
    {
        auto **list = static_cast<libusb_device **>(std::calloc(3, sizeof(libusb_device *)));
        if (!list) return LIBUSB_ERROR_NO_MEM;
        list[0] = device(1);
        list[1] = device(2);
        *out = list;
        return 2;
    }
    void free_device_list(libusb_device **list, int) override { std::free(list); }
    int get_device_descriptor(libusb_device *value, libusb_device_descriptor *out) override
    {
        const auto logical = logical_device(value);
        *out = {};
        out->idVendor = 0x1234;
        out->idProduct = logical == 1 ? 0x0201 : 0x0202;
        out->bNumConfigurations = 1;
        return LIBUSB_SUCCESS;
    }
    std::uint8_t get_bus_number(libusb_device *) override { return 1; }
    std::uint8_t get_device_address(libusb_device *value) override
    {
        return static_cast<std::uint8_t>(logical_device(value));
    }
    int get_port_numbers(libusb_device *value, std::uint8_t *ports, int) override
    {
        ports[0] = static_cast<std::uint8_t>(logical_device(value));
        return 1;
    }
    int get_config_descriptor(libusb_device *, std::uint8_t,
                              libusb_config_descriptor **out) override
    {
        if (fail_config) {
            fail_config = false;
            return LIBUSB_ERROR_IO;
        }
        auto *config = new libusb_config_descriptor{};
        auto *interface = new libusb_interface{};
        auto *alternate = new libusb_interface_descriptor{};
        alternate->bInterfaceNumber = 0;
        interface->altsetting = alternate;
        interface->num_altsetting = 1;
        config->interface = interface;
        config->bNumInterfaces = 1;
        config->bConfigurationValue = 1;
        *out = config;
        return LIBUSB_SUCCESS;
    }
    void free_config_descriptor(libusb_config_descriptor *config) override
    {
        delete config->interface->altsetting;
        delete config->interface;
        delete config;
    }
    int wrap_sys_device(libusb_context *, std::intptr_t descriptor,
                        libusb_device_handle **out) override
    {
        ++wraps;
        last_descriptor = static_cast<int>(descriptor);
        if (fail_wrap == wraps) return LIBUSB_ERROR_ACCESS;
        const auto logical = duplicate_second && wraps == 2 ? 1u : (wraps % 2 ? 1u : 2u);
        wrapped_logical.emplace(1000 + wrap_serial, logical);
        *out = reinterpret_cast<libusb_device_handle *>(0x2000 + wrap_serial++);
        return LIBUSB_SUCCESS;
    }
    libusb_device *get_device(libusb_device_handle *handle) override
    {
        const auto serial = reinterpret_cast<std::uintptr_t>(handle) - 0x2000;
        return device(1000 + serial);
    }
    bool get_connection_identity(libusb_device *value, int,
                                 dtv::internal::UsbConnectionIdentity *identity) override
    {
        const auto logical = logical_device(value);
        identity->filesystem = 1;
        identity->inode = logical + (reinserted ? 100 : 0);
        identity->device = 2;
        identity->valid = true;
        return true;
    }
    bool supports_connection_identity() const override { return true; }
    libusb_device *ref_device(libusb_device *value) override { return value; }
    void unref_device(libusb_device *) override {}
    libusb_transfer *allocate_transfer() override { return nullptr; }
    void free_transfer(libusb_transfer *) override {}
    int submit_transfer(libusb_transfer *) override { return LIBUSB_ERROR_NOT_SUPPORTED; }
    int cancel_transfer(libusb_transfer *) override { return LIBUSB_ERROR_NOT_SUPPORTED; }
    int handle_events(libusb_context *, timeval *) override { return LIBUSB_SUCCESS; }
    int get_active_config_descriptor(libusb_device *, libusb_config_descriptor **) override
    {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    int open_device(libusb_device *, libusb_device_handle **) override
    {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    int claim_interface(libusb_device_handle *, int) override { return LIBUSB_ERROR_NOT_SUPPORTED; }
    int set_interface_alt_setting(libusb_device_handle *, int, int) override
    {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    int release_interface(libusb_device_handle *, int) override { return LIBUSB_SUCCESS; }
    void close_handle(libusb_device_handle *) override { ++closed; }
    void exit_context(libusb_context *) override {}
};
}

extern "C" void fd_test_install_backend(void)
{
    dtv::internal::set_public_discovery_usb_backend_for_test(std::make_shared<FakeFdBackend>());
}
extern "C" void fd_test_reset(void)
{
    wraps = closed = fail_wrap = 0;
    fail_config = false;
    duplicate_second = false;
    reinserted = false;
}
extern "C" void fd_test_fail_wrap(unsigned value) { fail_wrap = value; }
extern "C" void fd_test_duplicate_second(unsigned value) { duplicate_second = value != 0; }
extern "C" void fd_test_reinserted(unsigned value) { reinserted = value != 0; }
extern "C" void fd_test_fail_config(unsigned value) { fail_config = value != 0; }
extern "C" void fd_test_group_error(unsigned value) { set_scan_group_case(value ? 5 : 0); }
extern "C" int fd_test_last_descriptor(void) { return last_descriptor; }
extern "C" unsigned fd_test_closed(void) { return closed; }
