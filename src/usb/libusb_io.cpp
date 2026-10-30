// SPDX-License-Identifier: GPL-2.0-only
#include "usb/libusb_io.h"

#include <libusb.h>

#include <climits>
#include <cstddef>
#include <cstdint>

namespace torne::usb {
namespace {

TransferCause cause_for_error(int error) noexcept
{
    switch (error) {
    case LIBUSB_ERROR_TIMEOUT: return TransferCause::timeout;
    case LIBUSB_ERROR_PIPE: return TransferCause::pipe;
    case LIBUSB_ERROR_NO_DEVICE: return TransferCause::no_device;
    case LIBUSB_ERROR_OVERFLOW: return TransferCause::overflow;
    case LIBUSB_ERROR_IO: return TransferCause::io;
    case LIBUSB_ERROR_NOT_SUPPORTED: return TransferCause::unsupported;
    case LIBUSB_ERROR_ACCESS: return TransferCause::access;
    case LIBUSB_ERROR_BUSY: return TransferCause::busy;
    case LIBUSB_ERROR_NO_MEM: return TransferCause::no_memory;
    case LIBUSB_ERROR_INVALID_PARAM: return TransferCause::invalid;
    default: return TransferCause::other;
    }
}

TransferOutcome outcome_for_error(int error) noexcept
{
    return TransferOutcome{TransferState::failed, cause_for_error(error), 0U, false};
}

OpenOutcome open_outcome_for_error(int error) noexcept
{
    switch (error) {
    case LIBUSB_ERROR_NO_DEVICE:
        return OpenOutcome{OpenState::disconnected, TransferCause::no_device};
    case LIBUSB_ERROR_ACCESS:
        return OpenOutcome{OpenState::access, TransferCause::access};
    case LIBUSB_ERROR_BUSY:
        return OpenOutcome{OpenState::busy, TransferCause::busy};
    case LIBUSB_ERROR_NOT_SUPPORTED:
        return OpenOutcome{OpenState::unsupported, TransferCause::unsupported};
    case LIBUSB_ERROR_INVALID_PARAM:
        return OpenOutcome{OpenState::invalid, TransferCause::invalid};
    case LIBUSB_ERROR_NO_MEM:
        return OpenOutcome{OpenState::io, TransferCause::no_memory};
    default:
        return OpenOutcome{OpenState::io, TransferCause::io};
    }
}

bool same_location(libusb_device* device,
                   const DeviceLocation& location) noexcept
{
    if (libusb_get_bus_number(device) != location.bus) return false;
    std::uint8_t ports[7] = {};
    const int count = libusb_get_port_numbers(
        device, ports, static_cast<int>(sizeof(ports)));
    if (count != static_cast<int>(location.port_count)) return false;
    for (int index = 0; index < count; ++index) {
        if (ports[index] != location.ports[static_cast<std::size_t>(index)])
            return false;
    }
    return count > 0;
}

class LibusbIo final : public UsbIo {
public:
    explicit LibusbIo(libusb_context* context) noexcept : context_(context) {}
    ~LibusbIo() noexcept override { close(); }

    OpenOutcome open(const DeviceLocation& location,
                     DeviceSummary& summary) noexcept override
    {
        if (context_ == nullptr || handle_ != nullptr)
            return OpenOutcome{OpenState::invalid, TransferCause::invalid};
        libusb_device** devices = nullptr;
        const ssize_t count = libusb_get_device_list(context_, &devices);
        if (count < 0) return open_outcome_for_error(static_cast<int>(count));
        libusb_device* selected = nullptr;
        for (ssize_t index = 0; index < count; ++index) {
            if (same_location(devices[index], location)) {
                selected = devices[index];
                break;
            }
        }
        if (selected == nullptr) {
            libusb_free_device_list(devices, 1);
            return OpenOutcome{OpenState::not_found, TransferCause::none};
        }
        libusb_device_descriptor descriptor{};
        const int descriptor_status =
            libusb_get_device_descriptor(selected, &descriptor);
        if (descriptor_status != LIBUSB_SUCCESS) {
            libusb_free_device_list(devices, 1);
            return open_outcome_for_error(descriptor_status);
        }
        if (descriptor.idVendor != 0x054cU || descriptor.idProduct != 0x04b2U) {
            libusb_free_device_list(devices, 1);
            return OpenOutcome{OpenState::unsupported, TransferCause::unsupported};
        }
        const int open_status = libusb_open(selected, &handle_);
        if (open_status != LIBUSB_SUCCESS) {
            handle_ = nullptr;
            libusb_free_device_list(devices, 1);
            return open_outcome_for_error(open_status);
        }

        int configuration = 0;
        const int configuration_status = libusb_get_configuration(handle_, &configuration);
        if (configuration_status != LIBUSB_SUCCESS) {
            libusb_free_device_list(devices, 1);
            return open_outcome_for_error(configuration_status);
        }
        if (configuration != 1) {
            libusb_free_device_list(devices, 1);
            return OpenOutcome{OpenState::unsupported, TransferCause::unsupported};
        }
        libusb_config_descriptor* active = nullptr;
        const int active_status = libusb_get_active_config_descriptor(selected, &active);
        if (active_status != LIBUSB_SUCCESS) {
            libusb_free_device_list(devices, 1);
            return open_outcome_for_error(active_status);
        }
        summary.vid = descriptor.idVendor;
        summary.pid = descriptor.idProduct;
        summary.num_configurations = descriptor.bNumConfigurations;
        summary.configuration_value = active->bConfigurationValue;
        summary.num_interfaces = active->bNumInterfaces;
        if (active->bNumInterfaces != 0U) {
            const libusb_interface& interface = active->interface[0];
            for (int alternate = 0; alternate < interface.num_altsetting; ++alternate) {
                const libusb_interface_descriptor& setting = interface.altsetting[alternate];
                if (setting.bAlternateSetting != 0U) continue;
                summary.interface_zero.number = setting.bInterfaceNumber;
                summary.interface_zero.alternate = setting.bAlternateSetting;
                summary.interface_zero.interface_class = setting.bInterfaceClass;
                summary.interface_zero.interface_subclass = setting.bInterfaceSubClass;
                summary.interface_zero.interface_protocol = setting.bInterfaceProtocol;
                const std::size_t endpoint_count =
                    setting.bNumEndpoints < summary.interface_zero.endpoints.size()
                        ? setting.bNumEndpoints
                        : summary.interface_zero.endpoints.size();
                summary.interface_zero.endpoint_count =
                    static_cast<std::uint8_t>(endpoint_count);
                for (std::size_t index = 0U; index < endpoint_count; ++index) {
                    const libusb_endpoint_descriptor& endpoint =
                        setting.endpoint[index];
                    summary.interface_zero.endpoints[index].address =
                        endpoint.bEndpointAddress;
                    summary.interface_zero.endpoints[index].attributes =
                        endpoint.bmAttributes;
                    summary.interface_zero.endpoints[index].max_packet =
                        endpoint.wMaxPacketSize;
                    summary.interface_zero.endpoints[index].interval =
                        endpoint.bInterval;
                }
                break;
            }
        }
        libusb_free_config_descriptor(active);
        libusb_free_device_list(devices, 1);
        return OpenOutcome{OpenState::ok, TransferCause::none};
    }

    OpenOutcome claim_interface(std::uint8_t number) noexcept override
    {
        if (handle_ == nullptr)
            return OpenOutcome{OpenState::invalid, TransferCause::invalid};
        const int status = libusb_claim_interface(handle_, static_cast<int>(number));
        if (status != LIBUSB_SUCCESS) return open_outcome_for_error(status);
        return OpenOutcome{OpenState::ok, TransferCause::none};
    }

    TransferOutcome control_in(std::uint8_t request_type,
                               std::uint8_t request,
                               std::uint16_t value,
                               std::uint16_t index,
                               std::uint16_t length,
                               std::uint8_t* data,
                               std::uint32_t timeout_ms) noexcept override
    {
        return control(request_type, request, value, index, length, data, timeout_ms);
    }

    TransferOutcome control_out(std::uint8_t request_type,
                                std::uint8_t request,
                                std::uint16_t value,
                                std::uint16_t index,
                                std::uint16_t length,
                                const std::uint8_t* data,
                                std::uint32_t timeout_ms) noexcept override
    {
        return control(request_type, request, value, index, length,
                       const_cast<std::uint8_t*>(data), timeout_ms);
    }

    TransferOutcome bulk_in(std::uint8_t endpoint,
                            std::uint8_t* data,
                            std::size_t length,
                            std::uint32_t timeout_ms) noexcept override
    {
        return bulk(endpoint, data, length, timeout_ms);
    }

    TransferOutcome bulk_out(std::uint8_t endpoint,
                             const std::uint8_t* data,
                             std::size_t length,
                             std::uint32_t timeout_ms) noexcept override
    {
        return bulk(endpoint, const_cast<std::uint8_t*>(data), length, timeout_ms);
    }

    void release_interface(std::uint8_t number) noexcept override
    {
        if (handle_ != nullptr)
            (void)libusb_release_interface(handle_, static_cast<int>(number));
    }

    void close() noexcept override
    {
        if (handle_ != nullptr) {
            libusb_close(handle_);
            handle_ = nullptr;
        }
        if (context_ != nullptr) {
            libusb_exit(context_);
            context_ = nullptr;
        }
    }

private:
    TransferOutcome control(std::uint8_t request_type,
                            std::uint8_t request,
                            std::uint16_t value,
                            std::uint16_t index,
                            std::uint16_t length,
                            std::uint8_t* data,
                            std::uint32_t timeout_ms) noexcept
    {
        if (handle_ == nullptr)
            return TransferOutcome{TransferState::failed, TransferCause::invalid, 0U, false};
        const int status = libusb_control_transfer(
            handle_, request_type, request, value, index, data, length,
            timeout_ms);
        if (status < 0) return outcome_for_error(status);
        return TransferOutcome{
            status == static_cast<int>(length) ? TransferState::complete
                                               : TransferState::short_transfer,
            TransferCause::none, static_cast<std::size_t>(status), false};
    }

    TransferOutcome bulk(std::uint8_t endpoint,
                         std::uint8_t* data,
                         std::size_t length,
                         std::uint32_t timeout_ms) noexcept
    {
        if (handle_ == nullptr || length > static_cast<std::size_t>(INT_MAX))
            return TransferOutcome{TransferState::failed, TransferCause::invalid, 0U, false};
        int transferred = 0;
        const int status = libusb_bulk_transfer(
            handle_, endpoint, data, static_cast<int>(length), &transferred,
            timeout_ms);
        return bulk_transfer_outcome(status, transferred, length);
    }

    libusb_context* context_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
};

}  // namespace

TransferOutcome bulk_transfer_outcome(int status, int transferred,
                                      std::size_t requested) noexcept
{
    const std::size_t actual =
        transferred > 0 ? static_cast<std::size_t>(transferred) : 0U;
    if (status != LIBUSB_SUCCESS)
        return TransferOutcome{TransferState::failed, cause_for_error(status),
                               actual, false};
    return TransferOutcome{
        actual == requested ? TransferState::complete
                            : TransferState::short_transfer,
        TransferCause::none, actual, false};
}

std::unique_ptr<UsbIo> create_libusb_io() noexcept
{
    libusb_context* context = nullptr;
    if (libusb_init(&context) != LIBUSB_SUCCESS || context == nullptr)
        return nullptr;
    return std::unique_ptr<UsbIo>(new LibusbIo(context));
}

}  // namespace torne::usb
