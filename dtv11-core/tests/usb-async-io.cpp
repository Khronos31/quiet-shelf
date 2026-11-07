// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/usb.hpp>

#include "usb-transfer-backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

struct Completion {
    libusb_transfer *transfer;
    libusb_transfer_status status;
    int actual_length;
    std::vector<unsigned char> input;
};

class FakeBackend final : public dtv::internal::UsbTransferBackend {
public:
    FakeBackend()
    {
        config_.bNumInterfaces = 2;
        config_.interface = interfaces_;
        for (std::size_t i = 0; i < 2; ++i) {
            interfaces_[i].num_altsetting = 1;
            interfaces_[i].altsetting = &alternate_settings_[i];
            alternate_settings_[i].bInterfaceNumber = i == 0 ? 1 : 3;
            alternate_settings_[i].bAlternateSetting = 0;
        }
    }

    int init_context(libusb_context **context) override
    {
        *context = reinterpret_cast<libusb_context *>(static_cast<std::uintptr_t>(1));
        return LIBUSB_SUCCESS;
    }

    libusb_transfer *allocate_transfer() override
    {
        std::lock_guard lock(mutex_);
        ++allocated_;
        return static_cast<libusb_transfer *>(std::calloc(1, sizeof(libusb_transfer)));
    }

    void free_transfer(libusb_transfer *transfer) override
    {
        std::lock_guard lock(mutex_);
        ++freed_;
        std::free(transfer);
        condition_.notify_all();
    }

    int submit_transfer(libusb_transfer *transfer) override
    {
        std::lock_guard lock(mutex_);
        if (next_submit_result_ != LIBUSB_SUCCESS) {
            const int result = next_submit_result_;
            next_submit_result_ = LIBUSB_SUCCESS;
            condition_.notify_all();
            return result;
        }
        pending_.push_back(transfer);
        ++outstanding_;
        if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
            const auto *setup = libusb_control_transfer_get_setup(transfer);
            if ((setup->bmRequestType & LIBUSB_ENDPOINT_IN) == 0 && setup->wLength != 0) {
                const auto *data = libusb_control_transfer_get_data(transfer);
                output_[setup->bRequest] = {data, data + setup->wLength};
            }
        } else if ((transfer->endpoint & LIBUSB_ENDPOINT_IN) == 0 && transfer->length != 0) {
            output_[transfer->endpoint] = {transfer->buffer, transfer->buffer + transfer->length};
        }
        condition_.notify_all();
        return LIBUSB_SUCCESS;
    }

    int cancel_transfer(libusb_transfer *transfer) override
    {
        std::lock_guard lock(mutex_);
        ++cancelled_;
        if (cancel_endpoint_cohort_) {
            for (auto it = pending_.begin(); it != pending_.end();) {
                if ((*it)->type != LIBUSB_TRANSFER_TYPE_CONTROL &&
                    (*it)->endpoint == transfer->endpoint) {
                    completions_.push_back({*it, LIBUSB_TRANSFER_CANCELLED, 0, {}});
                    it = pending_.erase(it);
                    ++active_;
                } else {
                    ++it;
                }
            }
        }
        condition_.notify_all();
        return LIBUSB_SUCCESS;
    }

    void cancel_endpoint_cohort(bool enabled)
    {
        std::lock_guard lock(mutex_);
        cancel_endpoint_cohort_ = enabled;
    }

    void fail_next_submit(int result)
    {
        std::lock_guard lock(mutex_);
        next_submit_result_ = result;
    }

    int handle_events(libusb_context *, timeval *timeout) override
    {
        Completion completion{};
        {
            std::unique_lock lock(mutex_);
            event_threads_.push_back(std::this_thread::get_id());
            const auto duration = std::chrono::seconds(timeout->tv_sec) +
                                  std::chrono::microseconds(timeout->tv_usec);
            condition_.wait_for(lock, duration, [&] { return !completions_.empty(); });
            if (completions_.empty()) return LIBUSB_SUCCESS;
            completion = std::move(completions_.front());
            completions_.erase(completions_.begin());
            --active_;
        }
        auto *transfer = completion.transfer;
        transfer->status = completion.status;
        transfer->actual_length = completion.actual_length;
        if (!completion.input.empty()) {
            auto *target = transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL
                ? libusb_control_transfer_get_data(transfer)
                : transfer->buffer;
            std::copy_n(completion.input.data(),
                        (std::min)(completion.input.size(),
                                   static_cast<std::size_t>((std::max)(completion.actual_length, 0))),
                        target);
        }
        transfer->callback(transfer);
        {
            std::lock_guard lock(mutex_);
            --outstanding_;
        }
        return LIBUSB_SUCCESS;
    }

    int get_device_descriptor(libusb_device *, libusb_device_descriptor *descriptor) override
    {
        descriptor->idVendor = 0x1234;
        descriptor->idProduct = 0x5678;
        descriptor->bcdDevice = 1;
        descriptor->bNumConfigurations = 1;
        return LIBUSB_SUCCESS;
    }

    std::uint8_t get_bus_number(libusb_device *) override { return 1; }
    std::uint8_t get_device_address(libusb_device *) override { return 2; }

    int get_active_config_descriptor(libusb_device *, libusb_config_descriptor **descriptor) override
    {
        *descriptor = &config_;
        return LIBUSB_SUCCESS;
    }

    int get_config_descriptor(libusb_device *, std::uint8_t,
                              libusb_config_descriptor **descriptor) override
    {
        *descriptor = &config_;
        return LIBUSB_SUCCESS;
    }

    void free_config_descriptor(libusb_config_descriptor *) override {}

    int open_device(libusb_device *, libusb_device_handle **handle) override
    {
        std::lock_guard lock(mutex_);
        ++opened_;
        *handle = reinterpret_cast<libusb_device_handle *>(static_cast<std::uintptr_t>(4));
        return LIBUSB_SUCCESS;
    }

    int claim_interface(libusb_device_handle *, int interface_number) override
    {
        std::lock_guard lock(mutex_);
        claimed_.push_back(interface_number);
        return LIBUSB_SUCCESS;
    }

    int set_interface_alt_setting(libusb_device_handle *, int, int) override
    {
        return LIBUSB_SUCCESS;
    }

    int wrap_sys_device(libusb_context *, std::intptr_t descriptor,
                        libusb_device_handle **handle) override
    {
        std::lock_guard lock(mutex_);
        ++wrapped_;
        wrapped_descriptor_ = static_cast<int>(descriptor);
        *handle = reinterpret_cast<libusb_device_handle *>(static_cast<std::uintptr_t>(5));
        return LIBUSB_SUCCESS;
    }

    libusb_device *get_device(libusb_device_handle *) override
    {
        return reinterpret_cast<libusb_device *>(static_cast<std::uintptr_t>(3));
    }

    libusb_device *ref_device(libusb_device *device) override { return device; }
    void unref_device(libusb_device *) override {}

    int release_interface(libusb_device_handle *, int interface_number) override
    {
        std::lock_guard lock(mutex_);
        ++released_;
        released_interfaces_.push_back(interface_number);
        return LIBUSB_SUCCESS;
    }

    void close_handle(libusb_device_handle *) override
    {
        std::lock_guard lock(mutex_);
        ++closed_;
    }

    void exit_context(libusb_context *) override
    {
        std::lock_guard lock(mutex_);
        ++exited_;
    }

    bool wait_for_submissions(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        const auto ready = condition_.wait_for(lock, std::chrono::seconds(2), [&] {
            return pending_.size() >= count;
        });
        return ready;
    }

    bool complete(std::uint8_t key, libusb_transfer_status status, int actual_length,
                  std::vector<unsigned char> input = {})
    {
        std::lock_guard lock(mutex_);
        const auto it = std::find_if(pending_.begin(), pending_.end(), [&](auto *transfer) {
            if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
                const auto *setup = libusb_control_transfer_get_setup(transfer);
                return setup->bRequest == key;
            }
            return transfer->endpoint == key;
        });
        if (it == pending_.end()) return false;
        auto *transfer = *it;
        pending_.erase(it);
        completions_.push_back({transfer, status, actual_length, std::move(input)});
        ++active_;
        condition_.notify_all();
        return true;
    }

    std::vector<unsigned char> output(std::uint8_t key)
    {
        std::lock_guard lock(mutex_);
        return output_[key];
    }

    bool drained_and_closed(std::size_t expected_releases = 1)
    {
        std::lock_guard lock(mutex_);
        return active_ == 0 && pending_.empty() && allocated_ == freed_ &&
               released_ == expected_releases && closed_ == 1 && exited_ == 1;
    }

    bool drained_and_released()
    {
        std::lock_guard lock(mutex_);
        return active_ == 0 && pending_.empty() && allocated_ == freed_ &&
               released_ == 1 && closed_ == 1;
    }

    bool resources_held_for_completion()
    {
        std::lock_guard lock(mutex_);
        return outstanding_ != 0 && allocated_ > freed_ && released_ == 0 && closed_ == 0;
    }

    bool owner_retained()
    {
        std::lock_guard lock(mutex_);
        return released_ == 0 && closed_ == 0 && exited_ == 0;
    }

    bool wait_for_freed(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(1), [&] {
            return freed_ >= count;
        });
    }

    bool wait_for_cancel(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(1), [&] {
            return cancelled_ >= count;
        });
    }

    std::size_t cancel_count()
    {
        std::lock_guard lock(mutex_);
        return cancelled_;
    }

    bool opened_one_device_with_claims(std::vector<int> claims)
    {
        std::lock_guard lock(mutex_);
        return opened_ == 1 && claimed_ == claims;
    }

    bool wrapped_one_fd(int descriptor)
    {
        std::lock_guard lock(mutex_);
        return wrapped_ == 1 && wrapped_descriptor_ == descriptor;
    }

    int wrapped_descriptor()
    {
        std::lock_guard lock(mutex_);
        return wrapped_descriptor_;
    }

    bool released_in_order(std::vector<int> interfaces)
    {
        std::lock_guard lock(mutex_);
        return released_interfaces_ == interfaces;
    }

    bool event_thread_is_separate(std::thread::id caller)
    {
        std::lock_guard lock(mutex_);
        return !event_threads_.empty() &&
               std::all_of(event_threads_.begin(), event_threads_.end(),
                           [&](auto id) { return id != caller; }) &&
               std::all_of(event_threads_.begin(), event_threads_.end(),
                           [&](auto id) { return id == event_threads_.front(); });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<libusb_transfer *> pending_;
    std::vector<Completion> completions_;
    std::vector<std::thread::id> event_threads_;
    std::vector<unsigned char> output_[256];
    libusb_interface_descriptor alternate_settings_[2]{};
    libusb_interface interfaces_[2]{};
    libusb_config_descriptor config_{};
    std::size_t allocated_ = 0;
    std::size_t freed_ = 0;
    std::size_t active_ = 0;
    std::size_t outstanding_ = 0;
    std::size_t released_ = 0;
    std::size_t closed_ = 0;
    std::size_t exited_ = 0;
    std::size_t cancelled_ = 0;
    bool cancel_endpoint_cohort_ = false;
    int next_submit_result_ = LIBUSB_SUCCESS;
    std::size_t opened_ = 0;
    std::size_t wrapped_ = 0;
    int wrapped_descriptor_ = -1;
    std::vector<int> claimed_;
    std::vector<int> released_interfaces_;
};

bool io_callbacks()
{
    auto backend = std::make_shared<FakeBackend>();
    const bool callbacks_completed = [&] {
        auto usb = dtv::internal::UsbTestDeviceFactory::context(backend);
        auto snapshot = dtv::internal::UsbTestDeviceFactory::snapshot(usb);
        const std::array claims{
            dtv::internal::UsbInterfaceClaim{1, 0},
            dtv::internal::UsbInterfaceClaim{3, 0}
        };
        auto device = usb.open(snapshot, claims);
        if (!device || device->result().code != DTV_INTERNAL_RESULT_OK || !device->io()) {
            return false;
        }
        const dtv_usb_io io = *device->io();

    const unsigned char control_out_bytes[]{'c', 't', 'l'};
    dtv_buffer control_out{const_cast<unsigned char *>(control_out_bytes), 3, 3};
    dtv_internal_result control_out_result{};
    std::thread::id control_out_caller;
    std::thread control_out_thread([&] {
        control_out_caller = std::this_thread::get_id();
        control_out_result = io.control(
            io.user, static_cast<std::uint8_t>(static_cast<unsigned int>(LIBUSB_REQUEST_TYPE_VENDOR) |
                                               static_cast<unsigned int>(LIBUSB_ENDPOINT_OUT)), 11, 2, 3,
            &control_out, {1000, 0});
    });
    if (!backend->wait_for_submissions(1) || !backend->complete(11, LIBUSB_TRANSFER_COMPLETED, 3)) {
        control_out_thread.join();
        return false;
    }
    control_out_thread.join();
    if (control_out_result.code != DTV_INTERNAL_RESULT_OK || control_out_result.detail != 0 ||
        control_out.size != 3 || backend->output(11) !=
            std::vector<unsigned char>(control_out_bytes, control_out_bytes + 3)) {
        return false;
    }

    unsigned char control_in_bytes[4]{};
    dtv_buffer control_in{control_in_bytes, sizeof(control_in_bytes), 0};
    dtv_internal_result control_in_result{};
    std::thread control_in_thread([&] {
        control_in_result = io.control(
            io.user, static_cast<std::uint8_t>(static_cast<unsigned int>(LIBUSB_REQUEST_TYPE_VENDOR) |
                                               static_cast<unsigned int>(LIBUSB_ENDPOINT_IN)), 12, 0, 0,
            &control_in, {1000, 0});
    });
    if (!backend->wait_for_submissions(1) || !backend->complete(
            12, LIBUSB_TRANSFER_COMPLETED, 2, {'i', 'n'})) {
        control_in_thread.join();
        return false;
    }
    control_in_thread.join();
    if (control_in_result.code != DTV_INTERNAL_RESULT_OK || control_in.size != 2 ||
        std::string(control_in_bytes, control_in_bytes + 2) != "in") {
        return false;
    }

    const unsigned char bulk_out_original[]{'o', 'u', 't'};
    unsigned char bulk_out_bytes[]{'o', 'u', 't'};
    dtv_internal_result bulk_out_result{};
    std::thread::id bulk_out_caller;
    std::thread bulk_out_thread([&] {
        bulk_out_caller = std::this_thread::get_id();
        bulk_out_result = io.bulk_out(io.user, 0x01,
            {bulk_out_bytes, sizeof(bulk_out_bytes)}, {1000, 0});
    });
    if (!backend->wait_for_submissions(1)) {
        bulk_out_thread.join();
        return false;
    }
    std::fill(std::begin(bulk_out_bytes), std::end(bulk_out_bytes), 'x');
    if (!backend->complete(0x01, LIBUSB_TRANSFER_COMPLETED, 3)) {
        bulk_out_thread.join();
        return false;
    }
    bulk_out_thread.join();
    if (bulk_out_result.code != DTV_INTERNAL_RESULT_OK || bulk_out_result.detail != 3 ||
        backend->output(0x01) != std::vector<unsigned char>(bulk_out_original,
                                                            bulk_out_original + 3)) {
        return false;
    }

    unsigned char bulk_in_bytes[4]{};
    dtv_buffer bulk_in{bulk_in_bytes, sizeof(bulk_in_bytes), 0};
    dtv_internal_result bulk_in_result{};
    std::thread bulk_in_thread([&] {
        bulk_in_result = io.bulk_in(io.user, 0x82, &bulk_in, {1000, 0});
    });
    if (!backend->wait_for_submissions(1) || !backend->complete(
            0x82, LIBUSB_TRANSFER_COMPLETED, 3, {'b', 'u', 'l'})) {
        bulk_in_thread.join();
        return false;
    }
    bulk_in_thread.join();
    if (bulk_in_result.code != DTV_INTERNAL_RESULT_OK || bulk_in.size != 3 ||
        std::string(bulk_in_bytes, bulk_in_bytes + 3) != "bul") {
        return false;
    }

    unsigned char interrupt_bytes[3]{};
    dtv_buffer interrupt_in{interrupt_bytes, sizeof(interrupt_bytes), 0};
    dtv_internal_result interrupt_result{};
    std::thread interrupt_thread([&] {
        interrupt_result = io.interrupt_in(io.user, 0x83, &interrupt_in, {1000, 0});
    });
    if (!backend->wait_for_submissions(1) || !backend->complete(
            0x83, LIBUSB_TRANSFER_COMPLETED, 1, {'i'})) {
        interrupt_thread.join();
        return false;
    }
    interrupt_thread.join();
    if (interrupt_result.code != DTV_INTERNAL_RESULT_OK || interrupt_in.size != 1 ||
        interrupt_bytes[0] != 'i' || !backend->event_thread_is_separate(control_out_caller) ||
        !backend->event_thread_is_separate(bulk_out_caller)) {
        return false;
    }

    unsigned char short_bytes[]{'s', 'h', 't'};
    dtv_internal_result short_result{};
    std::thread short_thread([&] {
        short_result = io.bulk_out(io.user, 0x02, {short_bytes, sizeof(short_bytes)}, {1000, 0});
    });
    if (!backend->wait_for_submissions(1) || !backend->complete(0x02, LIBUSB_TRANSFER_COMPLETED, 2)) {
        short_thread.join();
        return false;
    }
    short_thread.join();
    if (short_result.code != DTV_INTERNAL_RESULT_USB_IO || short_result.detail != 2) {
        return false;
    }

    unsigned char error_bytes[3]{};
    dtv_buffer error_in{error_bytes, sizeof(error_bytes), 0};
    dtv_internal_result error_result{};
    std::thread error_thread([&] {
        error_result = io.interrupt_in(io.user, 0x84, &error_in, {1000, 0});
    });
    if (!backend->wait_for_submissions(1) || !backend->complete(
            0x84, LIBUSB_TRANSFER_STALL, 1, {'e'})) {
        error_thread.join();
        return false;
    }
    error_thread.join();
    if (error_result.code != DTV_INTERNAL_RESULT_USB_IO ||
        error_result.detail != static_cast<std::uint32_t>(-LIBUSB_ERROR_PIPE) ||
        error_in.size != 1 || error_bytes[0] != 'e') {
        return false;
    }

    dtv_internal_result parallel_results[2]{};
    std::thread::id parallel_callers[2]{};
    std::thread parallel_threads[2]{
        std::thread([&] {
            parallel_callers[0] = std::this_thread::get_id();
            const unsigned char bytes[]{'a', 'a'};
            parallel_results[0] = io.bulk_out(io.user, 0x06, {bytes, sizeof(bytes)}, {1000, 0});
        }),
        std::thread([&] {
            parallel_callers[1] = std::this_thread::get_id();
            const unsigned char bytes[]{'b', 'b'};
            parallel_results[1] = io.bulk_out(io.user, 0x07, {bytes, sizeof(bytes)}, {1000, 0});
        })
    };
    const bool parallel_submitted = backend->wait_for_submissions(2);
    const bool first_completed = backend->complete(0x06, LIBUSB_TRANSFER_COMPLETED, 2);
    const bool second_completed = backend->complete(0x07, LIBUSB_TRANSFER_COMPLETED, 2);
    parallel_threads[0].join();
    parallel_threads[1].join();
    if (!parallel_submitted || !first_completed || !second_completed ||
        parallel_results[0].code != DTV_INTERNAL_RESULT_OK ||
        parallel_results[1].code != DTV_INTERNAL_RESULT_OK ||
        !backend->event_thread_is_separate(parallel_callers[0]) ||
        !backend->event_thread_is_separate(parallel_callers[1])) {
        return false;
    }
        device.reset();
        return true;
    }();
    const bool drained = backend->drained_and_closed(2);
    const bool opened = backend->opened_one_device_with_claims({1, 3});
    const bool released = backend->released_in_order({3, 1});
    return callbacks_completed && drained && opened && released;
}

bool request_holds_fd_owner()
{
#if defined(__linux__)
    int descriptors[2]{};
    if (pipe(descriptors) != 0) return false;
    auto backend = std::make_shared<FakeBackend>();
    dtv_internal_result open_result{};
    auto device = dtv::internal::UsbTestDeviceFactory::open_usb_fd(
        descriptors[0], 1, &open_result, backend);
    if (!device || open_result.code != DTV_INTERNAL_RESULT_OK || !device->io()) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    const int owned = backend->wrapped_descriptor();
    const dtv_usb_io io = *device->io();
    unsigned char bytes[]{'f', 'd'};
    dtv_internal_result result{};
    std::thread worker([&] {
        result = io.bulk_out(io.user, 0x05, {bytes, sizeof(bytes)}, {1000, 0});
    });
    const bool submitted = backend->wait_for_submissions(1);
    const bool still_owned = fcntl(owned, F_GETFD) >= 0;
    const bool held_until_completion = backend->resources_held_for_completion();
    const bool completed = backend->complete(0x05, LIBUSB_TRANSFER_COMPLETED, 2);
    worker.join();
    const bool caller_fd_open = fcntl(descriptors[0], F_GETFD) >= 0;
    const auto closed = device->run_close_callback({}, {UINT64_MAX, UINT64_MAX});
    device.reset();
    const bool owned_fd_closed = fcntl(owned, F_GETFD) < 0 && errno == EBADF;
    close(descriptors[0]);
    close(descriptors[1]);
    return submitted && owned >= 0 && owned != descriptors[0] &&
           backend->wrapped_one_fd(owned) && still_owned && held_until_completion && completed &&
           caller_fd_open && owned_fd_closed && result.code == DTV_INTERNAL_RESULT_OK &&
           closed.code == DTV_INTERNAL_RESULT_OK &&
           backend->drained_and_closed() && backend->released_in_order({1});
#else
    return true;
#endif
}

std::uint64_t steady_now_ns(void *)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

dtv_internal_result test_is_cancelled(void *user, const dtv_cancel *, std::uint32_t *cancelled)
{
    if (user == nullptr || cancelled == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    *cancelled = static_cast<std::atomic<bool> *>(user)->load() ? 1u : 0u;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

bool cancel_and_deadline_wake_waiters()
{
    auto backend = std::make_shared<FakeBackend>();
    auto usb = dtv::internal::UsbTestDeviceFactory::context(backend);
    auto snapshot = dtv::internal::UsbTestDeviceFactory::snapshot(usb);
    auto device = usb.open(snapshot, 1);
    if (!device || !device->io()) return false;
    const auto io = *device->io();
    std::atomic<bool> cancelled{false};
    dtv_runtime_clock clock{&cancelled, steady_now_ns, nullptr, test_is_cancelled};
    auto *token = reinterpret_cast<const dtv_cancel *>(static_cast<std::uintptr_t>(1));

    unsigned char cancel_bytes[4]{0x5a, 0x5a, 0x5a, 0x5a};
    dtv_buffer cancel_buffer{cancel_bytes, sizeof(cancel_bytes), sizeof(cancel_bytes)};
    dtv_internal_result cancel_result{};
    dtv::internal::UsbOperationTerminalReason cancel_terminal{};
    std::thread cancel_thread([&] {
        dtv::internal::UsbOperationScope operation(&clock, token, UINT64_MAX);
        cancel_result = io.bulk_in(io.user, 0x09, &cancel_buffer, {UINT64_MAX, UINT64_MAX});
        cancel_terminal = dtv::internal::current_usb_terminal_reason();
    });
    if (!backend->wait_for_submissions(1)) {
        cancelled.store(true);
        cancel_thread.join();
        return false;
    }
    cancelled.store(true);
    const bool cancel_requested = backend->wait_for_cancel(1);
    cancel_thread.join();
    const bool cancel_woke = cancel_result.code == DTV_INTERNAL_RESULT_TIMEOUT &&
        cancel_terminal == dtv::internal::UsbOperationTerminalReason::cancelled &&
        cancel_buffer.size == 0 && cancel_bytes[0] == 0x5a && cancel_requested &&
        backend->resources_held_for_completion();
    const bool cancel_completed_late = backend->complete(
        0x09, LIBUSB_TRANSFER_CANCELLED, 2, {'x', 'y'});
    const bool cancel_double_completion = !backend->complete(
        0x09, LIBUSB_TRANSFER_CANCELLED, 2, {'x', 'y'});
    const bool cancel_late_unchanged = cancel_bytes[0] == 0x5a && cancel_bytes[1] == 0x5a;

    unsigned char deadline_bytes[4]{0x6b, 0x6b, 0x6b, 0x6b};
    dtv_buffer deadline_buffer{deadline_bytes, sizeof(deadline_bytes), sizeof(deadline_bytes)};
    dtv_internal_result deadline_result{};
    dtv::internal::UsbOperationTerminalReason deadline_terminal{};
    const auto deadline = steady_now_ns(nullptr) + 30000000;
    std::thread deadline_thread([&] {
        dtv::internal::UsbOperationScope operation(&clock, nullptr, deadline);
        deadline_result = io.bulk_in(io.user, 0x0a, &deadline_buffer,
                                     {UINT64_MAX, deadline});
        deadline_terminal = dtv::internal::current_usb_terminal_reason();
    });
    const bool deadline_submitted = backend->wait_for_submissions(1);
    const bool deadline_cancel_requested = backend->wait_for_cancel(2);
    deadline_thread.join();
    const bool deadline_woke = deadline_result.code == DTV_INTERNAL_RESULT_TIMEOUT &&
        deadline_terminal == dtv::internal::UsbOperationTerminalReason::deadline &&
        deadline_buffer.size == 0 && deadline_bytes[0] == 0x6b &&
        backend->resources_held_for_completion();
    const bool deadline_completed_late = backend->complete(
        0x0a, LIBUSB_TRANSFER_CANCELLED, 2, {'p', 'q'});
    const bool deadline_double_completion = !backend->complete(
        0x0a, LIBUSB_TRANSFER_CANCELLED, 2, {'p', 'q'});
    const bool deadline_late_unchanged = deadline_bytes[0] == 0x6b && deadline_bytes[1] == 0x6b;
    device.reset();
    return cancel_woke && cancel_completed_late && cancel_double_completion && cancel_late_unchanged &&
           deadline_submitted && deadline_cancel_requested && deadline_woke &&
           deadline_completed_late && deadline_double_completion && deadline_late_unchanged &&
           backend->drained_and_released();
}

bool diagnostic_check(const char *name, bool passed)
{
    if (!passed) std::fprintf(stderr, "condition failed: %s\n", name);
    return passed;
}

bool submit_failure_clears_previous_terminal_reason()
{
    auto backend = std::make_shared<FakeBackend>();
    auto usb = dtv::internal::UsbTestDeviceFactory::context(backend);
    auto snapshot = dtv::internal::UsbTestDeviceFactory::snapshot(usb);
    auto device = usb.open(snapshot, 1);
    if (!device || !device->io()) return false;
    const auto io = *device->io();
    std::atomic<bool> cancelled{false};
    dtv_runtime_clock clock{&cancelled, steady_now_ns, nullptr, test_is_cancelled};
    auto *token = reinterpret_cast<const dtv_cancel *>(static_cast<std::uintptr_t>(1));
    unsigned char first_bytes[4]{0x5a, 0x5a, 0x5a, 0x5a};
    unsigned char second_bytes[4]{};
    dtv_buffer first_buffer{first_bytes, sizeof(first_bytes), sizeof(first_bytes)};
    dtv_buffer second_buffer{second_bytes, sizeof(second_bytes), sizeof(second_bytes)};
    dtv_internal_result first_result{};
    dtv_internal_result second_result{};
    dtv::internal::UsbOperationTerminalReason first_terminal{};
    dtv::internal::UsbOperationTerminalReason second_terminal{};
    std::thread operation_thread([&] {
        dtv::internal::UsbOperationScope operation(&clock, token, UINT64_MAX);
        first_result = io.bulk_in(io.user, 0x09, &first_buffer, {UINT64_MAX, UINT64_MAX});
        first_terminal = dtv::internal::current_usb_terminal_reason();
        second_result = io.bulk_in(io.user, 0x0a, &second_buffer, {UINT64_MAX, UINT64_MAX});
        second_terminal = dtv::internal::current_usb_terminal_reason();
    });
    const bool first_submitted = backend->wait_for_submissions(1);
    backend->fail_next_submit(LIBUSB_ERROR_NO_MEM);
    cancelled.store(true);
    const bool cancel_requested = backend->wait_for_cancel(1);
    operation_thread.join();
    const bool first_cancelled = first_result.code == DTV_INTERNAL_RESULT_TIMEOUT &&
        first_terminal == dtv::internal::UsbOperationTerminalReason::cancelled &&
        first_buffer.size == 0 && first_bytes[0] == 0x5a;
    const bool second_failed_without_stale_reason =
        second_result.code == DTV_INTERNAL_RESULT_USB_IO && second_result.detail == 11 &&
        second_terminal == dtv::internal::UsbOperationTerminalReason::none;
    const bool first_request_retained = backend->resources_held_for_completion();
    const bool late_completion = backend->complete(
        0x09, LIBUSB_TRANSFER_CANCELLED, 2, {'x', 'y'});
    const bool late_buffer_unchanged = first_bytes[0] == 0x5a && first_bytes[1] == 0x5a;
    device.reset();
    const bool drained = backend->drained_and_released();
    const bool passed = first_submitted && cancel_requested && first_cancelled &&
        second_failed_without_stale_reason && first_request_retained && late_completion &&
        late_buffer_unchanged && drained;
    if (!passed) {
        std::fprintf(stderr, "fixture conditions failed:");
        if (!first_submitted) std::fprintf(stderr, " first_submitted");
        if (!cancel_requested) std::fprintf(stderr, " cancel_requested");
        if (!first_cancelled) std::fprintf(stderr, " first_cancelled");
        if (!second_failed_without_stale_reason) {
            std::fprintf(stderr, " second_failed_without_stale_reason");
        }
        if (!first_request_retained) std::fprintf(stderr, " first_request_retained");
        if (!late_completion) std::fprintf(stderr, " late_completion");
        if (!late_buffer_unchanged) std::fprintf(stderr, " late_buffer_unchanged");
        if (!drained) std::fprintf(stderr, " drained");
        std::fprintf(stderr, "\n");
    }
    return passed;
}

bool close_callback_pending_and_failure()
{
    auto backend = std::make_shared<FakeBackend>();
    auto usb = dtv::internal::UsbTestDeviceFactory::context(backend);
    auto snapshot = dtv::internal::UsbTestDeviceFactory::snapshot(usb);
    auto device = usb.open(snapshot, 1);
    if (!device || !device->io()) return false;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool finish = false;
    std::size_t calls = 0;
    std::thread::id close_thread;
    dtv_internal_result cleanup_io_result{};
    const auto io = *device->io();
    auto callback = [&] {
        const unsigned char bytes[]{'c'};
        cleanup_io_result = io.bulk_out(io.user, 0x0c, {bytes, sizeof(bytes)}, {1000, 0});
        std::unique_lock lock(mutex);
        ++calls;
        close_thread = std::this_thread::get_id();
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return finish; });
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 42);
    };
    const auto pending = device->run_close_callback(callback, {0, 0});
    const bool cleanup_submitted = backend->wait_for_submissions(1);
    const bool cleanup_completed = backend->complete(0x0c, LIBUSB_TRANSFER_COMPLETED, 1);
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(1), [&] { return entered; })) {
            finish = true;
            condition.notify_all();
            return false;
        }
    }
    const auto rejected = device->io()->bulk_out(
        device->io()->user, 0x01, {nullptr, 0}, {100, 0});
    const bool pending_state = pending.code == DTV_INTERNAL_RESULT_NOT_READY &&
        device->close_status() == dtv::internal::UsbCloseStatus::pending &&
        backend->owner_retained();
    {
        std::lock_guard lock(mutex);
        finish = true;
    }
    condition.notify_all();
    const auto closed = device->run_close_callback({}, {UINT64_MAX, UINT64_MAX});
    const bool closed_state = device->close_status() == dtv::internal::UsbCloseStatus::closed;
    const bool close_is_separate = close_thread != std::this_thread::get_id();
    device.reset();
    return diagnostic_check("pending_state", pending_state) &&
        diagnostic_check("ordinary_io_rejected", rejected.code == DTV_INTERNAL_RESULT_NOT_READY) &&
        diagnostic_check("close_result", closed.code == DTV_INTERNAL_RESULT_USB_IO &&
                         closed.detail == 42) &&
        diagnostic_check("closed_state", closed_state) &&
        diagnostic_check("cleanup_submitted", cleanup_submitted) &&
        diagnostic_check("cleanup_completed", cleanup_completed) &&
        diagnostic_check("cleanup_io_result", cleanup_io_result.code == DTV_INTERNAL_RESULT_OK) &&
        diagnostic_check("close_callback_separate_thread", close_is_separate) &&
        diagnostic_check("close_callback_once", calls == 1) &&
        diagnostic_check("drained_and_released", backend->drained_and_released());
}

bool endpoint_cancel_cohort_is_not_success()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->cancel_endpoint_cohort(true);
    auto usb = dtv::internal::UsbTestDeviceFactory::context(backend);
    auto snapshot = dtv::internal::UsbTestDeviceFactory::snapshot(usb);
    auto device = usb.open(snapshot, 1);
    if (!device || !device->io()) return false;
    const auto io = *device->io();
    std::atomic<bool> cancelled{false};
    dtv_runtime_clock clock{&cancelled, steady_now_ns, nullptr, test_is_cancelled};
    auto *token = reinterpret_cast<const dtv_cancel *>(static_cast<std::uintptr_t>(1));
    unsigned char first_bytes[2]{};
    unsigned char second_bytes[2]{};
    dtv_buffer first_buffer{first_bytes, sizeof(first_bytes), 0};
    dtv_buffer second_buffer{second_bytes, sizeof(second_bytes), 0};
    dtv_internal_result first_result{};
    dtv_internal_result second_result{};
    std::thread first([&] {
        dtv::internal::UsbOperationScope operation(&clock, token, UINT64_MAX);
        first_result = io.bulk_in(io.user, 0x0b, &first_buffer, {UINT64_MAX, UINT64_MAX});
    });
    std::thread second([&] {
        second_result = io.bulk_in(io.user, 0x0b, &second_buffer, {UINT64_MAX, UINT64_MAX});
    });
    const bool submitted = backend->wait_for_submissions(2);
    cancelled.store(true);
    const bool cancel_requested = backend->wait_for_cancel(1);
    first.join();
    second.join();
    const bool released = backend->wait_for_freed(2);
    device.reset();
    return submitted && cancel_requested && released &&
        first_result.code == DTV_INTERNAL_RESULT_TIMEOUT &&
        second_result.code == DTV_INTERNAL_RESULT_USB_IO &&
        second_result.code != DTV_INTERNAL_RESULT_OK && backend->drained_and_released();
}

bool run_fixture(const char *name, bool (*fixture)())
{
    const bool passed = fixture();
    if (!passed) std::fprintf(stderr, "fixture failed: %s\n", name);
    return passed;
}

}

int main()
{
    return run_fixture("io_callbacks", io_callbacks) &&
           run_fixture("request_holds_fd_owner", request_holds_fd_owner) &&
           run_fixture("cancel_and_deadline_wake_waiters", cancel_and_deadline_wake_waiters) &&
           run_fixture("submit_failure_clears_previous_terminal_reason",
                       submit_failure_clears_previous_terminal_reason) &&
           run_fixture("close_callback_pending_and_failure", close_callback_pending_and_failure) &&
           run_fixture("endpoint_cancel_cohort_is_not_success",
                       endpoint_cancel_cohort_is_not_success)
        ? 0 : 1;
}
