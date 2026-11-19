/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_w3u3_satellite.hpp"

#include <limits>

namespace asicen::px_w3u3::detail {

namespace {

struct SatelliteTunerRow {
    std::uint8_t values[8];
};

constexpr SatelliteTunerRow satellite_tuner_rows[24] = {
    {{0x28, 0x29, 0xe0, 0xd2, 0xe4, 0xf4, 0xd6, 0x48}},
    {{0x24, 0x40, 0xe0, 0xe2, 0xe4, 0xf4, 0xe6, 0x44}},
    {{0x24, 0x66, 0xe0, 0xe2, 0xe4, 0xf4, 0xe6, 0x44}},
    {{0x24, 0x8d, 0xe0, 0x20, 0xe4, 0xf4, 0x24, 0x44}},
    {{0x24, 0xb3, 0xe0, 0x20, 0xe4, 0xf4, 0x24, 0x44}},
    {{0x24, 0xd9, 0xe0, 0x20, 0xe4, 0xf4, 0x24, 0x44}},
    {{0x25, 0x00, 0xe0, 0x20, 0xe4, 0xf4, 0x24, 0x45}},
    {{0x25, 0x26, 0xe0, 0x40, 0xe4, 0xf4, 0x44, 0x45}},
    {{0x25, 0x4c, 0xe0, 0x40, 0xe4, 0xf4, 0x44, 0x45}},
    {{0x25, 0x73, 0xe0, 0x40, 0xe4, 0xf4, 0x44, 0x45}},
    {{0x25, 0x99, 0xe0, 0x40, 0xe4, 0xf4, 0x44, 0x45}},
    {{0x25, 0xbf, 0xe0, 0x60, 0xe4, 0xf4, 0x64, 0x45}},
    {{0x26, 0x4d, 0xe0, 0x60, 0xe4, 0xf4, 0x64, 0x46}},
    {{0x26, 0x75, 0xe0, 0x80, 0xe4, 0xf4, 0x84, 0x46}},
    {{0x26, 0x9d, 0xe0, 0x80, 0xe4, 0xf4, 0x84, 0x46}},
    {{0x26, 0xc5, 0xe0, 0x80, 0xe4, 0xf4, 0x84, 0x46}},
    {{0x26, 0xed, 0xe0, 0x80, 0xe4, 0xf4, 0x84, 0x46}},
    {{0x27, 0x15, 0xe0, 0xa0, 0xe4, 0xf4, 0xa4, 0x47}},
    {{0x27, 0x3d, 0xe0, 0xa0, 0xe4, 0xf4, 0xa4, 0x47}},
    {{0x27, 0x65, 0xe0, 0xa0, 0xe4, 0xf4, 0xa4, 0x47}},
    {{0x27, 0x8d, 0xe0, 0xa0, 0xe4, 0xf4, 0xa4, 0x47}},
    {{0x27, 0xb5, 0xe0, 0xc0, 0xe4, 0xf4, 0xc4, 0x47}},
    {{0x27, 0xdd, 0xe0, 0xc0, 0xe4, 0xf4, 0xc4, 0x47}},
    {{0x28, 0x05, 0xe0, 0xc0, 0xe4, 0xf4, 0xc4, 0x48}},
};

std::uint64_t satellite_rf_frequency_khz(std::uint8_t row)
{
    if (row < 12) return 11727480 + static_cast<std::uint64_t>(row) * 38360;
    return 12291000 + static_cast<std::uint64_t>(row - 12) * 40000;
}

bool valid_tsid(std::uint16_t tsid)
{
    return tsid != 0x0000 && tsid != 0xffff;
}

}

const SatelliteDemodulatorWrite satellite_tune_prepare[2] = {
    {0x32, 0x25, 0x00},
    {0x32, 0x23, 0x4d},
};

const SatelliteDemodulatorWrite satellite_tune_finish[3] = {
    {0x30, 0x0f, 0x34},
    {0x32, 0x03, 0x01},
    {0x32, 0x23, 0x4c},
};

bool make_satellite_tune_plan(std::uint64_t if_frequency_khz, SatelliteTunePlan *out,
                              std::uint16_t initial_tsid)
{
    if (out == nullptr || if_frequency_khz >
                              std::numeric_limits<std::uint64_t>::max() - 10678000) {
        return false;
    }

    const auto rf_frequency_khz = if_frequency_khz + 10678000;
    std::uint8_t row = 0xff;
    for (std::uint8_t candidate = 0; candidate < 24; ++candidate) {
        if (rf_frequency_khz == satellite_rf_frequency_khz(candidate)) {
            row = candidate;
            break;
        }
    }
    if (row == 0xff || (row >= 12 && initial_tsid != 0xffff)) return false;

    const auto &values = satellite_tuner_rows[row].values;
    SatelliteTunePlan plan{
        .if_frequency_khz = if_frequency_khz,
        .rf_frequency_khz = rf_frequency_khz,
        .row = row,
        .initial_tsid = initial_tsid,
        .initial_tsid_register = 0x8f,
        .initial_tsid_bytes = {
            static_cast<std::uint8_t>(initial_tsid >> 8),
            static_cast<std::uint8_t>(initial_tsid & 0xff),
        },
        .tuner_messages = {
            {{0xfe, 0xc0, values[0], values[1], values[2], values[3]}, 6, 0},
            {{0xfe, 0xc0, values[4], 0x00, 0x00, 0x00}, 3, 0},
            {{0xfe, 0xc0, values[5], values[6], 0x00, 0x00}, 4, 0},
            {{0xfe, 0xc0, values[7], 0x00, 0x00, 0x00}, 3, 0},
        },
        .wait_after_message_index = 1,
        .wait_after_message_ms = 10,
    };
    *out = plan;
    return true;
}

bool decode_satellite_slots(const std::uint8_t bytes[16], SatelliteSlots *out)
{
    if (bytes == nullptr || out == nullptr) return false;

    SatelliteSlots slots{};
    for (std::uint8_t slot = 0; slot < 8; ++slot) {
        slots.values[slot] = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[slot * 2]) << 8) | bytes[slot * 2 + 1]);
    }
    *out = slots;
    return true;
}

bool choose_satellite_slot(const SatelliteSlots &slots, SatelliteSelection selection,
                           SatelliteSelectionResult *out)
{
    if (out == nullptr) return false;

    if (selection.kind == SatelliteSelectionKind::slot) {
        if (selection.value >= 8 || !valid_tsid(slots.values[selection.value])) return false;
        const auto tsid = slots.values[selection.value];
        *out = {
            static_cast<std::uint8_t>(selection.value),
            tsid,
            0x8f,
            {static_cast<std::uint8_t>(tsid >> 8), static_cast<std::uint8_t>(tsid & 0xff)},
        };
        return true;
    }

    if (selection.kind == SatelliteSelectionKind::tsid) {
        if (!valid_tsid(selection.value)) return false;
        for (std::uint8_t slot = 0; slot < 8; ++slot) {
            if (slots.values[slot] == selection.value) {
                *out = {
                    slot,
                    selection.value,
                    0x8f,
                    {static_cast<std::uint8_t>(selection.value >> 8),
                     static_cast<std::uint8_t>(selection.value & 0xff)},
                };
                return true;
            }
        }
        return false;
    }

    if (selection.kind == SatelliteSelectionKind::first_available) {
        for (std::uint8_t slot = 0; slot < 8; ++slot) {
            if (valid_tsid(slots.values[slot])) {
                const auto tsid = slots.values[slot];
                *out = {
                    slot,
                    tsid,
                    0x8f,
                    {static_cast<std::uint8_t>(tsid >> 8),
                     static_cast<std::uint8_t>(tsid & 0xff)},
                };
                return true;
            }
        }
    }
    return false;
}

SatelliteLock classify_satellite_lock(std::uint8_t register_value)
{
    return (register_value & 0x10) == 0 ? SatelliteLock::locked : SatelliteLock::unlocked;
}

}
