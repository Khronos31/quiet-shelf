/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_s3u_tune.hpp"

#include "ground_tune_common.hpp"

namespace asicen::px_s3u::detail {

namespace ground_tune = ::asicen::detail::ground_tune;

const DemodulatorWrite ground_tune_prepare[2] = {
    {0x30, 0x25, 0x00},
    {0x30, 0x23, 0x4d},
};

const DemodulatorWrite ground_tune_finish[3] = {
    {0x30, 0x0f, 0x14},
    {0x30, 0x01, 0x40},
    {0x30, 0x23, 0x4c},
};

bool calculate_ground_tune_khz(std::uint64_t frequency_khz, GroundTuneValues *out)
{
    if (out == nullptr || frequency_khz == 0 || frequency_khz > 999999) return false;

    ground_tune::TuneValues values{};
    if (!ground_tune::calculate_khz(frequency_khz, &values)) return false;

    *out = {
        .input_frequency_khz = values.input_frequency_khz,
        .center_frequency_khz = values.center_frequency_khz,
        .tuner = {
            .reg1 = values.tuner.reg1,
            .reg2 = values.tuner.reg2,
            .reg3 = values.tuner.reg3,
            .reg4 = values.tuner.reg4,
            .reg5 = values.tuner.reg5,
            .reg6 = values.tuner.reg6,
            .vco_selected = values.tuner.vco_selected,
        },
    };
    return true;
}

bool ground_rf_band_write(std::uint64_t center_frequency_khz, DemodulatorWrite *out)
{
    if (out == nullptr) return false;

    if (center_frequency_khz >= 93144 && center_frequency_khz <= 261142) {
        *out = {0x32, 0x14, 0x00};
        return true;
    }
    if (center_frequency_khz >= 255144 && center_frequency_khz <= 767142) {
        *out = {0x32, 0x14, 0x20};
        return true;
    }
    return false;
}

bool calculate_calibrated_reg6(std::uint8_t current_reg6, std::uint8_t calibration_value,
                               std::uint8_t *out)
{
    if (out == nullptr) return false;

    // S3Uの非選択VCO条件だけを反映し、レジスタ書込みは呼び出し側に委ねる。
    *out = current_reg6;
    if ((calibration_value & 0x3f) < 0x02) *out |= 0x08;
    return true;
}

}
