/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_w3u3_v2_tune.hpp"

#include <algorithm>
#include <limits>

namespace asicen::px_w3u3_v2::detail {

namespace {

// PX-W3U3 V2はNMI RF synthesizerと32-bit tuner registerを使うため、W2/W3U3のRF PLL値を共有しない。

constexpr std::uint64_t band_threshold_hz[8] = {
    0, 0, 434000000, 237000000, 214000000, 118000000, 79000000, 53000000,
};

constexpr std::uint32_t band_multipliers_by_index[8] = {
    0, 1, 2, 3, 4, 6, 9, 12,
};

bool known_family(NmiFamily family)
{
    return family == NmiFamily::family_120 || family == NmiFamily::family_130 ||
           family == NmiFamily::family_131 || family == NmiFamily::extended;
}

std::uint64_t xo_frequency_hz(std::uint8_t control_bits)
{
    constexpr std::uint64_t base_xo_hz = 24000;
    if (control_bits == 2) return base_xo_hz * 2;
    if (control_bits == 3) return base_xo_hz / 2;
    return base_xo_hz;
}

bool calculate_pll_once(std::uint64_t rf_frequency_hz, std::uint64_t xo_hz,
                         std::uint8_t reference_control_bits,
                         NmiSynthesizerValues *out)
{
    const auto band_index = nmi_band_index(rf_frequency_hz);
    const auto band_multiplier = band_multipliers_by_index[band_index];
    const auto vco_hz = (rf_frequency_hz / 1000) * 8 * band_multiplier;
    const auto integer = vco_hz / xo_hz;
    const auto fractional_scale = static_cast<std::uint32_t>(0x80000000u / xo_hz);
    const auto fractional_product = static_cast<std::uint32_t>(vco_hz % xo_hz) *
                                    fractional_scale;
    const auto fraction = (fractional_product >> 12) & 0x7ffff;
    const auto divider_unclamped = (vco_hz / 216000) & 0xff;
    const auto divider = static_cast<std::uint8_t>(
        std::clamp<std::uint64_t>(divider_unclamped, 16, 31));
    if (vco_hz > (std::numeric_limits<std::uint64_t>::max() >> 9)) return false;
    const auto scaled_clock = (vco_hz << 9) / divider;
    if (scaled_clock < 110592000) return false;
    const auto clock_hz = scaled_clock - 110592000;

    NmiSynthesizerValues values{
        .family = NmiFamily::family_120,
        .requested_rf_hz = rf_frequency_hz,
        .programmed_rf_hz = rf_frequency_hz,
        .xo_hz = xo_hz,
        .vco_hz = vco_hz,
        .clock_hz = clock_hz,
        .integer = static_cast<std::uint32_t>(integer),
        .fraction = fraction,
        .band_multiplier = band_multiplier,
        .band_index = band_index,
        .reference_control_bits = reference_control_bits,
        .divider = divider,
        .pll_registers = {
            {0x01, static_cast<std::uint32_t>((integer & 0xffff) >> 1)},
            {0x02, static_cast<std::uint32_t>((integer & 1) | (fraction * 2))},
            {0x03, fraction >> 7},
            {0x04, ((fraction >> 15) & 15) | (static_cast<std::uint32_t>(divider) << 4)},
        },
        .band_register_update = {
            0x1d,
            0x1f,
            static_cast<std::uint32_t>(band_index) << 5,
        },
        .write_fraction_register = false,
        .fraction_register_value = static_cast<std::uint8_t>(fraction != 0 ? 0x0e : 0x08),
        .fraction_adjustment_applied = false,
    };
    *out = values;
    return true;
}

void append_value(NmiFamilySettings *settings, std::uint16_t address, std::uint32_t value)
{
    settings->values[settings->count++] = {address, value};
}

void append_selection_value(NmiSelectionValues *values, std::uint16_t address,
                            std::uint32_t value)
{
    values->family_values[values->family_value_count++] = {address, value};
}

bool contains_frequency(const std::uint64_t *frequencies, std::uint8_t count,
                        std::uint64_t frequency_hz)
{
    for (std::uint8_t i = 0; i < count; ++i) {
        if (frequencies[i] == frequency_hz) return true;
    }
    return false;
}

}

bool normalize_ground_frequency(std::uint64_t input_frequency_khz, GroundFrequency *out)
{
    if (out == nullptr || input_frequency_khz < 90000 || input_frequency_khz > 770000) {
        return false;
    }

    std::uint64_t rf_frequency_khz = 0;
    if (input_frequency_khz >= 165000 && input_frequency_khz <= 167143) {
        rf_frequency_khz = 167143;
    } else if (input_frequency_khz >= 195143 && input_frequency_khz <= 197143) {
        rf_frequency_khz = 195143;
    } else if (input_frequency_khz >= 471143 && input_frequency_khz <= 473143) {
        rf_frequency_khz = 473143;
    } else {
        rf_frequency_khz = (input_frequency_khz / 1000) * 1000 + 143;
    }

    *out = {input_frequency_khz, rf_frequency_khz * 1000};
    return true;
}

std::uint8_t nmi_band_index(std::uint64_t rf_frequency_hz)
{
    std::uint8_t index = 7;
    while (index > 1 && rf_frequency_hz > band_threshold_hz[index]) --index;
    return index;
}

std::uint8_t family130_reference_control_bits(std::uint64_t rf_frequency_hz)
{
    constexpr std::uint64_t reference_x2_frequency_hz[] = {
        171000000, 506000000, 554000000, 602000000, 650000000,
        698000000, 746000000, 794000000, 842000000,
    };
    if (rf_frequency_hz == 147000000) return 3;
    for (const auto frequency_hz : reference_x2_frequency_hz) {
        if (rf_frequency_hz == frequency_hz) return 2;
    }
    return 1;
}

std::uint8_t rf_register_05_value(std::uint64_t rf_frequency_hz)
{
    return rf_frequency_hz >= 120100000 && rf_frequency_hz <= 120400000 ? 0x05 : 0x85;
}

bool calculate_nmi_synthesizer(NmiFamily family, std::uint64_t rf_frequency_hz,
                               std::uint8_t register_21_value, NmiSynthesizerValues *out)
{
    if (out == nullptr || !known_family(family)) return false;

    const auto reference_control_bits =
        family == NmiFamily::family_130
            ? family130_reference_control_bits(rf_frequency_hz)
            : static_cast<std::uint8_t>(register_21_value & 0x03);
    const auto xo_hz = xo_frequency_hz(reference_control_bits);

    auto programmed_rf_hz = rf_frequency_hz;
    bool fraction_adjustment_applied = false;
    NmiSynthesizerValues values{};
    for (std::uint8_t attempt = 0; attempt < 2; ++attempt) {
        if (!calculate_pll_once(programmed_rf_hz, xo_hz, reference_control_bits, &values)) {
            return false;
        }
        if (values.fraction == 0 || fraction_adjustment_applied) break;
        if (programmed_rf_hz > std::numeric_limits<std::uint64_t>::max() - 1000) return false;
        programmed_rf_hz += 1000;
        fraction_adjustment_applied = true;
    }

    values.family = family;
    values.requested_rf_hz = rf_frequency_hz;
    values.programmed_rf_hz = programmed_rf_hz;
    values.fraction_adjustment_applied = fraction_adjustment_applied;
    values.write_fraction_register = family != NmiFamily::family_120;
    *out = values;
    return true;
}

bool make_nmi_family_settings(NmiFamily family, std::uint64_t rf_frequency_hz,
                              bool include_family131_register_2e, NmiFamilySettings *out)
{
    if (out == nullptr || !known_family(family)) return false;

    NmiFamilySettings settings{
        .family = family,
        .values = {},
        .count = 0,
        .complete = family == NmiFamily::family_131 || family == NmiFamily::extended,
    };
    // Family 120/130の条件付きレジスタ値は仕様にないため、共通値だけを返す。
    if (settings.complete) {
        append_value(&settings, 0x0e, 0x45);
        append_value(&settings, 0x25, 0xfa);
        if (family == NmiFamily::family_131 && include_family131_register_2e) {
            append_value(&settings, 0x2e, 0x56);
        }
        append_value(&settings, 0x26, 0x82);

        const std::uint32_t gain_value = rf_frequency_hz >= 786000000
                                             ? 0x3f
                                             : rf_frequency_hz >= 762000000 ? 0x5f : 0x7f;
        append_value(&settings, 0x27, gain_value);
        append_value(&settings, 0x29, gain_value);

        const std::uint32_t range_value = rf_frequency_hz >= 818000000 ? 0xaf : 0xdf;
        append_value(&settings, 0x30, range_value);
        append_value(&settings, 0x32, range_value);
        append_value(&settings, 0x36, rf_frequency_hz < 155000000 ? 0x54 : 0x7c);
        append_value(&settings, 0x34, 0x78);
        append_value(&settings, 0x35, 0x54);
    }
    append_value(&settings, 0x37,
                 rf_frequency_hz >= 155000000 && rf_frequency_hz < 300000000 ? 0x9c : 0x84);

    *out = settings;
    return true;
}

bool calculate_nmi_selection_values(const NmiSynthesizerValues &synthesizer,
                                    NmiSelectionValues *out)
{
    if (out == nullptr || !known_family(synthesizer.family)) return false;

    const auto working_rf_hz = synthesizer.programmed_rf_hz;
    NmiSelectionValues values{
        .family_values = {},
        .family_value_count = 0,
        .cutoff_row_index = 8,
        .cutoff_registers = {},
    };

    // 選局時のFamily 120/130値は、仕様のworking RF条件に従う。
    if (synthesizer.family == NmiFamily::family_120) {
        const bool below_300_mhz = working_rf_hz < 300000000;
        append_selection_value(&values, 0x25, below_300_mhz ? 0x78 : 0xf4);
        append_selection_value(&values, 0x27, below_300_mhz ? 0x7f : 0xef);
        append_selection_value(&values, 0x29, below_300_mhz ? 0x7f : 0x4f);
        append_selection_value(&values, 0x2e, below_300_mhz ? 0x12 : 0x34);
        append_selection_value(&values, 0x36, working_rf_hz < 155000000 ? 0x54 : 0x7c);
    } else if (synthesizer.family == NmiFamily::family_130) {
        constexpr std::uint64_t register_0e_special_frequencies_hz[] = {
            147000000, 171000000, 195000000, 219000000, 243000000,
        };
        constexpr std::uint64_t registers_25_2e_special_frequencies_hz[] = {
            115000000, 123000000,
        };
        const auto register_0e_value =
            contains_frequency(register_0e_special_frequencies_hz, 5, working_rf_hz)
                ? 0x45
                : 0x25;
        const bool lower_register_values = contains_frequency(
            registers_25_2e_special_frequencies_hz, 2, working_rf_hz);
        const auto register_30_32_value = working_rf_hz < 139000000 ? 0xdf : 0x01;
        append_selection_value(&values, 0x0e, register_0e_value);
        append_selection_value(&values, 0x25, lower_register_values ? 0x43 : 0x56);
        append_selection_value(&values, 0x2e, lower_register_values ? 0x78 : 0x56);
        append_selection_value(&values, 0x30, register_30_32_value);
        append_selection_value(&values, 0x32, register_30_32_value);
    }

    // cutoffはT[i] < working RF <= T[i+1]で選び、不一致時は末尾行を使う。
    for (std::uint8_t i = 0; i + 1 < 9; ++i) {
        if (nmi_cutoff_settings[i].threshold_hz < working_rf_hz &&
            working_rf_hz <= nmi_cutoff_settings[i + 1].threshold_hz) {
            values.cutoff_row_index = i;
            break;
        }
    }
    const auto &cutoff = nmi_cutoff_settings[values.cutoff_row_index];
    values.cutoff_registers[0] = {0x08, cutoff.value_a};
    values.cutoff_registers[1] = {0x09, cutoff.value_b};

    *out = values;
    return true;
}

bool calculate_nmi_demodulator_values(NmiFamily family,
                                      const NmiSynthesizerValues &synthesizer,
                                      NmiDemodulatorValues *out)
{
    if (out == nullptr || !known_family(family)) return false;

    const auto rfword = synthesizer.clock_hz / 6750 + 16384;
    if (rfword == 0) return false;
    const auto register_230_value = ((126217ULL << 15) / rfword) | 0x80000;
    const auto ifword_denominator = (synthesizer.clock_hz >> 14) + 6750;
    if (ifword_denominator == 0) return false;
    const auto ifword = 2 * (262144000ULL / ifword_denominator);
    const auto clock_shifted = synthesizer.clock_hz >> 5;
    if (clock_shifted > std::numeric_limits<std::uint64_t>::max() / 864) return false;
    const auto sample_product = 864 * clock_shifted;
    if (sample_product < 0x4e058000ULL) return false;
    const auto sample_numerator = sample_product - 0x4e058000ULL;
    const auto sample_value = ((sample_numerator / 216000) & 0xfe00fff3) | 3;

    const std::uint8_t clock_selector = synthesizer.clock_hz > 3686396
                                            ? 2
                                            : synthesizer.clock_hz > 1843192 ? 1 : 0;
    const std::uint32_t clock_family_bits =
        family == NmiFamily::family_120 || family == NmiFamily::family_130
            ? 0x00a00000
            : 0x02600000;
    NmiDemodulatorValues values{
        .write_register_1c0 = family == NmiFamily::family_131,
        .register_1c0_value = 0x2d8c19c7,
        .register_164_value = static_cast<std::uint32_t>(
            family == NmiFamily::family_130
                ? 0x300
                : synthesizer.requested_rf_hz < 300000000 ? 0x600 : 0x500),
        .fixed_registers = {
            {0x130, 0x200}, {0x20c, 0x400}, {0x250, 0}, {0x27c, 0x1010},
            {0x1bc, 0}, {0x1c8, 0x8079}, {0x1cc, 0x8079}, {0x1d0, 0x8079},
            {0x110, 0x4c52}, {0x26c, 2}, {0x270, 6},
            {0x138, 0x5b18}, {0x148, 0x1ff082},
        },
        .rfword = static_cast<std::uint32_t>(rfword),
        .register_230_value = static_cast<std::uint32_t>(register_230_value),
        .ifword = static_cast<std::uint32_t>(ifword),
        .sample_value = static_cast<std::uint32_t>(sample_value),
        .clock_selector = clock_selector,
        .clock_family_bits = clock_family_bits,
        .updates = {
            {0x234, 0xf1ffffff, 0x08000000},
            {0x238, 0xffffc000, 0x00000000},
            {0x32c, 0xbfffbfff, 0x00000000},
            {0x234, 0xcfc00000, 0x00000000},
            {0x12c, 0xffff9f7f, 0x00001000},
            {0x200, 0xffffc000, 0x00008800},
        },
        .register_104_masks = {0x97ffffd1, 0xffff87ff, 0xffff7fff},
    };
    *out = values;
    return true;
}

GroundLock classify_ground_lock(std::uint32_t register_80_value,
                                std::uint32_t register_b0_value)
{
    if ((register_80_value & 0x08) != 0 || (register_b0_value & 0x0f) >= 8) {
        return GroundLock::locked;
    }
    return GroundLock::unlocked;
}

}
