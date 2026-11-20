/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_S3U_TUNE_HPP
#define ASICEN_PX_S3U_TUNE_HPP

#include <cstdint>

namespace asicen::px_s3u::detail {

struct DemodulatorWrite {
    std::uint8_t slave_address;
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

extern const DemodulatorWrite ground_tune_prepare[2];
extern const DemodulatorWrite ground_tune_finish[3];

bool calculate_ground_tune_khz(std::uint64_t frequency_khz, GroundTuneValues *out);
bool ground_rf_band_write(std::uint64_t center_frequency_khz, DemodulatorWrite *out);
bool calculate_calibrated_reg6(std::uint8_t current_reg6, std::uint8_t calibration_value,
                               std::uint8_t *out);

}

#endif
