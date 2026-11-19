/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U3_TUNE_HPP
#define ASICEN_PX_W3U3_TUNE_HPP

#include <cstdint>

namespace asicen::px_w3u3::detail {

struct DemodulatorWrite {
    std::uint8_t address;
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

struct GroundTuneValues {
    std::uint64_t input_frequency_khz;
    std::uint64_t center_frequency_khz;
    RfTunerRegisters tuner;
};

enum class GroundLock : std::uint8_t { unlocked, locked };

extern const DemodulatorWrite ground_tune_prepare[2];
extern const DemodulatorWrite ground_tune_finish[3];

bool calculate_ground_tune(std::uint64_t frequency_hz, GroundTuneValues *out);
std::uint8_t ground_agc_value(std::uint8_t current, std::uint64_t center_frequency_khz);
GroundLock classify_ground_lock(std::uint8_t register_value);

}

#endif
