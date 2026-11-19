/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>

#include <dtv/internal/l3-registry.hpp>

#include "usb-transfer-backend.hpp"

namespace {

unsigned device_mode = 0;
unsigned duplicate_tokens = 0;
std::thread::id callback_thread;
std::uint32_t callback_count = 0;

libusb_device *device(std::uintptr_t value)
{
    return reinterpret_cast<libusb_device *>(value);
}

class FakeScanBackend final : public dtv::internal::UsbTransferBackend {
public:
    int init_context(libusb_context **context) override
    {
        *context = reinterpret_cast<libusb_context *>(0x100);
        return LIBUSB_SUCCESS;
    }
    ssize_t get_device_list(libusb_context *, libusb_device ***out) override
    {
        if (device_mode == 4) return LIBUSB_ERROR_IO;
        const std::size_t count = device_mode == 3 ? 0 : device_mode == 1 ? 1 : 2;
        auto **list = static_cast<libusb_device **>(std::calloc(count + 1, sizeof(libusb_device *)));
        if (list == nullptr) return LIBUSB_ERROR_NO_MEM;
        if (count > 0) list[0] = device(1);
        if (count > 1) list[1] = device(device_mode == 2 ? 3 : 2);
        *out = list;
        return static_cast<ssize_t>(count);
    }
    void free_device_list(libusb_device **devices, int) override { std::free(devices); }
    int get_device_descriptor(libusb_device *value, libusb_device_descriptor *descriptor) override
    {
        *descriptor = {};
        descriptor->idVendor = 0x1234;
        descriptor->idProduct = value == device(1) ? 0x0201 : 0x0202;
        descriptor->bNumConfigurations = 1;
        return LIBUSB_SUCCESS;
    }
    std::uint8_t get_bus_number(libusb_device *) override { return 1; }
    std::uint8_t get_device_address(libusb_device *value) override
    {
        return value == device(1) ? 1 : value == device(2) ? 2 : 3;
    }
    int get_port_numbers(libusb_device *value, std::uint8_t *ports, int) override
    {
        ports[0] = value == device(1) ? 1 : value == device(2) ? 2 : 3;
        return 1;
    }
    int get_active_config_descriptor(libusb_device *, libusb_config_descriptor **) override
    {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    int get_config_descriptor(libusb_device *, std::uint8_t,
                              libusb_config_descriptor **out) override
    {
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
    libusb_transfer *allocate_transfer() override { return nullptr; }
    void free_transfer(libusb_transfer *) override {}
    int submit_transfer(libusb_transfer *) override { return LIBUSB_ERROR_NOT_SUPPORTED; }
    int cancel_transfer(libusb_transfer *) override { return LIBUSB_ERROR_NOT_SUPPORTED; }
    int handle_events(libusb_context *, timeval *) override { return LIBUSB_SUCCESS; }
    int open_device(libusb_device *, libusb_device_handle **) override
    {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    int claim_interface(libusb_device_handle *, int) override { return LIBUSB_ERROR_NOT_SUPPORTED; }
    int set_interface_alt_setting(libusb_device_handle *, int, int) override
    {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    int wrap_sys_device(libusb_context *, std::intptr_t, libusb_device_handle **) override
    {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    libusb_device *get_device(libusb_device_handle *) override { return nullptr; }
    libusb_device *ref_device(libusb_device *value) override { return value; }
    void unref_device(libusb_device *) override {}
    int release_interface(libusb_device_handle *, int) override { return LIBUSB_SUCCESS; }
    void close_handle(libusb_device_handle *) override {}
    void exit_context(libusb_context *) override {}
};

}

extern "C" void token_test_install_backend(void)
{
    dtv::internal::set_public_discovery_usb_backend_for_test(
        std::make_shared<FakeScanBackend>());
}

extern "C" void token_test_set_devices(unsigned mode)
{
    device_mode = mode;
}

extern "C" void token_test_set_duplicate_tokens(unsigned value)
{
    duplicate_tokens = value;
}

extern "C" void token_test_callback_seen(libusb_device *)
{
    callback_thread = std::this_thread::get_id();
    ++callback_count;
}

extern "C" unsigned token_test_callback_count(void)
{
    return callback_count;
}

extern "C" int token_test_callback_is_current_thread(void)
{
    return callback_thread == std::this_thread::get_id();
}

extern "C" dtv_usb_token token_test_token_for(libusb_device *value)
{
    if (value == device(1)) return 11;
    if (value == device(2) || value == device(3)) return duplicate_tokens != 0 ? 11 : 22;
    return 0;
}

extern "C" void token_test_fail_allocation(unsigned fail)
{
    if (fail != 0) {
        dtv::internal::set_public_discovery_token_allocation_hook_for_test(
            [] { throw std::bad_alloc(); });
    } else {
        dtv::internal::set_public_discovery_token_allocation_hook_for_test({});
    }
}

extern "C" void token_test_fail_journal(unsigned fail)
{
    if (fail != 0) {
        dtv::internal::set_public_discovery_journal_hook_for_test(
            [] { throw std::bad_alloc(); });
    } else {
        dtv::internal::set_public_discovery_journal_hook_for_test({});
    }
}
