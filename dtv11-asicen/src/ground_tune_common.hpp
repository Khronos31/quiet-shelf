/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_GROUND_TUNE_COMMON_HPP
#define ASICEN_GROUND_TUNE_COMMON_HPP

#include <cstdint>

namespace asicen::detail::ground_tune {

struct RegisterValue {
    std::uint8_t reg;
    std::uint8_t value;
};

struct RfTunerRegisters {
    std::uint8_t reg1;
    std::uint8_t reg2;
    std::uint8_t reg3;
    std::uint8_t reg4;
    std::uint8_t reg5;
    std::uint8_t reg6;
    bool vco_selected;
};

struct TuneValues {
    std::uint64_t input_frequency_khz;
    std::uint64_t center_frequency_khz;
    RfTunerRegisters tuner;
};

enum class Lock : std::uint8_t { unlocked, locked };

inline constexpr RegisterValue prepare[2] = {
    {0x25, 0x00},
    {0x23, 0x4d},
};

inline constexpr RegisterValue finish[3] = {
    {0x0f, 0x34},
    {0x01, 0x40},
    {0x23, 0x4c},
};

bool calculate_khz(std::uint64_t frequency_khz, TuneValues *out);
std::uint8_t agc_value(std::uint8_t current, std::uint64_t center_frequency_khz);
Lock classify_lock(std::uint8_t register_value);

}

#endif
