// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/usb.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <chrono>
#include <climits>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include <libusb.h>

#include <dtv/internal/result.hpp>

#include "usb-transfer-backend.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dtv::internal {

namespace {

thread_local const dtv_runtime_clock *active_operation_clock = nullptr;
thread_local const dtv_cancel *active_operation_cancel = nullptr;
thread_local std::uint64_t active_operation_deadline_ns = 0;
thread_local bool lifecycle_cleanup_io = false;
thread_local UsbOperationTerminalReason last_operation_terminal_reason =
    UsbOperationTerminalReason::none;

class LibusbTransferBackend final : public UsbTransferBackend {
public:
    int init_context(libusb_context **context) override { return libusb_init(context); }
    libusb_transfer *allocate_transfer() override { return libusb_alloc_transfer(0); }
    void free_transfer(libusb_transfer *transfer) override { libusb_free_transfer(transfer); }
    int submit_transfer(libusb_transfer *transfer) override { return libusb_submit_transfer(transfer); }
    int cancel_transfer(libusb_transfer *transfer) override
    {
        return libusb_cancel_transfer(transfer);
    }
    int handle_events(libusb_context *context, timeval *timeout) override
    {
        return libusb_handle_events_timeout(context, timeout);
    }
    int get_device_descriptor(libusb_device *device,
                              libusb_device_descriptor *descriptor) override
    {
        return libusb_get_device_descriptor(device, descriptor);
    }
    std::uint8_t get_bus_number(libusb_device *device) override
    {
        return libusb_get_bus_number(device);
    }
    std::uint8_t get_device_address(libusb_device *device) override
    {
        return libusb_get_device_address(device);
    }
    int get_active_config_descriptor(libusb_device *device,
                                     libusb_config_descriptor **descriptor) override
    {
        return libusb_get_active_config_descriptor(device, descriptor);
    }
    int get_config_descriptor(libusb_device *device, std::uint8_t index,
                              libusb_config_descriptor **descriptor) override
    {
        return libusb_get_config_descriptor(device, index, descriptor);
    }
    void free_config_descriptor(libusb_config_descriptor *descriptor) override
    {
        libusb_free_config_descriptor(descriptor);
    }
    int open_device(libusb_device *device, libusb_device_handle **handle) override
    {
        return libusb_open(device, handle);
    }
    int claim_interface(libusb_device_handle *handle, int interface_number) override
    {
        return libusb_claim_interface(handle, interface_number);
    }
    int set_interface_alt_setting(libusb_device_handle *handle, int interface_number,
                                  int alternate_setting) override
    {
        return libusb_set_interface_alt_setting(handle, interface_number, alternate_setting);
    }
    int wrap_sys_device(libusb_context *context, std::intptr_t descriptor,
                        libusb_device_handle **handle) override
    {
        return libusb_wrap_sys_device(context, descriptor, handle);
    }
    libusb_device *get_device(libusb_device_handle *handle) override
    {
        return libusb_get_device(handle);
    }
    libusb_device *ref_device(libusb_device *device) override
    {
        return libusb_ref_device(device);
    }
    void unref_device(libusb_device *device) override { libusb_unref_device(device); }
    int release_interface(libusb_device_handle *handle, int interface_number) override
    {
        return libusb_release_interface(handle, interface_number);
    }
    void close_handle(libusb_device_handle *handle) override { libusb_close(handle); }
    void exit_context(libusb_context *context) override { libusb_exit(context); }
};

}

std::shared_ptr<UsbTransferBackend> make_libusb_transfer_backend()
{
    return std::make_shared<LibusbTransferBackend>();
}

struct UsbTransferRequest;

struct UsbContext::State {
    libusb_context *context = nullptr;
    std::shared_ptr<UsbTransferBackend> backend = make_libusb_transfer_backend();
    dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    std::mutex requests_mutex;
    std::condition_variable requests_condition;
    std::unordered_map<libusb_transfer *, std::shared_ptr<UsbTransferRequest>> requests;
    std::thread event_thread;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_condition;
    std::deque<std::function<void()>> lifecycle_jobs;
    std::thread lifecycle_thread;
    bool stop_event_thread = false;
    bool stop_lifecycle_thread = false;
    void mark_callbacks_returned();

    void start_event_thread()
    {
        event_thread = std::thread([this] {
            while (true) {
                {
                    std::lock_guard lock(requests_mutex);
                    if (stop_event_thread) return;
                }
                timeval timeout{0, 100000};
                backend->handle_events(context, &timeout);
                mark_callbacks_returned();
            }
        });
    }

    void start_lifecycle_thread()
    {
        lifecycle_thread = std::thread([this] {
            while (true) {
                std::function<void()> job;
                {
                    std::unique_lock lock(lifecycle_mutex);
                    lifecycle_condition.wait(lock, [&] {
                        return stop_lifecycle_thread || !lifecycle_jobs.empty();
                    });
                    if (lifecycle_jobs.empty() && stop_lifecycle_thread) return;
                    job = std::move(lifecycle_jobs.front());
                    lifecycle_jobs.pop_front();
                }
                try { job(); } catch (...) {}
            }
        });
    }

    ~State()
    {
        if (lifecycle_thread.joinable()) {
            {
                std::lock_guard lock(lifecycle_mutex);
                stop_lifecycle_thread = true;
            }
            lifecycle_condition.notify_all();
            lifecycle_thread.join();
        }
        if (event_thread.joinable()) {
            {
                std::lock_guard lock(requests_mutex);
                stop_event_thread = true;
            }
            event_thread.join();
        }
        if (context != nullptr) {
            backend->exit_context(context);
        }
    }
};

struct UsbDeviceSnapshot::State {
    std::shared_ptr<UsbContext::State> context;
    libusb_device *device = nullptr;
    ~State()
    {
        if (device != nullptr) {
            context->backend->unref_device(device);
        }
    }
};

struct UsbHandleLease {
    std::shared_ptr<UsbContext::State> context;
    libusb_device_handle *handle = nullptr;
    std::vector<int> claimed_interfaces;
    int owned_descriptor = -1;
    std::atomic<bool> closing{false};
    std::atomic<UsbCloseStatus> close_status{UsbCloseStatus::open};
    std::mutex close_mutex;
    std::condition_variable close_condition;
    bool close_job_started = false;
    bool close_job_done = false;
    dtv_internal_result close_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);

    ~UsbHandleLease()
    {
        if (handle != nullptr) {
            for (auto interface = claimed_interfaces.rbegin();
                 interface != claimed_interfaces.rend(); ++interface) {
                context->backend->release_interface(handle, *interface);
            }
            context->backend->close_handle(handle);
        }
#if defined(__linux__)
        if (owned_descriptor >= 0) {
            close(owned_descriptor);
        }
#endif
    }
};

struct UsbDevice::State {
    struct IoBinding {
        std::shared_ptr<UsbHandleLease> handle;
        const dtv_runtime_clock *clock = nullptr;
    } binding;
    dtv_internal_result open_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    dtv_runtime_clock clock{};
    dtv_usb_io io{};
};

struct UsbTransferRequest {
    enum class TerminalReason {
        none, completed, cancelled, endpoint_cancelled, deadline, closing, disconnected
    };
    std::shared_ptr<UsbHandleLease> handle;
    libusb_transfer *transfer = nullptr;
    std::vector<unsigned char> buffer;
    unsigned char *input_target = nullptr;
    std::size_t input_capacity = 0;
    std::size_t input_offset = 0;
    std::size_t input_length = 0;
    bool input = false;
    bool output = false;
    bool report_length = false;
    std::mutex mutex;
    std::condition_variable completed_condition;
    bool completed = false;
    bool callback_returned = false;
    libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
    int actual_length = 0;
    TerminalReason terminal_reason = TerminalReason::none;
    const dtv_runtime_clock *clock = nullptr;
    const dtv_cancel *cancel = nullptr;
    std::uint64_t deadline_ns = 0;
    bool waiter_returned = false;

    ~UsbTransferRequest()
    {
        if (transfer != nullptr) {
            handle->context->backend->free_transfer(transfer);
        }
    }
};

void UsbContext::State::mark_callbacks_returned()
{
    std::lock_guard lock(requests_mutex);
    for (auto it = requests.begin(); it != requests.end();) {
        const auto &request = it->second;
        {
            std::lock_guard request_lock(request->mutex);
            if (!request->completed) {
                ++it;
                continue;
            }
            request->callback_returned = true;
            if (request->terminal_reason == UsbTransferRequest::TerminalReason::none) {
                request->terminal_reason = UsbTransferRequest::TerminalReason::completed;
            }
            if (request->waiter_returned) {
                it = requests.erase(it);
                requests_condition.notify_all();
                continue;
            }
        }
        request->completed_condition.notify_one();
        ++it;
    }
}

namespace {

dtv_internal_result make_result(std::int32_t code, std::uint32_t detail = 0)
{
    return dtv_internal_result_make(code, detail);
}

dtv_internal_result usb_result(int code)
{
    if (code == LIBUSB_ERROR_TIMEOUT) {
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT, static_cast<std::uint32_t>(-code));
    }
    return make_result(DTV_INTERNAL_RESULT_USB_IO, static_cast<std::uint32_t>(-code));
}

enum class TimeoutConversion {
    ready,
    no_wait,
    invalid
};

TimeoutConversion transfer_timeout(dtv_timeout timeout, const dtv_runtime_clock *clock,
                                   unsigned int *milliseconds,
                                   dtv_internal_result *failure)
{
    auto timeout_ms = timeout.timeout_ms;
    if (timeout.operation_deadline_ns != 0 && timeout.operation_deadline_ns != UINT64_MAX &&
        clock != nullptr && clock->monotonic_now_ns != nullptr) {
        uint64_t now = 0;
        try {
            now = clock->monotonic_now_ns(clock->user);
        } catch (...) {
            *failure = exception_result();
            return TimeoutConversion::invalid;
        }
        if (now >= timeout.operation_deadline_ns) {
            return TimeoutConversion::no_wait;
        }
        const auto remaining_ns = timeout.operation_deadline_ns - now;
        const auto remaining_ms = remaining_ns / 1000000 + (remaining_ns % 1000000 != 0 ? 1 : 0);
        timeout_ms = (std::min)(timeout_ms, remaining_ms);
    }
    if (timeout_ms == 0) {
        return TimeoutConversion::no_wait;
    }
    if (timeout_ms == UINT64_MAX) {
        *milliseconds = 0;
        return TimeoutConversion::ready;
    }
    if (timeout_ms > UINT_MAX) {
        return TimeoutConversion::invalid;
    }
    *milliseconds = static_cast<unsigned int>(timeout_ms);
    return TimeoutConversion::ready;
}

dtv_internal_result invalid()
{
    return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
}

dtv_internal_result no_context()
{
    return make_result(DTV_INTERNAL_RESULT_USB_IO,
                       static_cast<std::uint32_t>(-LIBUSB_ERROR_OTHER));
}

void transfer_completed(libusb_transfer *transfer)
{
    auto *request = static_cast<UsbTransferRequest *>(transfer->user_data);
    {
        std::lock_guard lock(request->mutex);
        request->status = transfer->status;
        request->actual_length = transfer->actual_length;
        request->completed = true;
    }
    request->completed_condition.notify_one();
}

int transfer_error(libusb_transfer_status status)
{
    switch (status) {
    case LIBUSB_TRANSFER_TIMED_OUT:
        return LIBUSB_ERROR_TIMEOUT;
    case LIBUSB_TRANSFER_CANCELLED:
        /* A backend may cancel the same-endpoint cohort; a peer has no own cancel reason. */
        return LIBUSB_ERROR_INTERRUPTED;
    case LIBUSB_TRANSFER_STALL:
        return LIBUSB_ERROR_PIPE;
    case LIBUSB_TRANSFER_NO_DEVICE:
        return LIBUSB_ERROR_NO_DEVICE;
    case LIBUSB_TRANSFER_OVERFLOW:
        return LIBUSB_ERROR_OVERFLOW;
    case LIBUSB_TRANSFER_ERROR:
    default:
        return LIBUSB_ERROR_OTHER;
    }
}

dtv_internal_result wait_for_transfer(const std::shared_ptr<UsbTransferRequest> &request,
                                     std::size_t *out_actual_length)
{
    auto &context = *request->handle->context;
    int submit_result = LIBUSB_SUCCESS;
    {
        std::lock_guard lock(context.requests_mutex);
        if (request->handle->closing.load() && !lifecycle_cleanup_io) {
            return make_result(DTV_INTERNAL_RESULT_NOT_READY);
        }
        context.requests.emplace(request->transfer, request);
        submit_result = context.backend->submit_transfer(request->transfer);
    }
    if (submit_result != LIBUSB_SUCCESS) {
        {
            std::lock_guard lock(context.requests_mutex);
            context.requests.erase(request->transfer);
            context.requests_condition.notify_all();
        }
        return usb_result(submit_result);
    }

    libusb_transfer_status status;
    int actual_length;
    bool cancelled = false;
    UsbTransferRequest::TerminalReason terminal_reason =
        UsbTransferRequest::TerminalReason::none;
    {
        std::unique_lock lock(request->mutex);
        while (!request->completed || !request->callback_returned) {
            request->completed_condition.wait_for(lock, std::chrono::milliseconds(5));
            if (request->completed && request->callback_returned) break;
            if (request->terminal_reason != UsbTransferRequest::TerminalReason::none) {
                request->waiter_returned = true;
                cancelled = true;
                break;
            }
            bool is_cancelled = false;
            if (request->cancel != nullptr && request->clock != nullptr &&
                request->clock->is_cancelled != nullptr) {
                try {
                    std::uint32_t value = 0;
                    const auto result = request->clock->is_cancelled(
                        request->clock->user, request->cancel, &value);
                    is_cancelled = result.code == DTV_INTERNAL_RESULT_OK && value != 0;
                } catch (...) {}
            }
            bool deadline = false;
            if (request->deadline_ns != 0 && request->deadline_ns != UINT64_MAX &&
                request->clock != nullptr && request->clock->monotonic_now_ns != nullptr) {
                try {
                    deadline = request->clock->monotonic_now_ns(request->clock->user) >=
                               request->deadline_ns;
                } catch (...) {}
            }
            if (is_cancelled || deadline) {
                request->terminal_reason = is_cancelled
                    ? UsbTransferRequest::TerminalReason::cancelled
                    : UsbTransferRequest::TerminalReason::deadline;
                request->waiter_returned = true;
                cancelled = true;
                break;
            }
        }
        if (cancelled) {
            status = LIBUSB_TRANSFER_CANCELLED;
            actual_length = 0;
        } else {
            status = request->status;
            actual_length = request->actual_length;
        }
        terminal_reason = request->terminal_reason;
    }
    if (cancelled) {
        last_operation_terminal_reason = terminal_reason == UsbTransferRequest::TerminalReason::cancelled
            ? UsbOperationTerminalReason::cancelled
            : terminal_reason == UsbTransferRequest::TerminalReason::deadline
                ? UsbOperationTerminalReason::deadline : UsbOperationTerminalReason::closing;
        if (terminal_reason != UsbTransferRequest::TerminalReason::closing) {
            context.backend->cancel_transfer(request->transfer);
        }
        return terminal_reason == UsbTransferRequest::TerminalReason::closing
            ? make_result(DTV_INTERNAL_RESULT_NOT_READY)
            : make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    if (terminal_reason == UsbTransferRequest::TerminalReason::closing) {
        last_operation_terminal_reason = UsbOperationTerminalReason::closing;
        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    }
    if (terminal_reason == UsbTransferRequest::TerminalReason::none ||
        terminal_reason == UsbTransferRequest::TerminalReason::completed) {
        if (status == LIBUSB_TRANSFER_TIMED_OUT) {
            bool operation_deadline_expired = false;
            if (request->deadline_ns != 0 && request->deadline_ns != UINT64_MAX &&
                request->clock != nullptr && request->clock->monotonic_now_ns != nullptr) {
                try {
                    operation_deadline_expired = request->clock->monotonic_now_ns(
                        request->clock->user) >= request->deadline_ns;
                } catch (...) {}
            }
            terminal_reason = operation_deadline_expired
                ? UsbTransferRequest::TerminalReason::deadline
                : UsbTransferRequest::TerminalReason::completed;
            last_operation_terminal_reason = operation_deadline_expired
                ? UsbOperationTerminalReason::deadline
                : UsbOperationTerminalReason::transfer_timeout;
        } else if (status == LIBUSB_TRANSFER_CANCELLED) {
            terminal_reason = UsbTransferRequest::TerminalReason::endpoint_cancelled;
            last_operation_terminal_reason = UsbOperationTerminalReason::endpoint_cancelled;
        } else {
            last_operation_terminal_reason = UsbOperationTerminalReason::completed;
        }
    } else {
        last_operation_terminal_reason = terminal_reason == UsbTransferRequest::TerminalReason::disconnected
            ? UsbOperationTerminalReason::disconnected : UsbOperationTerminalReason::completed;
    }
    {
        std::lock_guard lock(context.requests_mutex);
        context.requests.erase(request->transfer);
        context.requests_condition.notify_all();
    }

    const auto transferred = (std::max)(actual_length, 0);
    if (out_actual_length != nullptr) {
        *out_actual_length = static_cast<std::size_t>(transferred);
    }
    if (request->input && request->input_target != nullptr &&
        terminal_reason != UsbTransferRequest::TerminalReason::cancelled &&
        terminal_reason != UsbTransferRequest::TerminalReason::deadline &&
        terminal_reason != UsbTransferRequest::TerminalReason::endpoint_cancelled &&
        terminal_reason != UsbTransferRequest::TerminalReason::closing) {
        const auto copied = (std::min)(static_cast<std::size_t>(transferred),
                                       request->input_capacity);
        std::copy_n(request->buffer.data() + request->input_offset, copied,
                    request->input_target);
    }
    if (status != LIBUSB_TRANSFER_COMPLETED) {
        return usb_result(transfer_error(status));
    }
    if (request->output && static_cast<std::size_t>(transferred) != request->input_length) {
        return make_result(DTV_INTERNAL_RESULT_USB_IO,
                           static_cast<std::uint32_t>(transferred));
    }
    return make_result(DTV_INTERNAL_RESULT_OK,
                       request->report_length ? static_cast<std::uint32_t>(transferred) : 0);
}

std::shared_ptr<UsbTransferRequest> make_transfer_request(
    const std::shared_ptr<UsbHandleLease> &handle, std::size_t buffer_size)
{
    auto request = std::make_shared<UsbTransferRequest>();
    request->handle = handle;
    request->buffer.resize(buffer_size);
    request->transfer = handle->context->backend->allocate_transfer();
    if (request->transfer == nullptr) {
        throw std::bad_alloc();
    }
    return request;
}

void configure_operation(UsbTransferRequest &request, UsbDevice::State::IoBinding *binding,
                         dtv_timeout timeout)
{
    request.clock = active_operation_clock != nullptr ? active_operation_clock : binding->clock;
    request.cancel = active_operation_cancel;
    request.deadline_ns = timeout.operation_deadline_ns;
    if (active_operation_deadline_ns != 0 &&
        (request.deadline_ns == 0 || active_operation_deadline_ns < request.deadline_ns)) {
        request.deadline_ns = active_operation_deadline_ns;
    }
}

bool regular_io_allowed(const UsbDevice::State::IoBinding *binding)
{
    return binding != nullptr && binding->handle && binding->handle->handle != nullptr &&
           (!binding->handle->closing.load() || lifecycle_cleanup_io);
}

void record_no_wait_reason(UsbDevice::State::IoBinding *binding, dtv_timeout timeout)
{
    last_operation_terminal_reason = UsbOperationTerminalReason::none;
    const auto *clock = active_operation_clock != nullptr
        ? active_operation_clock : binding->clock;
    const auto deadline = active_operation_deadline_ns != 0
        ? (timeout.operation_deadline_ns == 0
               ? active_operation_deadline_ns
               : (std::min)(timeout.operation_deadline_ns, active_operation_deadline_ns))
        : timeout.operation_deadline_ns;
    if (deadline != 0 && deadline != UINT64_MAX && clock != nullptr &&
        clock->monotonic_now_ns != nullptr) {
        try {
            if (clock->monotonic_now_ns(clock->user) >= deadline) {
                last_operation_terminal_reason = UsbOperationTerminalReason::deadline;
            }
        } catch (...) {}
    }
    if (last_operation_terminal_reason != UsbOperationTerminalReason::deadline) {
        last_operation_terminal_reason = UsbOperationTerminalReason::none;
    }
}

void begin_handle_close(const std::shared_ptr<UsbHandleLease> &handle)
{
    if (!handle || handle->closing.exchange(true)) return;
    handle->close_status.store(UsbCloseStatus::closing);
    auto &context = *handle->context;
    std::vector<libusb_transfer *> transfers;
    {
        std::lock_guard lock(context.requests_mutex);
        for (const auto &[transfer, request] : context.requests) {
            if (request->handle != handle) continue;
            std::lock_guard request_lock(request->mutex);
            if (request->terminal_reason == UsbTransferRequest::TerminalReason::none) {
                request->terminal_reason = UsbTransferRequest::TerminalReason::closing;
                request->waiter_returned = true;
                request->completed_condition.notify_one();
                transfers.push_back(transfer);
            }
        }
    }
    for (auto *transfer : transfers) context.backend->cancel_transfer(transfer);
}

bool handle_drained(const std::shared_ptr<UsbHandleLease> &handle)
{
    const auto &context = *handle->context;
    return std::none_of(context.requests.begin(), context.requests.end(), [&](const auto &entry) {
        return entry.second->handle == handle;
    });
}

bool closing_requests_drained(const UsbContext::State &context)
{
    return std::none_of(context.requests.begin(), context.requests.end(), [](const auto &entry) {
        return entry.second->handle->closing.load();
    });
}

dtv_internal_result control_transfer(void *user, std::uint8_t request_type,
                            std::uint8_t request, std::uint16_t value,
                            std::uint16_t index, dtv_buffer *data,
                            dtv_timeout timeout)
{
    auto *binding = static_cast<UsbDevice::State::IoBinding *>(user);
    unsigned int milliseconds;
    dtv_internal_result failure = invalid();
    if (binding != nullptr && binding->handle && binding->handle->closing.load() &&
        !lifecycle_cleanup_io) {
        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    }
    if (!regular_io_allowed(binding) || data == nullptr ||
        data->capacity > UINT16_MAX ||
        data->size > data->capacity ||
        (data->capacity != 0 && data->data == nullptr)) {
        return invalid();
    }
    const auto timeout_conversion = transfer_timeout(timeout,
                                                     active_operation_clock != nullptr
                                                         ? active_operation_clock : binding->clock,
                                                     &milliseconds, &failure);
    if (timeout_conversion == TimeoutConversion::invalid) {
        return failure;
    }
    if (timeout_conversion == TimeoutConversion::no_wait) {
        record_no_wait_reason(binding, timeout);
        data->size = 0;
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    const bool input = (request_type & LIBUSB_ENDPOINT_IN) != 0;
    const auto length = input ? data->capacity : data->size;
    try {
        auto request_state = make_transfer_request(binding->handle,
            LIBUSB_CONTROL_SETUP_SIZE + static_cast<std::size_t>(length));
        configure_operation(*request_state, binding, timeout);
        request_state->input = input;
        request_state->output = !input;
        request_state->input_length = length;
        request_state->input_target = input ? data->data : nullptr;
        request_state->input_capacity = input ? data->capacity : 0;
        request_state->input_offset = LIBUSB_CONTROL_SETUP_SIZE;
        libusb_fill_control_setup(request_state->buffer.data(), request_type, request,
                                  value, index, static_cast<std::uint16_t>(length));
        if (!input && length != 0) {
            std::copy_n(data->data, static_cast<std::size_t>(length),
                        request_state->buffer.data() + LIBUSB_CONTROL_SETUP_SIZE);
        }
        libusb_fill_control_transfer(request_state->transfer, binding->handle->handle,
                                     request_state->buffer.data(), transfer_completed,
                                     request_state.get(), milliseconds);
        std::size_t transferred = 0;
        auto result = wait_for_transfer(request_state, &transferred);
        if (request_state->status == LIBUSB_TRANSFER_COMPLETED) {
            data->size = transferred;
        } else if (current_usb_terminal_reason() == UsbOperationTerminalReason::cancelled ||
                   current_usb_terminal_reason() == UsbOperationTerminalReason::deadline ||
                   current_usb_terminal_reason() == UsbOperationTerminalReason::closing) {
            data->size = 0;
        }
        return result;
    } catch (...) {
        return exception_result();
    }
}

dtv_internal_result bulk_out_transfer(void *user, std::uint8_t endpoint,
                             dtv_bytes data, dtv_timeout timeout)
{
    auto *binding = static_cast<UsbDevice::State::IoBinding *>(user);
    unsigned int milliseconds;
    dtv_internal_result failure = invalid();
    if (binding != nullptr && binding->handle && binding->handle->closing.load() &&
        !lifecycle_cleanup_io) {
        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    }
    if (!regular_io_allowed(binding) || data.size > INT_MAX ||
        (data.size != 0 && data.data == nullptr)) {
        return invalid();
    }
    const auto timeout_conversion = transfer_timeout(timeout,
                                                     active_operation_clock != nullptr
                                                         ? active_operation_clock : binding->clock,
                                                     &milliseconds, &failure);
    if (timeout_conversion == TimeoutConversion::invalid) {
        return failure;
    }
    if (timeout_conversion == TimeoutConversion::no_wait) {
        record_no_wait_reason(binding, timeout);
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    try {
        auto request_state = make_transfer_request(binding->handle, data.size);
        configure_operation(*request_state, binding, timeout);
        if (data.size != 0) {
            std::copy_n(data.data, static_cast<std::size_t>(data.size),
                        request_state->buffer.data());
        }
        request_state->output = true;
        request_state->report_length = true;
        request_state->input_length = static_cast<std::size_t>(data.size);
        libusb_fill_bulk_transfer(request_state->transfer, binding->handle->handle,
                                  endpoint, request_state->buffer.data(),
                                  static_cast<int>(data.size), transfer_completed,
                                  request_state.get(), milliseconds);
        auto result = wait_for_transfer(request_state, nullptr);
        if (result.code == DTV_INTERNAL_RESULT_OK && result.detail != data.size) {
            return make_result(DTV_INTERNAL_RESULT_USB_IO, result.detail);
        }
        return result;
    } catch (...) {
        return exception_result();
    }
}

dtv_internal_result input_transfer(void *user, std::uint8_t endpoint,
                          dtv_buffer *data, dtv_timeout timeout,
                          bool interrupt)
{
    auto *binding = static_cast<UsbDevice::State::IoBinding *>(user);
    unsigned int milliseconds;
    dtv_internal_result failure = invalid();
    if (binding != nullptr && binding->handle && binding->handle->closing.load() &&
        !lifecycle_cleanup_io) {
        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    }
    if (!regular_io_allowed(binding) || data == nullptr ||
        data->capacity > INT_MAX ||
        (data->capacity != 0 && data->data == nullptr)) {
        return invalid();
    }
    const auto timeout_conversion = transfer_timeout(timeout,
                                                     active_operation_clock != nullptr
                                                         ? active_operation_clock : binding->clock,
                                                     &milliseconds, &failure);
    if (timeout_conversion == TimeoutConversion::invalid) {
        return failure;
    }
    if (timeout_conversion == TimeoutConversion::no_wait) {
        record_no_wait_reason(binding, timeout);
        data->size = 0;
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    try {
        auto request_state = make_transfer_request(binding->handle, data->capacity);
        configure_operation(*request_state, binding, timeout);
        request_state->input = true;
        request_state->input_target = data->data;
        request_state->input_capacity = data->capacity;
        request_state->input_length = data->capacity;
        if (interrupt) {
            libusb_fill_interrupt_transfer(request_state->transfer, binding->handle->handle,
                                           endpoint, request_state->buffer.data(),
                                           static_cast<int>(data->capacity), transfer_completed,
                                           request_state.get(), milliseconds);
        } else {
            libusb_fill_bulk_transfer(request_state->transfer, binding->handle->handle,
                                      endpoint, request_state->buffer.data(),
                                      static_cast<int>(data->capacity), transfer_completed,
                                      request_state.get(), milliseconds);
        }
        std::size_t transferred = 0;
        auto result = wait_for_transfer(request_state, &transferred);
        data->size = transferred;
        return result;
    } catch (...) {
        return exception_result();
    }
}

dtv_internal_result bulk_in_transfer(void *user, std::uint8_t endpoint,
                            dtv_buffer *data, dtv_timeout timeout)
{
    return input_transfer(user, endpoint, data, timeout, false);
}

dtv_internal_result interrupt_in_transfer(void *user, std::uint8_t endpoint,
                                 dtv_buffer *data, dtv_timeout timeout)
{
    return input_transfer(user, endpoint, data, timeout, true);
}

dtv_usb_io make_io(void *user, const dtv_runtime_clock *clock)
{
    return {user, control_transfer, bulk_out_transfer,
            bulk_in_transfer, interrupt_in_transfer, clock};
}

}

UsbOperationTerminalReason current_usb_terminal_reason()
{
    return last_operation_terminal_reason;
}

UsbOperationScope::UsbOperationScope(const dtv_runtime_clock *clock, const dtv_cancel *cancel,
                                     std::uint64_t deadline_ns)
    : previous_clock_(active_operation_clock), previous_cancel_(active_operation_cancel),
      previous_deadline_ns_(active_operation_deadline_ns)
{
    active_operation_clock = clock;
    active_operation_cancel = cancel;
    active_operation_deadline_ns = deadline_ns;
}

UsbOperationScope::~UsbOperationScope()
{
    active_operation_clock = previous_clock_;
    active_operation_cancel = previous_cancel_;
    active_operation_deadline_ns = previous_deadline_ns_;
}

UsbContext::UsbContext() : state_(std::make_shared<State>())
{
    const int result = state_->backend->init_context(&state_->context);
    if (result != LIBUSB_SUCCESS) {
        state_->result = usb_result(result);
        return;
    }
    try {
        state_->start_event_thread();
        state_->start_lifecycle_thread();
    } catch (...) {
        state_->result = exception_result();
    }
}

UsbContext::~UsbContext() = default;
UsbContext::UsbContext(std::shared_ptr<State> state) : state_(std::move(state)) {}
UsbContext::UsbContext(UsbContext &&) noexcept = default;
UsbContext &UsbContext::operator=(UsbContext &&) noexcept = default;

dtv_internal_result UsbContext::result() const
{
    return state_ ? state_->result : no_context();
}

std::vector<UsbDeviceSnapshot> UsbContext::enumerate(dtv_internal_result *result) const
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
            if (state_->backend->get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS) {
                continue;
            }
            UsbDeviceSnapshot snapshot{};
            snapshot.vendor_id = descriptor.idVendor;
            snapshot.product_id = descriptor.idProduct;
            snapshot.device_version = descriptor.bcdDevice;
            snapshot.device_class = descriptor.bDeviceClass;
            snapshot.device_subclass = descriptor.bDeviceSubClass;
            snapshot.device_protocol = descriptor.bDeviceProtocol;
            snapshot.bus_number = state_->backend->get_bus_number(devices[i]);
            snapshot.device_address = state_->backend->get_device_address(devices[i]);
            snapshot.configuration_count = descriptor.bNumConfigurations;
            snapshot.candidate_id = static_cast<std::uint64_t>(i) + 1;
            std::uint8_t ports[8]{};
            const int port_count = libusb_get_port_numbers(devices[i], ports, sizeof(ports));
            if (port_count > 0) {
                snapshot.port_path.assign(ports, ports + port_count);
            }
            for (std::uint8_t config_index = 0; config_index < descriptor.bNumConfigurations;
                 ++config_index) {
                libusb_config_descriptor *config = nullptr;
                if (state_->backend->get_config_descriptor(devices[i], config_index, &config) !=
                    LIBUSB_SUCCESS) {
                    continue;
                }
                    std::unique_ptr<libusb_config_descriptor,
                                    std::function<void(libusb_config_descriptor *)>>
                    config_guard(config, [backend = state_->backend](auto *value) {
                        backend->free_config_descriptor(value);
                    });
                for (std::uint8_t interface_index = 0; interface_index < config->bNumInterfaces;
                     ++interface_index) {
                    const auto &interface = config->interface[interface_index];
                    for (int alt_index = 0; alt_index < interface.num_altsetting; ++alt_index) {
                        const auto &alt = interface.altsetting[alt_index];
                        UsbInterfaceSnapshot view{};
                        view.info.configuration = config->bConfigurationValue;
                        view.info.interface_number = alt.bInterfaceNumber;
                        view.info.alternate_setting = alt.bAlternateSetting;
                        view.info.class_code = alt.bInterfaceClass;
                        view.info.subclass_code = alt.bInterfaceSubClass;
                        view.info.protocol_code = alt.bInterfaceProtocol;
                        for (std::uint8_t endpoint_index = 0; endpoint_index < alt.bNumEndpoints;
                             ++endpoint_index) {
                            const auto &endpoint = alt.endpoint[endpoint_index];
                            view.endpoints.push_back({endpoint.bEndpointAddress, endpoint.bmAttributes,
                                                      endpoint.wMaxPacketSize, endpoint.bInterval});
                        }
                        snapshot.interfaces.push_back(std::move(view));
                    }
                }
            }
            snapshot.state_ = std::make_shared<UsbDeviceSnapshot::State>();
            snapshot.state_->context = state_;
            snapshot.state_->device = state_->backend->ref_device(devices[i]);
            snapshots.push_back(std::move(snapshot));
        }
    } catch (...) {
        libusb_free_device_list(devices, 1);
        throw;
    }
    libusb_free_device_list(devices, 1);
    if (result != nullptr) {
        *result = make_result(DTV_INTERNAL_RESULT_OK);
    }
    return snapshots;
}

UsbDevice::UsbDevice(std::unique_ptr<State> state) : state_(std::move(state)) {}
UsbDevice::~UsbDevice()
{
    if (state_ && state_->binding.handle && close_status() != UsbCloseStatus::closed) {
        run_close_callback({}, {UINT64_MAX, UINT64_MAX});
    }
}

dtv_internal_result UsbDevice::result() const
{
    return state_ ? state_->open_result : no_context();
}

const dtv_usb_io *UsbDevice::io() const
{
    return state_ && state_->binding.handle && state_->binding.handle->handle != nullptr &&
                   !state_->binding.handle->claimed_interfaces.empty()
               ? &state_->io
               : nullptr;
}

dtv_internal_result UsbDevice::run_close_callback(
    std::function<dtv_internal_result()> callback, dtv_timeout timeout)
{
    if (!state_ || !state_->binding.handle) {
        return callback ? callback() : make_result(DTV_INTERNAL_RESULT_OK);
    }
    auto handle = state_->binding.handle;
    begin_handle_close(handle);
    bool start_job = false;
    {
        std::lock_guard lock(handle->close_mutex);
        if (!handle->close_job_started) {
            handle->close_job_started = true;
            start_job = true;
        }
    }
    if (start_job) {
        auto &context = *handle->context;
        {
            std::lock_guard lock(context.lifecycle_mutex);
            context.lifecycle_jobs.emplace_back([handle, callback = std::move(callback)]() mutable {
                auto &owner = *handle->context;
                {
                    std::unique_lock lock(owner.requests_mutex);
                    owner.requests_condition.wait(lock, [&] { return closing_requests_drained(owner); });
                }
                lifecycle_cleanup_io = true;
                dtv_internal_result result = make_result(DTV_INTERNAL_RESULT_OK);
                if (callback) {
                    try { result = callback(); }
                    catch (...) { result = exception_result(); }
                }
                lifecycle_cleanup_io = false;
                {
                    std::unique_lock lock(owner.requests_mutex);
                    owner.requests_condition.wait(lock, [&] { return closing_requests_drained(owner); });
                }
                {
                    std::lock_guard lock(handle->close_mutex);
                    handle->close_result = result;
                    handle->close_job_done = true;
                    handle->close_status.store(UsbCloseStatus::closed);
                }
                handle->close_condition.notify_all();
            });
        }
        context.lifecycle_condition.notify_one();
    }

    const auto started = std::chrono::steady_clock::now();
    std::unique_lock lock(handle->close_mutex);
    while (!handle->close_job_done) {
        if (timeout.timeout_ms != UINT64_MAX &&
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(timeout.timeout_ms)) {
            handle->close_status.store(UsbCloseStatus::pending);
            return make_result(DTV_INTERNAL_RESULT_NOT_READY);
        }
        handle->close_condition.wait_for(lock, std::chrono::milliseconds(5));
        if (timeout.operation_deadline_ns != 0 && timeout.operation_deadline_ns != UINT64_MAX &&
            state_->binding.clock != nullptr &&
            state_->binding.clock->monotonic_now_ns != nullptr &&
            state_->binding.clock->monotonic_now_ns(state_->binding.clock->user) >=
                timeout.operation_deadline_ns && !handle->close_job_done) {
            handle->close_status.store(UsbCloseStatus::pending);
            return make_result(DTV_INTERNAL_RESULT_NOT_READY);
        }
    }
    return handle->close_result;
}

void UsbDevice::begin_close()
{
    if (state_) begin_handle_close(state_->binding.handle);
}

void UsbDevice::finish_close(dtv_internal_result result)
{
    if (!state_ || !state_->binding.handle) return;
    auto handle = state_->binding.handle;
    std::lock_guard lock(handle->close_mutex);
    handle->close_result = result;
    handle->close_status.store(UsbCloseStatus::closed);
    handle->close_job_done = true;
    handle->close_condition.notify_all();
}

UsbCloseStatus UsbDevice::close_status() const
{
    return state_ && state_->binding.handle
        ? state_->binding.handle->close_status.load()
        : UsbCloseStatus::closed;
}

dtv_internal_result UsbDevice::close_result() const
{
    if (!state_ || !state_->binding.handle) return make_result(DTV_INTERNAL_RESULT_OK);
    std::lock_guard lock(state_->binding.handle->close_mutex);
    return state_->binding.handle->close_result;
}

std::unique_ptr<UsbDevice> UsbContext::open(const UsbDeviceSnapshot &snapshot,
                                             int interface_number,
                                             const dtv_runtime_clock *clock) const
{
    if (interface_number < 0 || interface_number > 255) {
        auto device_state = std::make_unique<UsbDevice::State>();
        device_state->open_result = invalid();
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    const std::array claims{UsbInterfaceClaim{static_cast<std::uint8_t>(interface_number), 0}};
    return open(snapshot, claims, clock);
}

std::unique_ptr<UsbDevice> UsbContext::open(const UsbDeviceSnapshot &snapshot,
                                             std::span<const UsbInterfaceClaim> claims,
                                             const dtv_runtime_clock *clock) const
{
    auto device_state = std::make_unique<UsbDevice::State>();
    if (clock != nullptr) {
        device_state->clock = *clock;
        device_state->binding.clock = &device_state->clock;
    }
    if (state_) {
        device_state->binding.handle = std::make_shared<UsbHandleLease>();
        device_state->binding.handle->context = state_;
    }
    if (!state_ || state_->context == nullptr || !state_->event_thread.joinable() ||
        state_->result.code != DTV_INTERNAL_RESULT_OK || !snapshot.state_ ||
        snapshot.state_->context != state_ || claims.empty()) {
        device_state->open_result = invalid();
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }

    libusb_device_descriptor device_descriptor{};
    const int descriptor_result = state_->backend->get_device_descriptor(snapshot.state_->device,
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
        state_->backend->get_bus_number(snapshot.state_->device) != snapshot.bus_number ||
        state_->backend->get_device_address(snapshot.state_->device) != snapshot.device_address) {
        device_state->open_result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }

    libusb_config_descriptor *configuration = nullptr;
    int status = state_->backend->get_active_config_descriptor(snapshot.state_->device,
                                                               &configuration);
    if (status == LIBUSB_ERROR_NOT_FOUND) {
        status = state_->backend->get_config_descriptor(snapshot.state_->device, 0,
                                                         &configuration);
    }
    if (status != LIBUSB_SUCCESS || configuration == nullptr) {
        device_state->open_result = usb_result(status);
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    std::unique_ptr<libusb_config_descriptor, std::function<void(libusb_config_descriptor *)>>
        configuration_guard(configuration, [backend = state_->backend](auto *value) {
            backend->free_config_descriptor(value);
        });
    device_state->binding.handle->claimed_interfaces.reserve(claims.size());
    std::vector<int> seen_interfaces;
    seen_interfaces.reserve(claims.size());
    for (const auto &claim : claims) {
        if (std::find(seen_interfaces.begin(), seen_interfaces.end(), claim.interface_number) !=
            seen_interfaces.end()) {
            device_state->open_result = invalid();
            return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
        }
        seen_interfaces.push_back(claim.interface_number);
        bool alternate_found = false;
        for (std::uint8_t i = 0; i < configuration->bNumInterfaces; ++i) {
            const auto &interface = configuration->interface[i];
            for (int alternate = 0; alternate < interface.num_altsetting; ++alternate) {
                if (interface.altsetting[alternate].bInterfaceNumber == claim.interface_number &&
                    interface.altsetting[alternate].bAlternateSetting == claim.alternate_setting) {
                    alternate_found = true;
                }
            }
        }
        if (!alternate_found) {
            device_state->open_result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
        }
    }
    status = state_->backend->open_device(snapshot.state_->device,
                                          &device_state->binding.handle->handle);
    for (const auto &claim : claims) {
        if (status != LIBUSB_SUCCESS) break;
        status = state_->backend->claim_interface(device_state->binding.handle->handle,
                                                  claim.interface_number);
        if (status != LIBUSB_SUCCESS) break;
        device_state->binding.handle->claimed_interfaces.push_back(claim.interface_number);
        if (claim.alternate_setting != 0) {
            status = state_->backend->set_interface_alt_setting(
                device_state->binding.handle->handle, claim.interface_number,
                claim.alternate_setting);
        }
    }
    if (status != LIBUSB_SUCCESS) {
        device_state->open_result = usb_result(status);
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    device_state->io = make_io(&device_state->binding, device_state->binding.clock);
    return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
}

std::unique_ptr<UsbDevice> UsbTestDeviceFactory::open_usb_fd_impl(
    int descriptor, int interface_number, dtv_internal_result *result,
    std::shared_ptr<UsbTransferBackend> backend)
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
    device_state->binding.handle = std::make_shared<UsbHandleLease>();
    device_state->binding.handle->context = std::make_shared<UsbContext::State>();
    auto &context = *device_state->binding.handle->context;
    context.backend = std::move(backend);
    const int init_result = context.backend->init_context(&context.context);
    if (init_result != LIBUSB_SUCCESS) {
        close(owned_descriptor);
        *result = usb_result(init_result);
        return nullptr;
    }
    try {
        context.start_event_thread();
        context.start_lifecycle_thread();
    } catch (...) {
        close(owned_descriptor);
        *result = exception_result();
        return nullptr;
    }
    const int wrap_result = context.backend->wrap_sys_device(
        context.context, owned_descriptor, &device_state->binding.handle->handle);
    if (wrap_result != LIBUSB_SUCCESS) {
        close(owned_descriptor);
        *result = usb_result(wrap_result);
        return nullptr;
    }
    device_state->binding.handle->owned_descriptor = owned_descriptor;
    auto *wrapped_device = context.backend->get_device(device_state->binding.handle->handle);
    libusb_device_descriptor descriptor_info{};
    libusb_config_descriptor *configuration = nullptr;
    int descriptor_result = wrapped_device == nullptr
        ? LIBUSB_ERROR_NO_DEVICE
        : context.backend->get_device_descriptor(wrapped_device, &descriptor_info);
    if (descriptor_result == LIBUSB_SUCCESS) {
        descriptor_result = context.backend->get_active_config_descriptor(wrapped_device,
                                                                           &configuration);
    }
    if (descriptor_result != LIBUSB_SUCCESS || configuration == nullptr) {
        device_state->open_result = descriptor_result == LIBUSB_SUCCESS
            ? make_result(DTV_INTERNAL_RESULT_UNSUPPORTED)
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
    context.backend->free_config_descriptor(configuration);
    if (!interface_found) {
        device_state->open_result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        *result = device_state->open_result;
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    device_state->binding.handle->claimed_interfaces.reserve(1);
    const int claim_result = context.backend->claim_interface(
        device_state->binding.handle->handle, interface_number);
    if (claim_result != LIBUSB_SUCCESS) {
        device_state->open_result = usb_result(claim_result);
        *result = device_state->open_result;
        return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
    }
    device_state->binding.handle->claimed_interfaces.push_back(interface_number);
    device_state->io = make_io(&device_state->binding, nullptr);
    *result = make_result(DTV_INTERNAL_RESULT_OK);
    return std::unique_ptr<UsbDevice>(new UsbDevice(std::move(device_state)));
#else
    (void)descriptor;
    (void)interface_number;
    *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    return nullptr;
#endif
}

std::unique_ptr<UsbDevice> open_usb_fd(int descriptor, int interface_number,
                                       dtv_internal_result *result)
{
    return UsbTestDeviceFactory::open_usb_fd_impl(
        descriptor, interface_number, result, make_libusb_transfer_backend());
}

UsbContext UsbTestDeviceFactory::context(std::shared_ptr<UsbTransferBackend> backend)
{
    auto context = std::make_shared<UsbContext::State>();
    context->backend = std::move(backend);
    context->context = reinterpret_cast<libusb_context *>(1);
    context->start_event_thread();
    context->start_lifecycle_thread();
    return UsbContext(std::move(context));
}

UsbDeviceSnapshot UsbTestDeviceFactory::snapshot(const UsbContext &context)
{
    UsbDeviceSnapshot snapshot{};
    snapshot.vendor_id = 0x1234;
    snapshot.product_id = 0x5678;
    snapshot.device_version = 1;
    snapshot.bus_number = 1;
    snapshot.device_address = 2;
    snapshot.configuration_count = 1;
    snapshot.state_ = std::make_shared<UsbDeviceSnapshot::State>();
    snapshot.state_->context = context.state_;
    snapshot.state_->device = context.state_->backend->ref_device(
        reinterpret_cast<libusb_device *>(static_cast<std::uintptr_t>(3)));
    return snapshot;
}

std::unique_ptr<UsbDevice> UsbTestDeviceFactory::open_usb_fd(
    int descriptor, int interface_number, dtv_internal_result *result,
    std::shared_ptr<UsbTransferBackend> backend)
{
    return open_usb_fd_impl(descriptor, interface_number, result, std::move(backend));
}

}
