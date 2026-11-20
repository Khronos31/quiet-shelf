/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <new>
#include <utility>
#include <vector>

#include <dtv/internal/discovery.hpp>
#include <dtv/internal/l3-registry.hpp>
#include <dtv/internal/result.hpp>

extern "C" void dtv_load_scanfixture(void);
extern "C" void dtv_load_fixture(void);
void set_scan_group_case(unsigned value);

namespace {

dtv::internal::UsbDeviceSnapshot snapshot(std::uint16_t vendor, std::uint16_t product,
                                          std::uint64_t token, std::uint8_t bus,
                                          std::uint8_t address,
                                          std::vector<std::uint8_t> path)
{
    dtv::internal::UsbDeviceSnapshot value{};
    value.vendor_id = vendor;
    value.product_id = product;
    value.candidate_id = token;
    value.connection_token = token;
    value.bus_number = bus;
    value.device_address = address;
    value.port_path = std::move(path);
    dtv::internal::UsbInterfaceSnapshot interface{};
    interface.info.interface_number = 0;
    interface.info.alternate_setting = 0;
    value.interfaces.push_back(interface);
    return value;
}

dtv::internal::CompleteGroupBatch batch_for(unsigned mode)
{
    dtv::internal::CompleteGroupBatch batch;
    if (mode == 4) return batch;
    if (mode == 5) {
        batch.snapshots.push_back(snapshot(0xabcd, 0x9999, 404, 1, 4, {4}));
    } else if (mode == 6) {
        batch.snapshots.push_back(snapshot(0x1234, 0x0001, 505, 1, 5, {5}));
    } else if (mode == 7) {
        batch.snapshots.push_back(snapshot(0x1234, 0x0001, 606, 1, 6, {6}));
        auto duplicate = snapshot(0x1234, 0x0001, 606, 1, 6, {6});
        duplicate.candidate_id = 607;
        batch.snapshots.push_back(std::move(duplicate));
    } else if (mode == 8) {
        for (std::uint32_t index = 0; index <= DTV_MAX_ENCLOSURES; ++index) {
            const auto port = static_cast<std::uint8_t>(index + 1);
            batch.snapshots.push_back(snapshot(0x1234, 0x0001, 700 + index,
                                               1, port, {1, port}));
        }
    } else if (mode == 3) {
        batch.snapshots.push_back(snapshot(0x1234, 0x0201, 303, 1, 7, {1, 7}));
        batch.snapshots.push_back(snapshot(0x1234, 0x0202, 102, 1, 8, {1, 8}));
    } else if (mode == 2) {
        batch.snapshots.push_back(snapshot(0x1234, 0x0201, 101, 2, 8, {2, 8}));
    } else if (mode == 10) {
        batch.snapshots.push_back(snapshot(0x1234, 0x0202, 104, 1, 8, {1, 8}));
    } else {
        const auto token = mode == 9 ? 303 : 101;
        batch.snapshots.push_back(snapshot(0x1234, 0x0201, token, 1, 7, {1, 7}));
    }
    set_scan_group_case(0);
    const auto result = dtv::internal::observe_loaded_product_groups(
        std::move(batch.snapshots), &batch);
    if (result.code != DTV_INTERNAL_RESULT_OK) return {};
    return batch;
}

}

extern "C" void dtv_ungrouped_test_batch(unsigned mode)
{
    dtv::internal::set_public_discovery_batch_for_test(batch_for(mode));
}

extern "C" void dtv_ungrouped_test_scan_error(unsigned fail)
{
    dtv::internal::set_public_discovery_scan_error_for_test(dtv_internal_result_make(
        fail != 0 ? DTV_INTERNAL_RESULT_USB_IO : DTV_INTERNAL_RESULT_OK, 0));
}

extern "C" void dtv_ungrouped_test_fail_allocation(unsigned fail)
{
    if (fail != 0) {
        dtv::internal::set_public_discovery_ungrouped_allocation_hook_for_test(
            [] { throw std::bad_alloc(); });
    } else {
        dtv::internal::set_public_discovery_ungrouped_allocation_hook_for_test({});
    }
}
