// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include <dtv/internal/ts-framer.hpp>
#include <dtv/spi/device_ops.hpp>

namespace {

std::vector<std::uint8_t> units(std::uint16_t stride, std::uint8_t sync_offset,
                                std::size_t count, std::uint8_t seed = 1)
{
    std::vector<std::uint8_t> result(static_cast<std::size_t>(stride) * count);
    for (std::size_t unit = 0; unit < count; ++unit) {
        for (std::size_t byte = 0; byte < stride; ++byte) {
            result[unit * stride + byte] = static_cast<std::uint8_t>(seed + unit + byte);
        }
        result[unit * stride + sync_offset] = 0x47;
    }
    return result;
}

int check(bool value, int code) { return value ? 0 : code; }

}

int main()
{
    using dtv::internal::TsFramer;
    using dtv::internal::TsFramerCode;

    const dtv::spi::ModelEntry unreported{};
    const dtv::spi::ModelEntry ts188{.source_stride_bytes = 188,
                                     .source_sync_offset_bytes = 0};
    const dtv::spi::ModelEntry ts192{.source_stride_bytes = 192,
                                     .source_sync_offset_bytes = 4};
    const dtv::spi::ModelEntry bad_pair{.source_stride_bytes = 188,
                                        .source_sync_offset_bytes = 4};
    if (int failed = check(dtv::spi::valid_source_metadata(unreported) &&
                           dtv::spi::valid_source_metadata(ts188) &&
                           dtv::spi::valid_source_metadata(ts192) &&
                           !dtv::spi::valid_source_metadata(bad_pair), 1)) return failed;
    if (int failed = check(!TsFramer(0, 0, 2, 376).valid() &&
                           !TsFramer(188, 4, 2, 376).valid() &&
                           !TsFramer(188, 0, 3, 564).valid(), 2)) return failed;

    const auto source188 = units(188, 0, 6);
    TsFramer split188(188, 0, 2, 752);
    std::vector<std::uint8_t> aligned188;
    std::array<std::uint8_t, 752> output{};
    for (const auto byte : source188) {
        const std::array chunk{byte};
        const auto result = split188.feed(chunk, output);
        if (result.code != TsFramerCode::ok || result.consumed != 1) return 3;
        aligned188.insert(aligned188.end(), output.begin(), output.begin() + result.produced);
    }
    if (int failed = check(aligned188 == source188 && split188.pending_bytes() == 0, 4)) return failed;

    const auto source192 = units(192, 4, 5, 9);
    TsFramer split192(192, 4, 2, 768);
    std::vector<std::uint8_t> aligned192;
    std::array<std::uint8_t, 384> output192{};
    for (std::size_t offset = 0; offset < source192.size();) {
        const auto count = std::min<std::size_t>(17, source192.size() - offset);
        const auto result = split192.feed(std::span(source192).subspan(offset, count), output192);
        if (result.code != TsFramerCode::ok || result.consumed != count) return 5;
        aligned192.insert(aligned192.end(), output192.begin(), output192.begin() + result.produced);
        offset += count;
    }
    if (int failed = check(aligned192 == source192, 6)) return failed;

    TsFramer false_sync(188, 0, 2, 752);
    std::vector<std::uint8_t> noisy{0x47, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    noisy.insert(noisy.end(), source188.begin(), source188.begin() + 3 * 188);
    std::vector<std::uint8_t> aligned_noisy(noisy.size());
    const auto noisy_result = false_sync.feed(noisy, aligned_noisy);
    if (int failed = check(noisy_result.code == TsFramerCode::ok &&
                           std::vector<std::uint8_t>(aligned_noisy.begin(),
                               aligned_noisy.begin() + noisy_result.produced) ==
                               std::vector<std::uint8_t>(source188.begin(),
                                   source188.begin() + 3 * 188) &&
                           false_sync.stats().dropped_bytes == 7, 7)) return failed;

    TsFramer gated(188, 0, 4, 752);
    std::vector<std::uint8_t> bad_candidate = units(188, 0, 4, 33);
    bad_candidate[2 * 188] = 0x00;
    for (std::size_t i = 0; i < 3 * 188; ++i) bad_candidate.push_back(0x22);
    const auto reacquired = units(188, 0, 4, 71);
    bad_candidate.insert(bad_candidate.end(), reacquired.begin(), reacquired.end());
    std::vector<std::uint8_t> gated_output(bad_candidate.size());
    const auto gated_result = gated.feed(bad_candidate, gated_output);
    if (int failed = check(gated_result.code == TsFramerCode::ok &&
                           std::vector<std::uint8_t>(gated_output.begin(),
                               gated_output.begin() + gated_result.produced) == reacquired &&
                           gated.stats().desyncs == 1, 8)) return failed;

    TsFramer fourth_gate(188, 0, 4, 752);
    auto bad_fourth = units(188, 0, 4, 53);
    bad_fourth[3 * 188] = 0x00;
    bad_fourth.insert(bad_fourth.end(), 3 * 188, 0x22);
    bad_fourth.insert(bad_fourth.end(), reacquired.begin(), reacquired.end());
    std::vector<std::uint8_t> fourth_output(bad_fourth.size());
    const auto fourth_result = fourth_gate.feed(bad_fourth, fourth_output);
    if (int failed = check(fourth_result.code == TsFramerCode::ok &&
                           std::vector<std::uint8_t>(fourth_output.begin(),
                               fourth_output.begin() + fourth_result.produced) == reacquired &&
                           fourth_gate.stats().desyncs == 2 &&
                           fourth_gate.stats().dropped_bytes == 7 * 188, 16)) return failed;

    TsFramer rearmed(188, 0, 4, 752);
    const auto established = units(188, 0, 4, 111);
    std::vector<std::uint8_t> initial_output(752);
    const auto initial_result = rearmed.feed(established, initial_output);
    if (int failed = check(initial_result.code == TsFramerCode::ok &&
                           initial_result.produced == established.size(), 12)) return failed;
    std::vector<std::uint8_t> lost_sync(4 * 188, 0x19);
    const auto after_loss = units(188, 0, 4, 141);
    lost_sync.insert(lost_sync.end(), after_loss.begin(), after_loss.end());
    std::vector<std::uint8_t> rearmed_output(lost_sync.size());
    const auto rearmed_result = rearmed.feed(lost_sync, rearmed_output);
    if (int failed = check(rearmed_result.code == TsFramerCode::ok &&
                           std::vector<std::uint8_t>(rearmed_output.begin(),
                               rearmed_output.begin() + rearmed_result.produced) == after_loss &&
                           rearmed.stats().desyncs == 1 &&
                           rearmed.stats().dropped_bytes == 4 * 188, 13)) return failed;

    TsFramer generation(188, 0, 2, 752);
    const auto old_chunk = units(188, 0, 1, 3);
    const auto old_result = generation.feed(old_chunk, {});
    if (int failed = check(old_result.code == TsFramerCode::ok &&
                           old_result.consumed == 188 && generation.pending_bytes() == 188, 14))
        return failed;
    generation.reset(42);
    const auto new_generation = units(188, 0, 2, 205);
    std::array<std::uint8_t, 376> new_output{};
    const auto new_result = generation.feed(new_generation, new_output);
    if (int failed = check(new_result.code == TsFramerCode::ok &&
                           generation.generation() == 42 &&
                           std::vector<std::uint8_t>(new_output.begin(), new_output.end()) ==
                               new_generation && generation.pending_bytes() == 0, 15)) return failed;

    TsFramer stalled(188, 0, 2, 376);
    const auto longer = units(188, 0, 4, 101);
    const auto stalled_result = stalled.feed(longer, {});
    if (int failed = check(stalled_result.code == TsFramerCode::output_full &&
                           stalled_result.consumed == 376 && stalled_result.produced == 0 &&
                           stalled.pending_bytes() == 376, 9)) return failed;
    std::vector<std::uint8_t> drained(752);
    const auto drain_result = stalled.feed(
        std::span(longer).subspan(stalled_result.consumed), drained);
    if (int failed = check(drain_result.code == TsFramerCode::ok &&
                           drain_result.consumed == longer.size() - stalled_result.consumed &&
                           drain_result.produced == longer.size() &&
                           std::vector<std::uint8_t>(drained.begin(), drained.end()) == longer &&
                           stalled.pending_bytes() == 0, 10)) return failed;

    const auto old_stats = split188.stats();
    split188.reset(43);
    if (int failed = check(split188.generation() == 43 && split188.pending_bytes() == 0 &&
                           split188.stats().dropped_bytes == 0 &&
                           split188.stats().desyncs == 0 && old_stats.desyncs == 0, 11)) return failed;

    return 0;
}
