/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_w3u3_tune.hpp"

#include "ground_tune_common.hpp"

namespace asicen::px_w3u3::detail {

namespace ground_tune = ::asicen::detail::ground_tune;

namespace {

std::uint64_t rounded_frequency_khz(std::uint64_t frequency_hz)
{
    const auto whole_khz = frequency_hz / 1000;
    const auto remainder_hz = frequency_hz % 1000;
    return whole_khz + (remainder_hz >= 500 ? 1 : 0);
}

}

const DemodulatorWrite ground_tune_prepare[2] = {
    {ground_tune::prepare[0].reg, ground_tune::prepare[0].value},
    {ground_tune::prepare[1].reg, ground_tune::prepare[1].value},
};

const DemodulatorWrite ground_tune_finish[3] = {
    {ground_tune::finish[0].reg, ground_tune::finish[0].value},
    {ground_tune::finish[1].reg, ground_tune::finish[1].value},
    {ground_tune::finish[2].reg, ground_tune::finish[2].value},
};

bool calculate_ground_tune(std::uint64_t frequency_hz, GroundTuneValues *out)
{
    if (out == nullptr) return false;

    const auto input_frequency_khz = rounded_frequency_khz(frequency_hz);
    ground_tune::TuneValues values{};
    if (!ground_tune::calculate_khz(input_frequency_khz, &values)) return false;
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

std::uint8_t ground_agc_value(std::uint8_t current, std::uint64_t center_frequency_khz)
{
    return ground_tune::agc_value(current, center_frequency_khz);
}

GroundLock classify_ground_lock(std::uint8_t register_value)
{
    return ground_tune::classify_lock(register_value) == ground_tune::Lock::locked
               ? GroundLock::locked
               : GroundLock::unlocked;
}

}
