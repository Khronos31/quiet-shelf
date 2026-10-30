// SPDX-License-Identifier: GPL-2.0-only
#include "product/clock_gate.h"

#include <array>
#include <cstdint>

namespace torne::product {
namespace {

struct RegisterWrite final {
    std::uint32_t address;
    std::uint32_t value;
};

constexpr std::array<RegisterWrite, 6U> kClockGateWrites{{
    {0x0F1010CU, 0x82010000U},
    {0x0F10114U, 0x80080000U},
    {0x0F10118U, 0xFFFF0000U},
    {0x0F1011CU, 0x00060000U},
    {0x0F10128U, 0x60000000U},
    {0x0F1012CU, 0x40070000U},
}};

}  // namespace

bool clock_gate_init(widen::Ep0Io& io) noexcept
{
    for (const RegisterWrite& write : kClockGateWrites) {
        if (!io.write32(write.address, write.value, false).ok) return false;
    }
    return true;
}

}  // namespace torne::product
