// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/usb.hpp>

#include "usb-transfer-backend.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
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
    libusb_transfer *allocate_transfer() override
    {
        std::lock_guard lock(mutex_);
        ++allocated_;
        return new libusb_transfer{};
    }

    void free_transfer(libusb_transfer *transfer) override
    {
        std::lock_guard lock(mutex_);
        ++freed_;
        delete transfer;
    }

    int submit_transfer(libusb_transfer *transfer) override
    {
        std::lock_guard lock(mutex_);
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

    int release_interface(libusb_device_handle *, int) override
    {
        std::lock_guard lock(mutex_);
        ++released_;
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

    bool drained_and_closed()
    {
        std::lock_guard lock(mutex_);
        return active_ == 0 && pending_.empty() && allocated_ == freed_ &&
               released_ == 1 && closed_ == 1 && exited_ == 1;
    }

    bool resources_held_for_completion()
    {
        std::lock_guard lock(mutex_);
        return outstanding_ != 0 && allocated_ > freed_ && released_ == 0 && closed_ == 0;
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
    std::size_t allocated_ = 0;
    std::size_t freed_ = 0;
    std::size_t active_ = 0;
    std::size_t outstanding_ = 0;
    std::size_t released_ = 0;
    std::size_t closed_ = 0;
    std::size_t exited_ = 0;
};

bool io_callbacks()
{
    auto backend = std::make_shared<FakeBackend>();
    auto device = dtv::internal::UsbTestDeviceFactory::create(
        backend, reinterpret_cast<libusb_device_handle *>(static_cast<std::uintptr_t>(1)), {1});
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
    return backend->drained_and_closed();
}

bool request_holds_fd_owner()
{
#if defined(__linux__)
    int descriptors[2]{};
    if (pipe(descriptors) != 0) return false;
    const int owned = fcntl(descriptors[0], F_DUPFD_CLOEXEC, 0);
    if (owned < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    auto backend = std::make_shared<FakeBackend>();
    auto device = dtv::internal::UsbTestDeviceFactory::create(
        backend, reinterpret_cast<libusb_device_handle *>(static_cast<std::uintptr_t>(2)), {4}, owned);
    const dtv_usb_io io = *device->io();
    unsigned char bytes[]{'f', 'd'};
    dtv_internal_result result{};
    std::thread worker([&] {
        result = io.bulk_out(io.user, 0x05, {bytes, sizeof(bytes)}, {1000, 0});
    });
    const bool submitted = backend->wait_for_submissions(1);
    device.reset();
    const bool still_owned = fcntl(owned, F_GETFD) >= 0;
    const bool held_until_completion = backend->resources_held_for_completion();
    const bool completed = backend->complete(0x05, LIBUSB_TRANSFER_COMPLETED, 2);
    worker.join();
    const bool caller_fd_open = fcntl(descriptors[0], F_GETFD) >= 0;
    const bool owned_fd_closed = fcntl(owned, F_GETFD) < 0 && errno == EBADF;
    close(descriptors[0]);
    close(descriptors[1]);
    return submitted && still_owned && held_until_completion && completed && caller_fd_open && owned_fd_closed &&
           result.code == DTV_INTERNAL_RESULT_OK && backend->drained_and_closed();
#else
    return true;
#endif
}

}

int main()
{
    return io_callbacks() && request_holds_fd_owner() ? 0 : 1;
}
