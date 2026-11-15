/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_DISCOVERY_HPP
#define DTV_INTERNAL_DISCOVERY_HPP

#include <span>
#include <vector>

#include <dtv/internal/usb.hpp>
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

namespace dtv::internal {

struct BootGroupCandidate {
    const dtv::spi::ProductEntry *product;
    const dtv::spi::ModelEntry *model;
    std::uint32_t anchor_candidate_index;
    dtv_usb_group_match match;
    std::vector<dtv_usb_function_selection> functions;
    std::vector<UsbDeviceSnapshot> snapshots;
};

struct LoadedProductDiscoveryView {
    const dtv::spi::ProductEntry *product;
    const dtv::spi::l3_model_record *const *models;
    std::size_t model_count;
};

struct CompleteGroupObservation {
    const dtv::spi::ProductEntry *product;
    const dtv::spi::l3_model_record *model;
    std::uint32_t anchor_candidate_index;
    std::vector<dtv_usb_function_selection> functions;
};

struct UngroupedObservation {
    std::uint32_t snapshot_index;
    std::uint64_t connection_token;
    const char *model;
};

struct CompleteGroupBatch {
    std::vector<UsbDeviceSnapshot> snapshots;
    std::vector<CompleteGroupObservation> groups;
    std::vector<UngroupedObservation> ungrouped;
};

dtv_internal_result check_operation_requirements(
    const BootGroupCandidate &, dtv_spi_receiver_id receiver, std::uint32_t operation);

std::vector<BootGroupCandidate> discover_boot_candidates(
    std::span<const UsbDeviceSnapshot>,
    std::span<const dtv::spi::ProductEntry *const>,
    dtv_internal_result *result = nullptr);

dtv_internal_result collect_complete_groups(
    std::span<const LoadedProductDiscoveryView> products,
    std::vector<UsbDeviceSnapshot> snapshots,
    CompleteGroupBatch *out);

dtv_internal_result observe_loaded_product_groups(
    std::vector<UsbDeviceSnapshot> snapshots,
    CompleteGroupBatch *out);

dtv_internal_result scan_loaded_product_groups(CompleteGroupBatch *out);
dtv_internal_result scan_loaded_product_groups(const UsbContext &, CompleteGroupBatch *out);
dtv_internal_result scan_loaded_product_groups(
    const UsbContext &, CompleteGroupBatch *, dtv_token_of_callback, void *);

}

#endif
