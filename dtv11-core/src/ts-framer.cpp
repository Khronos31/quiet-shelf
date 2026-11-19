// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/ts-framer.hpp>

#include <algorithm>
#include <limits>

namespace dtv::internal {
namespace {

void increment(std::uint64_t &value)
{
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

}

TsFramer::TsFramer(std::uint16_t stride_bytes, std::uint8_t sync_offset_bytes,
                   std::uint8_t ready_units, std::size_t max_pending_bytes)
    : stride_bytes_(stride_bytes), sync_offset_bytes_(sync_offset_bytes),
      ready_units_(ready_units), max_pending_bytes_(max_pending_bytes),
      valid_((stride_bytes == 188 && sync_offset_bytes == 0) ||
             (stride_bytes == 192 && sync_offset_bytes == 4))
{
    const auto required = static_cast<std::size_t>(stride_bytes_) * ready_units_;
    valid_ = valid_ && (ready_units_ == 2 || ready_units_ == 4) &&
             max_pending_bytes_ >= required && required / ready_units_ == stride_bytes_;
    if (valid_) pending_.reserve(max_pending_bytes_);
}

void TsFramer::discard_one()
{
    if (head_ == pending_.size()) return;
    ++head_;
    increment(stats_.dropped_bytes);
}

void TsFramer::lose_sync()
{
    increment(stats_.desyncs);
    locked_ = false;
    gate_pending_ = false;
}

bool TsFramer::process(std::span<std::uint8_t> output, std::size_t &produced,
                       bool &output_full)
{
    bool changed = false;
    while (valid_) {
        const auto pending_size = pending_.size() - head_;
        if (!locked_) {
            const auto second_sync = static_cast<std::size_t>(stride_bytes_) + sync_offset_bytes_;
            if (pending_size <= second_sync) break;
            if (pending_[head_ + sync_offset_bytes_] != 0x47 ||
                pending_[head_ + second_sync] != 0x47) {
                discard_one();
                changed = true;
                continue;
            }
            locked_ = true;
            gate_pending_ = true;
            changed = true;
        }

        if (gate_pending_) {
            const auto available_units = std::min<std::size_t>(
                ready_units_, pending_size / stride_bytes_);
            bool gate_failed = false;
            for (std::size_t unit = 0; unit < available_units; ++unit) {
                if (pending_[head_ + unit * stride_bytes_ + sync_offset_bytes_] != 0x47) {
                    gate_failed = true;
                    break;
                }
            }
            if (gate_failed) {
                lose_sync();
                discard_one();
                changed = true;
                continue;
            }
            if (pending_size < static_cast<std::size_t>(stride_bytes_) * ready_units_) break;
            gate_pending_ = false;
            changed = true;
        }

        if (pending_size < stride_bytes_) break;
        if (pending_[head_ + sync_offset_bytes_] != 0x47) {
            lose_sync();
            discard_one();
            changed = true;
            continue;
        }
        if (output.size() - produced < stride_bytes_) {
            output_full = true;
            break;
        }
        std::copy_n(pending_.begin() + head_, stride_bytes_, output.begin() + produced);
        produced += stride_bytes_;
        head_ += stride_bytes_;
        changed = true;
    }
    return changed;
}

TsFramerResult TsFramer::feed(std::span<const std::uint8_t> input,
                              std::span<std::uint8_t> output)
{
    if (!valid_) return {TsFramerCode::invalid_configuration, 0, 0};
    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (;;) {
        bool output_full = false;
        process(output, produced, output_full);
        if (output_full) return {TsFramerCode::output_full, consumed, produced};
        if (consumed == input.size()) return {TsFramerCode::ok, consumed, produced};

        const auto pending_size = pending_.size() - head_;
        const auto available = max_pending_bytes_ - pending_size;
        if (available == 0) return {TsFramerCode::input_limit, consumed, produced};
        const auto count = std::min(available, input.size() - consumed);
        if (pending_.size() + count > max_pending_bytes_) {
            pending_.erase(pending_.begin(), pending_.begin() + head_);
            head_ = 0;
        }
        pending_.insert(pending_.end(), input.begin() + consumed,
                        input.begin() + consumed + count);
        consumed += count;
    }
}

void TsFramer::reset(std::uint64_t generation)
{
    pending_.clear();
    head_ = 0;
    locked_ = false;
    gate_pending_ = false;
    generation_ = generation;
    stats_ = {};
}

}
