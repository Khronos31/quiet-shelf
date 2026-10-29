// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <dtv/userland/error.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace torne {

// Physical observation only; a missing serial is never synthesized.
// The CECH-ZD1 J reports iSerialNumber=0 in the observed hardware.
struct UsbObservation {
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    std::string port; // Core grammar: "bus-port[.port...]"
};

// Uses dtv::tuning::resolve rather than duplicating the channel table.
// The hardware accepts only ISDB-T and tunes at a native Hz value.
dtv::userland::Result<std::uint32_t> resolve_terrestrial_hz(
    std::string_view channel) noexcept;

// Uses the core discovery JSON projector; no USB I/O or device opens.
// Model and reader capability describe hardware, not current card presence.
// Returns an error if an observed USB path violates the frozen Core grammar.
dtv::userland::Result<std::string> discovery_json(
    const std::vector<UsbObservation>& devices,
    bool complete,
    std::string_view version = "0.1.0-dev");

} // namespace torne
