/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <new>
#include <utility>
#include <vector>

#include <dtv/internal/discovery.hpp>
#include <dtv/internal/l3-registry.hpp>

extern "C" void dtv_load_scanfixture(void);
void set_scan_group_case(unsigned value);

namespace {

dtv::internal::CompleteGroupBatch batch_for(unsigned mode)
{
    if (mode == 1) return {};
    dtv::internal::UsbDeviceSnapshot snapshot{};
    snapshot.vendor_id = 0x1234;
    snapshot.product_id = 0x0101;
    snapshot.candidate_id = 1;
    snapshot.connection_token = mode == 2 ? 202 : mode == 3 ? 303 : 101;
    snapshot.bus_number = 1;
    snapshot.device_address = 7;
    snapshot.port_path = {1, 7};
    dtv::internal::UsbInterfaceSnapshot interface{};
    interface.info.interface_number = 0;
    snapshot.interfaces.push_back(interface);
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    (void)dtv::internal::observe_loaded_product_groups({std::move(snapshot)}, &batch);
    return batch;
}
}

extern "C" void dtv_public_test_batch(unsigned mode)
{
    dtv::internal::set_public_discovery_batch_for_test(batch_for(mode));
}

extern "C" void dtv_public_test_fail_journal(unsigned fail)
{
    if (fail != 0) {
        dtv::internal::set_public_discovery_journal_hook_for_test([] { throw std::bad_alloc(); });
    } else {
        dtv::internal::set_public_discovery_journal_hook_for_test({});
    }
}
