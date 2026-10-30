// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_PRODUCT_CORE_FAMILY_H
#define TORNE_PRODUCT_CORE_FAMILY_H

#include "product/device.h"

#include <dtv/userland/host_daemon/card_service.h>
#include <dtv/userland/host_runtime/family.h>
#include <px4/tuner_service.h>

#include <memory>

namespace torne::product {

// One-receiver ISDB-T family over the product Device. Card and satellite
// operations return UNSUPPORTED until their own increments exist.
std::unique_ptr<dtv::userland::host_runtime::EnclosureFamily> make_family(
    std::unique_ptr<Device> device) noexcept;

}  // namespace torne::product

#endif  // TORNE_PRODUCT_CORE_FAMILY_H
