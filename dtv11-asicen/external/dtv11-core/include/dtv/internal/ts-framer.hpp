/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_TS_FRAMER_HPP
#define DTV_INTERNAL_TS_FRAMER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dtv::internal {

enum class TsFramerCode {
    ok,
    invalid_configuration,
    input_limit,
    output_full
};

struct TsFramerResult {
    TsFramerCode code;
    /* Input bytes accepted into bounded internal storage; retry the remainder. */
    std::size_t consumed;
    /* Valid bytes written to the caller's output span, always whole source units. */
    std::size_t produced;
};

struct TsFramerStats {
    std::uint64_t dropped_bytes;
    std::uint64_t desyncs;
};

class TsFramer {
public:
    /* max_pending_bytes is caller-selected; it is not a device-buffer size. */
    TsFramer(std::uint16_t stride_bytes, std::uint8_t sync_offset_bytes,
             std::uint8_t ready_units, std::size_t max_pending_bytes);

    bool valid() const { return valid_; }
    /* Input/output spans are borrowed only for this call; output is not retained. */
    TsFramerResult feed(std::span<const std::uint8_t> input,
                        std::span<std::uint8_t> output);
    /* A new generation discards carry and gate state and restarts both counters. */
    void reset(std::uint64_t generation);

    std::uint64_t generation() const { return generation_; }
    TsFramerStats stats() const { return stats_; }
    std::size_t pending_bytes() const { return pending_.size() - head_; }

private:
    bool process(std::span<std::uint8_t> output, std::size_t &produced,
                 bool &output_full);
    void discard_one();
    void lose_sync();

    std::uint16_t stride_bytes_;
    std::uint8_t sync_offset_bytes_;
    std::uint8_t ready_units_;
    std::size_t max_pending_bytes_;
    bool valid_;
    bool locked_ = false;
    bool gate_pending_ = false;
    std::size_t head_ = 0;
    std::uint64_t generation_ = 0;
    TsFramerStats stats_{};
    std::vector<std::uint8_t> pending_;
};

}

#endif
