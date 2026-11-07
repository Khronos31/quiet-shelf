/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_DISCOVERY_HPP
#define DTV_INTERNAL_DISCOVERY_HPP

#include <span>
#include <vector>

#include <dtv/internal/usb.hpp>
#include <dtv/spi/device_ops.hpp>

namespace dtv::internal {

struct BootGroupCandidate {
    const dtv::spi::ProductEntry *product;
    const dtv::spi::ModelEntry *model;
    std::uint32_t anchor_candidate_index;
    dtv_usb_group_match match;
    std::vector<dtv_usb_function_selection> functions;
    std::vector<UsbDeviceSnapshot> snapshots;
};

dtv_internal_result check_operation_requirements(
    const BootGroupCandidate &, dtv_spi_receiver_id receiver, std::uint32_t operation);

std::vector<BootGroupCandidate> discover_boot_candidates(
    std::span<const UsbDeviceSnapshot>,
    std::span<const dtv::spi::ProductEntry *const>,
    dtv_internal_result *result = nullptr);

}

#endif
