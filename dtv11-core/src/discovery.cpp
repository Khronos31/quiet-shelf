// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/discovery.hpp>

#include <algorithm>
#include <limits>
#include <map>
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

bool declares_identity(const dtv::spi::l3_model_record &model,
                       const UsbDeviceSnapshot &snapshot)
{
    if (model.model != nullptr && model.model->vid == snapshot.vendor_id &&
        model.model->pid == snapshot.product_id) return true;
    if (model.discovery == nullptr) return false;
    const auto &discovery = *model.discovery;
    for (std::size_t index = 0; index < discovery.cold_identity_count; ++index) {
        if (discovery.cold_identities[index].vendor_id == snapshot.vendor_id &&
            discovery.cold_identities[index].product_id == snapshot.product_id) return true;
    }
    return false;
}

bool same_usb_location(const UsbDeviceSnapshot &left, const UsbDeviceSnapshot &right)
{
    return left.vendor_id == right.vendor_id && left.product_id == right.product_id &&
           left.bus_number == right.bus_number && left.device_address == right.device_address &&
           left.port_path == right.port_path;
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

dtv_internal_result collect_complete_groups(
    std::span<const LoadedProductDiscoveryView> products,
    std::vector<UsbDeviceSnapshot> snapshots,
    CompleteGroupBatch *out)
{
    if (out == nullptr) {
        return invalid();
    }
    out->snapshots.clear();
    out->groups.clear();
    out->ungrouped.clear();
    try {
        std::vector<const dtv::spi::ProductEntry *> discovery_products;
        discovery_products.reserve(products.size());
        for (const auto &product : products) {
            if (product.product == nullptr || product.models == nullptr ||
                product.model_count != product.product->model_count ||
                (product.model_count != 0 && product.product->models == nullptr)) {
                return invalid();
            }
            for (std::size_t index = 0; index < product.model_count; ++index) {
                if (product.models[index] == nullptr || product.models[index]->discovery == nullptr ||
                    product.models[index]->discovery != product.product->models[index]) {
                    return invalid();
                }
            }
            discovery_products.push_back(product.product);
        }

        dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        auto candidates = discover_boot_candidates(snapshots, discovery_products, &result);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            return result;
        }

        CompleteGroupBatch batch;
        batch.snapshots = std::move(snapshots);
        std::set<std::uint64_t> complete_connections;
        for (auto &candidate : candidates) {
            if (candidate.match != DTV_USB_GROUP_COMPLETE) continue;
            for (const auto &function : candidate.functions) {
                if (function.candidate_index >= batch.snapshots.size()) return invalid();
                complete_connections.insert(
                    batch.snapshots[function.candidate_index].connection_token);
            }
            const auto product = std::find_if(products.begin(), products.end(), [&](const auto &view) {
                return view.product == candidate.product;
            });
            if (product == products.end()) return invalid();
            const dtv::spi::l3_model_record *model_record = nullptr;
            for (std::size_t index = 0; index < product->model_count; ++index) {
                if (product->models[index]->discovery != candidate.model) continue;
                if (model_record != nullptr) return invalid();
                model_record = product->models[index];
            }
            if (model_record == nullptr) return invalid();
            batch.groups.push_back({candidate.product, model_record,
                                    candidate.anchor_candidate_index,
                                    std::move(candidate.functions)});
        }

        std::map<std::uint64_t, std::size_t> ungrouped_by_connection;
        for (std::size_t snapshot_index = 0; snapshot_index < batch.snapshots.size();
             ++snapshot_index) {
            const auto &snapshot = batch.snapshots[snapshot_index];
            std::set<const dtv::spi::ModelEntry *> matching_models;
            for (const auto &product : products) {
                for (std::size_t model_index = 0; model_index < product.model_count;
                     ++model_index) {
                    const auto *model = product.models[model_index];
                    if (declares_identity(*model, snapshot)) {
                        matching_models.insert(model->discovery);
                    }
                }
            }
            if (matching_models.empty() || complete_connections.contains(snapshot.connection_token)) {
                continue;
            }
            if (snapshot.connection_token == 0) return invalid();
            const auto previous = ungrouped_by_connection.find(snapshot.connection_token);
            if (previous != ungrouped_by_connection.end()) {
                const auto &prior = batch.snapshots[
                    batch.ungrouped[previous->second].snapshot_index];
                if (!same_usb_location(prior, snapshot)) return invalid();
                continue;
            }
            const char *model_name = nullptr;
            if (matching_models.size() == 1) {
                const auto *model = *matching_models.begin();
                if (model != nullptr && model->model.name != nullptr &&
                    model->model.name[0] != '\0') model_name = model->model.name;
            }
            ungrouped_by_connection.emplace(snapshot.connection_token, batch.ungrouped.size());
            batch.ungrouped.push_back({static_cast<std::uint32_t>(snapshot_index),
                                       snapshot.connection_token, model_name});
        }
        *out = std::move(batch);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    } catch (...) {
        return dtv::internal::exception_result();
    }
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
