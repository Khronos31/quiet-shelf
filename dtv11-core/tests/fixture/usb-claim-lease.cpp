/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <map>
#include <limits>
#include <iterator>
#include <initializer_list>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <vector>

#include <dtv/internal/discovery.hpp>
#include <dtv/internal/clock.hpp>
#include <dtv/internal/l3-registry.hpp>
#include <dtv/spi/device_ops.hpp>

#include "usb-transfer-backend.hpp"

extern "C" void dtv_load_scanfixture(void);
dtv_device_id scan_firmware_device_id();
dtv_function scan_firmware_function();
std::uint64_t scan_firmware_epoch();
std::size_t scan_firmware_handle_index();
void reset_scan_firmware_records();
void set_scan_firmware_same_id_mode(std::uint32_t);
unsigned scan_firmware_create_calls();
unsigned scan_firmware_open_calls();
unsigned scan_firmware_identify_calls();
unsigned scan_firmware_close_calls();
unsigned scan_firmware_destroy_calls();
extern const dtv::spi::l3_model_record dtv_l3_model_scanfixture_scan_model_alias;
extern "C" int dtv_public_test_receiver_transition(dtv_enclosure *);
void set_scan_group_case(unsigned);
void set_scan_public_open_case(unsigned);
void set_scan_card_reply_mode(unsigned);
void set_scan_card_transport_drop_reply(bool);
void set_scan_card_t1_exchange_mode(unsigned);
void set_scan_card_t1_bwt_mode(unsigned);
void set_scan_card_t1_bgt(unsigned, std::uint64_t, std::uint64_t);
unsigned scan_card_t1_bgt_refresh_count_value();
void set_scan_card_t1_bgt_send_plan_override(std::uint64_t, std::uint64_t);
unsigned scan_card_t1_apdu_ack_count_value();
unsigned scan_card_t1_apdu_ack_trace_value();
void reset_scan_card_t1_apdu_sequence();
void set_scan_card_t1_fault(unsigned);
void set_scan_card_t1_crc(bool);
void set_scan_card_t1_stale_reply(bool);
void set_scan_card_t1_apdu_mode(bool, unsigned);
void set_scan_card_t1_cwt_fault(unsigned);
bool scan_card_t1_cwt_silence_delivered_value();
void set_scan_card_t1_receive_capacity(bool, std::size_t, std::uint64_t, std::uint64_t);
void set_scan_card_interleave_phase(unsigned);
unsigned scan_card_interleave_phase_value();
void set_scan_stream_192_source();
unsigned scan_stream_transform_call_count();
void set_scan_stream_transform_mode(unsigned);
void set_scan_stream_transform_test_active(bool);
void set_scan_stream_transform_source_phase(unsigned);
void set_scan_stream_failure_modes(unsigned, unsigned, unsigned, unsigned);
void set_scan_stream_generation_override(std::uint64_t);
std::uint64_t scan_stream_previous_generation();
void set_scan_card_state_notification(std::uint32_t, bool);
void set_scan_receiver_lock_notification(std::uint32_t, std::uint32_t, bool);
void set_scan_receiver_lock_operation_epoch(std::uint64_t);
void set_scan_tune_context(dtv_enclosure *, dtv_receiver *, dtv_receiver *);
unsigned scan_tune_call_count();
unsigned scan_tune_effect_count();
bool scan_tune_peer_was_checked();
bool scan_tune_card_was_checked();
bool scan_tune_record_matches(std::size_t, std::uint32_t, std::uint32_t,
                              std::uint64_t, bool, std::uint32_t, std::uint32_t,
                              std::uint32_t);

extern "C" int dtv_public_test_receiver_transition(dtv_enclosure *root)
{
    const dtv::spi::model_information::device_receiver_information update[]{
        {2, true, nullptr, 0},
    };
    dtv::internal::set_public_discovery_receiver_allocation_hook_for_test(
        [] { throw std::bad_alloc(); });
    const auto failed = dtv::internal::update_public_discovery_receiver_declarations_for_test(
        root, update);
    dtv::internal::set_public_discovery_receiver_allocation_hook_for_test({});
    auto *const *devices = dtv_get_devices(root);
    if (failed.code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED || devices == nullptr ||
        dtv_get_receivers(devices[1]) != nullptr) return 1;
    return dtv::internal::update_public_discovery_receiver_declarations_for_test(
        root, update).code == DTV_INTERNAL_RESULT_OK ? 0 : 1;
}
std::size_t scan_open_identify_size();
bool scan_open_identify_is_blocked();
void release_scan_open_identify();
dtv::spi::PublicOpenSession scan_shared_open_session();
std::uint64_t scan_shared_token(std::size_t);
std::uint64_t scan_shared_generation(std::size_t);
unsigned scan_shared_packet_count();
std::uint64_t scan_shared_sequence(std::size_t);
std::size_t scan_shared_length(std::size_t);
void set_scan_shared_reply_enabled(bool);
void set_scan_shared_failure_mode(unsigned);
bool scan_shared_mode_enabled();
bool scan_operation_mode_enabled();
void set_scan_shared_stale_generation(bool);
void set_scan_shared_transaction_override(std::uint64_t);
std::size_t scan_stream_queue_capacity_value();
unsigned scan_pid_processed();
unsigned scan_pid_receiver_one_processed();
bool wait_for_scan_pid_processed(unsigned);
bool wait_for_scan_pid_receiver_one_processed(unsigned);
void set_scan_pid_fail_next(bool);
void set_scan_queue_notification_enabled(bool);
void set_scan_card_state_notification(std::uint32_t, bool);
std::uint64_t scan_stream_previous_generation();
void set_scan_stream_generation_override(std::uint64_t);
extern "C" int card_events_public_consumer(int (*)(void *, std::uint32_t), void *);
std::uint64_t scan_shared_order_at(std::size_t);
std::uint8_t scan_shared_order_endpoint(std::size_t);

namespace {

struct Operation {
    char kind;
    unsigned device;
    int interface_number;
    bool operator==(const Operation &) const = default;
};

struct ConfigAllocation {
    libusb_config_descriptor value{};
    libusb_interface interfaces[4]{};
    libusb_interface_descriptor alternates[5]{};
    libusb_endpoint_descriptor endpoint{};

    ConfigAllocation()
    {
        value.bConfigurationValue = 1;
        value.bNumInterfaces = 4;
        value.interface = interfaces;
        endpoint.bEndpointAddress = 0x81;
        endpoint.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
        endpoint.wMaxPacketSize = 512;
        for (auto &alternate : alternates) {
            alternate.bNumEndpoints = 1;
            alternate.endpoint = &endpoint;
        }
        alternates[0].bInterfaceNumber = 0;
        alternates[1].bInterfaceNumber = 1;
        alternates[2].bInterfaceNumber = 2;
        alternates[3].bInterfaceNumber = 3;
        alternates[4].bInterfaceNumber = 3;
        alternates[4].bAlternateSetting = 1;
        interfaces[0] = {&alternates[0], 1};
        interfaces[1] = {&alternates[1], 1};
        interfaces[2] = {&alternates[2], 1};
        interfaces[3] = {&alternates[3], 2};
    }
};

class FakeBackend final : public dtv::internal::UsbTransferBackend {
public:
    int init_context(libusb_context **context) override
    {
        *context = reinterpret_cast<libusb_context *>(1);
        return LIBUSB_SUCCESS;
    }
    libusb_transfer *allocate_transfer() override
    {
        return async_enabled ? static_cast<libusb_transfer *>(std::calloc(1, sizeof(libusb_transfer)))
                             : nullptr;
    }
    void free_transfer(libusb_transfer *transfer) override
    {
        std::free(transfer);
        ++free_count;
    }
    int submit_transfer(libusb_transfer *transfer) override
    {
        std::lock_guard lock(async_mutex);
        ++submit_calls;
        if (fail_submit || (fail_submit_call != 0 && submit_calls == fail_submit_call))
            return LIBUSB_ERROR_IO;
        if ((transfer->endpoint & LIBUSB_ENDPOINT_IN) == 0 && transfer->length > 0) {
            outputs.emplace_back(transfer->buffer, transfer->buffer + transfer->length);
        }
        ++endpoint_submit_counts[transfer->endpoint];
        submitted.push_back(transfer);
        async_condition.notify_all();
        return LIBUSB_SUCCESS;
    }
    int cancel_transfer(libusb_transfer *transfer) override
    {
        std::lock_guard lock(async_mutex);
        for (auto *candidate : submitted) {
            if (candidate == transfer || candidate->endpoint == transfer->endpoint) {
                cancelled.insert(candidate);
            }
        }
        ++cancel_count;
        async_condition.notify_all();
        return LIBUSB_SUCCESS;
    }
    int handle_events(libusb_context *, timeval *timeout) override
    {
        std::unique_lock lock(async_mutex);
        worker_ids.insert(std::this_thread::get_id());
        const auto duration = std::chrono::microseconds(timeout->tv_sec * 1000000 + timeout->tv_usec);
        async_condition.wait_for(lock, duration, [&] {
            for (auto *transfer : submitted) {
                if ((cancelled.contains(transfer) && !defer_cancel_completion) || complete_all ||
                    ready_counts[transfer->endpoint] != 0) return true;
            }
            return false;
        });
        auto found = submitted.end();
        for (auto &[endpoint, count] : reverse_counts) {
            if (count == 0) continue;
            auto reverse_found = std::find_if(submitted.rbegin(), submitted.rend(), [&](auto *transfer) {
                return transfer->endpoint == endpoint && ready_counts[endpoint] != 0;
            });
            if (reverse_found != submitted.rend()) {
                found = std::prev(reverse_found.base());
                --count;
                break;
            }
        }
        if (found == submitted.end()) found = std::find_if(submitted.begin(), submitted.end(), [&](auto *transfer) {
            return (cancelled.contains(transfer) && !defer_cancel_completion) || complete_all ||
                   ready_counts[transfer->endpoint] != 0;
        });
        if (found == submitted.end()) return LIBUSB_SUCCESS;
        auto *transfer = *found;
        submitted.erase(found);
        const bool was_cancelled = cancelled.erase(transfer) != 0;
        if (!complete_all && ready_counts[transfer->endpoint] != 0) --ready_counts[transfer->endpoint];
        lock.unlock();
        transfer->status = was_cancelled ? LIBUSB_TRANSFER_CANCELLED : LIBUSB_TRANSFER_COMPLETED;
        transfer->actual_length = was_cancelled ? 0
            : static_cast<int>((std::min)(static_cast<std::size_t>(transfer->length), actual_length_limit));
        if (transfer->endpoint & LIBUSB_ENDPOINT_IN) {
            for (int index = 0; index < transfer->actual_length; ++index) {
                transfer->buffer[index] = static_cast<unsigned char>(transfer->endpoint + index);
            }
        }
        transfer->callback(transfer);
        {
            std::lock_guard callback_lock(async_mutex);
            ++callback_count;
            async_condition.notify_all();
        }
        return LIBUSB_SUCCESS;
    }
    void complete_transfers()
    {
        std::lock_guard lock(async_mutex);
        complete_all = true;
        async_condition.notify_all();
    }
    void complete_endpoint(std::uint8_t endpoint, unsigned count = 1)
    {
        std::lock_guard lock(async_mutex);
        ready_counts[endpoint] += count;
        async_condition.notify_all();
    }
    void set_defer_cancel_completion(bool defer)
    {
        std::lock_guard lock(async_mutex);
        defer_cancel_completion = defer;
        async_condition.notify_all();
    }
    void complete_endpoint_reverse(std::uint8_t endpoint)
    {
        std::lock_guard lock(async_mutex);
        ++ready_counts[endpoint];
        ++reverse_counts[endpoint];
        async_condition.notify_all();
    }
    bool wait_for_submissions(std::size_t count)
    {
        std::unique_lock lock(async_mutex);
        return async_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return submitted.size() >= count;
        });
    }
    bool wait_for_endpoint_submissions(std::uint8_t endpoint, unsigned count)
    {
        std::unique_lock lock(async_mutex);
        return async_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return endpoint_submit_counts[endpoint] >= count;
        });
    }
    bool wait_for_pending_endpoint(std::uint8_t endpoint)
    {
        std::unique_lock lock(async_mutex);
        return async_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return std::any_of(submitted.begin(), submitted.end(), [endpoint](const auto *transfer) {
                return transfer->endpoint == endpoint;
            });
        });
    }
    unsigned endpoint_submission_count(std::uint8_t endpoint)
    {
        std::lock_guard lock(async_mutex);
        return endpoint_submit_counts[endpoint];
    }
    std::vector<std::vector<unsigned char>> output_snapshot()
    {
        std::lock_guard lock(async_mutex);
        return outputs;
    }
    void fail_next_submit()
    {
        std::lock_guard lock(async_mutex);
        fail_submit_call = submit_calls + 1;
    }
    bool wait_for_cancellations(unsigned count)
    {
        std::unique_lock lock(async_mutex);
        return async_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return cancel_count >= count;
        });
    }
    bool wait_for_callbacks(unsigned count)
    {
        std::unique_lock lock(async_mutex);
        return async_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return callback_count >= count;
        });
    }
    unsigned callback_count_snapshot()
    {
        std::lock_guard lock(async_mutex);
        return callback_count;
    }
    std::size_t pending_transfer_count()
    {
        std::lock_guard lock(async_mutex);
        return submitted.size();
    }
    std::size_t cancelled_transfer_count()
    {
        std::lock_guard lock(async_mutex);
        return cancelled.size();
    }
    bool cancel_completion_deferred()
    {
        std::lock_guard lock(async_mutex);
        return defer_cancel_completion;
    }
    int get_device_descriptor(libusb_device *device, libusb_device_descriptor *out) override
    {
        const auto raw = reinterpret_cast<std::uintptr_t>(device);
        *out = {};
        out->idVendor = 0x1234;
        out->idProduct = raw == 4 ? 0x0202 : raw == 5 || raw == 6 ? 0x0101 : 0x0201;
        out->bcdDevice = 1;
        out->bNumConfigurations = 1;
        return LIBUSB_SUCCESS;
    }
    std::uint8_t get_bus_number(libusb_device *) override { return 1; }
    std::uint8_t get_device_address(libusb_device *device) override
    {
        return static_cast<std::uint8_t>(reinterpret_cast<std::uintptr_t>(device));
    }
    int get_port_numbers(libusb_device *device, std::uint8_t *ports, int) override
    {
        ports[0] = get_device_address(device);
        return 1;
    }
    int get_active_config_descriptor(libusb_device *, libusb_config_descriptor **out) override
    {
        return get_config_descriptor(nullptr, 0, out);
    }
    int get_config_descriptor(libusb_device *, std::uint8_t,
                              libusb_config_descriptor **out) override
    {
        auto *config = new ConfigAllocation;
        *out = &config->value;
        return LIBUSB_SUCCESS;
    }
    void free_config_descriptor(libusb_config_descriptor *config) override
    {
        delete reinterpret_cast<ConfigAllocation *>(config);
    }
    int open_device(libusb_device *device, libusb_device_handle **out) override
    {
        const auto raw = reinterpret_cast<std::uintptr_t>(device);
        operations.push_back({'O', static_cast<unsigned>(raw), 0});
        if (fail_open) return LIBUSB_ERROR_ACCESS;
        const auto handle = next_handle++;
        handle_devices[handle] = static_cast<unsigned>(raw);
        *out = reinterpret_cast<libusb_device_handle *>(handle);
        ++open_count;
        return LIBUSB_SUCCESS;
    }
    int claim_interface(libusb_device_handle *handle, int interface_number) override
    {
        const auto id = reinterpret_cast<std::uintptr_t>(handle);
        operations.push_back({'C', handle_devices[id], interface_number});
        ++claim_count;
        if (claim_count == fail_claim_call) return fail_claim_result;
        return LIBUSB_SUCCESS;
    }
    int set_interface_alt_setting(libusb_device_handle *handle, int interface_number,
                                  int alternate_setting) override
    {
        const auto id = reinterpret_cast<std::uintptr_t>(handle);
        operations.push_back({'A', handle_devices[id], interface_number * 10 + alternate_setting});
        if (fail_alt && fail_alt_interface == interface_number) return LIBUSB_ERROR_IO;
        return LIBUSB_SUCCESS;
    }
    int wrap_sys_device(libusb_context *, std::intptr_t,
                        libusb_device_handle **out) override
    {
        const auto handle = next_handle++;
        handle_devices[handle] = wrapped_device;
        *out = reinterpret_cast<libusb_device_handle *>(handle);
        return LIBUSB_SUCCESS;
    }
    libusb_device *get_device(libusb_device_handle *handle) override
    {
        return reinterpret_cast<libusb_device *>(handle_devices[reinterpret_cast<std::uintptr_t>(handle)]);
    }
    bool get_connection_identity(libusb_device *device, int descriptor,
                                 dtv::internal::UsbConnectionIdentity *identity) override
    {
        const auto raw = descriptor >= 0 ? 3u :
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(device));
        identity->filesystem = 1;
        identity->inode = raw + identity_generation;
        identity->device = 2;
        identity->valid = true;
        return true;
    }
    bool supports_connection_identity() const override { return true; }
    libusb_device *ref_device(libusb_device *device) override { return device; }
    void unref_device(libusb_device *) override {}
    int release_interface(libusb_device_handle *handle, int interface_number) override
    {
        const auto id = reinterpret_cast<std::uintptr_t>(handle);
        operations.push_back({'R', handle_devices[id], interface_number});
        return LIBUSB_SUCCESS;
    }
    void close_handle(libusb_device_handle *handle) override
    {
        const auto id = reinterpret_cast<std::uintptr_t>(handle);
        operations.push_back({'X', handle_devices[id], 0});
        ++close_count;
    }
    void exit_context(libusb_context *) override {}

    std::vector<Operation> operations;
    std::vector<std::vector<unsigned char>> outputs;
    std::map<std::uintptr_t, unsigned> handle_devices;
    std::uintptr_t next_handle = 100;
    unsigned wrapped_device = 3;
    unsigned open_count = 0;
    unsigned close_count = 0;
    unsigned claim_count = 0;
    unsigned fail_claim_call = 0;
    int fail_claim_result = LIBUSB_ERROR_IO;
    int fail_alt_interface = 0;
    unsigned identity_generation = 0;
    bool fail_alt = false;
    bool fail_open = false;
    bool async_enabled = false;
    bool complete_all = false;
    bool defer_cancel_completion = false;
    bool fail_submit = false;
    unsigned submit_calls = 0;
    unsigned fail_submit_call = 0;
    std::size_t actual_length_limit = (std::numeric_limits<std::size_t>::max)();
    unsigned free_count = 0;
    unsigned cancel_count = 0;
    unsigned callback_count = 0;
    std::mutex async_mutex;
    std::condition_variable async_condition;
    std::vector<libusb_transfer *> submitted;
    std::set<libusb_transfer *> cancelled;
    std::map<std::uint8_t, unsigned> ready_counts;
    std::map<std::uint8_t, unsigned> reverse_counts;
    std::map<std::uint8_t, unsigned> endpoint_submit_counts;
    std::set<std::thread::id> worker_ids;
};

dtv::internal::UsbInterfaceSnapshot interface(std::uint8_t number, std::uint8_t alternate)
{
    dtv::internal::UsbInterfaceSnapshot value{};
    value.info.interface_number = number;
    value.info.alternate_setting = alternate;
    value.info.configuration = 1;
    value.endpoints.push_back({0x81, 0x02, 512, 0});
    return value;
}

std::uint64_t monotonic_now(void *user)
{
    return *static_cast<std::uint64_t *>(user);
}

dtv::internal::UsbDeviceSnapshot snapshot(const dtv::internal::UsbContext &context,
                                           std::uintptr_t device, std::uint64_t token)
{
    auto value = dtv::internal::UsbTestDeviceFactory::snapshot(context, device);
    value.candidate_id = device;
    value.connection_token = token;
    value.interfaces = {interface(0, 0), interface(1, 0), interface(2, 0),
                        interface(3, 0), interface(3, 1)};
    if (scan_shared_mode_enabled()) {
        for (auto &item : value.interfaces) item.endpoints.push_back({0x82, 0x02, 512, 0});
    }
    if (scan_operation_mode_enabled()) {
        for (auto &item : value.interfaces) item.endpoints.push_back({0x02, 0x02, 512, 0});
    }
    return value;
}

dtv::internal::CompleteGroupBatch complete_group(
    dtv::internal::UsbDeviceSnapshot first, dtv::internal::UsbDeviceSnapshot second)
{
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    const auto result = dtv::internal::observe_loaded_product_groups(
        {std::move(first), std::move(second)}, &batch);
    if (result.code != DTV_INTERNAL_RESULT_OK) return {};
    if (batch.groups.size() != 1 || batch.groups[0].functions.size() != 2) return {};
    batch.groups[0].functions[1].candidate_index = batch.groups[0].functions[0].candidate_index;
    batch.groups[0].functions[1].interface_number = 3;
    batch.groups[0].functions[1].alternate_setting = 1;
    return batch;
}

int registry_plan_and_success()
{
    auto backend = std::make_shared<FakeBackend>();
    auto context = dtv::internal::UsbContext::scan_only(backend);
    if (context.result().code != DTV_INTERNAL_RESULT_OK ||
        dtv::internal::UsbTestDeviceFactory::worker_count(context) != 0) return 1;
    dtv::internal::EnclosureRegistry registry;
    auto batch = complete_group(snapshot(context, 3, 3), snapshot(context, 4, 4));
    if (batch.groups.empty() || batch.groups[0].functions.size() != 2) return 29;
    if (registry.refresh(std::move(batch)).code != DTV_INTERNAL_RESULT_OK) return 2;
    auto *root = registry.live_slots()[0];
    if (root == nullptr) return 30;
    if (!registry.hold(root)) return 31;
    std::vector<dtv::internal::UsbClaimDeviceInput> inputs;
    if (registry.claim_inputs(root, &inputs).code != DTV_INTERNAL_RESULT_OK ||
        inputs.size() != 1 || inputs[0].connection_epoch != 3 ||
        inputs[0].interfaces.size() != 2 || registry.enqueue_event(
            DTV_EVENT_CARD_INSERTED, root, nullptr).code != DTV_INTERNAL_RESULT_OK) return 4;
    dtv::internal::UsbClaimLeaseGroup leases;
    constexpr std::uint64_t deadline = UINT64_MAX - 1;
    const auto result = context.acquire_claims(inputs, nullptr, nullptr, deadline, &leases);
    if (result.code != DTV_INTERNAL_RESULT_OK || leases.size() != 1 ||
        leases.deadline_ns() != deadline || backend->open_count != 1 ||
        dtv::internal::UsbTestDeviceFactory::worker_count(context) != 0 ||
        registry.live_slots()[0] != root || registry.status(root) != DTV_ENCLOSURE_READY) return 5;
    if (leases.release().code != DTV_INTERNAL_RESULT_OK || leases.size() != 0 ||
        backend->operations != std::vector<Operation>{{'O', 3, 0}, {'C', 3, 0}, {'C', 3, 3},
            {'A', 3, 31}, {'R', 3, 3}, {'R', 3, 0}, {'X', 3, 0}}) return 6;
    backend->operations.clear();
    backend->claim_count = 0;
    backend->fail_claim_call = 2;
    const auto failed = context.acquire_claims(inputs, nullptr, nullptr, deadline, &leases);
    if (failed.code != DTV_INTERNAL_RESULT_USB_IO ||
        backend->operations != std::vector<Operation>{{'O', 3, 0}, {'C', 3, 0}, {'C', 3, 3},
            {'R', 3, 0}, {'X', 3, 0}} || registry.live_slots()[0] != root ||
        registry.status(root) != DTV_ENCLOSURE_READY) return 8;
    dtv_event event{};
    bool available = false;
    if (registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_OK || !available ||
        event.kind != DTV_EVENT_USB_CONNECTED || event.enclosure != root) return 7;
    return 0;
}

int rollback_and_preflight()
{
    auto backend = std::make_shared<FakeBackend>();
    auto context = dtv::internal::UsbContext::scan_only(backend);
    auto first = snapshot(context, 3, 3);
    auto second = snapshot(context, 4, 4);
    std::vector<dtv::internal::UsbClaimDeviceInput> inputs{
        {3, first, {{1, 0}, {3, 1}}}, {4, second, {{1, 0}, {3, 0}}}};
    dtv::internal::UsbClaimLeaseGroup leases;
    auto success_inputs = std::vector<dtv::internal::UsbClaimDeviceInput>{
        {3, first, {{1, 0}}}, {4, second, {{1, 0}}}};
    auto result = context.acquire_claims(success_inputs, nullptr, nullptr, UINT64_MAX, &leases);
    if (result.code != DTV_INTERNAL_RESULT_OK || leases.size() != 2 || backend->open_count != 2 ||
        leases.release().code != DTV_INTERNAL_RESULT_OK ||
        backend->operations != std::vector<Operation>{{'O', 3, 0}, {'C', 3, 1}, {'O', 4, 0},
            {'C', 4, 1}, {'R', 4, 1}, {'X', 4, 0}, {'R', 3, 1}, {'X', 3, 0}}) return 5;
    backend->operations.clear();
    backend->open_count = backend->close_count = backend->claim_count = 0;
    backend->fail_claim_call = 4;
    result = context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &leases);
    if (result.code != DTV_INTERNAL_RESULT_USB_IO || leases.size() != 0 ||
        backend->operations != std::vector<Operation>{{'O', 3, 0}, {'C', 3, 1}, {'C', 3, 3},
            {'A', 3, 31}, {'O', 4, 0}, {'C', 4, 1}, {'C', 4, 3}, {'R', 4, 1},
            {'X', 4, 0}, {'R', 3, 3}, {'R', 3, 1}, {'X', 3, 0}}) return 1;

    backend->operations.clear();
    backend->open_count = backend->close_count = backend->claim_count = 0;
    backend->fail_claim_call = 0;
    backend->fail_alt = true;
    backend->fail_alt_interface = 3;
    inputs.resize(1);
    result = context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &leases);
    if (result.code != DTV_INTERNAL_RESULT_USB_IO ||
        backend->operations != std::vector<Operation>{{'O', 3, 0}, {'C', 3, 1}, {'C', 3, 3},
            {'A', 3, 31}, {'R', 3, 3}, {'R', 3, 1}, {'X', 3, 0}}) return 2;

    backend->operations.clear();
    backend->open_count = backend->close_count = backend->claim_count = 0;
    backend->fail_alt = false;
    inputs[0].connection_epoch = 99;
    result = context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &leases);
    if (result.code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT || !backend->operations.empty()) return 3;

    inputs[0].connection_epoch = 3;
    inputs[0].interfaces[0].interface_number = 99;
    result = context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &leases);
    if (result.code != DTV_INTERNAL_RESULT_UNSUPPORTED || !backend->operations.empty()) return 4;

    inputs[0] = {3, first, {{1, 0}}};
    std::uint64_t now = 10;
    const dtv_runtime_clock clock{&now, monotonic_now, nullptr, nullptr};
    result = context.acquire_claims(inputs, &clock, nullptr, now, &leases);
    if (result.code != DTV_INTERNAL_RESULT_TIMEOUT || !backend->operations.empty()) return 6;
    backend->identity_generation = 1;
    result = context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &leases);
    return result.code == DTV_INTERNAL_RESULT_USB_IO && backend->operations.empty() ? 0 : 5;
}

int busy_and_fd_owner()
{
    auto backend = std::make_shared<FakeBackend>();
    auto context = dtv::internal::UsbContext::scan_only(backend);
    auto normal = snapshot(context, 3, 3);
    std::vector<dtv::internal::UsbClaimDeviceInput> inputs{{3, normal, {{1, 0}}}};
    dtv::internal::UsbClaimLeaseGroup leases;
    backend->fail_claim_call = 1;
    backend->fail_claim_result = LIBUSB_ERROR_BUSY;
    auto result = context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &leases);
    if (result.code != DTV_INTERNAL_RESULT_BUSY || result.detail != 6 ||
        backend->operations != std::vector<Operation>{{'O', 3, 0}, {'C', 3, 1}, {'X', 3, 0}}) return 1;
#if defined(__linux__)
    int descriptors[2];
    if (pipe(descriptors) != 0) return 2;
    backend->operations.clear();
    backend->fail_claim_call = 0;
    backend->fail_claim_result = LIBUSB_ERROR_IO;
    dtv_internal_result scan_result{};
    auto snapshots = context.enumerate_fds(std::span<const std::int32_t>(
        reinterpret_cast<const std::int32_t *>(&descriptors[0]), 1), &scan_result);
    if (scan_result.code != DTV_INTERNAL_RESULT_OK || snapshots.size() != 1 ||
        fcntl(descriptors[0], F_GETFD) < 0) return 3;
    snapshots[0].interfaces = {interface(1, 0)};
    inputs = {{snapshots[0].connection_token, snapshots[0], {{1, 0}}}};
    const auto closes_before_claim = backend->close_count;
    result = context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &leases);
    if (result.code != DTV_INTERNAL_RESULT_OK || backend->open_count != 1 ||
        backend->close_count != closes_before_claim || fcntl(descriptors[0], F_GETFD) < 0) return 4;
    dtv::internal::UsbClaimLeaseGroup duplicate_lease;
    if (context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &duplicate_lease).code !=
        DTV_INTERNAL_RESULT_BUSY) return 8;
    auto duplicate_snapshots = context.enumerate_fds(std::span<const std::int32_t>(
        reinterpret_cast<const std::int32_t *>(&descriptors[0]), 1), &scan_result);
    if (scan_result.code != DTV_INTERNAL_RESULT_OK || duplicate_snapshots.size() != 1) return 9;
    duplicate_snapshots[0].interfaces = {interface(1, 0)};
    std::vector<dtv::internal::UsbClaimDeviceInput> duplicate_inputs{{
        duplicate_snapshots[0].connection_token, duplicate_snapshots[0], {{1, 0}}}};
    if (context.acquire_claims(duplicate_inputs, nullptr, nullptr, UINT64_MAX,
                               &duplicate_lease).code != DTV_INTERNAL_RESULT_BUSY) return 10;
    duplicate_inputs.clear();
    duplicate_snapshots.clear();
    const auto closes_before_release = backend->close_count;
    if (leases.release().code != DTV_INTERNAL_RESULT_OK ||
        backend->close_count != closes_before_release ||
        backend->operations.back() != Operation{'R', 3, 1}) return 5;
    inputs.clear();
    snapshots.clear();
    if (backend->close_count != closes_before_release + 1 ||
        fcntl(descriptors[0], F_GETFD) < 0) return 6;
    close(descriptors[0]);
    close(descriptors[1]);
#endif
    return dtv::internal::UsbTestDeviceFactory::worker_count(context) == 0 ? 0 : 7;
}

int enclosure_runtime_completions()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    std::vector<dtv::internal::UsbClaimDeviceInput> inputs{
        {3, snapshot(context, 3, 3), {{1, 0}}},
        {4, snapshot(context, 4, 4), {{1, 0}}}};
    dtv::internal::UsbClaimLeaseGroup claims;
    if (context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &claims).code !=
        DTV_INTERNAL_RESULT_OK || claims.size() != 2) return 1;
    dtv_internal_result result{};
    auto runtime = dtv::internal::EnclosureRuntime::start(std::move(claims), &result);
    if (!runtime || result.code != DTV_INTERNAL_RESULT_OK || runtime->worker_count() != 1 ||
        dtv::internal::UsbTestDeviceFactory::worker_count(context) != 0) return 2;
    std::uint64_t input_id = 0;
    std::uint64_t output_id = 0;
    const unsigned char output[] = {7, 8, 9};
    if (runtime->submit_bulk_in(0, 0x81, 8, 11, nullptr, nullptr, UINT64_MAX,
                                &input_id).code != DTV_INTERNAL_RESULT_OK ||
        runtime->submit_bulk_out(1, 0x02, output, 12, nullptr, nullptr, UINT64_MAX,
                                 &output_id).code != DTV_INTERNAL_RESULT_OK) return 3;
    dtv::internal::UsbRuntimeTerminal terminal{};
    dtv_internal_result wait_result{};
    std::thread waiter([&] {
        wait_result = runtime->wait_request(input_id, nullptr, UINT64_MAX, &terminal);
    });
    backend->complete_endpoint(0x02);
    dtv::internal::UsbRuntimeCompletion output_completion{};
    bool available = false;
    for (unsigned attempt = 0; attempt < 2000 && !available; ++attempt) {
        if (runtime->next_completion(&output_completion, &available).code != DTV_INTERNAL_RESULT_OK)
            return 5;
        if (!available) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!available || output_completion.request_id != output_id ||
        output_completion.generation != 12 || output_completion.actual_length != 3) return 5;
    backend->complete_endpoint(0x81);
    waiter.join();
    dtv::internal::UsbRuntimeCompletion input_completion{};
    if (wait_result.code != DTV_INTERNAL_RESULT_OK ||
        terminal != dtv::internal::UsbRuntimeTerminal::completed ||
        runtime->next_completion(&input_completion, &available).code != DTV_INTERNAL_RESULT_OK ||
        !available || output_completion.completion_sequence >= input_completion.completion_sequence ||
        input_completion.request_id != input_id || input_completion.generation != 11 ||
        input_completion.actual_length != 8 || input_completion.bytes.size() != 8 ||
        input_completion.bytes[0] != 0x81) return 5;
    {
        std::lock_guard lock(backend->async_mutex);
        if (backend->worker_ids.size() != 1) return 6;
    }
    const auto stopped = runtime->stop(nullptr, UINT64_MAX);
    if (stopped.pending || stopped.result.code != DTV_INTERNAL_RESULT_OK ||
        runtime->worker_count() != 0) return 7;
    const std::vector<Operation> expected_release{
        {'R', 4, 1}, {'X', 4, 0}, {'R', 3, 1}, {'X', 3, 0}};
    if (backend->operations.size() < expected_release.size() ||
        !std::equal(backend->operations.end() - expected_release.size(),
                    backend->operations.end(), expected_release.begin())) return 8;
    return 0;
}

int enclosure_runtime_pending_stop()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    dtv::internal::EnclosureRegistry registry;
    auto batch = complete_group(snapshot(context, 3, 3), snapshot(context, 4, 4));
    if (registry.refresh(std::move(batch)).code != DTV_INTERNAL_RESULT_OK) return 1;
    auto *root = registry.live_slots()[0];
    if (root == nullptr || !registry.hold(root) || registry.enqueue_event(
            DTV_EVENT_CARD_INSERTED, root, nullptr).code != DTV_INTERNAL_RESULT_OK) return 1;
#if defined(__linux__)
    int descriptors[2];
    if (pipe(descriptors) != 0) return 1;
    dtv_internal_result scan_result{};
    auto snapshots = context.enumerate_fds(std::span<const std::int32_t>(
        reinterpret_cast<const std::int32_t *>(&descriptors[0]), 1), &scan_result);
    if (scan_result.code != DTV_INTERNAL_RESULT_OK || snapshots.size() != 1) return 1;
    snapshots[0].interfaces = {interface(1, 0)};
    std::vector<dtv::internal::UsbClaimDeviceInput> inputs{
        {snapshots[0].connection_token, snapshots[0], {{1, 0}}}};
#else
    std::vector<dtv::internal::UsbClaimDeviceInput> inputs{{3, snapshot(context, 3, 3), {{1, 0}}}};
#endif
    dtv::internal::UsbClaimLeaseGroup claims;
    if (context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &claims).code !=
        DTV_INTERNAL_RESULT_OK) return 1;
#if defined(__linux__)
    inputs.clear();
    snapshots.clear();
    if (fcntl(descriptors[0], F_GETFD) < 0) return 1;
#endif
    dtv_internal_result result{};
    auto runtime = dtv::internal::EnclosureRuntime::start(std::move(claims), &result);
    if (!runtime || result.code != DTV_INTERNAL_RESULT_OK) return 2;
    std::uint64_t request_id = 0;
    if (runtime->submit_bulk_in(0, 0x81, 16, 1, nullptr, nullptr, UINT64_MAX,
                                &request_id).code != DTV_INTERNAL_RESULT_OK ||
        !backend->wait_for_submissions(1) ||
        runtime->cancel(request_id).code != DTV_INTERNAL_RESULT_OK) return 3;
    dtv::internal::UsbRuntimeTerminal terminal{};
    if (runtime->wait_request(request_id, nullptr, UINT64_MAX, &terminal).code !=
        DTV_INTERNAL_RESULT_TIMEOUT || terminal != dtv::internal::UsbRuntimeTerminal::cancelled) return 4;
    std::uint64_t now = 10;
    const dtv_runtime_clock clock{&now, monotonic_now, nullptr, nullptr};
    const auto pending = runtime->stop(&clock, now);
    if (!pending.pending || pending.result.code != DTV_INTERNAL_RESULT_TIMEOUT ||
        backend->free_count != 0 || backend->close_count != 0 ||
        registry.live_slots()[0] != root || registry.status(root) != DTV_ENCLOSURE_READY) return 5;
    std::uint64_t rejected_id = 0;
    if (runtime->submit_bulk_in(0, 0x81, 4, 2, nullptr, nullptr, UINT64_MAX,
                                &rejected_id).code != DTV_INTERNAL_RESULT_NOT_READY) return 5;
#if defined(__linux__)
    if (fcntl(descriptors[0], F_GETFD) < 0) return 5;
#endif
    backend->complete_transfers();
    const auto stopped = runtime->stop(nullptr, UINT64_MAX);
    if (stopped.pending || stopped.result.code != DTV_INTERNAL_RESULT_OK ||
        backend->free_count != 1 || backend->close_count != 1 ||
        registry.live_slots()[0] != root || registry.status(root) != DTV_ENCLOSURE_READY) return 6;
#if defined(__linux__)
    if (fcntl(descriptors[0], F_GETFD) < 0) return 6;
    close(descriptors[0]);
    close(descriptors[1]);
#endif
    dtv::internal::UsbRuntimeCompletion completion{};
    bool available = false;
    if (runtime->next_completion(&completion, &available).code != DTV_INTERNAL_RESULT_OK ||
        !available || completion.request_id != request_id ||
        completion.terminal != dtv::internal::UsbRuntimeTerminal::cancelled ||
        runtime->next_completion(&completion, &available).code != DTV_INTERNAL_RESULT_OK || available) return 7;
    dtv_event event{};
    if (registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_OK || !available ||
        event.kind != DTV_EVENT_USB_CONNECTED || event.enclosure != root ||
        registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_OK || !available ||
        event.kind != DTV_EVENT_CARD_INSERTED || event.enclosure != root) return 8;
    return 0;
}

int enclosure_runtime_endpoint_cancel()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    std::vector<dtv::internal::UsbClaimDeviceInput> inputs{{3, snapshot(context, 3, 3), {{1, 0}}}};
    dtv::internal::UsbClaimLeaseGroup claims;
    if (context.acquire_claims(inputs, nullptr, nullptr, UINT64_MAX, &claims).code !=
        DTV_INTERNAL_RESULT_OK) return 1;
    dtv_internal_result result{};
    auto runtime = dtv::internal::EnclosureRuntime::start(std::move(claims), &result);
    if (!runtime || result.code != DTV_INTERNAL_RESULT_OK) return 2;
    std::uint64_t cancelled_id = 0;
    std::uint64_t peer_id = 0;
    if (runtime->submit_bulk_in(0, 0x81, 8, 20, nullptr, nullptr, UINT64_MAX,
                                &cancelled_id).code != DTV_INTERNAL_RESULT_OK ||
        runtime->submit_bulk_in(0, 0x81, 8, 21, nullptr, nullptr, UINT64_MAX,
                                &peer_id).code != DTV_INTERNAL_RESULT_OK ||
        !backend->wait_for_submissions(2) ||
        runtime->cancel(cancelled_id).code != DTV_INTERNAL_RESULT_OK) return 3;
    dtv::internal::UsbRuntimeTerminal first_terminal{};
    dtv::internal::UsbRuntimeTerminal peer_terminal{};
    const auto first_result = runtime->wait_request(cancelled_id, nullptr, UINT64_MAX, &first_terminal);
    if (!backend->wait_for_cancellations(1)) return 4;
    backend->complete_transfers();
    const auto peer_result = runtime->wait_request(peer_id, nullptr, UINT64_MAX, &peer_terminal);
    if (first_result.code != DTV_INTERNAL_RESULT_TIMEOUT ||
        first_terminal != dtv::internal::UsbRuntimeTerminal::cancelled ||
        peer_result.code != DTV_INTERNAL_RESULT_USB_IO ||
        peer_terminal != dtv::internal::UsbRuntimeTerminal::endpoint_cancelled) return 4;
    dtv::internal::UsbRuntimeCompletion completion{};
    bool available = false;
    if (runtime->next_completion(&completion, &available).code != DTV_INTERNAL_RESULT_OK || !available ||
        runtime->next_completion(&completion, &available).code != DTV_INTERNAL_RESULT_OK || !available ||
        runtime->next_completion(&completion, &available).code != DTV_INTERNAL_RESULT_OK || available) return 5;
    std::uint64_t now = 10;
    const dtv_runtime_clock clock{&now, monotonic_now, nullptr, nullptr};
    std::uint64_t deadline_id = 0;
    if (runtime->submit_bulk_in(0, 0x81, 8, 22, &clock, nullptr, now,
                                &deadline_id).code != DTV_INTERNAL_RESULT_OK) return 6;
    dtv::internal::UsbRuntimeTerminal deadline_terminal{};
    if (runtime->wait_request(deadline_id, nullptr, UINT64_MAX, &deadline_terminal).code !=
        DTV_INTERNAL_RESULT_TIMEOUT || deadline_terminal != dtv::internal::UsbRuntimeTerminal::deadline)
        return 7;
    for (unsigned attempt = 0; attempt < 2000 && !available; ++attempt) {
        if (runtime->next_completion(&completion, &available).code != DTV_INTERNAL_RESULT_OK) return 8;
        if (!available) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!available || completion.request_id != deadline_id ||
        completion.terminal != dtv::internal::UsbRuntimeTerminal::deadline) return 9;
    const auto stopped = runtime->stop(nullptr, UINT64_MAX);
    return !stopped.pending && stopped.result.code == DTV_INTERNAL_RESULT_OK ? 0 : 6;
}

int public_open_lifecycle()
{
    set_scan_public_open_case(13);
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->complete_all = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 77), snapshot(context, 4, 78)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1 ||
        batch.groups[0].functions.size() != 2) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr || error.code != DTV_ERROR_OK) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    auto *const *devices = dtv_get_devices(root);
    auto *const *receivers = devices == nullptr ? nullptr : dtv_get_receivers(devices[0]);
    dtv_stream *stream = nullptr;
    if (receivers == nullptr || receivers[0] == nullptr ||
        dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_NOT_READY ||
        stream != nullptr) return 8;
    if (!backend->worker_ids.empty()) return 7;
    char message = 'x';
    error.cause_domain = 91;
    error.cause_code = 73;
    error.message = &message;
    error.message_capacity = 1;
    const auto opened = dtv_open_enclosure(root, nullptr, &error);
    if (opened != DTV_ERROR_OK || error.code != DTV_ERROR_OK || backend->open_count != 2 ||
        backend->claim_count != 2 || backend->worker_ids.size() != 1 ||
        scan_open_identify_size() != 64 || error.cause_domain != DTV_ERROR_DOMAIN_NONE ||
        error.cause_code != 0 || message != 'x') {
        std::fprintf(stderr, "public open values: result=%d error=%d opens=%u claims=%u workers=%zu\n",
                     opened, error.code, backend->open_count, backend->claim_count,
                     backend->worker_ids.size());
        return 3;
    }
    if (dtv_open_enclosure(root, nullptr, &error) != DTV_ERROR_BUSY) return 4;
    if (receivers == nullptr || receivers[0] == nullptr ||
        dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr ||
        dtv_get_dropped_bytes(stream) != 0 || dtv_get_desyncs(stream) != 0) return 41;
    std::array<std::uint8_t, 376> packets{};
    std::uint64_t output_size = 0;
    dtv_stream *next_stream = nullptr;
    if (dtv_read_stream(stream, packets.data(), 187, nullptr, &output_size, &error) !=
            DTV_ERROR_BUFFER_TOO_SMALL || output_size != 0) return 42;
    if (dtv_read_stream(stream, packets.data(), packets.size(), nullptr, &output_size, &error) !=
            DTV_ERROR_OK || output_size != packets.size() || packets[0] != 0x47 ||
        packets[188] != 0x47 || scan_stream_transform_call_count() == 0) return 44;
    if (dtv_read_stream(stream, packets.data(), packets.size(), nullptr, &output_size, &error) !=
            DTV_ERROR_OK || output_size != 0) return 45;
    if (dtv_open_stream(receivers[0], nullptr, &next_stream, &error) != DTV_ERROR_BUSY ||
        next_stream != nullptr) return 46;
    if (dtv_cancel_stream(stream, &error) != DTV_ERROR_OK ||
        dtv_read_stream(stream, packets.data(), packets.size(), nullptr, &output_size, &error) !=
            DTV_ERROR_CANCELLED || output_size != 0) return 47;
    if (dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED ||
        dtv_get_stream_close_state(stream, &error) != DTV_CLOSE_CLOSED) return 48;
    dtv_release_stream(stream);
    stream = nullptr;
    error.cause_domain = 91;
    error.cause_code = 73;
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED ||
        dtv_get_enclosure_close_state(root, &error) != DTV_CLOSE_CLOSED ||
        error.cause_domain != DTV_ERROR_DOMAIN_NONE || error.cause_code != 0 ||
        backend->close_count != 2 || std::count_if(backend->operations.begin(),
            backend->operations.end(), [](const auto &operation) { return operation.kind == 'R'; }) != 2)
        return 5;
    dtv_release_enclosure(root);
    if (backend->close_count != 2) return 6;
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

int public_open_192_stream()
{
    set_scan_public_open_case(14);
    set_scan_stream_192_source();
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->actual_length_limit = 7;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 179), snapshot(context, 4, 180)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(2)) {
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 3;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result != DTV_ERROR_OK || !backend->wait_for_submissions(2)) return 4;
    auto *const *devices = dtv_get_devices(root);
    auto *const *receivers = devices == nullptr ? nullptr : dtv_get_receivers(devices[0]);
    if (receivers == nullptr || receivers[1] == nullptr) return 5;
    dtv_stream *stream = nullptr;
    if (dtv_open_stream(receivers[1], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr)
        return 6;

    constexpr std::size_t source_size = 3 + 5 * 192;
    constexpr std::size_t first_source_size = 3 + 2 * 192;
    constexpr std::size_t chunk_size = 17;
    constexpr std::size_t first_completion_count =
        (first_source_size + chunk_size - 1) / chunk_size;
    constexpr std::size_t remaining_size = source_size - first_completion_count * chunk_size;
    constexpr std::size_t remaining_completion_count =
        (remaining_size + chunk_size - 1) / chunk_size + 1;
    auto callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81, first_completion_count);
    if (!backend->wait_for_callbacks(callbacks + first_completion_count)) {
        dtv_cancel_stream(stream, &error);
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 7;
    }

    std::array<unsigned char, 2 * 188> output{};
    std::array<unsigned char, 2 * 188> expected{};
    std::size_t expected_offset = 0;
    for (std::size_t unit : {0u, 1u}) {
        expected[expected_offset] = 0x47;
        std::fill(expected.begin() + expected_offset + 1,
                  expected.begin() + expected_offset + 188,
                  static_cast<unsigned char>(0x20 + unit));
        expected_offset += 188;
    }
    std::uint64_t output_size = 0;
    const dtv_call_options read_wait{sizeof(dtv_call_options), 2000};
    if (dtv_read_stream(stream, output.data(), output.size(), &read_wait, &output_size, &error) !=
            DTV_ERROR_OK || output_size != output.size() || output != expected ||
        dtv_get_dropped_bytes(stream) != 3 || dtv_get_desyncs(stream) != 0) return 8;

    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81, remaining_completion_count);
    if (!backend->wait_for_callbacks(callbacks + remaining_completion_count)) return 9;
    expected_offset = 0;
    for (std::size_t unit : {3u, 4u}) {
        expected[expected_offset] = 0x47;
        std::fill(expected.begin() + expected_offset + 1,
                  expected.begin() + expected_offset + 188,
                  static_cast<unsigned char>(0x20 + unit));
        expected_offset += 188;
    }
    if (dtv_read_stream(stream, output.data(), output.size(), &read_wait, &output_size, &error) !=
            DTV_ERROR_OK || output_size != output.size() || output != expected ||
        dtv_get_dropped_bytes(stream) != 195 || dtv_get_desyncs(stream) != 1) return 10;
    for (std::size_t offset = 0; offset < output_size; offset += 188) {
        if (output[offset] != 0x47) return 11;
    }
    if (dtv_read_stream(stream, output.data(), output.size(), &read_wait, &output_size, &error) !=
            DTV_ERROR_OK || output_size != 0) return 12;
    if (dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 13;
    dtv_release_stream(stream);

    set_scan_stream_transform_test_active(true);
    set_scan_stream_transform_mode(0);
    set_scan_stream_transform_source_phase(0);
    auto complete_source = [&](unsigned phase) {
        set_scan_stream_transform_source_phase(phase);
        const auto before = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        return backend->wait_for_callbacks(before + 1);
    };
    auto read_markers = [&](dtv_stream *target,
                            std::initializer_list<unsigned char> markers) {
        std::array<unsigned char, 3 * 188> actual{};
        const auto expected_size = markers.size() * 188;
        std::uint64_t actual_size = 0;
        if (dtv_read_stream(target, actual.data(), expected_size, &read_wait,
                &actual_size, &error) != DTV_ERROR_OK || actual_size != expected_size)
            return false;
        std::size_t unit = 0;
        for (const auto marker : markers) {
            if (actual[unit * 188] != 0x47 || actual[unit * 188 + 1] != marker) return false;
            ++unit;
        }
        return true;
    };
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK ||
        stream == nullptr || !backend->wait_for_submissions(2) || !complete_source(0) ||
        !read_markers(stream, {0x11, 0x12})) return 15;
    const auto transform_calls = scan_stream_transform_call_count();
    if (!complete_source(1) || !read_markers(stream, {0x21, 0xfa, 0xfa}) ||
        scan_stream_transform_call_count() != transform_calls + 1 ||
        dtv_get_dropped_bytes(stream) != 190 || dtv_get_desyncs(stream) != 1) return 16;
    const auto phase2_completed = complete_source(2);
    const auto phase2_read = phase2_completed && read_markers(stream, {0x31});
    const auto phase2_dropped = dtv_get_dropped_bytes(stream);
    const auto phase2_desyncs = dtv_get_desyncs(stream);
    if (!phase2_completed || !phase2_read || phase2_dropped != 192 || phase2_desyncs != 2) {
        std::fprintf(stderr, "runtime transform deferred skip: completed=%d read=%d dropped=%llu desyncs=%llu\n",
            phase2_completed, phase2_read, static_cast<unsigned long long>(phase2_dropped),
            static_cast<unsigned long long>(phase2_desyncs));
        return 17;
    }
    if (!complete_source(3) || !read_markers(stream, {0xfa, 0xfa}) ||
        dtv_get_dropped_bytes(stream) != 380 || dtv_get_desyncs(stream) != 2) return 18;
    if (!complete_source(4) || !read_markers(stream, {0x51}) ||
        dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 19;
    dtv_release_stream(stream);
    set_scan_stream_transform_source_phase(5);
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK ||
        stream == nullptr || !backend->wait_for_submissions(2) || !complete_source(5) ||
        !read_markers(stream, {0x61, 0x62}) ||
        dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 20;
    dtv_release_stream(stream);

    set_scan_stream_transform_mode(1);
    set_scan_stream_transform_source_phase(0);
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK ||
        stream == nullptr || !backend->wait_for_submissions(2) || !complete_source(0)) return 21;
    std::array<unsigned char, 188> shortened{};
    if (dtv_read_stream(stream, shortened.data(), shortened.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != shortened.size() ||
        shortened[0] != 0x47 || shortened[1] != 0x11) return 22;
    const dtv_call_options short_wait{sizeof(dtv_call_options), 10};
    if (dtv_read_stream(stream, shortened.data(), shortened.size(), &short_wait,
            &output_size, &error) != DTV_ERROR_TIMEOUT || output_size != 0 ||
        dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 23;
    dtv_release_stream(stream);

    set_scan_stream_transform_mode(2);
    set_scan_stream_transform_source_phase(0);
    const auto calls_before_zero = scan_stream_transform_call_count();
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK ||
        stream == nullptr || !backend->wait_for_submissions(2) || !complete_source(0)) return 24;
    const auto zero_read = dtv_read_stream(stream, shortened.data(), shortened.size(), &short_wait,
        &output_size, &error);
    const auto calls_after_zero = scan_stream_transform_call_count();
    const auto zero_close = dtv_close_stream(stream, nullptr, &error);
    if (zero_read != DTV_ERROR_TIMEOUT || output_size != 0 ||
        calls_after_zero != calls_before_zero + 1 || zero_close != DTV_CLOSE_CLOSED) {
        std::fprintf(stderr, "runtime transform zero output: read=%d size=%llu calls=%u/%u close=%d\n",
            zero_read, static_cast<unsigned long long>(output_size), calls_before_zero,
            calls_after_zero, zero_close);
        return 25;
    }
    dtv_release_stream(stream);
    set_scan_stream_transform_test_active(false);
    set_scan_stream_transform_mode(0);

    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED) return 14;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

int public_stream_queue_overflow()
{
    set_scan_public_open_case(15);
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->actual_length_limit = 7;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 181), snapshot(context, 4, 182)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(2)) {
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 3;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result != DTV_ERROR_OK || !backend->wait_for_submissions(2)) return 4;
    auto *const *devices = dtv_get_devices(root);
    auto *const *receivers = devices == nullptr ? nullptr : dtv_get_receivers(devices[0]);
    if (receivers == nullptr || receivers[0] == nullptr || receivers[1] == nullptr) return 5;

    dtv_stream *stream = nullptr;
    dtv_stream *other_stream = nullptr;
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr ||
        dtv_open_stream(receivers[1], nullptr, &other_stream, &error) != DTV_ERROR_OK ||
        other_stream == nullptr) return 6;
    const auto queue_capacity = scan_stream_queue_capacity_value();
    const auto saved_size = queue_capacity - queue_capacity % 188;
    if (queue_capacity < 188 * 2 || saved_size == 0) return 7;
    std::array<unsigned char, 188> first_packet{};
    std::uint64_t output_size = 0;
    const dtv_call_options short_wait{sizeof(dtv_call_options), 10};
    if (dtv_read_stream(stream, first_packet.data(), first_packet.size(), &short_wait,
            &output_size, &error) != DTV_ERROR_TIMEOUT || output_size != 0) return 8;

    const auto complete_in = [&](std::uint8_t endpoint, unsigned count) {
        while (count != 0) {
            const auto batch_count = (std::min)(count, 128u);
            const auto callbacks = backend->callback_count_snapshot();
            backend->complete_endpoint(endpoint, batch_count);
            if (!backend->wait_for_callbacks(callbacks + batch_count)) return false;
            count -= batch_count;
        }
        return true;
    };
    const auto source_count = static_cast<unsigned>(queue_capacity / 188 + 16);
    if (!complete_in(0x81, source_count)) return 9;

    const dtv_call_options read_wait{sizeof(dtv_call_options), 1000};
    if (dtv_read_stream(stream, first_packet.data(), first_packet.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != first_packet.size() ||
        first_packet[0] != 0x47 || first_packet[1] != 0 || first_packet[2] != 0) return 10;
    for (std::size_t index = 5; index < first_packet.size(); ++index) {
        if (first_packet[index] != static_cast<unsigned char>(index * 37)) return 11;
    }

    const auto run_card_operation = [&] {
        const std::array<unsigned char, 5> arguments{1, 42, 0, 91, 'Q'};
        const dtv::spi::PublicOperationRequest request{1,
            dtv::spi::PublicOperationTargetKind::card, 1, arguments};
        std::array<unsigned char, 32> reply{};
        std::size_t reply_size = 0;
        dtv_internal_result result{};
        std::thread operation([&] {
            result = dtv::internal::run_public_operation_for_test(root, request,
                dtv_timeout{1000, 0}, reply, &reply_size);
        });
        const auto out_count = backend->endpoint_submission_count(0x02) + 1;
        set_scan_shared_reply_enabled(true);
        set_scan_shared_transaction_override(91);
        if (!backend->wait_for_endpoint_submissions(0x02, out_count)) {
            operation.join();
            return false;
        }
        auto callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(callbacks + 1)) {
            operation.join();
            return false;
        }
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        const bool callback_done = backend->wait_for_callbacks(callbacks + 1);
        operation.join();
        set_scan_shared_reply_enabled(false);
        return callback_done && result.code == DTV_INTERNAL_RESULT_OK && reply_size == 7;
    };
    if (!run_card_operation()) return 12;

    set_scan_card_state_notification(DTV_CARD_PRESENT, false);
    set_scan_queue_notification_enabled(true);
    backend->complete_endpoint(0x81);
    const auto notification_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(read_wait.timeout_ms);
    auto notification_state = dtv_get_card_state(root);
    while (notification_state != DTV_CARD_PRESENT &&
           std::chrono::steady_clock::now() < notification_deadline) {
        std::this_thread::yield();
        notification_state = dtv_get_card_state(root);
    }
    set_scan_queue_notification_enabled(false);
    if (notification_state != DTV_CARD_PRESENT) {
        std::fprintf(stderr, "queue notification: state=%u reader=%p packets=%u\n",
            notification_state,
            static_cast<void *>(dtv_get_card_reader_device(root)), scan_shared_packet_count());
        return 13;
    }

    if (!complete_in(0x81, 3)) return 14;
    std::array<unsigned char, 376> other_packets{};
    if (!complete_in(0x82, 1) ||
        dtv_read_stream(other_stream, other_packets.data(), other_packets.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != other_packets.size() ||
        other_packets[0] != 0x47 || other_packets[1] != 0x80 ||
        other_packets[188] != 0x47 || other_packets[189] != 0x81) return 15;

    std::vector<unsigned char> saved(saved_size - 188);
    if (dtv_read_stream(stream, saved.data(), saved.size(), &read_wait, &output_size, &error) !=
            DTV_ERROR_OK || output_size != saved.size()) return 16;
    for (std::size_t packet = 0; packet < saved.size() / 188; ++packet) {
        const auto offset = packet * 188;
        const auto number = static_cast<std::uint32_t>(packet + 1);
        if (saved[offset] != 0x47 || saved[offset + 1] != static_cast<unsigned char>(number) ||
            saved[offset + 2] != static_cast<unsigned char>(number >> 8) ||
            saved[offset + 3] != static_cast<unsigned char>(number >> 16) ||
            saved[offset + 4] != static_cast<unsigned char>(number >> 24)) return 17;
        for (std::size_t index = 5; index < 188; ++index) {
            if (saved[offset + index] != static_cast<unsigned char>(number + index * 37)) return 18;
        }
    }
    if (dtv_read_stream(stream, first_packet.data(), first_packet.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_SLOW_CONSUMER || output_size != 0 ||
        error.code != DTV_ERROR_SLOW_CONSUMER || dtv_get_dropped_bytes(stream) != 0 ||
        dtv_get_desyncs(stream) != 0) return 19;

    set_scan_stream_failure_modes(0, 0, 1, 1);
    if (dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_PENDING) return 20;
    dtv_release_stream(stream);
    auto close_state = dtv_get_stream_close_state(stream, &error);
    for (unsigned attempt = 0; attempt < 4 && close_state == DTV_CLOSE_PENDING; ++attempt)
        close_state = dtv_get_stream_close_state(stream, &error);
    if (close_state != DTV_CLOSE_CLOSED) return 21;

    const auto previous_generation = scan_stream_previous_generation();
    if (previous_generation == 0 ||
        dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr)
        return 22;
    set_scan_stream_generation_override(previous_generation);
    if (!complete_in(0x81, 1) ||
        dtv_read_stream(stream, first_packet.data(), first_packet.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_TIMEOUT || output_size != 0) return 23;
    if (!complete_in(0x81, 2)) return 24;
    std::array<unsigned char, 376> reopened_packets{};
    if (dtv_read_stream(stream, reopened_packets.data(), reopened_packets.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != reopened_packets.size() ||
        reopened_packets[0] != 0x47 || reopened_packets[1] != 1 ||
        reopened_packets[188] != 0x47 || reopened_packets[189] != 2) return 25;
    if (dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 26;
    dtv_release_stream(stream);
    if (dtv_close_stream(other_stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 27;
    dtv_release_stream(other_stream);
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED) return 28;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

int public_stream_wait_lifecycle()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->actual_length_limit = 7;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    set_scan_public_open_case(13);
    dtv::internal::CompleteGroupBatch batch;
    static std::uint64_t snapshot_token = 1200;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, snapshot_token++), snapshot(context, 4, snapshot_token++)},
            &batch).code != DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(2)) {
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 3;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result != DTV_ERROR_OK || !backend->wait_for_submissions(2)) return 4;
    auto *const *devices = dtv_get_devices(root);
    auto *const *receivers = devices == nullptr ? nullptr : dtv_get_receivers(devices[0]);
    if (receivers == nullptr || receivers[0] == nullptr || receivers[1] == nullptr) return 5;
    dtv_stream *stream = nullptr;
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr)
        return 6;
    std::array<std::uint8_t, 376> output{};
    std::uint64_t output_size = 0;
    const dtv_call_options short_wait{sizeof(dtv_call_options), 20};
    set_scan_stream_generation_override(UINT64_MAX);
    auto callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1) ||
        dtv_read_stream(stream, output.data(), output.size(), &short_wait, &output_size, &error) !=
            DTV_ERROR_TIMEOUT) return 7;

    std::atomic<bool> read_started{false};
    std::atomic<std::int32_t> read_result{DTV_ERROR_INTERNAL};
    std::uint64_t read_size = UINT64_MAX;
    std::thread reader([&] {
        read_started.store(true, std::memory_order_release);
        read_result.store(dtv_read_stream(stream, output.data(), output.size(), nullptr,
                                          &read_size, &error), std::memory_order_release);
    });
    for (unsigned attempt = 0; attempt < 2000 && !read_started.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!read_started.load(std::memory_order_acquire)) { reader.join(); return 8; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const auto run_operation = [&](dtv::spi::PublicOperationTargetKind target_kind,
                                   std::uint64_t target_id, unsigned char transaction,
                                   unsigned char resource_key = 42) {
        const std::array<unsigned char, 5> arguments{1, resource_key, 0, transaction, 'Q'};
        const dtv::spi::PublicOperationRequest request{1, target_kind, target_id,
            std::span<const unsigned char>(arguments.data(), arguments.size())};
        std::array<unsigned char, 32> reply{};
        std::size_t reply_size = 0;
        dtv_internal_result result{};
        std::thread operation([&] {
            result = dtv::internal::run_public_operation_for_test(root, request,
                dtv_timeout{1000, 0}, reply, &reply_size);
        });
        const auto out_count = backend->endpoint_submission_count(0x02) + 1;
        set_scan_shared_reply_enabled(true);
        set_scan_shared_transaction_override(transaction);
        if (!backend->wait_for_endpoint_submissions(0x02, out_count)) {
            operation.join();
            return false;
        }
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(callbacks + 1)) { operation.join(); return false; }
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        const bool callback_done = backend->wait_for_callbacks(callbacks + 1);
        operation.join();
        set_scan_shared_reply_enabled(false);
        return callback_done && result.code == DTV_INTERNAL_RESULT_OK && reply_size == 7;
    };
    if (!run_operation(dtv::spi::PublicOperationTargetKind::receiver,
                       dtv_get_index(receivers[1]), 31) ||
        !run_operation(dtv::spi::PublicOperationTargetKind::card, 1, 32)) {
        dtv_cancel_stream(stream, &error);
        reader.join();
        return 9;
    }
    if (read_result.load(std::memory_order_acquire) != DTV_ERROR_INTERNAL) {
        dtv_cancel_stream(stream, &error);
        reader.join();
        return 10;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_stream(stream, &error);
        reader.join();
        return 11;
    }
    reader.join();
    if (read_result.load() != DTV_ERROR_OK || read_size != output.size() ||
        output[0] != 0x47 || output[188] != 0x47) return 12;
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1) ||
        dtv_read_stream(stream, output.data(), output.size(), &short_wait, &output_size, &error) !=
            DTV_ERROR_OK || output_size != 0) return 13;

    const auto shared_resource_out = backend->endpoint_submission_count(0x02);
    std::atomic<bool> shared_card_done{false};
    bool shared_card_result = false;
    std::thread shared_card([&] {
        shared_card_result = run_operation(dtv::spi::PublicOperationTargetKind::card, 1, 33, 90);
        shared_card_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (shared_card_done.load(std::memory_order_acquire) ||
        backend->endpoint_submission_count(0x02) != shared_resource_out) {
        dtv_cancel_stream(stream, &error);
        shared_card.join();
        return 14;
    }
    set_scan_stream_failure_modes(0, 0, 1, 1);
    if (dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_PENDING) {
        shared_card.join();
        return 15;
    }
    dtv_release_stream(stream);
    if (dtv_get_stream_close_state(stream, &error) != DTV_CLOSE_PENDING ||
        backend->endpoint_submission_count(0x02) != shared_resource_out ||
        dtv_get_stream_close_state(stream, &error) != DTV_CLOSE_CLOSED) {
        shared_card.join();
        return 16;
    }
    shared_card.join();
    if (!shared_card_done.load(std::memory_order_acquire) || !shared_card_result) return 17;
    dtv_release_stream(stream);

    set_scan_stream_failure_modes(0, 0, 0, 0);
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr)
        return 18;
    const auto previous_generation = scan_stream_previous_generation();
    if (previous_generation == 0) return 19;
    set_scan_stream_generation_override(previous_generation);
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1) ||
        dtv_read_stream(stream, output.data(), output.size(), &short_wait, &output_size, &error) !=
            DTV_ERROR_TIMEOUT) return 20;
    read_result.store(DTV_ERROR_INTERNAL, std::memory_order_release);
    std::thread cancel_reader([&] {
        read_result.store(dtv_read_stream(stream, output.data(), output.size(), nullptr,
                                           &read_size, &error), std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (dtv_cancel_stream(stream, &error) != DTV_ERROR_OK) {
        cancel_reader.join();
        return 21;
    }
    cancel_reader.join();
    if (read_result.load() != DTV_ERROR_CANCELLED ||
        dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 22;
    dtv_release_stream(stream);

    set_scan_stream_failure_modes(1, 0, 0, 0);
    stream = nullptr;
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_USB_IO || stream != nullptr)
        return 23;
    set_scan_stream_failure_modes(0, 1, 0, 0);
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_USB_IO || stream != nullptr)
        return 24;
    set_scan_stream_failure_modes(0, 2, 0, 0);
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_USB_IO || stream != nullptr)
        return 25;

    set_scan_stream_failure_modes(0, 0, 0, 0);
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr)
        return 26;
    read_result.store(DTV_ERROR_INTERNAL, std::memory_order_release);
    std::thread close_reader([&] {
        read_result.store(dtv_read_stream(stream, output.data(), output.size(), nullptr,
                                           &read_size, &error), std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) {
        dtv_cancel_stream(stream, &error);
        close_reader.join();
        return 27;
    }
    close_reader.join();
    if (read_result.load() != DTV_ERROR_CANCELLED ||
        dtv_get_stream_close_state(stream, &error) != DTV_CLOSE_CLOSED) return 28;
    dtv_release_stream(stream);
    set_scan_stream_failure_modes(0, 0, 1, 1);
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr)
        return 29;
    read_result.store(DTV_ERROR_INTERNAL, std::memory_order_release);
    std::atomic<bool> parent_reader_done{false};
    dtv_error parent_reader_error{};
    parent_reader_error.struct_size = sizeof(parent_reader_error);
    std::thread parent_close_reader([&] {
        read_result.store(dtv_read_stream(stream, output.data(), output.size(), nullptr,
                                           &read_size, &parent_reader_error), std::memory_order_release);
        parent_reader_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_PENDING) {
        dtv_cancel_stream(stream, &error);
        parent_close_reader.join();
        return 30;
    }
    for (unsigned attempt = 0; attempt < 2000 &&
         !parent_reader_done.load(std::memory_order_acquire); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!parent_reader_done.load(std::memory_order_acquire)) {
        dtv_cancel_stream(stream, &error);
        parent_close_reader.join();
        return 31;
    }
    parent_close_reader.join();
    if (read_result.load() != DTV_ERROR_CANCELLED ||
        dtv_get_enclosure_close_state(root, &error) != DTV_CLOSE_PENDING ||
        dtv_get_enclosure_close_state(root, &error) != DTV_CLOSE_CLOSED ||
        dtv_get_stream_close_state(stream, &error) != DTV_CLOSE_CLOSED) return 32;
    dtv_release_stream(stream);
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

int public_stream_pid_filter()
{
    set_scan_public_open_case(16);
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->actual_length_limit = 7;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 191), snapshot(context, 4, 192)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(2)) {
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 3;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result != DTV_ERROR_OK || !backend->wait_for_submissions(2)) return 4;
    auto *const *devices = dtv_get_devices(root);
    auto *const *receivers = devices == nullptr ? nullptr : dtv_get_receivers(devices[0]);
    if (receivers == nullptr || receivers[0] == nullptr || receivers[1] == nullptr) return 5;
    std::uint32_t support = DTV_UNKNOWN;
    if (dtv_get_receiver_support(receivers[0], DTV_OP_OUTPUT_PID_FILTER, &support, &error) !=
            DTV_ERROR_OK || support != DTV_YES) return 6;
    dtv_stream *stream = nullptr;
    dtv_stream *other_stream = nullptr;
    if (dtv_open_stream(receivers[0], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr ||
        dtv_open_stream(receivers[1], nullptr, &other_stream, &error) != DTV_ERROR_OK ||
        other_stream == nullptr) return 7;
    if (dtv_get_receiver_support(receivers[1], DTV_OP_OUTPUT_PID_FILTER, &support, &error) !=
            DTV_ERROR_OK || support != DTV_NO) return 8;
    const std::uint16_t pid_a = 0x100;
    const std::uint16_t pid_b = 0x200;
    const auto outputs_before_unsupported = backend->output_snapshot().size();
    const auto unsupported_result = dtv_set_pid_filter(other_stream, &pid_a, 1, &error);
    if (unsupported_result != DTV_ERROR_UNSUPPORTED || error.code != DTV_ERROR_UNSUPPORTED ||
        backend->output_snapshot().size() != outputs_before_unsupported) {
        std::fprintf(stderr, "PID unsupported: result=%d error=%d outputs=%zu/%zu\n",
            unsupported_result, error.code, backend->output_snapshot().size(),
            outputs_before_unsupported);
        return 9;
    }

    const auto send_packets = [&](unsigned count) {
        const auto processed = scan_pid_processed();
        const auto callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81, count);
        return backend->wait_for_callbacks(callbacks + count) &&
            wait_for_scan_pid_processed(processed + count);
    };
    if (!send_packets(2)) return 10;

    const auto apply_filter = [&](const std::uint16_t *pids, std::uint32_t count,
                                  std::int32_t expected_result, bool reject) {
        std::atomic<std::int32_t> result{DTV_ERROR_INTERNAL};
        dtv_error operation_error{};
        operation_error.struct_size = sizeof(operation_error);
        const auto output_count = backend->output_snapshot().size();
        const auto output_submission = backend->endpoint_submission_count(0x02) + 1;
        std::thread operation([&] {
            result.store(dtv_set_pid_filter(stream, pids, count, &operation_error),
                         std::memory_order_release);
        });
        if (!backend->wait_for_endpoint_submissions(0x02, output_submission)) {
            std::fprintf(stderr, "PID operation output wait failed\n");
            dtv_cancel_stream(stream, &error);
            operation.join();
            std::fprintf(stderr, "PID operation early result=%d error=%d\n",
                result.load(std::memory_order_acquire), operation_error.code);
            return false;
        }
        const auto outputs = backend->output_snapshot();
        std::vector<unsigned char> expected_output{0x57,
            static_cast<unsigned char>(count), static_cast<unsigned char>(count >> 8),
            static_cast<unsigned char>(count >> 16), static_cast<unsigned char>(count >> 24)};
        for (std::uint32_t index = 0; index < count; ++index) {
            expected_output.push_back(static_cast<unsigned char>(pids[index]));
            expected_output.push_back(static_cast<unsigned char>(pids[index] >> 8));
        }
        if (outputs.size() != output_count + 1 || outputs.back() != expected_output) {
            std::fprintf(stderr, "PID operation output mismatch: count=%zu expected=%zu\n",
                outputs.size(), output_count + 1);
            dtv_cancel_stream(stream, &error);
            operation.join();
            return false;
        }
        auto callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(callbacks + 1)) {
            std::fprintf(stderr, "PID operation OUT completion failed\n");
            dtv_cancel_stream(stream, &error);
            operation.join();
            return false;
        }
        const auto receiver_one_count = scan_pid_receiver_one_processed();
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x82);
        if (!backend->wait_for_callbacks(callbacks + 1) ||
            !wait_for_scan_pid_receiver_one_processed(receiver_one_count + 1)) {
            std::fprintf(stderr, "PID operation peer completion failed\n");
            dtv_cancel_stream(stream, &error);
            operation.join();
            return false;
        }
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        if (!backend->wait_for_callbacks(callbacks + 1)) {
            std::fprintf(stderr, "PID operation IN reply completion failed\n");
            dtv_cancel_stream(stream, &error);
            operation.join();
            return false;
        }
        operation.join();
        if (result.load(std::memory_order_acquire) != expected_result ||
            operation_error.code != expected_result)
            std::fprintf(stderr, "PID operation result: result=%d error=%d expected=%d\n",
                result.load(std::memory_order_acquire), operation_error.code, expected_result);
        return result.load(std::memory_order_acquire) == expected_result &&
            operation_error.code == expected_result && (reject || expected_result == DTV_ERROR_OK ||
                expected_result == DTV_ERROR_USB_IO);
    };
    if (!apply_filter(&pid_a, 1, DTV_ERROR_OK, false)) return 11;

    const auto expected_packet = [](std::uint32_t number, std::uint16_t pid) {
        std::array<unsigned char, 188> packet{};
        packet[0] = 0x47;
        packet[1] = static_cast<unsigned char>((pid >> 8) & 0x1f);
        packet[2] = static_cast<unsigned char>(pid);
        packet[3] = 0x10;
        for (std::size_t index = 4; index < packet.size(); ++index)
            packet[index] = static_cast<unsigned char>(number + index * 29);
        return packet;
    };
    std::array<unsigned char, 376> packets{};
    std::uint64_t output_size = 0;
    const dtv_call_options read_wait{sizeof(dtv_call_options), 1000};
    if (dtv_read_stream(stream, packets.data(), packets.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != packets.size()) return 12;
    const auto old_a = expected_packet(0, pid_a);
    const auto old_b = expected_packet(1, pid_b);
    if (!std::equal(old_a.begin(), old_a.end(), packets.begin()) ||
        !std::equal(old_b.begin(), old_b.end(), packets.begin() + 188)) return 13;
    if (!send_packets(2)) return 14;
    std::array<unsigned char, 188> packet{};
    if (dtv_read_stream(stream, packet.data(), packet.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != packet.size() ||
        packet != expected_packet(2, pid_a) || dtv_get_dropped_bytes(stream) != 0 ||
        dtv_get_desyncs(stream) != 0) return 15;
    const dtv_call_options short_wait{sizeof(dtv_call_options), 10};
    if (dtv_read_stream(stream, packet.data(), packet.size(), &short_wait,
            &output_size, &error) != DTV_ERROR_TIMEOUT || output_size != 0) return 16;

    if (!apply_filter(&pid_b, 1, DTV_ERROR_OK, false) || !send_packets(2) ||
        dtv_read_stream(stream, packet.data(), packet.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != packet.size() ||
        packet != expected_packet(5, pid_b) ||
        dtv_read_stream(stream, packet.data(), packet.size(), &short_wait,
            &output_size, &error) != DTV_ERROR_TIMEOUT || output_size != 0) return 17;

    set_scan_pid_fail_next(true);
    if (!apply_filter(&pid_a, 1, DTV_ERROR_USB_IO, true) || !send_packets(2) ||
        dtv_read_stream(stream, packet.data(), packet.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != packet.size() ||
        packet != expected_packet(7, pid_b) ||
        dtv_read_stream(stream, packet.data(), packet.size(), &short_wait,
            &output_size, &error) != DTV_ERROR_TIMEOUT || output_size != 0) return 18;

    if (!apply_filter(nullptr, 0, DTV_ERROR_OK, false) || !send_packets(2)) return 19;
    const auto cleared_a = expected_packet(8, pid_a);
    const auto cleared_b = expected_packet(9, pid_b);
    if (dtv_read_stream(stream, packets.data(), packets.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != packets.size() ||
        !std::equal(cleared_a.begin(), cleared_a.end(), packets.begin()) ||
        !std::equal(cleared_b.begin(), cleared_b.end(), packets.begin() + 188)) return 20;

    std::array<unsigned char, 188> other_packet{};
    if (dtv_read_stream(other_stream, other_packet.data(), other_packet.size(), &read_wait,
            &output_size, &error) != DTV_ERROR_OK || output_size != other_packet.size() ||
        other_packet[0] != 0x47 || other_packet[1] != 0x01) return 21;
    set_scan_card_state_notification(DTV_CARD_PRESENT, false);
    set_scan_queue_notification_enabled(true);
    auto callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1)) return 22;
    const auto notification_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(read_wait.timeout_ms);
    while (dtv_get_card_state(root) != DTV_CARD_PRESENT &&
           std::chrono::steady_clock::now() < notification_deadline)
        std::this_thread::yield();
    set_scan_queue_notification_enabled(false);
    if (dtv_get_card_state(root) != DTV_CARD_PRESENT) return 23;

    const auto cancel_pid = pid_a;
    std::atomic<std::int32_t> cancel_result{DTV_ERROR_INTERNAL};
    dtv_error cancel_operation_error{};
    cancel_operation_error.struct_size = sizeof(cancel_operation_error);
    const auto cancel_output = backend->endpoint_submission_count(0x02) + 1;
    std::thread cancelling([&] {
        cancel_result.store(dtv_set_pid_filter(stream, &cancel_pid, 1,
            &cancel_operation_error), std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, cancel_output)) {
        dtv_cancel_stream(stream, &error);
        cancelling.join();
        return 24;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x02);
    if (!backend->wait_for_callbacks(callbacks + 1) ||
        dtv_cancel_stream(stream, &error) != DTV_ERROR_OK) {
        cancelling.join();
        return 25;
    }
    cancelling.join();
    if (cancel_result.load(std::memory_order_acquire) != DTV_ERROR_CANCELLED ||
        cancel_operation_error.code != DTV_ERROR_CANCELLED) return 26;
    set_scan_stream_failure_modes(0, 0, 1, 0);
    if (dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_PENDING) return 27;
    dtv_release_stream(stream);
    auto close_state = dtv_get_stream_close_state(stream, &error);
    for (unsigned attempt = 0; attempt < 4 && close_state == DTV_CLOSE_PENDING; ++attempt)
        close_state = dtv_get_stream_close_state(stream, &error);
    if (close_state != DTV_CLOSE_CLOSED) return 28;
    const auto old_generation = scan_stream_previous_generation();
    if (old_generation == 0 || dtv_open_stream(receivers[0], nullptr, &stream, &error) !=
            DTV_ERROR_OK || stream == nullptr) return 29;
    set_scan_stream_generation_override(old_generation);
    if (!send_packets(1)) return 30;
    const auto stale_result = dtv_read_stream(stream, packet.data(), packet.size(), &short_wait,
        &output_size, &error);
    if (stale_result != DTV_ERROR_TIMEOUT || output_size != 0) {
        std::fprintf(stderr, "PID stale generation read: result=%d size=%llu\n",
            stale_result, static_cast<unsigned long long>(output_size));
        return 30;
    }
    if (!send_packets(2)) return 30;
    const auto reopen_result = dtv_read_stream(stream, packets.data(), packets.size(), &read_wait,
        &output_size, &error);
    const auto reopened_b = expected_packet(1, pid_b);
    const auto reopened_a = expected_packet(2, pid_a);
    if (reopen_result != DTV_ERROR_OK || output_size != packets.size() ||
        !std::equal(reopened_b.begin(), reopened_b.end(), packets.begin()) ||
        !std::equal(reopened_a.begin(), reopened_a.end(), packets.begin() + 188)) {
        std::fprintf(stderr, "PID reopened ordered read: result=%d size=%llu\n",
            reopen_result, static_cast<unsigned long long>(output_size));
        return 30;
    }
    if (dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 31;
    dtv_release_stream(stream);
    if (dtv_close_stream(other_stream, nullptr, &error) != DTV_CLOSE_CLOSED) return 32;
    dtv_release_stream(other_stream);
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED) return 33;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

int public_open_shared_in()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->actual_length_limit = 7;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    set_scan_public_open_case(9);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 177), snapshot(context, 4, 178)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(2)) {
        opening.join();
        return 3;
    }
    backend->complete_endpoint(0x81, 1);
    backend->complete_endpoint(0x82, 1);
    opening.join();
    if (open_result != DTV_ERROR_OK || backend->worker_ids.size() != 1 ||
        !backend->wait_for_submissions(2) || scan_shared_packet_count() < 2 ||
        scan_shared_generation(0) == 0 || scan_shared_generation(1) == 0 ||
        scan_shared_sequence(0) == 0 || scan_shared_sequence(1) == 0 ||
        scan_shared_length(0) != 7 || scan_shared_length(1) != 7) {
        std::fprintf(stderr, "shared open state: open=%d workers=%zu packets=%u gen=%llu/%llu seq=%llu/%llu len=%zu/%zu\n",
            open_result, backend->worker_ids.size(), scan_shared_packet_count(),
            static_cast<unsigned long long>(scan_shared_generation(0)),
            static_cast<unsigned long long>(scan_shared_generation(1)),
            static_cast<unsigned long long>(scan_shared_sequence(0)),
            static_cast<unsigned long long>(scan_shared_sequence(1)),
            scan_shared_length(0), scan_shared_length(1));
        return 4;
    }

    set_scan_shared_reply_enabled(true);
    const auto session = scan_shared_open_session();
    std::array<unsigned char, 32> output{};
    std::size_t output_size = 0;
    dtv::spi::PublicOpenWaitResult wait_result{};
    std::atomic<bool> wait_started{false};
    dtv_internal_result wait_code{};
    std::thread waiting([&] {
        wait_started.store(true, std::memory_order_release);
        wait_code = session.wait_reply(session.user, scan_shared_token(0), nullptr,
            dtv_timeout{250, 0}, &wait_result, output, &output_size);
    });
    while (!wait_started.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto result = session.cancel_reply(session.user, scan_shared_token(0));
    waiting.join();
    if (result.code != DTV_INTERNAL_RESULT_OK) return 5;
    result = wait_code;
    if (result.code != DTV_INTERNAL_RESULT_OK ||
        wait_result != dtv::spi::PublicOpenWaitResult::cancelled) return 5;
    const auto first_sequence = scan_shared_sequence(0);
    const auto second_sequence = scan_shared_sequence(1);
    backend->complete_endpoint(0x81, 1);
    backend->complete_endpoint(0x82, 1);
    for (unsigned attempt = 0; attempt < 2000 && scan_shared_packet_count() < 4; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (scan_shared_packet_count() < 4 || !backend->wait_for_submissions(2) ||
        scan_shared_sequence(0) <= first_sequence || scan_shared_sequence(1) <= second_sequence) return 6;
    result = session.wait_reply(session.user, scan_shared_token(1), nullptr,
        dtv_timeout{100, 0}, &wait_result, output, &output_size);
    if (result.code != DTV_INTERNAL_RESULT_OK ||
        wait_result != dtv::spi::PublicOpenWaitResult::completed || output_size != 7) return 7;

    std::uint64_t late_token = 0;
    result = session.register_reply(session.user, 7, 100, scan_shared_generation(0), &late_token);
    if (result.code != DTV_INTERNAL_RESULT_OK) return 8;
    backend->complete_endpoint(0x81, 1);
    backend->complete_endpoint(0x82, 1);
    for (unsigned attempt = 0; attempt < 2000 && scan_shared_packet_count() < 6; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (scan_shared_packet_count() < 6) return 9;
    result = session.wait_reply(session.user, late_token, nullptr,
        dtv_timeout{5, 0}, &wait_result, output, &output_size);
    if (result.code != DTV_INTERNAL_RESULT_OK ||
        wait_result != dtv::spi::PublicOpenWaitResult::deadline) return 10;
    std::uint64_t terminal_token = 0;
    result = session.register_reply(session.user, 7, 999, scan_shared_generation(0), &terminal_token);
    if (result.code != DTV_INTERNAL_RESULT_OK) return 11;
    backend->fail_submit_call = backend->submit_calls + 1;
    backend->complete_endpoint(0x81, 1);
    result = session.wait_reply(session.user, terminal_token, nullptr,
        dtv_timeout{100, 0}, &wait_result, output, &output_size);
    if (result.code != DTV_INTERNAL_RESULT_USB_IO ||
        wait_result != dtv::spi::PublicOpenWaitResult::failed) return 12;
    backend->defer_cancel_completion = true;
    const dtv_call_options short_close{sizeof(dtv_call_options), 1};
    if (dtv_close_enclosure(root, &short_close, &error) != DTV_CLOSE_PENDING ||
        backend->close_count != 0) return 13;
    backend->complete_endpoint(0x81, 1);
    backend->complete_endpoint(0x82, 1);
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED ||
        backend->close_count != 2 || backend->worker_ids.size() != 1 ||
        std::count_if(backend->operations.begin(), backend->operations.end(),
            [](const auto &operation) { return operation.kind == 'R'; }) != 2) return 14;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

int public_open_shared_depth_order()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->actual_length_limit = 7;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    set_scan_public_open_case(10);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 217), snapshot(context, 4, 218)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(3)) {
        backend->complete_endpoint_reverse(0x81);
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 3;
    }
    backend->complete_endpoint_reverse(0x81);
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result != DTV_ERROR_OK || !backend->wait_for_submissions(3) ||
        backend->worker_ids.size() != 1) return 4;
    std::array<std::uint64_t, 2> first_endpoint_sequences{};
    std::size_t endpoint_count = 0;
    for (std::size_t index = 0; index < scan_shared_packet_count(); ++index) {
        if (scan_shared_order_endpoint(index) == 0x81 && endpoint_count < first_endpoint_sequences.size())
            first_endpoint_sequences[endpoint_count++] = scan_shared_order_at(index);
    }
    if (endpoint_count != 2 || first_endpoint_sequences[0] >= first_endpoint_sequences[1]) return 5;

    const auto packet_count = scan_shared_packet_count();
    const auto callback_count = backend->callback_count_snapshot();
    backend->complete_endpoint_reverse(0x81);
    if (!backend->wait_for_callbacks(callback_count + 1) ||
        scan_shared_packet_count() != packet_count) return 6;
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callback_count + 2) ||
        !backend->wait_for_submissions(3)) return 7;
    if (scan_shared_packet_count() != packet_count + 2 ||
        scan_shared_order_endpoint(packet_count) != 0x81 ||
        scan_shared_order_endpoint(packet_count + 1) != 0x81 ||
        scan_shared_order_at(packet_count) >= scan_shared_order_at(packet_count + 1)) return 8;

    auto session = scan_shared_open_session();
    std::uint64_t token = 0;
    auto result = session.register_reply(session.user, 7, 777,
        scan_shared_generation(0), &token);
    if (result.code != DTV_INTERNAL_RESULT_OK) return 9;
    set_scan_shared_transaction_override(777);
    std::atomic<bool> wait_started{false};
    dtv::spi::PublicOpenWaitResult wait_result{};
    std::array<unsigned char, 32> output{};
    std::size_t output_size = 0;
    dtv_internal_result wait_code{};
    std::thread waiting([&] {
        wait_started.store(true, std::memory_order_release);
        wait_code = session.wait_reply(session.user, token, nullptr, dtv_timeout{500, 0},
            &wait_result, output, &output_size);
    });
    while (!wait_started.load(std::memory_order_acquire)) std::this_thread::yield();
    set_scan_shared_reply_enabled(true);
    set_scan_shared_stale_generation(true);
    const auto replies_callback_count = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(replies_callback_count + 1)) {
        set_scan_shared_stale_generation(false);
        backend->complete_endpoint(0x81);
        waiting.join();
        return 10;
    }
    set_scan_shared_stale_generation(false);
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(replies_callback_count + 2)) {
        waiting.join();
        return 11;
    }
    waiting.join();
    set_scan_shared_transaction_override(0);
    if (wait_code.code != DTV_INTERNAL_RESULT_OK ||
        wait_result != dtv::spi::PublicOpenWaitResult::completed || output_size != 7) return 12;
    backend->defer_cancel_completion = true;
    const dtv_call_options short_close{sizeof(dtv_call_options), 1};
    if (dtv_close_enclosure(root, &short_close, &error) != DTV_CLOSE_PENDING ||
        backend->close_count != 0) return 13;
    backend->complete_endpoint(0x81, 2);
    backend->complete_endpoint(0x82);
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED ||
        backend->close_count != 2 ||
        std::count_if(backend->operations.begin(), backend->operations.end(),
            [](const auto &operation) { return operation.kind == 'R'; }) != 2) return 14;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

bool send_card_packet(const std::shared_ptr<FakeBackend> &backend, std::uint8_t endpoint,
                      std::uint32_t state, bool stale_generation = false)
{
    set_scan_card_state_notification(state, stale_generation);
    const auto callbacks = backend->callback_count_snapshot();
    const auto packets = scan_shared_packet_count();
    const auto submissions = backend->endpoint_submission_count(endpoint);
    backend->complete_endpoint(endpoint);
    if (!backend->wait_for_callbacks(callbacks + 1)) return false;
    for (unsigned attempt = 0; attempt < 2000 && scan_shared_packet_count() == packets; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return scan_shared_packet_count() == packets + 1 &&
        backend->wait_for_endpoint_submissions(endpoint, submissions + 1);
}

bool send_lock_packet(const std::shared_ptr<FakeBackend> &backend, std::uint8_t endpoint,
                      std::uint32_t receiver, std::uint32_t state, bool stale_generation = false,
                      std::uint64_t operation_epoch = UINT64_MAX)
{
    set_scan_receiver_lock_notification(receiver, state, stale_generation);
    set_scan_receiver_lock_operation_epoch(operation_epoch);
    const auto callbacks = backend->callback_count_snapshot();
    const auto packets = scan_shared_packet_count();
    const auto submissions = backend->endpoint_submission_count(endpoint);
    backend->complete_endpoint(endpoint);
    if (!backend->wait_for_callbacks(callbacks + 1)) return false;
    for (unsigned attempt = 0; attempt < 2000 && scan_shared_packet_count() == packets; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto completed = scan_shared_packet_count() == packets + 1 &&
        backend->wait_for_endpoint_submissions(endpoint, submissions + 1);
    set_scan_receiver_lock_operation_epoch(UINT64_MAX);
    return completed;
}

int drive_card_event(void *user, std::uint32_t step)
{
    auto *const backend = static_cast<std::shared_ptr<FakeBackend> *>(user);
    if (backend == nullptr || !*backend) return 1;
    switch (step) {
    case 1:
        return send_card_packet(*backend, 0x81, DTV_CARD_PRESENT, true) &&
            send_card_packet(*backend, 0x82, DTV_CARD_PRESENT) &&
            send_card_packet(*backend, 0x81, 99) &&
            send_card_packet(*backend, 0x81, DTV_CARD_ABSENT) ? 0 : 1;
    case 2:
        return send_card_packet(*backend, 0x81, DTV_CARD_PRESENT) ? 0 : 1;
    case 3:
        return send_card_packet(*backend, 0x81, DTV_CARD_PRESENT) ? 0 : 1;
    case 4:
        return send_card_packet(*backend, 0x81, DTV_CARD_ABSENT) ? 0 : 1;
    case 5:
        return send_card_packet(*backend, 0x81, DTV_CARD_PRESENT) ? 0 : 1;
    case 6:
        dtv::internal::set_public_discovery_journal_hook_for_test([] {
            throw std::bad_alloc();
        });
        return send_lock_packet(*backend, 0x81, 0, 1) ? 0 : 1;
    case 7:
        (*backend)->set_defer_cancel_completion(true);
        return 0;
    case 8: {
        const auto callbacks = (*backend)->callback_count_snapshot();
        (*backend)->set_defer_cancel_completion(false);
        return (*backend)->wait_for_callbacks(callbacks + 2) ? 0 : 1;
    }
    case 9:
        return send_lock_packet(*backend, 0x81, 0, 0) ? 0 : 1;
    case 10:
        return send_lock_packet(*backend, 0x81, 1, 1) ? 0 : 1;
    case 11:
        return send_lock_packet(*backend, 0x81, 0, 1) ? 0 : 1;
    case 12:
        return send_lock_packet(*backend, 0x81, 1, 0) ? 0 : 1;
    case 13:
        return send_lock_packet(*backend, 0x81, 0, 1) ? 0 : 1;
    case 14:
        return send_lock_packet(*backend, 0x81, 99, 1) ? 0 : 1;
    case 15:
        return send_lock_packet(*backend, 0x81, 0, 0, true) ? 0 : 1;
    case 16:
        return send_lock_packet(*backend, 0x81, 0, 0) ? 0 : 1;
    case 17:
        return send_lock_packet(*backend, 0x82, 0, 0) ? 0 : 1;
    case 18:
        return send_lock_packet(*backend, 0x81, 0, 2) ? 0 : 1;
    case 19:
        return send_lock_packet(*backend, 0x81, 0, 0, false, 0) ? 0 : 1;
    case 20:
        return send_lock_packet(*backend, 0x81, 0, 0) ? 0 : 1;
    case 21:
        return send_lock_packet(*backend, 0x81, 1, 1) ? 0 : 1;
    case 22:
        return send_lock_packet(*backend, 0x81, 1, 1) ? 0 : 1;
    case 23:
        return send_lock_packet(*backend, 0x81, 0, 1) ? 0 : 1;
    default:
        return 1;
    }
}

int public_open_card_events()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    set_scan_public_open_case(12);
    dtv::internal::CompleteGroupBatch batch;
    static std::uint64_t snapshot_token = 1600;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, snapshot_token++), snapshot(context, 4, snapshot_token++)},
            &batch).code != DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    std::thread open_completer([&] {
        if (backend->wait_for_endpoint_submissions(0x81, 1)) backend->complete_endpoint(0x81);
        if (backend->wait_for_endpoint_submissions(0x82, 1)) backend->complete_endpoint(0x82);
    });
    const auto consumer_result = card_events_public_consumer(drive_card_event, &backend);
    open_completer.join();
    dtv::internal::set_public_discovery_journal_hook_for_test({});
    dtv::internal::set_public_discovery_batch_for_test({});
    const auto tune_verified = scan_tune_call_count() == 15 && scan_tune_effect_count() == 14 &&
        scan_tune_peer_was_checked() && scan_tune_card_was_checked() &&
        scan_tune_record_matches(0, 0, DTV_SYSTEM_ISDB_T, UINT64_C(5931428571), true,
            DTV_TUNE_SELECTOR_TSID, 0x1234, DTV_LNA_ON) &&
        scan_tune_record_matches(1, 1, DTV_SYSTEM_ISDB_S, UINT64_C(11700000000), true,
            DTV_TUNE_SELECTOR_SLOT, 1, DTV_LNA_OFF) &&
        scan_tune_record_matches(6, 0, DTV_SYSTEM_ISDB_T, UINT64_C(473142857), true,
            DTV_TUNE_SELECTOR_NONE, 0, DTV_LNA_ON) &&
        scan_tune_record_matches(7, 0, DTV_SYSTEM_ISDB_T, UINT64_C(473142857), false,
            DTV_TUNE_SELECTOR_NONE, 0, DTV_LNA_DEFAULT) &&
        scan_tune_record_matches(8, 0, DTV_SYSTEM_ISDB_T, UINT64_C(767142857), true,
            DTV_TUNE_SELECTOR_NONE, 0, DTV_LNA_OFF) &&
        scan_tune_record_matches(9, 1, DTV_SYSTEM_ISDB_S, UINT64_C(1049480000), true,
            DTV_TUNE_SELECTOR_SLOT, 0, DTV_LNA_DEFAULT) &&
        scan_tune_record_matches(10, 1, DTV_SYSTEM_ISDB_S, UINT64_C(1049480000), true,
            DTV_TUNE_SELECTOR_TSID, 0x1234, DTV_LNA_ON) &&
        scan_tune_record_matches(11, 1, DTV_SYSTEM_ISDB_S, UINT64_C(1471440000), true,
            DTV_TUNE_SELECTOR_TSID, 0x1234, DTV_LNA_ON) &&
        scan_tune_record_matches(12, 1, DTV_SYSTEM_ISDB_S, UINT64_C(1613000000), true,
            DTV_TUNE_SELECTOR_SLOT, 0, DTV_LNA_DEFAULT) &&
        scan_tune_record_matches(13, 1, DTV_SYSTEM_ISDB_S, UINT64_C(1733000000), true,
            DTV_TUNE_SELECTOR_SLOT, 0, DTV_LNA_OFF) &&
        scan_tune_record_matches(14, 1, DTV_SYSTEM_ISDB_S, UINT64_C(2053000000), true,
            DTV_TUNE_SELECTOR_SLOT, 0, DTV_LNA_DEFAULT);
    set_scan_public_open_case(0);
    return consumer_result != 0 ? consumer_result : tune_verified ? 0 : 2;
}

int public_lnb_roundtrip()
{
    set_scan_public_open_case(17);
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    static std::uint64_t snapshot_token = 260;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, snapshot_token++), snapshot(context, 4, snapshot_token++)},
            &batch).code != DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::uint32_t support = DTV_UNKNOWN;
    if (dtv_get_enclosure_support(root, DTV_OP_LNB_POWER, &support, &error) != DTV_ERROR_OK ||
        support != DTV_YES || dtv_get_lnb_state(root) != DTV_LNB_UNKNOWN) return 3;
    std::atomic<std::int32_t> open_result{DTV_ERROR_INTERNAL};
    std::thread opening([&] {
        open_result.store(dtv_open_enclosure(root, nullptr, &error), std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x81, 1) ||
        !backend->wait_for_endpoint_submissions(0x82, 1)) {
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 4;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result.load(std::memory_order_acquire) != DTV_ERROR_OK) return 5;

    const auto run_lnb = [&](std::uint32_t target_state, std::uint64_t transaction) {
        set_scan_public_open_case(18);
        const auto before_unready = backend->endpoint_submission_count(0x02);
        const auto previous_state = dtv_get_lnb_state(root);
        const auto unready = target_state == DTV_LNB_ON
            ? dtv_lnb_on(root, nullptr, &error) : dtv_lnb_off(root, nullptr, &error);
        if (unready != DTV_ERROR_NOT_READY || error.code != DTV_ERROR_NOT_READY ||
            backend->endpoint_submission_count(0x02) != before_unready ||
            dtv_get_lnb_state(root) != previous_state) return false;

        set_scan_public_open_case(17);
        set_scan_shared_reply_enabled(true);
        set_scan_shared_transaction_override(transaction);
        const auto output_submission = backend->endpoint_submission_count(0x02) + 1;
        std::atomic<std::int32_t> result{DTV_ERROR_INTERNAL};
        dtv_error call_error{};
        call_error.struct_size = sizeof(call_error);
        const dtv_call_options call{sizeof(dtv_call_options), 1000};
        std::thread caller([&] {
            result.store(target_state == DTV_LNB_ON
                    ? dtv_lnb_on(root, &call, &call_error)
                    : dtv_lnb_off(root, &call, &call_error),
                std::memory_order_release);
        });
        if (!backend->wait_for_endpoint_submissions(0x02, output_submission)) {
            dtv_cancel_enclosure(root, &error);
            caller.join();
            set_scan_shared_reply_enabled(false);
            return false;
        }
        auto callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(callbacks + 1)) {
            caller.join();
            set_scan_shared_reply_enabled(false);
            return false;
        }
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        const bool reply_done = backend->wait_for_callbacks(callbacks + 1);
        caller.join();
        set_scan_shared_reply_enabled(false);
        const auto output = backend->output_snapshot();
        if (!reply_done || result.load(std::memory_order_acquire) != DTV_ERROR_OK ||
            call_error.code != DTV_ERROR_OK || dtv_get_lnb_state(root) != target_state ||
            output.empty() || output.back() != std::vector<unsigned char>{0x4c,
                static_cast<unsigned char>(target_state)}) return false;
        return true;
    };
    if (!run_lnb(DTV_LNB_ON, 191) || !run_lnb(DTV_LNB_OFF, 192)) return 6;
    set_scan_shared_reply_enabled(false);
    const auto timeout_submission = backend->endpoint_submission_count(0x02) + 1;
    std::atomic<std::int32_t> timeout_result{DTV_ERROR_INTERNAL};
    dtv_error timeout_error{};
    timeout_error.struct_size = sizeof(timeout_error);
    const dtv_call_options short_call{sizeof(dtv_call_options), 20};
    std::thread timeout_caller([&] {
        timeout_result.store(dtv_lnb_on(root, &short_call, &timeout_error),
            std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, timeout_submission)) {
        dtv_cancel_enclosure(root, &error);
        timeout_caller.join();
        return 7;
    }
    timeout_caller.join();
    if (timeout_result.load(std::memory_order_acquire) != DTV_ERROR_TIMEOUT ||
        timeout_error.code != DTV_ERROR_TIMEOUT || dtv_get_lnb_state(root) != DTV_LNB_UNKNOWN ||
        backend->endpoint_submission_count(0x02) != timeout_submission) return 8;
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED) return 7;
    if (dtv_get_lnb_state(root) != DTV_LNB_UNKNOWN) return 9;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    dtv::internal::set_public_discovery_batch_for_test({});
    set_scan_public_open_case(0);
    return 0;
}

int public_lnb_unsupported()
{
    auto backend = std::make_shared<FakeBackend>();
    auto context = dtv::internal::UsbContext::scan_only(backend);
    auto batch = complete_group(snapshot(context, 3, 290), snapshot(context, 4, 291));
    if (batch.groups.size() != 1) return 1;
    for (auto &item : batch.snapshots) {
        item.vendor_id = 0x1234;
        item.product_id = 0x0201;
    }
    batch.groups[0].model = &dtv_l3_model_scanfixture_scan_model_alias;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) {
        std::fprintf(stderr, "unsupported list=%p first=%p error=%d\n",
            static_cast<const void *>(enclosures),
            enclosures == nullptr ? nullptr : static_cast<void *>(enclosures[0]), error.code);
        return 2;
    }
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    const auto before = backend->endpoint_submission_count(0x02);
    std::uint32_t support = DTV_UNKNOWN;
    if (dtv_get_enclosure_support(root, DTV_OP_LNB_POWER, &support, &error) != DTV_ERROR_OK ||
        support != DTV_NO || dtv_lnb_on(root, nullptr, &error) != DTV_ERROR_UNSUPPORTED ||
        error.code != DTV_ERROR_UNSUPPORTED ||
        backend->endpoint_submission_count(0x02) != before) return 3;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    dtv::internal::set_public_discovery_batch_for_test({});
    return 0;
}

int public_firmware_roundtrip()
{
    reset_scan_firmware_records();
    set_scan_public_open_case(9);
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 301), snapshot(context, 4, 302)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1 ||
        batch.groups[0].functions.size() != 2) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    auto *const *devices = dtv_get_devices(root);
    if (devices == nullptr || devices[0] == nullptr || devices[1] == nullptr ||
        dtv_get_mode(devices[0]) != DTV_DEVICE_LOADER ||
        dtv_get_mode(devices[1]) != DTV_DEVICE_LOADER) return 3;
    auto *const first_target = devices[0];
    auto *const target = devices[1];
    const std::array<unsigned char, 5> package{0x10, 0x20, 0x30, 0x40, 0x50};
    std::atomic<std::int32_t> cancelled_result{DTV_ERROR_INTERNAL};
    dtv_error cancelled_error{};
    cancelled_error.struct_size = sizeof(cancelled_error);
    std::thread cancelled_sender([&] {
        cancelled_result.store(dtv_send_firmware(target, package.data(), package.size(), nullptr,
                                                  &cancelled_error), std::memory_order_release);
    });
    const bool cancel_submitted = backend->wait_for_endpoint_submissions(0x01, 1);
    dtv_error cancel_error{};
    cancel_error.struct_size = sizeof(cancel_error);
    const auto cancel_result = dtv_cancel_enclosure(root, &cancel_error);
    cancelled_sender.join();
    const bool cancel_without_mode_change = cancel_submitted && cancel_result == DTV_ERROR_OK &&
        cancel_error.code == DTV_ERROR_OK &&
        cancelled_result.load(std::memory_order_acquire) == DTV_ERROR_CANCELLED &&
        cancelled_error.code == DTV_ERROR_CANCELLED && dtv_get_mode(target) == DTV_DEVICE_LOADER;
    const auto send = [&](dtv_device *device, unsigned output_index, unsigned ack_index) {
        std::atomic<std::int32_t> result{DTV_ERROR_INTERNAL};
        dtv_error send_error{};
        send_error.struct_size = sizeof(send_error);
        std::thread sender([&] {
            result.store(dtv_send_firmware(device, package.data(), package.size(), nullptr,
                                           &send_error), std::memory_order_release);
        });
        const bool sent = backend->wait_for_endpoint_submissions(0x01, output_index);
        if (sent) backend->complete_endpoint(0x01);
        const bool waited_ack = backend->wait_for_endpoint_submissions(0x83, ack_index);
        if (waited_ack) backend->complete_endpoint(0x83);
        sender.join();
        return sent && waited_ack && result.load(std::memory_order_acquire) == DTV_ERROR_OK &&
            send_error.code == DTV_ERROR_OK;
    };
    const bool first_sent = send(first_target, 2, 1);
    const auto first_device_id = scan_firmware_device_id();
    const auto first_function = scan_firmware_function();
    const auto first_epoch = scan_firmware_epoch();
    const auto first_handle = scan_firmware_handle_index();
    const bool second_sent = send(target, 3, 2);
    const auto second_device_id = scan_firmware_device_id();
    const auto second_function = scan_firmware_function();
    const auto second_epoch = scan_firmware_epoch();
    const auto second_handle = scan_firmware_handle_index();
    auto *const *receivers = dtv_get_receivers(target);
    dtv_error list_error{};
    list_error.struct_size = sizeof(list_error);
    auto *const *next_list = dtv_list_enclosures(&list_error);
    const auto *const *devices_after = dtv_get_devices(root);
    const auto mode_after = dtv_get_mode(target);
    const bool stable = next_list != nullptr && next_list[0] == root &&
        devices_after == devices && devices_after[0] == first_target && devices_after[1] == target &&
        dtv_get_receivers(target) == receivers;
    std::uint32_t receiver_support = DTV_UNKNOWN;
    const bool support_ready = receivers != nullptr && receivers[0] != nullptr &&
        dtv_get_receiver_support(receivers[0], DTV_OP_SIGNAL, &receiver_support, &error) ==
            DTV_ERROR_OK && receiver_support == DTV_YES;
    std::atomic<std::int32_t> open_result{DTV_ERROR_INTERNAL};
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    const dtv_call_options open_options{sizeof(dtv_call_options), 1000};
    std::thread opener([&] {
        open_result.store(dtv_open_enclosure(root, &open_options, &open_error),
                          std::memory_order_release);
    });
    const bool opened_inputs = backend->wait_for_submissions(2);
    if (opened_inputs) {
        backend->complete_endpoint(0x81, 1);
        backend->complete_endpoint(0x82, 1);
    }
    opener.join();
    const bool shared_owner_live = opened_inputs &&
        open_result.load(std::memory_order_acquire) == DTV_ERROR_OK &&
        backend->wait_for_pending_endpoint(0x82);
    const bool opened_send = shared_owner_live && send(target, 4, 3);
    const auto opened_handle = scan_firmware_handle_index();
    const bool shared_owner_live_after = opened_send && backend->wait_for_pending_endpoint(0x82);
    dtv_signal signal{};
    const bool receiver_operation = opened_send && receivers != nullptr && receivers[0] != nullptr &&
        dtv_get_signal(receivers[0], nullptr, &signal, &error) == DTV_ERROR_OK;
    if (open_result.load(std::memory_order_acquire) == DTV_ERROR_OK)
        (void)dtv_close_enclosure(root, nullptr, &error);
    const auto outputs = backend->output_snapshot();
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    dtv::internal::set_public_discovery_batch_for_test({});
    if (!first_sent || !second_sent || first_device_id != dtv_get_device_id(first_target) ||
        second_device_id != dtv_get_device_id(target) || first_device_id == second_device_id ||
        first_function != 1 || second_function != 2 || first_handle != 0 || second_handle != 1 ||
        opened_handle != 1 || first_epoch == 0 ||
        second_epoch == 0 || first_epoch == second_epoch || outputs.size() != 4 ||
        outputs[0] != std::vector<unsigned char>(package.begin(), package.end()) ||
        outputs[1] != std::vector<unsigned char>(package.begin(), package.end()) ||
        outputs[2] != std::vector<unsigned char>(package.begin(), package.end()) ||
        outputs[3] != std::vector<unsigned char>(package.begin(), package.end()) ||
        !cancel_without_mode_change || !stable || !shared_owner_live_after ||
        !support_ready || !receiver_operation || open_error.code != DTV_ERROR_OK ||
        list_error.code != DTV_ERROR_OK ||
        mode_after != DTV_DEVICE_RUNTIME || scan_firmware_create_calls() != 4 ||
        scan_firmware_open_calls() != 4 || scan_firmware_identify_calls() != 7 ||
        scan_firmware_close_calls() != 4 || scan_firmware_destroy_calls() != 4) return 4;
    set_scan_public_open_case(0);
    return 0;
}

int public_firmware_unknown_no_io()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 401), snapshot(context, 4, 402)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    if (batch.snapshots.empty()) return 2;
    batch.snapshots[0].vendor_id = 0xffff;
    batch.snapshots[0].product_id = 0xffff;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 3;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    auto *const *devices = dtv_get_devices(root);
    dtv_device *unknown = nullptr;
    for (std::size_t index = 0; devices != nullptr && devices[index] != nullptr; ++index) {
        if (dtv_get_vid(devices[index]) == 0xffff && dtv_get_pid(devices[index]) == 0xffff)
            unknown = devices[index];
    }
    const std::array<unsigned char, 3> package{1, 2, 3};
    const auto identify_before = scan_firmware_identify_calls();
    dtv_error send_error{};
    send_error.struct_size = sizeof(send_error);
    const auto result = unknown == nullptr ? DTV_ERROR_INTERNAL
        : dtv_send_firmware(unknown, package.data(), package.size(), nullptr, &send_error);
    const bool no_io = backend->submit_calls == 0 && backend->open_count == 0 &&
        backend->claim_count == 0;
    const bool remains_unknown = unknown != nullptr && dtv_get_mode(unknown) == DTV_DEVICE_UNKNOWN;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    dtv::internal::set_public_discovery_batch_for_test({});
    return result == DTV_ERROR_NOT_READY && send_error.code == DTV_ERROR_NOT_READY &&
        no_io && remains_unknown && scan_firmware_identify_calls() == identify_before ? 0 : 4;
}

int public_firmware_same_id_mode()
{
    reset_scan_firmware_records();
    set_scan_firmware_same_id_mode(DTV_IDENTIFY_LOADER);
    set_scan_public_open_case(9);
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 5, 431), snapshot(context, 4, 432)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1 || batch.snapshots.empty()) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr || error.code != DTV_ERROR_OK) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    auto *const *devices = dtv_get_devices(root);
    if (devices == nullptr || devices[0] == nullptr ||
        dtv_get_mode(devices[0]) != DTV_DEVICE_LOADER) return 3;
    auto *const target = devices[0];
    const auto target_id = dtv_get_device_id(target);
    const std::array<unsigned char, 5> package{0x10, 0x20, 0x30, 0x40, 0x50};
    std::atomic<std::int32_t> send_result{DTV_ERROR_INTERNAL};
    dtv_error send_error{};
    send_error.struct_size = sizeof(send_error);
    std::thread sender([&] {
        send_result.store(dtv_send_firmware(target, package.data(), package.size(), nullptr,
                                            &send_error), std::memory_order_release);
    });
    const bool sent = backend->wait_for_endpoint_submissions(0x01, 1);
    if (sent) backend->complete_endpoint(0x01);
    const bool awaiting_ack = backend->wait_for_endpoint_submissions(0x83, 1);
    if (awaiting_ack) backend->complete_endpoint(0x83);
    sender.join();
    if (!sent || !awaiting_ack || send_result.load(std::memory_order_acquire) != DTV_ERROR_OK ||
        send_error.code != DTV_ERROR_OK || dtv_get_mode(target) != DTV_DEVICE_RUNTIME) return 4;
    const auto output_count = backend->output_snapshot().size();
    dtv_error list_error{};
    list_error.struct_size = sizeof(list_error);
    auto *const *next = dtv_list_enclosures(&list_error);
    auto *const *devices_after = dtv_get_devices(root);
    if (next == nullptr || next[0] != root || devices_after == nullptr ||
        devices_after[0] != target || dtv_get_device_id(target) != target_id ||
        dtv_get_mode(target) != DTV_DEVICE_RUNTIME || list_error.code != DTV_ERROR_OK) return 5;
    const auto before_noop = backend->output_snapshot().size();
    dtv_error noop_error{};
    noop_error.struct_size = sizeof(noop_error);
    const auto noop = dtv_send_firmware(target, package.data(), package.size(), nullptr,
                                        &noop_error);
    const bool no_firmware_out = before_noop == output_count &&
        backend->output_snapshot().size() == before_noop;
    auto *const *receivers = dtv_get_receivers(target);
    std::uint32_t support = DTV_UNKNOWN;
    const bool support_ready = receivers != nullptr && receivers[0] != nullptr &&
        dtv_get_receiver_support(receivers[0], DTV_OP_SIGNAL, &support, &error) == DTV_ERROR_OK &&
        support == DTV_YES;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    dtv::internal::set_public_discovery_batch_for_test({});
    set_scan_public_open_case(0);
    return noop == DTV_ERROR_OK && noop_error.code == DTV_ERROR_OK && no_firmware_out &&
        support_ready ? 0 : 6;
}

int public_reset_card()
{
    set_scan_public_open_case(17);
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 193), snapshot(context, 4, 194)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::uint32_t support = DTV_UNKNOWN;
    if (dtv_get_enclosure_support(root, DTV_OP_CARD, &support, &error) != DTV_ERROR_OK ||
        support != DTV_YES || dtv_get_card_generation(root) != 0) return 3;
    std::atomic<std::int32_t> open_result{DTV_ERROR_INTERNAL};
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] {
        open_result.store(dtv_open_enclosure(root, nullptr, &open_error),
                          std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x81, 1) ||
        !backend->wait_for_endpoint_submissions(0x82, 1)) {
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 4;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result.load(std::memory_order_acquire) != DTV_ERROR_OK ||
        dtv_get_card_generation(root) != 0) return 5;
    const std::uint8_t *unread_atr = reinterpret_cast<const std::uint8_t *>(std::uintptr_t{1});
    std::uint64_t unread_size = 1;
    dtv_error unread_error{};
    unread_error.struct_size = sizeof(unread_error);
    const auto unread_submissions = backend->endpoint_submission_count(0x02);
    if (dtv_get_card_atr(root, nullptr, &unread_atr, &unread_size, &unread_error) !=
            DTV_ERROR_NOT_READY || unread_error.code != DTV_ERROR_NOT_READY ||
        unread_atr != nullptr || unread_size != 0 ||
        backend->endpoint_submission_count(0x02) != unread_submissions) return 6;

    const auto reset = [&](unsigned mode, std::int32_t expected) {
        set_scan_card_reply_mode(mode);
        const auto output_submission = backend->endpoint_submission_count(0x02) + 1;
        std::atomic<std::int32_t> result{DTV_ERROR_INTERNAL};
        std::atomic<const std::uint8_t *> atr{nullptr};
        std::atomic<std::uint64_t> size{0};
        dtv_error reset_error{};
        reset_error.struct_size = sizeof(reset_error);
        std::thread caller([&] {
            const std::uint8_t *data = nullptr;
            std::uint64_t length = 0;
            const dtv_call_options call{sizeof(dtv_call_options), mode == 4 ? 25u : 0u};
            result.store(dtv_reset_card(root, &call, &data, &length, &reset_error),
                         std::memory_order_release);
            atr.store(data, std::memory_order_release);
            size.store(length, std::memory_order_release);
        });
        if (!backend->wait_for_endpoint_submissions(0x02, output_submission)) {
            dtv_cancel_enclosure(root, &error);
            caller.join();
            return false;
        }
        backend->complete_endpoint(0x02);
        const auto callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        if (!backend->wait_for_callbacks(callbacks + 1)) {
            dtv_cancel_enclosure(root, &error);
            caller.join();
            return false;
        }
        caller.join();
        if (result.load(std::memory_order_acquire) != expected || reset_error.code != expected) {
            return false;
        }
        if (expected != DTV_ERROR_OK) {
            const std::uint8_t *unavailable_atr = reinterpret_cast<const std::uint8_t *>(
                std::uintptr_t{1});
            std::uint64_t unavailable_size = 1;
            dtv_error getter_error{};
            getter_error.struct_size = sizeof(getter_error);
            return atr.load(std::memory_order_acquire) == nullptr &&
                size.load(std::memory_order_acquire) == 0 &&
                dtv_get_card_state(root) == DTV_CARD_UNKNOWN &&
                dtv_get_card_atr(root, nullptr, &unavailable_atr, &unavailable_size,
                    &getter_error) == DTV_ERROR_NOT_READY &&
                getter_error.code == DTV_ERROR_NOT_READY && unavailable_atr == nullptr &&
                unavailable_size == 0;
        }
        const auto *data = atr.load(std::memory_order_acquire);
        if (data == nullptr || size.load(std::memory_order_acquire) != 4 ||
            data[0] != 0x3b || data[1] != 0x80 || data[2] != 0x01 || data[3] != 0x81 ||
            dtv_get_card_state(root) != DTV_CARD_READY || dtv_get_card_generation(root) != 0)
            return false;
        const auto getter_submissions = backend->endpoint_submission_count(0x02);
        const std::uint8_t *got_atr = nullptr;
        std::uint64_t got_size = 0;
        dtv_error getter_error{};
        getter_error.struct_size = sizeof(getter_error);
        if (dtv_get_card_atr(root, nullptr, &got_atr, &got_size, &getter_error) != DTV_ERROR_OK ||
            getter_error.code != DTV_ERROR_OK || got_atr != data || got_size != 4 ||
            got_atr[0] != data[0] || got_atr[1] != data[1] || got_atr[2] != data[2] ||
            got_atr[3] != data[3] ||
            backend->endpoint_submission_count(0x02) != getter_submissions) return false;
        auto *const *devices = dtv_get_devices(root);
        auto *const *receivers = devices == nullptr ? nullptr : dtv_get_receivers(devices[0]);
        dtv_signal signal{};
        const auto receiver_result = receivers == nullptr || receivers[0] == nullptr
            ? DTV_ERROR_INTERNAL : dtv_get_signal(receivers[0], nullptr, &signal, &error);
        if (receiver_result != DTV_ERROR_OK ||
            data[0] != 0x3b || data[1] != 0x80 || data[2] != 0x01 || data[3] != 0x81)
            return false;
        return true;
    };
    if (!reset(0, DTV_ERROR_OK)) return 6;
    if (!reset(1, DTV_ERROR_NO_CARD) || !reset(2, DTV_ERROR_CARD_IO) ||
        !reset(3, DTV_ERROR_CARD_IO) || !reset(4, DTV_ERROR_TIMEOUT)) return 7;
    auto *const *devices = dtv_get_devices(root);
    auto *const *receivers = devices == nullptr ? nullptr : dtv_get_receivers(devices[0]);
    dtv_stream *stream = nullptr;
    if (receivers == nullptr || receivers[1] == nullptr ||
        dtv_open_stream(receivers[1], nullptr, &stream, &error) != DTV_ERROR_OK || stream == nullptr)
        return 14;
    const dtv_call_options event_wait{sizeof(dtv_call_options), 2};
    bool event_queue_empty = false;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        dtv_event event{};
        event.struct_size = sizeof(event);
        const auto event_result = dtv_wait_event(&event, &event_wait, &error);
        if (event_result == DTV_ERROR_TIMEOUT) {
            event_queue_empty = true;
            break;
        }
        if (event_result != DTV_ERROR_OK || event.kind == DTV_EVENT_CARD_INSERTED ||
            event.kind == DTV_EVENT_CARD_REMOVED) {
            dtv_close_stream(stream, nullptr, &error);
            dtv_release_stream(stream);
            return 15;
        }
    }
    if (!event_queue_empty) {
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 16;
    }
    set_scan_card_interleave_phase(1);
    const auto interleave_output_submission = backend->endpoint_submission_count(0x02) + 1;
    std::atomic<bool> interleave_reset_done{false};
    std::atomic<std::int32_t> interleave_reset_result{DTV_ERROR_INTERNAL};
    std::atomic<const std::uint8_t *> interleave_atr{nullptr};
    std::atomic<std::uint64_t> interleave_atr_size{0};
    dtv_error interleave_error{};
    interleave_error.struct_size = sizeof(interleave_error);
    std::thread interleave_reset([&] {
        const std::uint8_t *data = nullptr;
        std::uint64_t size = 0;
        interleave_reset_result.store(
            dtv_reset_card(root, nullptr, &data, &size, &interleave_error),
            std::memory_order_release);
        interleave_atr.store(data, std::memory_order_release);
        interleave_atr_size.store(size, std::memory_order_release);
        interleave_reset_done.store(true, std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, interleave_output_submission)) {
        dtv_cancel_enclosure(root, &error);
        interleave_reset.join();
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 17;
    }
    backend->complete_endpoint(0x02);
    auto callbacks = backend->callback_count_snapshot();
    auto submissions = backend->endpoint_submission_count(0x81);
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1) ||
        !backend->wait_for_endpoint_submissions(0x81, submissions + 1)) {
        dtv_cancel_enclosure(root, &error);
        interleave_reset.join();
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 18;
    }
    const auto notification_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (dtv_get_card_state(root) != DTV_CARD_PRESENT &&
           std::chrono::steady_clock::now() < notification_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (dtv_get_card_state(root) != DTV_CARD_PRESENT || dtv_get_card_generation(root) != 0 ||
        interleave_reset_done.load(std::memory_order_acquire)) {
        dtv_cancel_enclosure(root, &error);
        interleave_reset.join();
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 19;
    }
    event_queue_empty = false;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        dtv_event event{};
        event.struct_size = sizeof(event);
        const auto event_result = dtv_wait_event(&event, &event_wait, &error);
        if (event_result == DTV_ERROR_TIMEOUT) {
            event_queue_empty = true;
            break;
        }
        if (event_result != DTV_ERROR_OK || event.kind == DTV_EVENT_CARD_INSERTED ||
            event.kind == DTV_EVENT_CARD_REMOVED) {
            dtv_cancel_enclosure(root, &error);
            interleave_reset.join();
            dtv_close_stream(stream, nullptr, &error);
            dtv_release_stream(stream);
            return 20;
        }
    }
    if (!event_queue_empty) {
        dtv_cancel_enclosure(root, &error);
        interleave_reset.join();
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 21;
    }
    std::array<std::uint8_t, 188> interleave_packet{};
    std::uint64_t interleave_read_size = 0;
    std::atomic<bool> interleave_read_done{false};
    std::atomic<std::int32_t> interleave_read_result{DTV_ERROR_INTERNAL};
    dtv_error interleave_read_error{};
    interleave_read_error.struct_size = sizeof(interleave_read_error);
    std::thread interleave_read([&] {
        const dtv_call_options read_call{sizeof(dtv_call_options), 5000};
        interleave_read_result.store(dtv_read_stream(stream, interleave_packet.data(),
            interleave_packet.size(), &read_call, &interleave_read_size, &interleave_read_error),
            std::memory_order_release);
        interleave_read_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (interleave_read_done.load(std::memory_order_acquire) ||
        interleave_reset_done.load(std::memory_order_acquire)) {
        dtv_cancel_stream(stream, &error);
        interleave_read.join();
        dtv_cancel_enclosure(root, &error);
        interleave_reset.join();
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 22;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x82);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_stream(stream, &error);
        interleave_read.join();
        dtv_cancel_enclosure(root, &error);
        interleave_reset.join();
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 23;
    }
    interleave_read.join();
    if (interleave_read_result.load(std::memory_order_acquire) != DTV_ERROR_OK ||
        interleave_read_size != interleave_packet.size() || interleave_packet[0] != 0x47 ||
        interleave_packet[1] != 0x81 || interleave_reset_done.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "reset interleave read=%d size=%llu packet=%02x/%02x reset_done=%d card=%u phase=%u\n",
            interleave_read_result.load(std::memory_order_acquire),
            static_cast<unsigned long long>(interleave_read_size), interleave_packet[0],
            interleave_packet[1], interleave_reset_done.load(std::memory_order_acquire),
            dtv_get_card_state(root), scan_card_interleave_phase_value());
        dtv_cancel_enclosure(root, &error);
        interleave_reset.join();
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 24;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_enclosure(root, &error);
        interleave_reset.join();
        dtv_close_stream(stream, nullptr, &error);
        dtv_release_stream(stream);
        return 25;
    }
    interleave_reset.join();
    set_scan_card_interleave_phase(0);
    const auto *const interleave_result_atr = interleave_atr.load(std::memory_order_acquire);
    if (interleave_reset_result.load(std::memory_order_acquire) != DTV_ERROR_OK ||
        interleave_error.code != DTV_ERROR_OK || interleave_result_atr == nullptr ||
        interleave_atr_size.load(std::memory_order_acquire) != 4 ||
        interleave_result_atr[0] != 0x3b || interleave_result_atr[1] != 0x80 ||
        interleave_result_atr[2] != 0x01 || interleave_result_atr[3] != 0x81 ||
        dtv_get_card_state(root) != DTV_CARD_READY || dtv_get_card_generation(root) != 0 ||
        dtv_close_stream(stream, nullptr, &error) != DTV_CLOSE_CLOSED) {
        dtv_release_stream(stream);
        return 26;
    }
    dtv_release_stream(stream);
    const auto output_submission = backend->endpoint_submission_count(0x02) + 1;
    std::atomic<std::int32_t> cancel_result{DTV_ERROR_INTERNAL};
    dtv_error cancel_error{};
    cancel_error.struct_size = sizeof(cancel_error);
    std::thread cancelled_reset([&] {
        const std::uint8_t *data = nullptr;
        std::uint64_t size = 0;
        cancel_result.store(dtv_reset_card(root, nullptr, &data, &size, &cancel_error),
                            std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, output_submission)) {
        dtv_cancel_enclosure(root, &error);
        cancelled_reset.join();
        return 8;
    }
    backend->complete_endpoint(0x02);
    if (dtv_cancel_enclosure(root, &error) != DTV_ERROR_OK) {
        cancelled_reset.join();
        return 9;
    }
    cancelled_reset.join();
    if (cancel_result.load(std::memory_order_acquire) != DTV_ERROR_CANCELLED ||
        cancel_error.code != DTV_ERROR_CANCELLED || dtv_get_card_state(root) != DTV_CARD_UNKNOWN)
        return 10;
    backend->set_defer_cancel_completion(true);
    const auto pending_output_submission = backend->endpoint_submission_count(0x02) + 1;
    std::atomic<std::int32_t> pending_result{DTV_ERROR_INTERNAL};
    dtv_error pending_error{};
    pending_error.struct_size = sizeof(pending_error);
    std::thread pending_reset([&] {
        const std::uint8_t *data = nullptr;
        std::uint64_t size = 0;
        pending_result.store(dtv_reset_card(root, nullptr, &data, &size, &pending_error),
                             std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, pending_output_submission)) {
        backend->set_defer_cancel_completion(false);
        dtv_cancel_enclosure(root, &error);
        pending_reset.join();
        return 11;
    }
    backend->complete_endpoint(0x02);
    const dtv_call_options close_call{sizeof(dtv_call_options), 1};
    if (dtv_close_enclosure(root, &close_call, &error) != DTV_CLOSE_PENDING) {
        backend->set_defer_cancel_completion(false);
        pending_reset.join();
        return 12;
    }
    const auto close_callbacks = backend->callback_count_snapshot();
    const auto pending_transfers = backend->pending_transfer_count();
    backend->set_defer_cancel_completion(false);
    backend->complete_endpoint(0x81, 2);
    backend->complete_endpoint(0x82, 2);
    if (!backend->wait_for_callbacks(close_callbacks + static_cast<unsigned>(pending_transfers)))
        return 13;
    pending_reset.join();
    if (pending_result.load(std::memory_order_acquire) != DTV_ERROR_CANCELLED ||
        pending_error.code != DTV_ERROR_CANCELLED) {
        return 13;
    }
    auto close_state = dtv_get_enclosure_close_state(root, &error);
    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (close_state == DTV_CLOSE_PENDING && std::chrono::steady_clock::now() < close_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        close_state = dtv_get_enclosure_close_state(root, &error);
    }
    if (close_state != DTV_CLOSE_CLOSED) {
        std::fprintf(stderr, "pending reset=%d error=%d close=%u\n",
            pending_result.load(std::memory_order_acquire), pending_error.code, close_state);
        return 13;
    }
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    dtv::internal::set_public_discovery_batch_for_test({});

    auto unsupported_backend = std::make_shared<FakeBackend>();
    unsupported_backend->async_enabled = true;
    auto unsupported_context = dtv::internal::UsbContext::scan_only(unsupported_backend);
    dtv::internal::CompleteGroupBatch unsupported_batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(unsupported_context, 3, 195), snapshot(unsupported_context, 4, 196)},
            &unsupported_batch).code != DTV_INTERNAL_RESULT_OK ||
        unsupported_batch.groups.size() != 1) return 11;
    unsupported_batch.groups[0].model = &dtv_l3_model_scanfixture_scan_model_alias;
    dtv::internal::set_public_discovery_context_for_test(
        std::move(unsupported_context), std::move(unsupported_batch));
    auto *const *unsupported_enclosures = dtv_list_enclosures(&error);
    if (unsupported_enclosures == nullptr || unsupported_enclosures[0] == nullptr) return 12;
    const auto output_count = unsupported_backend->output_snapshot().size();
    root = unsupported_enclosures[0];
    const std::uint8_t *unsupported_atr = nullptr;
    std::uint64_t unsupported_size = 0;
    const std::uint8_t *unsupported_getter_atr = reinterpret_cast<const std::uint8_t *>(
        std::uintptr_t{1});
    std::uint64_t unsupported_getter_size = 1;
    const std::array<std::uint8_t, 4> unsupported_command{0x00, 0xa4, 0x04, 0x00};
    const std::uint8_t *unsupported_response = reinterpret_cast<const std::uint8_t *>(
        std::uintptr_t{1});
    std::uint64_t unsupported_response_size = 1;
    if (dtv_reset_card(root, nullptr, &unsupported_atr, &unsupported_size, &error) !=
            DTV_ERROR_UNSUPPORTED || error.code != DTV_ERROR_UNSUPPORTED ||
        unsupported_atr != nullptr || unsupported_size != 0 ||
        dtv_get_card_atr(root, nullptr, &unsupported_getter_atr, &unsupported_getter_size,
            &error) != DTV_ERROR_UNSUPPORTED || error.code != DTV_ERROR_UNSUPPORTED ||
        unsupported_getter_atr != nullptr || unsupported_getter_size != 0 ||
        dtv_transmit_apdu(root, unsupported_command.data(), unsupported_command.size(),
            nullptr, &unsupported_response, &unsupported_response_size, &error) !=
            DTV_ERROR_UNSUPPORTED || error.code != DTV_ERROR_UNSUPPORTED ||
        unsupported_response != nullptr || unsupported_response_size != 0 ||
        unsupported_backend->output_snapshot().size() != output_count) return 13;
    dtv::internal::set_public_discovery_batch_for_test({});
    set_scan_public_open_case(0);
    return 0;
}

int public_card_transport()
{
    set_scan_public_open_case(18);
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 219), snapshot(context, 4, 220)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::atomic<std::int32_t> open_result{DTV_ERROR_INTERNAL};
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] {
        open_result.store(dtv_open_enclosure(root, nullptr, &open_error),
                          std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x81, 1) ||
        !backend->wait_for_endpoint_submissions(0x82, 1)) {
        std::fprintf(stderr, "card transport open endpoints: %u/%u result=%d\n",
            backend->endpoint_submission_count(0x81), backend->endpoint_submission_count(0x82),
            open_result.load(std::memory_order_acquire));
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 3;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result.load(std::memory_order_acquire) != DTV_ERROR_OK) return 4;

    const std::array<std::uint8_t, 4> not_ready_command{0x00, 0xa4, 0x04, 0x00};
    const auto not_ready_output_count = backend->output_snapshot().size();
    const std::uint8_t *not_ready_response = reinterpret_cast<const std::uint8_t *>(
        std::uintptr_t{1});
    std::uint64_t not_ready_response_size = 1;
    if (dtv_transmit_apdu(root, not_ready_command.data(), not_ready_command.size(), nullptr,
            &not_ready_response, &not_ready_response_size, &error) != DTV_ERROR_NOT_READY ||
        error.code != DTV_ERROR_NOT_READY || not_ready_response != nullptr ||
        not_ready_response_size != 0 ||
        backend->output_snapshot().size() != not_ready_output_count)
        return 269;

    const auto reset_output = backend->endpoint_submission_count(0x02) + 1;
    std::atomic<std::int32_t> reset_result{DTV_ERROR_INTERNAL};
    std::thread resetting([&] {
        const std::uint8_t *atr = nullptr;
        std::uint64_t atr_size = 0;
        dtv_error reset_error{};
        reset_error.struct_size = sizeof(reset_error);
        reset_result.store(dtv_reset_card(root, nullptr, &atr, &atr_size, &reset_error),
                           std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, reset_output)) {
        dtv_cancel_enclosure(root, &error);
        resetting.join();
        return 5;
    }
    auto callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x02);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_enclosure(root, &error);
        resetting.join();
        return 6;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_enclosure(root, &error);
        resetting.join();
        return 7;
    }
    resetting.join();
    if (reset_result.load(std::memory_order_acquire) != DTV_ERROR_OK ||
        dtv_get_card_state(root) != DTV_CARD_READY) return 8;

    const auto exchange = [&](const dtv::spi::CardTransportOperation &operation,
                              std::vector<unsigned char> *reply,
                              dtv::spi::CardIoOutcome *outcome, unsigned expected_transaction,
                              dtv_timeout timeout) {
        dtv_internal_result result{};
        std::thread caller([&] {
            result = dtv::internal::run_public_card_transport(root, operation, timeout,
                reply, outcome);
        });
        const auto output_submission = backend->endpoint_submission_count(0x02) + 1;
        if (!backend->wait_for_endpoint_submissions(0x02, output_submission)) {
            dtv_cancel_enclosure(root, &error);
            caller.join();
            return false;
        }
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(callbacks + 1)) {
            dtv_cancel_enclosure(root, &error);
            caller.join();
            return false;
        }
        if (timeout.timeout_ms == 0 || expected_transaction != 0) {
            callbacks = backend->callback_count_snapshot();
            backend->complete_endpoint(0x81);
            if (expected_transaction != 0 && !backend->wait_for_callbacks(callbacks + 1)) {
                caller.join();
                return false;
            }
        }
        caller.join();
        if (expected_transaction != 0 && result.code != DTV_INTERNAL_RESULT_OK)
            std::fprintf(stderr, "card transport result=%d detail=%u expected=%u tx=%zu rx=%zu packets=%u\n",
                result.code, result.detail, expected_transaction, outcome->tx_bytes_done,
                outcome->rx_bytes_done, scan_shared_packet_count());
        return true;
    };

    const std::array<unsigned char, 3> byte_chunk{0x3f, 0x01, 0x7a};
    std::vector<unsigned char> reply;
    dtv::spi::CardIoOutcome outcome{};
    const auto before_write = backend->output_snapshot().size();
    if (!exchange(dtv::spi::CardByteWrite{byte_chunk}, &reply, &outcome, 201,
            dtv_timeout{500, 0}) || reply != std::vector<unsigned char>{0xac} ||
        outcome.tx_bytes_done != byte_chunk.size() || outcome.rx_bytes_done != 1 ||
        !outcome.reply_complete || !outcome.result_known ||
        backend->output_snapshot().size() != before_write + 1 ||
        backend->output_snapshot().back() != std::vector<unsigned char>(byte_chunk.begin(), byte_chunk.end()))
        return 9;

    reply.clear();
    if (!exchange(dtv::spi::CardByteRead{8}, &reply, &outcome, 202, dtv_timeout{500, 0}) ||
        reply != std::vector<unsigned char>{0x5a, 0xc3} || outcome.tx_bytes_done != 2 ||
        outcome.rx_bytes_done != 2 || !outcome.reply_complete || !outcome.result_known ||
        backend->output_snapshot().back() != std::vector<unsigned char>{0x52, 0x01}) return 10;

    const std::array<unsigned char, 5> block{0x00, 0x00, 0x01, 0x00, 0x00};
    reply.clear();
    if (!exchange(dtv::spi::CardT1BlockExchange{block, 16}, &reply, &outcome, 203,
            dtv_timeout{500, 0}) ||
        reply != std::vector<unsigned char>{0x00, 0x00, 0x01, 0x90, 0x00} ||
        outcome.tx_bytes_done != block.size() || outcome.rx_bytes_done != 5 ||
        !outcome.reply_complete || !outcome.result_known ||
        backend->output_snapshot().back() != std::vector<unsigned char>(block.begin(), block.end()))
        return 11;

    struct SyntheticBgtClock {
        std::atomic<std::uint64_t> now_ns{0};
        std::atomic<std::uint64_t> revision_update_at_ns{0};
        std::atomic<std::uint64_t> revision_update_bound_ns{5000000};
        std::atomic<std::uint64_t> revision_update_revision{12};
        std::atomic<std::uint64_t> plan_override_bound_ns{10000000};
        std::atomic<std::uint64_t> plan_override_revision{13};
        std::atomic<std::uint64_t> revision_shrink_at_ns{0};
        std::atomic<std::uint64_t> revision_shrink_bound_ns{1000000};
        std::atomic<std::uint64_t> revision_shrink_revision{14};
        std::atomic<dtv_enclosure *> cancel_root{nullptr};
        std::atomic<bool> cancel_on_delay{false};
        std::atomic<std::uint64_t> cancel_delay_calls{0};
        std::atomic<std::uint64_t> cancel_delay_calls_at_arm{0};
        std::atomic<std::uint64_t> cancel_arm_monotonic_ns{0};
        std::atomic<std::uint64_t> cancel_arm_synthetic_ns{0};
        std::atomic<std::uint64_t> cancel_trigger_monotonic_ns{0};
        std::atomic<std::uint64_t> cancel_trigger_synthetic_ns{0};
        std::atomic<std::uint64_t> cancel_trigger_attempts{0};
        std::atomic<std::int32_t> cancel_trigger_result{DTV_ERROR_INTERNAL};
    } synthetic_bgt_clock;
    synthetic_bgt_clock.now_ns.store(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()),
        std::memory_order_release);
    const auto synthetic_now = [](void *user) -> std::uint64_t {
        auto *clock = static_cast<SyntheticBgtClock *>(user);
        return clock->now_ns.load(std::memory_order_acquire);
    };
    const auto synthetic_delay = [](void *user, std::uint64_t wake_at, std::uint64_t deadline,
                                    const dtv_cancel *, std::uint32_t *cancelled) {
        auto *clock = static_cast<SyntheticBgtClock *>(user);
        clock->cancel_delay_calls.fetch_add(1, std::memory_order_acq_rel);
        if (clock->cancel_on_delay && clock->cancel_root != nullptr) {
            clock->cancel_on_delay = false;
            clock->cancel_trigger_attempts.fetch_add(1, std::memory_order_acq_rel);
            clock->cancel_trigger_monotonic_ns.store(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()),
                std::memory_order_release);
            clock->cancel_trigger_synthetic_ns.store(
                clock->now_ns.load(std::memory_order_acquire), std::memory_order_release);
            dtv_error error{};
            error.struct_size = sizeof(error);
            clock->cancel_trigger_result.store(dtv_cancel_enclosure(clock->cancel_root, &error),
                std::memory_order_release);
        }
        const auto now = clock->now_ns.load(std::memory_order_acquire);
        if (deadline != UINT64_MAX && wake_at > deadline)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
        const auto update_at = clock->revision_update_at_ns.load(std::memory_order_acquire);
        if (update_at != 0 && now < update_at && update_at <= wake_at) {
            clock->now_ns.store(update_at, std::memory_order_release);
            clock->revision_update_at_ns.store(0, std::memory_order_release);
            set_scan_card_t1_bgt(2,
                clock->revision_update_bound_ns.load(std::memory_order_acquire),
                clock->revision_update_revision.load(std::memory_order_acquire));
            set_scan_card_t1_bgt_send_plan_override(
                clock->plan_override_bound_ns.load(std::memory_order_acquire),
                clock->plan_override_revision.load(std::memory_order_acquire));
            if (cancelled != nullptr) *cancelled = 0;
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        const auto shrink_at = clock->revision_shrink_at_ns.load(std::memory_order_acquire);
        if (shrink_at != 0 && now < shrink_at && shrink_at <= wake_at) {
            clock->now_ns.store(shrink_at, std::memory_order_release);
            clock->revision_shrink_at_ns.store(0, std::memory_order_release);
            set_scan_card_t1_bgt(2,
                clock->revision_shrink_bound_ns.load(std::memory_order_acquire),
                clock->revision_shrink_revision.load(std::memory_order_acquire));
            if (cancelled != nullptr) *cancelled = 0;
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        if (wake_at > now) clock->now_ns.store(wake_at, std::memory_order_release);
        if (cancelled != nullptr) *cancelled = 0;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    };
    const dtv_runtime_clock synthetic_clock{&synthetic_bgt_clock, synthetic_now,
        synthetic_delay, nullptr};
    dtv::internal::set_public_card_bgt_clock_for_test(&synthetic_clock);
    struct SyntheticBgtClockReset {
        ~SyntheticBgtClockReset() { dtv::internal::set_public_card_bgt_clock_for_test(nullptr); }
    } synthetic_bgt_clock_reset;

    const std::array<unsigned char, 3> t1_inf{0x10, 0x20, 0x30};
    std::array<dtv::internal::T1BlockResult, 2> t1_results{};
    const auto run_t1_exchange = [&](unsigned mode, dtv::internal::T1BlockResult *block_result,
                                     std::uint32_t timeout_ms = 1000,
                                     std::vector<std::vector<unsigned char>> *sent = nullptr,
                                     std::uint64_t *caller_started_ns = nullptr,
                                     std::uint64_t *caller_finished_ns = nullptr,
                                     std::vector<std::uint64_t> *output_submit_ns = nullptr) {
        set_scan_card_t1_exchange_mode(mode);
        const auto output_start = backend->output_snapshot().size();
        std::atomic<bool> done{false};
        dtv_internal_result exchange_result{};
        std::thread caller([&] {
            if (caller_started_ns != nullptr) *caller_started_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            exchange_result = dtv::internal::exchange_public_t1_block(root,
                dtv::internal::T1BlockRequest{0x00, 0x00, t1_inf}, dtv_timeout{timeout_ms, 0},
                block_result);
            if (caller_finished_ns != nullptr) *caller_finished_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            done.store(true, std::memory_order_release);
        });
        auto next_output = backend->endpoint_submission_count(0x02) + 1;
        for (unsigned step = 0; step < 16 && !done.load(std::memory_order_acquire); ++step) {
            if (!backend->wait_for_endpoint_submissions(0x02, next_output)) break;
            if (output_submit_ns != nullptr)
                output_submit_ns->push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()));
            callbacks = backend->callback_count_snapshot();
            backend->complete_endpoint(0x02);
            if (!backend->wait_for_callbacks(callbacks + 1)) break;
            callbacks = backend->callback_count_snapshot();
            backend->complete_endpoint(0x81);
            if (!backend->wait_for_callbacks(callbacks + 1)) break;
            ++next_output;
        }
        caller.join();
        if (sent != nullptr) {
            const auto output = backend->output_snapshot();
            sent->assign(output.begin() + static_cast<std::ptrdiff_t>(output_start), output.end());
        }
        if (exchange_result.code != DTV_INTERNAL_RESULT_OK)
            std::fprintf(stderr, "common T1 mode=%u result=%d tx=%zu rx=%zu known=%u complete=%u\n",
                mode, exchange_result.code, block_result->io.tx_bytes_done,
                block_result->io.rx_bytes_done, block_result->io.result_known,
                block_result->io.reply_complete);
        return exchange_result.code;
    };
    std::array<std::vector<std::vector<unsigned char>>, 2> t1_outputs{};
    if (run_t1_exchange(1, &t1_results[0], 1000, &t1_outputs[0]) != DTV_INTERNAL_RESULT_OK ||
        run_t1_exchange(2, &t1_results[1], 1000, &t1_outputs[1]) != DTV_INTERNAL_RESULT_OK ||
        t1_results[0].nad != 0 || t1_results[1].nad != 0 ||
        t1_results[0].pcb != 0x40 || t1_results[1].pcb != 0x40 ||
        t1_results[0].inf != std::vector<unsigned char>{0x90, 0x00} ||
        t1_results[1].inf != t1_results[0].inf ||
        t1_results[0].raw_block_length != 6 || t1_results[1].raw_block_length != 6 ||
        t1_results[0].io.tx_bytes_done != 7 || t1_results[1].io.tx_bytes_done != 7 ||
        t1_results[0].io.rx_bytes_done != 6 || t1_results[1].io.rx_bytes_done != 6 ||
        !t1_results[0].io.reply_complete || !t1_results[1].io.reply_complete ||
        !t1_results[0].io.result_known || !t1_results[1].io.result_known ||
        t1_results[0].io.transport_error.code != DTV_INTERNAL_RESULT_OK ||
        t1_results[1].io.transport_error.code != DTV_INTERNAL_RESULT_OK ||
        t1_outputs[0].size() < 3 ||
        t1_outputs[0][0] != std::vector<unsigned char>{0x00, 0x00, 0x03} ||
        t1_outputs[0][1] != std::vector<unsigned char>{0x10, 0x20, 0x30} ||
        t1_outputs[0][2] != std::vector<unsigned char>{0x03} || t1_outputs[1].empty() ||
        t1_outputs[1][0] != std::vector<unsigned char>{0x00, 0x00, 0x03, 0x10, 0x20, 0x30, 0x03}) {
        return 18;
    }
    const auto run_t1_apdu = [&](unsigned mode, std::span<const unsigned char> command,
                                 dtv::internal::T1ApduResult *apdu_result,
                                 std::vector<std::vector<unsigned char>> *sent = nullptr,
                                 std::uint32_t timeout_ms = 1000,
                                 std::vector<std::uint64_t> *submit_times = nullptr,
                                 std::vector<std::uint64_t> *input_complete_times = nullptr,
                                 std::size_t partial_output_index = 0,
                                 std::size_t cancel_after_input_count = 0) {
        set_scan_card_t1_exchange_mode(mode);
        const auto output_start = backend->output_snapshot().size();
        std::atomic<bool> done{false};
        dtv_internal_result exchange_result{};
        std::thread caller([&] {
            exchange_result = dtv::internal::exchange_public_t1_apdu(root, command,
                dtv_timeout{timeout_ms, 0}, apdu_result);
            done.store(true, std::memory_order_release);
        });
        auto next_output = backend->endpoint_submission_count(0x02) + 1;
        for (unsigned step = 0; step < 64 && !done.load(std::memory_order_acquire); ++step) {
            if (!backend->wait_for_endpoint_submissions(0x02, next_output)) break;
            if (submit_times != nullptr)
                submit_times->push_back(synthetic_now(&synthetic_bgt_clock));
            callbacks = backend->callback_count_snapshot();
            if (partial_output_index == step + 1) backend->actual_length_limit = 2;
            backend->complete_endpoint(0x02);
            if (!backend->wait_for_callbacks(callbacks + 1)) break;
            backend->actual_length_limit = (std::numeric_limits<std::size_t>::max)();
            callbacks = backend->callback_count_snapshot();
            if (cancel_after_input_count != 0 && step + 1 == cancel_after_input_count) {
                synthetic_bgt_clock.cancel_root = root;
                synthetic_bgt_clock.cancel_arm_monotonic_ns.store(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()),
                    std::memory_order_release);
                synthetic_bgt_clock.cancel_arm_synthetic_ns.store(
                    synthetic_now(&synthetic_bgt_clock), std::memory_order_release);
                synthetic_bgt_clock.cancel_delay_calls_at_arm.store(
                    synthetic_bgt_clock.cancel_delay_calls.load(std::memory_order_acquire),
                    std::memory_order_release);
                synthetic_bgt_clock.cancel_on_delay = true;
            }
            backend->complete_endpoint(0x81);
            if (!backend->wait_for_callbacks(callbacks + 1)) break;
            if (input_complete_times != nullptr)
                input_complete_times->push_back(synthetic_now(&synthetic_bgt_clock));
            ++next_output;
        }
        caller.join();
        if (sent != nullptr) {
            const auto output = backend->output_snapshot();
            sent->assign(output.begin() + static_cast<std::ptrdiff_t>(output_start), output.end());
        }
        return exchange_result.code;
    };
    const std::array<unsigned char, 4> apdu_one{0x00, 0xa4, 0x04, 0x00};
    const std::array<unsigned char, 4> apdu_two{0x00, 0xca, 0x00, 0x00};
    dtv::internal::T1ApduResult apdu_one_result{};
    dtv::internal::T1ApduResult apdu_two_result{};
    std::vector<std::vector<unsigned char>> apdu_one_output;
    std::vector<std::vector<unsigned char>> apdu_two_output;
    set_scan_card_t1_apdu_mode(true, 0);
    if (run_t1_apdu(1, apdu_one, &apdu_one_result, &apdu_one_output) !=
            DTV_INTERNAL_RESULT_OK) return 56;
    const auto bgt_receive_complete = synthetic_now(&synthetic_bgt_clock);
    set_scan_card_t1_bgt(2, 1000000, 10);
    const auto bgt_refreshes_before = scan_card_t1_bgt_refresh_count_value();
    synthetic_bgt_clock.revision_update_at_ns.store(bgt_receive_complete + 22000000 - 1,
        std::memory_order_release);
    synthetic_bgt_clock.revision_shrink_at_ns.store(bgt_receive_complete + 220000000 - 1,
        std::memory_order_release);
    if (run_t1_apdu(2, apdu_two, &apdu_two_result, &apdu_two_output) !=
            DTV_INTERNAL_RESULT_OK ||
        apdu_one_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        apdu_two_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
        apdu_one_output.size() < 3 ||
        apdu_one_output[0] != std::vector<unsigned char>{0x00, 0x00, 0x04} ||
        apdu_one_output[1] != std::vector<unsigned char>(apdu_one.begin(), apdu_one.end()) ||
        apdu_one_output[2] != std::vector<unsigned char>{0xa4} ||
        apdu_two_output.size() != 1 ||
        apdu_two_output[0] != std::vector<unsigned char>{0x00, 0x40, 0x04, 0x00,
            0xca, 0x00, 0x00, 0x8e}) {
        std::fprintf(stderr, "APDU sequence results=%d/%d response=%zu/%zu sent=%zu/%zu pcb=%02x tx=%zu rx=%zu\n",
            apdu_one_result.io.transport_error.code, apdu_two_result.io.transport_error.code,
            apdu_one_result.response.size(), apdu_two_result.response.size(),
            apdu_one_output.size(), apdu_two_output.size(),
            apdu_two_output.empty() || apdu_two_output[0].size() < 2 ? 0 : apdu_two_output[0][1],
            apdu_two_result.io.tx_bytes_done, apdu_two_result.io.rx_bytes_done);
        return 29;
    }
    if (synthetic_now(&synthetic_bgt_clock) < bgt_receive_complete + 220000000 ||
        scan_card_t1_bgt_refresh_count_value() < bgt_refreshes_before + 2) {
        std::fprintf(stderr, "BGT revision wait elapsed=%llu refresh=%u/%u\n",
            static_cast<unsigned long long>(synthetic_now(&synthetic_bgt_clock) - bgt_receive_complete),
            scan_card_t1_bgt_refresh_count_value(), bgt_refreshes_before);
        return 57;
    }
    set_scan_card_t1_bgt(2, 1000000, 15);
    synthetic_bgt_clock.revision_update_bound_ns.store(5000000, std::memory_order_release);
    synthetic_bgt_clock.revision_update_revision.store(16, std::memory_order_release);
    synthetic_bgt_clock.plan_override_bound_ns.store(10000000, std::memory_order_release);
    synthetic_bgt_clock.plan_override_revision.store(17, std::memory_order_release);
    synthetic_bgt_clock.revision_update_at_ns.store(
        synthetic_now(&synthetic_bgt_clock) + 22000000 - 1, std::memory_order_release);
    const auto outputs_before_deadline = backend->endpoint_submission_count(0x02);
    dtv::internal::T1ApduResult deadline_result{};
    const auto deadline_code = run_t1_apdu(2, apdu_one, &deadline_result, nullptr, 150);
    if (deadline_code != DTV_INTERNAL_RESULT_TIMEOUT ||
        backend->endpoint_submission_count(0x02) != outputs_before_deadline ||
        deadline_result.io.tx_bytes_done != 0 || !deadline_result.io.result_known) {
        std::fprintf(stderr, "BGT plan deadline code=%d out=%u/%u tx=%zu known=%u\n",
            deadline_code, backend->endpoint_submission_count(0x02), outputs_before_deadline,
            deadline_result.io.tx_bytes_done, deadline_result.io.result_known);
        return 62;
    }
    const auto check_bgt_rejected = [&](unsigned mode, std::uint64_t bound,
                                        std::uint64_t revision) {
        set_scan_card_t1_bgt(mode, bound, revision);
        const auto outputs_before = backend->endpoint_submission_count(0x02);
        dtv::internal::T1ApduResult rejected{};
        const auto code = run_t1_apdu(1, apdu_one, &rejected);
        return code == DTV_INTERNAL_RESULT_OK ? false
            : backend->endpoint_submission_count(0x02) == outputs_before &&
                rejected.io.tx_bytes_done == 0 && rejected.io.result_known;
    };
    const auto overflow_bound = (std::numeric_limits<std::uint64_t>::max)() / 22 + 1;
    const auto unknown_rejected = check_bgt_rejected(0, 0, 18);
    const auto zero_rejected = check_bgt_rejected(2, 0, 18);
    const auto overflow_rejected = check_bgt_rejected(2, overflow_bound, 18);
    const auto same_revision_mismatch = check_bgt_rejected(2, 11000000, 17);
    const auto revision_rollback = check_bgt_rejected(2, 1000000, 16);
    if (!unknown_rejected || !zero_rejected || !overflow_rejected ||
        !same_revision_mismatch || !revision_rollback) {
        std::fprintf(stderr, "BGT rejected=%u/%u/%u same=%u rollback=%u\n", unknown_rejected,
            zero_rejected, overflow_rejected, same_revision_mismatch, revision_rollback);
        return 58;
    }
    set_scan_card_t1_bgt(2, 1000000, 18);
    std::array<unsigned char, 0> empty_apdu{};
    const auto before_empty_apdu = backend->output_snapshot().size();
    dtv::internal::T1ApduResult oversized_result{};
    if (dtv::internal::exchange_public_t1_apdu(root, empty_apdu, dtv_timeout{1000, 0},
            &oversized_result).code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT ||
        backend->output_snapshot().size() != before_empty_apdu) return 30;
    set_scan_card_t1_apdu_mode(true, 1);
    dtv::internal::T1ApduResult wrong_sequence_result{};
    std::vector<std::vector<unsigned char>> wrong_sequence_output;
    const auto wrong_sequence_code = run_t1_apdu(1, apdu_one, &wrong_sequence_result,
        &wrong_sequence_output);
    if (wrong_sequence_code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
        !wrong_sequence_result.response.empty() || wrong_sequence_result.io.result_known ||
        wrong_sequence_result.io.reply_complete || wrong_sequence_output.size() < 3 ||
        wrong_sequence_output[0] != std::vector<unsigned char>{0x00, 0x00, 0x04}) {
        std::fprintf(stderr, "wrong sequence result=%d transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
            wrong_sequence_code, wrong_sequence_result.io.transport_error.code,
            wrong_sequence_result.response.size(), wrong_sequence_result.io.tx_bytes_done,
            wrong_sequence_result.io.rx_bytes_done, wrong_sequence_result.io.result_known,
            wrong_sequence_result.io.reply_complete, wrong_sequence_output.size());
        return 31;
    }
    const auto reset_t1_session = [&] {
        const auto output = backend->endpoint_submission_count(0x02) + 1;
        std::atomic<std::int32_t> value{DTV_ERROR_INTERNAL};
        std::thread resetting([&] {
            const std::uint8_t *atr = nullptr;
            std::uint64_t atr_size = 0;
            dtv_error reset_error{};
            reset_error.struct_size = sizeof(reset_error);
            value.store(dtv_reset_card(root, nullptr, &atr, &atr_size, &reset_error),
                        std::memory_order_release);
        });
        if (!backend->wait_for_endpoint_submissions(0x02, output)) {
            dtv_cancel_enclosure(root, &error);
            resetting.join();
            return false;
        }
        auto count = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(count + 1)) {
            dtv_cancel_enclosure(root, &error);
            resetting.join();
            return false;
        }
        count = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        if (!backend->wait_for_callbacks(count + 1)) {
            dtv_cancel_enclosure(root, &error);
            resetting.join();
            return false;
        }
        resetting.join();
        const auto reset_ok = value.load(std::memory_order_acquire) == DTV_ERROR_OK;
        if (reset_ok) reset_scan_card_t1_apdu_sequence();
        return reset_ok;
    };
    const auto same_apdu_frame_sent = [&](unsigned mode,
        const std::vector<std::vector<unsigned char>> &sent,
        const std::vector<unsigned char> &frame, std::span<const unsigned char> command,
        std::size_t expected_frames) {
        if (mode == 2) {
            return std::count(sent.begin(), sent.end(), frame) ==
                static_cast<std::ptrdiff_t>(expected_frames);
        }
        std::vector<std::size_t> headers;
        for (std::size_t index = 0; index < sent.size(); ++index) {
            if (sent[index] == std::vector<unsigned char>{0x00, frame[1], frame[2]})
                headers.push_back(index);
        }
        if (headers.size() != expected_frames) return false;
        for (const auto index : headers) {
            if (index + 2 >= sent.size() ||
                sent[index + 1] != std::vector<unsigned char>(command.begin(), command.end()) ||
                sent[index + 2] != std::vector<unsigned char>{frame.back()}) return false;
        }
        return true;
    };
    const auto same_r_frame_sent = [&](unsigned mode,
        const std::vector<std::vector<unsigned char>> &sent,
        const std::vector<unsigned char> &frame, std::size_t expected_frames) {
        if (mode == 2) {
            return std::count(sent.begin(), sent.end(), frame) ==
                static_cast<std::ptrdiff_t>(expected_frames);
        }
        std::vector<std::size_t> headers;
        for (std::size_t index = 0; index < sent.size(); ++index) {
            if (sent[index] == std::vector<unsigned char>{frame[0], frame[1], frame[2]})
                headers.push_back(index);
        }
        if (headers.size() != expected_frames) return false;
        return std::all_of(headers.begin(), headers.end(), [&](const auto index) {
            return index + 1 < sent.size() && sent[index + 1] ==
                std::vector<unsigned char>(frame.begin() + 3, frame.end());
        });
    };
    const std::vector<unsigned char> normal_r_one_frame{0x00, 0x90, 0x00, 0x90};
    const auto encode_t1_lrc_frame = [](unsigned char pcb,
                                        std::span<const unsigned char> inf) {
        std::vector<unsigned char> frame{0x00, pcb, static_cast<unsigned char>(inf.size())};
        frame.insert(frame.end(), inf.begin(), inf.end());
        unsigned char lrc = 0;
        for (const auto byte : frame) lrc ^= byte;
        frame.push_back(lrc);
        return frame;
    };
    const auto encode_t1_crc_frame = [](unsigned char pcb,
                                        std::span<const unsigned char> inf) {
        std::vector<unsigned char> frame{0x00, pcb, static_cast<unsigned char>(inf.size())};
        frame.insert(frame.end(), inf.begin(), inf.end());
        std::uint16_t crc = 0xffff;
        for (const auto byte : frame) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc & 1) != 0 ? static_cast<std::uint16_t>((crc >> 1) ^ 0x8408)
                                     : static_cast<std::uint16_t>(crc >> 1);
        }
        crc = static_cast<std::uint16_t>(~crc);
        frame.push_back(static_cast<unsigned char>(crc & 0xff));
        frame.push_back(static_cast<unsigned char>(crc >> 8));
        return frame;
    };
    const auto run_public_apdu = [&](unsigned mode,
                                     std::span<const unsigned char> command,
                                     std::vector<unsigned char> *response,
                                     std::vector<std::vector<unsigned char>> *sent,
                                     std::uint64_t timeout_ms = 0,
                                     const std::uint8_t **returned_data = nullptr,
                                     std::uint64_t *returned_size = nullptr) -> std::int32_t {
        set_scan_card_t1_exchange_mode(mode);
        const auto output_start = backend->output_snapshot().size();
        std::atomic<bool> done{false};
        std::atomic<std::int32_t> code{DTV_ERROR_INTERNAL};
        std::atomic<std::int32_t> error_code{DTV_ERROR_INTERNAL};
        std::vector<unsigned char> received;
        std::thread caller([&] {
            const std::uint8_t *data = nullptr;
            std::uint64_t size = 0;
            dtv_error public_apdu_error{};
            public_apdu_error.struct_size = sizeof(public_apdu_error);
            const dtv_call_options call_options{sizeof(dtv_call_options), timeout_ms};
            const auto result = dtv_transmit_apdu(root, command.data(), command.size(),
                timeout_ms == 0 ? nullptr : &call_options, &data, &size, &public_apdu_error);
            if (result == DTV_ERROR_OK && data != nullptr) {
                received.assign(data, data + size);
                if (returned_data != nullptr) *returned_data = data;
                if (returned_size != nullptr) *returned_size = size;
            }
            error_code.store(public_apdu_error.code, std::memory_order_release);
            code.store(result, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });
        auto next_output = backend->endpoint_submission_count(0x02) + 1;
        for (unsigned step = 0; step < 64 && !done.load(std::memory_order_acquire); ++step) {
            if (!backend->wait_for_endpoint_submissions(0x02, next_output)) break;
            callbacks = backend->callback_count_snapshot();
            backend->complete_endpoint(0x02);
            if (!backend->wait_for_callbacks(callbacks + 1)) break;
            callbacks = backend->callback_count_snapshot();
            backend->complete_endpoint(0x81);
            if (!backend->wait_for_callbacks(callbacks + 1)) break;
            ++next_output;
        }
        if (!done.load(std::memory_order_acquire)) dtv_cancel_enclosure(root, &error);
        caller.join();
        if (sent != nullptr) {
            const auto output = backend->output_snapshot();
            sent->assign(output.begin() + static_cast<std::ptrdiff_t>(output_start), output.end());
        }
        if (response != nullptr) *response = std::move(received);
        if (code.load(std::memory_order_acquire) !=
            error_code.load(std::memory_order_acquire)) return DTV_ERROR_INTERNAL;
        return code.load(std::memory_order_acquire);
    };
    set_scan_card_t1_crc(false);
    if (!reset_t1_session()) return 262;
    set_scan_card_t1_apdu_mode(true, 0);
    const auto invalid_output_count = backend->output_snapshot().size();
    const std::uint8_t *invalid_response = reinterpret_cast<const std::uint8_t *>(
        std::uintptr_t{1});
    std::uint64_t invalid_response_size = 1;
    if (dtv_transmit_apdu(root, apdu_one.data(), 0, nullptr, &invalid_response,
            &invalid_response_size, &error) != DTV_ERROR_INVALID_ARGUMENT ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || invalid_response != nullptr ||
        invalid_response_size != 0 ||
        backend->output_snapshot().size() != invalid_output_count)
        return 270;
    for (const auto mode : {1u, 2u}) {
        if (mode == 2) {
            set_scan_card_t1_crc(true);
            if (!reset_t1_session()) return 263;
            set_scan_card_t1_apdu_mode(true, 0);
        }
        const auto first_host_i = mode == 1 ? encode_t1_lrc_frame(0x00, apdu_one)
                                            : encode_t1_crc_frame(0x00, apdu_one);
        const auto second_host_i = mode == 1 ? encode_t1_lrc_frame(0x40, apdu_two)
                                             : encode_t1_crc_frame(0x40, apdu_two);
        std::vector<unsigned char> first_response;
        std::vector<unsigned char> second_response;
        std::vector<std::vector<unsigned char>> first_output;
        std::vector<std::vector<unsigned char>> second_output;
        const std::uint8_t *first_response_data = nullptr;
        std::uint64_t first_response_size = 0;
        if (run_public_apdu(mode, apdu_one, &first_response, &first_output, 0,
                &first_response_data, &first_response_size) != DTV_ERROR_OK ||
            first_response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            !same_apdu_frame_sent(mode, first_output, first_host_i, apdu_one, 1))
            return 264;
        auto *const *public_devices = dtv_get_devices(root);
        auto *const *public_receivers = public_devices == nullptr
            ? nullptr : dtv_get_receivers(public_devices[0]);
        dtv_signal public_signal{};
        if (public_receivers == nullptr || public_receivers[0] == nullptr ||
            dtv_get_signal(public_receivers[0], nullptr, &public_signal, &error) != DTV_ERROR_OK ||
            first_response_data == nullptr || first_response_size != first_response.size() ||
            !std::equal(first_response.begin(), first_response.end(), first_response_data))
            return 267;
        if (run_public_apdu(mode, apdu_two, &second_response, &second_output) != DTV_ERROR_OK ||
            second_response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
            !same_apdu_frame_sent(mode, second_output, second_host_i, apdu_two, 1)) {
            std::fprintf(stderr, "public APDU mode=%u first=%zu second=%zu\n",
                mode, first_response.size(), second_response.size());
            return 264;
        }
    }
    set_scan_card_t1_crc(false);
    set_scan_card_t1_bgt(2, 1000000, 200);
    set_scan_card_t1_bwt_mode(10);
    if (!reset_t1_session()) return 271;
    set_scan_card_t1_apdu_mode(true, 50);
    set_scan_card_t1_cwt_fault(100);
    const auto partial_cwt_host_i = encode_t1_lrc_frame(0x00, apdu_one);
    const auto partial_cwt_host_r = encode_t1_lrc_frame(0x82, {});
    const auto partial_cwt_next_host_i = encode_t1_lrc_frame(0x40, apdu_two);
    std::vector<unsigned char> partial_cwt_response;
    std::vector<std::vector<unsigned char>> partial_cwt_output;
    const std::uint8_t *partial_cwt_response_data = nullptr;
    std::uint64_t partial_cwt_response_size = 0;
    const auto partial_cwt_code = run_public_apdu(1, apdu_one,
        &partial_cwt_response, &partial_cwt_output, 0,
        &partial_cwt_response_data, &partial_cwt_response_size);
    if (partial_cwt_code != DTV_ERROR_OK ||
        partial_cwt_response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        partial_cwt_response_data == nullptr || partial_cwt_response_size != 4 ||
        !std::equal(partial_cwt_response.begin(), partial_cwt_response.end(),
            partial_cwt_response_data) ||
        !same_apdu_frame_sent(1, partial_cwt_output, partial_cwt_host_i, apdu_one, 1) ||
        !same_r_frame_sent(1, partial_cwt_output, partial_cwt_host_r, 1)) {
        std::fprintf(stderr, "partial CWT recovery code=%d response=%zu size=%llu output=%zu\n",
            partial_cwt_code,
            partial_cwt_response.size(),
            static_cast<unsigned long long>(partial_cwt_response_size),
            partial_cwt_output.size());
        for (const auto &packet : partial_cwt_output) {
            std::fprintf(stderr, "partial CWT packet:");
            for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        return 272;
    }
    set_scan_card_t1_cwt_fault(0);
    set_scan_card_t1_bgt(2, 1000000, 201);
    std::vector<unsigned char> partial_cwt_next_response;
    std::vector<std::vector<unsigned char>> partial_cwt_next_output;
    if (run_public_apdu(1, apdu_two, &partial_cwt_next_response,
            &partial_cwt_next_output) != DTV_ERROR_OK ||
        partial_cwt_next_response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
        !same_apdu_frame_sent(1, partial_cwt_next_output, partial_cwt_next_host_i,
            apdu_two, 1)) return 273;

    set_scan_card_t1_bgt(2, 36000000, 202);
    set_scan_card_t1_bwt_mode(9);
    if (!reset_t1_session()) return 281;
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    set_scan_card_t1_apdu_mode(true, 50);
    set_scan_card_t1_cwt_fault(101);
    const auto partial_cwt_cancel_start = backend->output_snapshot().size();
    const auto partial_cwt_cancel_output_start =
        backend->endpoint_submission_count(0x02) + 1;
    std::atomic<std::int32_t> partial_cwt_cancel_code{DTV_ERROR_INTERNAL};
    std::vector<unsigned char> partial_cwt_cancel_response;
    std::atomic<bool> partial_cwt_cancel_done{false};
    std::thread partial_cwt_cancel_caller([&] {
        const std::uint8_t *data = nullptr;
        std::uint64_t size = 0;
        dtv_error apdu_error{};
        apdu_error.struct_size = sizeof(apdu_error);
        const auto code = dtv_transmit_apdu(root, apdu_one.data(), apdu_one.size(),
            nullptr, &data, &size, &apdu_error);
        if (code == DTV_ERROR_OK && data != nullptr)
            partial_cwt_cancel_response.assign(data, data + size);
        partial_cwt_cancel_code.store(code, std::memory_order_release);
        partial_cwt_cancel_done.store(true, std::memory_order_release);
    });
    auto partial_cwt_cancel_next_output = partial_cwt_cancel_output_start;
    bool partial_cwt_cancel_sent = false;
    for (unsigned step = 0; step < 16 &&
         !partial_cwt_cancel_done.load(std::memory_order_acquire); ++step) {
        if (!backend->wait_for_endpoint_submissions(0x02, partial_cwt_cancel_next_output))
            break;
        auto cancel_callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(cancel_callbacks + 1)) break;
        cancel_callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        if (!backend->wait_for_callbacks(cancel_callbacks + 1)) break;
        ++partial_cwt_cancel_next_output;
        if (backend->output_snapshot().size() >= partial_cwt_cancel_start + 8) {
            if (dtv_cancel_enclosure(root, &error) == DTV_ERROR_OK)
                partial_cwt_cancel_sent = true;
            break;
        }
    }
    partial_cwt_cancel_caller.join();
    if (!partial_cwt_cancel_sent ||
        partial_cwt_cancel_code.load(std::memory_order_acquire) != DTV_ERROR_CANCELLED ||
        !partial_cwt_cancel_response.empty()) {
        std::fprintf(stderr, "partial CWT cancel sent=%u code=%d response=%zu\n",
            partial_cwt_cancel_sent ? 1 : 0,
            partial_cwt_cancel_code.load(std::memory_order_acquire),
            partial_cwt_cancel_response.size());
        return 282;
    }
    set_scan_card_t1_cwt_fault(0);
    if (!reset_t1_session()) return 283;

    set_scan_card_t1_apdu_mode(true, 26);
    set_scan_card_t1_cwt_fault(102);
    std::vector<unsigned char> partial_cwt_unknown_response;
    std::vector<std::vector<unsigned char>> partial_cwt_unknown_output;
    const auto partial_cwt_unknown_code = run_public_apdu(1, apdu_one,
        &partial_cwt_unknown_response, &partial_cwt_unknown_output, 1000);
    if (partial_cwt_unknown_code != DTV_ERROR_TIMEOUT ||
        !partial_cwt_unknown_response.empty() ||
        !same_apdu_frame_sent(1, partial_cwt_unknown_output, partial_cwt_host_i, apdu_one, 1) ||
        !same_r_frame_sent(1, partial_cwt_unknown_output, partial_cwt_host_r, 1)) {
        std::fprintf(stderr, "partial CWT unknown R code=%d response=%zu output=%zu\n",
            partial_cwt_unknown_code, partial_cwt_unknown_response.size(),
            partial_cwt_unknown_output.size());
        return 279;
    }
    set_scan_card_t1_cwt_fault(0);
    if (!reset_t1_session()) return 280;

    set_scan_card_t1_bgt(2, 36000000, 202);
    set_scan_card_t1_bwt_mode(9);
    if (!reset_t1_session()) return 274;
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    set_scan_card_t1_apdu_mode(true, 50);
    set_scan_card_t1_cwt_fault(101);
    const auto below_bgt_host_i = encode_t1_lrc_frame(0x00, apdu_one);
    const auto below_bgt_host_r = encode_t1_lrc_frame(0x82, {});
    std::vector<unsigned char> below_bgt_response;
    std::vector<std::vector<unsigned char>> below_bgt_output;
    const auto below_bgt_code = run_public_apdu(1, apdu_one, &below_bgt_response,
        &below_bgt_output, 100);
    if (below_bgt_code != DTV_ERROR_TIMEOUT || !below_bgt_response.empty() ||
        !same_apdu_frame_sent(1, below_bgt_output, below_bgt_host_i, apdu_one, 1) ||
        same_r_frame_sent(1, below_bgt_output, below_bgt_host_r, 1)) {
        std::fprintf(stderr, "partial CWT below BGT code=%d response=%zu output=%zu I=%d R=%d\n",
            below_bgt_code, below_bgt_response.size(), below_bgt_output.size(),
            same_apdu_frame_sent(1, below_bgt_output, below_bgt_host_i, apdu_one, 1),
            same_r_frame_sent(1, below_bgt_output, below_bgt_host_r, 1));
        for (const auto &packet : below_bgt_output) {
            std::fprintf(stderr, "partial CWT below BGT packet:");
            for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        return 275;
    }
    set_scan_card_t1_cwt_fault(0);
    if (!reset_t1_session()) return 276;
    std::vector<unsigned char> bgt_timeout_warmup_response;
    std::vector<std::vector<unsigned char>> bgt_timeout_warmup_output;
    const auto bgt_timeout_warmup_code = run_public_apdu(1, apdu_one,
        &bgt_timeout_warmup_response, &bgt_timeout_warmup_output);
    if (bgt_timeout_warmup_code != DTV_ERROR_OK ||
        bgt_timeout_warmup_response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00}) {
        std::fprintf(stderr, "BGT timeout warmup code=%d response=%zu output=%zu\n",
            bgt_timeout_warmup_code, bgt_timeout_warmup_response.size(),
            bgt_timeout_warmup_output.size());
        return 277;
    }

    set_scan_card_t1_bgt(2, 100000000, 1000);
    const auto timeout_output_count = backend->output_snapshot().size();
    std::vector<unsigned char> timeout_response;
    std::vector<std::vector<unsigned char>> timeout_output;
    const auto public_timeout_code = run_public_apdu(1, apdu_one,
        &timeout_response, &timeout_output, 1);
    if (public_timeout_code != DTV_ERROR_TIMEOUT || !timeout_response.empty() ||
        !timeout_output.empty() || backend->output_snapshot().size() != timeout_output_count) {
        std::fprintf(stderr, "public APDU timeout code=%d response=%zu output=%zu delta=%zu\n",
            public_timeout_code, timeout_response.size(), timeout_output.size(),
            backend->output_snapshot().size() - timeout_output_count);
        return 266;
    }
    set_scan_card_t1_bgt(2, 1000000, 1001);
    synthetic_bgt_clock.cancel_root.store(root, std::memory_order_release);
    synthetic_bgt_clock.cancel_on_delay.store(true, std::memory_order_release);
    const auto cancel_output_count = backend->output_snapshot().size();
    std::vector<unsigned char> cancelled_response;
    std::vector<std::vector<unsigned char>> cancelled_output;
    const auto public_cancel_code = run_public_apdu(1, apdu_one,
        &cancelled_response, &cancelled_output);
    synthetic_bgt_clock.cancel_root.store(nullptr, std::memory_order_release);
    synthetic_bgt_clock.cancel_on_delay.store(false, std::memory_order_release);
    if (public_cancel_code != DTV_ERROR_CANCELLED || !cancelled_response.empty() ||
        !cancelled_output.empty() || backend->output_snapshot().size() != cancel_output_count)
        return 268;
    set_scan_card_t1_bgt(2, 1000000, 1002);
    set_scan_card_t1_bwt_mode(0);
    set_scan_card_t1_crc(false);
    if (!reset_t1_session()) return 265;
    set_scan_card_t1_apdu_mode(true, 0);
    const auto same_wtx_frame_sent = [&](unsigned mode,
        const std::vector<std::vector<unsigned char>> &sent,
        const std::vector<unsigned char> &frame, std::size_t expected_frames) {
        if (mode == 2) {
            return std::count(sent.begin(), sent.end(), frame) ==
                static_cast<std::ptrdiff_t>(expected_frames);
        }
        std::vector<std::size_t> headers;
        for (std::size_t index = 0; index < sent.size(); ++index) {
            if (sent[index] == std::vector<unsigned char>{0x00, frame[1], frame[2]})
                headers.push_back(index);
        }
        if (headers.size() != expected_frames) return false;
        return std::all_of(headers.begin(), headers.end(), [&](const auto index) {
            return index + 2 < sent.size() &&
                sent[index + 1] == std::vector<unsigned char>(frame.begin() + 3,
                    frame.end() - 1) &&
                sent[index + 2] == std::vector<unsigned char>{frame.back()};
        });
    };
    const std::array<unsigned char, 1> host_ifs_value{0x10};
    const auto host_ifs_request_lrc = encode_t1_lrc_frame(0xc1, host_ifs_value);
    const auto host_i_zero_lrc = encode_t1_lrc_frame(0x00, apdu_one);
    const auto host_r_one_lrc = encode_t1_lrc_frame(0x90, {});
    const auto host_i_one_lrc = encode_t1_lrc_frame(0x40, apdu_two);
    const auto host_r_zero_lrc = encode_t1_lrc_frame(0x82, {});
    for (const auto mode : {1u, 2u}) {
        set_scan_card_t1_crc(mode == 2);
        if (!reset_t1_session()) return 220;
        set_scan_card_t1_apdu_mode(true, 51);
        const auto host_i_lrc = mode == 1 ? encode_t1_lrc_frame(0x00, apdu_one)
                                          : encode_t1_crc_frame(0x00, apdu_one);
        const std::array<unsigned char, 1> wtx_value{2};
        const auto host_wtx = mode == 1 ? encode_t1_lrc_frame(0xe3, wtx_value)
                                        : encode_t1_crc_frame(0xe3, wtx_value);
        const std::array<unsigned char, 4> wtx_response{0x11, 0x22, 0x90, 0x00};
        const auto wtx_receive_bytes = (mode == 1 ? encode_t1_lrc_frame(0xc3, wtx_value)
                                                  : encode_t1_crc_frame(0xc3, wtx_value)).size() +
            (mode == 1 ? encode_t1_lrc_frame(0x00, wtx_response)
                       : encode_t1_crc_frame(0x00, wtx_response)).size();
        dtv::internal::T1ApduResult wtx_result{};
        std::vector<std::vector<unsigned char>> wtx_output;
        const auto wtx_code = run_t1_apdu(mode, apdu_one, &wtx_result, &wtx_output);
        if (wtx_code != DTV_INTERNAL_RESULT_OK ||
            wtx_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            !wtx_result.io.reply_complete || !wtx_result.io.result_known ||
            wtx_result.io.tx_bytes_done != host_i_lrc.size() + host_wtx.size() ||
            wtx_result.io.rx_bytes_done != wtx_receive_bytes ||
            !same_apdu_frame_sent(mode, wtx_output, host_i_lrc, apdu_one, 1) ||
            !same_wtx_frame_sent(mode, wtx_output, host_wtx, 1)) {
            std::fprintf(stderr, "WTX mode=%u code=%d transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
                mode, wtx_code, wtx_result.io.transport_error.code,
                wtx_result.response.size(), wtx_result.io.tx_bytes_done,
                wtx_result.io.rx_bytes_done, wtx_result.io.result_known,
                wtx_result.io.reply_complete, wtx_output.size());
            for (const auto &packet : wtx_output) {
                std::fprintf(stderr, "wtx packet:");
                for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
                std::fprintf(stderr, "\n");
            }
            return 221;
        }
        set_scan_card_t1_apdu_mode(true, 50);
        dtv::internal::T1ApduResult next_wtx_result{};
        if (run_t1_apdu(mode, apdu_two, &next_wtx_result) != DTV_INTERNAL_RESULT_OK ||
            next_wtx_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00})
            return 222;
        if (!reset_t1_session()) return 223;
        set_scan_card_t1_apdu_mode(true, 52);
        dtv::internal::T1ApduResult repeated_wtx_result{};
        std::vector<std::vector<unsigned char>> repeated_wtx_output;
        const auto repeated_host_wtx = mode == 1 ? encode_t1_lrc_frame(0xe3, wtx_value)
                                                  : encode_t1_crc_frame(0xe3, wtx_value);
        if (run_t1_apdu(mode, apdu_one, &repeated_wtx_result, &repeated_wtx_output) !=
                DTV_INTERNAL_RESULT_OK ||
            repeated_wtx_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            !repeated_wtx_result.io.reply_complete || !repeated_wtx_result.io.result_known ||
            repeated_wtx_result.io.tx_bytes_done != host_i_lrc.size() +
                2 * repeated_host_wtx.size() ||
            repeated_wtx_result.io.rx_bytes_done != wtx_receive_bytes +
                (mode == 1 ? encode_t1_lrc_frame(0xc3, wtx_value)
                           : encode_t1_crc_frame(0xc3, wtx_value)).size() ||
            !same_wtx_frame_sent(mode, repeated_wtx_output, repeated_host_wtx, 2)) return 224;
        if (!reset_t1_session()) return 226;
        set_scan_card_t1_apdu_mode(true, 54);
        dtv::internal::T1ApduResult wtx_timeout_result{};
        std::vector<std::vector<unsigned char>> wtx_timeout_output;
        const auto wtx_timeout_code = run_t1_apdu(mode, apdu_one, &wtx_timeout_result,
            &wtx_timeout_output);
        const auto host_r_zero = mode == 1 ? encode_t1_lrc_frame(0x82, {})
                                           : encode_t1_crc_frame(0x82, {});
        if (wtx_timeout_code != DTV_INTERNAL_RESULT_OK ||
            wtx_timeout_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            wtx_timeout_result.io.tx_bytes_done != host_i_lrc.size() + host_wtx.size() +
                host_r_zero.size() || wtx_timeout_result.io.rx_bytes_done != wtx_receive_bytes ||
            !wtx_timeout_result.io.reply_complete || !wtx_timeout_result.io.result_known ||
            !same_apdu_frame_sent(mode, wtx_timeout_output, host_i_lrc, apdu_one, 1) ||
            !same_wtx_frame_sent(mode, wtx_timeout_output, host_wtx, 1) ||
            !same_r_frame_sent(mode, wtx_timeout_output, host_r_zero, 1)) {
            std::fprintf(stderr, "WTX timeout mode=%u code=%d transport=%d/%u response=%zu tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
                mode, wtx_timeout_code, wtx_timeout_result.io.transport_error.code,
                wtx_timeout_result.io.transport_error.detail, wtx_timeout_result.response.size(),
                wtx_timeout_result.io.tx_bytes_done, wtx_timeout_result.io.rx_bytes_done,
                wtx_timeout_result.io.result_known, wtx_timeout_result.io.reply_complete,
                wtx_timeout_output.size());
            for (const auto &packet : wtx_timeout_output) {
                std::fprintf(stderr, "WTX timeout packet:");
                for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
                std::fprintf(stderr, "\n");
            }
            return 227;
        }
        if (!reset_t1_session()) return 228;
        set_scan_card_t1_apdu_mode(true, 53);
        dtv::internal::T1ApduResult invalid_wtx_result{};
        std::vector<std::vector<unsigned char>> invalid_wtx_output;
        const auto invalid_wtx_code = run_t1_apdu(mode, apdu_one, &invalid_wtx_result,
            &invalid_wtx_output);
        if (invalid_wtx_code == DTV_INTERNAL_RESULT_OK ||
            !invalid_wtx_result.response.empty() || invalid_wtx_result.io.result_known ||
            invalid_wtx_result.io.reply_complete ||
            !same_apdu_frame_sent(mode, invalid_wtx_output, host_i_lrc, apdu_one, 1) ||
            !same_wtx_frame_sent(mode, invalid_wtx_output, host_wtx, 0)) {
            std::fprintf(stderr, "invalid WTX mode=%u code=%d transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
                mode, invalid_wtx_code, invalid_wtx_result.io.transport_error.code,
                invalid_wtx_result.response.size(), invalid_wtx_result.io.tx_bytes_done,
                invalid_wtx_result.io.rx_bytes_done, invalid_wtx_result.io.result_known,
                invalid_wtx_result.io.reply_complete, invalid_wtx_output.size());
            for (const auto &packet : invalid_wtx_output) {
                std::fprintf(stderr, "invalid WTX packet:");
                for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
                std::fprintf(stderr, "\n");
            }
            return 229;
        }
    }
    for (const auto mode : {1u, 2u}) {
        set_scan_card_t1_crc(mode == 2);
        for (const auto [fault, multiplier] : {
                std::pair{68u, static_cast<unsigned char>(60)},
                std::pair{69u, static_cast<unsigned char>(255)}}) {
            if (!reset_t1_session()) return 243;
            set_scan_card_t1_apdu_mode(true, fault);
            const std::array<unsigned char, 1> range_wtx_value{multiplier};
            const auto range_host_i = mode == 1 ? encode_t1_lrc_frame(0x00, apdu_one)
                                                 : encode_t1_crc_frame(0x00, apdu_one);
            const auto range_host_wtx = mode == 1
                ? encode_t1_lrc_frame(0xe3, range_wtx_value)
                : encode_t1_crc_frame(0xe3, range_wtx_value);
            const std::array<unsigned char, 4> range_response{0x11, 0x22, 0x90, 0x00};
            const auto range_receive_bytes = (mode == 1
                ? encode_t1_lrc_frame(0xc3, range_wtx_value)
                : encode_t1_crc_frame(0xc3, range_wtx_value)).size() +
                (mode == 1 ? encode_t1_lrc_frame(0x00, range_response)
                           : encode_t1_crc_frame(0x00, range_response)).size();
            dtv::internal::T1ApduResult range_wtx_result{};
            std::vector<std::vector<unsigned char>> range_wtx_output;
            const auto range_wtx_code = run_t1_apdu(mode, apdu_one,
                &range_wtx_result, &range_wtx_output, 5000);
            if (range_wtx_code != DTV_INTERNAL_RESULT_OK ||
                range_wtx_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
                !range_wtx_result.io.result_known || !range_wtx_result.io.reply_complete ||
                range_wtx_result.io.tx_bytes_done != range_host_i.size() + range_host_wtx.size() ||
                range_wtx_result.io.rx_bytes_done != range_receive_bytes ||
                !same_apdu_frame_sent(mode, range_wtx_output, range_host_i, apdu_one, 1) ||
                !same_wtx_frame_sent(mode, range_wtx_output, range_host_wtx, 1)) {
                std::fprintf(stderr, "WTX range mode=%u multiplier=%u code=%d tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
                    mode, multiplier, range_wtx_code, range_wtx_result.io.tx_bytes_done,
                    range_wtx_result.io.rx_bytes_done, range_wtx_result.io.result_known,
                    range_wtx_result.io.reply_complete, range_wtx_output.size());
                return 244;
            }
            set_scan_card_t1_apdu_mode(true, 50);
            dtv::internal::T1ApduResult range_next_result{};
            if (run_t1_apdu(mode, apdu_two, &range_next_result) != DTV_INTERNAL_RESULT_OK ||
                range_next_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
                !range_next_result.io.result_known || !range_next_result.io.reply_complete)
                return 245;
        }
    }
    set_scan_card_t1_crc(true);
    set_scan_card_t1_bwt_mode(8);
    if (!reset_t1_session()) return 246;
    set_scan_card_t1_apdu_mode(true, 70);
    dtv::internal::T1ApduResult overflow_wtx_result{};
    std::vector<std::vector<unsigned char>> overflow_wtx_output;
    const auto overflow_wtx_code = run_t1_apdu(2, apdu_one,
        &overflow_wtx_result, &overflow_wtx_output, 5000);
    const auto overflow_host_i = encode_t1_crc_frame(0x00, apdu_one);
    const auto overflow_wtx_request = encode_t1_crc_frame(0xc3,
        std::array<unsigned char, 1>{101});
    if (overflow_wtx_code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
        !overflow_wtx_result.response.empty() || overflow_wtx_result.io.result_known ||
        overflow_wtx_result.io.reply_complete ||
        overflow_wtx_result.io.tx_bytes_done != overflow_host_i.size() ||
        overflow_wtx_result.io.rx_bytes_done != overflow_wtx_request.size() ||
        !same_apdu_frame_sent(2, overflow_wtx_output, overflow_host_i, apdu_one, 1) ||
        !same_wtx_frame_sent(2, overflow_wtx_output,
            encode_t1_crc_frame(0xe3, std::array<unsigned char, 1>{101}), 0)) {
        std::fprintf(stderr, "WTX overflow code=%d transport=%d tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
            overflow_wtx_code, overflow_wtx_result.io.transport_error.code,
            overflow_wtx_result.io.tx_bytes_done,
            overflow_wtx_result.io.rx_bytes_done, overflow_wtx_result.io.result_known,
            overflow_wtx_result.io.reply_complete, overflow_wtx_output.size());
        for (const auto &packet : overflow_wtx_output) {
            std::fprintf(stderr, "WTX overflow packet:");
            for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        return 247;
    }
    set_scan_card_t1_bwt_mode(0);
    for (const auto mode : {1u, 2u}) {
        set_scan_card_t1_crc(mode == 2);
        set_scan_card_t1_bwt_mode(mode == 2 ? 9 : 0);
        set_scan_card_t1_cwt_fault(0);
        if (!reset_t1_session()) return 248;
        set_scan_card_t1_apdu_mode(true, 50);
        const auto cwt_host_i = mode == 1 ? encode_t1_lrc_frame(0x00, apdu_one)
                                          : encode_t1_crc_frame(0x00, apdu_one);
        const std::array<unsigned char, 4> cwt_response{0x11, 0x22, 0x90, 0x00};
        const auto cwt_response_frame = mode == 1
            ? encode_t1_lrc_frame(0x00, cwt_response)
            : encode_t1_crc_frame(0x00, cwt_response);
        dtv::internal::T1ApduResult cwt_timely_result{};
        std::vector<std::vector<unsigned char>> cwt_timely_output;
        const auto cwt_timely_code = run_t1_apdu(mode, apdu_one, &cwt_timely_result,
            &cwt_timely_output);
        if (cwt_timely_code != DTV_INTERNAL_RESULT_OK ||
            cwt_timely_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            cwt_timely_result.io.tx_bytes_done != cwt_host_i.size() ||
            cwt_timely_result.io.rx_bytes_done != cwt_response_frame.size() ||
            !cwt_timely_result.io.result_known || !cwt_timely_result.io.reply_complete ||
            !same_apdu_frame_sent(mode, cwt_timely_output, cwt_host_i, apdu_one, 1)) {
            std::fprintf(stderr, "CWT timely mode=%u code=%d transport=%d tx=%zu/%zu rx=%zu/%zu known=%u complete=%u output=%zu response=%zu\n",
                mode, cwt_timely_code, cwt_timely_result.io.transport_error.code,
                cwt_timely_result.io.tx_bytes_done, cwt_host_i.size(),
                cwt_timely_result.io.rx_bytes_done, cwt_response_frame.size(),
                cwt_timely_result.io.result_known, cwt_timely_result.io.reply_complete,
                cwt_timely_output.size(), cwt_timely_result.response.size());
            return 249;
        }
        set_scan_card_t1_cwt_fault(0);
        set_scan_card_t1_apdu_mode(true, 50);
        dtv::internal::T1ApduResult cwt_next_result{};
        if (run_t1_apdu(mode, apdu_two, &cwt_next_result) != DTV_INTERNAL_RESULT_OK ||
            cwt_next_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
            !cwt_next_result.io.result_known || !cwt_next_result.io.reply_complete)
            return 250;
        set_scan_card_t1_bwt_mode(0);
    }
    set_scan_card_t1_crc(true);
    set_scan_card_t1_bwt_mode(9);
    if (!reset_t1_session()) return 257;
    set_scan_card_t1_apdu_mode(true, 0);
    set_scan_card_t1_cwt_fault(97);
    const auto cwt_cwi0_host_i = encode_t1_crc_frame(0x00, apdu_one);
    const std::array<unsigned char, 4> cwt_cwi0_response{0x11, 0x22, 0x90, 0x00};
    const auto cwt_cwi0_card_i = encode_t1_crc_frame(0x00, cwt_cwi0_response);
    dtv::internal::T1ApduResult cwt_cwi0_result{};
    std::vector<std::vector<unsigned char>> cwt_cwi0_output;
    if (run_t1_apdu(2, apdu_one, &cwt_cwi0_result, &cwt_cwi0_output) !=
            DTV_INTERNAL_RESULT_UNSUPPORTED || !cwt_cwi0_result.response.empty() ||
        cwt_cwi0_result.io.result_known || cwt_cwi0_result.io.reply_complete ||
        cwt_cwi0_result.io.tx_bytes_done != cwt_cwi0_host_i.size() ||
        cwt_cwi0_result.io.rx_bytes_done != cwt_cwi0_card_i.size() ||
        !same_apdu_frame_sent(2, cwt_cwi0_output, cwt_cwi0_host_i, apdu_one, 1))
        return 258;
    set_scan_card_t1_cwt_fault(0);
    set_scan_card_t1_bwt_mode(0);
    for (const auto &[bwt_mode, fault] : {std::pair{10u, 98u}, std::pair{11u, 99u}}) {
        set_scan_card_t1_crc(true);
        set_scan_card_t1_bwt_mode(bwt_mode);
        if (!reset_t1_session()) return 259;
        set_scan_card_t1_apdu_mode(true, 50);
        set_scan_card_t1_cwt_fault(fault);
        const auto cwt_host_i = encode_t1_crc_frame(0x00, apdu_one);
        const std::array<unsigned char, 4> cwt_response{0x11, 0x22, 0x90, 0x00};
        const auto cwt_card_i = encode_t1_crc_frame(0x00, cwt_response);
        dtv::internal::T1ApduResult cwt_result{};
        std::vector<std::vector<unsigned char>> cwt_output;
        if (run_t1_apdu(2, apdu_one, &cwt_result, &cwt_output) != DTV_INTERNAL_RESULT_OK ||
            cwt_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            cwt_result.io.tx_bytes_done != cwt_host_i.size() ||
            cwt_result.io.rx_bytes_done != cwt_card_i.size() ||
            !cwt_result.io.result_known || !cwt_result.io.reply_complete ||
            !same_apdu_frame_sent(2, cwt_output, cwt_host_i, apdu_one, 1)) {
            std::fprintf(stderr, "CWT CWI14/15 mode=%u fault=%u code=%d tx=%zu/%zu rx=%zu/%zu known=%u complete=%u\n",
                bwt_mode, fault, cwt_result.io.transport_error.code,
                cwt_result.io.tx_bytes_done, cwt_host_i.size(),
                cwt_result.io.rx_bytes_done, cwt_card_i.size(),
                cwt_result.io.result_known, cwt_result.io.reply_complete);
            return 260;
        }
        set_scan_card_t1_cwt_fault(0);
        dtv::internal::T1ApduResult cwt_next_result{};
        if (run_t1_apdu(2, apdu_two, &cwt_next_result) != DTV_INTERNAL_RESULT_OK ||
            cwt_next_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
            !cwt_next_result.io.result_known || !cwt_next_result.io.reply_complete)
            return 261;
        set_scan_card_t1_bwt_mode(0);
    }
    for (const auto mode : {1u, 2u}) {
        set_scan_card_t1_crc(mode == 2);
        const auto cwt_host_i = mode == 1 ? encode_t1_lrc_frame(0x00, apdu_one)
                                          : encode_t1_crc_frame(0x00, apdu_one);
        const std::array<unsigned char, 4> cwt_response{0x11, 0x22, 0x90, 0x00};
        const auto cwt_card_i = mode == 1 ? encode_t1_lrc_frame(0x00, cwt_response)
                                          : encode_t1_crc_frame(0x00, cwt_response);
        for (const auto fault : {91u, 93u}) {
            if (!reset_t1_session()) return 251;
            set_scan_card_t1_apdu_mode(true, 0);
            set_scan_card_t1_cwt_fault(fault);
            dtv::internal::T1ApduResult cwt_rejected_result{};
            std::vector<std::vector<unsigned char>> cwt_rejected_output;
            const auto cwt_rejected_code = run_t1_apdu(mode, apdu_one,
                &cwt_rejected_result, &cwt_rejected_output);
            if (cwt_rejected_code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
                !cwt_rejected_result.response.empty() || cwt_rejected_result.io.result_known ||
                cwt_rejected_result.io.reply_complete ||
                cwt_rejected_result.io.tx_bytes_done != cwt_host_i.size() ||
                cwt_rejected_result.io.rx_bytes_done != cwt_card_i.size() ||
                !same_apdu_frame_sent(mode, cwt_rejected_output, cwt_host_i, apdu_one, 1)) {
                std::fprintf(stderr, "CWT reject mode=%u fault=%u code=%d tx=%zu rx=%zu known=%u complete=%u output=%zu same=%u expected-frame=%zu\n",
                    mode, fault, cwt_rejected_code, cwt_rejected_result.io.tx_bytes_done,
                    cwt_rejected_result.io.rx_bytes_done, cwt_rejected_result.io.result_known,
                    cwt_rejected_result.io.reply_complete, cwt_rejected_output.size(),
                    same_apdu_frame_sent(mode, cwt_rejected_output, cwt_host_i, apdu_one, 1),
                    cwt_host_i.size());
                return 252;
            }
        }
    }
    set_scan_card_t1_crc(false);
    if (!reset_t1_session()) return 253;
    set_scan_card_t1_apdu_mode(true, 0);
    set_scan_card_t1_cwt_fault(92);
    dtv::internal::T1ApduResult cwt_boundary_result{};
    std::vector<std::vector<unsigned char>> cwt_boundary_output;
    const auto cwt_boundary_code = run_t1_apdu(1, apdu_one,
        &cwt_boundary_result, &cwt_boundary_output);
    if (cwt_boundary_code != DTV_INTERNAL_RESULT_OK ||
        cwt_boundary_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        !cwt_boundary_result.io.result_known || !cwt_boundary_result.io.reply_complete)
        return 254;
    const auto cwt_lrc_host_i = encode_t1_lrc_frame(0x00, apdu_one);
    const std::array<unsigned char, 4> cwt_lrc_response{0x11, 0x22, 0x90, 0x00};
    const auto cwt_lrc_card_i = encode_t1_lrc_frame(0x00, cwt_lrc_response);
    for (const auto fault : {94u, 95u, 96u}) {
        if (!reset_t1_session()) return 255;
        set_scan_card_t1_apdu_mode(true, 0);
        set_scan_card_t1_cwt_fault(fault);
        dtv::internal::T1ApduResult cwt_invalid_evidence_result{};
        std::vector<std::vector<unsigned char>> cwt_invalid_evidence_output;
        const auto cwt_invalid_evidence_code = run_t1_apdu(1, apdu_one,
            &cwt_invalid_evidence_result, &cwt_invalid_evidence_output);
        if (cwt_invalid_evidence_code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
            !cwt_invalid_evidence_result.response.empty() ||
            cwt_invalid_evidence_result.io.result_known ||
            cwt_invalid_evidence_result.io.reply_complete ||
            cwt_invalid_evidence_result.io.tx_bytes_done != cwt_lrc_host_i.size() ||
            cwt_invalid_evidence_result.io.rx_bytes_done != cwt_lrc_card_i.size() ||
            !same_apdu_frame_sent(1, cwt_invalid_evidence_output,
                cwt_lrc_host_i, apdu_one, 1))
            return 256;
    }
    set_scan_card_t1_cwt_fault(0);
    set_scan_card_t1_crc(true);
    if (!reset_t1_session()) return 234;
    set_scan_card_t1_bwt_mode(0);
    set_scan_card_t1_apdu_mode(true, 55);
    dtv::internal::T1ApduResult delayed_wtx_result{};
    std::vector<std::vector<unsigned char>> delayed_wtx_output;
    const auto delayed_wtx_code = run_t1_apdu(2, apdu_one, &delayed_wtx_result,
        &delayed_wtx_output, 5000);
    const auto delayed_host_i = encode_t1_crc_frame(0x00, apdu_one);
    const std::array<unsigned char, 1> delayed_wtx_value{2};
    const auto delayed_host_wtx = encode_t1_crc_frame(0xe3, delayed_wtx_value);
    const std::array<unsigned char, 4> delayed_wtx_response{0x11, 0x22, 0x90, 0x00};
    const auto delayed_wtx_receive_bytes = encode_t1_crc_frame(0xc3,
        delayed_wtx_value).size() + encode_t1_crc_frame(0x00, delayed_wtx_response).size();
    if (delayed_wtx_code != DTV_INTERNAL_RESULT_OK ||
        delayed_wtx_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        delayed_wtx_result.io.tx_bytes_done != delayed_host_i.size() + delayed_host_wtx.size() ||
        !delayed_wtx_result.io.reply_complete || !delayed_wtx_result.io.result_known ||
        !same_apdu_frame_sent(2, delayed_wtx_output, delayed_host_i, apdu_one, 1) ||
        !same_wtx_frame_sent(2, delayed_wtx_output, delayed_host_wtx, 1)) {
        std::fprintf(stderr, "delayed WTX code=%d transport=%d tx=%zu/%zu rx=%zu/%zu known=%u complete=%u output=%zu response=%zu\n",
            delayed_wtx_code, delayed_wtx_result.io.transport_error.code,
            delayed_wtx_result.io.tx_bytes_done,
            delayed_host_i.size() + delayed_host_wtx.size(),
            delayed_wtx_result.io.rx_bytes_done, delayed_wtx_receive_bytes,
            delayed_wtx_result.io.result_known, delayed_wtx_result.io.reply_complete,
            delayed_wtx_output.size(), delayed_wtx_result.response.size());
        return 235;
    }
    if (!reset_t1_session()) return 236;
    set_scan_card_t1_apdu_mode(true, 56);
    dtv::internal::T1ApduResult invalid_wtx_epoch_result{};
    std::vector<std::vector<unsigned char>> invalid_wtx_epoch_output;
    const auto invalid_wtx_epoch_code = run_t1_apdu(2, apdu_one,
        &invalid_wtx_epoch_result, &invalid_wtx_epoch_output);
    if (invalid_wtx_epoch_code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
        !invalid_wtx_epoch_result.response.empty() ||
        invalid_wtx_epoch_result.io.result_known ||
        invalid_wtx_epoch_result.io.reply_complete ||
        invalid_wtx_epoch_result.io.tx_bytes_done != delayed_host_i.size() +
            delayed_host_wtx.size() ||
        invalid_wtx_epoch_result.io.rx_bytes_done != delayed_wtx_receive_bytes ||
        !same_apdu_frame_sent(2, invalid_wtx_epoch_output, delayed_host_i, apdu_one, 1) ||
        !same_wtx_frame_sent(2, invalid_wtx_epoch_output, delayed_host_wtx, 1)) return 237;
    set_scan_card_t1_bgt(2, 1000000, 202);
    if (!reset_t1_session()) return 238;
    set_scan_card_t1_apdu_mode(true, 57);
    dtv::internal::T1ApduResult caller_deadline_wtx_result{};
    std::vector<std::vector<unsigned char>> caller_deadline_wtx_output;
    const auto caller_deadline_wtx_code = run_t1_apdu(2, apdu_one,
        &caller_deadline_wtx_result, &caller_deadline_wtx_output, 100);
    if (caller_deadline_wtx_code != DTV_INTERNAL_RESULT_TIMEOUT ||
        !caller_deadline_wtx_result.response.empty() ||
        caller_deadline_wtx_result.io.tx_bytes_done != delayed_host_i.size() ||
        caller_deadline_wtx_result.io.rx_bytes_done !=
            encode_t1_crc_frame(0xc3, delayed_wtx_value).size() ||
        caller_deadline_wtx_result.io.result_known ||
        caller_deadline_wtx_result.io.reply_complete ||
        !same_apdu_frame_sent(2, caller_deadline_wtx_output, delayed_host_i, apdu_one, 1) ||
        !same_wtx_frame_sent(2, caller_deadline_wtx_output, delayed_host_wtx, 0)) {
        std::fprintf(stderr, "WTX caller deadline code=%d transport=%d tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
            caller_deadline_wtx_code, caller_deadline_wtx_result.io.transport_error.code,
            caller_deadline_wtx_result.io.tx_bytes_done,
            caller_deadline_wtx_result.io.rx_bytes_done,
            caller_deadline_wtx_result.io.result_known,
            caller_deadline_wtx_result.io.reply_complete,
            caller_deadline_wtx_output.size());
        for (const auto &packet : caller_deadline_wtx_output) {
            std::fprintf(stderr, "WTX deadline packet:");
            for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        return 239;
    }
    set_scan_card_t1_bgt(2, 1000000, 205);
    if (!reset_t1_session()) return 230;
    set_scan_card_t1_apdu_mode(true, 51);
    dtv::internal::T1ApduResult partial_wtx_out_result{};
    std::vector<std::vector<unsigned char>> partial_wtx_out;
    const auto partial_wtx_out_code = run_t1_apdu(2, apdu_one,
        &partial_wtx_out_result, &partial_wtx_out, 1000, nullptr, nullptr, 2);
    if (partial_wtx_out_code != DTV_INTERNAL_RESULT_USB_IO ||
        !partial_wtx_out_result.response.empty() ||
        partial_wtx_out_result.io.tx_bytes_done != 11 ||
        partial_wtx_out_result.io.result_known || partial_wtx_out_result.io.reply_complete)
        return 231;
    if (!reset_t1_session()) return 232;
    set_scan_card_t1_bgt(2, 1000000000, 200);
    set_scan_card_t1_apdu_mode(true, 51);
    dtv::internal::T1ApduResult cancelled_wtx_result{};
    std::vector<std::vector<unsigned char>> cancelled_wtx_output;
    const auto cancelled_wtx_code = run_t1_apdu(2, apdu_one, &cancelled_wtx_result,
        &cancelled_wtx_output, 30000, nullptr, nullptr, 0, 1);
    synthetic_bgt_clock.cancel_root = nullptr;
    synthetic_bgt_clock.cancel_on_delay = false;
    const std::array<unsigned char, 1> cancelled_wtx_value{2};
    if (cancelled_wtx_code != DTV_INTERNAL_RESULT_CANCELLED ||
        !cancelled_wtx_result.response.empty() ||
        cancelled_wtx_result.io.tx_bytes_done != 9 ||
        cancelled_wtx_result.io.result_known || cancelled_wtx_result.io.reply_complete ||
        !same_apdu_frame_sent(2, cancelled_wtx_output,
            encode_t1_crc_frame(0x00, apdu_one), apdu_one, 1) ||
        !same_wtx_frame_sent(2, cancelled_wtx_output,
            encode_t1_crc_frame(0xe3, cancelled_wtx_value), 0)) return 233;
    set_scan_card_t1_crc(false);
    if (!reset_t1_session()) return 225;
    const auto host_ifs_result = std::vector<unsigned char>{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        0x90, 0x00, 0x62, 0x83};
    set_scan_card_t1_bgt(2, 1000000, 18);
    for (const auto mode : {1u, 2u}) {
        for (const auto bwt_mode : {1u, 2u, 3u}) {
            if (!reset_t1_session()) return 200;
            set_scan_card_t1_receive_capacity(false, 0, 0, 0);
            set_scan_card_t1_bwt_mode(bwt_mode);
            set_scan_card_t1_apdu_mode(true, 0);
            dtv::internal::T1ApduResult recovered{};
            std::vector<std::vector<unsigned char>> output;
            const auto recovered_code = run_t1_apdu(mode, apdu_one, &recovered, &output, 5000);
            if (recovered_code != DTV_INTERNAL_RESULT_OK ||
                recovered.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
                !recovered.io.reply_complete || !recovered.io.result_known ||
                !same_apdu_frame_sent(mode, output, host_i_zero_lrc, apdu_one, 1) ||
                !same_r_frame_sent(mode, output, host_r_zero_lrc,
                    bwt_mode == 1 ? 1 : bwt_mode == 2 ? 2 : 0)) {
                std::fprintf(stderr, "BWT recovery mode=%u bwt=%u result=%d response=%zu tx=%zu rx=%zu known=%u complete=%u outputs=%zu\n",
                    mode, bwt_mode, recovered_code, recovered.response.size(),
                    recovered.io.tx_bytes_done, recovered.io.rx_bytes_done,
                    recovered.io.result_known, recovered.io.reply_complete, output.size());
                return 201;
            }
            set_scan_card_t1_bwt_mode(0);
            dtv::internal::T1ApduResult next{};
            if (run_t1_apdu(mode, apdu_two, &next) != DTV_INTERNAL_RESULT_OK ||
                next.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
                !next.io.reply_complete || !next.io.result_known) return 202;
        }
    }
    for (const auto mode : {1u, 2u}) {
        for (const auto bwt_mode : {4u, 5u, 6u, 7u}) {
            if (!reset_t1_session()) return 203;
            set_scan_card_t1_receive_capacity(false, 0, 0, 0);
            set_scan_card_t1_bwt_mode(bwt_mode);
            set_scan_card_t1_apdu_mode(true, 0);
            dtv::internal::T1ApduResult rejected{};
            std::vector<std::vector<unsigned char>> output;
            const auto rejected_code = run_t1_apdu(mode, apdu_one, &rejected, &output, 5000);
            if (rejected_code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
                rejected.io.reply_complete) {
                std::fprintf(stderr, "BWT capability mode=%u bwt=%u result=%d tx=%zu rx=%zu known=%u complete=%u outputs=%zu\n",
                    mode, bwt_mode, rejected_code, rejected.io.tx_bytes_done,
                    rejected.io.rx_bytes_done, rejected.io.result_known,
                    rejected.io.reply_complete, output.size());
                return 204;
            }
            if (bwt_mode == 6) {
                if (rejected.io.tx_bytes_done == 0 || rejected.io.result_known ||
                    !same_apdu_frame_sent(mode, output, host_i_zero_lrc, apdu_one, 1)) {
                    std::fprintf(stderr, "BWT boundary mode=%u result=%d tx=%zu rx=%zu known=%u outputs=%zu\n",
                        mode, rejected_code, rejected.io.tx_bytes_done,
                        rejected.io.rx_bytes_done, rejected.io.result_known, output.size());
                    return 204;
                }
            } else if (rejected.io.tx_bytes_done != 0 || !rejected.io.result_known ||
                !output.empty()) {
                std::fprintf(stderr, "BWT unavailable mode=%u bwt=%u result=%d tx=%zu rx=%zu known=%u outputs=%zu\n",
                    mode, bwt_mode, rejected_code, rejected.io.tx_bytes_done,
                    rejected.io.rx_bytes_done, rejected.io.result_known, output.size());
                return 204;
            }
            set_scan_card_t1_bwt_mode(0);
        }
    }
    for (const auto mode : {1u, 2u}) {
        if (!reset_t1_session()) return 205;
        set_scan_card_t1_receive_capacity(false, 0, 0, 0);
        set_scan_card_t1_bwt_mode(0);
        set_scan_card_t1_apdu_mode(true, 18);
        dtv::internal::T1ApduResult chained{};
        std::vector<std::vector<unsigned char>> chain_output;
        if (run_t1_apdu(mode, apdu_one, &chained, &chain_output) != DTV_INTERNAL_RESULT_OK ||
            chained.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00}) return 205;
        set_scan_card_t1_apdu_mode(true, 50);
        set_scan_card_t1_bwt_mode(1);
        dtv::internal::T1ApduResult divergent{};
        std::vector<std::vector<unsigned char>> divergent_output;
        const auto divergent_code = run_t1_apdu(mode, apdu_two, &divergent,
            &divergent_output);
        const auto expected_nr_zero = same_r_frame_sent(mode, divergent_output,
            host_r_zero_lrc, 1);
        if (divergent_code != DTV_INTERNAL_RESULT_OK || !expected_nr_zero ||
            !same_apdu_frame_sent(mode, divergent_output, host_i_one_lrc, apdu_two, 1) ||
            divergent.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            !divergent.io.reply_complete || !divergent.io.result_known) {
            std::fprintf(stderr, "BWT divergent-sequence mode=%u code=%d response=%zu known=%u complete=%u expected_R_NR0=%u tx=%zu rx=%zu outputs=",
                mode, divergent_code, divergent.response.size(), divergent.io.result_known,
                divergent.io.reply_complete, expected_nr_zero, divergent.io.tx_bytes_done,
                divergent.io.rx_bytes_done);
            for (const auto &frame : divergent_output) {
                std::fprintf(stderr, "[");
                for (const auto byte : frame) std::fprintf(stderr, "%02x", byte);
                std::fprintf(stderr, "]");
            }
            std::fprintf(stderr, "\n");
            return 206;
        }
        set_scan_card_t1_bwt_mode(0);
        set_scan_card_t1_apdu_mode(true, 50);
        dtv::internal::T1ApduResult next{};
        if (run_t1_apdu(mode, apdu_two, &next) != DTV_INTERNAL_RESULT_OK ||
            next.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
            !next.io.reply_complete || !next.io.result_known) return 207;
    }
    for (const auto mode : {1u, 2u}) {
        if (!reset_t1_session()) return 170;
        set_scan_card_t1_receive_capacity(true, 3 + 16 + 1, 1, 1601);
        set_scan_card_t1_apdu_mode(true, 40);
        dtv::internal::T1ApduResult result{};
        std::vector<std::vector<unsigned char>> output;
        std::vector<std::uint64_t> submit_times;
        std::vector<std::uint64_t> input_complete_times;
        if (run_t1_apdu(mode, apdu_one, &result, &output, 1000,
                &submit_times, &input_complete_times) != DTV_INTERNAL_RESULT_OK ||
            result.response != host_ifs_result || result.io.tx_bytes_done != 17 ||
            result.io.rx_bytes_done != 33 || !result.io.reply_complete ||
            !result.io.result_known ||
            !same_apdu_frame_sent(mode, output, host_ifs_request_lrc,
                host_ifs_value, 1) ||
            !same_apdu_frame_sent(mode, output, host_i_zero_lrc, apdu_one, 1) ||
            !same_r_frame_sent(mode, output, host_r_one_lrc, 1)) return 171;
        const auto command_header = mode == 2
            ? std::find(output.begin(), output.end(), host_i_zero_lrc)
            : std::find(output.begin(), output.end(),
                std::vector<unsigned char>{0x00, 0x00, 0x04});
        const auto r_header = mode == 2
            ? std::find(output.begin(), output.end(), host_r_one_lrc)
            : std::find(output.begin(), output.end(),
                std::vector<unsigned char>{0x00, 0x90, 0x00});
        const auto command_index = static_cast<std::size_t>(command_header - output.begin());
        const auto r_index = static_cast<std::size_t>(r_header - output.begin());
        if (command_header == output.end() || r_header == output.end() ||
            command_index == 0 || r_index == 0 ||
            command_index >= submit_times.size() || r_index >= submit_times.size() ||
            command_index - 1 >= input_complete_times.size() ||
            r_index - 1 >= input_complete_times.size() ||
            submit_times[command_index] < input_complete_times[command_index - 1] + 22000000 ||
            submit_times[r_index] < input_complete_times[r_index - 1] + 22000000)
            return 172;
        dtv::internal::T1ApduResult next_result{};
        std::vector<std::vector<unsigned char>> next_output;
        if (run_t1_apdu(mode, apdu_two, &next_result, &next_output) !=
                DTV_INTERNAL_RESULT_OK ||
            next_result.response != std::vector<unsigned char>{0x90, 0x00} ||
            !same_apdu_frame_sent(mode, next_output, host_i_one_lrc, apdu_two, 1) ||
            std::any_of(next_output.begin(), next_output.end(), [](const auto &sent) {
                return sent.size() >= 2 && sent[1] == 0xc1;
            })) return 173;
    }
    set_scan_card_t1_crc(true);
    if (!reset_t1_session()) return 174;
    set_scan_card_t1_receive_capacity(true, 3 + 16 + 2, 2, 1602);
    set_scan_card_t1_apdu_mode(true, 40);
    dtv::internal::T1ApduResult host_ifs_crc_result{};
    std::vector<std::vector<unsigned char>> host_ifs_crc_output;
    const auto host_ifs_request_crc = encode_t1_crc_frame(0xc1, host_ifs_value);
    if (run_t1_apdu(2, apdu_one, &host_ifs_crc_result, &host_ifs_crc_output) !=
            DTV_INTERNAL_RESULT_OK ||
        host_ifs_crc_result.response != host_ifs_result ||
        host_ifs_crc_result.io.tx_bytes_done != 20 ||
        host_ifs_crc_result.io.rx_bytes_done != 36 ||
        !same_apdu_frame_sent(2, host_ifs_crc_output, host_ifs_request_crc,
            host_ifs_value, 1)) return 175;
    set_scan_card_t1_crc(false);
    for (const auto fault : {41u, 42u, 43u, 44u, 46u, 47u, 48u}) {
        if (!reset_t1_session()) return 176;
        set_scan_card_t1_receive_capacity(true, 3 + 16 + 1, 3, 1603);
        set_scan_card_t1_apdu_mode(true, fault);
        dtv::internal::T1ApduResult malformed_result{};
        std::vector<std::vector<unsigned char>> malformed_output;
        const auto code = run_t1_apdu(2, apdu_one, &malformed_result,
            &malformed_output);
        if ((fault == 43 ? code == DTV_INTERNAL_RESULT_OK
                         : code != DTV_INTERNAL_RESULT_USB_IO) ||
            !malformed_result.response.empty() ||
            !malformed_result.io.result_known || malformed_result.io.reply_complete ||
            malformed_result.io.tx_bytes_done != 5 ||
            malformed_result.io.rx_bytes_done != (fault == 43 ? 0u : 5u) ||
            malformed_output.size() != 1 ||
            std::any_of(malformed_output.begin(), malformed_output.end(), [](const auto &sent) {
                return sent.size() >= 2 && sent[1] == 0x00;
            })) return 177;
    }
    if (!reset_t1_session()) return 178;
    set_scan_card_t1_receive_capacity(true, 3 + 16 + 1, 4, 1604);
    set_scan_card_t1_apdu_mode(true, 45);
    dtv_internal_result_code missing_host_ifs_code = DTV_INTERNAL_RESULT_OK;
    dtv::internal::T1ApduResult missing_host_ifs_result{};
    std::vector<std::vector<unsigned char>> missing_host_ifs_output;
    missing_host_ifs_code = static_cast<dtv_internal_result_code>(run_t1_apdu(2,
        apdu_one, &missing_host_ifs_result, &missing_host_ifs_output, 150));
    set_scan_card_transport_drop_reply(false);
    if (missing_host_ifs_code != DTV_INTERNAL_RESULT_TIMEOUT ||
        !missing_host_ifs_result.response.empty() ||
        !missing_host_ifs_result.io.result_known ||
        missing_host_ifs_result.io.tx_bytes_done != 5 ||
        missing_host_ifs_output.size() != 1) return 179;
    if (!reset_t1_session()) return 180;
    set_scan_card_t1_receive_capacity(true, 3 + 16 + 1, 5, 1605);
    set_scan_card_t1_apdu_mode(true, 0);
    dtv::internal::T1ApduResult partial_host_ifs_result{};
    std::vector<std::vector<unsigned char>> partial_host_ifs_output;
    const auto partial_host_ifs_code = run_t1_apdu(2, apdu_one,
        &partial_host_ifs_result, &partial_host_ifs_output, 1000,
        nullptr, nullptr, 1);
    if (partial_host_ifs_code != DTV_INTERNAL_RESULT_USB_IO ||
        !partial_host_ifs_result.response.empty() ||
        !partial_host_ifs_result.io.result_known ||
        partial_host_ifs_result.io.reply_complete ||
        partial_host_ifs_result.io.tx_bytes_done != 2 ||
        partial_host_ifs_output.size() != 1) return 181;
    if (!reset_t1_session()) return 182;
    set_scan_card_t1_receive_capacity(true, 3 + 16 + 1, 6, 1606);
    set_scan_card_t1_apdu_mode(true, 40);
    dtv::internal::T1ApduResult cancelled_host_ifs_result{};
    std::vector<std::vector<unsigned char>> cancelled_host_ifs_output;
    const auto cancelled_host_ifs_code = run_t1_apdu(2, apdu_one,
        &cancelled_host_ifs_result, &cancelled_host_ifs_output, 1000,
        nullptr, nullptr, 0, 1);
    synthetic_bgt_clock.cancel_root = nullptr;
    synthetic_bgt_clock.cancel_on_delay = false;
    if (cancelled_host_ifs_code != DTV_INTERNAL_RESULT_CANCELLED ||
        !cancelled_host_ifs_result.response.empty() ||
        !cancelled_host_ifs_result.io.result_known ||
        cancelled_host_ifs_result.io.reply_complete ||
        cancelled_host_ifs_result.io.tx_bytes_done != 5 ||
        cancelled_host_ifs_output.size() != 1) return 183;
    if (!reset_t1_session()) return 184;
    set_scan_card_t1_receive_capacity(true, 3 + 16 + 1, 7, 1607);
    set_scan_card_t1_apdu_mode(true, 49);
    dtv::internal::T1ApduResult oversized_host_ifs_result{};
    std::vector<std::vector<unsigned char>> oversized_host_ifs_output;
    const auto oversized_host_ifs_code = run_t1_apdu(2, apdu_one,
        &oversized_host_ifs_result, &oversized_host_ifs_output);
    if (oversized_host_ifs_code == DTV_INTERNAL_RESULT_OK ||
        !oversized_host_ifs_result.response.empty() ||
        oversized_host_ifs_result.io.result_known ||
        oversized_host_ifs_result.io.reply_complete ||
        oversized_host_ifs_result.io.tx_bytes_done != 13 ||
        oversized_host_ifs_output.size() != 2) return 185;
    set_scan_card_t1_receive_capacity(false, 0, 0, 0);
    if (!reset_t1_session()) return 186;
    set_scan_card_t1_apdu_mode(true, 0);
    dtv::internal::T1ApduResult unknown_capacity_result{};
    std::vector<std::vector<unsigned char>> unknown_capacity_output;
    if (run_t1_apdu(2, apdu_one, &unknown_capacity_result,
            &unknown_capacity_output) != DTV_INTERNAL_RESULT_OK ||
        std::any_of(unknown_capacity_output.begin(), unknown_capacity_output.end(),
            [](const auto &sent) { return sent.size() >= 2 && sent[1] == 0xc1; }))
        return 187;
    if (!reset_t1_session()) return 188;
    set_scan_card_t1_receive_capacity(true, 3 + 32 + 1, 8, 1608);
    dtv::internal::T1ApduResult large_capacity_result{};
    std::vector<std::vector<unsigned char>> large_capacity_output;
    if (run_t1_apdu(2, apdu_one, &large_capacity_result,
            &large_capacity_output) != DTV_INTERNAL_RESULT_OK ||
        std::any_of(large_capacity_output.begin(), large_capacity_output.end(),
            [](const auto &sent) { return sent.size() >= 2 && sent[1] == 0xc1; }))
        return 189;
    if (!reset_t1_session()) return 190;
    set_scan_card_t1_receive_capacity(true, 4, 9, 1609);
    dtv::internal::T1ApduResult invalid_capacity_result{};
    std::vector<std::vector<unsigned char>> invalid_capacity_output;
    const auto invalid_capacity_code = run_t1_apdu(2, apdu_one,
        &invalid_capacity_result, &invalid_capacity_output);
    if (invalid_capacity_code == DTV_INTERNAL_RESULT_OK ||
        !invalid_capacity_result.response.empty() ||
        !invalid_capacity_result.io.result_known ||
        invalid_capacity_result.io.tx_bytes_done != 0 ||
        invalid_capacity_result.io.rx_bytes_done != 0 ||
        !invalid_capacity_output.empty()) return 191;
    set_scan_card_t1_receive_capacity(false, 0, 0, 0);
    const std::array<unsigned char, 1> negotiated_ifs{0x10};
    const auto ifs_response_frame = encode_t1_lrc_frame(0xe1, negotiated_ifs);
    std::array<unsigned char, 33> ifs_chained_command{};
    for (std::size_t index = 0; index < ifs_chained_command.size(); ++index)
        ifs_chained_command[index] = static_cast<unsigned char>(index + 1);
    const auto ifs_command_piece_one = std::span<const unsigned char>(ifs_chained_command).first(16);
    const auto ifs_command_piece_two = std::span<const unsigned char>(ifs_chained_command).subspan(16, 16);
    const auto ifs_command_piece_three = std::span<const unsigned char>(ifs_chained_command).subspan(32);
    const auto ifs_command_frame_one = encode_t1_lrc_frame(0x60, ifs_command_piece_one);
    const auto ifs_command_frame_two = encode_t1_lrc_frame(0x20, ifs_command_piece_two);
    const auto ifs_command_frame_three = encode_t1_lrc_frame(0x40, ifs_command_piece_three);
    set_scan_card_t1_bgt(2, 1000000, 18);
    for (const auto mode : {1u, 2u}) {
        if (!reset_t1_session()) return 120;
        set_scan_card_t1_apdu_mode(true, 30);
        dtv::internal::T1ApduResult ifs_result{};
        std::vector<std::vector<unsigned char>> ifs_output;
        std::vector<std::uint64_t> ifs_submit_times;
        std::vector<std::uint64_t> ifs_input_complete_times;
        const auto ifs_code = run_t1_apdu(mode, apdu_one, &ifs_result, &ifs_output, 1000,
            &ifs_submit_times, &ifs_input_complete_times);
        if (ifs_code != DTV_INTERNAL_RESULT_OK) return 121;
        if (ifs_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            ifs_result.io.tx_bytes_done != 13 || ifs_result.io.rx_bytes_done != 13 ||
            !ifs_result.io.reply_complete || !ifs_result.io.result_known) return 132;
        if (!same_apdu_frame_sent(mode, ifs_output, ifs_response_frame,
                negotiated_ifs, 1)) return 133;
        const auto ifs_header = mode == 2
            ? std::find(ifs_output.begin(), ifs_output.end(), ifs_response_frame)
            : std::find(ifs_output.begin(), ifs_output.end(),
                std::vector<unsigned char>{0x00, 0xe1, 0x01});
        const auto ifs_header_index = static_cast<std::size_t>(ifs_header - ifs_output.begin());
        if (ifs_header == ifs_output.end() || ifs_header_index >= ifs_submit_times.size() ||
            ifs_input_complete_times.empty() ||
            ifs_submit_times[ifs_header_index] < ifs_input_complete_times[0] + 22000000)
            return 134;
        dtv::internal::T1ApduResult ifs_chained_result{};
        std::vector<std::vector<unsigned char>> ifs_chained_output;
        const auto ifs_chained_code = run_t1_apdu(mode, ifs_chained_command,
            &ifs_chained_result, &ifs_chained_output);
        if (ifs_chained_code != DTV_INTERNAL_RESULT_OK) return 122;
        if (
            ifs_chained_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            ifs_chained_result.io.tx_bytes_done != 45 || ifs_chained_result.io.rx_bytes_done != 16 ||
            !ifs_chained_result.io.reply_complete || !ifs_chained_result.io.result_known ||
            !same_apdu_frame_sent(mode, ifs_chained_output, ifs_command_frame_one,
                ifs_command_piece_one, 1) ||
            !same_apdu_frame_sent(mode, ifs_chained_output, ifs_command_frame_two,
                ifs_command_piece_two, 1) ||
            !same_apdu_frame_sent(mode, ifs_chained_output, ifs_command_frame_three,
                ifs_command_piece_three, 1) ||
            scan_card_t1_apdu_ack_trace_value() != 2) return 152;
    }
    set_scan_card_t1_crc(true);
    if (!reset_t1_session()) return 123;
    set_scan_card_t1_apdu_mode(true, 30);
    dtv::internal::T1ApduResult ifs_crc_result{};
    std::vector<std::vector<unsigned char>> ifs_crc_output;
    const auto ifs_crc_response_frame = encode_t1_crc_frame(0xe1, negotiated_ifs);
    if (run_t1_apdu(2, apdu_one, &ifs_crc_result, &ifs_crc_output) !=
            DTV_INTERNAL_RESULT_OK ||
        !same_apdu_frame_sent(2, ifs_crc_output, ifs_crc_response_frame,
            negotiated_ifs, 1) || ifs_crc_result.io.tx_bytes_done != 15 ||
        ifs_crc_result.io.rx_bytes_done != 15) return 124;
    set_scan_card_t1_crc(false);
    for (const auto fault : {31u, 32u, 33u, 34u, 53u}) {
        if (!reset_t1_session()) return 125;
        set_scan_card_t1_apdu_mode(true, fault);
        dtv::internal::T1ApduResult invalid_ifs_result{};
        std::vector<std::vector<unsigned char>> invalid_ifs_output;
        const auto invalid_ifs_code = run_t1_apdu(2, apdu_one,
            &invalid_ifs_result, &invalid_ifs_output);
        if (invalid_ifs_code == DTV_INTERNAL_RESULT_OK ||
            !invalid_ifs_result.response.empty() || invalid_ifs_result.io.result_known ||
            invalid_ifs_result.io.reply_complete || invalid_ifs_output.size() != 1) {
            std::fprintf(stderr, "invalid IFS fault=%u code=%d transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
                fault, invalid_ifs_code, invalid_ifs_result.io.transport_error.code,
                invalid_ifs_result.response.size(), invalid_ifs_result.io.tx_bytes_done,
                invalid_ifs_result.io.rx_bytes_done, invalid_ifs_result.io.result_known,
                invalid_ifs_result.io.reply_complete, invalid_ifs_output.size());
            return 126;
        }
    }
    if (!reset_t1_session()) return 127;
    set_scan_card_t1_apdu_mode(true, 37);
    dtv::internal::T1ApduResult invalid_ifs_edc_result{};
    std::vector<std::vector<unsigned char>> invalid_ifs_edc_output;
    const auto invalid_ifs_edc_code = run_t1_apdu(1, apdu_one,
        &invalid_ifs_edc_result, &invalid_ifs_edc_output);
    if (invalid_ifs_edc_code != DTV_INTERNAL_RESULT_USB_IO ||
        !invalid_ifs_edc_result.response.empty() || invalid_ifs_edc_result.io.result_known ||
        invalid_ifs_edc_result.io.reply_complete ||
        invalid_ifs_edc_result.io.tx_bytes_done != 8 ||
        invalid_ifs_edc_result.io.rx_bytes_done != 5 ||
        std::find(invalid_ifs_edc_output.begin(), invalid_ifs_edc_output.end(),
            std::vector<unsigned char>{0x00, 0xe1, 0x01}) != invalid_ifs_edc_output.end()) {
        return 128;
    }
    if (!reset_t1_session()) return 129;
    set_scan_card_t1_apdu_mode(true, 30);
    dtv::internal::T1ApduResult partial_ifs_result{};
    std::vector<std::vector<unsigned char>> partial_ifs_output;
    const auto partial_ifs_code = run_t1_apdu(2, apdu_one, &partial_ifs_result,
        &partial_ifs_output, 1000, nullptr, nullptr, 2);
    if (partial_ifs_code != DTV_INTERNAL_RESULT_USB_IO ||
        !partial_ifs_result.response.empty() || partial_ifs_result.io.result_known ||
        partial_ifs_result.io.reply_complete || partial_ifs_result.io.tx_bytes_done != 10 ||
        partial_ifs_result.io.rx_bytes_done != 5 || partial_ifs_output.size() != 2)
        return 153;
    if (!reset_t1_session()) return 129;
    set_scan_card_t1_apdu_mode(true, 30);
    dtv::internal::T1ApduResult cancelled_ifs_result{};
    std::vector<std::vector<unsigned char>> cancelled_ifs_output;
    synthetic_bgt_clock.cancel_delay_calls.store(0, std::memory_order_release);
    synthetic_bgt_clock.cancel_delay_calls_at_arm.store(0, std::memory_order_release);
    synthetic_bgt_clock.cancel_arm_monotonic_ns.store(0, std::memory_order_release);
    synthetic_bgt_clock.cancel_arm_synthetic_ns.store(0, std::memory_order_release);
    synthetic_bgt_clock.cancel_trigger_monotonic_ns.store(0, std::memory_order_release);
    synthetic_bgt_clock.cancel_trigger_synthetic_ns.store(0, std::memory_order_release);
    synthetic_bgt_clock.cancel_trigger_attempts.store(0, std::memory_order_release);
    synthetic_bgt_clock.cancel_trigger_result.store(DTV_ERROR_INTERNAL, std::memory_order_release);
    const auto cancelled_ifs_started = std::chrono::steady_clock::now();
    const auto cancelled_ifs_code = run_t1_apdu(2, apdu_one, &cancelled_ifs_result,
        &cancelled_ifs_output, 1000, nullptr, nullptr, 0, 1);
    const auto cancelled_ifs_finished = std::chrono::steady_clock::now();
    const auto cancelled_ifs_cancel_pending = synthetic_bgt_clock.cancel_on_delay.load(
        std::memory_order_acquire);
    std::fprintf(stderr,
        "IFS cancel observed code=%d expected=%d tx=%llu rx=%llu response=%zu known=%u complete=%u transport=%d out=%zu delays=%llu delays_at_arm=%llu cancel_pending=%u cancel_armed=%u arm_ns=%llu trigger_attempts=%llu cancel_result=%d trigger_ns=%llu elapsed_ns=%llu synthetic_arm=%llu synthetic_trigger=%llu\n",
        cancelled_ifs_code, DTV_INTERNAL_RESULT_CANCELLED,
        static_cast<unsigned long long>(cancelled_ifs_result.io.tx_bytes_done),
        static_cast<unsigned long long>(cancelled_ifs_result.io.rx_bytes_done),
        cancelled_ifs_result.response.size(), cancelled_ifs_result.io.result_known,
        cancelled_ifs_result.io.reply_complete,
        cancelled_ifs_result.io.transport_error.code, cancelled_ifs_output.size(),
        static_cast<unsigned long long>(synthetic_bgt_clock.cancel_delay_calls.load(
            std::memory_order_acquire)),
        static_cast<unsigned long long>(synthetic_bgt_clock.cancel_delay_calls_at_arm.load(
            std::memory_order_acquire)), cancelled_ifs_cancel_pending,
        synthetic_bgt_clock.cancel_arm_monotonic_ns.load(std::memory_order_acquire) != 0,
        static_cast<unsigned long long>(synthetic_bgt_clock.cancel_arm_monotonic_ns.load(
            std::memory_order_acquire)),
        static_cast<unsigned long long>(synthetic_bgt_clock.cancel_trigger_attempts.load(
            std::memory_order_acquire)),
        synthetic_bgt_clock.cancel_trigger_result.load(std::memory_order_acquire),
        static_cast<unsigned long long>(synthetic_bgt_clock.cancel_trigger_monotonic_ns.load(
            std::memory_order_acquire)),
        static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            cancelled_ifs_finished - cancelled_ifs_started).count()),
        static_cast<unsigned long long>(synthetic_bgt_clock.cancel_arm_synthetic_ns.load(
            std::memory_order_acquire)),
        static_cast<unsigned long long>(synthetic_bgt_clock.cancel_trigger_synthetic_ns.load(
            std::memory_order_acquire)));
    synthetic_bgt_clock.cancel_root = nullptr;
    synthetic_bgt_clock.cancel_on_delay = false;
    if (cancelled_ifs_code != DTV_INTERNAL_RESULT_CANCELLED) {
        std::fprintf(stderr,
            "IFS cancel triage code=%d expected=%d tx=%llu rx=%llu response=%zu known=%u complete=%u transport=%d out=%zu delays=%llu cancel_armed=%u arm_ns=%llu trigger_attempts=%llu cancel_result=%d trigger_ns=%llu elapsed_ns=%llu synthetic_arm=%llu synthetic_trigger=%llu\n",
            cancelled_ifs_code,
            DTV_INTERNAL_RESULT_CANCELLED,
            static_cast<unsigned long long>(cancelled_ifs_result.io.tx_bytes_done),
            static_cast<unsigned long long>(cancelled_ifs_result.io.rx_bytes_done),
            cancelled_ifs_result.response.size(), cancelled_ifs_result.io.result_known,
            cancelled_ifs_result.io.reply_complete,
            cancelled_ifs_result.io.transport_error.code, cancelled_ifs_output.size(),
            static_cast<unsigned long long>(synthetic_bgt_clock.cancel_delay_calls.load(
                std::memory_order_acquire)),
            synthetic_bgt_clock.cancel_arm_monotonic_ns.load(std::memory_order_acquire) != 0,
            static_cast<unsigned long long>(synthetic_bgt_clock.cancel_arm_monotonic_ns.load(
                std::memory_order_acquire)),
            static_cast<unsigned long long>(synthetic_bgt_clock.cancel_trigger_attempts.load(
                std::memory_order_acquire)),
            synthetic_bgt_clock.cancel_trigger_result.load(std::memory_order_acquire),
            static_cast<unsigned long long>(synthetic_bgt_clock.cancel_trigger_monotonic_ns.load(
                std::memory_order_acquire)),
            static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                cancelled_ifs_finished - cancelled_ifs_started).count()),
            static_cast<unsigned long long>(synthetic_bgt_clock.cancel_arm_synthetic_ns.load(
                std::memory_order_acquire)),
            static_cast<unsigned long long>(synthetic_bgt_clock.cancel_trigger_synthetic_ns.load(
                std::memory_order_acquire)));
        for (std::size_t index = 0; index < cancelled_ifs_output.size(); ++index) {
            std::fprintf(stderr, "IFS cancel OUT[%zu] len=%zu:", index,
                cancelled_ifs_output[index].size());
            for (const auto byte : cancelled_ifs_output[index])
                std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        return 160 + cancelled_ifs_code;
    }
    if (
        !cancelled_ifs_result.response.empty() || cancelled_ifs_result.io.result_known ||
        cancelled_ifs_result.io.reply_complete || cancelled_ifs_result.io.tx_bytes_done != 8 ||
        cancelled_ifs_result.io.rx_bytes_done != 5 || cancelled_ifs_output.size() != 1)
        return 154;
    if (!reset_t1_session()) return 129;
    set_scan_card_t1_apdu_mode(true, 36);
    dtv::internal::T1ApduResult missing_ifs_i_result{};
    std::vector<std::vector<unsigned char>> missing_ifs_i_output;
    const auto missing_ifs_i_code = run_t1_apdu(2, apdu_one,
        &missing_ifs_i_result, &missing_ifs_i_output, 150);
    set_scan_card_transport_drop_reply(false);
    if (missing_ifs_i_code != DTV_INTERNAL_RESULT_TIMEOUT ||
        !missing_ifs_i_result.response.empty() || missing_ifs_i_result.io.result_known ||
        missing_ifs_i_result.io.reply_complete || missing_ifs_i_result.io.tx_bytes_done != 13 ||
        missing_ifs_i_result.io.rx_bytes_done != 5 || missing_ifs_i_output.size() != 2)
        return 130;
    if (!reset_t1_session()) return 131;
    std::array<unsigned char, 65> chained_command{};
    for (std::size_t index = 0; index < chained_command.size(); ++index)
        chained_command[index] = static_cast<unsigned char>(index + 1);
    const auto command_piece_one = std::span<const unsigned char>(chained_command).first(32);
    const auto command_piece_two = std::span<const unsigned char>(chained_command).subspan(32, 32);
    const auto command_piece_three = std::span<const unsigned char>(chained_command).subspan(64);
    const auto command_frame_one = encode_t1_lrc_frame(0x20, command_piece_one);
    const auto command_frame_two = encode_t1_lrc_frame(0x60, command_piece_two);
    const auto command_frame_three = encode_t1_lrc_frame(0x00, command_piece_three);
    set_scan_card_t1_bgt(2, 1000000, 18);
    for (const auto mode : {1u, 2u}) {
        if (!reset_t1_session()) return 76;
        set_scan_card_t1_apdu_mode(true, 22);
        dtv::internal::T1ApduResult command_chain_result{};
        std::vector<std::vector<unsigned char>> command_chain_output;
        std::vector<std::uint64_t> command_chain_submit_times;
        const auto command_chain_code = run_t1_apdu(mode, chained_command,
            &command_chain_result, &command_chain_output, 1000,
            &command_chain_submit_times);
        const auto find_frame_submit = [&](const std::vector<unsigned char> &frame) {
            const auto found = std::find_if(command_chain_output.begin(),
                command_chain_output.end(), [&](const auto &packet) {
                    return mode == 2 ? packet == frame
                        : packet == std::vector<unsigned char>{0x00, frame[1], frame[2]};
                });
            return found == command_chain_output.end()
                ? command_chain_output.size()
                : static_cast<std::size_t>(found - command_chain_output.begin());
        };
        const auto first_frame_submit = find_frame_submit(command_frame_one);
        const auto second_frame_submit = find_frame_submit(command_frame_two);
        const auto third_frame_submit = find_frame_submit(command_frame_three);
        if (command_chain_code != DTV_INTERNAL_RESULT_OK ||
            command_chain_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            command_chain_result.io.tx_bytes_done != 77 ||
            command_chain_result.io.rx_bytes_done != 16 ||
            !command_chain_result.io.reply_complete || !command_chain_result.io.result_known ||
            !same_apdu_frame_sent(mode, command_chain_output, command_frame_one,
                command_piece_one, 1) ||
            !same_apdu_frame_sent(mode, command_chain_output, command_frame_two,
                command_piece_two, 1) ||
            !same_apdu_frame_sent(mode, command_chain_output, command_frame_three,
                command_piece_three, 1) ||
            scan_card_t1_apdu_ack_count_value() != 2 ||
            scan_card_t1_apdu_ack_trace_value() != 1 || command_chain_submit_times.empty() ||
            command_chain_submit_times.size() != command_chain_output.size() ||
            first_frame_submit >= second_frame_submit ||
            second_frame_submit >= third_frame_submit ||
            command_chain_submit_times[second_frame_submit] <
                command_chain_submit_times[first_frame_submit] + 22000000 ||
            command_chain_submit_times[third_frame_submit] <
                command_chain_submit_times[second_frame_submit] + 22000000) {
            std::fprintf(stderr, "command chain mode=%u result=%d transport=%d/%u tx=%zu rx=%zu response=%zu output=%zu\n",
                mode, command_chain_code, command_chain_result.io.transport_error.code,
                command_chain_result.io.transport_error.detail,
                command_chain_result.io.tx_bytes_done, command_chain_result.io.rx_bytes_done,
                command_chain_result.response.size(),
                command_chain_output.size());
            for (const auto &packet : command_chain_output) {
                std::fprintf(stderr, " sent:");
                for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
                std::fprintf(stderr, "\n");
            }
            return 77;
        }
        dtv::internal::T1ApduResult following_command_result{};
        std::vector<std::vector<unsigned char>> following_command_output;
        if (run_t1_apdu(mode, apdu_one, &following_command_result,
                &following_command_output) != DTV_INTERNAL_RESULT_OK ||
            following_command_result.response !=
                std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            following_command_output.size() == 0 ||
            !same_apdu_frame_sent(mode, following_command_output,
                encode_t1_lrc_frame(0x40, apdu_one), apdu_one, 1)) return 78;
    }
    if (!reset_t1_session()) return 79;
    set_scan_card_t1_apdu_mode(true, 23);
    dtv::internal::T1ApduResult repeated_command_block_result{};
    std::vector<std::vector<unsigned char>> repeated_command_block_output;
    if (run_t1_apdu(2, chained_command, &repeated_command_block_result,
            &repeated_command_block_output) != DTV_INTERNAL_RESULT_OK ||
        repeated_command_block_result.response !=
            std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        repeated_command_block_result.io.tx_bytes_done != 113 ||
        repeated_command_block_result.io.rx_bytes_done != 20 ||
        !repeated_command_block_result.io.reply_complete ||
        !repeated_command_block_result.io.result_known ||
        scan_card_t1_apdu_ack_count_value() != 3 ||
        scan_card_t1_apdu_ack_trace_value() != 2 ||
        !same_apdu_frame_sent(2, repeated_command_block_output, command_frame_one,
            command_piece_one, 2) ||
        !same_apdu_frame_sent(2, repeated_command_block_output, command_frame_two,
            command_piece_two, 1) ||
        !same_apdu_frame_sent(2, repeated_command_block_output, command_frame_three,
            command_piece_three, 1)) {
        std::fprintf(stderr, "same N(R) command retry tx=%zu rx=%zu ack=%u/%u output=%zu\n",
            repeated_command_block_result.io.tx_bytes_done,
            repeated_command_block_result.io.rx_bytes_done,
            scan_card_t1_apdu_ack_count_value(), scan_card_t1_apdu_ack_trace_value(),
            repeated_command_block_output.size());
        for (const auto &packet : repeated_command_block_output) {
            std::fprintf(stderr, " sent:");
            for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr, "code=%d response=%zu known=%u complete=%u frames=%d/%d/%d\n",
            repeated_command_block_result.io.transport_error.code,
            repeated_command_block_result.response.size(),
            repeated_command_block_result.io.result_known,
            repeated_command_block_result.io.reply_complete,
            static_cast<int>(same_apdu_frame_sent(2, repeated_command_block_output,
                command_frame_one, command_piece_one, 2)),
            static_cast<int>(same_apdu_frame_sent(2, repeated_command_block_output,
                command_frame_two, command_piece_two, 1)),
            static_cast<int>(same_apdu_frame_sent(2, repeated_command_block_output,
                command_frame_three, command_piece_three, 1)));
        std::fprintf(stderr, " response bytes:");
        for (const auto byte : repeated_command_block_result.response)
            std::fprintf(stderr, " %02x", byte);
        std::fprintf(stderr, "\n");
        return 80;
    }
    for (const auto fault : {24u, 25u}) {
        if (!reset_t1_session()) return 81;
        set_scan_card_t1_apdu_mode(true, fault);
        dtv::internal::T1ApduResult invalid_command_chain_result{};
        std::vector<std::vector<unsigned char>> invalid_command_chain_output;
        if (run_t1_apdu(2, chained_command, &invalid_command_chain_result,
                &invalid_command_chain_output) != DTV_INTERNAL_RESULT_USB_IO ||
            !invalid_command_chain_result.response.empty() ||
            invalid_command_chain_result.io.tx_bytes_done != command_frame_one.size() ||
            invalid_command_chain_result.io.result_known ||
            invalid_command_chain_result.io.reply_complete ||
            !same_apdu_frame_sent(2, invalid_command_chain_output, command_frame_one,
                command_piece_one, 1)) return 82;
    }
    if (!reset_t1_session()) return 83;
    set_scan_card_t1_apdu_mode(true, 26);
    dtv::internal::T1ApduResult missing_command_ack_result{};
    std::vector<std::vector<unsigned char>> missing_command_ack_output;
    if (run_t1_apdu(2, chained_command, &missing_command_ack_result,
            &missing_command_ack_output, 100) != DTV_INTERNAL_RESULT_TIMEOUT ||
        !missing_command_ack_result.response.empty() ||
        missing_command_ack_result.io.tx_bytes_done != command_frame_one.size() ||
        missing_command_ack_result.io.rx_bytes_done != 0 ||
        missing_command_ack_result.io.result_known ||
        missing_command_ack_result.io.reply_complete ||
        !same_apdu_frame_sent(2, missing_command_ack_output, command_frame_one,
            command_piece_one, 1)) return 84;
    if (!reset_t1_session()) return 85;
    set_scan_card_t1_apdu_mode(true, 22);
    backend->actual_length_limit = 7;
    dtv::internal::T1ApduResult partial_command_out_result{};
    std::vector<std::vector<unsigned char>> partial_command_out;
    const auto partial_command_out_code = run_t1_apdu(2, chained_command,
        &partial_command_out_result, &partial_command_out);
    backend->actual_length_limit = (std::numeric_limits<std::size_t>::max)();
    if (partial_command_out_code != DTV_INTERNAL_RESULT_USB_IO ||
        !partial_command_out_result.response.empty() ||
        partial_command_out_result.io.tx_bytes_done == 0 ||
        partial_command_out_result.io.result_known ||
        partial_command_out_result.io.reply_complete ||
        partial_command_out.size() != 1) return 86;
    if (!reset_t1_session()) return 87;
    set_scan_card_t1_apdu_mode(true, 22);
    synthetic_bgt_clock.cancel_root = root;
    synthetic_bgt_clock.cancel_on_delay = true;
    dtv::internal::T1ApduResult cancelled_command_chain_result{};
    std::vector<std::vector<unsigned char>> cancelled_command_chain_output;
    const auto cancelled_command_chain_code = run_t1_apdu(2, chained_command,
        &cancelled_command_chain_result, &cancelled_command_chain_output, 30000);
    synthetic_bgt_clock.cancel_root = nullptr;
    if (cancelled_command_chain_code != DTV_INTERNAL_RESULT_CANCELLED ||
        !cancelled_command_chain_result.response.empty() ||
        cancelled_command_chain_result.io.tx_bytes_done != command_frame_one.size() ||
        cancelled_command_chain_result.io.rx_bytes_done != 4 ||
        cancelled_command_chain_result.io.result_known ||
        cancelled_command_chain_result.io.reply_complete ||
        !same_apdu_frame_sent(2, cancelled_command_chain_output, command_frame_one,
            command_piece_one, 1)) return 88;
    for (const auto mode : {1u, 2u}) {
        if (!reset_t1_session()) return 63;
        set_scan_card_t1_apdu_mode(true, 18);
        dtv::internal::T1ApduResult chained_result{};
        std::vector<std::vector<unsigned char>> chained_output;
        std::vector<std::uint64_t> chained_submit_times;
        if (run_t1_apdu(mode, apdu_one, &chained_result, &chained_output, 1000,
                &chained_submit_times) != DTV_INTERNAL_RESULT_OK ||
            chained_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            chained_result.io.tx_bytes_done != 12 || chained_result.io.rx_bytes_done != 12 ||
            !chained_result.io.reply_complete || !chained_result.io.result_known ||
            !same_r_frame_sent(mode, chained_output, normal_r_one_frame, 1)) {
            std::fprintf(stderr, "response chain mode=%u transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u outputs=%zu\n",
                mode, chained_result.io.transport_error.code, chained_result.response.size(),
                chained_result.io.tx_bytes_done, chained_result.io.rx_bytes_done,
                chained_result.io.result_known, chained_result.io.reply_complete,
                chained_output.size());
            for (const auto byte : chained_result.response)
                std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
            for (const auto &packet : chained_output) {
                std::fprintf(stderr, " sent:");
                for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
                std::fprintf(stderr, "\n");
            }
            return 64;
        }
        if (chained_submit_times.empty()) return 65;
        const auto guarded_submit = std::find_if(chained_submit_times.begin(),
            chained_submit_times.end(), [&](const auto time) {
                return time >= chained_submit_times.front() + 22000000;
            });
        if (guarded_submit == chained_submit_times.end()) return 65;
        set_scan_card_t1_apdu_mode(true, 20);
        dtv::internal::T1ApduResult following_result{};
        std::vector<std::vector<unsigned char>> following_output;
        if (run_t1_apdu(mode, apdu_two, &following_result, &following_output) !=
                DTV_INTERNAL_RESULT_OK ||
            following_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
            following_output.empty() ||
            (mode == 1 && following_output[0] != std::vector<unsigned char>{0x00, 0x40, 0x04}) ||
            (mode == 2 && following_output[0] != std::vector<unsigned char>{0x00, 0x40, 0x04,
                0x00, 0xca, 0x00, 0x00, 0x8e})) return 66;
    }
    if (!reset_t1_session()) return 69;
    set_scan_card_t1_apdu_mode(true, 21);
    dtv::internal::T1ApduResult incomplete_chain_result{};
    std::vector<std::vector<unsigned char>> incomplete_chain_output;
    if (run_t1_apdu(2, apdu_one, &incomplete_chain_result, &incomplete_chain_output) !=
            DTV_INTERNAL_RESULT_UNSUPPORTED ||
        !incomplete_chain_result.response.empty() || incomplete_chain_result.io.result_known ||
        incomplete_chain_result.io.reply_complete ||
        std::count(incomplete_chain_output.begin(), incomplete_chain_output.end(),
            std::vector<unsigned char>{0x00, 0x00, 0x04, 0x00, 0xa4, 0x04, 0x00, 0xa4}) != 1)
        return 70;
    set_scan_card_t1_crc(true);
    if (!reset_t1_session()) return 71;
    set_scan_card_t1_apdu_mode(true, 18);
    dtv::internal::T1ApduResult crc_chain_result{};
    std::vector<std::vector<unsigned char>> crc_chain_output;
    const auto crc_chain_code = run_t1_apdu(2, apdu_one, &crc_chain_result, &crc_chain_output);
    if (crc_chain_code !=
            DTV_INTERNAL_RESULT_OK ||
        crc_chain_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        crc_chain_result.io.tx_bytes_done != 14 || crc_chain_result.io.rx_bytes_done != 14 ||
        !same_r_frame_sent(2, crc_chain_output,
            std::vector<unsigned char>{0x00, 0x90, 0x00, 0x91, 0xdf}, 1)) {
        std::fprintf(stderr, "CRC response chain result=%d transport=%d response=%zu tx=%zu rx=%zu output=%zu\n",
            crc_chain_code, crc_chain_result.io.transport_error.code,
            crc_chain_result.response.size(), crc_chain_result.io.tx_bytes_done,
            crc_chain_result.io.rx_bytes_done, crc_chain_output.size());
        for (const auto &packet : crc_chain_output) {
            std::fprintf(stderr, " sent:");
            for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        return 72;
    }
    set_scan_card_t1_crc(false);
    if (!reset_t1_session()) return 67;
    set_scan_card_t1_apdu_mode(true, 19);
    dtv::internal::T1ApduResult three_block_result{};
    std::vector<std::vector<unsigned char>> three_block_output;
    std::vector<std::uint64_t> three_block_submit_times;
    if (run_t1_apdu(2, apdu_one, &three_block_result, &three_block_output, 1000,
            &three_block_submit_times) != DTV_INTERNAL_RESULT_OK ||
        three_block_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        three_block_result.io.tx_bytes_done != 16 || three_block_result.io.rx_bytes_done != 16 ||
        std::count(three_block_output.begin(), three_block_output.end(), normal_r_one_frame) != 1 ||
        std::count(three_block_output.begin(), three_block_output.end(),
            std::vector<unsigned char>{0x00, 0x80, 0x00, 0x80}) != 1 ||
        three_block_submit_times.size() < 3 ||
        three_block_submit_times[1] < three_block_submit_times[0] + 22000000 ||
        three_block_submit_times[2] < three_block_submit_times[1] + 22000000) return 68;
    const std::vector<unsigned char> first_apdu_frame{
        0x00, 0x00, 0x04, 0x00, 0xa4, 0x04, 0x00, 0xa4};
    const std::vector<unsigned char> second_apdu_frame{
        0x00, 0x40, 0x04, 0x00, 0xca, 0x00, 0x00, 0x8e};
    const std::vector<unsigned char> sequence_one_r_frame{0x00, 0x92, 0x00, 0x92};
    for (const auto mode : {1u, 2u}) {
        if (!reset_t1_session()) return 32;
        set_scan_card_t1_apdu_mode(true, mode == 1 ? 5 : 8);
        dtv::internal::T1ApduResult retried_result{};
        std::vector<std::vector<unsigned char>> retried_output;
        const auto retry_code = run_t1_apdu(mode, apdu_one, &retried_result, &retried_output);
        if (retry_code != DTV_INTERNAL_RESULT_OK ||
            retried_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            retried_result.io.tx_bytes_done != 16 || retried_result.io.rx_bytes_done != 12 ||
            !retried_result.io.reply_complete || !retried_result.io.result_known ||
            !same_apdu_frame_sent(mode, retried_output, first_apdu_frame, apdu_one, 2)) {
            std::fprintf(stderr, "R retry mode=%u result=%d transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u outputs=%zu\n",
                mode, retry_code, retried_result.io.transport_error.code,
                retried_result.response.size(), retried_result.io.tx_bytes_done,
                retried_result.io.rx_bytes_done, retried_result.io.result_known,
                retried_result.io.reply_complete, retried_output.size());
            for (const auto &packet : retried_output) {
                std::fprintf(stderr, "retry packet:");
                for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
                std::fprintf(stderr, "\n");
            }
            return 33;
        }
        const auto next_mode = mode == 1 ? 2u : 1u;
        set_scan_card_t1_apdu_mode(true, 0);
        dtv::internal::T1ApduResult next_result{};
        std::vector<std::vector<unsigned char>> next_output;
        const auto next_code = run_t1_apdu(next_mode, apdu_two, &next_result, &next_output);
        if (next_code != DTV_INTERNAL_RESULT_OK ||
            next_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
            next_result.io.tx_bytes_done != 8 || !next_result.io.reply_complete ||
            !next_result.io.result_known ||
            !same_apdu_frame_sent(next_mode, next_output, second_apdu_frame, apdu_two, 1)) {
            std::fprintf(stderr, "after R retry mode=%u next=%u result=%d transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u outputs=%zu\n",
                mode, next_mode, next_code, next_result.io.transport_error.code,
                next_result.response.size(),
                next_result.io.tx_bytes_done, next_result.io.rx_bytes_done,
                next_result.io.result_known, next_result.io.reply_complete, next_output.size());
            for (const auto &packet : next_output) {
                std::fprintf(stderr, "next packet:");
                for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
                std::fprintf(stderr, "\n");
            }
            return 34;
        }
    }
    if (!reset_t1_session()) return 43;
    set_scan_card_t1_apdu_mode(true, 0);
    dtv::internal::T1ApduResult sequence_zero_result{};
    std::vector<std::vector<unsigned char>> sequence_zero_output;
    if (run_t1_apdu(1, apdu_one, &sequence_zero_result, &sequence_zero_output) !=
            DTV_INTERNAL_RESULT_OK ||
        !same_apdu_frame_sent(1, sequence_zero_output, first_apdu_frame, apdu_one, 1)) return 44;

    for (const auto mode : {1u, 2u}) {
        if (!reset_t1_session()) return 47;
        set_scan_card_t1_apdu_mode(true, 0);
        dtv::internal::T1ApduResult first_result{};
        if (run_t1_apdu(mode, apdu_one, &first_result) != DTV_INTERNAL_RESULT_OK) return 48;
        set_scan_card_t1_apdu_mode(true, 13);
        dtv::internal::T1ApduResult recovered_result{};
        std::vector<std::vector<unsigned char>> recovered_output;
        const auto recovered_code = run_t1_apdu(mode, apdu_two, &recovered_result,
            &recovered_output);
        if (recovered_code != DTV_INTERNAL_RESULT_OK ||
            recovered_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
            recovered_result.io.tx_bytes_done != second_apdu_frame.size() +
                sequence_one_r_frame.size() ||
            recovered_result.io.rx_bytes_done != 16 ||
            !recovered_result.io.reply_complete || !recovered_result.io.result_known ||
            !same_apdu_frame_sent(mode, recovered_output, second_apdu_frame, apdu_two, 1) ||
            !same_r_frame_sent(mode, recovered_output, sequence_one_r_frame, 1)) {
            std::fprintf(stderr, "R recovery mode=%u result=%d transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u outputs=%zu\n",
                mode, recovered_code, recovered_result.io.transport_error.code,
                recovered_result.response.size(), recovered_result.io.tx_bytes_done,
                recovered_result.io.rx_bytes_done, recovered_result.io.result_known,
                recovered_result.io.reply_complete, recovered_output.size());
            return 49;
        }
        set_scan_card_t1_apdu_mode(true, 0);
        dtv::internal::T1ApduResult following_result{};
        std::vector<std::vector<unsigned char>> following_output;
        if (run_t1_apdu(mode, apdu_one, &following_result, &following_output) !=
                DTV_INTERNAL_RESULT_OK ||
            following_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
            !same_apdu_frame_sent(mode, following_output, first_apdu_frame, apdu_one, 1))
            return 50;
    }

    if (!reset_t1_session()) return 56;
    set_scan_card_t1_apdu_mode(true, 0);
    dtv::internal::T1ApduResult before_r_timeout_result{};
    if (run_t1_apdu(2, apdu_one, &before_r_timeout_result) != DTV_INTERNAL_RESULT_OK)
        return 57;
    set_scan_card_t1_apdu_mode(true, 15);
    dtv::internal::T1ApduResult r_timeout_result{};
    std::vector<std::vector<unsigned char>> r_timeout_output;
    const auto r_timeout_code = run_t1_apdu(2, apdu_two, &r_timeout_result,
        &r_timeout_output, 100);
    if (r_timeout_code != DTV_INTERNAL_RESULT_TIMEOUT || !r_timeout_result.response.empty() ||
        r_timeout_result.io.tx_bytes_done != second_apdu_frame.size() +
            sequence_one_r_frame.size() || r_timeout_result.io.result_known ||
        r_timeout_result.io.reply_complete ||
        !same_apdu_frame_sent(2, r_timeout_output, second_apdu_frame, apdu_two, 1) ||
        !same_r_frame_sent(2, r_timeout_output, sequence_one_r_frame, 1)) {
        std::fprintf(stderr, "R timeout code=%d tx=%zu rx=%zu known=%u complete=%u output=%zu\n",
            r_timeout_code, r_timeout_result.io.tx_bytes_done, r_timeout_result.io.rx_bytes_done,
            r_timeout_result.io.result_known, r_timeout_result.io.reply_complete,
            r_timeout_output.size());
        for (const auto &packet : r_timeout_output) {
            std::fprintf(stderr, " sent:");
            for (const auto byte : packet) std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        return 58;
    }

    if (!reset_t1_session()) return 51;
    set_scan_card_t1_apdu_mode(true, 0);
    dtv::internal::T1ApduResult before_repeat_result{};
    if (run_t1_apdu(2, apdu_one, &before_repeat_result) != DTV_INTERNAL_RESULT_OK) return 52;
    set_scan_card_t1_apdu_mode(true, 14);
    dtv::internal::T1ApduResult repeated_i_result{};
    std::vector<std::vector<unsigned char>> repeated_i_output;
    const auto repeated_i_code = run_t1_apdu(2, apdu_two, &repeated_i_result,
        &repeated_i_output);
    if (repeated_i_code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
        !repeated_i_result.response.empty() ||
        repeated_i_result.io.tx_bytes_done != second_apdu_frame.size() +
            sequence_one_r_frame.size() * 3 ||
        repeated_i_result.io.rx_bytes_done != 32 || repeated_i_result.io.reply_complete ||
        repeated_i_result.io.result_known ||
        !same_apdu_frame_sent(2, repeated_i_output, second_apdu_frame, apdu_two, 1) ||
        !same_r_frame_sent(2, repeated_i_output, sequence_one_r_frame, 3)) return 53;
    if (!reset_t1_session()) return 54;
    set_scan_card_t1_apdu_mode(true, 0);
    dtv::internal::T1ApduResult after_repeat_reset_result{};
    if (run_t1_apdu(2, apdu_one, &after_repeat_reset_result) != DTV_INTERNAL_RESULT_OK ||
        after_repeat_reset_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00})
        return 55;

    set_scan_card_t1_apdu_mode(true, 5);
    dtv::internal::T1ApduResult sequence_one_retry_result{};
    std::vector<std::vector<unsigned char>> sequence_one_retry_output;
    const auto sequence_one_retry_code = run_t1_apdu(2, apdu_two,
        &sequence_one_retry_result, &sequence_one_retry_output);
    if (sequence_one_retry_code != DTV_INTERNAL_RESULT_OK ||
        sequence_one_retry_result.response != std::vector<unsigned char>{0x33, 0x44, 0x90, 0x00} ||
        sequence_one_retry_result.io.tx_bytes_done != 16 ||
        sequence_one_retry_result.io.rx_bytes_done != 12 ||
        !sequence_one_retry_result.io.reply_complete || !sequence_one_retry_result.io.result_known ||
        !same_apdu_frame_sent(2, sequence_one_retry_output, second_apdu_frame, apdu_two, 2)) {
        std::fprintf(stderr, "sequence one R result=%d transport=%d response=%zu tx=%zu rx=%zu known=%u complete=%u output=%zu pcb=%02x\n",
            sequence_one_retry_code, sequence_one_retry_result.io.transport_error.code,
            sequence_one_retry_result.response.size(), sequence_one_retry_result.io.tx_bytes_done,
            sequence_one_retry_result.io.rx_bytes_done, sequence_one_retry_result.io.result_known,
            sequence_one_retry_result.io.reply_complete,
            sequence_one_retry_output.size(),
            sequence_one_retry_output.empty() || sequence_one_retry_output[0].size() < 2
                ? 0 : sequence_one_retry_output[0][1]);
        return 45;
    }
    set_scan_card_t1_apdu_mode(true, 0);
    dtv::internal::T1ApduResult sequence_wrap_result{};
    std::vector<std::vector<unsigned char>> sequence_wrap_output;
    if (run_t1_apdu(1, apdu_one, &sequence_wrap_result, &sequence_wrap_output) !=
            DTV_INTERNAL_RESULT_OK ||
        sequence_wrap_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        !same_apdu_frame_sent(1, sequence_wrap_output, first_apdu_frame, apdu_one, 1)) return 46;
    for (const auto fault : {6u, 7u, 10u, 11u}) {
        if (!reset_t1_session()) return 35;
        set_scan_card_t1_apdu_mode(true, fault);
        dtv::internal::T1ApduResult invalid_r_result{};
        std::vector<std::vector<unsigned char>> invalid_r_output;
        if (run_t1_apdu(2, apdu_one, &invalid_r_result, &invalid_r_output) !=
                DTV_INTERNAL_RESULT_USB_IO || !invalid_r_result.response.empty() ||
            invalid_r_result.io.result_known || invalid_r_result.io.reply_complete ||
            invalid_r_output.size() != 1) return 36;
    }
    if (!reset_t1_session()) return 37;
    set_scan_card_t1_apdu_mode(true, 9);
    dtv::internal::T1ApduResult repeated_r_result{};
    std::vector<std::vector<unsigned char>> repeated_r_output;
    if (run_t1_apdu(2, apdu_one, &repeated_r_result, &repeated_r_output) !=
            DTV_INTERNAL_RESULT_UNSUPPORTED || !repeated_r_result.response.empty() ||
        repeated_r_result.io.tx_bytes_done != 24 || repeated_r_result.io.rx_bytes_done != 12 ||
        repeated_r_result.io.reply_complete || repeated_r_result.io.result_known ||
        repeated_r_output.size() != 3 || repeated_r_output[0] != first_apdu_frame ||
        repeated_r_output[1] != first_apdu_frame || repeated_r_output[2] != first_apdu_frame)
        return 38;
    if (!reset_t1_session()) return 39;
    set_scan_card_t1_apdu_mode(true, 5);
    set_scan_card_transport_drop_reply(true);
    dtv::internal::T1ApduResult missing_r_result{};
    std::vector<std::vector<unsigned char>> missing_r_output;
    const auto missing_r_code = run_t1_apdu(2, apdu_one, &missing_r_result,
        &missing_r_output, 100);
    set_scan_card_transport_drop_reply(false);
    if (missing_r_code != DTV_INTERNAL_RESULT_TIMEOUT || !missing_r_result.response.empty() ||
        missing_r_result.io.tx_bytes_done != first_apdu_frame.size() ||
        missing_r_result.io.rx_bytes_done != 0 || missing_r_result.io.result_known ||
        missing_r_result.io.reply_complete || missing_r_output.size() != 1 ||
        missing_r_output[0] != first_apdu_frame) return 40;
    if (!reset_t1_session()) return 41;
    set_scan_card_t1_apdu_mode(true, 12);
    dtv::internal::T1ApduResult missing_retry_result{};
    std::vector<std::vector<unsigned char>> missing_retry_output;
    const auto missing_retry_code = run_t1_apdu(2, apdu_one, &missing_retry_result,
        &missing_retry_output, 100);
    if (missing_retry_code != DTV_INTERNAL_RESULT_TIMEOUT ||
        !missing_retry_result.response.empty() ||
        missing_retry_result.io.tx_bytes_done != first_apdu_frame.size() * 2 ||
        missing_retry_result.io.rx_bytes_done != 4 || missing_retry_result.io.result_known ||
        missing_retry_result.io.reply_complete || missing_retry_output.size() != 2 ||
        missing_retry_output[0] != first_apdu_frame ||
        missing_retry_output[1] != first_apdu_frame) return 42;
    for (const auto fault : {3u, 4u}) {
        if (!reset_t1_session()) return 32;
        set_scan_card_t1_apdu_mode(true, fault);
        dtv::internal::T1ApduResult unsupported_result{};
        if (run_t1_apdu(2, apdu_one, &unsupported_result) != DTV_INTERNAL_RESULT_UNSUPPORTED ||
            !unsupported_result.response.empty() || unsupported_result.io.result_known ||
            unsupported_result.io.reply_complete) return 33;
    }
    if (!reset_t1_session()) return 34;
    set_scan_card_t1_apdu_mode(true, 0);
    dtv::internal::T1ApduResult reset_apdu_result{};
    std::vector<std::vector<unsigned char>> reset_apdu_output;
    if (run_t1_apdu(2, apdu_one, &reset_apdu_result, &reset_apdu_output) !=
            DTV_INTERNAL_RESULT_OK ||
        reset_apdu_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        reset_apdu_output.size() != 1 ||
        reset_apdu_output[0] != std::vector<unsigned char>{0x00, 0x00, 0x04, 0x00,
            0xa4, 0x04, 0x00, 0xa4}) return 35;
    set_scan_card_t1_apdu_mode(false, 0);
    set_scan_card_t1_exchange_mode(0);
    set_scan_card_t1_fault(1);
    dtv::internal::T1BlockResult incomplete_result{};
    if (run_t1_exchange(1, &incomplete_result) != DTV_INTERNAL_RESULT_USB_IO ||
        incomplete_result.io.reply_complete || incomplete_result.io.result_known)
        return 19;
    set_scan_card_t1_fault(2);
    dtv::internal::T1BlockResult extra_result{};
    if (run_t1_exchange(1, &extra_result) != DTV_INTERNAL_RESULT_USB_IO ||
        extra_result.io.reply_complete || extra_result.io.result_known)
        return 20;
    set_scan_card_t1_fault(3);
    dtv::internal::T1BlockResult bad_edc_result{};
    if (run_t1_exchange(1, &bad_edc_result) != DTV_INTERNAL_RESULT_USB_IO ||
        bad_edc_result.io.reply_complete || bad_edc_result.io.result_known)
        return 21;
    set_scan_card_t1_fault(0);
    set_scan_card_t1_exchange_mode(1);
    set_scan_card_transport_drop_reply(true);
    const auto t1_timeout_outputs = backend->endpoint_submission_count(0x02);
    dtv::internal::T1BlockResult timed_out_t1{};
    std::vector<std::vector<unsigned char>> timed_out_t1_output;
    std::vector<std::uint64_t> timed_out_t1_output_submit_ns;
    std::uint64_t timed_out_t1_caller_started_ns = 0;
    std::uint64_t timed_out_t1_caller_finished_ns = 0;
    const auto timed_out_t1_started = std::chrono::steady_clock::now();
    const auto timed_out_t1_code = run_t1_exchange(1, &timed_out_t1, 200,
        &timed_out_t1_output, &timed_out_t1_caller_started_ns,
        &timed_out_t1_caller_finished_ns, &timed_out_t1_output_submit_ns);
    const auto timed_out_t1_finished = std::chrono::steady_clock::now();
    const auto timed_out_t1_output_delta =
        backend->endpoint_submission_count(0x02) - t1_timeout_outputs;
    std::fprintf(stderr,
        "common deadline observed code=%d expected=%d tx=%llu rx=%llu known=%u complete=%u transport=%d out_delta=%llu caller_elapsed_ns=%llu driver_elapsed_ns=%llu budget_ms=200 packets=%zu\n",
        timed_out_t1_code, DTV_INTERNAL_RESULT_TIMEOUT,
        static_cast<unsigned long long>(timed_out_t1.io.tx_bytes_done),
        static_cast<unsigned long long>(timed_out_t1.io.rx_bytes_done),
        timed_out_t1.io.result_known, timed_out_t1.io.reply_complete,
        timed_out_t1.io.transport_error.code,
        static_cast<unsigned long long>(timed_out_t1_output_delta),
        static_cast<unsigned long long>(timed_out_t1_caller_finished_ns -
            timed_out_t1_caller_started_ns),
        static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            timed_out_t1_finished - timed_out_t1_started).count()),
        timed_out_t1_output.size());
    std::fprintf(stderr, "common deadline OUT offsets_us=");
    for (const auto submitted_ns : timed_out_t1_output_submit_ns)
        std::fprintf(stderr, " %lld",
            static_cast<long long>(submitted_ns - timed_out_t1_caller_started_ns) / 1000);
    std::fprintf(stderr, "\n");
    if (timed_out_t1_code != DTV_INTERNAL_RESULT_TIMEOUT ||
        timed_out_t1.io.reply_complete || timed_out_t1.io.result_known ||
        timed_out_t1_output_delta != 4) {
        std::fprintf(stderr,
            "common deadline triage code=%d expected=%d tx=%llu rx=%llu known=%u complete=%u transport=%d out_delta=%llu caller_elapsed_ns=%llu driver_elapsed_ns=%llu budget_ms=200 packets=%zu\n",
            timed_out_t1_code, DTV_INTERNAL_RESULT_TIMEOUT,
            static_cast<unsigned long long>(timed_out_t1.io.tx_bytes_done),
            static_cast<unsigned long long>(timed_out_t1.io.rx_bytes_done),
            timed_out_t1.io.result_known, timed_out_t1.io.reply_complete,
            timed_out_t1.io.transport_error.code,
            static_cast<unsigned long long>(timed_out_t1_output_delta),
            static_cast<unsigned long long>(timed_out_t1_caller_finished_ns -
                timed_out_t1_caller_started_ns),
            static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                timed_out_t1_finished - timed_out_t1_started).count()),
            timed_out_t1_output.size());
        for (std::size_t index = 0; index < timed_out_t1_output.size(); ++index) {
            std::fprintf(stderr, "common deadline OUT[%zu] len=%zu:", index,
                timed_out_t1_output[index].size());
            for (const auto byte : timed_out_t1_output[index])
                std::fprintf(stderr, " %02x", byte);
            std::fprintf(stderr, "\n");
        }
        return 27;
    }
    set_scan_card_transport_drop_reply(false);
    set_scan_card_t1_exchange_mode(0);
    set_scan_card_t1_crc(true);
    std::atomic<std::int32_t> crc_reset_result{DTV_ERROR_INTERNAL};
    std::thread crc_resetting([&] {
        const std::uint8_t *atr = nullptr;
        std::uint64_t atr_size = 0;
        dtv_error reset_error{};
        reset_error.struct_size = sizeof(reset_error);
        crc_reset_result.store(dtv_reset_card(root, nullptr, &atr, &atr_size, &reset_error),
                               std::memory_order_release);
    });
    const auto crc_reset_output = backend->endpoint_submission_count(0x02) + 1;
    if (!backend->wait_for_endpoint_submissions(0x02, crc_reset_output)) {
        dtv_cancel_enclosure(root, &error);
        crc_resetting.join();
        return 22;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x02);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_enclosure(root, &error);
        crc_resetting.join();
        return 23;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_enclosure(root, &error);
        crc_resetting.join();
        return 24;
    }
    crc_resetting.join();
    if (crc_reset_result.load(std::memory_order_acquire) != DTV_ERROR_OK) return 25;
    const auto crc_byte_result = run_t1_exchange(1, &t1_results[0]);
    const auto crc_block_result = crc_byte_result == DTV_INTERNAL_RESULT_OK
        ? run_t1_exchange(2, &t1_results[1]) : DTV_INTERNAL_RESULT_INTERNAL;
    if (crc_byte_result != DTV_INTERNAL_RESULT_OK ||
        t1_results[0].inf != std::vector<unsigned char>{0x90, 0x00} ||
        t1_results[1].inf != t1_results[0].inf ||
        t1_results[0].raw_block_length != 7 || t1_results[1].raw_block_length != 7 ||
        t1_results[0].io.tx_bytes_done != 8 || t1_results[1].io.tx_bytes_done != 8 ||
        t1_results[0].io.rx_bytes_done != 7 || t1_results[1].io.rx_bytes_done != 7 ||
        !t1_results[0].io.reply_complete || !t1_results[1].io.reply_complete ||
        crc_block_result != DTV_INTERNAL_RESULT_OK) {
        std::fprintf(stderr, "CRC exchange byte=%d block=%d byte_len=%zu block_len=%zu\n",
            crc_byte_result, crc_block_result, t1_results[0].raw_block_length,
            t1_results[1].raw_block_length);
        return 26;
    }
    set_scan_card_t1_exchange_mode(0);
    set_scan_card_t1_crc(false);

    auto *const interleave_devices = dtv_get_devices(root);
    auto *const interleave_receivers = interleave_devices == nullptr
        ? nullptr : dtv_get_receivers(interleave_devices[0]);
    dtv_stream *interleave_stream = nullptr;
    if (interleave_receivers == nullptr || interleave_receivers[1] == nullptr ||
        dtv_open_stream(interleave_receivers[1], nullptr, &interleave_stream, &error) !=
            DTV_ERROR_OK || interleave_stream == nullptr) return 28;
    std::thread interleave_exchange;
    std::thread interleave_read;
    std::thread interleave_reset;
    const auto cleanup_interleave = [&] {
        dtv_cancel_stream(interleave_stream, &error);
        dtv_cancel_enclosure(root, &error);
        if (interleave_read.joinable()) interleave_read.join();
        if (interleave_exchange.joinable()) interleave_exchange.join();
        if (interleave_reset.joinable()) interleave_reset.join();
        (void)dtv_close_stream(interleave_stream, nullptr, &error);
        dtv_release_stream(interleave_stream);
    };
    const dtv_call_options event_call{sizeof(dtv_call_options), 2};
    bool event_queue_empty = false;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        dtv_event event{};
        event.struct_size = sizeof(event);
        const auto event_result = dtv_wait_event(&event, &event_call, &error);
        if (event_result == DTV_ERROR_TIMEOUT) {
            event_queue_empty = true;
            break;
        }
        if (event_result != DTV_ERROR_OK || event.kind == DTV_EVENT_CARD_INSERTED ||
            event.kind == DTV_EVENT_CARD_REMOVED) {
            cleanup_interleave();
            return 47;
        }
    }
    if (!event_queue_empty) {
        cleanup_interleave();
        return 48;
    }
    const auto initial_card_generation = dtv_get_card_generation(root);
    set_scan_card_t1_crc(true);
    set_scan_card_t1_stale_reply(true);
    set_scan_card_t1_apdu_mode(true, 0);
    set_scan_card_t1_exchange_mode(2);
    set_scan_card_interleave_phase(1);
    const auto interleave_output_count = backend->endpoint_submission_count(0x02);
    const auto interleave_output_start = backend->output_snapshot().size();
    std::atomic<bool> interleave_exchange_done{false};
    dtv_internal_result interleave_exchange_result{};
    dtv::internal::T1ApduResult stale_t1_result{};
    interleave_exchange = std::thread([&] {
        interleave_exchange_result = dtv::internal::exchange_public_t1_apdu(root,
            apdu_one, dtv_timeout{5000, 0},
            &stale_t1_result);
        interleave_exchange_done.store(true, std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, interleave_output_count + 1)) {
        cleanup_interleave();
        return 29;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x02);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        cleanup_interleave();
        return 30;
    }
    const auto notification_submissions = backend->endpoint_submission_count(0x81);
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1) ||
        !backend->wait_for_endpoint_submissions(0x81, notification_submissions + 1)) {
        cleanup_interleave();
        return 31;
    }
    const auto absent_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dtv_get_card_state(root) != DTV_CARD_ABSENT &&
           std::chrono::steady_clock::now() < absent_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    dtv_event removed_event{};
    removed_event.struct_size = sizeof(removed_event);
    const dtv_call_options card_event_call{sizeof(dtv_call_options), 1000};
    const auto removed_state = dtv_get_card_state(root);
    const auto removed_generation = dtv_get_card_generation(root);
    const auto removed_result = dtv_wait_event(&removed_event, &card_event_call, &error);
    if (removed_state != DTV_CARD_ABSENT ||
        removed_generation != initial_card_generation + 1 ||
        removed_result != DTV_ERROR_OK || removed_event.kind != DTV_EVENT_CARD_REMOVED ||
        interleave_exchange_done.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "T1 interleave notification state=%u generation=%llu initial=%llu event=%d/%u done=%d phase=%u\n",
            removed_state, static_cast<unsigned long long>(removed_generation),
            static_cast<unsigned long long>(initial_card_generation), removed_result,
            removed_event.kind, interleave_exchange_done.load(std::memory_order_acquire),
            scan_card_interleave_phase_value());
        cleanup_interleave();
        return 32;
    }
    std::array<std::uint8_t, 188> interleave_packet{};
    std::uint64_t interleave_read_size = 0;
    std::atomic<std::int32_t> interleave_read_result{DTV_ERROR_INTERNAL};
    dtv_error interleave_read_error{};
    interleave_read_error.struct_size = sizeof(interleave_read_error);
    interleave_read = std::thread([&] {
        const dtv_call_options read_call{sizeof(dtv_call_options), 5000};
        interleave_read_result.store(dtv_read_stream(interleave_stream, interleave_packet.data(),
            interleave_packet.size(), &read_call, &interleave_read_size, &interleave_read_error),
            std::memory_order_release);
    });
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x82);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        cleanup_interleave();
        return 33;
    }
    interleave_read.join();
    if (interleave_read_result.load(std::memory_order_acquire) != DTV_ERROR_OK ||
        interleave_read_size != interleave_packet.size() || interleave_packet[0] != 0x47 ||
        interleave_packet[1] != 0x81 ||
        interleave_exchange_done.load(std::memory_order_acquire) ||
        dtv_get_card_state(root) != DTV_CARD_ABSENT ||
        scan_card_interleave_phase_value() != 3) {
        cleanup_interleave();
        return 34;
    }
    const auto stale_reply_submissions = backend->endpoint_submission_count(0x81);
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1) ||
        !backend->wait_for_endpoint_submissions(0x81, stale_reply_submissions + 1)) {
        cleanup_interleave();
        return 36;
    }
    interleave_exchange.join();
    const auto interleave_outputs = backend->output_snapshot();
    const std::array<unsigned char, 7> interleave_header{0x00, 0x00, 0x04, 0x00,
        0xa4, 0x04, 0x00};
    if (interleave_exchange_result.code != DTV_INTERNAL_RESULT_NOT_READY ||
        !stale_t1_result.response.empty() ||
        stale_t1_result.io.tx_bytes_done != 9 || stale_t1_result.io.rx_bytes_done != 0 ||
        stale_t1_result.io.reply_complete || stale_t1_result.io.result_known ||
        stale_t1_result.io.transport_error.code != DTV_INTERNAL_RESULT_NOT_READY ||
        backend->endpoint_submission_count(0x02) != interleave_output_count + 1 ||
        interleave_outputs.size() != interleave_output_start + 1 ||
        interleave_outputs.back().size() != 9 ||
        !std::equal(interleave_header.begin(), interleave_header.end(),
            interleave_outputs.back().begin()) ||
        scan_card_interleave_phase_value() != 4) {
        std::fprintf(stderr, "T1 stale result=%d transport=%d inf=%zu tx=%zu rx=%zu known=%d complete=%d out=%u/%zu phase=%u\n",
            interleave_exchange_result.code, stale_t1_result.io.transport_error.code,
            stale_t1_result.response.size(),
            stale_t1_result.io.tx_bytes_done, stale_t1_result.io.rx_bytes_done,
            stale_t1_result.io.result_known, stale_t1_result.io.reply_complete,
            backend->endpoint_submission_count(0x02) - interleave_output_count,
            interleave_outputs.size() - interleave_output_start,
            scan_card_interleave_phase_value());
        cleanup_interleave();
        return 37;
    }
    const auto removed_apdu_output_count = backend->output_snapshot().size();
    const std::uint8_t *removed_apdu_response = reinterpret_cast<const std::uint8_t *>(
        std::uintptr_t{1});
    std::uint64_t removed_apdu_response_size = 1;
    dtv_error removed_apdu_error{};
    removed_apdu_error.struct_size = sizeof(removed_apdu_error);
    if (dtv_transmit_apdu(root, apdu_one.data(), apdu_one.size(), nullptr,
            &removed_apdu_response, &removed_apdu_response_size, &removed_apdu_error) !=
            DTV_ERROR_CARD_REMOVED || removed_apdu_error.code != DTV_ERROR_CARD_REMOVED ||
        removed_apdu_response != nullptr || removed_apdu_response_size != 0 ||
        backend->output_snapshot().size() != removed_apdu_output_count) {
        cleanup_interleave();
        return 41;
    }
    const auto present_submissions = backend->endpoint_submission_count(0x81);
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1) ||
        !backend->wait_for_endpoint_submissions(0x81, present_submissions + 1)) {
        cleanup_interleave();
        return 39;
    }
    const auto present_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dtv_get_card_state(root) != DTV_CARD_PRESENT &&
           std::chrono::steady_clock::now() < present_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    dtv_event inserted_event{};
    inserted_event.struct_size = sizeof(inserted_event);
    if (dtv_get_card_state(root) != DTV_CARD_PRESENT ||
        dtv_get_card_generation(root) != initial_card_generation + 2 ||
        dtv_wait_event(&inserted_event, &card_event_call, &error) != DTV_ERROR_OK ||
        inserted_event.kind != DTV_EVENT_CARD_INSERTED) {
        cleanup_interleave();
        return 40;
    }
    set_scan_card_interleave_phase(0);
    set_scan_card_t1_stale_reply(false);
    set_scan_card_t1_exchange_mode(0);
    set_scan_card_t1_crc(true);
    const auto next_session_reset_output = backend->endpoint_submission_count(0x02) + 1;
    std::atomic<std::int32_t> next_session_reset_result{DTV_ERROR_INTERNAL};
    dtv_error next_session_reset_error{};
    next_session_reset_error.struct_size = sizeof(next_session_reset_error);
    interleave_reset = std::thread([&] {
        const std::uint8_t *atr = nullptr;
        std::uint64_t atr_size = 0;
        next_session_reset_result.store(dtv_reset_card(root, nullptr, &atr, &atr_size,
            &next_session_reset_error), std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, next_session_reset_output)) {
        cleanup_interleave();
        return 41;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x02);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        cleanup_interleave();
        return 42;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        cleanup_interleave();
        return 44;
    }
    interleave_reset.join();
    if (next_session_reset_result.load(std::memory_order_acquire) != DTV_ERROR_OK ||
        next_session_reset_error.code != DTV_ERROR_OK || dtv_get_card_state(root) != DTV_CARD_READY) {
        cleanup_interleave();
        return 45;
    }
    dtv::internal::T1ApduResult next_session_result{};
    std::vector<std::vector<unsigned char>> next_session_outputs;
    if (run_t1_apdu(2, apdu_one, &next_session_result, &next_session_outputs) !=
            DTV_INTERNAL_RESULT_OK ||
        next_session_result.response != std::vector<unsigned char>{0x11, 0x22, 0x90, 0x00} ||
        next_session_result.io.tx_bytes_done != 9 ||
        next_session_result.io.rx_bytes_done != 9 ||
        !next_session_result.io.reply_complete || !next_session_result.io.result_known ||
        next_session_outputs.size() != 1 ||
        next_session_outputs[0].size() != 9 ||
        !std::equal(interleave_header.begin(), interleave_header.end(),
            next_session_outputs[0].begin()) ||
        dtv_close_stream(interleave_stream, nullptr, &error) != DTV_CLOSE_CLOSED) {
        dtv_release_stream(interleave_stream);
        return 46;
    }
    dtv_release_stream(interleave_stream);
    set_scan_card_t1_crc(false);

    set_scan_card_t1_bgt(2, 500000000, 19);
    synthetic_bgt_clock.cancel_root = root;
    synthetic_bgt_clock.cancel_on_delay = true;
    dtv::internal::set_public_card_bgt_clock_for_test(&synthetic_clock);
    const auto before_bgt_cancel = backend->endpoint_submission_count(0x02);
    dtv::internal::T1ApduResult bgt_cancel_result{};
    const auto bgt_cancel_result_code = dtv::internal::exchange_public_t1_apdu(root, apdu_one,
        dtv_timeout{30000, 0}, &bgt_cancel_result);
    synthetic_bgt_clock.cancel_root = nullptr;
    if (bgt_cancel_result_code.code != DTV_INTERNAL_RESULT_CANCELLED ||
        backend->endpoint_submission_count(0x02) != before_bgt_cancel ||
        bgt_cancel_result.io.tx_bytes_done != 0 || !bgt_cancel_result.io.result_known) {
        std::fprintf(stderr, "BGT cancel result=%d outputs=%u/%u tx=%zu known=%u\n",
            bgt_cancel_result_code.code, backend->endpoint_submission_count(0x02),
            before_bgt_cancel, bgt_cancel_result.io.tx_bytes_done,
            bgt_cancel_result.io.result_known);
        return 60;
    }
    synthetic_bgt_clock.now_ns.fetch_add(20000000000, std::memory_order_acq_rel);
    set_scan_card_t1_bgt(2, 1000000, 20);
    if (!reset_t1_session()) return 73;
    set_scan_card_t1_apdu_mode(true, 18);
    set_scan_card_t1_bgt(2, 500000000, 21);
    synthetic_bgt_clock.cancel_root = root;
    synthetic_bgt_clock.cancel_on_delay = true;
    const auto before_chained_bgt_cancel = backend->endpoint_submission_count(0x02);
    dtv::internal::T1ApduResult chained_bgt_cancel_result{};
    std::vector<std::vector<unsigned char>> chained_bgt_cancel_output;
    const auto chained_bgt_cancel_code = run_t1_apdu(2, apdu_one,
        &chained_bgt_cancel_result, &chained_bgt_cancel_output, 30000);
    synthetic_bgt_clock.cancel_root = nullptr;
    if (chained_bgt_cancel_code != DTV_INTERNAL_RESULT_CANCELLED ||
        backend->endpoint_submission_count(0x02) != before_chained_bgt_cancel + 1 ||
        chained_bgt_cancel_result.io.tx_bytes_done != 8 ||
        chained_bgt_cancel_result.io.rx_bytes_done != 6 ||
        chained_bgt_cancel_result.io.result_known ||
        chained_bgt_cancel_result.io.reply_complete ||
        !chained_bgt_cancel_result.response.empty() ||
        chained_bgt_cancel_output != std::vector<std::vector<unsigned char>>{{
            0x00, 0x00, 0x04, 0x00, 0xa4, 0x04, 0x00, 0xa4}}) {
        std::fprintf(stderr, "chained BGT cancel result=%d outputs=%u/%u tx=%zu rx=%zu known=%u complete=%u response=%zu packets=%zu\n",
            chained_bgt_cancel_code, backend->endpoint_submission_count(0x02),
            before_chained_bgt_cancel, chained_bgt_cancel_result.io.tx_bytes_done,
            chained_bgt_cancel_result.io.rx_bytes_done, chained_bgt_cancel_result.io.result_known,
            chained_bgt_cancel_result.io.reply_complete,
            chained_bgt_cancel_result.response.size(), chained_bgt_cancel_output.size());
        return 74;
    }
    set_scan_card_t1_crc(true);
    set_scan_card_t1_apdu_mode(true, 0);
    if (!reset_t1_session()) return 75;
    set_scan_card_t1_bgt(2, 1000000, 22);

    set_scan_card_transport_drop_reply(true);
    reply.clear();
    const auto output_count = backend->endpoint_submission_count(0x02);
    if (!exchange(dtv::spi::CardByteWrite{byte_chunk}, &reply, &outcome, 0,
            dtv_timeout{40, 0})) return 12;
    set_scan_card_transport_drop_reply(false);
    if (outcome.result_known || outcome.reply_complete || outcome.tx_bytes_done != byte_chunk.size() ||
        outcome.rx_bytes_done != 0 || outcome.transport_error.code != DTV_INTERNAL_RESULT_TIMEOUT ||
        backend->endpoint_submission_count(0x02) != output_count + 1) return 13;

    set_scan_card_transport_drop_reply(true);
    dtv_internal_result close_operation_result{};
    dtv::internal::T1ApduResult close_t1_result{};
    set_scan_card_t1_exchange_mode(1);
    std::thread close_operation([&] {
        close_operation_result = dtv::internal::exchange_public_t1_apdu(root,
            apdu_one, dtv_timeout{5000, 0},
            &close_t1_result);
    });
    auto close_output = backend->endpoint_submission_count(0x02) + 1;
    for (unsigned step = 0; step < 4; ++step) {
        if (!backend->wait_for_endpoint_submissions(0x02, close_output)) {
            dtv_cancel_enclosure(root, &error);
            close_operation.join();
            return 14;
        }
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(callbacks + 1)) {
            dtv_cancel_enclosure(root, &error);
            close_operation.join();
            return 15;
        }
        if (step < 3) {
            callbacks = backend->callback_count_snapshot();
            backend->complete_endpoint(0x81);
            if (!backend->wait_for_callbacks(callbacks + 1)) {
                dtv_cancel_enclosure(root, &error);
                close_operation.join();
                return 16;
            }
        }
        ++close_output;
    }
    const dtv_call_options close_call{sizeof(dtv_call_options), 1};
    const auto close_state = dtv_close_enclosure(root, &close_call, &error);
    if (close_state == DTV_CLOSE_PENDING) {
        const auto callbacks_before = backend->callback_count_snapshot();
        const auto pending_transfers = backend->pending_transfer_count();
        backend->complete_endpoint(0x81, 2);
        backend->complete_endpoint(0x82, 2);
        if (!backend->wait_for_callbacks(callbacks_before + pending_transfers)) return 16;
    } else if (close_state != DTV_CLOSE_CLOSED) {
        return 17;
    }
    close_operation.join();
    set_scan_card_transport_drop_reply(false);
    if (close_operation_result.code != DTV_INTERNAL_RESULT_CANCELLED ||
        !close_t1_result.response.empty() ||
        close_t1_result.io.result_known || close_t1_result.io.reply_complete ||
        close_t1_result.io.tx_bytes_done != 9 || close_t1_result.io.rx_bytes_done != 0) {
        std::fprintf(stderr, "card close result=%d known=%d complete=%d tx=%zu rx=%zu\n",
            close_operation_result.code, close_t1_result.io.result_known,
            close_t1_result.io.reply_complete, close_t1_result.io.tx_bytes_done,
            close_t1_result.io.rx_bytes_done);
        return 18;
    }
    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto final_close_state = dtv_get_enclosure_close_state(root, &error);
    while (final_close_state == DTV_CLOSE_PENDING &&
           std::chrono::steady_clock::now() < close_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        final_close_state = dtv_get_enclosure_close_state(root, &error);
    }
    if (final_close_state != DTV_CLOSE_CLOSED) return 19;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    dtv::internal::set_public_discovery_batch_for_test({});
    set_scan_public_open_case(0);
    return 0;
}

int public_partial_cwt_close()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    set_scan_public_open_case(18);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 921), snapshot(context, 4, 922)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::atomic<std::int32_t> open_result{DTV_ERROR_INTERNAL};
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] {
        open_result.store(dtv_open_enclosure(root, nullptr, &open_error),
                          std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x81, 1) ||
        !backend->wait_for_endpoint_submissions(0x82, 1)) {
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 3;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result.load(std::memory_order_acquire) != DTV_ERROR_OK) return 4;

    set_scan_card_t1_crc(false);
    set_scan_card_t1_exchange_mode(1);
    set_scan_card_t1_bgt(2, 36000000, 2200);
    set_scan_card_t1_bwt_mode(9);
    const auto reset_output = backend->endpoint_submission_count(0x02) + 1;
    std::atomic<std::int32_t> reset_result{DTV_ERROR_INTERNAL};
    std::thread resetting([&] {
        const std::uint8_t *atr = nullptr;
        std::uint64_t atr_size = 0;
        dtv_error reset_error{};
        reset_error.struct_size = sizeof(reset_error);
        reset_result.store(dtv_reset_card(root, nullptr, &atr, &atr_size, &reset_error),
                           std::memory_order_release);
    });
    if (!backend->wait_for_endpoint_submissions(0x02, reset_output)) {
        dtv_cancel_enclosure(root, &error);
        resetting.join();
        return 5;
    }
    auto callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x02);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_enclosure(root, &error);
        resetting.join();
        return 6;
    }
    callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x81);
    if (!backend->wait_for_callbacks(callbacks + 1)) {
        dtv_cancel_enclosure(root, &error);
        resetting.join();
        return 7;
    }
    resetting.join();
    if (reset_result.load(std::memory_order_acquire) != DTV_ERROR_OK) return 8;
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    struct SystemClockGate {
        std::atomic<bool> armed{true};
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
    } gate;
    const auto delay_hook = [](void *user) {
        auto *state = static_cast<SystemClockGate *>(user);
        if (!scan_card_t1_cwt_silence_delivered_value() ||
            !state->armed.exchange(false, std::memory_order_acq_rel)) return;
        state->entered.store(true, std::memory_order_release);
        while (!state->release.load(std::memory_order_acquire))
            std::this_thread::yield();
    };
    const std::array<unsigned char, 4> command{0x00, 0xa4, 0x04, 0x00};
    const auto expected_i_header = std::vector<unsigned char>{0x00, 0x00, 0x04};
    const auto expected_i_data = std::vector<unsigned char>(command.begin(), command.end());
    std::uint8_t expected_i_lrc = 0;
    for (const auto byte : expected_i_header) expected_i_lrc ^= byte;
    for (const auto byte : expected_i_data) expected_i_lrc ^= byte;
    const auto expected_r_header = std::vector<unsigned char>{0x00, 0x82, 0x00};
    std::uint8_t expected_r_lrc = 0;
    for (const auto byte : expected_r_header) expected_r_lrc ^= byte;
    const auto output_start = backend->output_snapshot().size();
    const std::uint8_t *response_data = reinterpret_cast<const std::uint8_t *>(
        std::uintptr_t{1});
    std::uint64_t response_size = 1;
    std::atomic<std::int32_t> api_code{DTV_ERROR_INTERNAL};
    std::atomic<std::int32_t> api_error_code{DTV_ERROR_INTERNAL};
    std::atomic<bool> done{false};
    set_scan_card_t1_apdu_mode(true, 50);
    set_scan_card_t1_cwt_fault(101);
    dtv::internal::set_system_clock_delay_hook_for_test(delay_hook, &gate);
    std::thread caller([&] {
        dtv_error apdu_error{};
        apdu_error.struct_size = sizeof(apdu_error);
        api_code.store(dtv_transmit_apdu(root, command.data(), command.size(), nullptr,
            &response_data, &response_size, &apdu_error), std::memory_order_release);
        api_error_code.store(apdu_error.code, std::memory_order_release);
        done.store(true, std::memory_order_release);
    });
    auto next_out = backend->endpoint_submission_count(0x02) + 1;
    for (unsigned step = 0; step < 16 && !done.load(std::memory_order_acquire) &&
         !scan_card_t1_cwt_silence_delivered_value(); ++step) {
        if (!backend->wait_for_endpoint_submissions(0x02, next_out)) break;
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        if (!backend->wait_for_callbacks(callbacks + 1)) break;
        callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        if (!backend->wait_for_callbacks(callbacks + 1)) break;
        ++next_out;
    }
    const auto gate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!gate.entered.load(std::memory_order_acquire) &&
           !done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < gate_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!scan_card_t1_cwt_silence_delivered_value() ||
        !gate.entered.load(std::memory_order_acquire) || done.load(std::memory_order_acquire)) {
        gate.release.store(true, std::memory_order_release);
        dtv::internal::set_system_clock_delay_hook_for_test(nullptr, nullptr);
        if (!done.load(std::memory_order_acquire)) dtv_cancel_enclosure(root, &error);
        caller.join();
        (void)dtv_close_enclosure(root, nullptr, &error);
        dtv_release_enclosure(root);
        dtv_drop_enclosure(root);
        dtv::internal::set_public_discovery_batch_for_test({});
        set_scan_public_open_case(0);
        return 9;
    }
    const dtv_call_options short_close{sizeof(dtv_call_options), 1};
    const auto close_state = dtv_close_enclosure(root, &short_close, &error);
    gate.release.store(true, std::memory_order_release);
    const auto caller_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < caller_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    caller.join();
    dtv::internal::set_system_clock_delay_hook_for_test(nullptr, nullptr);
    set_scan_card_t1_cwt_fault(0);
    const auto all_output = backend->output_snapshot();
    std::vector<std::vector<unsigned char>> output(
        all_output.begin() + static_cast<std::ptrdiff_t>(output_start), all_output.end());
    std::size_t command_count = 0;
    for (std::size_t index = 0; index + 2 < output.size(); ++index) {
        if (output[index] == expected_i_header && output[index + 1] == expected_i_data &&
            output[index + 2] == std::vector<unsigned char>{expected_i_lrc})
            ++command_count;
    }
    std::size_t r_frame_count = 0;
    for (std::size_t index = 0; index + 1 < output.size(); ++index) {
        if (output[index] == expected_r_header && output[index + 1] ==
            std::vector<unsigned char>{expected_r_lrc}) ++r_frame_count;
    }
    const auto final_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto final_state = dtv_get_enclosure_close_state(root, &error);
    while (final_state == DTV_CLOSE_PENDING &&
           std::chrono::steady_clock::now() < final_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        final_state = dtv_get_enclosure_close_state(root, &error);
    }
    const bool passed = close_state == DTV_CLOSE_PENDING &&
        api_code.load(std::memory_order_acquire) == DTV_ERROR_CANCELLED &&
        api_error_code.load(std::memory_order_acquire) == DTV_ERROR_CANCELLED &&
        response_data == nullptr && response_size == 0 && command_count == 1 &&
        r_frame_count == 0 && final_state == DTV_CLOSE_CLOSED;
    std::fprintf(stderr,
        "cwt-close-gated evidence=%d close=%d api=%d error=%d ptr_null=%d size=%llu outputs=%zu I_frames=%zu R_frames=%zu final=%d\n",
        scan_card_t1_cwt_silence_delivered_value() ? 1 : 0,
        static_cast<int>(close_state), api_code.load(), api_error_code.load(),
        response_data == nullptr ? 1 : 0,
        static_cast<unsigned long long>(response_size), output.size(), command_count,
        r_frame_count, static_cast<int>(final_state));
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    dtv::internal::set_public_discovery_batch_for_test({});
    set_scan_public_open_case(0);
    return passed ? 0 : 10;
}

int public_open_operation_admission()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->actual_length_limit = 7;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    set_scan_public_open_case(11);
    dtv::internal::CompleteGroupBatch batch;
    static std::uint64_t snapshot_token = 900;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, snapshot_token++), snapshot(context, 4, snapshot_token++)},
            &batch).code != DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    auto *const *devices = dtv_get_devices(root);
    auto *const *receivers = devices == nullptr ? nullptr : dtv_get_receivers(devices[0]);
    if (receivers == nullptr || receivers[0] == nullptr || receivers[1] == nullptr ||
        receivers[2] == nullptr) return 2;
    const auto receiver_a = dtv_get_index(receivers[0]);
    const auto receiver_b = dtv_get_index(receivers[1]);
    const auto receiver_d = dtv_get_index(receivers[2]);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(2)) {
        backend->complete_endpoint(0x81);
        backend->complete_endpoint(0x82);
        opening.join();
        return 3;
    }
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    opening.join();
    if (open_result != DTV_ERROR_OK || !backend->wait_for_submissions(2)) return 4;

    const auto make_arguments = [](std::initializer_list<unsigned char> keys,
                                   unsigned char transaction,
                                   std::initializer_list<unsigned char> payload) {
        std::vector<unsigned char> arguments{static_cast<unsigned char>(keys.size()), 0, 0,
                                             transaction};
        auto key = keys.begin();
        if (key != keys.end()) arguments[1] = *key++;
        if (key != keys.end()) arguments[2] = *key;
        arguments.insert(arguments.end(), payload.begin(), payload.end());
        return arguments;
    };
    const auto invoke = [&](std::uint32_t operation,
                            dtv::spi::PublicOperationTargetKind target_kind,
                            std::uint64_t target_id, const std::vector<unsigned char> &arguments,
                            dtv_timeout timeout, std::span<unsigned char> reply,
                            std::size_t *reply_size) {
        const dtv::spi::PublicOperationRequest request{operation, target_kind, target_id,
            std::span<const unsigned char>(arguments.data(), arguments.size())};
        return dtv::internal::run_public_operation_for_test(root, request, timeout, reply,
                                                            reply_size);
    };
    std::array<unsigned char, 32> reply{};
    std::size_t reply_size = 0;
    const auto baseline_out = backend->endpoint_submission_count(0x02);
    const auto valid_arguments = make_arguments({42}, 200, {'V'});
    auto result = invoke(99, dtv::spi::PublicOperationTargetKind::receiver, 1,
        valid_arguments, dtv_timeout{100, 0}, reply, &reply_size);
    if (result.code != DTV_INTERNAL_RESULT_UNSUPPORTED) return 5;
    result = invoke(2, dtv::spi::PublicOperationTargetKind::receiver, 1,
        std::vector<unsigned char>{1, 42}, dtv_timeout{100, 0}, reply, &reply_size);
    if (result.code != DTV_INTERNAL_RESULT_NOT_READY) return 6;
    result = invoke(1, dtv::spi::PublicOperationTargetKind::receiver, 1,
        std::vector<unsigned char>{1, 42}, dtv_timeout{100, 0}, reply, &reply_size);
    if (result.code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT) return 7;
    result = invoke(1, dtv::spi::PublicOperationTargetKind::receiver, 1,
        valid_arguments, dtv_timeout{100, 0}, {}, &reply_size);
    if (result.code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
        backend->endpoint_submission_count(0x02) != baseline_out) return 8;
    dtv::internal::set_public_operation_allocation_failure_for_test(true);
    result = invoke(1, dtv::spi::PublicOperationTargetKind::receiver, 1,
        valid_arguments, dtv_timeout{100, 0}, reply, &reply_size);
    if (result.code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
        backend->endpoint_submission_count(0x02) != baseline_out) return 9;

    const auto session = scan_shared_open_session();
    std::array<std::uint64_t, 14> filler_tokens{};
    for (std::size_t index = 0; index < filler_tokens.size(); ++index) {
        result = session.register_reply(session.user, 7, 300 + index,
            scan_shared_generation(0), &filler_tokens[index]);
        if (result.code != DTV_INTERNAL_RESULT_OK) return 10;
    }
    result = invoke(1, dtv::spi::PublicOperationTargetKind::receiver, 1,
        make_arguments({42}, 201, {'F'}), dtv_timeout{100, 0}, reply, &reply_size);
    if (result.code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
        backend->endpoint_submission_count(0x02) != baseline_out) return 11;
    for (const auto token : filler_tokens) {
        if (session.abandon_reply(session.user, token).code != DTV_INTERNAL_RESULT_OK) return 12;
    }

    const auto send_out = [&](unsigned count) {
        if (!backend->wait_for_endpoint_submissions(0x02, count)) return false;
        const auto callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x02);
        return backend->wait_for_callbacks(callbacks + 1);
    };
    const auto send_reply = [&](unsigned char transaction) {
        set_scan_shared_reply_enabled(true);
        set_scan_shared_transaction_override(transaction);
        const auto packets = scan_shared_packet_count();
        const auto callbacks = backend->callback_count_snapshot();
        backend->complete_endpoint(0x81);
        const bool completed = backend->wait_for_callbacks(callbacks + 1);
        for (unsigned attempt = 0; completed && attempt < 2000 &&
             scan_shared_packet_count() == packets; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto after = scan_shared_packet_count();
        if (!completed || after != packets + 1) {
            std::fprintf(stderr, "operation reply %u: callback=%d packets=%u->%u\n",
                static_cast<unsigned>(transaction), completed ? 1 : 0, packets, after);
            return false;
        }
        return true;
    };
    std::array<unsigned char, 32> a_reply{}, b_reply{}, c_reply{}, d_reply{};
    std::size_t a_size = 0, b_size = 0, c_size = 0, d_size = 0;
    std::atomic<std::int32_t> a_result{DTV_ERROR_INTERNAL}, b_result{DTV_ERROR_INTERNAL},
        c_result{DTV_ERROR_INTERNAL}, d_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> a_done{false}, b_done{false}, c_done{false}, d_done{false};
    auto start = [&](std::uint32_t operation, dtv::spi::PublicOperationTargetKind target_kind,
                     std::uint64_t target_id, std::vector<unsigned char> arguments,
                     dtv_timeout timeout, std::array<unsigned char, 32> &output,
                     std::size_t &output_size, std::atomic<std::int32_t> &out_result,
                     std::atomic<bool> &done) {
        return std::thread([&, operation, target_kind, target_id,
                            arguments = std::move(arguments), timeout] {
            const auto call_result = invoke(operation, target_kind, target_id, arguments,
                                            timeout, output, &output_size);
            out_result.store(call_result.code, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });
    };
    auto a = start(1, dtv::spi::PublicOperationTargetKind::receiver, receiver_a,
        make_arguments({42, 43}, 201, {'A', '1'}), dtv_timeout{1000, 0},
        a_reply, a_size, a_result, a_done);
    if (!send_out(baseline_out + 1)) { a.join(); return 12; }
    auto b = start(1, dtv::spi::PublicOperationTargetKind::receiver, receiver_b,
        make_arguments({43}, 202, {'B'}), dtv_timeout{1000, 0},
        b_reply, b_size, b_result, b_done);
    auto c = start(1, dtv::spi::PublicOperationTargetKind::card, 1,
        make_arguments({44}, 203, {'C'}), dtv_timeout{1000, 0},
        c_reply, c_size, c_result, c_done);
    if (!send_out(baseline_out + 2) ||
        backend->endpoint_submission_count(0x02) != baseline_out + 2) {
        b.join(); c.join(); a.join(); return 13;
    }
    if (!send_reply(203)) { b.join(); c.join(); a.join(); return 14; }
    c.join();
    if (c_result.load() != DTV_INTERNAL_RESULT_OK || c_size != 7) { b.join(); a.join(); return 15; }
    auto d = start(1, dtv::spi::PublicOperationTargetKind::receiver, receiver_d,
        make_arguments({45}, 204, {'D'}), dtv_timeout{1000, 0},
        d_reply, d_size, d_result, d_done);
    if (!send_out(baseline_out + 3) || !send_reply(204)) {
        b.join(); d.join(); a.join(); return 16;
    }
    d.join();
    if (d_result.load() != DTV_INTERNAL_RESULT_OK || d_size != 7) { b.join(); a.join(); return 17; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (backend->endpoint_submission_count(0x02) != baseline_out + 3) {
        b.join(); a.join(); return 18;
    }
    if (!send_reply(201)) { b.join(); a.join(); return 19; }
    a.join();
    if (a_result.load() != DTV_INTERNAL_RESULT_OK || a_size != 7) { b.join(); return 20; }
    if (!send_out(baseline_out + 4) || !send_reply(202)) { b.join(); return 21; }
    b.join();
    if (b_result.load() != DTV_INTERNAL_RESULT_OK || b_size != 7) return 22;
    const auto output_payloads = backend->output_snapshot();
    if (output_payloads.size() < 4 ||
        output_payloads[0] != std::vector<unsigned char>({'A', '1'}) ||
        output_payloads[1] != std::vector<unsigned char>({'C'}) ||
        output_payloads[2] != std::vector<unsigned char>({'D'}) ||
        output_payloads[3] != std::vector<unsigned char>({'B'})) return 23;

    std::array<unsigned char, 32> timed_reply{};
    std::size_t timed_size = 0;
    std::atomic<std::int32_t> timed_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> timed_done{false};
    auto timed = start(1, dtv::spi::PublicOperationTargetKind::receiver, 3,
        make_arguments({46}, 205, {'T'}), dtv_timeout{20, 0},
        timed_reply, timed_size, timed_result, timed_done);
    if (!send_out(baseline_out + 5)) { timed.join(); return 24; }
    timed.join();
    if (timed_result.load() != DTV_INTERNAL_RESULT_TIMEOUT) return 25;
    std::array<unsigned char, 32> late_reply{};
    std::size_t late_size = 0;
    std::atomic<std::int32_t> late_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> late_done{false};
    auto late = start(1, dtv::spi::PublicOperationTargetKind::receiver, 4,
        make_arguments({47}, 206, {'L'}), dtv_timeout{1000, 0},
        late_reply, late_size, late_result, late_done);
    if (!send_out(baseline_out + 6) || !send_reply(205)) { late.join(); return 26; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (late_done.load(std::memory_order_acquire)) { late.join(); return 27; }
    if (!send_reply(206)) { late.join(); return 28; }
    late.join();
    if (late_result.load() != DTV_INTERNAL_RESULT_OK || late_size != 7) {
        std::fprintf(stderr, "late operation result=%d size=%zu\n", late_result.load(), late_size);
        return 29;
    }

    backend->fail_next_submit();
    std::array<unsigned char, 32> failed_reply{};
    std::size_t failed_size = 0;
    std::atomic<std::int32_t> failed_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> failed_done{false};
    auto failed = start(1, dtv::spi::PublicOperationTargetKind::receiver, 5,
        make_arguments({48}, 207, {'X'}), dtv_timeout{1000, 0},
        failed_reply, failed_size, failed_result, failed_done);
    failed.join();
    if (failed_result.load() != DTV_INTERNAL_RESULT_USB_IO) return 30;
    std::array<unsigned char, 32> reuse_reply{};
    std::size_t reuse_size = 0;
    std::atomic<std::int32_t> reuse_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> reuse_done{false};
    auto reuse = start(1, dtv::spi::PublicOperationTargetKind::receiver, 5,
        make_arguments({48}, 208, {'R'}), dtv_timeout{1000, 0},
        reuse_reply, reuse_size, reuse_result, reuse_done);
    const auto reuse_out = baseline_out + 7;
    if (!send_out(reuse_out) || !send_reply(208)) { reuse.join(); return 31; }
    reuse.join();
    if (reuse_result.load() != DTV_INTERNAL_RESULT_OK || reuse_size != 7) return 32;

    backend->set_defer_cancel_completion(true);
    std::array<unsigned char, 32> cancelled_reply{};
    std::size_t cancelled_size = 0;
    std::atomic<std::int32_t> cancelled_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> cancelled_done{false};
    const auto cancel_out = baseline_out + 8;
    auto cancelled = start(1, dtv::spi::PublicOperationTargetKind::receiver, 6,
        make_arguments({49}, 209, {'Q'}), dtv_timeout{1000, 0},
        cancelled_reply, cancelled_size, cancelled_result, cancelled_done);
    if (!backend->wait_for_endpoint_submissions(0x02, cancel_out)) { cancelled.join(); return 33; }
    if (dtv_cancel_enclosure(root, &error) != DTV_ERROR_OK) { cancelled.join(); return 34; }
    cancelled.join();
    if (cancelled_result.load() != DTV_INTERNAL_RESULT_CANCELLED) return 35;
    std::array<unsigned char, 32> blocked_reply{};
    std::size_t blocked_size = 0;
    std::atomic<std::int32_t> blocked_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> blocked_done{false};
    auto blocked = start(1, dtv::spi::PublicOperationTargetKind::receiver, 7,
        make_arguments({49}, 210, {'W'}), dtv_timeout{30, 0},
        blocked_reply, blocked_size, blocked_result, blocked_done);
    blocked.join();
    if (blocked_result.load() != DTV_INTERNAL_RESULT_TIMEOUT ||
        backend->endpoint_submission_count(0x02) != cancel_out) return 36;
    const auto cancel_callbacks = backend->callback_count_snapshot();
    backend->complete_endpoint(0x02);
    if (!backend->wait_for_callbacks(cancel_callbacks + 1)) return 37;
    std::array<unsigned char, 32> after_cancel_reply{};
    std::size_t after_cancel_size = 0;
    std::atomic<std::int32_t> after_cancel_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> after_cancel_done{false};
    auto after_cancel = start(1, dtv::spi::PublicOperationTargetKind::receiver, 8,
        make_arguments({49}, 211, {'S'}), dtv_timeout{1000, 0},
        after_cancel_reply, after_cancel_size, after_cancel_result, after_cancel_done);
    if (!send_out(cancel_out + 1) || !send_reply(211)) { after_cancel.join(); return 38; }
    after_cancel.join();
    if (after_cancel_result.load() != DTV_INTERNAL_RESULT_OK) return 39;

    backend->set_defer_cancel_completion(false);
    const auto ticket_out = cancel_out + 2;
    std::array<std::array<unsigned char, 32>, 16> ticket_replies{};
    std::array<std::size_t, 16> ticket_reply_sizes{};
    std::array<std::atomic<std::int32_t>, 16> ticket_results{};
    std::array<std::atomic<bool>, 16> ticket_done{};
    std::vector<std::thread> ticket_calls;
    ticket_calls.reserve(ticket_results.size());
    ticket_calls.push_back(start(1, dtv::spi::PublicOperationTargetKind::receiver, 20,
        make_arguments({60}, 220, {'Q'}), dtv_timeout{1000, 0}, ticket_replies[0],
        ticket_reply_sizes[0], ticket_results[0], ticket_done[0]));
    if (!backend->wait_for_endpoint_submissions(0x02, ticket_out)) {
        dtv_cancel_enclosure(root, &error);
        for (auto &call : ticket_calls) call.join();
        return 40;
    }
    for (std::size_t index = 1; index < ticket_results.size(); ++index) {
        ticket_results[index].store(DTV_ERROR_INTERNAL, std::memory_order_release);
        ticket_calls.push_back(start(1, dtv::spi::PublicOperationTargetKind::receiver, 20 + index,
            make_arguments({60}, static_cast<unsigned char>(220 + index), {'Q'}),
            dtv_timeout{1000, 0}, ticket_replies[index], ticket_reply_sizes[index],
            ticket_results[index], ticket_done[index]));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto tickets_full = invoke(1, dtv::spi::PublicOperationTargetKind::card, 30,
        make_arguments({61}, 250, {'F'}), dtv_timeout{100, 0}, reply, &reply_size);
    if (tickets_full.code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
        backend->endpoint_submission_count(0x02) != ticket_out) {
        dtv_cancel_enclosure(root, &error);
        for (auto &call : ticket_calls) call.join();
        return 41;
    }
    const auto ticket_callbacks = backend->callback_count_snapshot();
    if (dtv_cancel_enclosure(root, &error) != DTV_ERROR_OK) {
        for (auto &call : ticket_calls) call.join();
        return 42;
    }
    for (auto &call : ticket_calls) call.join();
    if (std::any_of(ticket_results.begin(), ticket_results.end(), [](const auto &value) {
            return value.load(std::memory_order_acquire) != DTV_INTERNAL_RESULT_CANCELLED;
        })) return 43;
    if (!backend->wait_for_callbacks(ticket_callbacks + 1)) return 44;
    std::array<unsigned char, 32> ticket_reuse_reply{};
    std::size_t ticket_reuse_size = 0;
    std::atomic<std::int32_t> ticket_reuse_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> ticket_reuse_done{false};
    auto ticket_reuse = start(1, dtv::spi::PublicOperationTargetKind::card, 30,
        make_arguments({61}, 251, {'U'}), dtv_timeout{1000, 0},
        ticket_reuse_reply, ticket_reuse_size, ticket_reuse_result, ticket_reuse_done);
    if (!send_out(ticket_out + 1) || !send_reply(251)) { ticket_reuse.join(); return 45; }
    ticket_reuse.join();
    if (ticket_reuse_result.load() != DTV_INTERNAL_RESULT_OK) return 46;

    std::array<unsigned char, 32> close_reply{};
    std::size_t close_size = 0;
    std::atomic<std::int32_t> close_result{DTV_ERROR_INTERNAL};
    std::atomic<bool> close_done{false};
    const auto close_out = ticket_out + 2;
    auto close_waiter = start(1, dtv::spi::PublicOperationTargetKind::receiver, 9,
        make_arguments({50}, 212, {'Z'}), dtv_timeout{1000, 0},
        close_reply, close_size, close_result, close_done);
    if (!send_out(close_out)) { close_waiter.join(); return 47; }
    backend->set_defer_cancel_completion(true);
    if (!backend->wait_for_pending_endpoint(0x81) ||
        !backend->wait_for_pending_endpoint(0x82)) {
        std::fprintf(stderr, "operation close setup: transfers=%zu cancelled=%zu\n",
            backend->pending_transfer_count(), backend->cancelled_transfer_count());
        backend->set_defer_cancel_completion(false);
        dtv_close_enclosure(root, nullptr, &error);
        close_waiter.join();
        return 51;
    }
    const auto close_callbacks_before = backend->callback_count_snapshot();
    const dtv_call_options short_close{sizeof(dtv_call_options), 1};
    const auto close_status = dtv_close_enclosure(root, &short_close, &error);
    close_waiter.join();
    const auto close_pending_transfers = backend->pending_transfer_count();
    const auto close_cancelled_transfers = backend->cancelled_transfer_count();
    if (close_result.load() != DTV_INTERNAL_RESULT_CANCELLED ||
        close_status != DTV_CLOSE_PENDING || backend->close_count != 0 ||
        close_pending_transfers == 0) {
        std::fprintf(stderr,
            "operation close: caller=%d done=%d close=%u error=%u handles=%u/%u claims=%u "
            "callbacks=%u->%u transfers=%zu cancelled=%zu out=%u defer=%d\n",
            close_result.load(std::memory_order_acquire), close_done.load(std::memory_order_acquire),
            close_status, error.code, backend->open_count, backend->close_count,
            backend->claim_count, close_callbacks_before, backend->callback_count_snapshot(),
            close_pending_transfers, close_cancelled_transfers,
            backend->endpoint_submission_count(0x02), backend->cancel_completion_deferred() ? 1 : 0);
        return 48;
    }
    backend->set_defer_cancel_completion(false);
    backend->complete_endpoint(0x81);
    backend->complete_endpoint(0x82);
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED ||
        backend->close_count != 2) return 49;
    dtv::internal::set_public_discovery_batch_for_test({});
    (void)dtv_list_enclosures(&error);
    const auto disconnected_out = backend->endpoint_submission_count(0x02);
    result = invoke(1, dtv::spi::PublicOperationTargetKind::receiver, 10,
        make_arguments({51}, 213, {'D'}), dtv_timeout{100, 0}, reply, &reply_size);
    if (result.code != DTV_INTERNAL_RESULT_DISCONNECTED ||
        backend->endpoint_submission_count(0x02) != disconnected_out) return 50;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

int public_open_shared_terminal(unsigned failure_mode, std::int32_t expected)
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    set_scan_public_open_case(9);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 187 + failure_mode * 2),
             snapshot(context, 4, 188 + failure_mode * 2)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(2)) { opening.join(); return 3; }
    backend->complete_endpoint(0x81, 1);
    backend->complete_endpoint(0x82, 1);
    opening.join();
    if (open_result != DTV_ERROR_OK) return 4;
    auto session = scan_shared_open_session();
    std::uint64_t token = 0;
    const auto registered = session.register_reply(session.user, 8, 901,
        scan_shared_generation(0), &token);
    if (registered.code != DTV_INTERNAL_RESULT_OK) return 5;
    set_scan_shared_failure_mode(failure_mode);
    backend->complete_endpoint(0x81, 1);
    backend->complete_endpoint(0x82, 1);
    std::array<unsigned char, 32> output{};
    std::size_t output_size = 0;
    dtv::spi::PublicOpenWaitResult wait_result{};
    const auto waited = session.wait_reply(session.user, token, nullptr,
        dtv_timeout{100, 0}, &wait_result, output, &output_size);
    if (waited.code != expected || wait_result != dtv::spi::PublicOpenWaitResult::failed) return 6;
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED) return 7;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    set_scan_public_open_case(0);
    return 0;
}

int public_open_pending_close()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->defer_cancel_completion = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 87), snapshot(context, 4, 88)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    std::int32_t open_result = DTV_ERROR_INTERNAL;
    dtv_error open_error{};
    open_error.struct_size = sizeof(open_error);
    std::thread opening([&] { open_result = dtv_open_enclosure(root, nullptr, &open_error); });
    if (!backend->wait_for_submissions(2)) {
        backend->complete_endpoint(0x81, 2);
        opening.join();
        return 3;
    }
    const dtv_call_options short_close{sizeof(dtv_call_options), 1};
    const auto closed = dtv_close_enclosure(root, &short_close, &error);
    if (closed != DTV_CLOSE_PENDING || error.code != DTV_ERROR_OK) {
        backend->complete_endpoint(0x81, 2);
        opening.join();
        return 4;
    }
    dtv_release_enclosure(root);
    if (dtv_open_enclosure(root, nullptr, &error) != DTV_ERROR_BUSY) {
        backend->complete_endpoint(0x81, 2);
        opening.join();
        return 5;
    }
    backend->complete_endpoint(0x81, 2);
    opening.join();
    if (open_result != DTV_ERROR_CANCELLED ||
        dtv_get_enclosure_close_state(root, &error) != DTV_CLOSE_CLOSED ||
        backend->close_count != 2) return 6;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    return 0;
}

int public_open_preflight_and_identify_failures()
{
    for (unsigned mode = 1; mode <= 7; ++mode) {
        auto backend = std::make_shared<FakeBackend>();
        backend->async_enabled = true;
        backend->complete_all = true;
        if (mode == 4) backend->fail_claim_call = 1;
        if (mode == 5) backend->fail_submit = true;
        auto context = dtv::internal::UsbContext::scan_only(backend);
        set_scan_group_case(0);
        dtv::internal::CompleteGroupBatch batch;
        auto first = snapshot(context, 3, 100 + mode * 2);
        auto second = snapshot(context, 4, 101 + mode * 2);
        if (mode == 6) ++second.device_address;
        if (dtv::internal::observe_loaded_product_groups(
                {std::move(first), std::move(second)}, &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1) return 1;
        dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
        dtv_error error{};
        error.struct_size = sizeof(error);
        auto *const *enclosures = dtv_list_enclosures(&error);
        if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
        auto *root = enclosures[0];
        dtv_hold_enclosure(root);
        set_scan_public_open_case(mode <= 3 ? mode : 0);
        if (mode == 7) dtv::internal::set_enclosure_runtime_start_failure_for_test(true);
        error.cause_domain = 91;
        error.cause_code = 73;
        const auto opened = dtv_open_enclosure(root, nullptr, &error);
        const auto expected = mode == 7 ? DTV_ERROR_RESOURCE_LIMIT
                            : mode >= 3 ? DTV_ERROR_USB_IO : DTV_ERROR_UNSUPPORTED;
        if (opened != expected || error.code != expected) return 3;
        const auto expected_cause_domain = mode == 4 || mode == 5 || mode == 6
            ? DTV_ERROR_DOMAIN_LIBUSB : DTV_ERROR_DOMAIN_NONE;
        const auto expected_cause_code = mode == 4 || mode == 5 ? LIBUSB_ERROR_IO
                                     : mode == 6 ? LIBUSB_ERROR_NO_DEVICE : 0;
        if (error.cause_domain != expected_cause_domain ||
            error.cause_code != expected_cause_code) return 11;
        if (mode <= 2 && (backend->open_count != 0 || backend->claim_count != 0)) return 4;
        if (mode == 3 && (backend->open_count != 2 || backend->claim_count != 2)) return 5;
        if (mode == 4 && (backend->open_count != 1 || backend->claim_count != 1)) return 7;
        if (mode == 5 && (backend->open_count != 2 || backend->claim_count != 2)) return 8;
        if (mode == 6 && (backend->open_count != 0 || backend->claim_count != 0)) return 9;
        if (mode == 7 && (backend->open_count != 2 || backend->claim_count != 2 ||
                          backend->close_count != 2)) return 10;
        const auto close_expected = mode == 3 || mode == 5 ? DTV_ERROR_OK : expected;
        if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED ||
            error.code != close_expected ||
            error.cause_domain != (close_expected == expected ? expected_cause_domain
                                                               : DTV_ERROR_DOMAIN_NONE) ||
            error.cause_code != (close_expected == expected ? expected_cause_code : 0)) return 6;
        if (dtv_get_enclosure_close_state(root, &error) != DTV_CLOSE_CLOSED ||
            error.code != expected || error.cause_domain != expected_cause_domain ||
            error.cause_code != expected_cause_code) return 12;
        dtv_release_enclosure(root);
        dtv_drop_enclosure(root);
    }
    set_scan_public_open_case(0);
    return 0;
}

int public_open_error_boundaries()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->fail_claim_call = 1;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 140), snapshot(context, 4, 141)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error list_error{};
    list_error.struct_size = sizeof(list_error);
    auto *const *enclosures = dtv_list_enclosures(&list_error);
    if (enclosures == nullptr || enclosures[0] == nullptr) return 2;
    auto *root = enclosures[0];
    dtv_hold_enclosure(root);
    char message = 'x';
    dtv_error short_error{};
    short_error.struct_size = offsetof(dtv_error, cause_domain);
    short_error.code = -77;
    short_error.cause_domain = 91;
    short_error.cause_code = 73;
    short_error.message = &message;
    short_error.message_capacity = 1;
    const auto opened = dtv_open_enclosure(root, nullptr, &short_error);
    if (opened != DTV_ERROR_USB_IO || short_error.code != DTV_ERROR_USB_IO ||
        short_error.cause_domain != 91 || short_error.cause_code != 73 || message != 'x') return 3;
    dtv_error cleanup_error{};
    cleanup_error.struct_size = sizeof(cleanup_error);
    if (dtv_close_enclosure(root, nullptr, &cleanup_error) != DTV_CLOSE_CLOSED ||
        cleanup_error.code != DTV_ERROR_USB_IO ||
        cleanup_error.cause_domain != DTV_ERROR_DOMAIN_LIBUSB ||
        cleanup_error.cause_code != LIBUSB_ERROR_IO) return 4;
    if (dtv_get_enclosure_close_state(root, nullptr) != DTV_CLOSE_CLOSED) return 5;
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    return 0;
}

int public_open_independent_enclosures()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->complete_all = true;
    auto context = dtv::internal::UsbContext::scan_only(backend);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    if (dtv::internal::observe_loaded_product_groups(
            {snapshot(context, 3, 120), snapshot(context, 6, 121)}, &batch).code !=
            DTV_INTERNAL_RESULT_OK || batch.groups.size() != 1) return 1;
    auto second_group = batch.groups.front();
    const auto first_candidate = second_group.functions.front().candidate_index;
    second_group.anchor_candidate_index = first_candidate == 0 ? 1 : 0;
    for (auto &function : second_group.functions) {
        function.candidate_index = first_candidate == 0 ? 1 : 0;
    }
    batch.groups.push_back(std::move(second_group));
    dtv::internal::set_public_discovery_context_for_test(std::move(context), std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == nullptr || enclosures[0] == nullptr || enclosures[1] == nullptr) return 2;
    auto *first = enclosures[0];
    auto *second = enclosures[1];
    dtv_hold_enclosure(first);
    dtv_hold_enclosure(second);
    set_scan_public_open_case(8);
    std::int32_t first_result = DTV_ERROR_INTERNAL;
    dtv_error first_error{};
    first_error.struct_size = sizeof(first_error);
    std::thread opening([&] { first_result = dtv_open_enclosure(first, nullptr, &first_error); });
    bool blocked = false;
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        if (scan_open_identify_is_blocked()) {
            blocked = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!blocked) {
        release_scan_open_identify();
        opening.join();
        return 3;
    }
    const auto second_result = dtv_open_enclosure(second, nullptr, nullptr);
    if (second_result != DTV_ERROR_OK || backend->worker_ids.size() != 2) {
        release_scan_open_identify();
        opening.join();
        return 4;
    }
    if (dtv_close_enclosure(second, nullptr, nullptr) != DTV_CLOSE_CLOSED) {
        release_scan_open_identify();
        opening.join();
        return 5;
    }
    release_scan_open_identify();
    opening.join();
    if (first_result != DTV_ERROR_OK || first_error.code != DTV_ERROR_OK ||
        dtv_close_enclosure(first, nullptr, &error) != DTV_CLOSE_CLOSED) return 6;
    dtv_release_enclosure(second);
    dtv_release_enclosure(first);
    dtv_drop_enclosure(second);
    dtv_drop_enclosure(first);
    set_scan_public_open_case(0);
    return 0;
}

int public_open_fd_owner()
{
#if defined(__linux__)
    auto backend = std::make_shared<FakeBackend>();
    backend->async_enabled = true;
    backend->complete_all = true;
    backend->wrapped_device = 5;
    dtv::internal::set_public_discovery_usb_backend_for_test(backend);
    const int caller_fd = open("/dev/null", O_RDONLY);
    if (caller_fd < 0) return 1;
    dtv_error error{};
    error.struct_size = sizeof(error);
    auto *const *enclosures = dtv_list_enclosures_by_fds(&caller_fd, 1, &error);
    if (enclosures == nullptr) {
        close(caller_fd);
        return 2;
    }
    const auto found = std::find_if(enclosures, enclosures + DTV_MAX_ENCLOSURES,
                                    [](auto *entry) { return entry != nullptr; });
    if (found == enclosures + DTV_MAX_ENCLOSURES) {
        close(caller_fd);
        return 2;
    }
    auto *root = *found;
    dtv_hold_enclosure(root);
    const auto opened = dtv_open_enclosure(root, nullptr, &error);
    if (opened != DTV_ERROR_OK || error.code != DTV_ERROR_OK ||
        fcntl(caller_fd, F_GETFD) < 0 || backend->claim_count != 1 ||
        backend->worker_ids.size() != 1 || scan_open_identify_size() != 32) {
        close(caller_fd);
        return 3;
    }
    if (dtv_close_enclosure(root, nullptr, &error) != DTV_CLOSE_CLOSED ||
        fcntl(caller_fd, F_GETFD) < 0) {
        close(caller_fd);
        return 4;
    }
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    close(caller_fd);
    return 0;
#else
    return 0;
#endif
}

int public_error_message_boundary()
{
    char message = 'x';
    dtv_error error{};
    error.struct_size = sizeof(error);
    error.cause_domain = 91;
    error.cause_code = 73;
    error.message = &message;
    error.message_capacity = 1;
    const auto *const *enclosures = dtv_list_enclosures_by_tokens(nullptr, 1, &error);
    return enclosures == nullptr && error.code == DTV_ERROR_INVALID_ARGUMENT &&
            error.cause_domain == DTV_ERROR_DOMAIN_NONE && error.cause_code == 0 && message == '\0'
        ? 0 : 1;
}

}

int main(int argc, char **argv)
{
    dtv_load_scanfixture();
    if (argc == 2 && std::strcmp(argv[1], "--lnb") == 0) {
        const int result = public_lnb_roundtrip();
        if (result != 0) {
            std::fprintf(stderr, "public_lnb_roundtrip failed: %d\n", result);
            return result;
        }
        const int unsupported_result = public_lnb_unsupported();
        if (unsupported_result != 0)
            std::fprintf(stderr, "public_lnb_unsupported failed: %d\n", unsupported_result);
        return unsupported_result;
    }
    if (argc == 2 && std::strcmp(argv[1], "--firmware") == 0) {
        const int result = public_firmware_roundtrip();
        return result != 0 ? result : public_firmware_same_id_mode();
    }
    if (argc == 2 && std::strcmp(argv[1], "--firmware-same-id") == 0)
        return public_firmware_same_id_mode();
    if (argc == 2 && std::strcmp(argv[1], "--firmware-unknown") == 0)
        return public_firmware_unknown_no_io();
    if (argc == 2 && std::strcmp(argv[1], "--card-events") == 0)
        return public_open_card_events();
    if (const int result = public_lnb_roundtrip(); result != 0) {
        std::fprintf(stderr, "public_lnb_roundtrip failed: %d\n", result);
        return 139 + result;
    }
    if (const int result = public_firmware_roundtrip(); result != 0) {
        std::fprintf(stderr, "public_firmware_roundtrip failed: %d\n", result);
        return 130 + result;
    }
    if (const int result = public_firmware_same_id_mode(); result != 0) {
        std::fprintf(stderr, "public_firmware_same_id_mode failed: %d\n", result);
        return 150 + result;
    }
    if (const int result = public_reset_card(); result != 0) {
        std::fprintf(stderr, "public_reset_card failed: %d\n", result);
        return 140 + result;
    }
    if (const int result = public_card_transport(); result != 0) {
        std::fprintf(stderr, "public_card_transport failed: %d\n", result);
        return 160 + result;
    }
    if (const int result = public_partial_cwt_close(); result != 0) {
        std::fprintf(stderr, "public_partial_cwt_close failed: %d\n", result);
        return 170 + result;
    }
    if (const int result = registry_plan_and_success(); result != 0) {
        std::fprintf(stderr, "registry_plan_and_success failed: %d\n", result);
        return result;
    }
    if (const int result = rollback_and_preflight(); result != 0) {
        std::fprintf(stderr, "rollback_and_preflight failed: %d\n", result);
        return 10 + result;
    }
    if (const int result = busy_and_fd_owner(); result != 0) {
        std::fprintf(stderr, "busy_and_fd_owner failed: %d\n", result);
        return 20 + result;
    }
    if (const int result = enclosure_runtime_completions(); result != 0) {
        std::fprintf(stderr, "enclosure_runtime_completions failed: %d\n", result);
        return 30 + result;
    }
    if (const int result = enclosure_runtime_pending_stop(); result != 0) {
        std::fprintf(stderr, "enclosure_runtime_pending_stop failed: %d\n", result);
        return 40 + result;
    }
    if (const int result = enclosure_runtime_endpoint_cancel(); result != 0) {
        std::fprintf(stderr, "enclosure_runtime_endpoint_cancel failed: %d\n", result);
        return 50 + result;
    }
    if (const int result = public_open_lifecycle(); result != 0) {
        std::fprintf(stderr, "public_open_lifecycle failed: %d\n", result);
        return 60 + result;
    }
    if (const int result = public_stream_wait_lifecycle(); result != 0) {
        std::fprintf(stderr, "public_stream_wait_lifecycle failed: %d\n", result);
        return 61 + result;
    }
    if (const int result = public_open_192_stream(); result != 0) {
        std::fprintf(stderr, "public_open_192_stream failed: %d\n", result);
        return 62 + result;
    }
    if (const int result = public_stream_queue_overflow(); result != 0) {
        std::fprintf(stderr, "public_stream_queue_overflow failed: %d\n", result);
        return 63 + result;
    }
    if (const int result = public_stream_pid_filter(); result != 0) {
        std::fprintf(stderr, "public_stream_pid_filter failed: %d\n", result);
        return 64 + result;
    }
    if (const int result = public_open_shared_in(); result != 0) {
        std::fprintf(stderr, "public_open_shared_in failed: %d\n", result);
        return 65 + result;
    }
    if (const int result = public_open_shared_depth_order(); result != 0) {
        std::fprintf(stderr, "public_open_shared_depth_order failed: %d\n", result);
        return 75 + result;
    }
    if (const int result = public_open_operation_admission(); result != 0) {
        std::fprintf(stderr, "public_open_operation_admission failed: %d\n", result);
        return 90 + result;
    }
    if (const int result = public_open_shared_terminal(1,
            DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED); result != 0) {
        std::fprintf(stderr, "public_open_shared_sink_overflow failed: %d\n", result);
        return 80 + result;
    }
    if (const int result = public_open_shared_terminal(2,
            DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED); result != 0) {
        std::fprintf(stderr, "public_open_shared_classifier_oom failed: %d\n", result);
        return 90 + result;
    }
    if (const int result = public_open_pending_close(); result != 0) {
        std::fprintf(stderr, "public_open_pending_close failed: %d\n", result);
        return 70 + result;
    }
    if (const int result = public_open_preflight_and_identify_failures(); result != 0) {
        std::fprintf(stderr, "public_open_preflight_and_identify_failures failed: %d\n", result);
        return 80 + result;
    }
    if (const int result = public_open_error_boundaries(); result != 0) {
        std::fprintf(stderr, "public_open_error_boundaries failed: %d\n", result);
        return 90 + result;
    }
    if (const int result = public_open_independent_enclosures(); result != 0) {
        std::fprintf(stderr, "public_open_independent_enclosures failed: %d\n", result);
        return 100 + result;
    }
    if (const int result = public_open_fd_owner(); result != 0) {
        std::fprintf(stderr, "public_open_fd_owner failed: %d\n", result);
        return 110 + result;
    }
    if (const int result = public_error_message_boundary(); result != 0) {
        std::fprintf(stderr, "public_error_message_boundary failed: %d\n", result);
        return 120 + result;
    }
    if (const int result = public_open_card_events(); result != 0) {
        std::fprintf(stderr, "public_open_card_events failed: %d\n", result);
        return 130 + result;
    }
    return 0;
}
