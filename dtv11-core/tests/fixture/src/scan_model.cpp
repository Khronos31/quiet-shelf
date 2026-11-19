/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <thread>
#include <new>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <span>
#include <vector>

#include <dtv/internal/l3-registry.hpp>
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>
#include <dtv/spi/firmware_session.hpp>

struct ScanFirmwareContext {
    const dtv_usb_io *io = nullptr;
    std::uint32_t mode = DTV_IDENTIFY_UNKNOWN;
    dtv_device_id device_id = 0;
    std::uint16_t product_id = 0;
};

std::atomic<dtv_device_id> scan_firmware_last_device{0};
std::atomic<dtv_function> scan_firmware_last_function{0};
std::atomic<std::uint64_t> scan_firmware_last_epoch{0};
std::atomic<std::size_t> scan_firmware_last_handle{0};
std::atomic<unsigned> scan_firmware_create_count{0};
std::atomic<unsigned> scan_firmware_open_count{0};
std::atomic<unsigned> scan_firmware_identify_count{0};
std::atomic<unsigned> scan_firmware_close_count{0};
std::atomic<unsigned> scan_firmware_destroy_count{0};
std::mutex scan_firmware_mutex;
std::vector<dtv_device_id> scan_firmware_runtime_devices;
std::atomic<std::uint32_t> scan_firmware_same_id_mode{DTV_IDENTIFY_RUNTIME};

namespace {

dtv_internal_result scan_firmware_open(dtv_context *context, const dtv_usb_io *io,
                                       const dtv_open_args *args)
{
    if (context == nullptr || io == nullptr || args == nullptr || args->usb_observation == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    auto *state = static_cast<ScanFirmwareContext *>(context);
    scan_firmware_open_count.fetch_add(1, std::memory_order_acq_rel);
    state->io = io;
    state->product_id = args->usb_observation->product_id;
    std::lock_guard lock(scan_firmware_mutex);
    state->mode = args->usb_observation->product_id == 0x0101
        ? scan_firmware_same_id_mode.load(std::memory_order_acquire)
        : (std::find(scan_firmware_runtime_devices.begin(), scan_firmware_runtime_devices.end(),
                     state->device_id) != scan_firmware_runtime_devices.end()
            ? DTV_IDENTIFY_RUNTIME : DTV_IDENTIFY_LOADER);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_firmware_identify(dtv_context *context, dtv_identify_info *identity,
                                           dtv_timeout)
{
    if (context == nullptr || identity == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    auto *state = static_cast<ScanFirmwareContext *>(context);
    scan_firmware_identify_count.fetch_add(1, std::memory_order_acq_rel);
    identity->state = state->mode;
    identity->firmware_key = state->mode == DTV_IDENTIFY_RUNTIME ? 1 : 0;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_firmware_close(dtv_context *context, dtv_timeout)
{
    if (context != nullptr) static_cast<ScanFirmwareContext *>(context)->io = nullptr;
    scan_firmware_close_count.fetch_add(1, std::memory_order_acq_rel);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_firmware_create(dtv_device_id device_id, dtv_context **out)
{
    if (out == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    auto *state = new ScanFirmwareContext;
    state->device_id = device_id;
    *out = state;
    scan_firmware_create_count.fetch_add(1, std::memory_order_acq_rel);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

void scan_firmware_destroy(dtv_context *context)
{
    delete static_cast<ScanFirmwareContext *>(context);
    scan_firmware_destroy_count.fetch_add(1, std::memory_order_acq_rel);
}

const dtv::spi::DeviceOps scan_firmware_ops{
    .open = scan_firmware_open,
    .identify = scan_firmware_identify,
    .close = scan_firmware_close,
};

enum class ScanGroupCase {
    complete, no_match, ambiguous, bad_index, bad_interface, callback_error, callback_exception
};
ScanGroupCase scan_group_case = ScanGroupCase::complete;
std::thread::id scan_group_thread;
unsigned scan_public_open_case = 0;
std::size_t scan_open_identify_bytes = 0;
std::atomic<unsigned> scan_open_identify_calls{0};
std::atomic<bool> scan_open_identify_blocked{false};
std::atomic<bool> scan_open_identify_release{true};
dtv::spi::PublicOpenSession scan_shared_session{};
std::array<std::uint64_t, 2> scan_shared_tokens{};
std::array<std::uint64_t, 2> scan_shared_generations{};
std::atomic<unsigned> scan_shared_packets{0};
std::atomic<bool> scan_shared_reply_enabled{false};
std::atomic<unsigned> scan_shared_failure_mode{0};
std::atomic<bool> scan_shared_stale_generation{false};
std::atomic<std::uint64_t> scan_shared_transaction_override{0};
std::atomic<std::uint32_t> scan_shared_card_state{DTV_CARD_ABSENT};
std::atomic<bool> scan_shared_card_stale_generation{false};
std::atomic<std::uint32_t> scan_shared_lock_receiver{0};
std::atomic<std::uint32_t> scan_shared_lock_state{0};
std::atomic<std::uint32_t> scan_lnb_requested_state{DTV_LNB_UNKNOWN};
std::atomic<bool> scan_lnb_reply_pending{false};
std::atomic<bool> scan_shared_lock_stale_generation{false};
std::atomic<bool> scan_shared_lock_enabled{false};
std::atomic<std::uint64_t> scan_stream_generation{0};
std::atomic<std::uint64_t> previous_stream_generation{0};
std::atomic<std::uint64_t> scan_stream_generation_override{0};
std::atomic<unsigned> scan_stream_packet{0};
std::atomic<std::uint32_t> scan_queue_packet{0};
std::atomic<std::size_t> scan_stream_queue_capacity{0};
std::array<std::atomic<std::uint64_t>, 3> scan_queue_generations{};
std::atomic<bool> scan_queue_notification_enabled{false};
std::atomic<unsigned> scan_stream_prepare_mode{0};
std::atomic<unsigned> scan_stream_start_mode{0};
std::atomic<unsigned> scan_stream_stop_mode{0};
std::atomic<unsigned> scan_stream_drain_mode{0};
std::atomic<std::size_t> scan_stream_192_offset{0};
std::atomic<bool> scan_stream_192_end_sent{false};
std::atomic<unsigned> scan_stream_receiver{0};
std::atomic<unsigned> scan_stream_transform_calls{0};
std::atomic<unsigned> scan_stream_transform_mode{0};
std::atomic<bool> scan_stream_transform_test_active{false};
std::atomic<unsigned> scan_stream_transform_test_receiver{0};
std::atomic<std::size_t> scan_stream_transform_source_size{0};
std::array<unsigned char, 754> scan_stream_transform_source{};
std::array<unsigned char, 1347> scan_stream_192_source{};
std::array<std::atomic<std::uint64_t>, 3> scan_tune_epochs{};
std::atomic<std::uint64_t> scan_shared_lock_epoch_override{UINT64_MAX};
std::atomic<unsigned> scan_tune_mode{0};
std::atomic<unsigned> scan_tune_calls{0};
std::atomic<unsigned> scan_tune_effects{0};
std::atomic<bool> scan_tune_peer_checked{false};
std::atomic<bool> scan_tune_card_checked{false};
std::atomic<dtv_enclosure *> scan_tune_enclosure{nullptr};
std::array<std::atomic<dtv_receiver *>, 3> scan_tune_receivers{};
struct ScanTuneRecord {
    std::uint32_t receiver_index = 0;
    std::uint32_t system = 0;
    std::uint64_t frequency_hz = 0;
    bool has_options = false;
    dtv_tune_options options{};
    dtv_timeout timeout{};
};
std::array<ScanTuneRecord, 16> scan_tune_records{};
std::array<std::uint64_t, 2> scan_shared_sequences{};
std::array<std::size_t, 2> scan_shared_lengths{};
std::array<std::uint64_t, 32> scan_shared_order{};
std::array<std::uint8_t, 32> scan_shared_order_endpoints{};
std::mutex scan_pid_mutex;
std::condition_variable scan_pid_condition;
std::vector<std::uint16_t> scan_pid_active;
std::vector<std::uint16_t> scan_pid_pending;
bool scan_pid_active_all = true;
bool scan_pid_pending_all = true;
bool scan_pid_pending_valid = false;
std::atomic<std::uint32_t> scan_pid_transaction{0};
std::atomic<std::uint32_t> scan_pid_transaction_next{1};
std::atomic<bool> scan_pid_fail_next{false};
std::atomic<unsigned> scan_pid_source_count{0};
std::atomic<unsigned> scan_pid_processed_count{0};
std::atomic<unsigned> scan_pid_receiver_one_count{0};
std::atomic<unsigned> scan_card_reply_mode{0};
std::atomic<unsigned> scan_card_transaction{1};
std::atomic<unsigned> scan_card_interleave_phase{0};
std::atomic<unsigned> scan_card_transport_kind{0};
std::atomic<bool> scan_card_transport_drop_reply{false};
std::atomic<unsigned> scan_card_t1_exchange_mode{0};
std::atomic<unsigned> scan_card_t1_bgt_mode{2};
std::atomic<std::uint64_t> scan_card_t1_bgt_etu_upper_bound_ns{1000000};
std::atomic<std::uint64_t> scan_card_t1_bgt_revision{1};
std::atomic<unsigned> scan_card_t1_bgt_refresh_count{0};
std::atomic<bool> scan_card_t1_bgt_send_plan_override{false};
std::atomic<std::uint64_t> scan_card_t1_bgt_send_plan_bound_ns{0};
std::atomic<std::uint64_t> scan_card_t1_bgt_send_plan_revision{0};
std::atomic<unsigned> scan_card_t1_fault{0};
std::atomic<bool> scan_card_t1_apdu_mode{false};
std::atomic<unsigned> scan_card_t1_apdu_fault{0};
std::atomic<bool> scan_card_t1_apdu_rblock_sent{false};
std::atomic<unsigned> scan_card_t1_apdu_chain_index{0};
std::atomic<bool> scan_card_t1_apdu_response_sequence{false};
std::atomic<unsigned> scan_card_t1_apdu_ack_count{0};
std::atomic<unsigned> scan_card_t1_apdu_ack_trace{0};
std::atomic<bool> scan_card_t1_apdu_drop_r_reply{false};
std::atomic<unsigned> scan_card_t1_apdu_reply_count{0};
std::atomic<unsigned> scan_card_t1_wtx_exchange_count{0};
std::atomic<std::uint64_t> scan_card_t1_cwt_card_transaction{0};
std::atomic<std::uint64_t> scan_card_t1_cwt_duration_ns{0};
std::atomic<unsigned> scan_card_t1_cwt_fault{0};
std::atomic<bool> scan_card_t1_cwt_silence_delivered{false};
std::atomic<unsigned> scan_card_t1_bwt_mode{0};
std::atomic<unsigned> scan_card_t1_bwt_timeout_count{0};
std::atomic<bool> scan_card_t1_bwt_decided{false};
std::atomic<bool> scan_card_t1_bwt_current_late{false};
std::atomic<unsigned char> scan_card_t1_apdu_pending_ins{0};
std::atomic<bool> scan_card_t1_apdu_ifs_requested{false};
std::atomic<bool> scan_card_t1_apdu_ifs_negotiated{false};
std::atomic<unsigned> scan_card_t1_host_ifs_chain_state{0};
std::atomic<bool> scan_card_t1_receive_capacity_known{false};
std::atomic<std::size_t> scan_card_t1_receive_capacity_bytes{0};
std::atomic<std::uint64_t> scan_card_t1_receive_capacity_revision{0};
std::atomic<std::uint64_t> scan_card_t1_receive_capacity_evidence{0};
std::atomic<bool> scan_card_t1_crc{false};
std::atomic<bool> scan_card_t1_stale_reply{false};
std::atomic<std::uint64_t> scan_card_t1_transaction{300};
std::atomic<std::uint64_t> scan_card_t1_current_transaction{0};
std::atomic<std::uint64_t> scan_card_t1_last_write_transaction{0};
std::atomic<unsigned> scan_card_t1_last_outgoing_pcb{0};
std::atomic<std::uint64_t> scan_card_t1_connection_epoch{0};
std::atomic<std::uint64_t> scan_card_t1_request_epoch{0};
std::mutex scan_card_t1_mutex;
std::vector<unsigned char> scan_card_t1_request;
std::vector<unsigned char> scan_card_t1_reply;
std::vector<unsigned char> scan_card_t1_pending;
std::size_t scan_card_t1_reply_offset = 0;
bool scan_card_t1_write_started = false;
bool scan_card_t1_complete_probe_pending = false;

void scan_prepare_t1_reply(std::span<const unsigned char> request)
{
    scan_card_t1_cwt_card_transaction.store(0, std::memory_order_release);
    const auto edc_size = scan_card_t1_crc.load(std::memory_order_acquire) ? 2u : 1u;
    if (request.size() < 3 + edc_size ||
        request.size() != static_cast<std::size_t>(request[2]) + 3 + edc_size)
        return;
    if (edc_size == 1) {
        unsigned char lrc = 0;
        for (const auto byte : request) lrc ^= byte;
        if (lrc != 0) return;
    } else {
        std::uint16_t crc = 0xffff;
        for (const auto byte : request.first(request.size() - 2)) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc & 1) != 0 ? static_cast<std::uint16_t>((crc >> 1) ^ 0x8408)
                                     : static_cast<std::uint16_t>(crc >> 1);
        }
        crc = static_cast<std::uint16_t>(~crc);
        if (request[request.size() - 2] != static_cast<unsigned char>(crc & 0xff) ||
            request.back() != static_cast<unsigned char>(crc >> 8)) return;
    }
    scan_card_t1_reply.clear();
    scan_card_t1_reply_offset = 0;
    scan_card_t1_reply.push_back(request[0]);
    scan_card_t1_last_outgoing_pcb.store(request[1], std::memory_order_release);
    if (scan_card_t1_bwt_mode.load(std::memory_order_acquire) != 0 &&
        (request[1] & 0xc0) == 0x80) {
        if (scan_card_t1_cwt_fault.load(std::memory_order_acquire) == 102)
            scan_card_t1_apdu_drop_r_reply.store(true, std::memory_order_release);
        const auto pcb = scan_card_t1_apdu_response_sequence.load(std::memory_order_acquire)
            ? 0x40 : 0x00;
        scan_card_t1_reply.push_back(static_cast<unsigned char>(pcb));
        scan_card_t1_reply.push_back(4);
        scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x11, 0x22, 0x90, 0x00});
        scan_card_t1_apdu_response_sequence.store(pcb == 0, std::memory_order_release);
    } else if (scan_card_t1_apdu_mode.load(std::memory_order_acquire)) {
        const auto fault = scan_card_t1_apdu_fault.load(std::memory_order_acquire);
        auto pcb = static_cast<unsigned char>(request[1] & 0x40);
        const auto expected_nr = static_cast<unsigned char>((request[1] & 0x40) != 0 ? 0x10 : 0);
        bool duplicate_i_block = false;
        bool send_r_block = fault == 9;
        unsigned error_code = 0;
        if ((fault == 51 || fault == 52 || fault == 55 || fault == 56 || fault == 57 ||
             fault == 68 || fault == 69 || fault == 70) &&
            request[1] == 0xe3) {
            const auto exchange_count = scan_card_t1_wtx_exchange_count.fetch_add(1,
                std::memory_order_acq_rel) + 1;
            if (fault == 52 && exchange_count == 1) {
                pcb = 0xc3;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(1);
                scan_card_t1_reply.push_back(2);
            } else {
                pcb = scan_card_t1_apdu_response_sequence.load(std::memory_order_acquire)
                    ? 0x40 : 0x00;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(4);
                scan_card_t1_reply.insert(scan_card_t1_reply.end(),
                    {0x11, 0x22, 0x90, 0x00});
                scan_card_t1_apdu_response_sequence.store(pcb == 0,
                    std::memory_order_release);
            }
        } else if ((fault == 51 || fault == 52 || fault == 55 || fault == 56 || fault == 57 ||
                    fault == 68 || fault == 69 || fault == 70) &&
                   (request[1] & 0x80) == 0) {
            scan_card_t1_wtx_exchange_count.store(0, std::memory_order_release);
            pcb = 0xc3;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(1);
            scan_card_t1_reply.push_back(fault == 68 ? 60 : fault == 69 ? 255 :
                fault == 70 ? 101 : 2);
        } else if (fault == 53 && (request[1] & 0x80) == 0) {
            pcb = 0xc3;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(1);
            scan_card_t1_reply.push_back(0);
        } else if (fault == 54 && request[1] == 0xe3) {
            pcb = scan_card_t1_apdu_response_sequence.load(std::memory_order_acquire)
                ? 0x40 : 0x00;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(4);
            scan_card_t1_reply.insert(scan_card_t1_reply.end(),
                {0x11, 0x22, 0x90, 0x00});
        } else if (fault == 54 && (request[1] & 0xc0) == 0x80) {
            pcb = scan_card_t1_apdu_response_sequence.load(std::memory_order_acquire)
                ? 0x40 : 0x00;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(4);
            scan_card_t1_reply.insert(scan_card_t1_reply.end(),
                {0x11, 0x22, 0x90, 0x00});
            scan_card_t1_apdu_response_sequence.store(pcb == 0,
                std::memory_order_release);
        } else if (fault == 54 && (request[1] & 0x80) == 0) {
            pcb = 0xc3;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(1);
            scan_card_t1_reply.push_back(2);
        } else if (fault >= 40 && request[1] == 0xc1) {
            scan_card_t1_apdu_ifs_negotiated.store(true, std::memory_order_release);
            scan_card_t1_host_ifs_chain_state.store(0, std::memory_order_release);
            if (fault == 45)
                scan_card_transport_drop_reply.store(true, std::memory_order_release);
            pcb = fault == 42 ? 0xc1 : 0xe1;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(fault == 43 ? 2 : 1);
            scan_card_t1_reply.push_back(fault == 47 ? 0 : fault == 48 ? 0xff :
                fault == 41 ? static_cast<unsigned char>(request[3] ^ 1) : request[3]);
            if (fault == 43) scan_card_t1_reply.push_back(0);
            if (fault == 46) scan_card_t1_reply[0] = 1;
        } else if ((fault == 40 || fault == 49) && request[1] != 0xc1) {
            const auto chain_state = scan_card_t1_host_ifs_chain_state.load(
                std::memory_order_acquire);
            if ((request[1] & 0xc0) == 0x80 && chain_state == 1) {
                pcb = 0x40;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(4);
                scan_card_t1_reply.insert(scan_card_t1_reply.end(),
                    {0x90, 0x00, 0x62, 0x83});
                scan_card_t1_apdu_response_sequence.store(true, std::memory_order_release);
                scan_card_t1_host_ifs_chain_state.store(2, std::memory_order_release);
            } else if (chain_state == 0 && (request[1] & 0x80) == 0) {
                pcb = 0x20;
                scan_card_t1_reply.push_back(pcb);
                const auto length = fault == 49 ? 17u : 16u;
                scan_card_t1_reply.push_back(static_cast<unsigned char>(length));
                for (unsigned index = 0; index < length; ++index)
                    scan_card_t1_reply.push_back(static_cast<unsigned char>(index + 1));
                scan_card_t1_apdu_response_sequence.store(false, std::memory_order_release);
                scan_card_t1_host_ifs_chain_state.store(1, std::memory_order_release);
            } else if (chain_state == 2 && (request[1] & 0x80) == 0) {
                pcb = 0x00;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(2);
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x90, 0x00});
                scan_card_t1_host_ifs_chain_state.store(3, std::memory_order_release);
            } else {
                pcb = 0;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(2);
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x90, 0x00});
            }
        } else if (fault == 50) {
            pcb = scan_card_t1_apdu_response_sequence.load(std::memory_order_acquire)
                ? 0x40 : 0x00;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(4);
            const auto ins = request[3 + (request[2] > 1 ? 1 : 0)];
            if (ins == 0xca)
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x33, 0x44, 0x90, 0x00});
            else
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x11, 0x22, 0x90, 0x00});
            if (scan_card_t1_cwt_fault.load(std::memory_order_acquire) < 100)
                scan_card_t1_apdu_response_sequence.store(pcb == 0,
                    std::memory_order_release);
        } else if (fault >= 30) {
            if (request[1] == 0xe1) {
                scan_card_t1_apdu_ifs_negotiated.store(true, std::memory_order_release);
                if (fault == 36)
                    scan_card_transport_drop_reply.store(true, std::memory_order_release);
                pcb = scan_card_t1_apdu_response_sequence.load(std::memory_order_acquire)
                    ? 0x40 : 0x00;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(4);
                scan_card_t1_reply.insert(scan_card_t1_reply.end(),
                    {0x11, 0x22, 0x90, 0x00});
                scan_card_t1_apdu_response_sequence.store(pcb == 0,
                    std::memory_order_release);
            } else if ((request[1] & 0xc0) == 0x80) {
                pcb = static_cast<unsigned char>(0x80 |
                    (((request[1] & 0x40) == 0) ? 0x10 : 0));
                const auto ack_index = scan_card_t1_apdu_ack_count.fetch_add(1,
                    std::memory_order_acq_rel);
                if (ack_index < 32 && (pcb & 0x10) != 0)
                    scan_card_t1_apdu_ack_trace.fetch_or(1u << ack_index,
                        std::memory_order_acq_rel);
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(0);
            } else if (!scan_card_t1_apdu_ifs_negotiated.load(std::memory_order_acquire) &&
                       !scan_card_t1_apdu_ifs_requested.exchange(true,
                           std::memory_order_acq_rel)) {
                pcb = fault == 33 ? 0xe1 : fault == 35 ? 0xc3 : 0xc1;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(fault == 34 ? 2 : 1);
                scan_card_t1_reply.push_back(fault == 31 ? 0 : fault == 32 ? 0xff : 0x10);
                if (fault == 34) scan_card_t1_reply.push_back(0x11);
            } else if ((request[1] & 0x20) != 0) {
                pcb = static_cast<unsigned char>(0x80 |
                    (((request[1] & 0x40) == 0) ? 0x10 : 0));
                const auto ack_index = scan_card_t1_apdu_ack_count.fetch_add(1,
                    std::memory_order_acq_rel);
                if (ack_index < 32 && (pcb & 0x10) != 0)
                    scan_card_t1_apdu_ack_trace.fetch_or(1u << ack_index,
                        std::memory_order_acq_rel);
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(0);
            } else {
                pcb = scan_card_t1_apdu_response_sequence.load(std::memory_order_acquire)
                    ? 0x40 : 0x00;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(4);
                scan_card_t1_reply.insert(scan_card_t1_reply.end(),
                    {0x11, 0x22, 0x90, 0x00});
                scan_card_t1_apdu_response_sequence.store(pcb == 0,
                    std::memory_order_release);
            }
            scan_card_t1_reply[2] = static_cast<unsigned char>(scan_card_t1_reply.size() - 3);
        } else if (fault >= 22 && fault <= 26) {
            const auto is_r_block = (request[1] & 0xc0) == 0x80;
            const auto more_command = !is_r_block && (request[1] & 0x20) != 0;
            if (more_command) {
                const auto same_sequence = (request[1] & 0x40) != 0;
                pcb = fault == 25 ? static_cast<unsigned char>(same_sequence ? 0x40 : 0x00)
                    : static_cast<unsigned char>(0x80 | ((same_sequence ? 0x00 : 0x10)));
                if (fault == 23 && !scan_card_t1_apdu_rblock_sent.exchange(true,
                        std::memory_order_acq_rel))
                    pcb = static_cast<unsigned char>(0x80 | (same_sequence ? 0x10 : 0x00));
                if (fault == 24) pcb |= 0x20;
                if (fault == 26)
                    scan_card_t1_apdu_drop_r_reply.store(true, std::memory_order_release);
                if ((pcb & 0xc0) == 0x80) {
                    const auto ack_index = scan_card_t1_apdu_ack_count.fetch_add(1,
                        std::memory_order_acq_rel);
                    if (ack_index < 32 && (pcb & 0x10) != 0)
                        scan_card_t1_apdu_ack_trace.fetch_or(1u << ack_index,
                            std::memory_order_acq_rel);
                }
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(0);
            } else if (!is_r_block) {
                pcb = scan_card_t1_apdu_response_sequence.load(std::memory_order_acquire)
                    ? 0x40 : 0x00;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(4);
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x11, 0x22, 0x90, 0x00});
                scan_card_t1_apdu_response_sequence.store(pcb == 0,
                    std::memory_order_release);
            } else {
                pcb = 0x00;
                scan_card_t1_reply.push_back(pcb);
                scan_card_t1_reply.push_back(4);
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x11, 0x22, 0x90, 0x00});
            }
            scan_card_t1_reply[2] = static_cast<unsigned char>(scan_card_t1_reply.size() - 3);
        } else if (fault == 18 || fault == 19) {
            const auto is_r_block = (request[1] & 0xc0) == 0x80;
            auto chain_index = 0u;
            if (is_r_block)
                chain_index = scan_card_t1_apdu_chain_index.fetch_add(1,
                    std::memory_order_acq_rel) + 1;
            else
                scan_card_t1_apdu_chain_index.store(0, std::memory_order_release);
            const auto block_count = fault == 19 ? 3u : 2u;
            const auto response_sequence = static_cast<unsigned char>((chain_index & 1u) != 0
                ? 0x40 : 0x00);
            const auto more = chain_index + 1 < block_count;
            pcb = static_cast<unsigned char>(response_sequence | (more ? 0x20 : 0x00));
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(0);
            if (fault == 18) {
                if (chain_index == 0)
                    scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x11, 0x22});
                else
                    scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x90, 0x00});
            } else if (chain_index == 0) {
                scan_card_t1_reply.push_back(0x11);
            } else if (chain_index == 1) {
                scan_card_t1_reply.push_back(0x22);
            } else {
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x90, 0x00});
            }
            scan_card_t1_reply[2] = static_cast<unsigned char>(scan_card_t1_reply.size() - 3);
        } else if (fault == 21) {
            const auto is_r_block = (request[1] & 0xc0) == 0x80;
            pcb = is_r_block ? 0 : 0x20;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(2);
            if (is_r_block)
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0xde, 0xad});
            else
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x11, 0x22});
        } else if (fault == 20) {
            const auto ins = request[3 + (request[2] > 1 ? 1 : 0)];
            pcb = 0;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(4);
            if (ins == 0xca)
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x33, 0x44, 0x90, 0x00});
            else
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x11, 0x22, 0x90, 0x00});
        } else if (fault == 13 || fault == 14 || fault == 15) {
            if ((request[1] & 0xc0) == 0x80) {
                if (fault == 15)
                    scan_card_t1_apdu_drop_r_reply.store(true, std::memory_order_release);
                pcb = static_cast<unsigned char>((request[1] & 0x10) != 0 ? 0x40 : 0);
                duplicate_i_block = fault == 14;
                if (duplicate_i_block) pcb ^= 0x40;
            } else {
                const auto ins = request[3 + (request[2] > 1 ? 1 : 0)];
                scan_card_t1_apdu_pending_ins.store(ins, std::memory_order_release);
                duplicate_i_block = fault == 14 ||
                    !scan_card_t1_apdu_rblock_sent.exchange(true, std::memory_order_acq_rel);
                if (duplicate_i_block) pcb ^= 0x40;
            }
        } else if ((fault == 5 || fault == 8 || fault == 12) &&
            !scan_card_t1_apdu_rblock_sent.exchange(true, std::memory_order_acq_rel)) {
            send_r_block = true;
            error_code = fault == 8 ? 1 : 0;
        } else if (fault == 6 || fault == 10 || fault == 11) {
            send_r_block = true;
            error_code = 0;
            pcb = static_cast<unsigned char>(0x80 | (expected_nr ^ 0x10));
        } else if (fault == 7) {
            send_r_block = true;
            error_code = 3;
        }
        if (fault < 22 && fault != 18 && fault != 19 && fault != 20 && fault != 21 && send_r_block) {
            pcb = static_cast<unsigned char>(0x80 | expected_nr | error_code);
            if (fault == 6) pcb = static_cast<unsigned char>(0x80 | (expected_nr ^ 0x10));
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(fault == 10 ? 1 : 0);
            if (fault == 10) scan_card_t1_reply.push_back(0xaa);
            if (fault == 11) scan_card_t1_reply[0] = 1;
        } else if (fault < 22 && fault != 18 && fault != 19 && fault != 20 && fault != 21) {
            if (fault == 1) pcb ^= 0x40;
            else if (fault == 3) pcb |= 0x20;
            else if (fault == 4) pcb = 0xc0;
            scan_card_t1_reply.push_back(pcb);
            scan_card_t1_reply.push_back(0);
            if (duplicate_i_block) {
                scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0xde, 0xad, 0x90, 0x00});
            } else {
                const auto ins = (fault == 13 || fault == 14) &&
                    (request[1] & 0xc0) == 0x80
                    ? scan_card_t1_apdu_pending_ins.load(std::memory_order_acquire)
                    : request[3 + (request[2] > 1 ? 1 : 0)];
                if (ins == 0xa4) {
                    scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x11, 0x22, 0x90, 0x00});
                } else if (ins == 0xca) {
                    scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x33, 0x44, 0x90, 0x00});
                } else {
                    scan_card_t1_reply.insert(scan_card_t1_reply.end(), {0x55, 0x66, 0x90, 0x00});
                }
            }
            scan_card_t1_reply[2] = static_cast<unsigned char>(scan_card_t1_reply.size() - 3);
        }
    } else {
        scan_card_t1_reply.push_back(static_cast<unsigned char>(request[1] ^ 0x40));
        scan_card_t1_reply.push_back(2);
        scan_card_t1_reply.push_back(scan_card_t1_stale_reply.load(std::memory_order_acquire)
            ? 0x6f : 0x90);
        scan_card_t1_reply.push_back(0x00);
    }
    if (edc_size == 1) {
        unsigned char lrc = 0;
        for (const auto byte : scan_card_t1_reply) lrc ^= byte;
        scan_card_t1_reply.push_back(lrc);
    } else {
        std::uint16_t crc = 0xffff;
        for (const auto byte : scan_card_t1_reply) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc & 1) != 0 ? static_cast<std::uint16_t>((crc >> 1) ^ 0x8408)
                                     : static_cast<std::uint16_t>(crc >> 1);
        }
        crc = static_cast<std::uint16_t>(~crc);
        scan_card_t1_reply.push_back(static_cast<unsigned char>(crc & 0xff));
        scan_card_t1_reply.push_back(static_cast<unsigned char>(crc >> 8));
    }
    const auto fault = scan_card_t1_fault.load(std::memory_order_acquire);
    if (fault == 1) scan_card_t1_reply.pop_back();
    else if (fault == 2) scan_card_t1_reply.push_back(0x55);
    else if (fault == 3) scan_card_t1_reply.back() ^= 0x01;
    if (scan_card_t1_apdu_fault.load(std::memory_order_acquire) == 37)
        scan_card_t1_reply.back() ^= 0x01;
    if (scan_card_t1_apdu_fault.load(std::memory_order_acquire) == 44)
        scan_card_t1_reply.back() ^= 0x01;
    scan_card_t1_reply_offset = 0;
    scan_card_t1_bwt_decided.store(false, std::memory_order_release);
    scan_card_t1_bwt_current_late.store(false, std::memory_order_release);
}

dtv_internal_result select_scan_group(
    const dtv_usb_candidate_info *candidates, std::uint32_t candidate_count,
    std::uint32_t anchor, dtv_usb_function_selection *out,
    std::uint32_t capacity, std::uint32_t *out_count,
    dtv_usb_group_match *match)
{
    scan_group_thread = std::this_thread::get_id();
    *out_count = 0;
    *match = DTV_USB_GROUP_NO_MATCH;
    if (scan_group_case == ScanGroupCase::callback_error) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 23);
    }
    if (scan_group_case == ScanGroupCase::callback_exception) {
        throw std::bad_alloc();
    }
    if (scan_group_case == ScanGroupCase::no_match) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_group_case == ScanGroupCase::ambiguous) {
        *match = DTV_USB_GROUP_AMBIGUOUS;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }

    std::uint32_t single = candidate_count;
    std::uint32_t first = candidate_count;
    std::uint32_t second = candidate_count;
    for (std::uint32_t index = 0; index < candidate_count; ++index) {
        if (candidates[index].product_id == 0x0101) single = index;
        if (candidates[index].product_id == 0x0201) first = index;
        if (candidates[index].product_id == 0x0202) second = index;
    }
    if (single == candidate_count && first == candidate_count && second == candidate_count) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (capacity == 0) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);

    if (single != candidate_count) {
        *match = DTV_USB_GROUP_COMPLETE;
        *out_count = 1;
        out[0] = {scan_group_case == ScanGroupCase::bad_index ? candidate_count : single,
                  1, static_cast<std::uint8_t>(
                         scan_group_case == ScanGroupCase::bad_interface ? 9 : 0), 0};
    } else if (first != candidate_count && second != candidate_count) {
        if (capacity < 2) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        *match = DTV_USB_GROUP_COMPLETE;
        *out_count = 2;
        out[0] = {scan_group_case == ScanGroupCase::bad_index ? candidate_count : first, 1, 0, 0};
        out[1] = {second, 2, static_cast<std::uint8_t>(
                                scan_group_case == ScanGroupCase::bad_interface ? 9 : 0), 0};
    } else {
        const auto present = first != candidate_count ? first : second;
        *match = DTV_USB_GROUP_INCOMPLETE;
        *out_count = 1;
        out[0] = {present, first != candidate_count ? 1u : 2u, 0, 0};
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

const dtv_usb_identity scan_identities[]{{0x1234, 0x0101}, {0x1234, 0x0201},
                                         {0x1234, 0x0202}};

dtv_internal_result scan_open_plan(
    std::span<const dtv_usb_candidate_info> candidates,
    std::span<const dtv_function>, std::span<dtv::spi::PublicOpenPlan> plans,
    std::size_t *count)
{
    if (scan_public_open_case == 1) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
    }
    if (count == nullptr || candidates.empty() || plans.size() < candidates.size()) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
    }
    *count = 0;
    for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
        const auto &candidate = candidates[candidate_index];
        for (std::uint32_t interface_index = 0;
             interface_index < candidate.interface_count; ++interface_index) {
            const auto &interface = candidate.interfaces[interface_index];
            for (std::uint32_t endpoint_index = 0;
                 endpoint_index < interface.endpoint_count; ++endpoint_index) {
                const auto &endpoint = interface.endpoints[endpoint_index];
                const auto wanted_endpoint = (scan_public_open_case == 9 ||
                    scan_public_open_case == 10 || scan_public_open_case == 11 ||
                    scan_public_open_case == 12 || scan_public_open_case == 13 ||
                    scan_public_open_case == 14 || scan_public_open_case == 15 ||
                    scan_public_open_case == 16 || scan_public_open_case == 17 ||
                    scan_public_open_case == 18) &&
                    candidate_index == 1
                    ? 0x82 : 0x81;
                if ((endpoint.address == wanted_endpoint && (endpoint.attributes & 0x03u) == 0x02u) ||
                    scan_public_open_case == 2) {
                    plans[*count] = {candidate_index,
                        static_cast<std::uint8_t>(scan_public_open_case == 2 ? 0x82 : wanted_endpoint), 32};
                    if (scan_public_open_case == 9 || scan_public_open_case == 10 ||
                        scan_public_open_case == 11 || scan_public_open_case == 12 ||
                        scan_public_open_case == 13 || scan_public_open_case == 14 ||
                        scan_public_open_case == 15 || scan_public_open_case == 16 ||
                        scan_public_open_case == 17 || scan_public_open_case == 18) {
                        plans[*count].shared_in_depth = scan_public_open_case == 10 && candidate_index == 0
                            ? 2 : 1;
                        plans[*count].prestream_sink_limit = 16;
                        if (scan_public_open_case == 13) {
                            plans[*count].prestream_sink_limit = 376;
                        }
                        plans[*count].reply_capacity = 32;
                        plans[*count].card_reader_source =
                            (scan_public_open_case == 12 || scan_public_open_case == 15 ||
                             scan_public_open_case == 16 || scan_public_open_case == 17 ||
                             scan_public_open_case == 18) &&
                            candidate_index == 0;
                        plans[*count].receiver_lock_source =
                            scan_public_open_case == 12 && candidate_index == 0;
                    }
                    ++*count;
                    break;
                }
            }
            if (*count > candidate_index) break;
        }
    }
    return *count == candidates.size() ? dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0)
                                       : dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
}

dtv_internal_result scan_open_identify(dtv_context *, std::span<const unsigned char> bytes,
                                       dtv_timeout, const std::atomic<bool> *)
{
    scan_open_identify_bytes = bytes.size();
    if (scan_public_open_case == 8 && scan_open_identify_calls.fetch_add(1) == 0) {
        scan_open_identify_blocked.store(true, std::memory_order_release);
        scan_open_identify_blocked.notify_all();
        while (!scan_open_identify_release.load(std::memory_order_acquire)) {
            scan_open_identify_release.wait(false);
        }
    }
    if (scan_public_open_case == 3) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0);
    }
    return bytes.empty() ? dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0)
                         : dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_shared_demux(dtv_context *, const dtv::spi::PublicOpenPacket *packet,
                                      dtv::spi::PublicOpenMessage *message)
{
    if (packet == nullptr || message == nullptr || packet->handle_index >= 2 ||
        packet->actual_length > packet->bytes.size()) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    const auto index = packet->handle_index;
    scan_shared_sequences[index] = packet->submit_sequence;
    scan_shared_lengths[index] = packet->actual_length;
    const auto packet_index = scan_shared_packets.load(std::memory_order_relaxed);
    if (packet_index < scan_shared_order.size()) {
        scan_shared_order[packet_index] = packet->submit_sequence;
        scan_shared_order_endpoints[packet_index] = packet->endpoint;
    }
    scan_shared_packets.fetch_add(1, std::memory_order_release);
    if (scan_public_open_case == 18 &&
        scan_card_interleave_phase.load(std::memory_order_acquire) != 0) {
        const auto phase = scan_card_interleave_phase.load(std::memory_order_acquire);
        if (phase == 1 && packet->endpoint == 0x81) {
            message->kind = dtv::spi::PublicOpenMessageKind::notification;
            message->notification_kind = dtv::spi::PublicOpenNotificationKind::card_state;
            message->generation = packet->generation;
            message->card_state = DTV_CARD_ABSENT;
            scan_card_interleave_phase.store(2, std::memory_order_release);
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        if (phase == 2 && packet->endpoint == 0x82) {
            static thread_local std::array<unsigned char, 5 * 192> source{};
            for (std::size_t packet_index = 0; packet_index < 5; ++packet_index) {
                const auto offset = packet_index * 192;
                std::fill(source.begin() + offset, source.begin() + offset + 4, 0xa0);
                source[offset + 4] = 0x47;
                source[offset + 5] = static_cast<unsigned char>(0x81 + packet_index);
                for (std::size_t index = 6; index < 192; ++index)
                    source[offset + index] = static_cast<unsigned char>((offset + index) * 11);
            }
            message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
            message->receiver_index = 1;
            message->stream_generation = scan_stream_generation.load(std::memory_order_acquire);
            message->generation = packet->generation;
            message->payload = source;
            scan_card_interleave_phase.store(3, std::memory_order_release);
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        if (phase == 3 && packet->endpoint == 0x81) {
            static thread_local std::array<unsigned char, 260> reply{};
            std::size_t size = 0;
            {
                std::lock_guard lock(scan_card_t1_mutex);
                size = scan_card_t1_reply.size();
                std::copy(scan_card_t1_reply.begin(), scan_card_t1_reply.end(), reply.begin());
            }
            const auto bwt_mode = scan_card_t1_bwt_mode.load(std::memory_order_acquire);
            const auto outgoing_is_r =
                (scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) & 0xc0) == 0x80;
            const auto bwt_timeout = bwt_mode == 1 && !outgoing_is_r
                ? scan_card_t1_bwt_timeout_count.fetch_add(1, std::memory_order_acq_rel) == 0
                : bwt_mode == 2
                    ? scan_card_t1_bwt_timeout_count.fetch_add(1, std::memory_order_acq_rel) < 2
                    : false;
            if (bwt_timeout) {
                if (size > 1)
                    scan_card_t1_apdu_response_sequence.store((reply[1] & 0x40) != 0,
                        std::memory_order_release);
            }
            message->kind = dtv::spi::PublicOpenMessageKind::control_reply;
            message->resource_scope = 18;
            message->transaction_id = scan_card_t1_current_transaction.load(std::memory_order_acquire);
            message->generation = packet->generation;
            message->payload = std::span<const unsigned char>(reply).first(size);
            message->t1_bwt.observation = dtv::spi::CardT1BwtObservation::first_character;
            message->t1_bwt.host_transaction_id =
                scan_card_t1_last_write_transaction.load(std::memory_order_acquire);
            message->t1_bwt.card_transaction_id = message->transaction_id;
            message->t1_bwt.host_last_character_edge_ns = 1000000000;
            message->t1_bwt.card_first_character_edge_ns = bwt_timeout ? 3000000000 : 1001000000;
            message->t1_bwt.absence_observed_through_ns = 0;
            message->t1_bwt.clock_domain = 71;
            message->t1_bwt.error_bound_ns = 1;
            message->t1_bwt.error_bound_known = true;
            message->t1_bwt.evidence_id = 1;
            message->t1_bwt.revision = 1;
            message->t1_bwt.connection_epoch =
                scan_card_t1_connection_epoch.load(std::memory_order_acquire);
            message->t1_bwt.request_epoch =
                scan_card_t1_request_epoch.load(std::memory_order_acquire);
            scan_card_interleave_phase.store(4, std::memory_order_release);
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        if (phase == 4 && packet->endpoint == 0x81) {
            message->kind = dtv::spi::PublicOpenMessageKind::notification;
            message->notification_kind = dtv::spi::PublicOpenNotificationKind::card_state;
            message->generation = packet->generation;
            message->card_state = DTV_CARD_PRESENT;
            scan_card_interleave_phase.store(5, std::memory_order_release);
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 18 && packet->endpoint == 0x81) {
        const auto kind = scan_card_transport_kind.load(std::memory_order_acquire);
        if (scan_card_t1_apdu_fault.load(std::memory_order_acquire) == 12 && kind == 5 &&
            scan_card_t1_apdu_reply_count.fetch_add(1, std::memory_order_acq_rel) != 0)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        if ((scan_card_t1_apdu_fault.load(std::memory_order_acquire) == 15 ||
             scan_card_t1_apdu_fault.load(std::memory_order_acquire) == 26) &&
            scan_card_t1_apdu_drop_r_reply.load(std::memory_order_acquire) &&
            (kind == 4 || kind == 5))
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        if (scan_card_transport_drop_reply.load(std::memory_order_acquire) && kind != 6)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        static thread_local std::array<unsigned char, 260> reply{};
        std::size_t size = 4;
        if (kind == 0) {
            if (scan_card_t1_crc.load(std::memory_order_acquire)) {
                const auto bwt_mode = scan_card_t1_bwt_mode.load(std::memory_order_acquire);
                if (bwt_mode == 8 || bwt_mode == 9 || bwt_mode == 10 || bwt_mode == 11) {
                    reply = {0, 0x3b, 0x90, 0xb1, 0xd1, 0x01, 0x00, 0xf1,
                        0xfe, static_cast<unsigned char>(bwt_mode == 10 ? 0x9e :
                            bwt_mode == 11 ? 0x9f : 0x90), 0x01, 0x01,
                        static_cast<unsigned char>(bwt_mode == 10 ? 0x60 :
                            bwt_mode == 11 ? 0x61 : 0x6e)};
                    size = 13;
                } else {
                    reply = {0, 0x3b, 0x80, 0x81, 0x41, 0x01, 0x41, 0};
                    size = 7;
                }
            } else {
                reply = {0, 0x3b, 0x80, 0x01, 0x81, 0, 0, 0};
                size = 5;
            }
        } else if (kind == 1) {
            reply = {0xac, 0, 0, 0, 0, 0, 0, 0};
            size = 1;
        } else if (kind == 2) {
            reply = {0x5a, 0xc3, 0, 0, 0, 0, 0, 0};
            size = 2;
        } else if (kind == 6) {
            reply[0] = 0xac;
            size = 1;
        } else if (kind == 4 || kind == 5) {
            std::lock_guard lock(scan_card_t1_mutex);
            if (kind == 4) {
                size = scan_card_t1_pending.size();
                std::copy(scan_card_t1_pending.begin(), scan_card_t1_pending.end(), reply.begin());
                scan_card_t1_pending.clear();
            } else {
                size = scan_card_t1_reply.size();
                std::copy(scan_card_t1_reply.begin(), scan_card_t1_reply.end(), reply.begin());
            }
        } else {
            reply = {0x00, 0x00, 0x01, 0x90, 0x00, 0, 0, 0};
            size = 5;
        }
        message->kind = dtv::spi::PublicOpenMessageKind::control_reply;
        message->resource_scope = kind == 0 ? 17 : 18;
        message->transaction_id = kind == 0 ? scan_card_transaction.load(std::memory_order_acquire)
            : (kind >= 4 ? scan_card_t1_current_transaction.load(std::memory_order_acquire)
                         : 200 + kind);
        message->generation = packet->generation;
        message->payload = std::span<const unsigned char>(reply).first(size);
        if (kind == 4 || kind == 5) {
            const auto bwt_mode = scan_card_t1_bwt_mode.load(std::memory_order_acquire);
            const auto outgoing_is_r =
                (scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) & 0xc0) == 0x80;
            if (!scan_card_t1_bwt_decided.exchange(true, std::memory_order_acq_rel)) {
                const auto bwt_timeout = bwt_mode == 1 && !outgoing_is_r
                    ? scan_card_t1_bwt_timeout_count.fetch_add(1, std::memory_order_acq_rel) == 0
                    : bwt_mode == 2
                        ? scan_card_t1_bwt_timeout_count.fetch_add(1, std::memory_order_acq_rel) < 2
                        : false;
                scan_card_t1_bwt_current_late.store(bwt_timeout, std::memory_order_release);
                if (bwt_mode == 3) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            const auto bwt_timeout =
                scan_card_t1_bwt_current_late.load(std::memory_order_acquire);
            if (bwt_timeout) {
                if (size > 1)
                    scan_card_t1_apdu_response_sequence.store((reply[1] & 0x40) != 0,
                        std::memory_order_release);
                message->payload = std::span<const unsigned char>(reply).first(0);
                size = 0;
            }
            message->t1_bwt.observation = bwt_timeout
                ? dtv::spi::CardT1BwtObservation::absence_confirmed
                : dtv::spi::CardT1BwtObservation::first_character;
            message->t1_bwt.host_transaction_id =
                scan_card_t1_last_write_transaction.load(std::memory_order_acquire);
            message->t1_bwt.card_transaction_id = message->transaction_id;
            message->t1_bwt.host_last_character_edge_ns = 1000000000;
            message->t1_bwt.card_first_character_edge_ns = bwt_timeout ? 0 : 1001000000;
            message->t1_bwt.absence_observed_through_ns = bwt_timeout ? 3000000000 : 0;
            message->t1_bwt.clock_domain = 71;
            message->t1_bwt.error_bound_ns = 1;
            message->t1_bwt.error_bound_known = true;
            message->t1_bwt.evidence_id = 1;
            message->t1_bwt.revision = 1;
            message->t1_bwt.connection_epoch =
                scan_card_t1_connection_epoch.load(std::memory_order_acquire);
            message->t1_bwt.request_epoch =
                scan_card_t1_request_epoch.load(std::memory_order_acquire);
            const auto apdu_fault = scan_card_t1_apdu_fault.load(std::memory_order_acquire);
            if (apdu_fault == 57 &&
                scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) == 0x00) {
                scan_card_t1_bgt_etu_upper_bound_ns.store(1000000000,
                    std::memory_order_release);
                scan_card_t1_bgt_revision.store(204, std::memory_order_release);
            }
            const auto wtx_timeout = apdu_fault == 54 &&
                scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) == 0xe3;
            if (wtx_timeout) {
                message->payload = std::span<const unsigned char>(reply).first(0);
                message->t1_bwt.observation = dtv::spi::CardT1BwtObservation::absence_confirmed;
                message->t1_bwt.host_last_character_edge_ns = 2000000000;
                message->t1_bwt.absence_observed_through_ns = 7000000000;
            } else if ((apdu_fault == 51 || apdu_fault == 52 || apdu_fault == 56) &&
                scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) == 0xe3) {
                const auto exchange_count = scan_card_t1_wtx_exchange_count.load(
                    std::memory_order_acquire);
                message->t1_bwt.host_last_character_edge_ns = exchange_count == 1
                    ? 2000000000 : 4000000000;
                message->t1_bwt.card_first_character_edge_ns = apdu_fault == 52 &&
                    exchange_count == 1 ? 2001000000 :
                    (exchange_count == 1 ? 4859005000 : 6859005000);
            }
            if (apdu_fault == 56 &&
                scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) == 0xe3)
                ++message->t1_bwt.request_epoch;
            if (apdu_fault == 55 &&
                scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) == 0x00)
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            if (!bwt_timeout && (size >= 2 ||
                scan_card_t1_cwt_fault.load(std::memory_order_acquire) >= 100)) {
                auto &cwt = message->t1_bwt.t1_cwt;
                cwt.host_transaction_id = scan_card_t1_last_write_transaction.load(
                    std::memory_order_acquire);
                cwt.card_transaction_id = scan_card_t1_cwt_card_transaction.load(
                    std::memory_order_acquire);
                cwt.clock_domain = message->t1_bwt.clock_domain;
                cwt.error_bound_ns = message->t1_bwt.error_bound_ns;
                cwt.error_bound_known = message->t1_bwt.error_bound_known;
                cwt.evidence_id = message->t1_bwt.evidence_id;
                cwt.revision = message->t1_bwt.revision;
                cwt.connection_epoch = message->t1_bwt.connection_epoch;
                cwt.request_epoch = message->t1_bwt.request_epoch;
                {
                    std::lock_guard lock(scan_card_t1_mutex);
                    cwt.received_octet_count = scan_card_t1_reply.size();
                    cwt.adjacent_gap_bounds.assign(cwt.received_octet_count - 1,
                        {1000, 1000});
                }
                const auto cwt_fault = scan_card_t1_cwt_fault.load(std::memory_order_acquire);
                if (cwt_fault >= 100) {
                    std::lock_guard lock(scan_card_t1_mutex);
                    cwt.received_octet_count = (std::min)(scan_card_t1_reply_offset,
                        scan_card_t1_reply.size());
                    cwt.received_octets.assign(scan_card_t1_reply.begin(),
                        scan_card_t1_reply.begin() +
                            static_cast<std::ptrdiff_t>(cwt.received_octet_count));
                    cwt.adjacent_gap_bounds.assign(cwt.received_octet_count == 0 ? 0 :
                        cwt.received_octet_count - 1, {1000, 1000});
                    if (size == 0 && cwt.received_octet_count >= 2) {
                        const auto cwt_duration = scan_card_t1_cwt_duration_ns.load(
                            std::memory_order_acquire);
                        const auto extra_quiet_ns = cwt_fault == 101
                            ? 1000000 : 100000000;
                        cwt.silence_observation =
                            dtv::spi::CardT1CwtSilenceObservation::continuous_no_io;
                        scan_card_t1_cwt_silence_delivered.store(true,
                            std::memory_order_release);
                        cwt.last_character_edge_ns = 1000000000;
                        cwt.io_silence_observed_through_ns =
                            cwt.last_character_edge_ns + cwt_duration + extra_quiet_ns;
                    }
                }
                if (cwt_fault >= 91 && cwt_fault <= 99 &&
                    scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) == 0x00 &&
                    cwt.adjacent_gap_bounds.size() > 2) {
                    const auto cwt_duration = scan_card_t1_cwt_duration_ns.load(
                        std::memory_order_acquire);
                    auto &gap = cwt.adjacent_gap_bounds[2];
                    if (cwt_fault == 91)
                        gap = {cwt_duration + 3, cwt_duration + 3};
                    else if (cwt_fault == 92)
                        gap = {cwt_duration - 2, cwt_duration - 2};
                    else if (cwt_fault == 93)
                        gap = {cwt_duration - 1, cwt_duration + 1};
                    else if (cwt_fault == 94)
                        cwt = {};
                    else if (cwt_fault == 95)
                        ++cwt.clock_domain;
                    else if (cwt_fault == 96)
                        ++cwt.received_octet_count;
                    else if (cwt_fault == 97)
                        gap = {100000000, 100000000};
                    else if (cwt_fault == 98)
                        gap = {1000000000, 1000000000};
                    else if (cwt_fault == 99)
                        gap = {6000000000, 6000000000};
                }
            }
        }
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 17 &&
        scan_card_interleave_phase.load(std::memory_order_acquire) != 0) {
        const auto phase = scan_card_interleave_phase.load(std::memory_order_acquire);
        if (phase == 1 && packet->endpoint == 0x81) {
            message->kind = dtv::spi::PublicOpenMessageKind::notification;
            message->notification_kind = dtv::spi::PublicOpenNotificationKind::card_state;
            message->generation = packet->generation;
            message->card_state = DTV_CARD_PRESENT;
            scan_card_interleave_phase.store(2, std::memory_order_release);
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        if (phase == 2 && packet->endpoint == 0x82) {
            static thread_local std::array<unsigned char, 5 * 192> source{};
            for (std::size_t packet_index = 0; packet_index < 5; ++packet_index) {
                const auto offset = packet_index * 192;
                std::fill(source.begin() + offset, source.begin() + offset + 4, 0xa0);
                source[offset + 4] = 0x47;
                source[offset + 5] = static_cast<unsigned char>(0x81 + packet_index);
                for (std::size_t index = 6; index < 192; ++index)
                    source[offset + index] = static_cast<unsigned char>((offset + index) * 11);
            }
            message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
            message->receiver_index = 1;
            message->stream_generation = scan_stream_generation.load(std::memory_order_acquire);
            message->generation = packet->generation;
            message->payload = source;
            scan_card_interleave_phase.store(3, std::memory_order_release);
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        if (phase == 3 && packet->endpoint == 0x81) {
            static thread_local std::array<unsigned char, 8> reply{};
            reply = {0, 0x3b, 0x80, 0x01, 0x81, 0, 0, 0};
            message->kind = dtv::spi::PublicOpenMessageKind::control_reply;
            message->resource_scope = 17;
            message->transaction_id = scan_card_transaction.load(std::memory_order_acquire);
            message->generation = packet->generation;
            message->payload = std::span<const unsigned char>(reply).first(5);
            scan_card_interleave_phase.store(4, std::memory_order_release);
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (packet->endpoint == 0x81 &&
        scan_lnb_reply_pending.exchange(false, std::memory_order_acq_rel)) {
        static thread_local std::array<unsigned char, 2> reply{};
        reply = {0xa5, static_cast<unsigned char>(
            scan_lnb_requested_state.load(std::memory_order_acquire))};
        message->kind = dtv::spi::PublicOpenMessageKind::control_reply;
        message->resource_scope = 19;
        message->transaction_id = 190 + reply[1];
        message->generation = packet->generation;
        message->payload = reply;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 17 && packet->endpoint == 0x81) {
        static thread_local std::array<unsigned char, 8> reply{};
        const auto mode = scan_card_reply_mode.exchange(0, std::memory_order_acq_rel);
        reply = {0, 0x3b, 0x80, 0x01, 0x81, 0, 0, 0};
        std::size_t size = 5;
        if (mode == 1) reply[0] = 1;
        if (mode == 2) size = 3;
        if (mode == 3) reply[4] = 0x80;
        message->kind = dtv::spi::PublicOpenMessageKind::control_reply;
        message->resource_scope = 17;
        message->transaction_id = scan_card_transaction.load(std::memory_order_acquire);
        message->generation = packet->generation - (mode == 4 ? 1 : 0);
        message->payload = std::span<const unsigned char>(reply).first(size);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    const auto stream_generation = scan_stream_generation.load(std::memory_order_acquire);
    if (scan_stream_transform_test_active.load(std::memory_order_acquire) &&
        stream_generation != 0 && packet->endpoint == 0x81) {
        message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
        message->receiver_index = scan_stream_transform_test_receiver.load(std::memory_order_acquire);
        message->stream_generation = stream_generation;
        message->generation = packet->generation;
        const auto size = scan_stream_transform_source_size.load(std::memory_order_acquire);
        message->payload = std::span<const unsigned char>(scan_stream_transform_source).first(size);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 16 && packet->endpoint == 0x81) {
        bool pending = false;
        {
            std::lock_guard lock(scan_pid_mutex);
            pending = scan_pid_pending_valid;
        }
        if (pending) {
            static thread_local std::array<unsigned char, 1> reply{};
            {
                std::lock_guard lock(scan_pid_mutex);
                reply[0] = scan_pid_fail_next.exchange(false, std::memory_order_acq_rel) ? 1 : 0;
                if (reply[0] == 0) {
                    scan_pid_active = scan_pid_pending;
                    scan_pid_active_all = scan_pid_pending_all;
                }
                scan_pid_pending.clear();
                scan_pid_pending_valid = false;
            }
            message->kind = dtv::spi::PublicOpenMessageKind::control_reply;
            message->resource_scope = 7;
            message->transaction_id = scan_pid_transaction.load(std::memory_order_acquire);
            message->generation = packet->generation;
            message->payload = reply;
            scan_pid_condition.notify_all();
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
    }
    if (scan_public_open_case == 16 && packet->endpoint == 0x81 &&
        scan_queue_notification_enabled.load(std::memory_order_acquire)) {
        message->kind = dtv::spi::PublicOpenMessageKind::notification;
        message->notification_kind = dtv::spi::PublicOpenNotificationKind::card_state;
        message->generation = packet->generation;
        message->card_state = scan_shared_card_state.load(std::memory_order_acquire);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 16 && packet->endpoint == 0x81 &&
        scan_queue_generations[0].load(std::memory_order_acquire) != 0) {
        const auto number = scan_pid_source_count.fetch_add(1, std::memory_order_acq_rel);
        const auto pid = static_cast<std::uint16_t>(number % 2 == 0 ? 0x100 : 0x200);
        bool accepted = false;
        {
            std::lock_guard lock(scan_pid_mutex);
            accepted = scan_pid_active_all || std::find(scan_pid_active.begin(),
                scan_pid_active.end(), pid) != scan_pid_active.end();
        }
        static thread_local std::array<unsigned char, 188> source{};
        source[0] = 0x47;
        source[1] = static_cast<unsigned char>((pid >> 8) & 0x1f);
        source[2] = static_cast<unsigned char>(pid);
        source[3] = 0x10;
        for (std::size_t index = 4; index < source.size(); ++index)
            source[index] = static_cast<unsigned char>(number + index * 29);
        scan_pid_processed_count.fetch_add(1, std::memory_order_release);
        scan_pid_condition.notify_all();
        if (!accepted) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
        message->receiver_index = 0;
        const auto generation_override = scan_stream_generation_override.exchange(0,
            std::memory_order_acq_rel);
        const auto source_generation = scan_queue_generations[0].load(std::memory_order_acquire);
        message->stream_generation = generation_override == 0 ? source_generation
                                                               : generation_override;
        message->generation = packet->generation;
        message->payload = source;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 16 && packet->endpoint == 0x82 &&
        scan_queue_generations[1].load(std::memory_order_acquire) != 0) {
        static thread_local std::array<unsigned char, 192> source{};
        const auto number = scan_pid_receiver_one_count.fetch_add(1, std::memory_order_acq_rel);
        std::fill(source.begin(), source.begin() + 4, 0xa0);
        source[4] = 0x47;
        source[5] = 0x01;
        source[6] = static_cast<unsigned char>(number);
        source[7] = 0x10;
        for (std::size_t index = 8; index < source.size(); ++index)
            source[index] = static_cast<unsigned char>(number + index * 11);
        scan_pid_condition.notify_all();
        message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
        message->receiver_index = 1;
        message->stream_generation = scan_queue_generations[1].load(std::memory_order_acquire);
        message->generation = packet->generation;
        message->payload = source;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 15 && packet->endpoint == 0x81 &&
        scan_queue_generations[0].load(std::memory_order_acquire) != 0 &&
        scan_shared_reply_enabled.load(std::memory_order_acquire)) {
        message->kind = dtv::spi::PublicOpenMessageKind::control_reply;
        message->resource_scope = 7;
        message->transaction_id = scan_shared_transaction_override.load(std::memory_order_acquire);
        message->generation = packet->generation;
        message->payload = packet->bytes.first(packet->actual_length);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 15 && packet->endpoint == 0x81 &&
        scan_queue_notification_enabled.load(std::memory_order_acquire)) {
        message->kind = dtv::spi::PublicOpenMessageKind::notification;
        message->notification_kind = dtv::spi::PublicOpenNotificationKind::card_state;
        message->generation = packet->generation -
            (scan_shared_card_stale_generation.load(std::memory_order_acquire) ? 1 : 0);
        message->card_state = scan_shared_card_state.load(std::memory_order_acquire);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 15 && packet->endpoint == 0x81 &&
        scan_queue_generations[0].load(std::memory_order_acquire) != 0) {
        const auto generation_override = scan_stream_generation_override.exchange(0,
            std::memory_order_acq_rel);
        const auto source_generation = scan_queue_generations[0].load(std::memory_order_acquire);
        const auto number = scan_queue_packet.fetch_add(1, std::memory_order_acq_rel);
        static thread_local std::array<unsigned char, 188> source{};
        source[0] = 0x47;
        source[1] = static_cast<unsigned char>(number);
        source[2] = static_cast<unsigned char>(number >> 8);
        source[3] = static_cast<unsigned char>(number >> 16);
        source[4] = static_cast<unsigned char>(number >> 24);
        for (std::size_t index = 5; index < source.size(); ++index)
            source[index] = static_cast<unsigned char>(number + index * 37);
        message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
        message->receiver_index = 0;
        message->stream_generation = generation_override == 0 ? source_generation
                                                               : generation_override;
        message->generation = packet->generation;
        message->payload = source;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 15 && packet->endpoint == 0x82 &&
        scan_queue_generations[1].load(std::memory_order_acquire) != 0) {
        static thread_local std::array<unsigned char, 384> source{};
        for (std::size_t unit = 0; unit < 2; ++unit) {
            const auto source_offset = unit * 192;
            std::fill(source.begin() + source_offset, source.begin() + source_offset + 4,
                static_cast<unsigned char>(0xa0 + unit));
            source[source_offset + 4] = 0x47;
            source[source_offset + 5] = static_cast<unsigned char>(0x80 + unit);
            for (std::size_t index = 6; index < 192; ++index)
                source[source_offset + index] = static_cast<unsigned char>(unit + index * 19);
        }
        message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
        message->receiver_index = 1;
        message->stream_generation = scan_queue_generations[1].load(std::memory_order_acquire);
        message->generation = packet->generation;
        message->payload = source;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 13 && stream_generation != 0 &&
        scan_shared_reply_enabled.load(std::memory_order_acquire)) {
        const auto transaction = scan_shared_transaction_override.load(std::memory_order_acquire);
        message->kind = dtv::spi::PublicOpenMessageKind::control_reply;
        message->resource_scope = 7;
        message->transaction_id = transaction;
        message->generation = packet->generation;
        message->payload = packet->bytes.first(packet->actual_length);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 13 && stream_generation != 0) {
        const auto generation_override = scan_stream_generation_override.exchange(0,
            std::memory_order_acq_rel);
        const auto stream_packet = generation_override == 0
            ? scan_stream_packet.fetch_add(1, std::memory_order_acq_rel)
            : 0;
        message->receiver_index = 0;
        message->stream_generation = generation_override == 0 ? stream_generation
                                                               : generation_override;
        message->generation = packet->generation;
        if (stream_packet == 0) {
        static const auto source = [] {
            std::array<unsigned char, 376> value{};
            value[0] = 0x47;
            value[188] = 0x47;
            return value;
        }();
        message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
        message->payload = source;
        } else if (stream_packet == 1) {
            message->kind = dtv::spi::PublicOpenMessageKind::source_end;
        }
        if (message->kind != dtv::spi::PublicOpenMessageKind::ignore)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 14 && stream_generation != 0 && packet->endpoint == 0x81) {
        message->receiver_index = scan_stream_receiver.load(std::memory_order_acquire);
        message->stream_generation = stream_generation;
        message->generation = packet->generation;
        const auto offset = scan_stream_192_offset.load(std::memory_order_acquire);
        if (offset < scan_stream_192_source.size()) {
            constexpr std::size_t chunk_size = 17;
            const auto count = (std::min)(chunk_size, scan_stream_192_source.size() - offset);
            message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
            message->payload = std::span<const unsigned char>(scan_stream_192_source).subspan(offset, count);
            scan_stream_192_offset.store(offset + count, std::memory_order_release);
        } else if (!scan_stream_192_end_sent.exchange(true, std::memory_order_acq_rel)) {
            message->kind = dtv::spi::PublicOpenMessageKind::source_end;
        }
        if (message->kind != dtv::spi::PublicOpenMessageKind::ignore)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    const auto failure_mode = scan_shared_failure_mode.load(std::memory_order_acquire);
    if (failure_mode == 2) throw std::bad_alloc();
    if (failure_mode == 1) {
        message->kind = dtv::spi::PublicOpenMessageKind::source_chunk;
        message->payload = packet->bytes.first(packet->actual_length);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (scan_public_open_case == 12 && packet->endpoint ==
        (packet->handle_index == 0 ? 0x81 : 0x82)) {
        message->kind = dtv::spi::PublicOpenMessageKind::notification;
        if (scan_shared_lock_enabled.load(std::memory_order_acquire)) {
            message->notification_kind = dtv::spi::PublicOpenNotificationKind::receiver_lock;
            message->generation = packet->generation -
                (scan_shared_lock_stale_generation.load(std::memory_order_acquire) ? 1 : 0);
            message->receiver_index = scan_shared_lock_receiver.load(std::memory_order_acquire);
            message->lock_state = scan_shared_lock_state.load(std::memory_order_acquire);
            const auto epoch_override = scan_shared_lock_epoch_override.load(std::memory_order_acquire);
            message->operation_epoch = epoch_override == UINT64_MAX &&
                message->receiver_index < scan_tune_epochs.size()
                ? scan_tune_epochs[message->receiver_index].load(std::memory_order_acquire)
                : epoch_override;
        } else {
            message->notification_kind = dtv::spi::PublicOpenNotificationKind::card_state;
            message->generation = packet->generation -
                (scan_shared_card_stale_generation.load(std::memory_order_acquire) ? 1 : 0);
            message->card_state = scan_shared_card_state.load(std::memory_order_acquire);
        }
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    message->kind = scan_shared_reply_enabled.load(std::memory_order_acquire)
        ? dtv::spi::PublicOpenMessageKind::control_reply
        : dtv::spi::PublicOpenMessageKind::ignore;
    message->resource_scope = 7;
    const auto transaction = scan_shared_transaction_override.load(std::memory_order_acquire);
    message->transaction_id = transaction == 0 ? 100 + index : transaction;
    message->generation = packet->generation -
        (scan_shared_stale_generation.load(std::memory_order_acquire) ? 1 : 0);
    message->payload = packet->bytes.first(packet->actual_length);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_shared_identify(dtv_context *context,
    std::span<const unsigned char> bytes, dtv_timeout timeout,
    const std::atomic<bool> *cancel, dtv::spi::PublicOpenSession *session)
{
    if (session == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    scan_shared_session = *session;
    for (std::size_t index = 0; index < scan_shared_tokens.size(); ++index) {
        auto result = session->generation_for_handle(session->user, index, &scan_shared_generations[index]);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = session->register_reply(session->user, 7, 100 + index,
            scan_shared_generations[index], &scan_shared_tokens[index]);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    return scan_open_identify(context, bytes, timeout, cancel);
}

dtv_internal_result scan_plan_operation(dtv_context *, const dtv::spi::PublicOperationRequest *request,
                                        dtv::spi::PublicOperationPlan *plan)
{
    if (request == nullptr || plan == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    if (request->operation == DTV_OP_LNB_POWER && request->target_kind ==
            dtv::spi::PublicOperationTargetKind::enclosure && request->arguments.size() == 1) {
        static constexpr std::uint64_t lnb_resource = 919;
        static thread_local std::array<unsigned char, 2> output{};
        plan->supported = true;
        plan->ready = scan_public_open_case != 18;
        plan->arguments_valid = request->target_kind ==
            dtv::spi::PublicOperationTargetKind::enclosure && request->arguments.size() == 1 &&
            (request->arguments[0] == DTV_LNB_ON || request->arguments[0] == DTV_LNB_OFF);
        if (!plan->arguments_valid) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        if (!plan->ready) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        scan_lnb_requested_state.store(request->arguments[0], std::memory_order_release);
        scan_lnb_reply_pending.store(true, std::memory_order_release);
        output = {0x4c, request->arguments[0]};
        plan->handle_index = 0;
        plan->bulk_out_endpoint = 0x02;
        plan->resource_scope = 19;
        plan->transaction_id = 190 + request->arguments[0];
        plan->reply_capacity = 2;
        plan->resource_keys = std::span<const std::uint64_t>(&lnb_resource, 1);
        plan->output = output;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (request->operation == 4) {
        plan->supported = true;
        plan->ready = true;
        plan->arguments_valid = request->target_kind ==
            dtv::spi::PublicOperationTargetKind::card && request->arguments.size() == 2 &&
            request->arguments[0] == 0x52 && request->arguments[1] != 0;
        if (!plan->arguments_valid) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        scan_card_transaction.store(request->arguments[1], std::memory_order_release);
        scan_card_transport_kind.store(0, std::memory_order_release);
        static constexpr std::uint64_t card_resource = 902;
        static constexpr std::array<unsigned char, 1> output{0x52};
        plan->handle_index = 0;
        plan->bulk_out_endpoint = 0x02;
        plan->resource_scope = 17;
        plan->transaction_id = request->arguments[1];
        plan->reply_capacity = 32;
        plan->resource_keys = std::span<const std::uint64_t>(&card_resource, 1);
        plan->output = output;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    plan->supported = request->operation != 99;
    plan->ready = request->operation != 2;
    plan->arguments_valid = request->arguments.size() >= 5 && request->arguments[0] >= 1 &&
        request->arguments[0] <= 2 && request->arguments[3] != 0;
    if (!plan->arguments_valid) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    thread_local std::array<std::uint64_t, 2> keys{};
    keys[0] = request->arguments[1] == 90 ? 900 :
        request->arguments[1] == 91 ? 901 : request->arguments[1];
    keys[1] = request->arguments[2] == 90 ? 900 :
        request->arguments[2] == 91 ? 901 : request->arguments[2];
    plan->handle_index = 0;
    plan->bulk_out_endpoint = 0x02;
    plan->resource_scope = 7;
    plan->transaction_id = request->arguments[3];
    plan->reply_capacity = 32;
    plan->resource_keys = std::span<const std::uint64_t>(keys.data(), request->arguments[0]);
    plan->output = request->arguments.subspan(4);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_plan_card_transport(
    dtv_context *, const dtv::spi::CardTransportRequest *request,
    dtv::spi::PublicCardTransportPlan *transport_plan)
{
    if (request == nullptr || transport_plan == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    scan_card_t1_connection_epoch.store(request->context.connection_epoch,
        std::memory_order_release);
    scan_card_t1_request_epoch.store(request->context.request_epoch,
        std::memory_order_release);
    static thread_local std::array<unsigned char, 1024> read_command{};
    static constexpr std::uint64_t card_resource = 918;
    auto &plan = transport_plan->operation;
    transport_plan->bgt.mode = static_cast<dtv::spi::CardBgtMode>(
        scan_card_t1_bgt_mode.load(std::memory_order_acquire));
    transport_plan->bgt.receive_complete_after_last_edge = true;
    transport_plan->bgt.etu_upper_bound_ns =
        scan_card_t1_bgt_etu_upper_bound_ns.load(std::memory_order_acquire);
    transport_plan->bgt.revision = scan_card_t1_bgt_revision.load(std::memory_order_acquire);
    transport_plan->bgt.evidence_id = 2;
    transport_plan->t1_bwt.card_clock_hz =
        scan_card_t1_bwt_mode.load(std::memory_order_acquire) == 3 ? 4000000000 :
        scan_card_t1_bwt_mode.load(std::memory_order_acquire) == 4 ? 0 :
        scan_card_t1_bwt_mode.load(std::memory_order_acquire) == 6 ? 5718012000 :
        scan_card_t1_bwt_mode.load(std::memory_order_acquire) == 8 ? 1 : 4000000;
    const auto cwt_factor = std::uint64_t{11} +
        (std::uint64_t{1} << request->context.t1_cwi);
    const auto cwt_numerator = cwt_factor * request->context.t1_f * 1000000000;
    const auto cwt_denominator = static_cast<std::uint64_t>(request->context.t1_d) *
        transport_plan->t1_bwt.card_clock_hz;
    scan_card_t1_cwt_duration_ns.store(cwt_denominator == 0 ? 0 :
        cwt_numerator / cwt_denominator +
            (cwt_numerator % cwt_denominator != 0 ? 1 : 0), std::memory_order_release);
    transport_plan->t1_bwt.clock_domain =
        scan_card_t1_bwt_mode.load(std::memory_order_acquire) == 5 ? 0 : 71;
    transport_plan->t1_bwt.error_bound_ns = 1;
    transport_plan->t1_bwt.error_bound_known =
        scan_card_t1_bwt_mode.load(std::memory_order_acquire) != 7;
    transport_plan->t1_bwt.revision = 1;
    transport_plan->t1_bwt.evidence_id = 1;
    if (scan_card_t1_receive_capacity_known.load(std::memory_order_acquire)) {
        transport_plan->receive_capacity.known = true;
        transport_plan->receive_capacity.complete_block_bytes =
            scan_card_t1_receive_capacity_bytes.load(std::memory_order_acquire);
        transport_plan->receive_capacity.revision =
            scan_card_t1_receive_capacity_revision.load(std::memory_order_acquire);
        transport_plan->receive_capacity.evidence_id =
            scan_card_t1_receive_capacity_evidence.load(std::memory_order_acquire);
    }
    const auto *refresh = std::get_if<dtv::spi::CardByteRead>(&request->operation);
    if (refresh != nullptr && refresh->capacity == 0) {
        scan_card_t1_bgt_refresh_count.fetch_add(1, std::memory_order_acq_rel);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (std::holds_alternative<dtv::spi::CardT1BlockExchange>(request->operation) &&
        scan_card_t1_bgt_send_plan_override.exchange(false, std::memory_order_acq_rel)) {
        transport_plan->bgt.etu_upper_bound_ns =
            scan_card_t1_bgt_send_plan_bound_ns.load(std::memory_order_acquire);
        transport_plan->bgt.revision =
            scan_card_t1_bgt_send_plan_revision.load(std::memory_order_acquire);
        scan_card_t1_bgt_etu_upper_bound_ns.store(transport_plan->bgt.etu_upper_bound_ns,
            std::memory_order_release);
        scan_card_t1_bgt_revision.store(transport_plan->bgt.revision,
            std::memory_order_release);
    }
    plan.supported = true;
    plan.ready = scan_public_open_case == 18;
    plan.arguments_valid = true;
    plan.handle_index = 0;
    plan.bulk_out_endpoint = 0x02;
    plan.resource_scope = 18;
    plan.reply_capacity = 32;
    plan.resource_keys = std::span<const std::uint64_t>(&card_resource, 1);
    if (const auto *write = std::get_if<dtv::spi::CardByteWrite>(&request->operation)) {
        if (write->bytes.empty()) plan.arguments_valid = false;
        const auto t1_mode = scan_card_t1_exchange_mode.load(std::memory_order_acquire);
        plan.transaction_id = t1_mode == 0 ? 201
            : scan_card_t1_transaction.fetch_add(1, std::memory_order_acq_rel);
        scan_card_t1_last_write_transaction.store(plan.transaction_id,
            std::memory_order_release);
        if (t1_mode != 0)
            scan_card_t1_current_transaction.store(plan.transaction_id, std::memory_order_release);
        plan.output = write->bytes;
        plan.reply_capacity = 1;
        scan_card_transport_kind.store(t1_mode == 0 ? 1 : 6, std::memory_order_release);
        if (t1_mode != 0) {
            std::lock_guard lock(scan_card_t1_mutex);
            if (!scan_card_t1_write_started) {
                scan_card_t1_request.clear();
                scan_card_t1_write_started = true;
            }
            scan_card_t1_request.insert(scan_card_t1_request.end(),
                write->bytes.begin(), write->bytes.end());
            if (scan_card_t1_request.size() >= 3 &&
                scan_card_t1_request.size() == static_cast<std::size_t>(scan_card_t1_request[2]) + 3 +
                    (scan_card_t1_crc.load(std::memory_order_acquire) ? 2u : 1u)) {
                scan_prepare_t1_reply(scan_card_t1_request);
                scan_card_t1_write_started = false;
            }
        }
    } else if (const auto *read = std::get_if<dtv::spi::CardByteRead>(&request->operation)) {
        const auto t1_mode = scan_card_t1_exchange_mode.load(std::memory_order_acquire);
        if (read->capacity < (t1_mode == 0 ? 2u : 1u) || read->capacity > read_command.size())
            plan.arguments_valid = false;
        read_command[0] = 0x52;
        read_command[1] = 0x01;
        plan.transaction_id = t1_mode == 0 ? 202
            : scan_card_t1_transaction.fetch_add(1, std::memory_order_acq_rel);
        if (t1_mode != 0)
            scan_card_t1_current_transaction.store(plan.transaction_id, std::memory_order_release);
        plan.output = std::span<const unsigned char>(read_command).first(2);
        plan.reply_capacity = read->capacity;
        scan_card_transport_kind.store(t1_mode == 0 ? 2 : 4, std::memory_order_release);
        if (t1_mode != 0) {
            std::lock_guard lock(scan_card_t1_mutex);
            if (scan_card_t1_reply_offset == 0)
                scan_card_t1_cwt_card_transaction.store(plan.transaction_id,
                    std::memory_order_release);
            const auto available = scan_card_t1_reply.size() - scan_card_t1_reply_offset;
            auto count = (std::min)({available, read->capacity, std::size_t{2}});
            const auto cwt_fault = scan_card_t1_cwt_fault.load(std::memory_order_acquire);
            if (cwt_fault >= 100 &&
                (scan_card_t1_last_outgoing_pcb.load(std::memory_order_acquire) & 0xc0) != 0x80) {
                const auto partial_remaining = scan_card_t1_reply_offset < 4
                    ? 4 - scan_card_t1_reply_offset : 0;
                count = (std::min)(count, partial_remaining);
            }
            scan_card_t1_pending.assign(
                scan_card_t1_reply.begin() + static_cast<std::ptrdiff_t>(scan_card_t1_reply_offset),
                scan_card_t1_reply.begin() + static_cast<std::ptrdiff_t>(scan_card_t1_reply_offset + count));
            scan_card_t1_reply_offset += count;
        }
    } else if (const auto *exchange = std::get_if<dtv::spi::CardT1BlockExchange>(&request->operation)) {
        if (!exchange->block.empty() && exchange->response_capacity < 5)
            plan.arguments_valid = false;
        const auto mode = scan_card_t1_exchange_mode.load(std::memory_order_acquire);
        transport_plan->mode = mode == 2 ? dtv::spi::CardTransportMode::complete_block
            : dtv::spi::CardTransportMode::byte_chunks;
        if (!exchange->block.empty()) {
            plan.transaction_id = mode == 0 ? 203
                : scan_card_t1_transaction.fetch_add(1, std::memory_order_acq_rel);
            scan_card_t1_last_write_transaction.store(plan.transaction_id,
                std::memory_order_release);
            if (mode != 0)
                scan_card_t1_current_transaction.store(plan.transaction_id, std::memory_order_release);
            plan.output = exchange->block;
            plan.reply_capacity = exchange->response_capacity;
            scan_card_t1_cwt_card_transaction.store(plan.transaction_id,
                std::memory_order_release);
            scan_card_transport_kind.store(mode == 0 ? 3 : 5, std::memory_order_release);
            if (mode == 2) {
                std::lock_guard lock(scan_card_t1_mutex);
                if (!scan_card_t1_complete_probe_pending) {
                    scan_prepare_t1_reply(exchange->block);
                    scan_card_t1_cwt_card_transaction.store(plan.transaction_id,
                        std::memory_order_release);
                    scan_card_t1_complete_probe_pending = true;
                } else {
                    scan_card_t1_complete_probe_pending = false;
                }
            }
        }
    } else {
        plan.arguments_valid = false;
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_plan_tune(dtv_context *, const dtv::spi::PublicTuneRequest *request,
                                   dtv::spi::PublicTunePlan *plan)
{
    if (request == nullptr || plan == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    plan->supported = true;
    plan->ready = true;
    plan->arguments_valid = true;
    if (request->has_tune_options &&
        request->tune_options.selector == DTV_TUNE_SELECTOR_SLOT &&
        request->tune_options.value > 1) {
        plan->supported = false;
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_tune_by_frequency(dtv_context *, const dtv::spi::PublicTuneRequest *request,
                                           dtv::spi::PublicTuneSession *session)
{
    if (request == nullptr || session == nullptr || session->begin_effect == nullptr ||
        request->receiver_index >= scan_tune_receivers.size())
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    const auto record_index = scan_tune_calls.fetch_add(1, std::memory_order_acq_rel);
    if (record_index < scan_tune_records.size()) {
        scan_tune_records[record_index] = {request->receiver_index, request->system,
            request->frequency_hz, request->has_tune_options, request->tune_options, request->timeout};
    }
    const auto receiver_index = request->receiver_index;
    auto *peer = scan_tune_receivers[receiver_index == 0 ? 1 : 0].load(std::memory_order_acquire);
    if (peer != nullptr) {
        dtv_signal signal{};
        dtv_error error{};
        error.struct_size = sizeof(error);
        if (dtv_get_signal(peer, nullptr, &signal, &error) == DTV_ERROR_OK &&
            error.code == DTV_ERROR_OK) {
            scan_tune_peer_checked.store(true, std::memory_order_release);
        }
    }
    auto *enclosure = scan_tune_enclosure.load(std::memory_order_acquire);
    if (enclosure != nullptr && dtv_get_card_state(enclosure) != DTV_CARD_UNKNOWN) {
        scan_tune_card_checked.store(true, std::memory_order_release);
    }
    const auto mode = scan_tune_mode.load(std::memory_order_acquire);
    if (mode == 1) return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
    std::uint64_t operation_epoch = 0;
    auto result = session->begin_effect(session->user, &operation_epoch);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    scan_tune_epochs[receiver_index].store(operation_epoch, std::memory_order_release);
    scan_tune_effects.fetch_add(1, std::memory_order_release);
    if (mode == 2) return dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0);
    if (mode == 3) {
        dtv_error error{};
        error.struct_size = sizeof(error);
        if (enclosure != nullptr) (void)dtv_cancel_enclosure(enclosure, &error);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
    }
    if (mode == 4) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_plan_stream(dtv_context *, const dtv::spi::PublicStreamRequest *request,
                                     dtv::spi::PublicStreamPlan *plan)
{
    if (request == nullptr || plan == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    thread_local std::array<std::uint64_t, 1> resource_keys{};
    resource_keys[0] = (scan_public_open_case == 15 || scan_public_open_case == 16)
        ? 900 + request->receiver_index : 900;
    plan->supported = true;
    plan->ready = true;
    plan->arguments_valid = request->receiver_index < 3;
    plan->resource_keys = resource_keys;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_prepare_stream(dtv_context *, const dtv::spi::PublicStreamRequest *request)
{
    if (request == nullptr ||
        !((request->receiver_index == 0 && request->source_stride_bytes == 188 &&
           request->source_sync_offset_bytes == 0) ||
          (request->receiver_index == 2 && request->source_stride_bytes == 188 &&
           request->source_sync_offset_bytes == 0) ||
          (request->receiver_index == 1 && request->source_stride_bytes == 192 &&
           request->source_sync_offset_bytes == 4)))
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    if (scan_stream_prepare_mode.load(std::memory_order_acquire) != 0)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0);
    if (scan_public_open_case == 15 || scan_public_open_case == 16) {
        scan_stream_queue_capacity.store(request->queue_capacity, std::memory_order_release);
        if (request->receiver_index == 0) {
            scan_queue_packet.store(0, std::memory_order_release);
            if (scan_public_open_case == 16) {
                scan_pid_source_count.store(0, std::memory_order_release);
                scan_pid_processed_count.store(0, std::memory_order_release);
            }
        }
    }
    scan_stream_packet.store(0, std::memory_order_release);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_start_stream(dtv_context *, const dtv::spi::PublicStreamRequest *request,
                                      bool *producer_started)
{
    if (request == nullptr || producer_started == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    if (scan_shared_packets.load(std::memory_order_acquire) < 2)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    *producer_started = false;
    const auto mode = scan_stream_start_mode.load(std::memory_order_acquire);
    if (mode == 1) return dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0);
    *producer_started = true;
    scan_stream_receiver.store(request->receiver_index, std::memory_order_release);
    scan_stream_generation.store(request->stream_generation, std::memory_order_release);
    if ((scan_public_open_case == 15 || scan_public_open_case == 16) &&
        request->receiver_index < scan_queue_generations.size())
        scan_queue_generations[request->receiver_index].store(request->stream_generation,
            std::memory_order_release);
    if (scan_public_open_case == 16 && request->receiver_index == 0) {
        std::lock_guard lock(scan_pid_mutex);
        scan_pid_active.clear();
        scan_pid_active_all = true;
        scan_pid_pending.clear();
        scan_pid_pending_all = true;
        scan_pid_pending_valid = false;
    }
    return mode == 2 ? dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0)
                     : dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_stop_stream(dtv_context *, const dtv::spi::PublicStreamRequest *request,
                                     dtv_timeout)
{
    if (request == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    auto generation = request->stream_generation;
    (void)scan_stream_generation.compare_exchange_strong(generation, 0,
        std::memory_order_acq_rel);
    previous_stream_generation.store(request->stream_generation, std::memory_order_release);
    if ((scan_public_open_case == 15 || scan_public_open_case == 16) &&
        request->receiver_index < scan_queue_generations.size()) {
        auto active_generation = request->stream_generation;
        (void)scan_queue_generations[request->receiver_index].compare_exchange_strong(
            active_generation, 0, std::memory_order_acq_rel);
    }
    if (scan_stream_stop_mode.exchange(0, std::memory_order_acq_rel) != 0)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_after_stream_drain(dtv_context *,
    const dtv::spi::PublicStreamRequest *request)
{
    if (request == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    if (scan_stream_drain_mode.exchange(0, std::memory_order_acq_rel) != 0)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result scan_transform_stream(dtv_context *,
    const dtv::spi::PublicStreamRequest *request, std::span<const unsigned char> input,
    std::span<unsigned char> output, std::size_t *consumed, std::size_t *produced)
{
    if (request == nullptr || consumed == nullptr || produced == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    scan_stream_transform_calls.fetch_add(1, std::memory_order_acq_rel);
    if (request->source_stride_bytes == 188 && request->source_sync_offset_bytes == 0) {
        if (input.size() % 188 != 0 || output.size() != input.size())
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        std::size_t skip_index = 0;
        bool skipped = false;
        for (std::size_t offset = 0; offset < input.size(); offset += 188) {
            while (skip_index < request->source_sync_skips_before_units.size() &&
                   request->source_sync_skips_before_units[skip_index] == offset / 188) {
                skipped = true;
                ++skip_index;
            }
            std::copy_n(input.begin() + offset, 188, output.begin() + offset);
            if (skipped) output[offset + 1] = 0xfa;
        }
        if (skip_index != request->source_sync_skips_before_units.size())
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        *consumed = input.size();
        const auto mode = scan_stream_transform_mode.load(std::memory_order_acquire);
        *produced = mode == 1 ? 188 : mode == 2 ? 0 : output.size();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (request->source_stride_bytes != 192 || request->source_sync_offset_bytes != 4 ||
        input.size() % 192 != 0 || output.size() != input.size() / 192 * 188)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    for (std::size_t source_offset = 0, output_offset = 0;
         source_offset < input.size(); source_offset += 192, output_offset += 188) {
        if (input[source_offset + 4] != 0x47)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        for (std::size_t byte = 0; byte < 188; ++byte)
            output[output_offset + byte] = input[source_offset + 4 + byte];
    }
    *consumed = input.size();
    *produced = output.size();
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

const dtv::spi::PublicOpenOps scan_public_open_ops{
    scan_open_plan, scan_open_identify, nullptr, scan_shared_demux, scan_shared_identify,
    scan_plan_operation, scan_plan_tune, scan_tune_by_frequency,
    scan_prepare_stream, scan_start_stream, scan_stop_stream, scan_after_stream_drain,
    scan_plan_stream, scan_transform_stream, scan_plan_card_transport};

}

void set_scan_group_case(unsigned value)
{
    scan_group_case = static_cast<ScanGroupCase>(value);
}

void set_scan_public_open_case(unsigned value)
{
    scan_public_open_case = value;
    scan_stream_generation.store(0, std::memory_order_release);
    previous_stream_generation.store(0, std::memory_order_release);
    scan_stream_generation_override.store(0, std::memory_order_release);
    scan_stream_packet.store(0, std::memory_order_release);
    scan_queue_packet.store(0, std::memory_order_release);
    scan_stream_queue_capacity.store(0, std::memory_order_release);
    for (auto &generation : scan_queue_generations)
        generation.store(0, std::memory_order_release);
    scan_queue_notification_enabled.store(false, std::memory_order_release);
    scan_stream_prepare_mode.store(0, std::memory_order_release);
    scan_stream_start_mode.store(0, std::memory_order_release);
    scan_stream_stop_mode.store(0, std::memory_order_release);
    scan_stream_drain_mode.store(0, std::memory_order_release);
    scan_stream_192_offset.store(0, std::memory_order_release);
    scan_stream_192_end_sent.store(false, std::memory_order_release);
    scan_stream_receiver.store(0, std::memory_order_release);
    scan_stream_transform_calls.store(0, std::memory_order_release);
    scan_stream_transform_mode.store(0, std::memory_order_release);
    scan_stream_transform_test_active.store(false, std::memory_order_release);
    scan_stream_transform_test_receiver.store(0, std::memory_order_release);
    scan_stream_transform_source_size.store(0, std::memory_order_release);
    scan_open_identify_calls.store(0, std::memory_order_release);
    scan_open_identify_blocked.store(false, std::memory_order_release);
    scan_open_identify_release.store(value != 8, std::memory_order_release);
    scan_shared_session = {};
    scan_shared_tokens = {};
    scan_shared_generations = {};
    scan_shared_packets.store(0, std::memory_order_release);
    scan_shared_reply_enabled.store(false, std::memory_order_release);
    scan_shared_failure_mode.store(0, std::memory_order_release);
    scan_shared_stale_generation.store(false, std::memory_order_release);
    scan_shared_transaction_override.store(0, std::memory_order_release);
    scan_shared_card_state.store(DTV_CARD_ABSENT, std::memory_order_release);
    scan_shared_card_stale_generation.store(false, std::memory_order_release);
    scan_shared_lock_receiver.store(0, std::memory_order_release);
    scan_shared_lock_state.store(2, std::memory_order_release);
    scan_shared_lock_stale_generation.store(false, std::memory_order_release);
    scan_shared_lock_enabled.store(false, std::memory_order_release);
    for (auto &epoch : scan_tune_epochs) epoch.store(0, std::memory_order_release);
    scan_shared_lock_epoch_override.store(UINT64_MAX, std::memory_order_release);
    scan_tune_mode.store(0, std::memory_order_release);
    scan_tune_calls.store(0, std::memory_order_release);
    scan_tune_effects.store(0, std::memory_order_release);
    scan_pid_transaction.store(0, std::memory_order_release);
    scan_pid_transaction_next.store(1, std::memory_order_release);
    scan_pid_fail_next.store(false, std::memory_order_release);
    scan_pid_source_count.store(0, std::memory_order_release);
    scan_pid_processed_count.store(0, std::memory_order_release);
    scan_pid_receiver_one_count.store(0, std::memory_order_release);
    scan_card_reply_mode.store(0, std::memory_order_release);
    scan_card_transaction.store(1, std::memory_order_release);
    scan_card_interleave_phase.store(0, std::memory_order_release);
    scan_card_transport_kind.store(0, std::memory_order_release);
    scan_card_transport_drop_reply.store(false, std::memory_order_release);
    scan_card_t1_exchange_mode.store(0, std::memory_order_release);
    scan_card_t1_fault.store(0, std::memory_order_release);
    scan_card_t1_apdu_mode.store(false, std::memory_order_release);
    scan_card_t1_apdu_fault.store(0, std::memory_order_release);
    scan_card_t1_host_ifs_chain_state.store(0, std::memory_order_release);
    scan_card_t1_receive_capacity_known.store(false, std::memory_order_release);
    scan_card_t1_receive_capacity_bytes.store(0, std::memory_order_release);
    scan_card_t1_receive_capacity_revision.store(0, std::memory_order_release);
    scan_card_t1_receive_capacity_evidence.store(0, std::memory_order_release);
    scan_card_t1_apdu_rblock_sent.store(false, std::memory_order_release);
    scan_card_t1_apdu_drop_r_reply.store(false, std::memory_order_release);
    scan_card_t1_apdu_reply_count.store(0, std::memory_order_release);
    scan_card_t1_cwt_card_transaction.store(0, std::memory_order_release);
    scan_card_t1_cwt_duration_ns.store(0, std::memory_order_release);
    scan_card_t1_cwt_fault.store(0, std::memory_order_release);
    scan_card_t1_cwt_silence_delivered.store(false, std::memory_order_release);
    scan_card_t1_bwt_mode.store(0, std::memory_order_release);
    scan_card_t1_bwt_timeout_count.store(0, std::memory_order_release);
    scan_card_t1_apdu_pending_ins.store(0, std::memory_order_release);
    scan_card_t1_crc.store(false, std::memory_order_release);
    scan_card_t1_stale_reply.store(false, std::memory_order_release);
    scan_card_t1_transaction.store(300, std::memory_order_release);
    scan_card_t1_current_transaction.store(0, std::memory_order_release);
    scan_card_t1_last_write_transaction.store(0, std::memory_order_release);
    scan_card_t1_connection_epoch.store(0, std::memory_order_release);
    scan_card_t1_request_epoch.store(0, std::memory_order_release);
    {
        std::lock_guard lock(scan_card_t1_mutex);
        scan_card_t1_request.clear();
        scan_card_t1_reply.clear();
        scan_card_t1_pending.clear();
        scan_card_t1_reply_offset = 0;
        scan_card_t1_write_started = false;
        scan_card_t1_complete_probe_pending = false;
    }
    {
        std::lock_guard lock(scan_pid_mutex);
        scan_pid_active.clear();
        scan_pid_pending.clear();
        scan_pid_active_all = true;
        scan_pid_pending_all = true;
        scan_pid_pending_valid = false;
    }
    scan_pid_condition.notify_all();
    scan_tune_peer_checked.store(false, std::memory_order_release);
    scan_tune_card_checked.store(false, std::memory_order_release);
    scan_tune_enclosure.store(nullptr, std::memory_order_release);
    for (auto &receiver : scan_tune_receivers) receiver.store(nullptr, std::memory_order_release);
    scan_tune_records = {};
    scan_shared_order = {};
    scan_shared_order_endpoints = {};
}

unsigned scan_stream_transform_call_count()
{
    return scan_stream_transform_calls.load(std::memory_order_acquire);
}

void set_scan_stream_transform_mode(unsigned mode)
{
    scan_stream_transform_mode.store(mode, std::memory_order_release);
}

void set_scan_stream_transform_test_active(bool active)
{
    scan_stream_transform_test_active.store(active, std::memory_order_release);
}

void set_scan_stream_transform_test_receiver(unsigned receiver_index)
{
    scan_stream_transform_test_receiver.store(receiver_index, std::memory_order_release);
}

void set_scan_stream_transform_source_phase(unsigned phase)
{
    auto set_unit = [](std::size_t offset, unsigned char marker) {
        std::fill(scan_stream_transform_source.begin() + offset,
                  scan_stream_transform_source.begin() + offset + 188, marker);
        scan_stream_transform_source[offset] = 0x47;
    };
    std::fill(scan_stream_transform_source.begin(), scan_stream_transform_source.end(), 0);
    std::size_t size = 0;
    if (phase == 1) {
        set_unit(0, 0x21);
        set_unit(378, 0x22);
        set_unit(566, 0x23);
        size = 754;
    } else if (phase == 2) {
        set_unit(0, 0x31);
        size = 378;
    } else if (phase == 3) {
        set_unit(0, 0x41);
        set_unit(188, 0x42);
        size = 376;
    } else if (phase == 4) {
        set_unit(0, 0x51);
        size = 378;
    } else if (phase == 6) {
        set_unit(0, 0x71);
        set_unit(188, 0x72);
        size = 376;
    } else if (phase == 7) {
        set_unit(0, 0x73);
        size = 188;
    } else {
        set_unit(0, phase == 5 ? 0x61 : 0x11);
        set_unit(188, phase == 5 ? 0x62 : 0x12);
        size = 376;
    }
    if (phase == 1) std::fill(scan_stream_transform_source.begin() + 188,
                               scan_stream_transform_source.begin() + 378, 0);
    if (phase == 2) std::fill(scan_stream_transform_source.begin() + 188,
                               scan_stream_transform_source.begin() + 378, 0);
    if (phase == 4) std::fill(scan_stream_transform_source.begin() + 188,
                               scan_stream_transform_source.begin() + 378, 0);
    scan_stream_transform_source_size.store(size, std::memory_order_release);
}

void set_scan_card_reply_mode(unsigned mode)
{
    scan_card_reply_mode.store(mode, std::memory_order_release);
}

void set_scan_card_transport_drop_reply(bool drop)
{
    scan_card_transport_drop_reply.store(drop, std::memory_order_release);
}

void set_scan_card_t1_exchange_mode(unsigned mode)
{
    scan_card_t1_exchange_mode.store(mode, std::memory_order_release);
}

void set_scan_card_t1_bwt_mode(unsigned mode)
{
    scan_card_t1_bwt_mode.store(mode, std::memory_order_release);
    scan_card_t1_bwt_timeout_count.store(0, std::memory_order_release);
}

void set_scan_card_t1_bgt(unsigned mode, std::uint64_t etu_upper_bound_ns,
                          std::uint64_t revision)
{
    scan_card_t1_bgt_etu_upper_bound_ns.store(etu_upper_bound_ns, std::memory_order_release);
    scan_card_t1_bgt_revision.store(revision, std::memory_order_release);
    scan_card_t1_bgt_mode.store(mode, std::memory_order_release);
}

unsigned scan_card_t1_bgt_refresh_count_value()
{
    return scan_card_t1_bgt_refresh_count.load(std::memory_order_acquire);
}

void set_scan_card_t1_bgt_send_plan_override(std::uint64_t bound_ns,
                                              std::uint64_t revision)
{
    scan_card_t1_bgt_send_plan_bound_ns.store(bound_ns, std::memory_order_release);
    scan_card_t1_bgt_send_plan_revision.store(revision, std::memory_order_release);
    scan_card_t1_bgt_send_plan_override.store(true, std::memory_order_release);
}

unsigned scan_card_t1_apdu_ack_count_value()
{
    return scan_card_t1_apdu_ack_count.load(std::memory_order_acquire);
}

unsigned scan_card_t1_apdu_ack_trace_value()
{
    return scan_card_t1_apdu_ack_trace.load(std::memory_order_acquire);
}

void reset_scan_card_t1_apdu_sequence()
{
    scan_card_t1_apdu_response_sequence.store(false, std::memory_order_release);
    scan_card_t1_apdu_chain_index.store(0, std::memory_order_release);
    scan_card_t1_wtx_exchange_count.store(0, std::memory_order_release);
    scan_card_t1_apdu_ifs_requested.store(false, std::memory_order_release);
    scan_card_t1_apdu_ifs_negotiated.store(false, std::memory_order_release);
    scan_card_t1_host_ifs_chain_state.store(0, std::memory_order_release);
}

void set_scan_card_t1_fault(unsigned fault)
{
    scan_card_t1_fault.store(fault, std::memory_order_release);
}

void set_scan_card_t1_apdu_mode(bool enabled, unsigned fault)
{
    scan_card_t1_apdu_fault.store(fault, std::memory_order_release);
    scan_card_t1_apdu_rblock_sent.store(false, std::memory_order_release);
    scan_card_t1_apdu_drop_r_reply.store(false, std::memory_order_release);
    scan_card_t1_apdu_chain_index.store(0, std::memory_order_release);
    scan_card_t1_apdu_ack_count.store(0, std::memory_order_release);
    scan_card_t1_apdu_ack_trace.store(0, std::memory_order_release);
    scan_card_t1_apdu_ifs_requested.store(false, std::memory_order_release);
    scan_card_t1_apdu_ifs_negotiated.store(false, std::memory_order_release);
    if (fault >= 22 && fault <= 26)
        scan_card_t1_apdu_response_sequence.store(false, std::memory_order_release);
    if (fault == 51 || fault == 52 || fault == 56)
        scan_card_t1_apdu_response_sequence.store(false, std::memory_order_release);
    scan_card_t1_wtx_exchange_count.store(0, std::memory_order_release);
    scan_card_t1_apdu_reply_count.store(0, std::memory_order_release);
    scan_card_t1_apdu_mode.store(enabled, std::memory_order_release);
}

void set_scan_card_t1_cwt_fault(unsigned fault)
{
    if (fault >= 100)
        scan_card_t1_cwt_silence_delivered.store(false, std::memory_order_release);
    scan_card_t1_cwt_fault.store(fault, std::memory_order_release);
}

bool scan_card_t1_cwt_silence_delivered_value()
{
    return scan_card_t1_cwt_silence_delivered.load(std::memory_order_acquire);
}

void set_scan_card_t1_receive_capacity(bool known, std::size_t complete_block_bytes,
                                       std::uint64_t revision, std::uint64_t evidence_id)
{
    scan_card_t1_receive_capacity_bytes.store(complete_block_bytes, std::memory_order_release);
    scan_card_t1_receive_capacity_revision.store(revision, std::memory_order_release);
    scan_card_t1_receive_capacity_evidence.store(evidence_id, std::memory_order_release);
    scan_card_t1_receive_capacity_known.store(known, std::memory_order_release);
}

void set_scan_card_t1_crc(bool crc)
{
    scan_card_t1_crc.store(crc, std::memory_order_release);
}

void set_scan_card_t1_stale_reply(bool stale)
{
    scan_card_t1_stale_reply.store(stale, std::memory_order_release);
}

void set_scan_card_interleave_phase(unsigned phase)
{
    scan_card_interleave_phase.store(phase, std::memory_order_release);
}

unsigned scan_card_interleave_phase_value()
{
    return scan_card_interleave_phase.load(std::memory_order_acquire);
}

void set_scan_stream_192_source()
{
    std::fill(scan_stream_192_source.begin(), scan_stream_192_source.begin() + 3, 0x13);
    for (std::size_t unit = 0; unit < 7; ++unit) {
        const auto offset = 3 + unit * 192;
        std::fill(scan_stream_192_source.begin() + offset,
                  scan_stream_192_source.begin() + offset + 4,
                  static_cast<unsigned char>(0xa0 + unit));
        std::fill(scan_stream_192_source.begin() + offset + 4,
                  scan_stream_192_source.begin() + offset + 192,
                  static_cast<unsigned char>(0x20 + unit));
        if (unit != 2) scan_stream_192_source[offset + 4] = 0x47;
    }
    scan_stream_192_offset.store(0, std::memory_order_release);
    scan_stream_192_end_sent.store(false, std::memory_order_release);
}

void set_scan_stream_failure_modes(unsigned prepare, unsigned start, unsigned stop, unsigned drain)
{
    scan_stream_prepare_mode.store(prepare, std::memory_order_release);
    scan_stream_start_mode.store(start, std::memory_order_release);
    scan_stream_stop_mode.store(stop, std::memory_order_release);
    scan_stream_drain_mode.store(drain, std::memory_order_release);
}

void set_scan_shared_reply_enabled(bool value)
{
    scan_shared_reply_enabled.store(value, std::memory_order_release);
}

void set_scan_shared_failure_mode(unsigned value)
{
    scan_shared_failure_mode.store(value, std::memory_order_release);
}

void set_scan_shared_stale_generation(bool value)
{
    scan_shared_stale_generation.store(value, std::memory_order_release);
}

void set_scan_shared_transaction_override(std::uint64_t value)
{
    scan_shared_transaction_override.store(value, std::memory_order_release);
}

dtv::spi::PublicOpenSession scan_shared_open_session()
{
    return scan_shared_session;
}

std::uint64_t scan_shared_token(std::size_t index)
{
    return index < scan_shared_tokens.size() ? scan_shared_tokens[index] : 0;
}

std::uint64_t scan_shared_generation(std::size_t index)
{
    return index < scan_shared_generations.size() ? scan_shared_generations[index] : 0;
}

unsigned scan_shared_packet_count()
{
    return scan_shared_packets.load(std::memory_order_acquire);
}

bool scan_shared_mode_enabled()
{
    return scan_public_open_case == 9 || scan_public_open_case == 10 ||
        scan_public_open_case == 11 || scan_public_open_case == 12 ||
        scan_public_open_case == 13 || scan_public_open_case == 14 ||
        scan_public_open_case == 15 || scan_public_open_case == 16 ||
        scan_public_open_case == 17 || scan_public_open_case == 18;
}

bool scan_operation_mode_enabled()
{
    return scan_public_open_case == 11 || scan_public_open_case == 13 ||
        scan_public_open_case == 15 || scan_public_open_case == 16 ||
        scan_public_open_case == 17 || scan_public_open_case == 18;
}

std::size_t scan_stream_queue_capacity_value()
{
    return scan_stream_queue_capacity.load(std::memory_order_acquire);
}

unsigned scan_pid_processed()
{
    return scan_pid_processed_count.load(std::memory_order_acquire);
}

unsigned scan_pid_receiver_one_processed()
{
    return scan_pid_receiver_one_count.load(std::memory_order_acquire);
}

bool wait_for_scan_pid_processed(unsigned count)
{
    std::unique_lock lock(scan_pid_mutex);
    return scan_pid_condition.wait_for(lock, std::chrono::seconds(2), [&] {
        return scan_pid_processed_count.load(std::memory_order_acquire) >= count;
    });
}

bool wait_for_scan_pid_receiver_one_processed(unsigned count)
{
    std::unique_lock lock(scan_pid_mutex);
    return scan_pid_condition.wait_for(lock, std::chrono::seconds(2), [&] {
        return scan_pid_receiver_one_count.load(std::memory_order_acquire) >= count;
    });
}

void set_scan_pid_fail_next(bool value)
{
    scan_pid_fail_next.store(value, std::memory_order_release);
}

void set_scan_queue_notification_enabled(bool value)
{
    scan_queue_notification_enabled.store(value, std::memory_order_release);
}

void set_scan_stream_generation_override(std::uint64_t value)
{
    scan_stream_generation_override.store(value, std::memory_order_release);
}

std::uint64_t scan_stream_previous_generation()
{
    return previous_stream_generation.load(std::memory_order_acquire);
}

void set_scan_card_state_notification(std::uint32_t state, bool stale_generation)
{
    scan_shared_card_state.store(state, std::memory_order_release);
    scan_shared_card_stale_generation.store(stale_generation, std::memory_order_release);
    scan_shared_lock_enabled.store(false, std::memory_order_release);
}

void set_scan_receiver_lock_notification(std::uint32_t receiver, std::uint32_t state,
                                         bool stale_generation)
{
    scan_shared_lock_receiver.store(receiver, std::memory_order_release);
    scan_shared_lock_state.store(state, std::memory_order_release);
    scan_shared_lock_stale_generation.store(stale_generation, std::memory_order_release);
    scan_shared_lock_enabled.store(true, std::memory_order_release);
}

void set_scan_receiver_lock_operation_epoch(std::uint64_t operation_epoch)
{
    scan_shared_lock_epoch_override.store(operation_epoch, std::memory_order_release);
}

void set_scan_tune_context(dtv_enclosure *enclosure, dtv_receiver *receiver0,
                           dtv_receiver *receiver1)
{
    scan_tune_enclosure.store(enclosure, std::memory_order_release);
    scan_tune_receivers[0].store(receiver0, std::memory_order_release);
    scan_tune_receivers[1].store(receiver1, std::memory_order_release);
}

void set_scan_tune_mode(unsigned mode)
{
    scan_tune_mode.store(mode, std::memory_order_release);
}

extern "C" void dtv_public_test_set_tune_context(dtv_enclosure *enclosure,
                                                   dtv_receiver *receiver0,
                                                   dtv_receiver *receiver1)
{
    set_scan_tune_context(enclosure, receiver0, receiver1);
}

extern "C" void dtv_public_test_set_tune_mode(unsigned mode)
{
    set_scan_tune_mode(mode);
}

unsigned scan_tune_call_count() { return scan_tune_calls.load(std::memory_order_acquire); }
unsigned scan_tune_effect_count() { return scan_tune_effects.load(std::memory_order_acquire); }
bool scan_tune_peer_was_checked() { return scan_tune_peer_checked.load(std::memory_order_acquire); }
bool scan_tune_card_was_checked() { return scan_tune_card_checked.load(std::memory_order_acquire); }

bool scan_tune_record_matches(std::size_t index, std::uint32_t receiver_index,
                              std::uint32_t system, std::uint64_t frequency_hz,
                              bool has_options, std::uint32_t selector, std::uint32_t value,
                              std::uint32_t lna)
{
    if (index >= scan_tune_records.size() || index >= scan_tune_calls.load(std::memory_order_acquire))
        return false;
    const auto &record = scan_tune_records[index];
    return record.receiver_index == receiver_index && record.system == system &&
        record.frequency_hz == frequency_hz && record.has_options == has_options &&
        record.options.selector == selector && record.options.value == value &&
        record.options.lna == lna && record.timeout.operation_deadline_ns != 0;
}

std::uint64_t scan_shared_sequence(std::size_t index)
{
    return index < scan_shared_sequences.size() ? scan_shared_sequences[index] : 0;
}

std::size_t scan_shared_length(std::size_t index)
{
    return index < scan_shared_lengths.size() ? scan_shared_lengths[index] : 0;
}

std::uint64_t scan_shared_order_at(std::size_t index)
{
    return index < scan_shared_order.size() ? scan_shared_order[index] : 0;
}

std::uint8_t scan_shared_order_endpoint(std::size_t index)
{
    return index < scan_shared_order_endpoints.size() ? scan_shared_order_endpoints[index] : 0;
}

std::size_t scan_open_identify_size()
{
    return scan_open_identify_bytes;
}

bool scan_open_identify_is_blocked()
{
    return scan_open_identify_blocked.load(std::memory_order_acquire);
}

dtv_device_id scan_firmware_device_id()
{
    return scan_firmware_last_device.load(std::memory_order_acquire);
}

dtv_function scan_firmware_function()
{
    return scan_firmware_last_function.load(std::memory_order_acquire);
}

std::uint64_t scan_firmware_epoch()
{
    return scan_firmware_last_epoch.load(std::memory_order_acquire);
}

std::size_t scan_firmware_handle_index()
{
    return scan_firmware_last_handle.load(std::memory_order_acquire);
}

void reset_scan_firmware_records()
{
    scan_firmware_create_count.store(0, std::memory_order_release);
    scan_firmware_open_count.store(0, std::memory_order_release);
    scan_firmware_identify_count.store(0, std::memory_order_release);
    scan_firmware_close_count.store(0, std::memory_order_release);
    scan_firmware_destroy_count.store(0, std::memory_order_release);
    scan_firmware_runtime_devices.clear();
    scan_firmware_same_id_mode.store(DTV_IDENTIFY_RUNTIME, std::memory_order_release);
}

void set_scan_firmware_same_id_mode(std::uint32_t mode)
{
    scan_firmware_same_id_mode.store(mode, std::memory_order_release);
}

unsigned scan_firmware_create_calls() { return scan_firmware_create_count.load(); }
unsigned scan_firmware_open_calls() { return scan_firmware_open_count.load(); }
unsigned scan_firmware_identify_calls() { return scan_firmware_identify_count.load(); }
unsigned scan_firmware_close_calls() { return scan_firmware_close_count.load(); }
unsigned scan_firmware_destroy_calls() { return scan_firmware_destroy_count.load(); }

void release_scan_open_identify()
{
    scan_open_identify_release.store(true, std::memory_order_release);
    scan_open_identify_release.notify_all();
}

std::thread::id scan_group_callback_thread()
{
    return scan_group_thread;
}

namespace dtv::spi {

namespace firmware_assets {
struct scan_firmware {
    static constexpr unsigned char data[]{0x31, 0x72, 0xb4, 0xe5};
    static constexpr std::size_t size = sizeof(data);
};
}

inline constexpr FunctionEntry scan_firmware_functions[]{
    {.function = 1, .bootstrap_ops = &scan_firmware_ops,
     .create_state = scan_firmware_create, .destroy_state = scan_firmware_destroy},
    {.function = 2, .bootstrap_ops = &scan_firmware_ops,
     .create_state = scan_firmware_create, .destroy_state = scan_firmware_destroy},
};

}

DTV_PRODUCT(scanfixture, DTV_FEATURES(::dtv::spi::Feature::LnbPower,
    ::dtv::spi::Feature::OutputPidFilter, ::dtv::spi::Feature::Card));
inline constexpr dtv::spi::model_information::receiver_information scan_receivers[]{
    {0, 0, 1u << DTV_SYSTEM_ISDB_T, 188, 0,
     (1u << (DTV_OP_OUTPUT_PID_FILTER - 1)) |
         (1u << (DTV_OP_TUNE_ISDB_T - 1)) | (1u << (DTV_OP_SIGNAL - 1)) |
         (1u << (DTV_OP_SELECT_TSID - 1)) | (1u << (DTV_OP_LNA - 1)),
     (1u << (DTV_OP_OUTPUT_PID_FILTER - 1)) |
         (1u << (DTV_OP_TUNE_ISDB_T - 1)) | (1u << (DTV_OP_SIGNAL - 1)) |
         (1u << (DTV_OP_SELECT_TSID - 1)) | (1u << (DTV_OP_LNA - 1)), DTV_TS_DESCRAMBLED},
    {1, 1, 1u << DTV_SYSTEM_ISDB_S, 192, 4,
     (1u << (DTV_OP_OUTPUT_PID_FILTER - 1)) |
         (1u << (DTV_OP_TUNE_ISDB_S - 1)) | (1u << (DTV_OP_SIGNAL - 1)) |
         (1u << (DTV_OP_SELECT_SLOT - 1)) | (1u << (DTV_OP_SELECT_TSID - 1)) |
         (1u << (DTV_OP_LNA - 1)),
     (1u << (DTV_OP_TUNE_ISDB_S - 1)) | (1u << (DTV_OP_SIGNAL - 1)) |
         (1u << (DTV_OP_SELECT_SLOT - 1)) | (1u << (DTV_OP_SELECT_TSID - 1)) |
         (1u << (DTV_OP_LNA - 1)), DTV_TS_SCRAMBLED, 4},
    {2, 2, 1u << DTV_SYSTEM_ISDB_T, 188, 0,
     (1u << (DTV_OP_TUNE_ISDB_T - 1)) | (1u << (DTV_OP_SIGNAL - 1)),
     0, DTV_TS_UNKNOWN, 3},
};
inline constexpr dtv::spi::model_information::receiver_information scan_receivers_second[]{
    {3, 0, 1u << DTV_SYSTEM_ISDB_T, 188, 0,
     (1u << (DTV_OP_OUTPUT_PID_FILTER - 1)) |
         (1u << (DTV_OP_TUNE_ISDB_T - 1)) | (1u << (DTV_OP_SIGNAL - 1)) |
         (1u << (DTV_OP_SELECT_TSID - 1)) | (1u << (DTV_OP_LNA - 1)),
     (1u << (DTV_OP_OUTPUT_PID_FILTER - 1)) |
         (1u << (DTV_OP_TUNE_ISDB_T - 1)) | (1u << (DTV_OP_SIGNAL - 1)) |
         (1u << (DTV_OP_SELECT_TSID - 1)) | (1u << (DTV_OP_LNA - 1)), DTV_TS_DESCRAMBLED},
};
inline constexpr dtv::spi::model_information::device_receiver_information scan_device_receivers[]{
    {1, true, scan_receivers, std::size(scan_receivers)},
};
inline constexpr dtv::spi::model_information::device_receiver_information scan_runtime_device_receivers[]{
    {2, true, scan_receivers_second, std::size(scan_receivers_second)},
};

namespace dtv::spi {

extern const ModelEntry scan_model_model{
    .model = {"Scan Fixture", 0x1234, 0x0101},
    .firmware = nullptr,
    .firmware_count = 0,
    .cold_identities = scan_identities,
    .cold_identity_count = sizeof(scan_identities) / sizeof(scan_identities[0]),
    .select_usb_group = select_scan_group,
    .functions = scan_firmware_functions,
    .function_count = std::size(scan_firmware_functions),
    .source_stride_bytes = 188,
    .public_open = &scan_public_open_ops,
    .runtime_device_receivers = ::scan_runtime_device_receivers,
    .runtime_device_receiver_count = std::size(::scan_runtime_device_receivers),
};

}
DTV_MODEL(scanfixture, scan_model,
          DTV_FEATURES(::dtv::spi::Feature::LnbPower,
              ::dtv::spi::Feature::OutputPidFilter, ::dtv::spi::Feature::Card),
          DTV_FIRMWARE(scan_firmware),
          (::dtv::spi::model_information{"Scan Fixture", 0x1234, 0x0101,
                                           scan_device_receivers, std::size(scan_device_receivers)}));
DTV_DEFINE_MODEL(scanfixture, scan_model);

namespace {

int32_t scan_firmware_transfer(dtv_context *context,
    const dtv::spi::FirmwareSession *session, dtv_bytes package, dtv_error *)
{
    auto *state = static_cast<ScanFirmwareContext *>(context);
    if (state == nullptr || state->io == nullptr || session == nullptr ||
        session->device_id == 0 || session->connection_epoch == 0)
        return DTV_ERROR_NOT_READY;
    if (state->product_id == 0x0101 &&
        session->initial_identity.state == DTV_IDENTIFY_RUNTIME &&
        session->initial_identity.firmware_key == 1) return DTV_ERROR_OK;
    if (state->io->bulk_out == nullptr || state->io->bulk_in == nullptr)
        return DTV_ERROR_NOT_READY;
    scan_firmware_last_device.store(session->device_id, std::memory_order_release);
    scan_firmware_last_function.store(session->function, std::memory_order_release);
    scan_firmware_last_epoch.store(session->connection_epoch, std::memory_order_release);
    scan_firmware_last_handle.store(session->handle_index, std::memory_order_release);
    auto result = state->io->bulk_out(state->io->user, 0x01, package,
                                        dtv_timeout{1000, 0});
    if (result.code == DTV_INTERNAL_RESULT_CANCELLED) return DTV_ERROR_CANCELLED;
    if (result.code == DTV_INTERNAL_RESULT_TIMEOUT) return DTV_ERROR_TIMEOUT;
    if (result.code != DTV_INTERNAL_RESULT_OK) return DTV_ERROR_USB_IO;
    std::array<unsigned char, 2> acknowledgement{};
    dtv_buffer reply{acknowledgement.data(), acknowledgement.size(), 0};
    result = state->io->bulk_in(state->io->user, 0x83, &reply, dtv_timeout{1000, 0});
    if (result.code == DTV_INTERNAL_RESULT_CANCELLED) return DTV_ERROR_CANCELLED;
    if (result.code == DTV_INTERNAL_RESULT_TIMEOUT) return DTV_ERROR_TIMEOUT;
    if (result.code != DTV_INTERNAL_RESULT_OK) return DTV_ERROR_USB_IO;
    if (reply.size != acknowledgement.size() || acknowledgement[0] != 0x83 ||
        acknowledgement[1] != 0x84) return DTV_ERROR_FIRMWARE_REJECTED;
    state->mode = DTV_IDENTIFY_RUNTIME;
    scan_firmware_same_id_mode.store(DTV_IDENTIFY_RUNTIME, std::memory_order_release);
    {
        std::lock_guard lock(scan_firmware_mutex);
        if (std::find(scan_firmware_runtime_devices.begin(), scan_firmware_runtime_devices.end(),
                      session->device_id) == scan_firmware_runtime_devices.end())
            scan_firmware_runtime_devices.push_back(session->device_id);
    }
    return DTV_ERROR_OK;
}

}

extern "C" int32_t dtv_model_impl_send_firmware_scanfixture_scan_model(
    dtv_device *device, const uint8_t *firmware, uint64_t size,
    const dtv_call_options *options, dtv_error *error)
{
    return dtv::spi::with_firmware_session(device, {firmware, size}, options,
        scan_firmware_transfer, error);
}

extern "C" int32_t dtv_model_impl_lnb_on_scanfixture_scan_model(
    dtv_enclosure *enclosure, const dtv_call_options *options, dtv_error *error)
{
    std::array<unsigned char, 2> reply{};
    std::size_t reply_size = 0;
    const auto result = dtv::internal::run_public_lnb_operation(enclosure, DTV_LNB_ON,
        dtv_timeout{options == nullptr ? 0 : options->timeout_ms, 0}, reply, &reply_size, error);
    if (result != DTV_ERROR_OK) return result;
    return reply_size == reply.size() && reply[0] == 0xa5 && reply[1] == DTV_LNB_ON
        ? DTV_ERROR_OK : DTV_ERROR_USB_IO;
}

extern "C" int32_t dtv_model_impl_lnb_off_scanfixture_scan_model(
    dtv_enclosure *enclosure, const dtv_call_options *options, dtv_error *error)
{
    std::array<unsigned char, 2> reply{};
    std::size_t reply_size = 0;
    const auto result = dtv::internal::run_public_lnb_operation(enclosure, DTV_LNB_OFF,
        dtv_timeout{options == nullptr ? 0 : options->timeout_ms, 0}, reply, &reply_size, error);
    if (result != DTV_ERROR_OK) return result;
    return reply_size == reply.size() && reply[0] == 0xa5 && reply[1] == DTV_LNB_OFF
        ? DTV_ERROR_OK : DTV_ERROR_USB_IO;
}

extern "C" int32_t dtv_model_impl_reset_card_scanfixture_scan_model(
    dtv_enclosure *enclosure, const dtv_call_options *options,
    std::vector<std::uint8_t> *out, dtv_error *error)
{
    if (enclosure == nullptr || out == nullptr) return DTV_ERROR_INVALID_ARGUMENT;
    const auto transaction = static_cast<unsigned char>(
        scan_card_transaction.fetch_add(1, std::memory_order_acq_rel) % 254 + 1);
    std::array<unsigned char, 2> arguments{0x52, transaction};
    std::array<unsigned char, 256> reply{};
    std::size_t reply_size = 0;
    const dtv_timeout timeout{options == nullptr ? 0 : options->timeout_ms, 0};
    const auto result = dtv::internal::run_public_card_operation(enclosure, 4,
        arguments, timeout, reply, &reply_size, error);
    if (result != DTV_ERROR_OK) return result;
    if (reply_size < 2) return DTV_ERROR_CARD_IO;
    if (reply[0] == 1) return DTV_ERROR_NO_CARD;
    if (reply[0] == 2) return DTV_ERROR_CARD_REMOVED;
    if (reply[0] != 0) return DTV_ERROR_CARD_IO;
    try {
        out->assign(reply.begin() + 1, reply.begin() + static_cast<std::ptrdiff_t>(reply_size));
    } catch (...) {
        return DTV_ERROR_RESOURCE_LIMIT;
    }
    return DTV_ERROR_OK;
}

extern "C" int32_t dtv_model_impl_set_pid_filter_scanfixture_scan_model(
    dtv_stream *stream, const std::uint16_t *pids, std::uint32_t count, dtv_error *error)
{
    if (stream == nullptr || (count != 0 && pids == nullptr)) {
        if (error != nullptr && error->struct_size >= sizeof(error->code))
            error->code = DTV_ERROR_INVALID_ARGUMENT;
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::vector<unsigned char> arguments{1, 90, 0, 0, 0x57};
        const auto transaction = scan_pid_transaction_next.fetch_add(1,
            std::memory_order_acq_rel) % 255 + 1;
        arguments[3] = static_cast<unsigned char>(transaction);
        arguments.push_back(static_cast<unsigned char>(count));
        arguments.push_back(static_cast<unsigned char>(count >> 8));
        arguments.push_back(static_cast<unsigned char>(count >> 16));
        arguments.push_back(static_cast<unsigned char>(count >> 24));
        for (std::uint32_t index = 0; index < count; ++index) {
            arguments.push_back(static_cast<unsigned char>(pids[index]));
            arguments.push_back(static_cast<unsigned char>(pids[index] >> 8));
        }
        {
            std::lock_guard lock(scan_pid_mutex);
            if (scan_pid_pending_valid) {
                if (error != nullptr && error->struct_size >= sizeof(error->code))
                    error->code = DTV_ERROR_BUSY;
                return DTV_ERROR_BUSY;
            }
            scan_pid_pending.clear();
            if (count != 0) scan_pid_pending.assign(pids, pids + count);
            scan_pid_pending_all = count == 0;
            scan_pid_pending_valid = true;
            scan_pid_transaction.store(transaction, std::memory_order_release);
        }
        std::array<unsigned char, 32> reply{};
        std::size_t reply_size = 0;
        const auto result = dtv::internal::run_public_stream_operation(stream, 3,
            arguments, {}, reply, &reply_size, error);
        {
            std::lock_guard lock(scan_pid_mutex);
            if (scan_pid_pending_valid &&
                scan_pid_transaction.load(std::memory_order_acquire) == transaction) {
                scan_pid_pending.clear();
                scan_pid_pending_valid = false;
            }
        }
        if (result != DTV_ERROR_OK) return result;
        if (reply_size != 1 || reply[0] != 0) {
            if (error != nullptr && error->struct_size >= sizeof(error->code))
                error->code = DTV_ERROR_USB_IO;
            return DTV_ERROR_USB_IO;
        }
        if (error != nullptr && error->struct_size >= sizeof(error->code))
            error->code = DTV_ERROR_OK;
        return DTV_ERROR_OK;
    } catch (...) {
        if (error != nullptr && error->struct_size >= sizeof(error->code))
            error->code = DTV_ERROR_RESOURCE_LIMIT;
        return DTV_ERROR_RESOURCE_LIMIT;
    }
}
