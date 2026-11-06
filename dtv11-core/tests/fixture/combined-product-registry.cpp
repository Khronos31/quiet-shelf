// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstring>

#include <dtv/internal/discovery.hpp>

namespace {

enum class SelectionCase { complete, no_match, ambiguous, bad_index, bad_alternate, duplicate_role,
                           duplicate_interface, empty_complete, callback_error };
SelectionCase selection_case = SelectionCase::complete;
bool use_two_devices = false;
std::uint32_t callback_count = 0;

dtv_internal_result select_group(const dtv_usb_candidate_info *candidates, std::uint32_t candidate_count,
                                 std::uint32_t anchor, dtv_usb_function_selection *out,
                                 std::uint32_t capacity, std::uint32_t *out_count,
                                 dtv_usb_group_match *match)
{
    ++callback_count;
    if (selection_case == SelectionCase::callback_error) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 19);
    }
    if (selection_case == SelectionCase::no_match) {
        *out_count = 0;
        *match = DTV_USB_GROUP_NO_MATCH;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (selection_case == SelectionCase::ambiguous) {
        *out_count = 0;
        *match = DTV_USB_GROUP_AMBIGUOUS;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (candidates[anchor].port_path_length != 2 || candidates[anchor].interface_count == 0 ||
        candidates[anchor].interfaces[0].endpoint_count != 1) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    *match = DTV_USB_GROUP_COMPLETE;
    *out_count = selection_case == SelectionCase::empty_complete ? 0 : 2;
    if (*out_count > capacity) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    const auto first_candidate = use_two_devices && candidate_count == 2 ? 0u : anchor;
    const auto second_candidate = use_two_devices && candidate_count == 2 ? 1u : anchor;
    out[0] = {selection_case == SelectionCase::bad_index ? 4u : first_candidate, 1, 0, 0};
    out[1] = {second_candidate, selection_case == SelectionCase::duplicate_role ? 1u : 2u,
              static_cast<std::uint8_t>(selection_case == SelectionCase::duplicate_interface || use_two_devices ? 0 : 1),
              static_cast<std::uint8_t>(selection_case == SelectionCase::bad_alternate ? 3 : 0)};
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

const dtv_usb_identity cold_ids[]{{0x1234, 0x0001}};
const dtv::spi::ModelEntry first_model{
    .model = {.vendor_id = 0x1234, .product_id = 0x1001},
    .cold_identities = cold_ids,
    .cold_identity_count = 1,
    .select_usb_group = select_group};
const dtv::spi::ModelEntry second_model{
    .model = {.vendor_id = 0x1234, .product_id = 0x1002},
    .cold_identities = cold_ids,
    .cold_identity_count = 1,
    .select_usb_group = select_group};
const dtv::spi::ModelEntry *first_models[]{&first_model};
const dtv::spi::ModelEntry *second_models[]{&second_model};
const dtv::spi::ProductEntry cold_product_a{"cold-a", first_models, 1};
const dtv::spi::ProductEntry cold_product_b{"cold-b", second_models, 1};
const dtv::spi::ProductEntry *cold_products[]{&cold_product_a, &cold_product_b};

int check(bool value, int code) { return value ? 0 : code; }

}

int main()
{
    const auto registry = dtv::spi::product_registry();
    if (int failed = check(registry.size() == 2, 1)) return failed;
    if (int failed = check(std::strcmp(registry[0]->product_id, "i0_asicen_fixture") == 0 &&
                           std::strcmp(registry[1]->product_id, "i0_torne_fixture") == 0, 2)) return failed;
    std::uint32_t model_count = 0;
    if (int failed = check(dtv_model_count(&model_count).code == DTV_ERROR_OK && model_count == 2, 3)) return failed;

    std::array<dtv_usb_endpoint_info, 1> endpoint{{{0x81, 2, 512, 0}}};
    std::array<dtv::internal::UsbInterfaceSnapshot, 2> interfaces{};
    for (std::uint8_t i = 0; i < interfaces.size(); ++i) {
        interfaces[i].info.interface_number = i;
        interfaces[i].info.alternate_setting = 0;
        interfaces[i].endpoints.assign(endpoint.begin(), endpoint.end());
    }
    dtv::internal::UsbDeviceSnapshot snapshot{};
    snapshot.vendor_id = 0x1234;
    snapshot.product_id = 0x0001;
    snapshot.candidate_id = 77;
    snapshot.port_path = {2, 4};
    snapshot.interfaces.assign(interfaces.begin(), interfaces.end());
    const std::array snapshots{snapshot};
    dtv_internal_result result{};
    selection_case = SelectionCase::complete;
    auto candidates = dtv::internal::discover_boot_candidates(snapshots, cold_products, &result);
    if (int failed = check(result.code == DTV_INTERNAL_RESULT_OK && candidates.size() == 2, 4)) return failed;
    if (int failed = check(candidates[0].match == DTV_USB_GROUP_COMPLETE &&
                           candidates[1].product == &cold_product_b, 5)) return failed;
    if (int failed = check(candidates[0].functions[0].candidate_index == 0 &&
                           candidates[0].functions[1].candidate_index == 0, 9)) return failed;

    auto second_snapshot = snapshot;
    second_snapshot.candidate_id = 78;
    const std::array two_snapshots{snapshot, second_snapshot};
    use_two_devices = true;
    candidates = dtv::internal::discover_boot_candidates(two_snapshots, cold_products, &result);
    if (int failed = check(result.code == DTV_INTERNAL_RESULT_OK && candidates.size() == 2 &&
                           candidates[0].functions[0].candidate_index !=
                               candidates[0].functions[1].candidate_index, 10)) return failed;
    use_two_devices = false;

    selection_case = SelectionCase::no_match;
    candidates = dtv::internal::discover_boot_candidates(snapshots, cold_products, &result);
    if (int failed = check(result.code == DTV_INTERNAL_RESULT_OK && candidates.empty(), 6)) return failed;
    selection_case = SelectionCase::ambiguous;
    candidates = dtv::internal::discover_boot_candidates(snapshots, cold_products, &result);
    if (int failed = check(candidates.size() == 2 &&
                           candidates[0].match == DTV_USB_GROUP_AMBIGUOUS, 7)) return failed;
    selection_case = SelectionCase::callback_error;
    callback_count = 0;
    candidates = dtv::internal::discover_boot_candidates(snapshots, cold_products, &result);
    if (int failed = check(candidates.empty() && result.code == DTV_INTERNAL_RESULT_UNSUPPORTED &&
                           result.detail == 19 && callback_count == 1, 11)) return failed;
    for (const auto test : {SelectionCase::bad_index, SelectionCase::bad_alternate,
                            SelectionCase::duplicate_role, SelectionCase::duplicate_interface,
                            SelectionCase::empty_complete}) {
        selection_case = test;
        candidates = dtv::internal::discover_boot_candidates(snapshots, cold_products, &result);
        if (int failed = check(result.code == DTV_INTERNAL_RESULT_INVALID_ARGUMENT &&
                               candidates.empty(), 8)) return failed;
    }
    return 0;
}
