/* SPDX-License-Identifier: GPL-2.0-or-later */
/* USB の文字列記述子の観測・復号・有限予算・接続内 cache の契約 test（決定 66）。 */
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <libusb.h>

#include <dtv/internal/usb.hpp>

#include "usb-transfer-backend.hpp"

namespace {

libusb_device *device_value(std::uintptr_t value)
{
    return reinterpret_cast<libusb_device *>(value);
}

class SerialBackend final : public dtv::internal::UsbTransferBackend {
public:
    std::uintptr_t device_address_value = 1;
    std::uint8_t serial_number_index = 1;
    int open_error = LIBUSB_SUCCESS;
    int langid_error = LIBUSB_SUCCESS;
    int read_error = LIBUSB_SUCCESS;
    std::vector<unsigned char> string_descriptor;
    unsigned open_calls = 0;
    unsigned close_calls = 0;
    unsigned claim_calls = 0;
    unsigned wrap_calls = 0;
    std::uint64_t identity_epoch = 0;
    unsigned control_calls = 0;
    unsigned failed_control_calls = 0;

    int init_context(libusb_context **context) override
    {
        *context = reinterpret_cast<libusb_context *>(0x100);
        return LIBUSB_SUCCESS;
    }
    ssize_t get_device_list(libusb_context *, libusb_device ***out) override
    {
        auto **list = static_cast<libusb_device **>(std::calloc(2, sizeof(libusb_device *)));
        if (list == nullptr) return LIBUSB_ERROR_NO_MEM;
        list[0] = device_value(device_address_value);
        *out = list;
        return 1;
    }
    void free_device_list(libusb_device **devices, int) override { std::free(devices); }
    int get_device_descriptor(libusb_device *value, libusb_device_descriptor *descriptor) override
    {
        (void)value;
        *descriptor = {};
        descriptor->idVendor = 0x1234;
        descriptor->idProduct = 0x0001;
        descriptor->bNumConfigurations = 1;
        descriptor->iSerialNumber = serial_number_index;
        return LIBUSB_SUCCESS;
    }
    std::uint8_t get_bus_number(libusb_device *) override { return 1; }
    std::uint8_t get_device_address(libusb_device *) override
    {
        return static_cast<std::uint8_t>(device_address_value);
    }
    int get_port_numbers(libusb_device *, std::uint8_t *ports, int) override
    {
        ports[0] = static_cast<std::uint8_t>(device_address_value);
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
    int open_device(libusb_device *, libusb_device_handle **out) override
    {
        ++open_calls;
        if (open_error != LIBUSB_SUCCESS) {
            ++failed_control_calls;
            return open_error;
        }
        *out = reinterpret_cast<libusb_device_handle *>(0x500);
        return LIBUSB_SUCCESS;
    }
    int claim_interface(libusb_device_handle *, int) override
    {
        ++claim_calls;
        return LIBUSB_SUCCESS;
    }
    int set_interface_alt_setting(libusb_device_handle *, int, int) override
    {
        return LIBUSB_SUCCESS;
    }
    int wrap_sys_device(libusb_context *, std::intptr_t, libusb_device_handle **out) override
    {
        ++wrap_calls;
        *out = reinterpret_cast<libusb_device_handle *>(0x600);
        return LIBUSB_SUCCESS;
    }
    libusb_device *get_device(libusb_device_handle *) override
    {
        return device_value(device_address_value);
    }
    bool get_connection_identity(libusb_device *, int descriptor,
                                 dtv::internal::UsbConnectionIdentity *identity) override
    {
        if (identity == nullptr) return false;
        identity->filesystem = 1;
        identity->inode = identity_epoch + static_cast<std::uint64_t>(descriptor + 2);
        identity->device = 1;
        identity->valid = true;
        return true;
    }
    bool supports_connection_identity() const override { return true; }
    int get_string_descriptor(libusb_device_handle *, std::uint8_t index,
                              std::uint16_t langid, unsigned char *data, int length,
                              unsigned int) override
    {
        ++control_calls;
        if (index == 0) {
            if (langid_error != LIBUSB_SUCCESS) {
                ++failed_control_calls;
                return langid_error;
            }
            const unsigned char language[] = {4, 0x03, 0x09, 0x04};
            const auto copied = static_cast<int>((std::min)(sizeof(language),
                                                            static_cast<std::size_t>(length)));
            std::memcpy(data, language, static_cast<std::size_t>(copied));
            return copied;
        }
        (void)langid;
        if (read_error != LIBUSB_SUCCESS) {
            ++failed_control_calls;
            return read_error;
        }
        const auto copied = static_cast<int>((std::min)(string_descriptor.size(),
                                                        static_cast<std::size_t>(length)));
        if (copied > 0) std::memcpy(data, string_descriptor.data(),
                                    static_cast<std::size_t>(copied));
        return copied;
    }
    libusb_transfer *allocate_transfer() override { return nullptr; }
    void free_transfer(libusb_transfer *) override {}
    int submit_transfer(libusb_transfer *) override { return LIBUSB_ERROR_NOT_SUPPORTED; }
    int cancel_transfer(libusb_transfer *) override { return LIBUSB_ERROR_NOT_SUPPORTED; }
    int handle_events(libusb_context *, timeval *) override { return LIBUSB_SUCCESS; }
    libusb_device *ref_device(libusb_device *value) override { return value; }
    void unref_device(libusb_device *) override {}
    int release_interface(libusb_device_handle *, int) override { return LIBUSB_SUCCESS; }
    void close_handle(libusb_device_handle *) override { ++close_calls; }
    void exit_context(libusb_context *) override {}
};

std::vector<unsigned char> descriptor(std::vector<unsigned char> payload)
{
    std::vector<unsigned char> raw;
    raw.push_back(static_cast<unsigned char>(payload.size() + 2));
    raw.push_back(0x03);
    raw.insert(raw.end(), payload.begin(), payload.end());
    return raw;
}

std::vector<unsigned char> units(std::initializer_list<std::uint16_t> values)
{
    std::vector<unsigned char> payload;
    for (const auto unit : values) {
        payload.push_back(static_cast<unsigned char>(unit & 0xff));
        payload.push_back(static_cast<unsigned char>((unit >> 8) & 0xff));
    }
    return descriptor(payload);
}

std::vector<unsigned char> ascii(const char *text)
{
    std::vector<unsigned char> payload;
    for (const char *current = text; *current != '\0'; ++current) {
        payload.push_back(static_cast<unsigned char>(*current));
        payload.push_back(0);
    }
    return descriptor(payload);
}

const std::vector<std::pair<std::uint16_t, std::uint16_t>> kDeclared{{0x1234, 0x0001}};

int observe(dtv::internal::UsbContext &context, std::uint64_t deadline,
            std::vector<dtv::internal::UsbDeviceSnapshot> *out, std::uint32_t timeout_ms = 250)
{
    dtv_internal_result result{};
    auto snapshots = context.enumerate(&result);
    if (result.code != DTV_INTERNAL_RESULT_OK || snapshots.size() != 1) return 1;
    result = context.observe_serials(snapshots, kDeclared, deadline, timeout_ms);
    if (result.code != DTV_INTERNAL_RESULT_OK) return 2;
    *out = std::move(snapshots);
    return 0;
}

}

int main()
{
    const auto far_deadline = (std::numeric_limits<std::uint64_t>::max)();

    {
        SerialBackend backend;
        backend.string_descriptor = ascii("ABC123");
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 1;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            snapshots[0].serial_utf8 != "ABC123") return 2;
    }

    {
        SerialBackend backend;
        backend.string_descriptor = units({0x041c, 0x0438, 0x0440});
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 3;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            snapshots[0].serial_utf8 != "\xd0\x9c\xd0\xb8\xd1\x80") return 4;
    }

    {
        SerialBackend backend;
        backend.string_descriptor = units({0xd834, 0xdd1e});
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 5;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            snapshots[0].serial_utf8 != "\xf0\x9d\x84\x9e") return 6;
    }

    {
        SerialBackend backend;
        backend.string_descriptor = units({0x0041, 0x0000, 0x0042});
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 7;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            snapshots[0].serial_utf8 != std::string("A\0B", 3)) return 8;
    }

    {
        SerialBackend backend;
        backend.string_descriptor = descriptor({});
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 9;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            !snapshots[0].serial_utf8.empty()) return 10;
    }

    {
        SerialBackend backend;
        backend.serial_number_index = 0;
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 11;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_ABSENT ||
            !snapshots[0].serial_utf8.empty()) return 12;
    }

    {
        SerialBackend backend;
        backend.string_descriptor = units({0xd800, 0x0041});
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 13;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_MALFORMED) return 14;
    }

    {
        SerialBackend backend;
        backend.string_descriptor = {4, 0x03, 0x41};
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 15;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_MALFORMED) return 16;
    }

    {
        SerialBackend backend;
        backend.open_error = LIBUSB_ERROR_ACCESS;
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 17;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_UNREADABLE) return 18;
    }

    {
        SerialBackend backend;
        backend.read_error = LIBUSB_ERROR_TIMEOUT;
        auto context = dtv::internal::UsbContext::scan_only(
            std::make_shared<SerialBackend>(backend));
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, far_deadline, &snapshots) != 0) return 19;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_UNREADABLE) return 20;
    }

    {
        auto backend = std::make_shared<SerialBackend>();
        backend->string_descriptor = ascii("X");
        auto context = dtv::internal::UsbContext::scan_only(backend);
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        if (observe(context, 0, &snapshots) != 0) return 21;
        if (snapshots[0].serial_state != DTV_USB_SERIAL_UNREADABLE ||
            backend->control_calls != 0) return 22;
    }

    {
        auto backend = std::make_shared<SerialBackend>();
        backend->string_descriptor = ascii("CACHE");
        auto context = dtv::internal::UsbContext::scan_only(backend);
        std::vector<dtv::internal::UsbDeviceSnapshot> first;
        if (observe(context, far_deadline, &first) != 0) return 23;
        if (first[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            first[0].serial_utf8 != "CACHE") return 24;
        const auto controls = backend->control_calls;
        backend->read_error = LIBUSB_ERROR_TIMEOUT;
        std::vector<dtv::internal::UsbDeviceSnapshot> second;
        if (observe(context, far_deadline, &second) != 0) return 25;
        if (second[0].connection_token != first[0].connection_token) return 26;
        if (second[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            second[0].serial_utf8 != "CACHE" ||
            backend->control_calls != controls) return 27;

        backend->device_address_value = 2;
        backend->identity_epoch = 1;
        backend->read_error = LIBUSB_SUCCESS;
        backend->string_descriptor = ascii("NEW");
        std::vector<dtv::internal::UsbDeviceSnapshot> third;
        if (observe(context, far_deadline, &third) != 0) return 28;
        if (third[0].connection_token == first[0].connection_token ||
            third[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            third[0].serial_utf8 != "NEW") return 29;
        if (backend->claim_calls != 0) return 30;
        if (backend->open_calls == 0 || backend->close_calls != backend->open_calls) return 31;
    }

#if defined(__linux__)
    {
        auto backend = std::make_shared<SerialBackend>();
        backend->string_descriptor = ascii("FD-SERIAL");
        auto context = dtv::internal::UsbContext::scan_only(backend);
        const int caller_fd = open("/dev/null", O_RDONLY);
        if (caller_fd < 0) return 32;
        dtv_internal_result result{};
        const std::array<std::int32_t, 1> fds{{caller_fd}};
        auto snapshots = context.enumerate_fds(fds, &result);
        if (result.code != DTV_INTERNAL_RESULT_OK || snapshots.size() != 1) {
            close(caller_fd);
            return 33;
        }
        result = context.observe_serials(snapshots, kDeclared, far_deadline, 250);
        const bool kept_fd = fcntl(caller_fd, F_GETFD) >= 0;
        close(caller_fd);
        if (result.code != DTV_INTERNAL_RESULT_OK || !kept_fd ||
            snapshots[0].serial_state != DTV_USB_SERIAL_PRESENT ||
            snapshots[0].serial_utf8 != "FD-SERIAL") return 34;
        if (backend->open_calls != 0 || backend->close_calls != 0 ||
            backend->claim_calls != 0) return 35;
    }
#endif

    return 0;
}
