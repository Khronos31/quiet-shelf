// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/usb.hpp>

#include <cerrno>
#include <climits>
#include <limits>
#include <utility>

#include <libusb.h>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dtv::internal {

struct UsbContext::State {
    libusb_context *context = nullptr;
    dtv_result result = {0, 0};
    ~State()
    {
        if (context != nullptr) {
            libusb_exit(context);
        }
    }
};

struct UsbDeviceSnapshot::State {
    std::shared_ptr<UsbContext::State> context;
    libusb_device *device = nullptr;
    ~State()
    {
        if (device != nullptr) {
            libusb_unref_device(device);
        }
    }
};

struct UsbDevice::State {
    std::shared_ptr<UsbContext::State> context;
    libusb_device_handle *handle = nullptr;
    int interface_number = -1;
    int owned_descriptor = -1;
    dtv_result open_result = {0, 0};
    dtv_usb_io io{};
    ~State()
    {
        if (handle != nullptr) {
            if (interface_number >= 0) {
                libusb_release_interface(handle, interface_number);
            }
            libusb_close(handle);
        }
#if defined(__linux__)
        if (owned_descriptor >= 0) {
            close(owned_descriptor);
        }
#endif
    }
};

namespace {

constexpr std::int32_t invalid_argument = 2;
constexpr std::int32_t unsupported = 3;
constexpr std::int32_t usb_error = 7;
constexpr std::int32_t timeout_error = 10;

dtv_result make_result(std::int32_t code, std::uint32_t detail = 0)
{
    return {code, detail};
}

dtv_result usb_result(int code)
{
    if (code == LIBUSB_ERROR_TIMEOUT) {
        return make_result(timeout_error, static_cast<std::uint32_t>(-code));
    }
    return make_result(usb_error, static_cast<std::uint32_t>(-code));
}

bool valid_timeout(dtv_timeout timeout, unsigned int *milliseconds)
{
    if (timeout.timeout_ms == 0) {
        return false;
    }
    *milliseconds = timeout.timeout_ms > UINT_MAX
                        ? UINT_MAX
                        : static_cast<unsigned int>(timeout.timeout_ms);
    return true;
}

dtv_result invalid()
{
    return make_result(invalid_argument);
}

dtv_result no_context()
{
    return make_result(usb_error, static_cast<std::uint32_t>(-LIBUSB_ERROR_OTHER));
}

dtv_result control_transfer(void *user, std::uint8_t request_type,
                            std::uint8_t request, std::uint16_t value,
                            std::uint16_t index, dtv_buffer *data,
                            dtv_timeout timeout)
{
    auto *state = static_cast<UsbDevice::State *>(user);
    unsigned int milliseconds;
    if (state == nullptr || state->handle == nullptr || data == nullptr ||
        !valid_timeout(timeout, &milliseconds) || data->capacity > UINT16_MAX ||
        data->size > data->capacity ||
        (data->capacity != 0 && data->data == nullptr)) {
        return invalid();
    }
    const bool input = (request_type & LIBUSB_ENDPOINT_IN) != 0;
    const auto length = input ? data->capacity : data->size;
    const int transferred = libusb_control_transfer(
        state->handle, request_type, request, value, index, data->data,
        static_cast<std::uint16_t>(length), milliseconds);
    if (transferred < 0) {
        return usb_result(transferred);
    }
    data->size = static_cast<std::uint64_t>(transferred);
    return make_result(0);
}

dtv_result bulk_out_transfer(void *user, std::uint8_t endpoint,
                             dtv_bytes data, dtv_timeout timeout)
{
    auto *state = static_cast<UsbDevice::State *>(user);
    unsigned int milliseconds;
    if (state == nullptr || state->handle == nullptr || data.size > INT_MAX ||
        (data.size != 0 && data.data == nullptr) ||
        !valid_timeout(timeout, &milliseconds)) {
        return invalid();
    }
    int transferred = 0;
    const int result = libusb_bulk_transfer(
        state->handle, endpoint, const_cast<unsigned char *>(data.data),
        static_cast<int>(data.size), &transferred, milliseconds);
    return result == LIBUSB_SUCCESS
        ? make_result(0, static_cast<std::uint32_t>(transferred))
        : usb_result(result);
}

dtv_result input_transfer(void *user, std::uint8_t endpoint,
                          dtv_buffer *data, dtv_timeout timeout,
                          bool interrupt)
{
    auto *state = static_cast<UsbDevice::State *>(user);
    unsigned int milliseconds;
    if (state == nullptr || state->handle == nullptr || data == nullptr ||
        data->capacity > INT_MAX ||
        (data->capacity != 0 && data->data == nullptr) ||
        !valid_timeout(timeout, &milliseconds)) {
        return invalid();
    }
    int transferred = 0;
    const int result = interrupt
        ? libusb_interrupt_transfer(state->handle, endpoint, data->data,
                                    static_cast<int>(data->capacity), &transferred,
                                    milliseconds)
        : libusb_bulk_transfer(state->handle, endpoint, data->data,
                               static_cast<int>(data->capacity), &transferred,
                               milliseconds);
    if (result != LIBUSB_SUCCESS) {
        data->size = transferred < 0 ? 0 : static_cast<std::uint64_t>(transferred);
        return usb_result(result);
    }
    data->size = static_cast<std::uint64_t>(transferred);
    return make_result(0);
}

dtv_result bulk_in_transfer(void *user, std::uint8_t endpoint,
                            dtv_buffer *data, dtv_timeout timeout)
{
    return input_transfer(user, endpoint, data, timeout, false);
}

dtv_result interrupt_in_transfer(void *user, std::uint8_t endpoint,
                                 dtv_buffer *data, dtv_timeout timeout)
{
    return input_transfer(user, endpoint, data, timeout, true);
}

dtv_usb_io make_io(void *user)
{
    return {user, control_transfer, bulk_out_transfer,
            bulk_in_transfer, interrupt_in_transfer};
}

}

UsbContext::UsbContext() : state_(std::make_shared<State>())
{
    const int result = libusb_init(&state_->context);
    if (result != LIBUSB_SUCCESS) {
        state_->result = usb_result(result);
    }
}

UsbContext::~UsbContext() = default;
UsbContext::UsbContext(UsbContext &&) noexcept = default;
UsbContext &UsbContext::operator=(UsbContext &&) noexcept = default;

dtv_result UsbContext::result() const
{
    return state_ ? state_->result : no_context();
}

std::vector<UsbDeviceSnapshot> UsbContext::enumerate(dtv_result *result) const
{
    std::vector<UsbDeviceSnapshot> snapshots;
    if (!state_ || state_->context == nullptr) {
        if (result != nullptr) {
            *result = no_context();
        }
        return snapshots;
    }
    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(state_->context, &devices);
    if (count < 0) {
        if (result != nullptr) {
            *result = usb_result(static_cast<int>(count));
        }
        return snapshots;
    }
    try {
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS) {
                continue;
            }
            UsbDeviceSnapshot snapshot{};
            snapshot.vendor_id = descriptor.idVendor;
            snapshot.product_id = descriptor.idProduct;
            snapshot.device_version = descriptor.bcdDevice;
            snapshot.device_class = descriptor.bDeviceClass;
            snapshot.device_subclass = descriptor.bDeviceSubClass;
            snapshot.device_protocol = descriptor.bDeviceProtocol;
            snapshot.bus_number = libusb_get_bus_number(devices[i]);
            snapshot.device_address = libusb_get_device_address(devices[i]);
            snapshot.configuration_count = descriptor.bNumConfigurations;
            snapshot.state_ = std::make_shared<UsbDeviceSnapshot::State>();
            snapshot.state_->context = state_;
            snapshot.state_->device = libusb_ref_device(devices[i]);
            snapshots.push_back(std::move(snapshot));
        }
    } catch (...) {
        libusb_free_device_list(devices, 1);
        throw;
    }
    libusb_free_device_list(devices, 1);
    if (result != nullptr) {
        *result = make_result(0);
    }
    return snapshots;
}

UsbDevice::UsbDevice(std::unique_ptr<State> state) : state_(std::move(state)) {}
UsbDevice::~UsbDevice() = default;

dtv_result UsbDevice::result() const
{
    return state_ ? state_->open_result : no_context();
}

const dtv_usb_io *UsbDevice::io() const
{
    return state_ && state_->handle != nullptr ? &state_->io : nullptr;
}

std::unique_ptr<UsbDevice> UsbContext::open(const UsbDeviceSnapshot &snapshot,
                                             int interface_number) const
{
    auto device_state = std::make_unique<UsbDevice::State>();
    device_state->context = state_;
    if (!state_ || state_->context == nullptr || !snapshot.state_ ||
        snapshot.state_->context != state_ || interface_number < 0 || interface_number > 255) {
        device_state->open_result = invalid();
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }

    libusb_device_descriptor device_descriptor{};
    const int descriptor_result = libusb_get_device_descriptor(snapshot.state_->device,
                                                                &device_descriptor);
    if (descriptor_result != LIBUSB_SUCCESS) {
        device_state->open_result = usb_result(descriptor_result);
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    if (device_descriptor.idVendor != snapshot.vendor_id ||
        device_descriptor.idProduct != snapshot.product_id ||
        device_descriptor.bcdDevice != snapshot.device_version ||
        device_descriptor.bDeviceClass != snapshot.device_class ||
        device_descriptor.bDeviceSubClass != snapshot.device_subclass ||
        device_descriptor.bDeviceProtocol != snapshot.device_protocol ||
        device_descriptor.bNumConfigurations != snapshot.configuration_count ||
        libusb_get_bus_number(snapshot.state_->device) != snapshot.bus_number ||
        libusb_get_device_address(snapshot.state_->device) != snapshot.device_address) {
        device_state->open_result = make_result(unsupported);
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }

    libusb_config_descriptor *configuration = nullptr;
    int status = libusb_get_active_config_descriptor(snapshot.state_->device, &configuration);
    if (status == LIBUSB_ERROR_NOT_FOUND) {
        status = libusb_get_config_descriptor(snapshot.state_->device, 0, &configuration);
    }
    if (status != LIBUSB_SUCCESS || configuration == nullptr) {
        device_state->open_result = usb_result(status);
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    bool interface_found = false;
    for (std::uint8_t i = 0; i < configuration->bNumInterfaces; ++i) {
        const auto &interface = configuration->interface[i];
        for (int alternate = 0; alternate < interface.num_altsetting; ++alternate) {
            if (interface.altsetting[alternate].bInterfaceNumber == interface_number) {
                interface_found = true;
            }
        }
    }
    libusb_free_config_descriptor(configuration);
    if (!interface_found) {
        device_state->open_result = make_result(unsupported);
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }

    status = libusb_open(snapshot.state_->device, &device_state->handle);
    if (status == LIBUSB_SUCCESS) {
        status = libusb_claim_interface(device_state->handle, interface_number);
    }
    if (status != LIBUSB_SUCCESS) {
        device_state->open_result = usb_result(status);
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    device_state->interface_number = interface_number;
    device_state->io = make_io(device_state.get());
    return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
}

std::unique_ptr<UsbDevice> open_usb_fd(int descriptor, int interface_number,
                                       dtv_result *result)
{
    auto device_state = std::make_unique<UsbDevice::State>();
    if (result == nullptr || descriptor < 0 || interface_number < 0 || interface_number > 255) {
        if (result != nullptr) {
            *result = invalid();
        }
        return nullptr;
    }
#if defined(__linux__)
    const int owned_descriptor = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if (owned_descriptor < 0) {
        *result = invalid();
        return nullptr;
    }
    device_state->context = std::make_shared<UsbContext::State>();
    const int init_result = libusb_init(&device_state->context->context);
    if (init_result != LIBUSB_SUCCESS) {
        close(owned_descriptor);
        *result = usb_result(init_result);
        return nullptr;
    }
    const int wrap_result = libusb_wrap_sys_device(device_state->context->context,
                                                   owned_descriptor,
                                                   &device_state->handle);
    if (wrap_result != LIBUSB_SUCCESS) {
        close(owned_descriptor);
        *result = usb_result(wrap_result);
        return nullptr;
    }
    device_state->owned_descriptor = owned_descriptor;
    auto *wrapped_device = libusb_get_device(device_state->handle);
    libusb_device_descriptor descriptor_info{};
    libusb_config_descriptor *configuration = nullptr;
    int descriptor_result = wrapped_device == nullptr
        ? LIBUSB_ERROR_NO_DEVICE
        : libusb_get_device_descriptor(wrapped_device, &descriptor_info);
    if (descriptor_result == LIBUSB_SUCCESS) {
        descriptor_result = libusb_get_active_config_descriptor(wrapped_device,
                                                                 &configuration);
    }
    if (descriptor_result != LIBUSB_SUCCESS || configuration == nullptr) {
        device_state->open_result = descriptor_result == LIBUSB_SUCCESS
            ? make_result(unsupported)
            : usb_result(descriptor_result);
        *result = device_state->open_result;
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    bool interface_found = false;
    for (std::uint8_t i = 0; i < configuration->bNumInterfaces; ++i) {
        const auto &interface = configuration->interface[i];
        for (int alternate = 0; alternate < interface.num_altsetting; ++alternate) {
            if (interface.altsetting[alternate].bInterfaceNumber == interface_number) {
                interface_found = true;
            }
        }
    }
    libusb_free_config_descriptor(configuration);
    if (!interface_found) {
        device_state->open_result = make_result(unsupported);
        *result = device_state->open_result;
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    const int claim_result = libusb_claim_interface(device_state->handle, interface_number);
    if (claim_result != LIBUSB_SUCCESS) {
        device_state->open_result = usb_result(claim_result);
        *result = device_state->open_result;
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    device_state->interface_number = interface_number;
    device_state->io = make_io(device_state.get());
    *result = make_result(0);
    return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
#else
    (void)descriptor;
    (void)interface_number;
    *result = make_result(unsupported);
    return nullptr;
#endif
}

}
