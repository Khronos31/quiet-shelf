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

namespace {

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
std::array<unsigned char, 963> scan_stream_192_source{};
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
                    scan_public_open_case == 16 || scan_public_open_case == 17) &&
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
                        scan_public_open_case == 17) {
                        plans[*count].shared_in_depth = scan_public_open_case == 10 && candidate_index == 0
                            ? 2 : 1;
                        plans[*count].prestream_sink_limit = 16;
                        if (scan_public_open_case == 13) {
                            plans[*count].prestream_sink_limit = 376;
                        }
                        plans[*count].reply_capacity = 32;
                        plans[*count].card_reader_source =
                            (scan_public_open_case == 12 || scan_public_open_case == 15 ||
                             scan_public_open_case == 16 || scan_public_open_case == 17) &&
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
    if (request->operation == 4) {
        plan->supported = true;
        plan->ready = true;
        plan->arguments_valid = request->target_kind ==
            dtv::spi::PublicOperationTargetKind::card && request->arguments.size() == 2 &&
            request->arguments[0] == 0x52 && request->arguments[1] != 0;
        if (!plan->arguments_valid) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        scan_card_transaction.store(request->arguments[1], std::memory_order_release);
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
    if (request == nullptr || consumed == nullptr || produced == nullptr ||
        request->source_stride_bytes != 192 || request->source_sync_offset_bytes != 4 ||
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
    scan_plan_stream, scan_transform_stream};

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

void set_scan_card_reply_mode(unsigned mode)
{
    scan_card_reply_mode.store(mode, std::memory_order_release);
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
    for (std::size_t unit = 0; unit < 5; ++unit) {
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
        scan_public_open_case == 17;
}

bool scan_operation_mode_enabled()
{
    return scan_public_open_case == 11 || scan_public_open_case == 13 ||
        scan_public_open_case == 15 || scan_public_open_case == 16 ||
        scan_public_open_case == 17;
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

extern const ModelEntry scan_model_model{
    .model = {"Scan Fixture", 0x1234, 0x0101},
    .firmware = nullptr,
    .firmware_count = 0,
    .cold_identities = scan_identities,
    .cold_identity_count = sizeof(scan_identities) / sizeof(scan_identities[0]),
    .select_usb_group = select_scan_group,
    .source_stride_bytes = 188,
    .public_open = &scan_public_open_ops,
};

}

DTV_PRODUCT(scanfixture, DTV_FEATURES(::dtv::spi::Feature::OutputPidFilter, ::dtv::spi::Feature::Card));
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
         (1u << (DTV_OP_LNA - 1)), DTV_TS_SCRAMBLED},
    {2, 2, 1u << DTV_SYSTEM_ISDB_T, 0, 0,
     (1u << (DTV_OP_TUNE_ISDB_T - 1)) | (1u << (DTV_OP_SIGNAL - 1)),
     0, DTV_TS_UNKNOWN},
};
inline constexpr dtv::spi::model_information::device_receiver_information scan_device_receivers[]{
    {1, true, scan_receivers, std::size(scan_receivers)},
};
DTV_MODEL(scanfixture, scan_model,
          DTV_FEATURES(::dtv::spi::Feature::OutputPidFilter, ::dtv::spi::Feature::Card), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"Scan Fixture", 0x1234, 0x0101,
                                           scan_device_receivers, std::size(scan_device_receivers)}));
DTV_DEFINE_MODEL(scanfixture, scan_model);

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
