/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <new>
#include <thread>
#include <utility>

#include <dtv/internal/discovery.hpp>
#include <dtv/internal/l3-registry.hpp>
#include <dtv/internal/result.hpp>
#include <i0_asicen_fixture_api.h>
#include <i0_torne_fixture_api.h>

extern "C" void dtv_load_i0_asicen_fixture(void);
extern "C" void dtv_load_i0_torne_fixture(void);
extern const dtv::spi::l3_model_record dtv_l3_model_i0_asicen_fixture_px_w3u3;
extern const dtv::spi::l3_model_record dtv_l3_model_i0_torne_fixture_cech_zd1j;

namespace {

const dtv::spi::ProductEntry *product_for(const dtv::spi::l3_model_record &model)
{
    for (const auto *product : dtv::spi::product_registry()) {
        if (product == nullptr || product->models == nullptr) continue;
        for (std::size_t index = 0; index < product->model_count; ++index) {
            if (product->models[index] == model.discovery) return product;
        }
    }
    return nullptr;
}

dtv::internal::UsbDeviceSnapshot snapshot(std::uint64_t token, std::uint16_t vendor,
                                          std::uint16_t product)
{
    dtv::internal::UsbDeviceSnapshot value{};
    value.vendor_id = vendor;
    value.product_id = product;
    value.candidate_id = token;
    value.connection_token = token;
    value.bus_number = 1;
    value.device_address = static_cast<std::uint8_t>(token);
    value.port_path = {static_cast<std::uint8_t>(token)};
    dtv::internal::UsbInterfaceSnapshot interface{};
    interface.info.interface_number = 0;
    interface.info.alternate_setting = 0;
    value.interfaces.push_back(interface);
    return value;
}

dtv::internal::CompleteGroupBatch batch_for(unsigned mode)
{
    dtv::internal::CompleteGroupBatch batch;
    if (mode == 1) return batch;
    const auto append = [&batch](const dtv::spi::l3_model_record &model,
                                 std::uint64_t token, std::uint16_t vendor,
                                 std::uint16_t product, std::uint32_t function) {
        const auto candidate = static_cast<std::uint32_t>(batch.snapshots.size());
        batch.snapshots.push_back(snapshot(token, vendor, product));
        batch.groups.push_back({product_for(model), &model, candidate,
                                {{candidate, function, 0, 0}}});
    };
    append(dtv_l3_model_i0_asicen_fixture_px_w3u3, 101, 0x0b06, 0x0005, 1);
    if (mode != 2) {
        append(dtv_l3_model_i0_torne_fixture_cech_zd1j, 202, 0x054c, 0x04b2, 2);
    }
    return batch;
}

}

extern "C" void dtv_product_list_test_batch(unsigned mode)
{
    dtv::internal::set_public_discovery_batch_for_test(batch_for(mode));
}

extern "C" void dtv_product_list_test_fail_journal(unsigned fail)
{
    if (fail != 0) {
        dtv::internal::set_public_discovery_journal_hook_for_test([] { throw std::bad_alloc(); });
    } else {
        dtv::internal::set_public_discovery_journal_hook_for_test({});
    }
}

extern "C" void dtv_product_list_test_scan_error(unsigned fail)
{
    const auto result = dtv_internal_result_make(
        fail != 0 ? DTV_INTERNAL_RESULT_USB_IO : DTV_INTERNAL_RESULT_OK, 0);
    dtv::internal::set_public_discovery_scan_error_for_test(result);
}

extern "C" dtv_enclosure *const *dtv_product_list_test_call_other_thread(
    unsigned product, int32_t *out_error)
{
    dtv_enclosure *const *result = nullptr;
    dtv_error error{};
    error.struct_size = sizeof(error);
    std::thread caller([&] {
        result = product == 0 ? dtv_list_enclosures_i0_asicen_fixture(&error)
                              : dtv_list_enclosures_i0_torne_fixture(&error);
    });
    caller.join();
    if (out_error != nullptr) *out_error = error.code;
    return result;
}
