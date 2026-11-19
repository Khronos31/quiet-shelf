/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U3_V2_TUNE_HPP
#define ASICEN_PX_W3U3_V2_TUNE_HPP

#include <cstdint>

namespace asicen::px_w3u3_v2::detail {

enum class NmiFamily : std::uint8_t { family_120, family_130, family_131, extended };

struct GroundFrequency {
    std::uint64_t input_frequency_khz;
    std::uint64_t rf_frequency_hz;
};

struct NmiRegisterWrite {
    std::uint16_t address;
    std::uint32_t value;
};

struct NmiRegisterUpdate {
    std::uint16_t address;
    std::uint32_t preserve_mask;
    std::uint32_t set_bits;
};

struct NmiCutoffSetting {
    std::uint32_t threshold_hz;
    std::uint8_t value_a;
    std::uint8_t value_b;
};

struct NmiSynthesizerValues {
    NmiFamily family;
    std::uint64_t requested_rf_hz;
    std::uint64_t programmed_rf_hz;
    std::uint64_t xo_hz;
    std::uint64_t vco_hz;
    std::uint64_t clock_hz;
    std::uint32_t integer;
    std::uint32_t fraction;
    std::uint32_t band_multiplier;
    std::uint8_t band_index;
    std::uint8_t reference_control_bits;
    std::uint8_t divider;
    NmiRegisterWrite pll_registers[4];
    NmiRegisterUpdate band_register_update;
    bool write_fraction_register;
    std::uint8_t fraction_register_value;
    bool fraction_adjustment_applied;
};

// family固有の設定値集合であり、書込み順序をここでは定義しない。
struct NmiFamilySettings {
    NmiFamily family;
    NmiRegisterWrite values[12];
    std::uint8_t count;
    bool complete;
};

struct NmiSelectionValues {
    NmiRegisterWrite family_values[5];
    std::uint8_t family_value_count;
    std::uint8_t cutoff_row_index;
    NmiRegisterWrite cutoff_registers[2];
};

struct NmiDemodulatorValues {
    bool write_register_1c0;
    std::uint32_t register_1c0_value;
    std::uint32_t register_164_value;
    NmiRegisterWrite fixed_registers[13];
    std::uint32_t rfword;
    std::uint32_t register_230_value;
    std::uint32_t ifword;
    std::uint32_t sample_value;
    std::uint8_t clock_selector;
    std::uint32_t clock_family_bits;
    NmiRegisterUpdate updates[6];
    std::uint32_t register_104_masks[3];
};

enum class GroundLock : std::uint8_t { unlocked, locked };

// 閾値と対応値のみを保持し、周波数から行を選ぶ比較規則は仕様にないため決めない。
inline constexpr NmiCutoffSetting nmi_cutoff_settings[9] = {
    {45000000, 167, 58},
    {55000000, 151, 57},
    {65000000, 100, 54},
    {75000000, 83, 53},
    {85000000, 82, 53},
    {95000000, 65, 52},
    {105000000, 64, 52},
    {115000000, 64, 52},
    {125000000, 0, 0},
};

bool normalize_ground_frequency(std::uint64_t input_frequency_khz, GroundFrequency *out);
std::uint8_t nmi_band_index(std::uint64_t rf_frequency_hz);
std::uint8_t family130_reference_control_bits(std::uint64_t rf_frequency_hz);
std::uint8_t rf_register_05_value(std::uint64_t rf_frequency_hz);
bool calculate_nmi_synthesizer(NmiFamily family, std::uint64_t rf_frequency_hz,
                               std::uint8_t register_21_value, NmiSynthesizerValues *out);
bool make_nmi_family_settings(NmiFamily family, std::uint64_t rf_frequency_hz,
                              bool include_family131_register_2e, NmiFamilySettings *out);
bool calculate_nmi_selection_values(const NmiSynthesizerValues &synthesizer,
                                    NmiSelectionValues *out);
bool calculate_nmi_demodulator_values(NmiFamily family,
                                      const NmiSynthesizerValues &synthesizer,
                                      NmiDemodulatorValues *out);
GroundLock classify_ground_lock(std::uint32_t register_80_value,
                                std::uint32_t register_b0_value);

}

#endif
