// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/discovery.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <tuple>

#include <dtv/internal/result.hpp>

namespace dtv::internal {
namespace {

bool identity_matches(const dtv::spi::ModelEntry &model,
                      const dtv_usb_candidate_info &candidate)
{
    for (std::size_t i = 0; i < model.cold_identity_count; ++i) {
        if (model.cold_identities == nullptr) {
            return false;
        }
        if (model.cold_identities[i].vendor_id == candidate.vendor_id &&
            model.cold_identities[i].product_id == candidate.product_id) {
            return true;
        }
    }
    return false;
}

bool same_functions(const BootGroupCandidate &left,
                    const std::vector<dtv_usb_function_selection> &right)
{
    if (left.functions.size() != right.size()) {
        return false;
    }
    auto key = [](const dtv_usb_function_selection &entry) {
        return std::tuple(entry.candidate_index, entry.function, entry.interface_number,
                          entry.alternate_setting);
    };
    std::vector<decltype(key(right.front()))> left_keys;
    std::vector<decltype(key(right.front()))> right_keys;
    for (const auto &entry : left.functions) left_keys.push_back(key(entry));
    for (const auto &entry : right) right_keys.push_back(key(entry));
    std::sort(left_keys.begin(), left_keys.end());
    std::sort(right_keys.begin(), right_keys.end());
    return left_keys == right_keys;
}

dtv_internal_result invalid()
{
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
}

}

std::vector<BootGroupCandidate> discover_boot_candidates(
    std::span<const UsbDeviceSnapshot> snapshots,
    std::span<const dtv::spi::ProductEntry *const> products,
    dtv_internal_result *out_result)
{
    std::vector<BootGroupCandidate> found;
    if (snapshots.size() > std::numeric_limits<std::uint32_t>::max()) {
        if (out_result) *out_result = invalid();
        return {};
    }
    std::vector<dtv_usb_candidate_info> candidates(snapshots.size());
    std::size_t capacity_size = 0;
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const auto &snapshot = snapshots[i];
        auto &candidate = candidates[i];
        candidate.candidate_id = snapshot.candidate_id;
        candidate.vendor_id = snapshot.vendor_id;
        candidate.product_id = snapshot.product_id;
        candidate.device_version = snapshot.device_version;
        candidate.bus_number = snapshot.bus_number;
        candidate.device_address = snapshot.device_address;
        candidate.port_path = snapshot.port_path.data();
        candidate.port_path_length = static_cast<std::uint32_t>(snapshot.port_path.size());
        if (snapshot.interfaces.size() > std::numeric_limits<std::uint32_t>::max() ||
            snapshot.port_path.size() > std::numeric_limits<std::uint32_t>::max()) {
            if (out_result) *out_result = invalid();
            return {};
        }
        candidate.interface_count = static_cast<std::uint32_t>(snapshot.interfaces.size());
        capacity_size += snapshot.interfaces.size();
    }
    std::vector<std::unique_ptr<dtv_usb_interface_info[]>> interface_views;
    interface_views.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto count = candidates[i].interface_count;
        if (count == 0) continue;
        auto view = std::make_unique<dtv_usb_interface_info[]>(count);
        for (std::uint32_t j = 0; j < count; ++j) {
            if (snapshots[i].interfaces[j].endpoints.size() >
                std::numeric_limits<std::uint32_t>::max()) {
                if (out_result) *out_result = invalid();
                return {};
            }
            view[j] = snapshots[i].interfaces[j].info;
            view[j].endpoints = snapshots[i].interfaces[j].endpoints.data();
            view[j].endpoint_count = static_cast<std::uint32_t>(snapshots[i].interfaces[j].endpoints.size());
        }
        candidates[i].interfaces = view.get();
        interface_views.push_back(std::move(view));
    }
    if (capacity_size > std::numeric_limits<std::uint32_t>::max()) {
        if (out_result) *out_result = invalid();
        return {};
    }
    const auto capacity = static_cast<std::uint32_t>(capacity_size);
    std::set<std::string> product_ids;
    for (const auto *product : products) {
        if (product == nullptr || product->product_id == nullptr || product->product_id[0] == '\0' ||
            (product->model_count != 0 && product->models == nullptr) ||
            !product_ids.insert(product->product_id).second) {
            if (out_result) *out_result = invalid();
            return {};
        }
        for (std::size_t m = 0; m < product->model_count; ++m) {
            const auto *model = product->models[m];
            if (model == nullptr || !dtv::spi::valid_source_metadata(*model) ||
                (model->cold_identity_count > 0 && model->cold_identities == nullptr)) {
                if (out_result) *out_result = invalid();
                return {};
            }
            if (model->select_usb_group == nullptr) continue;
            for (std::uint32_t anchor = 0; anchor < candidates.size(); ++anchor) {
                if (!identity_matches(*model, candidates[anchor])) continue;
                std::vector<dtv_usb_function_selection> selected(capacity);
                std::uint32_t count = 0;
                dtv_usb_group_match match = DTV_USB_GROUP_NO_MATCH;
                const auto callback_result = dtv::internal::call_callback([&] {
                    return model->select_usb_group(candidates.data(),
                        static_cast<std::uint32_t>(candidates.size()), anchor,
                        selected.data(), capacity, &count, &match);
                });
                if (callback_result.code != DTV_INTERNAL_RESULT_OK) {
                    if (out_result) *out_result = callback_result;
                    return {};
                }
                if (count > capacity || match < DTV_USB_GROUP_NO_MATCH ||
                    match > DTV_USB_GROUP_INCOMPLETE ||
                    ((match != DTV_USB_GROUP_COMPLETE && match != DTV_USB_GROUP_INCOMPLETE) &&
                     count != 0) ||
                    ((match == DTV_USB_GROUP_COMPLETE || match == DTV_USB_GROUP_INCOMPLETE) &&
                     count == 0)) {
                    if (out_result) *out_result = invalid();
                    return {};
                }
                if (match == DTV_USB_GROUP_NO_MATCH) continue;
                selected.resize(count);
                std::set<dtv_function> functions;
                std::set<std::pair<std::uint32_t, std::uint8_t>> interfaces;
                if (match == DTV_USB_GROUP_COMPLETE || match == DTV_USB_GROUP_INCOMPLETE) {
                    for (const auto &entry : selected) {
                        if (entry.candidate_index >= candidates.size() ||
                            !functions.insert(entry.function).second ||
                            !interfaces.emplace(entry.candidate_index, entry.interface_number).second) {
                            if (out_result) *out_result = invalid();
                            return {};
                        }
                        const auto &candidate = candidates[entry.candidate_index];
                        std::uint32_t matches = 0;
                        for (std::uint32_t j = 0; j < candidate.interface_count; ++j) {
                            const auto &interface = candidate.interfaces[j];
                            if (interface.interface_number == entry.interface_number &&
                                interface.alternate_setting == entry.alternate_setting) ++matches;
                        }
                        if (matches != 1) {
                            if (out_result) *out_result = invalid();
                            return {};
                        }
                    }
                }
                bool duplicate = false;
                for (const auto &prior : found) {
                    if (prior.product == product && prior.model == model && prior.match == match &&
                        ((match == DTV_USB_GROUP_AMBIGUOUS && prior.anchor_candidate_index == anchor) ||
                         ((match == DTV_USB_GROUP_COMPLETE || match == DTV_USB_GROUP_INCOMPLETE) &&
                          same_functions(prior, selected)))) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    BootGroupCandidate group{product, model, anchor, match, std::move(selected), {}};
                    group.snapshots.assign(snapshots.begin(), snapshots.end());
                    found.push_back(std::move(group));
                }
            }
        }
    }
    if (out_result) *out_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    return found;
}

dtv_internal_result check_operation_requirements(
    const BootGroupCandidate &group, dtv_spi_receiver_id receiver, std::uint32_t operation)
{
    if (group.model == nullptr || group.match == DTV_USB_GROUP_AMBIGUOUS ||
        (group.match != DTV_USB_GROUP_COMPLETE && group.match != DTV_USB_GROUP_INCOMPLETE)) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    const auto &model = *group.model;
    if (model.operation_requirement_count != 0 && model.operation_requirements == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    const dtv::spi::OperationRequirement *selected = nullptr;
    for (std::size_t i = 0; i < model.operation_requirement_count; ++i) {
        const auto &requirement = model.operation_requirements[i];
        if (requirement.receiver != receiver || requirement.operation != operation) continue;
        if (selected != nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        selected = &requirement;
    }
    if (selected != nullptr) {
        const auto &requirement = *selected;
        if (requirement.function_count != 0 && requirement.functions == nullptr) {
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        }
        for (std::size_t j = 0; j < requirement.function_count; ++j) {
            if (std::find(requirement.functions, requirement.functions + j,
                          requirement.functions[j]) != requirement.functions + j) {
                return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            }
            if (std::none_of(group.functions.begin(), group.functions.end(), [&](const auto &visible) {
                    return visible.function == requirement.functions[j];
                })) {
                return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
            }
        }
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
}

}
