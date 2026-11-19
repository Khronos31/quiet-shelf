/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U3_SATELLITE_HPP
#define ASICEN_PX_W3U3_SATELLITE_HPP

#include <cstdint>

namespace asicen::px_w3u3::detail {

struct SatelliteDemodulatorWrite {
    std::uint8_t address;
    std::uint8_t reg;
    std::uint8_t value;
};

struct SatelliteTunerMessage {
    std::uint8_t data[6];
    std::uint8_t size;
    std::uint8_t stop;
};

struct SatelliteTunePlan {
    std::uint64_t if_frequency_khz;
    std::uint64_t rf_frequency_khz;
    std::uint8_t row;
    std::uint16_t initial_tsid;
    std::uint8_t initial_tsid_register;
    std::uint8_t initial_tsid_bytes[2];
    SatelliteTunerMessage tuner_messages[4];
    std::uint8_t wait_after_message_index;
    std::uint16_t wait_after_message_ms;
};

struct SatelliteSlots {
    std::uint16_t values[8];
};

enum class SatelliteSelectionKind : std::uint8_t { slot, tsid, first_available };

struct SatelliteSelection {
    SatelliteSelectionKind kind;
    std::uint16_t value;
};

struct SatelliteSelectionResult {
    std::uint8_t slot;
    std::uint16_t tsid;
    std::uint8_t tsid_register;
    std::uint8_t tsid_bytes[2];
};

enum class SatelliteLock : std::uint8_t { unlocked, locked };

extern const SatelliteDemodulatorWrite satellite_tune_prepare[2];
extern const SatelliteDemodulatorWrite satellite_tune_finish[3];

bool make_satellite_tune_plan(std::uint64_t if_frequency_khz, SatelliteTunePlan *out,
                              std::uint16_t initial_tsid = 0xffff);
bool decode_satellite_slots(const std::uint8_t bytes[16], SatelliteSlots *out);
bool choose_satellite_slot(const SatelliteSlots &slots, SatelliteSelection selection,
                           SatelliteSelectionResult *out);
SatelliteLock classify_satellite_lock(std::uint8_t register_value);

}

#endif
