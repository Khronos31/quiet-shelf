// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/usb.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <chrono>
#include <climits>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <libusb.h>

#include <dtv/internal/result.hpp>
#include <dtv/internal/clock.hpp>

#include "usb-transfer-backend.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
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

struct ClaimReservationKey {
    enum class Kind : std::uint8_t { wrapped_owner, physical_identity, connection_epoch } kind;
    const void *owner = nullptr;
    const void *context = nullptr;
    std::uint64_t epoch = 0;
    UsbConnectionIdentity identity{};

    bool operator==(const ClaimReservationKey &) const = default;
};

struct ClaimReservationKeyHash {
    std::size_t operator()(const ClaimReservationKey &key) const
    {
        std::size_t value = std::hash<unsigned>{}(static_cast<unsigned>(key.kind));
        const auto combine = [&value](std::size_t next) {
            value ^= next + std::size_t{0x9e3779b9} + (value << 6) + (value >> 2);
        };
        combine(std::hash<const void *>{}(key.owner));
        combine(std::hash<const void *>{}(key.context));
        combine(std::hash<std::uint64_t>{}(key.epoch));
        combine(std::hash<std::uint64_t>{}(key.identity.filesystem));
        combine(std::hash<std::uint64_t>{}(key.identity.inode));
        combine(std::hash<std::uint64_t>{}(key.identity.device));
        return value;
    }
};

std::mutex claim_reservation_mutex;
std::unordered_map<ClaimReservationKey, std::uint64_t, ClaimReservationKeyHash>
    live_claim_reservations;
std::uint64_t next_claim_reservation = 1;
std::atomic<bool> fail_next_runtime_start_for_test{false};

struct ClaimReservation {
    std::uint64_t id = 0;
    std::vector<ClaimReservationKey> keys;

    ~ClaimReservation()
    {
        std::lock_guard lock(claim_reservation_mutex);
        for (const auto &key : keys) {
            const auto found = live_claim_reservations.find(key);
            if (found != live_claim_reservations.end() && found->second == id) {
                live_claim_reservations.erase(found);
            }
        }
    }
};

std::unique_ptr<ClaimReservation> reserve_claims(std::vector<ClaimReservationKey> keys,
                                                 dtv_internal_result *result)
{
    try {
        std::unordered_set<ClaimReservationKey, ClaimReservationKeyHash> unique;
        unique.reserve(keys.size());
        for (const auto &key : keys) {
            if (!unique.insert(key).second) {
                if (result != nullptr) *result = dtv_internal_result_make(
                    DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
                return nullptr;
            }
        }
        auto reservation = std::make_unique<ClaimReservation>();
        reservation->keys = std::move(keys);
        std::lock_guard lock(claim_reservation_mutex);
        for (const auto &key : reservation->keys) {
            if (live_claim_reservations.contains(key)) {
                if (result != nullptr) *result = dtv_internal_result_make(
                    DTV_INTERNAL_RESULT_BUSY, 0);
                return nullptr;
            }
        }
        if (next_claim_reservation == 0) {
            if (result != nullptr) *result = dtv_internal_result_make(
                DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
            return nullptr;
        }
        reservation->id = next_claim_reservation++;
        try {
            for (const auto &key : reservation->keys) {
                live_claim_reservations.emplace(key, reservation->id);
            }
        } catch (...) {
            for (const auto &key : reservation->keys) {
                const auto found = live_claim_reservations.find(key);
                if (found != live_claim_reservations.end() && found->second == reservation->id) {
                    live_claim_reservations.erase(found);
                }
            }
            throw;
        }
        if (result != nullptr) *result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        return reservation;
    } catch (...) {
        if (result != nullptr) *result = dtv_internal_result_make(
            DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
        return nullptr;
    }
}

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
    bool get_connection_identity(libusb_device *device, int descriptor,
                                 UsbConnectionIdentity *identity) override
    {
#if defined(__linux__)
        /* libusb USBFS wraps allocate fresh device objects, so key the kernel node. */
        if (identity == nullptr) return false;
        struct stat info{};
        int status = -1;
        if (descriptor >= 0) {
            status = fstat(descriptor, &info);
        } else {
            char path[64]{};
            std::snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u",
                          get_bus_number(device), get_device_address(device));
            status = stat(path, &info);
        }
        if (status != 0) return false;
        identity->filesystem = static_cast<std::uint64_t>(info.st_dev);
        identity->inode = static_cast<std::uint64_t>(info.st_ino);
        identity->device = static_cast<std::uint64_t>(info.st_rdev);
        identity->valid = true;
        return true;
#else
        (void)device;
        (void)descriptor;
        (void)identity;
        return false;
#endif
    }
    bool supports_connection_identity() const override
    {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
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
    struct DeviceConnection {
        std::uint64_t token;
        libusb_device *reference;
    };
    libusb_context *context = nullptr;
    bool scan_only = false;
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
    std::unordered_map<libusb_device *, DeviceConnection> device_connections;
    std::vector<std::pair<UsbConnectionIdentity, std::uint64_t>> connection_epochs;
    std::uint64_t next_connection_token = 1;
    std::mutex scan_mutex;
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
        for (const auto &[device, connection] : device_connections) {
            (void)device;
            backend->unref_device(connection.reference);
        }
        if (context != nullptr) {
            backend->exit_context(context);
        }
    }

    std::uint64_t connection_token(libusb_device *device,
                                   const UsbConnectionIdentity &identity,
                                   bool observed_in_full_scan)
    {
        const auto found = device_connections.find(device);
        if (identity.valid) {
            const auto epoch = std::find_if(connection_epochs.begin(), connection_epochs.end(),
                [&](const auto &entry) { return entry.first == identity; });
            if (epoch != connection_epochs.end()) {
                if (found != device_connections.end()) {
                    found->second.token = epoch->second;
                    return epoch->second;
                }
                if (observed_in_full_scan) {
                    auto *reference = backend->ref_device(device);
                    try {
                        device_connections.emplace(device,
                            DeviceConnection{epoch->second, reference});
                    } catch (...) {
                        backend->unref_device(reference);
                        throw;
                    }
                }
                return epoch->second;
            }
        }
        if (found != device_connections.end()) {
            if (!identity.valid) return found->second.token;
            if (next_connection_token == 0) throw std::bad_alloc();
            const auto token = next_connection_token++;
            connection_epochs.emplace_back(identity, token);
            found->second.token = token;
            return token;
        }
        if (next_connection_token == 0) throw std::bad_alloc();
        auto *reference = observed_in_full_scan ? backend->ref_device(device) : nullptr;
        const auto token = next_connection_token++;
        bool pointer_inserted = false;
        try {
            if (observed_in_full_scan) {
                pointer_inserted = device_connections.emplace(
                    device, DeviceConnection{token, reference}).second;
            }
            if (identity.valid) connection_epochs.emplace_back(identity, token);
        } catch (...) {
            if (pointer_inserted) device_connections.erase(device);
            if (reference != nullptr) backend->unref_device(reference);
            throw;
        }
        return token;
    }
};

struct UsbDeviceSnapshot::State {
    std::shared_ptr<UsbContext::State> context;
    libusb_device *device = nullptr;
    std::shared_ptr<void> owner;
    UsbConnectionIdentity identity;
    ~State()
    {
        if (device != nullptr) {
            context->backend->unref_device(device);
        }
    }
};

struct UsbCloseState {
    std::atomic<UsbCloseStatus> status{UsbCloseStatus::open};
    std::mutex mutex;
    std::condition_variable condition;
    bool job_started = false;
    bool job_done = false;
    dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
};

struct UsbHandleLease {
    std::shared_ptr<UsbContext::State> context;
    libusb_device_handle *handle = nullptr;
    std::vector<int> claimed_interfaces;
    int owned_descriptor = -1;
    std::atomic<bool> closing{false};
    std::shared_ptr<UsbCloseState> close_state = std::make_shared<UsbCloseState>();

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

struct UsbClaimHandleLease {
    std::shared_ptr<UsbContext::State> context;
    std::shared_ptr<UsbHandleLease> wrapped_owner;
    libusb_device_handle *handle = nullptr;
    std::vector<int> claimed_interfaces;
    bool owns_handle = false;

    ~UsbClaimHandleLease()
    {
        release();
    }

    int release() noexcept
    {
        int first_error = LIBUSB_SUCCESS;
        for (auto interface = claimed_interfaces.rbegin();
             interface != claimed_interfaces.rend(); ++interface) {
            const int result = context->backend->release_interface(handle, *interface);
            if (first_error == LIBUSB_SUCCESS && result != LIBUSB_SUCCESS) first_error = result;
        }
        claimed_interfaces.clear();
        if (owns_handle && handle != nullptr) {
            context->backend->close_handle(handle);
            handle = nullptr;
        }
        wrapped_owner.reset();
        return first_error;
    }
};

struct UsbClaimLeaseGroup::State {
    std::vector<std::unique_ptr<UsbClaimHandleLease>> handles;
    std::unique_ptr<ClaimReservation> reservation;
    std::uint64_t deadline = 0;

    ~State() { release(); }

    int release() noexcept
    {
        int first_error = LIBUSB_SUCCESS;
        while (!handles.empty()) {
            const int result = handles.back()->release();
            if (first_error == LIBUSB_SUCCESS && result != LIBUSB_SUCCESS) first_error = result;
            handles.pop_back();
        }
        reservation.reset();
        return first_error;
    }
};

namespace {

dtv_internal_result make_result(std::int32_t code, std::uint32_t detail = 0);
dtv_internal_result usb_result(int code);
dtv_internal_result invalid();
dtv_internal_result claim_checkpoint(const dtv_runtime_clock *clock,
                                     const dtv_cancel *cancel, std::uint64_t deadline);
int transfer_error(libusb_transfer_status status);

}

struct EnclosureRuntime::State {
    struct Request {
        State *owner = nullptr;
        std::uint64_t id = 0;
        std::uint64_t submit_sequence = 0;
        std::uint64_t generation = 0;
        std::size_t handle_index = 0;
        std::uint8_t endpoint = 0;
        bool input = false;
        bool continuous = false;
        bool initial_published = false;
        dtv_internal_result (*sink)(void *, const UsbRuntimePacket *) = nullptr;
        void *sink_user = nullptr;
        std::vector<unsigned char> buffer;
        dtv_runtime_clock clock{};
        bool has_clock = false;
        const dtv_cancel *cancel = nullptr;
        std::uint64_t deadline = 0;
        libusb_transfer *transfer = nullptr;
        libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
        int actual_length = 0;
        bool reached_backend = false;
        bool callback_received = false;
        bool terminal_requested = false;
        bool cancel_sent = false;
        bool completion_ready = false;
        UsbRuntimeTerminal terminal = UsbRuntimeTerminal::failed;
        dtv_internal_result terminal_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        UsbRuntimeCompletion completion;
    };

    static constexpr std::size_t kRequestCapacity = 64;
    using ActiveRequest = std::pair<libusb_transfer *, std::shared_ptr<Request>>;
    UsbClaimLeaseGroup claims;
    std::shared_ptr<UsbContext::State> context;
    std::vector<UsbClaimHandleLease *> handles;
    std::mutex mutex;
    std::condition_variable condition;
    std::unordered_map<std::uint64_t, std::shared_ptr<Request>> requests;
    std::array<std::shared_ptr<Request>, kRequestCapacity> active{};
    std::size_t active_size = 0;
    std::deque<std::uint64_t> pending;
    std::array<std::uint64_t, kRequestCapacity> completed{};
    std::size_t completed_head = 0;
    std::size_t completed_size = 0;
    std::thread worker;
    std::uint64_t next_request_id = 1;
    std::uint64_t next_submit_sequence = 1;
    std::uint64_t next_completion_sequence = 1;
    bool stopping = false;
    bool exited = true;
    bool started = false;
    bool released = false;
    std::mutex join_mutex;

    bool has_active_locked() const { return active_size != 0; }

    static void transfer_completed(libusb_transfer *transfer)
    {
        auto *request = static_cast<Request *>(transfer->user_data);
        std::lock_guard lock(request->owner->mutex);
        request->status = transfer->status;
        request->actual_length = transfer->actual_length;
        request->callback_received = true;
        request->owner->condition.notify_all();
    }

    void push_completed_locked(const std::shared_ptr<Request> &request,
                               UsbRuntimeTerminal terminal, dtv_internal_result result,
                               std::size_t actual_length, bool include_bytes)
    {
        if (request->completion_ready) return;
        request->terminal = terminal;
        request->terminal_result = result;
        request->completion.request_id = request->id;
        request->completion.submit_sequence = request->submit_sequence;
        request->completion.completion_sequence = next_completion_sequence++;
        request->completion.generation = request->generation;
        request->completion.handle_index = request->handle_index;
        request->completion.endpoint = request->endpoint;
        request->completion.terminal = terminal;
        request->completion.result = result;
        request->completion.actual_length = actual_length;
        request->completion.reached_backend = request->reached_backend;
        if (include_bytes && request->continuous) {
            const auto size = (std::min)(actual_length, request->buffer.size());
            request->completion.bytes.assign(request->buffer.begin(), request->buffer.begin() + size);
        } else if (include_bytes) {
            const auto size = (std::min)(actual_length, request->buffer.size());
            request->buffer.resize(size);
            request->completion.bytes = std::move(request->buffer);
        } else {
            request->buffer.clear();
        }
        request->completion_ready = true;
        request->initial_published = true;
        const auto tail = (completed_head + completed_size) % completed.size();
        completed[tail] = request->id;
        ++completed_size;
        condition.notify_all();
    }

    void finish_without_transfer(const std::shared_ptr<Request> &request,
                                 UsbRuntimeTerminal terminal,
                                 dtv_internal_result result)
    {
        bool notify_sink = false;
        {
            std::lock_guard lock(mutex);
            if (request->continuous && request->initial_published) {
                request->terminal_requested = true;
                request->terminal = terminal;
                request->terminal_result = result;
                notify_sink = true;
                condition.notify_all();
            } else {
                push_completed_locked(request, terminal, result, 0, false);
            }
        }
        if (notify_sink) {
            UsbRuntimePacket packet;
            packet.request_id = request->id;
            packet.submit_sequence = request->submit_sequence;
            packet.generation = request->generation;
            packet.handle_index = request->handle_index;
            packet.endpoint = request->endpoint;
            packet.terminal = terminal;
            packet.result = result;
            (void)call_callback([&] { return request->sink(request->sink_user, &packet); });
        }
    }

    void mark_terminal_locked(const std::shared_ptr<Request> &request,
                              UsbRuntimeTerminal terminal, dtv_internal_result result)
    {
        if (request->terminal_requested || (request->completion_ready && !request->continuous)) return;
        request->terminal_requested = true;
        request->terminal = terminal;
        request->terminal_result = result;
        condition.notify_all();
    }

    dtv_internal_result queue_request(
        std::size_t handle_index, std::uint8_t endpoint, bool input,
        std::span<const unsigned char> output, std::size_t input_capacity,
        std::uint64_t generation, const dtv_runtime_clock *clock,
        const dtv_cancel *cancel, std::uint64_t deadline, std::uint64_t *request_id)
    {
        if (request_id == nullptr || handle_index >= handles.size() ||
            (input ? input_capacity > INT_MAX : output.size() > INT_MAX)) return invalid();
        try {
            auto request = std::make_shared<Request>();
            request->owner = this;
            request->handle_index = handle_index;
            request->endpoint = endpoint;
            request->input = input;
            request->generation = generation;
            request->deadline = deadline;
            request->cancel = cancel;
            if (clock != nullptr) {
                request->clock = *clock;
                request->has_clock = true;
            }
            if (input) request->buffer.resize(input_capacity);
            else request->buffer.assign(output.begin(), output.end());
            std::lock_guard lock(mutex);
            if (stopping || !started || requests.size() >= kRequestCapacity ||
                next_request_id == 0 || next_submit_sequence == 0) {
                return stopping ? make_result(DTV_INTERNAL_RESULT_NOT_READY)
                                : make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
            }
            request->id = next_request_id++;
            request->submit_sequence = next_submit_sequence++;
            requests.emplace(request->id, request);
            try {
                pending.push_back(request->id);
            } catch (...) {
                requests.erase(request->id);
                throw;
            }
            *request_id = request->id;
            condition.notify_all();
            return make_result(DTV_INTERNAL_RESULT_OK);
        } catch (...) {
            return exception_result();
        }
    }

    dtv_internal_result queue_shared_request(
        std::size_t handle_index, std::uint8_t endpoint, std::size_t input_capacity,
        std::uint64_t generation,
        dtv_internal_result (*sink)(void *, const UsbRuntimePacket *), void *sink_user,
        std::uint64_t *request_id)
    {
        if (sink == nullptr || input_capacity == 0 || request_id == nullptr ||
            handle_index >= handles.size() || input_capacity > INT_MAX) return invalid();
        try {
            auto request = std::make_shared<Request>();
            request->owner = this;
            request->handle_index = handle_index;
            request->endpoint = endpoint;
            request->input = true;
            request->continuous = true;
            request->sink = sink;
            request->sink_user = sink_user;
            request->generation = generation;
            request->buffer.resize(input_capacity);
            request->completion.bytes.resize(input_capacity);
            std::lock_guard lock(mutex);
            if (stopping || !started || requests.size() >= kRequestCapacity ||
                next_request_id == 0 || next_submit_sequence == 0) {
                return stopping ? make_result(DTV_INTERNAL_RESULT_NOT_READY)
                                : make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
            }
            request->id = next_request_id++;
            request->submit_sequence = next_submit_sequence++;
            requests.emplace(request->id, request);
            try { pending.push_back(request->id); }
            catch (...) { requests.erase(request->id); throw; }
            *request_id = request->id;
            condition.notify_all();
            return make_result(DTV_INTERNAL_RESULT_OK);
        } catch (...) {
            return exception_result();
        }
    }

    void check_deadlines_and_cancel_tokens()
    {
        std::array<std::shared_ptr<Request>, kRequestCapacity> current{};
        std::size_t current_size = 0;
        {
            std::lock_guard lock(mutex);
            for (const auto &[id, request] : requests) {
                (void)id;
                if (!request->completion_ready && !request->terminal_requested) {
                    current[current_size++] = request;
                }
            }
        }
        for (std::size_t index = 0; index < current_size; ++index) {
            const auto &request = current[index];
            const auto *clock = request->has_clock ? &request->clock : nullptr;
            if (request->cancel != nullptr && clock != nullptr && clock->is_cancelled != nullptr) {
                std::uint32_t cancelled = 0;
                const auto result = call_callback([&] {
                    return clock->is_cancelled(clock->user, request->cancel, &cancelled);
                });
                std::lock_guard lock(mutex);
                if (result.code != DTV_INTERNAL_RESULT_OK) {
                    mark_terminal_locked(request, UsbRuntimeTerminal::failed, result);
                    continue;
                }
                if (cancelled != 0) {
                    mark_terminal_locked(request, UsbRuntimeTerminal::cancelled,
                                         make_result(DTV_INTERNAL_RESULT_TIMEOUT));
                    continue;
                }
            }
            if (request->deadline != 0 && request->deadline != UINT64_MAX) {
                const auto *runtime_clock = request->has_clock ? &request->clock
                                                               : system_runtime_clock();
                if (runtime_clock->monotonic_now_ns != nullptr) {
                    std::uint64_t now = 0;
                    const auto result = call_callback([&] {
                        now = runtime_clock->monotonic_now_ns(runtime_clock->user);
                        return make_result(DTV_INTERNAL_RESULT_OK);
                    });
                    std::lock_guard lock(mutex);
                    if (result.code != DTV_INTERNAL_RESULT_OK) {
                        mark_terminal_locked(request, UsbRuntimeTerminal::failed, result);
                    } else if (now >= request->deadline) {
                        mark_terminal_locked(request, UsbRuntimeTerminal::deadline,
                                             make_result(DTV_INTERNAL_RESULT_TIMEOUT));
                    }
                }
            }
        }
    }

    void process_pending()
    {
        for (;;) {
            std::shared_ptr<Request> request;
            {
                std::lock_guard lock(mutex);
                if (pending.empty()) return;
                const auto id = pending.front();
                pending.pop_front();
                const auto found = requests.find(id);
                if (found == requests.end()) continue;
                request = found->second;
                if (stopping && !request->terminal_requested) {
                    mark_terminal_locked(request, UsbRuntimeTerminal::stopped,
                                         make_result(DTV_INTERNAL_RESULT_NOT_READY));
                }
                if (request->terminal_requested) {
                    push_completed_locked(request, request->terminal, request->terminal_result,
                                          0, false);
                    continue;
                }
            }
            auto result = claim_checkpoint(request->has_clock ? &request->clock : nullptr,
                                           request->cancel, request->deadline);
            if (result.code != DTV_INTERNAL_RESULT_OK) {
                auto terminal = UsbRuntimeTerminal::failed;
                if (result.code == DTV_INTERNAL_RESULT_TIMEOUT) {
                    terminal = UsbRuntimeTerminal::deadline;
                    const auto *clock = request->has_clock ? &request->clock : system_runtime_clock();
                    if (request->cancel != nullptr && clock->is_cancelled != nullptr) {
                        std::uint32_t cancelled = 0;
                        const auto cancel_result = call_callback([&] {
                            return clock->is_cancelled(clock->user, request->cancel, &cancelled);
                        });
                        if (cancel_result.code != DTV_INTERNAL_RESULT_OK) {
                            result = cancel_result;
                            terminal = UsbRuntimeTerminal::failed;
                        } else if (cancelled != 0) {
                            terminal = UsbRuntimeTerminal::cancelled;
                        }
                    }
                }
                finish_without_transfer(request, terminal, result);
                continue;
            }
            auto *lease = handles[request->handle_index];
            libusb_transfer *transfer = nullptr;
            try {
                transfer = context->backend->allocate_transfer();
            } catch (...) {
                finish_without_transfer(request, UsbRuntimeTerminal::failed, exception_result());
                continue;
            }
            if (transfer == nullptr) {
                finish_without_transfer(request, UsbRuntimeTerminal::failed,
                                        make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED));
                continue;
            }
            request->transfer = transfer;
            libusb_fill_bulk_transfer(transfer, lease->handle, request->endpoint,
                request->buffer.data(), static_cast<int>(request->buffer.size()),
                transfer_completed, request.get(), 0);
            {
                std::lock_guard lock(mutex);
                const auto slot = std::find_if(active.begin(), active.end(),
                    [](const auto &entry) { return !entry; });
                if (slot == active.end()) {
                    request->transfer = nullptr;
                    try { context->backend->free_transfer(transfer); } catch (...) {}
                    push_completed_locked(request, UsbRuntimeTerminal::failed,
                        make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED), 0, false);
                    continue;
                }
                *slot = request;
                ++active_size;
            }
            int status = LIBUSB_ERROR_OTHER;
            try {
                status = context->backend->submit_transfer(transfer);
            } catch (...) {
                status = LIBUSB_ERROR_OTHER;
            }
            if (status != LIBUSB_SUCCESS) {
                {
                    std::lock_guard lock(mutex);
                    const auto slot = std::find_if(active.begin(), active.end(),
                        [&](const auto &entry) { return entry && entry->transfer == transfer; });
                    if (slot != active.end()) {
                        slot->reset();
                        --active_size;
                    }
                    request->transfer = nullptr;
                }
                dtv_internal_result result = usb_result(status);
                try { context->backend->free_transfer(transfer); }
                catch (...) { result = exception_result(); }
                finish_without_transfer(request, UsbRuntimeTerminal::failed, result);
            } else {
                std::lock_guard lock(mutex);
                request->reached_backend = true;
            }
        }
    }

    void request_cancellations()
    {
        std::array<ActiveRequest, kRequestCapacity> cancel_list{};
        std::size_t cancel_count = 0;
        {
            std::lock_guard lock(mutex);
            for (const auto &request : active) {
                if (!request) continue;
                if (request->terminal_requested && !request->cancel_sent) {
                    request->cancel_sent = true;
                    cancel_list[cancel_count++] = {request->transfer, request};
                }
            }
        }
        for (std::size_t index = 0; index < cancel_count; ++index) {
            const auto &[transfer, request] = cancel_list[index];
            try {
                (void)context->backend->cancel_transfer(transfer);
            } catch (...) {
                std::lock_guard lock(mutex);
                mark_terminal_locked(request, UsbRuntimeTerminal::failed, exception_result());
            }
        }
    }

    void retire_callbacks()
    {
        std::array<ActiveRequest, kRequestCapacity> retired{};
        std::size_t retired_count = 0;
        {
            std::lock_guard lock(mutex);
            for (auto &request : active) {
                if (!request) continue;
                if (!request->callback_received) {
                    continue;
                }
                if (request->continuous) {
                    const bool earlier_pending = std::any_of(active.begin(), active.end(),
                        [&](const auto &candidate) {
                            return candidate && candidate->continuous &&
                                candidate->handle_index == request->handle_index &&
                                candidate->endpoint == request->endpoint &&
                                candidate->submit_sequence < request->submit_sequence;
                        });
                    if (earlier_pending) continue;
                }
                retired[retired_count++] = {request->transfer, request};
                request->transfer = nullptr;
                request.reset();
                --active_size;
            }
        }
        for (std::size_t index = 0; index < retired_count; ++index) {
            const auto &[transfer, request] = retired[index];
            auto free_result = make_result(DTV_INTERNAL_RESULT_OK);
            try { context->backend->free_transfer(transfer); }
            catch (...) { free_result = exception_result(); }
            UsbRuntimeTerminal terminal;
            dtv_internal_result result;
            {
            std::unique_lock lock(mutex);
            if (free_result.code != DTV_INTERNAL_RESULT_OK) {
                terminal = UsbRuntimeTerminal::failed;
                result = free_result;
            } else if (request->terminal_requested) {
                terminal = request->terminal;
                result = request->terminal_result;
            } else if (request->status == LIBUSB_TRANSFER_COMPLETED) {
                terminal = UsbRuntimeTerminal::completed;
                result = make_result(DTV_INTERNAL_RESULT_OK,
                                     static_cast<std::uint32_t>((std::max)(request->actual_length, 0)));
            } else if (request->status == LIBUSB_TRANSFER_CANCELLED) {
                terminal = UsbRuntimeTerminal::endpoint_cancelled;
                result = usb_result(LIBUSB_ERROR_INTERRUPTED);
            } else {
                terminal = UsbRuntimeTerminal::failed;
                result = usb_result(transfer_error(request->status));
            }
            if (!request->continuous || !request->initial_published ||
                terminal != UsbRuntimeTerminal::completed || stopping || request->terminal_requested) {
                if (!request->continuous || !request->initial_published) {
                    if (request->continuous && terminal == UsbRuntimeTerminal::completed &&
                        !stopping && !request->terminal_requested) {
                        UsbRuntimePacket packet;
                        packet.request_id = request->id;
                        packet.submit_sequence = request->submit_sequence;
                        packet.generation = request->generation;
                        packet.handle_index = request->handle_index;
                        packet.endpoint = request->endpoint;
                        packet.actual_length = static_cast<std::size_t>((std::max)(request->actual_length, 0));
                        packet.bytes = std::span<const unsigned char>(request->buffer.data(),
                            (std::min)(packet.actual_length, request->buffer.size()));
                        lock.unlock();
                        const auto sink_result = call_callback([&] {
                            return request->sink(request->sink_user, &packet);
                        });
                        lock.lock();
                        if (sink_result.code != DTV_INTERNAL_RESULT_OK) {
                            terminal = UsbRuntimeTerminal::failed;
                            result = sink_result;
                        }
                    }
                    push_completed_locked(request, terminal, result,
                        static_cast<std::size_t>((std::max)(request->actual_length, 0)),
                        request->input && terminal == UsbRuntimeTerminal::completed);
                    if (request->continuous && terminal == UsbRuntimeTerminal::completed &&
                        !stopping && !request->terminal_requested && next_submit_sequence != 0) {
                        request->submit_sequence = next_submit_sequence++;
                        request->callback_received = false;
                        request->actual_length = 0;
                        try { pending.push_back(request->id); }
                        catch (...) {
                            request->terminal_requested = true;
                            request->terminal = UsbRuntimeTerminal::failed;
                            request->terminal_result = exception_result();
                        }
                        condition.notify_all();
                    }
                } else if (request->continuous) {
                    request->terminal_requested = true;
                    request->terminal = terminal;
                    request->terminal_result = result;
                    condition.notify_all();
                    UsbRuntimePacket packet;
                    packet.request_id = request->id;
                    packet.submit_sequence = request->submit_sequence;
                    packet.generation = request->generation;
                    packet.handle_index = request->handle_index;
                    packet.endpoint = request->endpoint;
                    packet.terminal = terminal;
                    packet.result = result;
                    lock.unlock();
                    (void)call_callback([&] { return request->sink(request->sink_user, &packet); });
                }
                continue;
            }
            }
            UsbRuntimePacket packet;
            packet.request_id = request->id;
            packet.submit_sequence = request->submit_sequence;
            packet.generation = request->generation;
            packet.handle_index = request->handle_index;
            packet.endpoint = request->endpoint;
            packet.actual_length = static_cast<std::size_t>((std::max)(request->actual_length, 0));
            packet.bytes = std::span<const unsigned char>(request->buffer.data(),
                (std::min)(packet.actual_length, request->buffer.size()));
            const auto sink_result = call_callback([&] { return request->sink(request->sink_user, &packet); });
            std::lock_guard lock(mutex);
            if (sink_result.code != DTV_INTERNAL_RESULT_OK || stopping || request->terminal_requested) {
                request->terminal_requested = true;
                request->terminal = stopping ? UsbRuntimeTerminal::stopped : UsbRuntimeTerminal::failed;
                request->terminal_result = stopping
                    ? make_result(DTV_INTERNAL_RESULT_NOT_READY) : sink_result;
                condition.notify_all();
                continue;
            }
            if (next_submit_sequence == 0) {
                request->terminal_requested = true;
                request->terminal = UsbRuntimeTerminal::failed;
                request->terminal_result = make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
                condition.notify_all();
                continue;
            }
            request->submit_sequence = next_submit_sequence++;
            request->callback_received = false;
            request->cancel_sent = false;
            request->actual_length = 0;
            try {
                pending.push_back(request->id);
            } catch (...) {
                request->terminal_requested = true;
                request->terminal = UsbRuntimeTerminal::failed;
                request->terminal_result = exception_result();
                condition.notify_all();
            }
            condition.notify_all();
        }
    }

    void run() noexcept
    {
        for (;;) {
            check_deadlines_and_cancel_tokens();
            process_pending();
            request_cancellations();
            retire_callbacks();
            {
                std::unique_lock lock(mutex);
                if (stopping && !has_active_locked() && pending.empty()) {
                    exited = true;
                    condition.notify_all();
                    return;
                }
                if (!has_active_locked()) {
                    condition.wait_for(lock, std::chrono::milliseconds(10), [&] {
                        return stopping || !pending.empty() || has_active_locked();
                    });
                    continue;
                }
            }
            timeval timeout{0, 10000};
            try {
                const int result = context->backend->handle_events(context->context, &timeout);
                if (result != LIBUSB_SUCCESS) {
                    std::lock_guard lock(mutex);
                    for (const auto &request : active) {
                        if (!request) continue;
                        mark_terminal_locked(request, UsbRuntimeTerminal::failed,
                                             usb_result(result));
                    }
                }
            } catch (...) {
                std::lock_guard lock(mutex);
                for (const auto &request : active) {
                    if (!request) continue;
                    mark_terminal_locked(request, UsbRuntimeTerminal::failed, exception_result());
                }
            }
            retire_callbacks();
        }
    }
};

EnclosureRuntime::EnclosureRuntime(std::unique_ptr<State> state) : state_(std::move(state)) {}

std::unique_ptr<EnclosureRuntime> EnclosureRuntime::start(
    UsbClaimLeaseGroup &&claims, dtv_internal_result *result)
{
    if (!claims.state_ || claims.state_->handles.empty()) {
        if (result != nullptr) *result = invalid();
        return nullptr;
    }
    if (fail_next_runtime_start_for_test.exchange(false, std::memory_order_acq_rel)) {
        if (result != nullptr) *result = make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
        return nullptr;
    }
    try {
        auto state = std::make_unique<State>();
        state->claims = std::move(claims);
        state->handles.reserve(state->claims.state_->handles.size());
        for (const auto &lease : state->claims.state_->handles) {
            if (!lease || lease->handle == nullptr || !lease->context ||
                lease->context->context == nullptr || !lease->context->scan_only) {
                if (result != nullptr) *result = invalid();
                return nullptr;
            }
            if (!state->context) state->context = lease->context;
            if (state->context != lease->context) {
                if (result != nullptr) *result = invalid();
                return nullptr;
            }
            state->handles.push_back(lease.get());
        }
        auto runtime = std::unique_ptr<EnclosureRuntime>(new EnclosureRuntime(std::move(state)));
        auto *runtime_state = runtime->state_.get();
        {
            std::lock_guard lock(runtime_state->mutex);
            runtime_state->started = true;
            runtime_state->exited = false;
        }
        try {
            runtime_state->worker = std::thread([runtime_state] { runtime_state->run(); });
        } catch (...) {
            std::lock_guard lock(runtime_state->mutex);
            runtime_state->started = false;
            runtime_state->exited = true;
            if (result != nullptr) *result = exception_result();
            return nullptr;
        }
        if (result != nullptr) *result = make_result(DTV_INTERNAL_RESULT_OK, 0);
        return runtime;
    } catch (...) {
        if (result != nullptr) *result = exception_result();
        return nullptr;
    }
}

void set_enclosure_runtime_start_failure_for_test(bool fail)
{
    fail_next_runtime_start_for_test.store(fail, std::memory_order_release);
}

EnclosureRuntime::~EnclosureRuntime()
{
    if (!state_) return;
    auto stopped = stop(nullptr, UINT64_MAX);
    (void)stopped;
    (void)join_release();
}

dtv_internal_result EnclosureRuntime::submit_bulk_out(
    std::size_t handle_index, std::uint8_t endpoint, std::span<const unsigned char> bytes,
    std::uint64_t generation, const dtv_runtime_clock *clock, const dtv_cancel *cancel,
    std::uint64_t deadline_ns, std::uint64_t *request_id)
{
    return state_->queue_request(handle_index, endpoint, false, bytes, 0, generation,
                                 clock, cancel, deadline_ns, request_id);
}

dtv_internal_result EnclosureRuntime::submit_bulk_in(
    std::size_t handle_index, std::uint8_t endpoint, std::size_t capacity,
    std::uint64_t generation, const dtv_runtime_clock *clock, const dtv_cancel *cancel,
    std::uint64_t deadline_ns, std::uint64_t *request_id)
{
    return state_->queue_request(handle_index, endpoint, true, {}, capacity, generation,
                                 clock, cancel, deadline_ns, request_id);
}

dtv_internal_result EnclosureRuntime::submit_shared_bulk_in(
    std::size_t handle_index, std::uint8_t endpoint, std::size_t capacity,
    std::uint64_t generation,
    dtv_internal_result (*sink)(void *, const UsbRuntimePacket *), void *sink_user,
    std::uint64_t *request_id)
{
    return state_->queue_shared_request(handle_index, endpoint, capacity, generation,
                                        sink, sink_user, request_id);
}

dtv_internal_result EnclosureRuntime::cancel(std::uint64_t request_id)
{
    std::lock_guard lock(state_->mutex);
    const auto found = state_->requests.find(request_id);
    if (found == state_->requests.end()) return invalid();
    state_->mark_terminal_locked(found->second, UsbRuntimeTerminal::cancelled,
                                 make_result(DTV_INTERNAL_RESULT_TIMEOUT, 0));
    return make_result(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result EnclosureRuntime::wait_request(
    std::uint64_t request_id, const dtv_runtime_clock *clock, std::uint64_t deadline_ns,
    UsbRuntimeTerminal *terminal)
{
    if (terminal == nullptr) return invalid();
    std::unique_lock lock(state_->mutex);
    const auto found = state_->requests.find(request_id);
    if (found == state_->requests.end()) return invalid();
    const auto request = found->second;
    for (;;) {
        if (request->completion_ready) {
            *terminal = request->terminal;
            return request->terminal_result;
        }
        if (request->terminal_requested) {
            *terminal = request->terminal;
            return request->terminal_result;
        }
        lock.unlock();
        const auto checkpoint = claim_checkpoint(clock, nullptr, deadline_ns);
        lock.lock();
        if (checkpoint.code != DTV_INTERNAL_RESULT_OK) return checkpoint;
        state_->condition.wait_for(lock, std::chrono::milliseconds(5));
    }
}

dtv_internal_result EnclosureRuntime::next_completion(UsbRuntimeCompletion *completion,
                                                       bool *available)
{
    if (completion == nullptr || available == nullptr) return invalid();
    std::lock_guard lock(state_->mutex);
    *available = false;
    if (state_->completed_size == 0) return make_result(DTV_INTERNAL_RESULT_OK, 0);
    const auto id = state_->completed[state_->completed_head];
    state_->completed_head = (state_->completed_head + 1) % state_->completed.size();
    --state_->completed_size;
    const auto found = state_->requests.find(id);
    if (found == state_->requests.end()) return make_result(DTV_INTERNAL_RESULT_USB_IO, 0);
    *completion = std::move(found->second->completion);
    if (found->second->continuous) {
        found->second->completion_ready = false;
        found->second->completion.bytes.clear();
    } else {
        state_->requests.erase(found);
    }
    *available = true;
    return make_result(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result EnclosureRuntime::take_completion(std::uint64_t request_id,
                                                        UsbRuntimeCompletion *completion,
                                                        bool *available)
{
    if (request_id == 0 || completion == nullptr || available == nullptr) return invalid();
    std::lock_guard lock(state_->mutex);
    *available = false;
    const auto found = state_->requests.find(request_id);
    if (found == state_->requests.end()) return invalid();
    if (!found->second->completion_ready) return make_result(DTV_INTERNAL_RESULT_OK, 0);
    std::size_t position = state_->completed_size;
    for (std::size_t index = 0; index < state_->completed_size; ++index) {
        const auto slot = (state_->completed_head + index) % state_->completed.size();
        if (state_->completed[slot] == request_id) {
            position = index;
            break;
        }
    }
    if (position == state_->completed_size) return make_result(DTV_INTERNAL_RESULT_INTERNAL, 0);
    *completion = std::move(found->second->completion);
    for (std::size_t index = position; index + 1 < state_->completed_size; ++index) {
        const auto current = (state_->completed_head + index) % state_->completed.size();
        const auto next = (state_->completed_head + index + 1) % state_->completed.size();
        state_->completed[current] = state_->completed[next];
    }
    --state_->completed_size;
    state_->requests.erase(found);
    *available = true;
    return make_result(DTV_INTERNAL_RESULT_OK, 0);
}

UsbRuntimeStopResult EnclosureRuntime::stop(const dtv_runtime_clock *clock,
                                             std::uint64_t deadline_ns)
{
    UsbRuntimeStopResult result;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        for (const auto &[id, request] : state_->requests) {
            (void)id;
            if (!request->completion_ready || request->continuous) {
                state_->mark_terminal_locked(request, UsbRuntimeTerminal::stopped,
                    make_result(DTV_INTERNAL_RESULT_NOT_READY, 0));
            }
        }
        state_->condition.notify_all();
    }
    std::unique_lock lock(state_->mutex);
    while (!state_->exited) {
        lock.unlock();
        const auto checkpoint = claim_checkpoint(clock, nullptr, deadline_ns);
        lock.lock();
        if (checkpoint.code != DTV_INTERNAL_RESULT_OK) {
            result.pending = true;
            result.result = checkpoint;
            return result;
        }
        state_->condition.wait_for(lock, std::chrono::milliseconds(5));
    }
    lock.unlock();
    result.result = join_release();
    return result;
}

dtv_internal_result EnclosureRuntime::join_release()
{
    std::lock_guard join_lock(state_->join_mutex);
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->exited) return make_result(DTV_INTERNAL_RESULT_NOT_READY, 0);
    }
    if (state_->worker.joinable()) state_->worker.join();
    if (state_->released) return make_result(DTV_INTERNAL_RESULT_OK, 0);
    state_->released = true;
    return state_->claims.release();
}

std::size_t EnclosureRuntime::worker_count() const
{
    std::lock_guard lock(state_->mutex);
    return state_->started && !state_->exited ? 1 : 0;
}

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

dtv_internal_result make_result(std::int32_t code, std::uint32_t detail)
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

dtv_internal_result claim_result(int code)
{
    if (code == LIBUSB_ERROR_BUSY) {
        return make_result(DTV_INTERNAL_RESULT_BUSY, static_cast<std::uint32_t>(-code));
    }
    return usb_result(code);
}

dtv_internal_result invalid();

dtv_internal_result claim_checkpoint(const dtv_runtime_clock *clock,
                                     const dtv_cancel *cancel, std::uint64_t deadline)
{
    const auto *runtime = clock == nullptr ? system_runtime_clock() : clock;
    if (cancel != nullptr && runtime->is_cancelled != nullptr) {
        std::uint32_t cancelled = 0;
        const auto result = call_callback([&] {
            return runtime->is_cancelled(runtime->user, cancel, &cancelled);
        });
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        if (cancelled != 0) return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    if (deadline == 0 || deadline == UINT64_MAX) return make_result(DTV_INTERNAL_RESULT_OK);
    if (runtime->monotonic_now_ns == nullptr) return invalid();
    std::uint64_t now = 0;
    const auto result = call_callback([&] {
        now = runtime->monotonic_now_ns(runtime->user);
        return make_result(DTV_INTERNAL_RESULT_OK);
    });
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    return now >= deadline ? make_result(DTV_INTERNAL_RESULT_TIMEOUT)
                           : make_result(DTV_INTERNAL_RESULT_OK);
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
    handle->close_state->status.store(UsbCloseStatus::closing);
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
    last_operation_terminal_reason = UsbOperationTerminalReason::none;
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
    last_operation_terminal_reason = UsbOperationTerminalReason::none;
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
    last_operation_terminal_reason = UsbOperationTerminalReason::none;
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

UsbClaimLeaseGroup::UsbClaimLeaseGroup() : state_(std::make_unique<State>()) {}
UsbClaimLeaseGroup::~UsbClaimLeaseGroup() = default;
UsbClaimLeaseGroup::UsbClaimLeaseGroup(UsbClaimLeaseGroup &&) noexcept = default;
UsbClaimLeaseGroup &UsbClaimLeaseGroup::operator=(UsbClaimLeaseGroup &&) noexcept = default;

dtv_internal_result UsbClaimLeaseGroup::release() noexcept
{
    if (!state_) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    const int result = state_->release();
    return result == LIBUSB_SUCCESS ? dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0)
                                   : dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO,
                                                              static_cast<std::uint32_t>(-result));
}

std::uint64_t UsbClaimLeaseGroup::deadline_ns() const
{
    return state_ ? state_->deadline : 0;
}

std::size_t UsbClaimLeaseGroup::size() const
{
    return state_ ? state_->handles.size() : 0;
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

UsbContext UsbContext::scan_only(std::shared_ptr<UsbTransferBackend> backend)
{
    auto state = std::make_shared<State>();
    state->scan_only = true;
    if (backend) state->backend = std::move(backend);
    const int result = state->backend->init_context(&state->context);
    if (result != LIBUSB_SUCCESS) {
        state->result = usb_result(result);
    }
    return UsbContext(std::move(state));
}

UsbContext::~UsbContext() = default;
UsbContext::UsbContext(std::shared_ptr<State> state) : state_(std::move(state)) {}
UsbContext::UsbContext(UsbContext &&) noexcept = default;
UsbContext &UsbContext::operator=(UsbContext &&) noexcept = default;

dtv_internal_result UsbContext::result() const
{
    return state_ ? state_->result : no_context();
}

std::vector<UsbDeviceSnapshot> UsbContext::enumerate(dtv_internal_result *result,
                                                    dtv_token_of_callback token_callback,
                                                    void *token_user) const
{
    std::vector<UsbDeviceSnapshot> snapshots;
    if (!state_ || state_->context == nullptr) {
        if (result != nullptr) {
            *result = no_context();
        }
        return snapshots;
    }
    std::unique_lock scan_lock(state_->scan_mutex, std::defer_lock);
    if (state_->scan_only) scan_lock.lock();
    libusb_device **devices = nullptr;
    const ssize_t count = state_->backend->get_device_list(state_->context, &devices);
    if (count < 0) {
        if (result != nullptr) {
            *result = usb_result(static_cast<int>(count));
        }
        return snapshots;
    }
    std::unordered_set<libusb_device *> observed_devices;
    std::unordered_set<std::uint64_t> observed_connections;
    try {
        if (state_->scan_only) {
            observed_devices.reserve(static_cast<std::size_t>(count));
            observed_connections.reserve(static_cast<std::size_t>(count));
        }
        for (ssize_t i = 0; i < count; ++i) {
            if (state_->scan_only) observed_devices.insert(devices[i]);
            const auto selection_token = token_callback == nullptr
                ? dtv_usb_token{0} : token_callback(token_user, devices[i]);
            libusb_device_descriptor descriptor{};
            const int descriptor_result = state_->backend->get_device_descriptor(devices[i], &descriptor);
            if (descriptor_result != LIBUSB_SUCCESS) {
                if (state_->scan_only) {
                    state_->backend->free_device_list(devices, 1);
                    if (result != nullptr) *result = usb_result(descriptor_result);
                    return {};
                }
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
            snapshot.selection_token = selection_token;
            std::uint8_t ports[8]{};
            const int port_count = state_->backend->get_port_numbers(devices[i], ports, sizeof(ports));
            if (state_->scan_only && port_count < 0) {
                state_->backend->free_device_list(devices, 1);
                if (result != nullptr) *result = usb_result(port_count);
                return {};
            }
            if (port_count > 0) {
                snapshot.port_path.assign(ports, ports + port_count);
            }
            if (state_->scan_only) {
                UsbConnectionIdentity identity{};
                const bool identity_found = state_->backend->get_connection_identity(
                    devices[i], -1, &identity);
                if (state_->backend->supports_connection_identity() && !identity_found) {
                    state_->backend->free_device_list(devices, 1);
                    if (result != nullptr) *result = usb_result(LIBUSB_ERROR_IO);
                    return {};
                }
                snapshot.connection_token = state_->connection_token(devices[i], identity, true);
                observed_connections.insert(snapshot.connection_token);
                snapshot.state_ = std::make_shared<UsbDeviceSnapshot::State>();
                snapshot.state_->context = state_;
                snapshot.state_->device = state_->backend->ref_device(devices[i]);
                snapshot.state_->identity = identity;
            }
            for (std::uint8_t config_index = 0; config_index < descriptor.bNumConfigurations;
                 ++config_index) {
                libusb_config_descriptor *config = nullptr;
                const int config_result = state_->backend->get_config_descriptor(
                    devices[i], config_index, &config);
                if (config_result != LIBUSB_SUCCESS) {
                    if (state_->scan_only) {
                        state_->backend->free_device_list(devices, 1);
                        if (result != nullptr) *result = usb_result(config_result);
                        return {};
                    }
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
            if (!snapshot.state_) {
                snapshot.state_ = std::make_shared<UsbDeviceSnapshot::State>();
                snapshot.state_->context = state_;
                snapshot.state_->device = state_->backend->ref_device(devices[i]);
            }
            snapshots.push_back(std::move(snapshot));
        }
    } catch (...) {
        state_->backend->free_device_list(devices, 1);
        throw;
    }
    if (state_->scan_only) {
        for (auto it = state_->device_connections.begin(); it != state_->device_connections.end();) {
            if (observed_devices.contains(it->first)) {
                ++it;
                continue;
            }
            state_->backend->unref_device(it->second.reference);
            it = state_->device_connections.erase(it);
        }
        state_->connection_epochs.erase(
            std::remove_if(state_->connection_epochs.begin(), state_->connection_epochs.end(),
                [&](const auto &entry) { return !observed_connections.contains(entry.second); }),
            state_->connection_epochs.end());
    }
    state_->backend->free_device_list(devices, 1);
    if (result != nullptr) {
        *result = make_result(DTV_INTERNAL_RESULT_OK);
    }
    return snapshots;
}

std::vector<UsbDeviceSnapshot> UsbContext::enumerate_fds(
    std::span<const std::int32_t> descriptors, dtv_internal_result *result) const
{
    std::vector<UsbDeviceSnapshot> snapshots;
    if (!state_ || state_->context == nullptr) {
        if (result != nullptr) *result = no_context();
        return snapshots;
    }
#if defined(__linux__)
    std::unique_lock scan_lock(state_->scan_mutex);
    try {
        snapshots.reserve(descriptors.size());
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            const int owned = fcntl(descriptors[index], F_DUPFD_CLOEXEC, 0);
            if (owned < 0) {
                if (result != nullptr) *result = invalid();
                return {};
            }
            auto lease = std::make_shared<UsbHandleLease>();
            lease->context = state_;
            lease->owned_descriptor = owned;
            const int wrapped = state_->backend->wrap_sys_device(
                state_->context, owned, &lease->handle);
            if (wrapped != LIBUSB_SUCCESS) {
                if (result != nullptr) *result = usb_result(wrapped);
                return {};
            }
            auto *device = state_->backend->get_device(lease->handle);
            if (device == nullptr) {
                if (result != nullptr) *result = usb_result(LIBUSB_ERROR_NO_DEVICE);
                return {};
            }
            libusb_device_descriptor descriptor{};
            int status = state_->backend->get_device_descriptor(device, &descriptor);
            if (status != LIBUSB_SUCCESS) {
                if (result != nullptr) *result = usb_result(status);
                return {};
            }
            UsbDeviceSnapshot snapshot{};
            snapshot.vendor_id = descriptor.idVendor;
            snapshot.product_id = descriptor.idProduct;
            snapshot.device_version = descriptor.bcdDevice;
            snapshot.device_class = descriptor.bDeviceClass;
            snapshot.device_subclass = descriptor.bDeviceSubClass;
            snapshot.device_protocol = descriptor.bDeviceProtocol;
            snapshot.bus_number = state_->backend->get_bus_number(device);
            snapshot.device_address = state_->backend->get_device_address(device);
            snapshot.configuration_count = descriptor.bNumConfigurations;
            snapshot.candidate_id = static_cast<std::uint64_t>(index) + 1;
            std::uint8_t ports[8]{};
            const int port_count = state_->backend->get_port_numbers(device, ports, sizeof(ports));
            if (port_count < 0) {
                if (result != nullptr) *result = usb_result(port_count);
                return {};
            }
            if (port_count > 0) snapshot.port_path.assign(ports, ports + port_count);
            UsbConnectionIdentity identity{};
            if (!state_->backend->get_connection_identity(device, owned, &identity) ||
                !identity.valid) {
                if (result != nullptr) *result = usb_result(LIBUSB_ERROR_IO);
                return {};
            }
            snapshot.connection_token = state_->connection_token(device, identity, false);
            snapshot.state_ = std::make_shared<UsbDeviceSnapshot::State>();
            snapshot.state_->context = state_;
            snapshot.state_->device = state_->backend->ref_device(device);
            snapshot.state_->owner = lease;
            snapshot.state_->identity = identity;
            for (std::uint8_t config_index = 0; config_index < descriptor.bNumConfigurations;
                 ++config_index) {
                libusb_config_descriptor *config = nullptr;
                status = state_->backend->get_config_descriptor(device, config_index, &config);
                if (status != LIBUSB_SUCCESS) {
                    if (result != nullptr) *result = usb_result(status);
                    return {};
                }
                std::unique_ptr<libusb_config_descriptor,
                                std::function<void(libusb_config_descriptor *)>>
                    guard(config, [backend = state_->backend](auto *value) {
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
            snapshots.push_back(std::move(snapshot));
        }
    } catch (...) {
        if (result != nullptr) *result = exception_result();
        return {};
    }
    if (result != nullptr) *result = make_result(DTV_INTERNAL_RESULT_OK);
    return snapshots;
#else
    (void)descriptors;
    if (result != nullptr) *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    return snapshots;
#endif
}

dtv_internal_result UsbContext::acquire_claims(
    std::span<const UsbClaimDeviceInput> inputs, const dtv_runtime_clock *clock,
    const dtv_cancel *cancel, std::uint64_t deadline_ns, UsbClaimLeaseGroup *out) const
{
    if (out == nullptr || !state_ || state_->context == nullptr || inputs.empty() ||
        out->size() != 0) return invalid();
    try {
        UsbClaimLeaseGroup pending;
        pending.state_->deadline = deadline_ns;
        pending.state_->handles.reserve(inputs.size());
        std::vector<std::shared_ptr<UsbHandleLease>> wrapped_owners;
        wrapped_owners.reserve(inputs.size());
        std::vector<ClaimReservationKey> reservation_keys;
        reservation_keys.reserve(inputs.size() * 3);
        std::vector<UsbConnectionIdentity> identities;
        identities.reserve(inputs.size());
        std::unordered_set<std::uint64_t> connections;
        connections.reserve(inputs.size());

        for (const auto &input : inputs) {
            const auto &snapshot = input.snapshot;
            if (input.connection_epoch == 0 || input.connection_epoch != snapshot.connection_token ||
                !connections.insert(input.connection_epoch).second || input.interfaces.empty() ||
                !snapshot.state_ || snapshot.state_->context != state_ ||
                snapshot.state_->device == nullptr) return invalid();
            auto owner = snapshot.state_->owner
                ? std::static_pointer_cast<UsbHandleLease>(snapshot.state_->owner)
                : std::shared_ptr<UsbHandleLease>{};
            if (owner && (owner->context != state_ || owner->handle == nullptr ||
                          owner->owned_descriptor < 0)) return invalid();
            ClaimReservationKey reservation_key{};
            if (owner) {
                reservation_key.kind = ClaimReservationKey::Kind::wrapped_owner;
                reservation_key.owner = owner.get();
                reservation_keys.push_back(reservation_key);
            }
            if (snapshot.state_->identity.valid) {
                reservation_key.kind = ClaimReservationKey::Kind::physical_identity;
                reservation_key.owner = nullptr;
                reservation_key.identity = snapshot.state_->identity;
                reservation_keys.push_back(reservation_key);
            }
            reservation_key = {};
            reservation_key.kind = ClaimReservationKey::Kind::connection_epoch;
            reservation_key.context = state_.get();
            reservation_key.epoch = input.connection_epoch;
            reservation_keys.push_back(reservation_key);
            wrapped_owners.push_back(std::move(owner));
            std::vector<unsigned> seen;
            seen.reserve(input.interfaces.size());
            for (const auto &claim : input.interfaces) {
                if (std::find(seen.begin(), seen.end(), claim.interface_number) != seen.end()) {
                    return invalid();
                }
                seen.push_back(claim.interface_number);
                const auto matches = std::count_if(snapshot.interfaces.begin(), snapshot.interfaces.end(),
                    [&](const auto &interface) {
                        return interface.info.interface_number == claim.interface_number &&
                               interface.info.alternate_setting == claim.alternate_setting;
                    });
                if (matches != 1) return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            }
        }

        dtv_internal_result reservation_result{};
        pending.state_->reservation = reserve_claims(std::move(reservation_keys),
                                                      &reservation_result);
        if (!pending.state_->reservation) return reservation_result;

        for (std::size_t index = 0; index < inputs.size(); ++index) {
            const auto &input = inputs[index];
            const auto &snapshot = input.snapshot;
            auto result = claim_checkpoint(clock, cancel, deadline_ns);
            if (result.code != DTV_INTERNAL_RESULT_OK) return result;
            libusb_device_descriptor descriptor{};
            int status = state_->backend->get_device_descriptor(snapshot.state_->device, &descriptor);
            if (status != LIBUSB_SUCCESS) return usb_result(status);
            result = claim_checkpoint(clock, cancel, deadline_ns);
            if (result.code != DTV_INTERNAL_RESULT_OK) return result;
            if (descriptor.idVendor != snapshot.vendor_id || descriptor.idProduct != snapshot.product_id ||
                descriptor.bcdDevice != snapshot.device_version ||
                descriptor.bDeviceClass != snapshot.device_class ||
                descriptor.bDeviceSubClass != snapshot.device_subclass ||
                descriptor.bDeviceProtocol != snapshot.device_protocol ||
                descriptor.bNumConfigurations != snapshot.configuration_count ||
                state_->backend->get_bus_number(snapshot.state_->device) != snapshot.bus_number ||
                state_->backend->get_device_address(snapshot.state_->device) != snapshot.device_address) {
                return usb_result(LIBUSB_ERROR_NO_DEVICE);
            }
            if (state_->backend->supports_connection_identity()) {
                const int fd = wrapped_owners[index]
                    ? wrapped_owners[index]->owned_descriptor : -1;
                UsbConnectionIdentity current{};
                if (!state_->backend->get_connection_identity(snapshot.state_->device, fd, &current) ||
                    !current.valid) return usb_result(LIBUSB_ERROR_IO);
                if (!snapshot.state_->identity.valid || current != snapshot.state_->identity) {
                    return usb_result(LIBUSB_ERROR_NO_DEVICE);
                }
                for (std::size_t previous = 0; previous < identities.size(); ++previous) {
                    if (identities[previous] == current &&
                        inputs[previous].connection_epoch != input.connection_epoch) return invalid();
                }
                identities.push_back(current);
            }
            result = claim_checkpoint(clock, cancel, deadline_ns);
            if (result.code != DTV_INTERNAL_RESULT_OK) return result;
            libusb_config_descriptor *configuration = nullptr;
            status = state_->backend->get_active_config_descriptor(snapshot.state_->device,
                                                                    &configuration);
            if (status == LIBUSB_ERROR_NOT_FOUND) {
                status = state_->backend->get_config_descriptor(snapshot.state_->device, 0,
                                                                 &configuration);
            }
            if (status != LIBUSB_SUCCESS || configuration == nullptr) {
                return status == LIBUSB_SUCCESS ? make_result(DTV_INTERNAL_RESULT_UNSUPPORTED)
                                                : usb_result(status);
            }
            std::unique_ptr<libusb_config_descriptor,
                std::function<void(libusb_config_descriptor *)>> configuration_guard(
                    configuration, [backend = state_->backend](auto *value) {
                        backend->free_config_descriptor(value);
                    });
            for (const auto &claim : input.interfaces) {
                std::size_t matches = 0;
                for (std::uint8_t interface_index = 0;
                     interface_index < configuration->bNumInterfaces; ++interface_index) {
                    const auto &interface = configuration->interface[interface_index];
                    for (int alternate_index = 0; alternate_index < interface.num_altsetting;
                         ++alternate_index) {
                        const auto &alternate = interface.altsetting[alternate_index];
                        if (alternate.bInterfaceNumber == claim.interface_number &&
                            alternate.bAlternateSetting == claim.alternate_setting) ++matches;
                    }
                }
                if (matches != 1) return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            }
            result = claim_checkpoint(clock, cancel, deadline_ns);
            if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        }

        for (std::size_t index = 0; index < inputs.size(); ++index) {
            auto result = claim_checkpoint(clock, cancel, deadline_ns);
            if (result.code != DTV_INTERNAL_RESULT_OK) return result;
            auto owner = std::make_unique<UsbClaimHandleLease>();
            owner->context = state_;
            owner->wrapped_owner = wrapped_owners[index];
            owner->handle = owner->wrapped_owner ? owner->wrapped_owner->handle : nullptr;
            owner->owns_handle = !owner->wrapped_owner;
            owner->claimed_interfaces.reserve(inputs[index].interfaces.size());
            pending.state_->handles.push_back(std::move(owner));
            auto &lease = *pending.state_->handles.back();
            if (lease.owns_handle) {
                const int status = state_->backend->open_device(
                    inputs[index].snapshot.state_->device, &lease.handle);
                if (status != LIBUSB_SUCCESS) return usb_result(status);
                result = claim_checkpoint(clock, cancel, deadline_ns);
                if (result.code != DTV_INTERNAL_RESULT_OK) return result;
            }
            for (const auto &claim : inputs[index].interfaces) {
                result = claim_checkpoint(clock, cancel, deadline_ns);
                if (result.code != DTV_INTERNAL_RESULT_OK) return result;
                const int status = state_->backend->claim_interface(lease.handle,
                                                                     claim.interface_number);
                if (status != LIBUSB_SUCCESS) return claim_result(status);
                lease.claimed_interfaces.push_back(claim.interface_number);
                result = claim_checkpoint(clock, cancel, deadline_ns);
                if (result.code != DTV_INTERNAL_RESULT_OK) return result;
                if (claim.alternate_setting != 0) {
                    const int alternate_status = state_->backend->set_interface_alt_setting(
                        lease.handle, claim.interface_number, claim.alternate_setting);
                    if (alternate_status != LIBUSB_SUCCESS) return claim_result(alternate_status);
                    result = claim_checkpoint(clock, cancel, deadline_ns);
                    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
                }
            }
        }
        *out = std::move(pending);
        return make_result(DTV_INTERNAL_RESULT_OK);
    } catch (...) {
        return exception_result();
    }
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
    auto close_state = handle->close_state;
    begin_handle_close(handle);
    bool start_job = false;
    {
        std::lock_guard lock(close_state->mutex);
        if (!close_state->job_started) {
            close_state->job_started = true;
            start_job = true;
        }
    }
    if (start_job) {
        auto &context = *handle->context;
        {
            std::lock_guard lock(context.lifecycle_mutex);
            context.lifecycle_jobs.emplace_back(
                [handle, close_state, callback = std::move(callback)]() mutable {
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
                handle.reset();
                {
                    std::lock_guard lock(close_state->mutex);
                    close_state->result = result;
                    close_state->job_done = true;
                    close_state->status.store(UsbCloseStatus::closed);
                }
                close_state->condition.notify_all();
            });
        }
        context.lifecycle_condition.notify_one();
    }

    const auto started = std::chrono::steady_clock::now();
    std::unique_lock lock(close_state->mutex);
    while (!close_state->job_done) {
        if (timeout.timeout_ms != UINT64_MAX &&
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(timeout.timeout_ms)) {
            close_state->status.store(UsbCloseStatus::pending);
            return make_result(DTV_INTERNAL_RESULT_NOT_READY);
        }
        close_state->condition.wait_for(lock, std::chrono::milliseconds(5));
        if (timeout.operation_deadline_ns != 0 && timeout.operation_deadline_ns != UINT64_MAX &&
            state_->binding.clock != nullptr &&
            state_->binding.clock->monotonic_now_ns != nullptr &&
            state_->binding.clock->monotonic_now_ns(state_->binding.clock->user) >=
                timeout.operation_deadline_ns && !close_state->job_done) {
            close_state->status.store(UsbCloseStatus::pending);
            return make_result(DTV_INTERNAL_RESULT_NOT_READY);
        }
    }
    return close_state->result;
}

void UsbDevice::begin_close()
{
    if (state_) begin_handle_close(state_->binding.handle);
}

void UsbDevice::finish_close(dtv_internal_result result)
{
    if (!state_ || !state_->binding.handle) return;
    auto handle = state_->binding.handle;
    auto close_state = handle->close_state;
    std::lock_guard lock(close_state->mutex);
    close_state->result = result;
    close_state->status.store(UsbCloseStatus::closed);
    close_state->job_done = true;
    close_state->condition.notify_all();
}

UsbCloseStatus UsbDevice::close_status() const
{
    return state_ && state_->binding.handle
        ? state_->binding.handle->close_state->status.load()
        : UsbCloseStatus::closed;
}

dtv_internal_result UsbDevice::close_result() const
{
    if (!state_ || !state_->binding.handle) return make_result(DTV_INTERNAL_RESULT_OK);
    auto close_state = state_->binding.handle->close_state;
    std::lock_guard lock(close_state->mutex);
    return close_state->result;
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

std::size_t UsbTestDeviceFactory::worker_count(const UsbContext &context)
{
    if (!context.state_) return 0;
    return static_cast<std::size_t>(context.state_->event_thread.joinable()) +
           static_cast<std::size_t>(context.state_->lifecycle_thread.joinable());
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

UsbDeviceSnapshot UsbTestDeviceFactory::snapshot(const UsbContext &context,
                                                  std::uintptr_t raw_device)
{
    UsbDeviceSnapshot snapshot{};
    auto *device = reinterpret_cast<libusb_device *>(raw_device);
    libusb_device_descriptor descriptor{};
    if (context.state_ && context.state_->backend->get_device_descriptor(device, &descriptor) ==
                              LIBUSB_SUCCESS) {
        snapshot.vendor_id = descriptor.idVendor;
        snapshot.product_id = descriptor.idProduct;
        snapshot.device_version = descriptor.bcdDevice;
        snapshot.device_class = descriptor.bDeviceClass;
        snapshot.device_subclass = descriptor.bDeviceSubClass;
        snapshot.device_protocol = descriptor.bDeviceProtocol;
        snapshot.configuration_count = descriptor.bNumConfigurations;
        snapshot.bus_number = context.state_->backend->get_bus_number(device);
        snapshot.device_address = context.state_->backend->get_device_address(device);
    }
    snapshot.state_ = std::make_shared<UsbDeviceSnapshot::State>();
    snapshot.state_->context = context.state_;
    snapshot.state_->device = context.state_->backend->ref_device(device);
    if (context.state_->backend->supports_connection_identity()) {
        (void)context.state_->backend->get_connection_identity(device, -1,
                                                                &snapshot.state_->identity);
    }
    return snapshot;
}

std::unique_ptr<UsbDevice> UsbTestDeviceFactory::open_usb_fd(
    int descriptor, int interface_number, dtv_internal_result *result,
    std::shared_ptr<UsbTransferBackend> backend)
{
    return open_usb_fd_impl(descriptor, interface_number, result, std::move(backend));
}

}
