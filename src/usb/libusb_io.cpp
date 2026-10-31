// SPDX-License-Identifier: GPL-2.0-only
#include "usb/libusb_io.h"

#include <libusb.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

#if defined(__linux__) || defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#endif

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

TransferCause cause_for_transfer_status(libusb_transfer_status status) noexcept
{
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED: return TransferCause::none;
    case LIBUSB_TRANSFER_TIMED_OUT: return TransferCause::timeout;
    case LIBUSB_TRANSFER_STALL: return TransferCause::pipe;
    case LIBUSB_TRANSFER_NO_DEVICE: return TransferCause::no_device;
    case LIBUSB_TRANSFER_OVERFLOW: return TransferCause::overflow;
    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_CANCELLED:
    default: return TransferCause::other;
    }
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

#if defined(__linux__) || defined(__ANDROID__)
int duplicate_fd(int fd) noexcept
{
#if defined(F_DUPFD_CLOEXEC)
    const int cloexec_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (cloexec_fd >= 0) return cloexec_fd;
#endif
    const int duplicated = ::dup(fd);
    if (duplicated < 0) return -1;
#if defined(FD_CLOEXEC)
    const int flags = fcntl(duplicated, F_GETFD);
    if (flags >= 0) (void)fcntl(duplicated, F_SETFD, flags | FD_CLOEXEC);
#endif
    return duplicated;
}
#endif

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

#if defined(__linux__) || defined(__ANDROID__)
    OpenOutcome open_fd(int fd, DeviceSummary& summary) noexcept override
    {
        if (context_ == nullptr || handle_ != nullptr || fd < 0)
            return OpenOutcome{OpenState::invalid, TransferCause::invalid};
        const int retained_fd = duplicate_fd(fd);
        if (retained_fd < 0)
            return OpenOutcome{OpenState::io, TransferCause::io};

        libusb_device_handle* wrapped = nullptr;
        const int wrap_status = libusb_wrap_sys_device(
            context_, static_cast<std::intptr_t>(retained_fd), &wrapped);
        if (wrap_status != LIBUSB_SUCCESS || wrapped == nullptr) {
            ::close(retained_fd);
            return open_outcome_for_error(wrap_status);
        }

        libusb_device* device = libusb_get_device(wrapped);
        if (device == nullptr) {
            libusb_close(wrapped);
            ::close(retained_fd);
            return OpenOutcome{OpenState::io, TransferCause::io};
        }
        libusb_device_descriptor descriptor{};
        const int descriptor_status =
            libusb_get_device_descriptor(device, &descriptor);
        if (descriptor_status != LIBUSB_SUCCESS) {
            libusb_close(wrapped);
            ::close(retained_fd);
            return open_outcome_for_error(descriptor_status);
        }
        if (descriptor.idVendor != 0x054cU || descriptor.idProduct != 0x04b2U) {
            libusb_close(wrapped);
            ::close(retained_fd);
            return OpenOutcome{OpenState::unsupported, TransferCause::unsupported};
        }

        int configuration = 0;
        const int configuration_status =
            libusb_get_configuration(wrapped, &configuration);
        if (configuration_status != LIBUSB_SUCCESS) {
            libusb_close(wrapped);
            ::close(retained_fd);
            return open_outcome_for_error(configuration_status);
        }
        if (configuration != 1) {
            libusb_close(wrapped);
            ::close(retained_fd);
            return OpenOutcome{OpenState::unsupported, TransferCause::unsupported};
        }
        libusb_config_descriptor* active = nullptr;
        const int active_status =
            libusb_get_active_config_descriptor(device, &active);
        if (active_status != LIBUSB_SUCCESS) {
            libusb_close(wrapped);
            ::close(retained_fd);
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
        handle_ = wrapped;
        retained_fd_ = retained_fd;
        return OpenOutcome{OpenState::ok, TransferCause::none};
    }
#else
    OpenOutcome open_fd(int fd, DeviceSummary& summary) noexcept override
    {
        (void)fd;
        (void)summary;
        return OpenOutcome{OpenState::unsupported, TransferCause::unsupported};
    }
#endif

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

    int async_bulk_in_submit(std::uint8_t endpoint, std::uint8_t* data,
                             std::size_t length, std::uint32_t timeout_ms,
                             std::uint64_t* id) noexcept override
    {
        if (handle_ == nullptr || data == nullptr || id == nullptr ||
            (endpoint & 0x80U) == 0U ||
            length > static_cast<std::size_t>(INT_MAX))
            return -1;
        std::unique_ptr<AsyncTransfer> item(new AsyncTransfer());
        item->id = next_async_id_++;
        item->data = data;
        item->length = length;
        item->transfer = libusb_alloc_transfer(0);
        if (item->transfer == nullptr) return -1;
        libusb_fill_bulk_transfer(
            item->transfer, handle_, endpoint, data, static_cast<int>(length),
            &LibusbIo::async_callback, item.get(), timeout_ms);
        const int status = libusb_submit_transfer(item->transfer);
        if (status != LIBUSB_SUCCESS) {
            libusb_free_transfer(item->transfer);
            item->transfer = nullptr;
            return -1;
        }
        *id = item->id;
        async_[item->id] = std::move(item);
        return 0;
    }

    int async_bulk_cancel_all() noexcept override
    {
        int result = 0;
        for (auto& entry : async_) {
            if (!entry.second->completed.load(std::memory_order_acquire) &&
                libusb_cancel_transfer(entry.second->transfer) != LIBUSB_SUCCESS)
                result = -1;
        }
        return result;
    }

    int async_bulk_poll(AsyncBulkEvent& event) noexcept override
    {
        if (context_ == nullptr) return -1;
        timeval timeout{0, 0};
        (void)libusb_handle_events_timeout_completed(context_, &timeout, nullptr);
        for (auto item = async_.begin(); item != async_.end(); ++item) {
            AsyncTransfer& transfer = *item->second;
            if (!transfer.completed.load(std::memory_order_acquire)) continue;
            event.id = transfer.id;
            event.data = transfer.data;
            event.length = transfer.length;
            event.transfer = transfer.outcome;
            libusb_free_transfer(transfer.transfer);
            async_.erase(item);
            return 1;
        }
        return 0;
    }

    std::size_t async_bulk_active() const noexcept override
    {
        return async_.size();
    }

    int async_bulk_drain(std::uint32_t timeout_ms) noexcept override
    {
        if (async_.empty()) return 0;
        for (auto& entry : async_) {
            if (!entry.second->completed.load(std::memory_order_acquire))
                (void)libusb_cancel_transfer(entry.second->transfer);
        }
        const int attempts = static_cast<int>(timeout_ms / 10U) + 1;
        for (int attempt = 0; attempt < attempts && !async_.empty(); ++attempt) {
            timeval timeout{0, 10000};
            if (context_ != nullptr)
                (void)libusb_handle_events_timeout_completed(context_, &timeout, nullptr);
            for (auto item = async_.begin(); item != async_.end();) {
                if (!item->second->completed.load(std::memory_order_acquire)) {
                    ++item;
                    continue;
                }
                libusb_free_transfer(item->second->transfer);
                item = async_.erase(item);
            }
        }
        return async_.empty() ? 0 : -1;
    }

    void release_interface(std::uint8_t number) noexcept override
    {
        if (handle_ != nullptr)
            (void)libusb_release_interface(handle_, static_cast<int>(number));
    }

    void close() noexcept override
    {
        if (unrecovered_) return;
        if (async_bulk_drain(100U) != 0) {
            // A callback has not completed. Keep the handle/context and
            // transfer/user_data storage alive rather than freeing it first.
            (void)new std::map<std::uint64_t, std::unique_ptr<AsyncTransfer>>(
                std::move(async_));
            unrecovered_ = true;
            return;
        }
        if (handle_ != nullptr) {
            libusb_close(handle_);
            handle_ = nullptr;
        }
#if defined(__linux__) || defined(__ANDROID__)
        if (retained_fd_ >= 0) {
            ::close(retained_fd_);
            retained_fd_ = -1;
        }
#endif
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

    struct AsyncTransfer final {
        std::uint64_t id = 0U;
        libusb_transfer* transfer = nullptr;
        std::uint8_t* data = nullptr;
        std::size_t length = 0U;
        // The callback publishes the outcome with a release store and poll/
        // drain acquire it before freeing the item or libusb_transfer. This
        // keeps a synchronous control transfer that runs libusb callbacks from
        // exposing a half-written outcome to the TS poll thread.
        std::atomic<bool> completed{false};
        TransferOutcome outcome{};
    };

    static void LIBUSB_CALL async_callback(libusb_transfer* transfer) noexcept
    {
        auto* item = static_cast<AsyncTransfer*>(transfer->user_data);
        if (item == nullptr) return;
        item->outcome.cause = cause_for_transfer_status(transfer->status);
        item->outcome.transferred = transfer->actual_length;
        if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
            item->outcome.state =
                transfer->actual_length == static_cast<int>(item->length)
                    ? TransferState::complete
                    : TransferState::short_transfer;
        } else {
            item->outcome.state = TransferState::failed;
        }
        item->completed.store(true, std::memory_order_release);
    }

    libusb_context* context_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
#if defined(__linux__) || defined(__ANDROID__)
    int retained_fd_ = -1;
#endif
    std::map<std::uint64_t, std::unique_ptr<AsyncTransfer>> async_;
    std::uint64_t next_async_id_ = 1U;
    bool unrecovered_ = false;
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

std::unique_ptr<UsbIo> create_libusb_io_fd() noexcept
{
#if defined(__linux__) || defined(__ANDROID__)
    libusb_init_option option{};
    option.option = LIBUSB_OPTION_NO_DEVICE_DISCOVERY;
    option.value.ival = 1;
    libusb_context* context = nullptr;
    if (libusb_init_context(&context, &option, 1) != LIBUSB_SUCCESS ||
        context == nullptr)
        return nullptr;
    return std::unique_ptr<UsbIo>(new LibusbIo(context));
#else
    return create_libusb_io();
#endif
}

}  // namespace torne::usb
