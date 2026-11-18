/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 機種の宣言による筐体 serial の導出・L3 公開・一意性の契約 test（決定 66）。 */
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <dtv/dtv.h>
#include <dtv/internal/discovery.hpp>
#include <dtv/internal/l3-registry.hpp>
#include <dtv/internal/usb.hpp>
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

namespace {

unsigned derive_mode = 0;
bool saw_candidate_serial = false;

dtv_internal_result derive_common_base(const dtv_serial_derive_input *input,
                                       dtv_serial_derive_output *output,
                                       char *buffer, std::uint32_t capacity)
{
    if (input == nullptr || output == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    if (derive_mode == 1) {
        output->outcome = DTV_SERIAL_DERIVE_UNKNOWN;
        output->length = 0;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (derive_mode == 2) {
        output->outcome = DTV_SERIAL_DERIVE_VALUE;
        output->length = 1;
        if (buffer != nullptr && capacity >= 1) buffer[0] = '\t';
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (derive_mode == 3 || derive_mode == 4) {
        output->outcome = DTV_SERIAL_DERIVE_VALUE;
        output->length = derive_mode == 3 ? 513u : 0xffffffffu;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (derive_mode == 5) {
        output->outcome = DTV_SERIAL_DERIVE_VALUE;
        output->length = 512;
        if (buffer != nullptr && capacity >= 512) {
            std::memset(buffer, 'A', 512);
        }
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    std::string base;
    for (std::uint32_t index = 0; index < input->function_count; ++index) {
        const auto &candidate = input->candidates[input->functions[index].candidate_index];
        if (candidate.serial_state != DTV_USB_SERIAL_PRESENT || candidate.serial_utf8 == nullptr ||
            candidate.serial_utf8_bytes < 2) {
            output->outcome = DTV_SERIAL_DERIVE_UNKNOWN;
            output->length = 0;
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        const std::string value(reinterpret_cast<const char *>(candidate.serial_utf8),
                                candidate.serial_utf8_bytes);
        if (index == 0) {
            base = value.substr(0, value.size() - 1);
        } else if (value.size() != base.size() + 1 ||
                   value.compare(0, base.size(), base) != 0) {
            output->outcome = DTV_SERIAL_DERIVE_UNKNOWN;
            output->length = 0;
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
    }
    output->outcome = DTV_SERIAL_DERIVE_VALUE;
    output->length = static_cast<std::uint32_t>(base.size());
    if (buffer != nullptr && capacity >= base.size()) {
        std::memcpy(buffer, base.data(), base.size());
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

const dtv_usb_identity pair_identities[]{{0x1234, 0x0201}, {0x1234, 0x0202}};
const dtv_usb_identity no_callback_identities[]{{0x1234, 0x0401}, {0x1234, 0x0402}};
const dtv_usb_identity single_identities[]{{0x1234, 0x0301}};

dtv_internal_result select_pair(const dtv_usb_candidate_info *candidates,
                                std::uint32_t candidate_count, std::uint32_t anchor,
                                dtv_usb_function_selection *out, std::uint32_t capacity,
                                std::uint32_t *out_count, dtv_usb_group_match *match)
{
    if (anchor >= candidate_count ||
        (candidates[anchor].product_id != 0x0201 && candidates[anchor].product_id != 0x0202)) {
        *out_count = 0;
        *match = DTV_USB_GROUP_NO_MATCH;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    const std::uint16_t other = candidates[anchor].product_id == 0x0201 ? 0x0202 : 0x0201;
    std::uint32_t partner = candidate_count;
    for (std::uint32_t index = 0; index < candidate_count; ++index) {
        if (candidates[index].serial_state == DTV_USB_SERIAL_PRESENT &&
            candidates[index].serial_utf8 != nullptr && candidates[index].serial_utf8_bytes != 0) {
            saw_candidate_serial = true;
        }
        if (candidates[index].product_id == other) {
            partner = index;
            break;
        }
    }
    if (partner == candidate_count) {
        *out_count = 1;
        out[0] = {anchor, 1, 0, 0};
        *match = DTV_USB_GROUP_INCOMPLETE;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (capacity < 2) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    *out_count = 2;
    out[0] = {anchor, candidates[anchor].product_id == 0x0201 ? 1u : 2u, 0, 0};
    out[1] = {partner, candidates[partner].product_id == 0x0201 ? 1u : 2u, 0, 0};
    *match = DTV_USB_GROUP_COMPLETE;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result select_no_callback_pair(
    const dtv_usb_candidate_info *candidates, std::uint32_t candidate_count, std::uint32_t anchor,
    dtv_usb_function_selection *out, std::uint32_t capacity, std::uint32_t *out_count,
    dtv_usb_group_match *match)
{
    if (anchor >= candidate_count ||
        (candidates[anchor].product_id != 0x0401 && candidates[anchor].product_id != 0x0402)) {
        *out_count = 0;
        *match = DTV_USB_GROUP_NO_MATCH;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    const std::uint16_t other = candidates[anchor].product_id == 0x0401 ? 0x0402 : 0x0401;
    std::uint32_t partner = candidate_count;
    for (std::uint32_t index = 0; index < candidate_count; ++index) {
        if (candidates[index].product_id == other) {
            partner = index;
            break;
        }
    }
    if (partner == candidate_count) {
        *out_count = 1;
        out[0] = {anchor, 1, 0, 0};
        *match = DTV_USB_GROUP_INCOMPLETE;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (capacity < 2) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    *out_count = 2;
    out[0] = {anchor, candidates[anchor].product_id == 0x0401 ? 1u : 2u, 0, 0};
    out[1] = {partner, candidates[partner].product_id == 0x0401 ? 1u : 2u, 0, 0};
    *match = DTV_USB_GROUP_COMPLETE;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result select_single(const dtv_usb_candidate_info *candidates,
                                  std::uint32_t candidate_count, std::uint32_t anchor,
                                  dtv_usb_function_selection *out, std::uint32_t capacity,
                                  std::uint32_t *out_count, dtv_usb_group_match *match)
{
    if (anchor >= candidate_count || candidates[anchor].product_id != 0x0301) {
        *out_count = 0;
        *match = DTV_USB_GROUP_NO_MATCH;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (candidates[anchor].serial_state == DTV_USB_SERIAL_PRESENT &&
        candidates[anchor].serial_utf8 != nullptr && candidates[anchor].serial_utf8_bytes != 0) {
        saw_candidate_serial = true;
    }
    if (capacity == 0) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    *out_count = 1;
    out[0] = {anchor, 1, 0, 0};
    *match = DTV_USB_GROUP_COMPLETE;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

const dtv_model pair_model{"synthserial", "Serial Pair", 0x1234, 0x0201};
const dtv_model single_model{"synthserial", "Serial Single", 0x1234, 0x0301};
const dtv_model multi_model{"synthserial", "Serial Multi", 0x1234, 0x0401};

const dtv::spi::ModelEntry pair_entry{
    .model = {"Serial Pair", 0x1234, 0x0201},
    .cold_identities = pair_identities,
    .cold_identity_count = 2,
    .select_usb_group = select_pair,
};
const dtv::spi::ModelEntry single_entry{
    .model = {"Serial Single", 0x1234, 0x0301},
    .cold_identities = single_identities,
    .cold_identity_count = 1,
    .select_usb_group = select_single,
};
const dtv::spi::ModelEntry multi_entry{
    .model = {"Serial Multi", 0x1234, 0x0401},
    .cold_identities = no_callback_identities,
    .cold_identity_count = 2,
    .select_usb_group = select_no_callback_pair,
};

const dtv::spi::model_information pair_information{
    .name = "Serial Pair",
    .vendor_id = 0x1234,
    .product_id = 0x0201,
    .derive_enclosure_serial = derive_common_base,
};
const dtv::spi::model_information single_information{
    .name = "Serial Single",
    .vendor_id = 0x1234,
    .product_id = 0x0301,
};
const dtv::spi::model_information multi_information{
    .name = "Serial Multi",
    .vendor_id = 0x1234,
    .product_id = 0x0401,
};

const dtv::spi::l3_model_record pair_record{
    &pair_model, 0, nullptr, &pair_entry, &pair_information};
const dtv::spi::l3_model_record single_record{
    &single_model, 0, nullptr, &single_entry, &single_information};
const dtv::spi::l3_model_record multi_record{
    &multi_model, 0, nullptr, &multi_entry, &multi_information};

const dtv::spi::ModelEntry *product_models[]{&pair_entry, &single_entry, &multi_entry};
const dtv::spi::ProductEntry product{"synthserial", product_models, 3};
const dtv::spi::l3_model_record *product_records[]{&pair_record, &single_record, &multi_record};

dtv::internal::UsbDeviceSnapshot snapshot(std::uint16_t product_id, std::uint64_t token,
                                          std::uint8_t address, std::uint32_t serial_state,
                                          const std::string &serial)
{
    dtv::internal::UsbDeviceSnapshot value{};
    value.vendor_id = 0x1234;
    value.product_id = product_id;
    value.candidate_id = token;
    value.connection_token = token;
    value.bus_number = 1;
    value.device_address = address;
    value.port_path = {1, address};
    value.serial_state = serial_state;
    if (serial_state == DTV_USB_SERIAL_PRESENT) value.serial_utf8 = serial;
    dtv::internal::UsbInterfaceSnapshot interface{};
    interface.info.interface_number = 0;
    interface.info.alternate_setting = 0;
    value.interfaces.push_back(interface);
    return value;
}

dtv_internal_result collect(std::vector<dtv::internal::UsbDeviceSnapshot> snapshots,
                            dtv::internal::CompleteGroupBatch *batch)
{
    const std::array<dtv::internal::LoadedProductDiscoveryView, 1> views{{
        {&product, product_records, 3}}};
    return dtv::internal::collect_complete_groups(views, std::move(snapshots), batch);
}

int publish(dtv::internal::CompleteGroupBatch batch)
{
    dtv::internal::set_public_discovery_batch_for_test(std::move(batch));
    dtv_error error{};
    error.struct_size = sizeof(error);
    return dtv_list_enclosures(&error) != nullptr && error.code == DTV_ERROR_OK ? 0 : 1;
}

dtv_enclosure *first_enclosure(dtv_error *error)
{
    dtv_enclosure *const *list = dtv_list_enclosures(error);
    if (list == nullptr) return nullptr;
    for (std::size_t index = 0; index < dtv_get_list_capacity(); ++index) {
        if (list[index] != nullptr) return list[index];
    }
    return nullptr;
}

}

int main()
{
    dtv_error error{};
    error.struct_size = sizeof(error);

    /* 宣言 callback が末尾違いから共通 base を導く。L3 は導出値を返す。 */
    {
        derive_mode = 0;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0201, 101, 2, DTV_USB_SERIAL_PRESENT, "ABCD1"),
                     snapshot(0x0202, 102, 3, DTV_USB_SERIAL_PRESENT, "ABCD2")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1 || !batch.groups[0].serial_known ||
            batch.groups[0].serial != "ABCD" || !saw_candidate_serial) return 1;
        if (publish(std::move(batch)) != 0) return 2;
        auto *root = first_enclosure(&error);
        if (root == nullptr || dtv_get_enclosure_serial(root) == nullptr ||
            std::strcmp(dtv_get_enclosure_serial(root), "ABCD") != 0) return 3;
        if (dtv_get_serial_unique(root) != DTV_YES) return 4;
        const dtv_candidate *const *candidates =
            dtv_get_candidates(root);
        if (candidates == nullptr || candidates[0] == nullptr ||
            candidates[0]->serial == nullptr ||
            std::strcmp(candidates[0]->serial, "ABCD1") != 0) return 5;
        dtv_device *const *devices = dtv_get_devices(root);
        if (devices == nullptr || devices[0] == nullptr ||
            dtv_get_device_serial(devices[0]) == nullptr ||
            std::strcmp(dtv_get_device_serial(devices[0]), "ABCD1") != 0) return 6;
    }

    /* callback が UNKNOWN を返しても group は消えず、値だけ NULL になる。 */
    {
        derive_mode = 1;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0201, 111, 2, DTV_USB_SERIAL_PRESENT, "WXYZ1"),
                     snapshot(0x0202, 112, 3, DTV_USB_SERIAL_PRESENT, "WXYZ2")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1 || batch.groups[0].serial_known) return 7;
        if (publish(std::move(batch)) != 0) return 8;
        auto *root = first_enclosure(&error);
        if (root == nullptr ||
            dtv_get_enclosure_serial(root) != nullptr ||
            dtv_get_serial_unique(root) != DTV_UNKNOWN)
            return 9;
    }

    /* 不正出力（制御文字・513・極大長）は確保前に拒み、値だけ UNKNOWN にする。 */
    for (const unsigned mode : {2u, 3u, 4u}) {
        derive_mode = mode;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0201, 121, 2, DTV_USB_SERIAL_PRESENT, "QQQQ1"),
                     snapshot(0x0202, 122, 3, DTV_USB_SERIAL_PRESENT, "QQQQ2")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1 || batch.groups[0].serial_known) return 10;
    }

    /* 512 byte は通る。 */
    {
        derive_mode = 5;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0201, 131, 2, DTV_USB_SERIAL_PRESENT, "RRRR1"),
                     snapshot(0x0202, 132, 3, DTV_USB_SERIAL_PRESENT, "RRRR2")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1 || !batch.groups[0].serial_known ||
            batch.groups[0].serial.size() != 512) return 11;
    }

    /* 宣言のない単一 device は raw をそのまま使い、2 device は NULL。 */
    {
        derive_mode = 0;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0301, 141, 2, DTV_USB_SERIAL_PRESENT, "SINGLE-RAW")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1 || !batch.groups[0].serial_known ||
            batch.groups[0].serial != "SINGLE-RAW") return 12;
        derive_mode = 0;
        if (collect({snapshot(0x0401, 151, 2, DTV_USB_SERIAL_PRESENT, "MULTI1"),
                     snapshot(0x0402, 152, 3, DTV_USB_SERIAL_PRESENT, "MULTI2")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1 || batch.groups[0].serial_known) return 13;
    }

    /* 公開規則を外れる raw は候補には残るが、公開 serial は NULL。 */
    {
        derive_mode = 0;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0301, 161, 2, DTV_USB_SERIAL_PRESENT, "BAD\tTEXT")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1 || batch.groups[0].serial_known) return 14;
        if (publish(std::move(batch)) != 0) return 15;
        auto *root = first_enclosure(&error);
        if (root == nullptr ||
            dtv_get_enclosure_serial(root) != nullptr) return 16;
        const dtv_candidate *const *candidates =
            dtv_get_candidates(root);
        if (candidates == nullptr || candidates[0] == nullptr ||
            candidates[0]->serial != nullptr) return 17;
    }

    /* 同じ導出値の別筐体は NO。不明の別筐体は YES を妨げず、ungrouped も比較外。 */
    {
        derive_mode = 0;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0201, 171, 2, DTV_USB_SERIAL_PRESENT, "SAME1"),
                     snapshot(0x0202, 172, 3, DTV_USB_SERIAL_PRESENT, "SAME2"),
                     snapshot(0x0301, 173, 4, DTV_USB_SERIAL_PRESENT, "SAME"),
                     snapshot(0x0301, 174, 5, DTV_USB_SERIAL_ABSENT, ""),
                     snapshot(0x0401, 175, 6, DTV_USB_SERIAL_PRESENT, "SAME")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 3) return 18;
        bool pair_no = false;
        bool single_no = false;
        bool absent_unknown = false;
        for (const auto &group : batch.groups) {
            if (!group.serial_known) {
                absent_unknown = true;
            } else if (group.serial == "SAME" && group.functions.size() == 2) {
                pair_no = group.serial_unique == DTV_NO;
            } else if (group.serial == "SAME" && group.functions.size() == 1) {
                single_no = group.serial_unique == DTV_NO;
            }
        }
        if (!pair_no || !single_no || !absent_unknown) return 19;
        if (publish(std::move(batch)) != 0) return 20;
        const dtv_ungrouped_device *const *entries = dtv_list_ungrouped_devices();
        if (entries == nullptr || entries[0] == nullptr || entries[0]->serial == nullptr ||
            std::strcmp(entries[0]->serial, "SAME") != 0) return 21;
    }

    /* 他が NULL でも、対象 serial が既知で重なりがなければ YES。 */
    {
        derive_mode = 0;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0301, 181, 2, DTV_USB_SERIAL_PRESENT, "ONLY-KNOWN"),
                     snapshot(0x0301, 182, 3, DTV_USB_SERIAL_UNREADABLE, "")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 2) return 22;
        bool known_yes = false;
        bool unreadable_unknown = false;
        for (const auto &group : batch.groups) {
            if (!group.serial_known) unreadable_unknown = true;
            else if (group.serial == "ONLY-KNOWN" && group.serial_unique == DTV_YES) known_yes = true;
        }
        if (!known_yes || !unreadable_unknown) return 23;
    }

    /* 読取失敗でも同じ接続の既知値と候補の不変値を保つ。 */
    {
        derive_mode = 0;
        dtv::internal::CompleteGroupBatch batch;
        if (collect({snapshot(0x0301, 191, 2, DTV_USB_SERIAL_PRESENT, "KEEP-ME")},
                    &batch).code != DTV_INTERNAL_RESULT_OK) return 24;
        if (publish(std::move(batch)) != 0) return 25;
        auto *root = first_enclosure(&error);
        if (root == nullptr || dtv_get_enclosure_serial(root) == nullptr ||
            std::strcmp(dtv_get_enclosure_serial(root), "KEEP-ME") != 0) return 26;
        const dtv_candidate *const *candidates = dtv_get_candidates(root);
        if (candidates == nullptr || candidates[0] == nullptr) return 27;
        const auto *candidate = candidates[0];
        dtv_hold_enclosure(root);

        if (collect({snapshot(0x0301, 191, 2, DTV_USB_SERIAL_UNREADABLE, "")},
                    &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != 1 || batch.groups[0].serial_known) return 28;
        if (publish(std::move(batch)) != 0) return 29;
        if (dtv_get_enclosure_serial(root) == nullptr ||
            std::strcmp(dtv_get_enclosure_serial(root), "KEEP-ME") != 0) return 30;
        if (dtv_get_candidates(root) != candidates || dtv_get_candidates(root)[0] != candidate ||
            candidate->serial == nullptr ||
            std::strcmp(candidate->serial, "KEEP-ME") != 0) return 31;

        /* 走査が済んでいないので、既知の重複 (NO) 以外は UNKNOWN。serial は保つ。 */
        dtv::internal::set_public_discovery_scan_error_for_test(
            dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0));
        (void)dtv_list_enclosures(&error);
        if (dtv_get_serial_unique(root) != DTV_UNKNOWN) return 32;
        if (dtv_get_enclosure_serial(root) == nullptr ||
            std::strcmp(dtv_get_enclosure_serial(root), "KEEP-ME") != 0) return 33;
        dtv::internal::set_public_discovery_scan_error_for_test(
            dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0));
        dtv_drop_enclosure(root);
    }

    /* 容量を超えて省いた筐体も比較に入る。 */
    {
        derive_mode = 0;
        std::vector<dtv::internal::UsbDeviceSnapshot> snapshots;
        for (std::uint32_t index = 0; index <= DTV_MAX_ENCLOSURES; ++index) {
            snapshots.push_back(snapshot(0x0301, 300 + index,
                                         static_cast<std::uint8_t>(index + 1),
                                         DTV_USB_SERIAL_PRESENT, "DUP"));
        }
        dtv::internal::CompleteGroupBatch batch;
        if (collect(std::move(snapshots), &batch).code != DTV_INTERNAL_RESULT_OK ||
            batch.groups.size() != DTV_MAX_ENCLOSURES + 1) return 34;
        if (publish(std::move(batch)) != 0) return 35;
        dtv_enclosure *const *list = dtv_list_enclosures(&error);
        if (list == nullptr) return 36;
        std::size_t live = 0;
        for (std::size_t index = 0; index < dtv_get_list_capacity(); ++index) {
            if (list[index] == nullptr) continue;
            ++live;
            if (dtv_get_serial_unique(list[index]) != DTV_NO) return 37;
        }
        if (live != DTV_MAX_ENCLOSURES) return 38;
    }

    return 0;
}
