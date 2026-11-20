/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_w3u2_tune.hpp"

#include "ground_tune_common.hpp"

namespace asicen::px_w3u2::detail {

namespace ground_tune = ::asicen::detail::ground_tune;

const DemodulatorWrite ground_tune_prepare[2] = {
    {0x30, ground_tune::prepare[0].reg, ground_tune::prepare[0].value},
    {0x30, ground_tune::prepare[1].reg, ground_tune::prepare[1].value},
};

const DemodulatorWrite ground_tune_finish[3] = {
    {0x30, ground_tune::finish[0].reg, ground_tune::finish[0].value},
    {0x30, ground_tune::finish[1].reg, ground_tune::finish[1].value},
    {0x30, ground_tune::finish[2].reg, ground_tune::finish[2].value},
};

bool calculate_ground_tune(std::uint64_t frequency_khz, GroundTuneValues *out)
{
    if (out == nullptr) return false;

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

std::uint8_t ground_agc_value(std::uint8_t current, std::uint64_t center_frequency_khz)
{
    return ground_tune::agc_value(current, center_frequency_khz);
}

RetryResetValues ground_retry_reset_values(std::uint8_t current)
{
    const auto asserted = static_cast<std::uint8_t>(current | 0x30);
    return {asserted, static_cast<std::uint8_t>(asserted & 0xef)};
}

TunerReady classify_tuner_ready(std::uint8_t register_value)
{
    return (register_value & 0x40) != 0 ? TunerReady::ready : TunerReady::not_ready;
}

GroundLock classify_ground_lock(std::uint8_t register_value)
{
    return ground_tune::classify_lock(register_value) == ground_tune::Lock::locked
               ? GroundLock::locked
               : GroundLock::unlocked;
}

}
