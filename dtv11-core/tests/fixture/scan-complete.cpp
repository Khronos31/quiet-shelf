/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

#include <dtv/internal/discovery.hpp>
#include <dtv/spi/types.h>

#include "usb-transfer-backend.hpp"

extern "C" void dtv_load_scanfixture(void);
void set_scan_group_case(unsigned value);
std::thread::id scan_group_callback_thread();

namespace {

enum : unsigned {
    complete, no_match, ambiguous, bad_index, bad_interface, callback_error, callback_exception
};

dtv::internal::UsbDeviceSnapshot snapshot(std::uint16_t product_id, std::uint64_t candidate_id)
{
    dtv::internal::UsbDeviceSnapshot value{};
    value.vendor_id = 0x1234;
    value.product_id = product_id;
    value.candidate_id = candidate_id;
    value.connection_token = candidate_id;
    value.bus_number = 1;
    value.device_address = static_cast<std::uint8_t>(candidate_id);
    value.port_path = {1, static_cast<std::uint8_t>(candidate_id)};
    dtv::internal::UsbInterfaceSnapshot interface{};
    interface.info.interface_number = 0;
    interface.info.alternate_setting = 0;
    value.interfaces.push_back(interface);
    return value;
}

int check_complete(std::vector<dtv::internal::UsbDeviceSnapshot> snapshots,
                   std::size_t expected_snapshots, std::size_t expected_functions)
{
    set_scan_group_case(complete);
    dtv::internal::CompleteGroupBatch batch;
    const auto caller = std::this_thread::get_id();
    const auto result = dtv::internal::observe_loaded_product_groups(std::move(snapshots), &batch);
    if (result.code != DTV_INTERNAL_RESULT_OK || batch.snapshots.size() != expected_snapshots ||
        batch.groups.size() != 1 || batch.groups[0].functions.size() != expected_functions ||
        batch.groups[0].product == nullptr || batch.groups[0].model == nullptr ||
        batch.groups[0].model->model == nullptr ||
        std::string(batch.groups[0].model->model->name) != "Scan Fixture" ||
        batch.groups[0].anchor_candidate_index >= batch.snapshots.size() ||
        std::strcmp(batch.groups[0].product->product_id, "scanfixture") != 0 ||
        scan_group_callback_thread() != caller) {
        return 1;
    }
    for (const auto &function : batch.groups[0].functions) {
        if (function.candidate_index >= batch.snapshots.size()) return 1;
        const auto &interfaces = batch.snapshots[function.candidate_index].interfaces;
        if (std::none_of(interfaces.begin(), interfaces.end(), [&](const auto &interface) {
                return interface.info.interface_number == function.interface_number &&
                       interface.info.alternate_setting == function.alternate_setting;
            })) return 1;
    }
    if (expected_functions == 2 &&
        batch.groups[0].functions[0].candidate_index == batch.groups[0].functions[1].candidate_index) {
        return 1;
    }
    return 0;
}

}

int main()
{
    auto context = dtv::internal::UsbContext::scan_only();
    if (context.result().code != DTV_INTERNAL_RESULT_OK ||
        dtv::internal::UsbTestDeviceFactory::worker_count(context) != 0) {
        return 1;
    }

    dtv::internal::CompleteGroupBatch batch;
    auto unloaded = dtv::internal::observe_loaded_product_groups(
        {snapshot(0x0101, 1)}, &batch);
    if (unloaded.code != DTV_INTERNAL_RESULT_OK || !batch.groups.empty()) return 2;
    dtv_load_scanfixture();
    if (check_complete({snapshot(0x0101, 1)}, 1, 1) != 0) return 3;
    if (check_complete({snapshot(0x0201, 2), snapshot(0x0202, 3)}, 2, 2) != 0) return 4;

    set_scan_group_case(complete);
    auto incomplete = dtv::internal::observe_loaded_product_groups(
        {snapshot(0x0201, 4)}, &batch);
    if (incomplete.code != DTV_INTERNAL_RESULT_OK || !batch.groups.empty() ||
        batch.snapshots.size() != 1) return 5;
    for (const auto test_case : {no_match, ambiguous}) {
        set_scan_group_case(test_case);
        auto empty = dtv::internal::observe_loaded_product_groups(
            {snapshot(0x0101, 5)}, &batch);
        if (empty.code != DTV_INTERNAL_RESULT_OK || !batch.groups.empty()) return 6;
    }

    for (const auto test_case : {bad_index, bad_interface}) {
        set_scan_group_case(test_case);
        auto invalid = dtv::internal::observe_loaded_product_groups(
            {snapshot(0x0101, 6)}, &batch);
        if (invalid.code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT ||
            !batch.groups.empty() || !batch.snapshots.empty()) return 7;
    }
    set_scan_group_case(callback_error);
    auto failed = dtv::internal::observe_loaded_product_groups({snapshot(0x0101, 7)}, &batch);
    if (failed.code != DTV_INTERNAL_RESULT_UNSUPPORTED || !batch.groups.empty() ||
        !batch.snapshots.empty()) return 8;
    set_scan_group_case(callback_exception);
    auto exhausted = dtv::internal::observe_loaded_product_groups({snapshot(0x0101, 8)}, &batch);
    return exhausted.code == DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED && batch.groups.empty() &&
                   batch.snapshots.empty()
               ? 0
               : 9;
}
