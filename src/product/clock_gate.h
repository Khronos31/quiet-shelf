// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_PRODUCT_CLOCK_GATE_H
#define TORNE_PRODUCT_CLOCK_GATE_H

#include "widen/bridge.h"

namespace torne::product {

bool clock_gate_init(widen::Ep0Io& io) noexcept;

}  // namespace torne::product

#endif  // TORNE_PRODUCT_CLOCK_GATE_H
