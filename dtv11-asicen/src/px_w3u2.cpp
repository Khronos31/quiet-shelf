/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstdint>

#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

#include "asicen_group.hpp"

namespace {

using receiver_information = dtv::spi::model_information::receiver_information;
using device_receiver_information = dtv::spi::model_information::device_receiver_information;

constexpr receiver_information primary_receivers[] = {
    {0, 0, DTV_SYSTEM_ISDB_S, 188, 0, 0, 0},
    {1, 1, DTV_SYSTEM_ISDB_T, 188, 0, 0, 0},
};

constexpr receiver_information secondary_receivers[] = {
    {2, 0, DTV_SYSTEM_ISDB_S, 188, 0, 0, 0},
    {3, 1, DTV_SYSTEM_ISDB_T, 188, 0, 0, 0},
};

constexpr device_receiver_information runtime_devices[] = {
    {0, true, primary_receivers, 2},
    {1, true, secondary_receivers, 2},
};

dtv_internal_result select_group(
    const dtv_usb_candidate_info *candidates, std::uint32_t candidate_count,
    std::uint32_t anchor_candidate_index, dtv_usb_function_selection *selections,
    std::uint32_t selection_capacity, std::uint32_t *selection_count,
    dtv_usb_group_match *match)
{
    return asicen::detail::select_runtime_pair(
        candidates, candidate_count, anchor_candidate_index, selections,
        selection_capacity, selection_count, match, 0x0004);
}

}

DTV_PRODUCT(asicen, DTV_FEATURES());
DTV_MODEL(asicen, px_w3u2, DTV_FEATURES(), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"PX-W3U2", 0x0b06, 0x0004,
                                         runtime_devices, 2}));
DTV_DEFINE_MODEL(asicen, px_w3u2);

namespace dtv::spi {

extern const ModelEntry px_w3u2_model{
    .model = {"PX-W3U2", 0x0b06, 0x0004},
    .firmware = nullptr,
    .firmware_count = 0,
    .select_usb_group = select_group,
    .source_stride_bytes = 188,
    .source_sync_offset_bytes = 0,
    .runtime_device_receivers = runtime_devices,
    .runtime_device_receiver_count = 2,
};

}
