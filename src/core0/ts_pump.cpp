// SPDX-License-Identifier: GPL-2.0-only
#include "core0/ts_pump.h"

namespace torne::core0 {

TsPump::TsPump(usb::Transport& transport, Framer& framer, Clock& clock,
               std::size_t depth, std::size_t buffer_size,
               PacketSink sink, void* sink_context) noexcept
    : transport_(transport), framer_(framer), clock_(clock),
      buffer_size_(buffer_size), buffers_(depth),
      ids_(depth, 0U), epochs_(depth, 0U), active_(depth, false),
      sink_(sink), sink_context_(sink_context)
{
    for (std::vector<std::uint8_t>& buffer : buffers_)
        buffer.resize(buffer_size_);
}

TsPump::~TsPump() noexcept
{
    if (!running_ && id_to_index_.empty()) return;
    if (stop() != 0) {
        // A callback has not completed. Keep buffer storage alive because
        // native transfers still point into it; leak rather than free early.
        (void)new std::vector<std::vector<std::uint8_t>>(std::move(buffers_));
    }
}

int TsPump::submit(std::size_t index) noexcept
{
    if (index >= buffers_.size() || buffer_size_ == 0U) return -1;
    std::uint64_t id = 0U;
    if (transport_.async_bulk_in_submit(0x81U, buffers_[index].data(),
                                        buffer_size_, 1000U, id) != 0)
        return -1;
    ids_[index] = id;
    epochs_[index] = epoch_;
    active_[index] = true;
    id_to_index_[id] = index;
    return 0;
}

int TsPump::start(std::uint32_t epoch) noexcept
{
    if (running_) return -1;
    epoch_ = epoch;
    stopping_ = false;
    failed_ = false;
    stats_ = PumpStats{};
    for (std::size_t index = 0U; index < buffers_.size(); ++index) {
        active_[index] = false;
        ids_[index] = 0U;
    }
    id_to_index_.clear();
    running_ = true;
    framer_.reset();
    for (std::size_t index = 0U; index < buffers_.size(); ++index) {
        if (submit(index) != 0) {
            (void)stop();
            return -1;
        }
    }
    return 0;
}

int TsPump::pump(std::size_t max_completions) noexcept
{
    if (!running_ || stopping_) return -1;
    usb::UsbIo::AsyncBulkEvent event{};
    std::size_t completed = 0U;
    while (true) {
        if (max_completions != 0U && completed >= max_completions) break;
        const int polled = transport_.async_bulk_poll(event);
        if (polled < 0) return -1;
        if (polled == 0) break;
        const auto found = id_to_index_.find(event.id);
        if (found == id_to_index_.end()) continue;
        const std::size_t index = found->second;
        id_to_index_.erase(found);
        if (index >= buffers_.size()) continue;
        active_[index] = false;
        ++stats_.transfers;
        ++completed;
        const bool usable = !stopping_ && !failed_ && epochs_[index] == epoch_ &&
                            (event.transfer.state == usb::TransferState::complete ||
                             event.transfer.state == usb::TransferState::short_transfer);
        if (usable) {
            stats_.bytes += event.transfer.transferred;
            packets_.clear();
            framer_.push(event.data, event.transfer.transferred, packets_);
            if (sink_ != nullptr) {
                for (const auto& packet : packets_)
                    sink_(sink_context_, packet.data(), packet.size());
            }
        } else if (event.transfer.state == usb::TransferState::failed ||
                   event.transfer.state == usb::TransferState::outcome_unknown) {
            ++stats_.errors;
            failed_ = true;
        } else {
            ++stats_.discarded_old_epoch;
        }
        if (running_ && !stopping_ && !failed_) {
            if (submit(index) != 0) return -1;
        }
    }
    return failed_ ? -1 : 0;
}

int TsPump::stop() noexcept
{
    if (!running_ && id_to_index_.empty()) return 0;
    stopping_ = true;
    (void)transport_.async_bulk_cancel_all();
    const std::uint64_t start = clock_.monotonic_ms();
    usb::UsbIo::AsyncBulkEvent event{};
    while (true) {
        const int polled = transport_.async_bulk_poll(event);
        if (polled < 0) {
            stopping_ = true;
            return -1;
        }
        if (polled > 0) {
            const auto found = id_to_index_.find(event.id);
            if (found != id_to_index_.end()) {
                const std::size_t index = found->second;
                id_to_index_.erase(found);
                if (index < active_.size()) active_[index] = false;
            }
            continue;
        }
        if (transport_.async_bulk_active() == 0U) break;
        if (clock_.monotonic_ms() - start >= 100U) {
            stopping_ = true;
            return -1;
        }
        clock_.sleep_ms(1U);
    }
    for (std::size_t index = 0U; index < active_.size(); ++index)
        active_[index] = false;
    id_to_index_.clear();
    running_ = false;
    stopping_ = false;
    return 0;
}

}  // namespace torne::core0
