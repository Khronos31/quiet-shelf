/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ground_tune_common.hpp"

namespace asicen::detail::ground_tune {

namespace {

struct BandMultiplier {
    std::uint8_t multiplier;
    std::uint8_t reg5;
    std::uint8_t reg6;
};

constexpr BandMultiplier band_multipliers[] = {
    {96, 0x82, 0x00},
    {64, 0x82, 0x02},
    {48, 0x42, 0x00},
    {32, 0x42, 0x02},
    {24, 0x22, 0x00},
    {16, 0x22, 0x02},
    {12, 0x12, 0x00},
    {8, 0x12, 0x02},
    {6, 0x0a, 0x00},
    {4, 0x0a, 0x02},
};

std::uint64_t center_frequency_khz(std::uint64_t frequency_khz)
{
    if (frequency_khz >= 165000 && frequency_khz <= 167000) return 167143;
    if (frequency_khz >= 195000 && frequency_khz <= 197999) return 195143;
    if (frequency_khz >= 471000 && frequency_khz <= 473000) return 473143;
    return (frequency_khz / 1000) * 1000 + 0x8f;
}

}

bool calculate_khz(std::uint64_t frequency_khz, TuneValues *out)
{
    if (out == nullptr) return false;

    const auto center_khz = center_frequency_khz(frequency_khz);
    constexpr std::uint64_t max_vco = 0x36523f;

    const BandMultiplier *band = nullptr;
    for (const auto &candidate : band_multipliers) {
        if (center_khz <= max_vco / candidate.multiplier) {
            band = &candidate;
            break;
        }
    }
    if (band == nullptr) return false;

    const auto vco = center_khz * band->multiplier;
    const bool vco_selected = vco >= 3060000;
    const auto xdiv = (vco + 9000) / 18000;
    auto pm = xdiv / 8;
    const auto am = xdiv % 8;
    std::uint64_t reg1 = am;
    if (am < 2) {
        reg1 += 8;
        --pm;
    }

    const auto remainder = vco - (vco / 18000) * 18000;
    auto xin = (remainder << 15) / 18000;
    if (xin >= 16384) xin += 32768;

    TuneValues values{
        .input_frequency_khz = frequency_khz,
        .center_frequency_khz = center_khz,
        .tuner = {
            .reg1 = static_cast<std::uint8_t>(reg1),
            .reg2 = static_cast<std::uint8_t>(pm),
            .reg3 = static_cast<std::uint8_t>(xin >> 8),
            .reg4 = static_cast<std::uint8_t>(xin & 0xff),
            .reg5 = static_cast<std::uint8_t>(band->reg5 | 0x07),
            .reg6 = static_cast<std::uint8_t>(band->reg6 | 0x80 |
                                              (vco_selected ? 0x08 : 0x00)),
            .vco_selected = vco_selected,
        },
    };
    *out = values;
    return true;
}

std::uint8_t agc_value(std::uint8_t current, std::uint64_t center_frequency_khz)
{
    if (center_frequency_khz <= 260999) {
        return static_cast<std::uint8_t>((current & 0xcf) | 0x20);
    }
    return static_cast<std::uint8_t>(current | 0x30);
}

Lock classify_lock(std::uint8_t register_value)
{
    return (register_value & 0x0f) == 0x09 ? Lock::locked : Lock::unlocked;
}

}
