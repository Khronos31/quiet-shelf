/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_GROUP_HPP
#define ASICEN_GROUP_HPP

#include <cstdint>

#include <dtv/spi/types.h>

namespace asicen::detail {

inline bool has_runtime_identity(const dtv_usb_candidate_info &candidate, std::uint16_t product_id)
{
    return candidate.vendor_id == 0x0b06 && candidate.product_id == product_id;
}

inline bool same_parent_path(const dtv_usb_candidate_info &left,
                             const dtv_usb_candidate_info &right)
{
    if (left.bus_number != right.bus_number || left.port_path == nullptr ||
        right.port_path == nullptr || left.port_path_length == 0 ||
        left.port_path_length != right.port_path_length) {
        return false;
    }
    for (std::uint32_t i = 0; i + 1 < left.port_path_length; ++i) {
        if (left.port_path[i] != right.port_path[i]) return false;
    }
    return true;
}

inline bool has_interface_zero_alt_zero(const dtv_usb_candidate_info &candidate)
{
    if (candidate.interfaces == nullptr && candidate.interface_count != 0) return false;
    std::uint32_t matches = 0;
    for (std::uint32_t i = 0; i < candidate.interface_count; ++i) {
        const auto &interface = candidate.interfaces[i];
        if (interface.interface_number == 0 && interface.alternate_setting == 0) ++matches;
    }
    return matches == 1;
}

inline dtv_internal_result select_runtime_pair(
    const dtv_usb_candidate_info *candidates, std::uint32_t candidate_count,
    std::uint32_t anchor_candidate_index, dtv_usb_function_selection *selections,
    std::uint32_t selection_capacity, std::uint32_t *selection_count,
    dtv_usb_group_match *match, std::uint16_t product_id)
{
    if (selection_count == nullptr || match == nullptr ||
        (candidates == nullptr && candidate_count != 0) ||
        anchor_candidate_index >= candidate_count ||
        (selections == nullptr && selection_capacity != 0)) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }

    *selection_count = 0;
    *match = DTV_USB_GROUP_NO_MATCH;
    const auto &anchor = candidates[anchor_candidate_index];
    if (!has_runtime_identity(anchor, product_id) || anchor.port_path == nullptr ||
        anchor.port_path_length == 0) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }

    const auto anchor_port = anchor.port_path[anchor.port_path_length - 1];
    if (anchor_port != 1 && anchor_port != 2) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }

    std::uint32_t members[3]{};
    std::uint32_t member_count = 0;
    for (std::uint32_t i = 0; i < candidate_count; ++i) {
        if (has_runtime_identity(candidates[i], product_id) &&
            same_parent_path(anchor, candidates[i])) {
            if (member_count < 3) members[member_count] = i;
            ++member_count;
        }
    }

    if (member_count > 2) {
        *match = DTV_USB_GROUP_AMBIGUOUS;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }

    bool seen_port_one = false;
    bool seen_port_two = false;
    bool valid_pair = member_count == 2;
    bool distinct_addresses = member_count == 2;
    for (std::uint32_t i = 0; i < member_count; ++i) {
        const auto &candidate = candidates[members[i]];
        const auto port = candidate.port_path[candidate.port_path_length - 1];
        if (port == 1 && !seen_port_one) {
            seen_port_one = true;
        } else if (port == 2 && !seen_port_two) {
            seen_port_two = true;
        } else {
            valid_pair = false;
        }
        if (member_count == 2 && i != 0 &&
            candidate.device_address == candidates[members[0]].device_address) {
            distinct_addresses = false;
        }
    }
    const bool paired_ports = valid_pair && seen_port_one && seen_port_two;
    valid_pair = paired_ports && distinct_addresses;

    for (std::uint32_t i = 0; i < member_count; ++i) {
        const auto candidate_index = paired_ports ? members[i] : anchor_candidate_index;
        if (i != 0 && !paired_ports) break;
        const auto &candidate = candidates[candidate_index];
        if (!has_interface_zero_alt_zero(candidate)) continue;
        if (*selection_count >= selection_capacity) {
            *selection_count = 0;
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        }
        const auto port = candidate.port_path[candidate.port_path_length - 1];
        selections[*selection_count] = {
            candidate_index, port == 1 ? 0u : 1u, 0, 0};
        ++*selection_count;
    }

    if (*selection_count == 0) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }

    *match = valid_pair && *selection_count == 2 ? DTV_USB_GROUP_COMPLETE
                                                  : DTV_USB_GROUP_INCOMPLETE;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

}

#endif
