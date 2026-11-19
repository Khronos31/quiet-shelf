/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U2_TUNE_HPP
#define ASICEN_PX_W3U2_TUNE_HPP

#include <cstdint>

namespace asicen::px_w3u2::detail {

struct DemodulatorWrite {
    std::uint8_t address;
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

struct GroundTuneValues {
    std::uint64_t input_frequency_khz;
    std::uint64_t center_frequency_khz;
    RfTunerRegisters tuner;
};

struct RetryResetValues {
    std::uint8_t asserted;
    std::uint8_t released;
};

enum class GroundLock : std::uint8_t { unlocked, locked };
enum class TunerReady : std::uint8_t { not_ready, ready };

inline constexpr std::uint8_t ground_tune_max_attempts = 4;
inline constexpr std::uint8_t ground_retry_wait_ms = 10;

extern const DemodulatorWrite ground_tune_prepare[2];
extern const DemodulatorWrite ground_tune_finish[3];

bool calculate_ground_tune(std::uint64_t frequency_khz, GroundTuneValues *out);
std::uint8_t ground_agc_value(std::uint8_t current, std::uint64_t center_frequency_khz);
RetryResetValues ground_retry_reset_values(std::uint8_t current);
TunerReady classify_tuner_ready(std::uint8_t register_value);
GroundLock classify_ground_lock(std::uint8_t register_value);

}

#endif
