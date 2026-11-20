/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix_dt295_internal.hpp"

namespace dtv::pix::detail {

std::optional<TerrestrialTuneParameters> terrestrial_tune_parameters(uint64_t frequency_hz)
{
    if (frequency_hz == 0 || frequency_hz > UINT64_MAX / 64) return std::nullopt;
    // L1から届いたHzは保ち、機器のPLL分周だけ仕様の整数切捨てで求める。
    const auto divider = (frequency_hz * 64) / 1000000;
    if (divider > UINT16_MAX) return std::nullopt;
    return TerrestrialTuneParameters{
        frequency_hz, static_cast<uint16_t>(divider), frequency_hz >= 700000000};
}

TerrestrialLockState terrestrial_lock_state(uint8_t reg_80, uint8_t reg_b0)
{
    if ((reg_80 & 0x28) == 0 && (reg_b0 & 0x0F) >= 8)
        return TerrestrialLockState::locked;
    if ((reg_80 & 0x80) != 0) return TerrestrialLockState::unlocked;
    return TerrestrialLockState::unknown;
}

std::optional<uint32_t> terrestrial_raw_cn(std::span<const uint8_t> bytes)
{
    if (bytes.size() != 3) return std::nullopt;
    // raw counterを保持し、校正済みC/N値へ換算しない。
    return (static_cast<uint32_t>(bytes[0]) << 16) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           static_cast<uint32_t>(bytes[2]);
}

}
