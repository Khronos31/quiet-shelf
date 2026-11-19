/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <dtv/internal/discovery.hpp>
#include <dtv/internal/l3-registry.hpp>
#include <dtv/spi/types.h>

extern "C" void dtv_load_scanfixture(void);
void set_scan_group_case(unsigned value);
namespace dtv::spi {
extern const ModelEntry scan_model_alias_model;
}
extern const dtv::spi::l3_model_record dtv_l3_model_scanfixture_scan_model_alias;

struct dtv_receiver {};

namespace {

dtv::internal::UsbDeviceSnapshot snapshot(std::uint16_t product_id, std::uint64_t token,
                                          std::uint8_t address)
{
    dtv::internal::UsbDeviceSnapshot value{};
    value.vendor_id = 0x1234;
    value.product_id = product_id;
    value.candidate_id = address;
    value.connection_token = token;
    value.bus_number = 1;
    value.device_address = address;
    value.port_path = {1, address};
    dtv::internal::UsbInterfaceSnapshot interface{};
    interface.info.interface_number = 0;
    value.interfaces.push_back(interface);
    return value;
}

dtv::internal::CompleteGroupBatch observe(
    std::vector<dtv::internal::UsbDeviceSnapshot> snapshots)
{
    set_scan_group_case(0);
    dtv::internal::CompleteGroupBatch batch;
    const auto result = dtv::internal::observe_loaded_product_groups(std::move(snapshots), &batch);
    if (result.code != DTV_INTERNAL_RESULT_OK) return {};
    return batch;
}

dtv::internal::CompleteGroupBatch combine(
    dtv::internal::CompleteGroupBatch first, dtv::internal::CompleteGroupBatch second)
{
    const auto offset = static_cast<std::uint32_t>(first.snapshots.size());
    first.snapshots.insert(first.snapshots.end(),
                           std::make_move_iterator(second.snapshots.begin()),
                           std::make_move_iterator(second.snapshots.end()));
    for (auto &group : second.groups) {
        group.anchor_candidate_index += offset;
        for (auto &function : group.functions) function.candidate_index += offset;
        first.groups.push_back(std::move(group));
    }
    return first;
}

dtv::internal::CompleteGroupBatch composite(std::uint64_t second_token)
{
    return observe({snapshot(0x0201, 101, 2), snapshot(0x0202, second_token, 3)});
}

dtv::internal::CompleteGroupBatch other_group(std::uint64_t token = 303)
{
    return observe({snapshot(0x0101, token, 4)});
}

dtv::internal::CompleteGroupBatch all_groups(std::uint64_t second_token)
{
    return combine(composite(second_token), other_group());
}

int registry_contract()
{
    dtv::internal::EnclosureRegistry registry;
    dtv::internal::DiscoverySession session(registry);
    if (session.worker_count_for_test() != 0 || registry.capacity() != DTV_MAX_ENCLOSURES) return 1;
    const auto *slots_address = &registry.live_slots();

    if (session.refresh(all_groups(202)).code != DTV_INTERNAL_RESULT_OK ||
        !registry.scan_complete()) return 2;
    auto *old_root = registry.live_slots()[0];
    auto *other_root = registry.live_slots()[1];
    if (old_root == nullptr || other_root == nullptr || registry.slot_of(old_root) != 0 ||
        registry.slot_of(other_root) != 1) return 3;
    const auto old_children = registry.devices(old_root);
    if (old_children.size() != 2) return 4;
    const auto *first_child = old_children[0];
    const auto *second_child = old_children[1];
    auto *const *receivers = registry.receiver_list(first_child);
    if (receivers == nullptr || receivers[0] == nullptr || receivers[1] == nullptr ||
        receivers[2] == nullptr || receivers[3] != nullptr ||
        registry.receiver_list(second_child) != nullptr) return 16;
    if (registry.enqueue_event(DTV_EVENT_CARD_INSERTED, old_root, nullptr).code !=
            DTV_INTERNAL_RESULT_OK ||
        registry.enqueue_event(DTV_EVENT_CARD_REMOVED, other_root, nullptr).code !=
            DTV_INTERNAL_RESULT_OK) return 15;
    auto receiver_owner = std::make_shared<dtv_receiver>();
    std::weak_ptr<dtv_receiver> receiver_lifetime = receiver_owner;
    auto *receiver = receiver_owner.get();
    if (registry.enqueue_event(DTV_EVENT_LOCK_ACQUIRED, old_root, receiver,
                               receiver_owner).code != DTV_INTERNAL_RESULT_OK ||
        registry.enqueue_event(DTV_EVENT_LOCK_LOST, old_root, receiver,
                               receiver_owner).code != DTV_INTERNAL_RESULT_OK) return 17;
    receiver_owner.reset();
    if (receiver_lifetime.expired()) return 18;

    auto reordered = observe({snapshot(0x0202, 202, 3), snapshot(0x0201, 101, 2)});
    reordered = combine(std::move(reordered), other_group());
    if (session.refresh(std::move(reordered)).code != DTV_INTERNAL_RESULT_OK ||
        &registry.live_slots() != slots_address || registry.live_slots()[0] != old_root ||
        registry.live_slots()[1] != other_root || registry.slot_of(old_root) != 0 ||
        registry.devices(old_root).size() != 2 || registry.devices(old_root)[0] != first_child ||
        registry.devices(old_root)[1] != second_child) return 5;

    auto conflicting = all_groups(202);
    auto duplicate = other_group();
    duplicate.snapshots[0].connection_token = 101;
    conflicting = combine(std::move(conflicting), std::move(duplicate));
    if (session.refresh(std::move(conflicting)).code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT ||
        registry.live_slots()[0] != old_root || registry.live_slots()[1] != other_root) return 6;

    auto invalid = all_groups(202);
    invalid.groups[0].functions[0].candidate_index = 99;
    if (session.refresh(std::move(invalid)).code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT ||
        registry.live_slots()[0] != old_root || registry.live_slots()[1] != other_root) return 7;

    if (!registry.hold(old_root) || !registry.retain_open(old_root)) return 8;
    auto partial = observe({snapshot(0x0201, 101, 2)});
    partial = combine(std::move(partial), other_group());
    if (session.refresh(std::move(partial)).code != DTV_INTERNAL_RESULT_OK ||
        registry.live_slots()[0] != nullptr || registry.live_slots()[1] != other_root ||
        registry.status(old_root) != DTV_ENCLOSURE_DISCONNECTED ||
        registry.devices(old_root).size() != 2 || registry.devices(old_root)[0] != first_child) return 9;

    if (session.refresh(all_groups(404)).code != DTV_INTERNAL_RESULT_OK ||
        registry.live_slots()[0] == nullptr || registry.live_slots()[0] == old_root ||
        registry.live_slots()[1] != other_root || registry.status(old_root) != DTV_ENCLOSURE_DISCONNECTED) {
        return 10;
    }

    const std::uint32_t expected_kinds[]{DTV_EVENT_USB_CONNECTED, DTV_EVENT_USB_CONNECTED,
                                         DTV_EVENT_CARD_INSERTED, DTV_EVENT_CARD_REMOVED,
                                         DTV_EVENT_LOCK_ACQUIRED, DTV_EVENT_LOCK_LOST,
                                         DTV_EVENT_USB_DISCONNECTED, DTV_EVENT_USB_CONNECTED};
    dtv_event event{};
    bool available = false;
    for (std::size_t index = 0; index < std::size(expected_kinds); ++index) {
        if (registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_OK ||
            !available || event.struct_size != sizeof(dtv_event) ||
            event.kind != expected_kinds[index]) return 11;
        if (index == 0 || index == 2 || index == 4 || index == 5 || index == 6) {
            if (event.enclosure != old_root) return 12;
        } else if (index == 3 && event.enclosure != other_root) {
            return 16;
        }
        if ((index == 4 || index == 5) && event.receiver != receiver) return 19;
        if (index == 6 && !receiver_lifetime.expired()) return 20;
    }
    if (!registry.drop(old_root) || registry.next_event(&event, &available).code !=
            DTV_INTERNAL_RESULT_OK || available ||
        registry.status(old_root) != DTV_ENCLOSURE_DISCONNECTED ||
        registry.devices(old_root).size() != 2 || !registry.release_open(old_root) ||
        registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_OK || available ||
        registry.status(old_root) != DTV_ENCLOSURE_UNKNOWN) {
        return 13;
    }

    dtv::internal::EnclosureRegistry ambiguous_registry;
    dtv::internal::DiscoverySession ambiguous_session(ambiguous_registry);
    set_scan_group_case(2);
    dtv::internal::CompleteGroupBatch ambiguous;
    auto input = std::vector<dtv::internal::UsbDeviceSnapshot>{snapshot(0x0101, 707, 7)};
    const auto observed = dtv::internal::observe_loaded_product_groups(std::move(input), &ambiguous);
    if (observed.code != DTV_INTERNAL_RESULT_OK || !ambiguous.groups.empty() ||
        ambiguous_session.refresh(std::move(ambiguous)).code != DTV_INTERNAL_RESULT_OK ||
        std::any_of(ambiguous_registry.live_slots().begin(), ambiguous_registry.live_slots().end(),
                    [](const auto *slot) { return slot != nullptr; })) return 14;
    return 0;
}

int journal_contract()
{
    bool fail_allocation = false;
    dtv::internal::EnclosureRegistry registry([&] {
        if (fail_allocation) throw std::bad_alloc();
    });
    dtv::internal::DiscoverySession session(registry);
    if (session.refresh(other_group()).code != DTV_INTERNAL_RESULT_OK) return 1;
    auto *removed = registry.live_slots()[0];
    if (removed == nullptr || session.refresh({}).code != DTV_INTERNAL_RESULT_OK ||
        registry.status(removed) != DTV_ENCLOSURE_DISCONNECTED) return 2;
    fail_allocation = true;
    auto failed = session.refresh(other_group(404));
    if (failed.code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED || !registry.resource_limited() ||
        registry.scan_complete() || registry.live_slots()[0] != nullptr ||
        registry.devices(removed).size() != 1) return 3;

    dtv_event event{};
    bool available = false;
    if (registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_OK || !available ||
        event.kind != DTV_EVENT_USB_CONNECTED || event.enclosure != removed) return 4;
    if (registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_OK || !available ||
        event.kind != DTV_EVENT_USB_DISCONNECTED || event.enclosure != removed ||
        registry.status(removed) != DTV_ENCLOSURE_DISCONNECTED) return 5;
    for (int index = 0; index < 2; ++index) {
        if (registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
            available) return 6;
    }
    return 0;
}

int unbounded_journal_contract()
{
    dtv::internal::EnclosureRegistry registry;
    dtv::internal::DiscoverySession session(registry);
    const auto cycles = registry.capacity() + 1;
    for (std::size_t index = 0; index < cycles; ++index) {
        if (session.refresh(observe({snapshot(0x0101, 1000 + index, 9)})).code !=
                DTV_INTERNAL_RESULT_OK ||
            session.refresh({}).code != DTV_INTERNAL_RESULT_OK) return 1;
    }
    dtv_event event{};
    bool available = false;
    for (std::size_t index = 0; index < cycles * 2; ++index) {
        if (registry.next_event(&event, &available).code != DTV_INTERNAL_RESULT_OK || !available ||
            event.kind != (index % 2 == 0 ? DTV_EVENT_USB_CONNECTED : DTV_EVENT_USB_DISCONNECTED)) {
            return 2;
        }
    }
    return registry.next_event(&event, &available).code == DTV_INTERNAL_RESULT_OK && !available ? 0 : 3;
}

struct LogCapture {
    std::size_t calls = 0;
    std::uint32_t level = 0;
    std::string message;
};

void capture_log(void *user, std::uint32_t level, const char *message)
{
    auto &capture = *static_cast<LogCapture *>(user);
    ++capture.calls;
    capture.level = level;
    capture.message = message;
}

int capacity_contract()
{
    LogCapture capture;
    dtv_set_log_callback(capture_log, &capture);
    dtv::internal::EnclosureRegistry registry;
    dtv::internal::DiscoverySession session(registry);
    dtv::internal::CompleteGroupBatch batch;
    const auto group_count = registry.capacity() + 1;
    for (std::size_t index = 0; index < group_count; ++index) {
        auto one = other_group(10000 + index);
        one.snapshots[0].candidate_id = index + 1;
        one.snapshots[0].device_address = static_cast<std::uint8_t>(index + 1);
        batch = combine(std::move(batch), std::move(one));
    }
    if (batch.groups.size() != group_count ||
        session.refresh(std::move(batch)).code != DTV_INTERNAL_RESULT_OK ||
        !registry.scan_complete() ||
        std::count_if(registry.live_slots().begin(), registry.live_slots().end(),
                      [](const auto *slot) { return slot != nullptr; }) != registry.capacity()) {
        return 1;
    }
    const auto expected = "omitted 1 complete enclosures; list capacity is " +
                          std::to_string(registry.capacity());
    return capture.calls == 1 && capture.message.find(expected) != std::string::npos ? 0 : 2;
}

int receiver_allocation_failure_contract()
{
    dtv::internal::EnclosureRegistry registry;
    dtv::internal::DiscoverySession session(registry);
    if (session.refresh(other_group(801)).code != DTV_INTERNAL_RESULT_OK) return 1;
    auto *prior = registry.live_slots()[0];
    if (prior == nullptr || registry.status(prior) != DTV_ENCLOSURE_READY) return 2;
    registry.set_receiver_allocation_hook_for_test([] { throw std::bad_alloc(); });
    const auto result = session.refresh(composite(802));
    registry.set_receiver_allocation_hook_for_test({});
    if (result.code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
        registry.live_slots()[0] != prior || registry.status(prior) != DTV_ENCLOSURE_READY ||
        registry.live_slots()[1] != nullptr) return 3;
    return 0;
}

int receiver_known_zero_contract()
{
    dtv::internal::EnclosureRegistry registry;
    dtv::internal::DiscoverySession session(registry);
    const dtv::spi::ModelEntry *const models[]{&dtv::spi::scan_model_alias_model};
    const dtv::spi::ProductEntry product{"scanfixture", models, 1};
    dtv::internal::CompleteGroupBatch batch;
    batch.snapshots.push_back(snapshot(0x0201, 803, 8));
    batch.groups.push_back({&product, &dtv_l3_model_scanfixture_scan_model_alias,
                             0, {{0, 1, 0, 0}}});
    if (session.refresh(std::move(batch)).code != DTV_INTERNAL_RESULT_OK) return 1;
    auto *root = registry.live_slots()[0];
    if (root == nullptr) return 2;
    const auto devices = registry.devices(root);
    if (devices.size() != 1) return 3;
    auto *const *receivers = registry.receiver_list(devices[0]);
    return receivers != nullptr && receivers[0] == nullptr ? 0 : 4;
}

int invalid_receiver_declaration_contract()
{
    const dtv::spi::ModelEntry *const models[]{&dtv::spi::scan_model_alias_model};
    const dtv::spi::ProductEntry product{"scanfixture", models, 1};
    auto reject = [&](const dtv::spi::model_information::device_receiver_information &layout) {
        auto information = dtv::spi::l3_model_record(
            dtv_l3_model_scanfixture_scan_model_alias);
        const auto current = layout;
        const dtv::spi::model_information current_info{
            "Scan Fixture Alias", 0x1234, 0x0201, &current, 1};
        information.information = &current_info;
        dtv::internal::EnclosureRegistry registry;
        dtv::internal::DiscoverySession session(registry);
        dtv::internal::CompleteGroupBatch batch;
        batch.snapshots.push_back(snapshot(0x0201, 804, 8));
        batch.groups.push_back({&product, &information, 0, {{0, 1, 0, 0}}});
        return session.refresh(std::move(batch)).code == DTV_INTERNAL_RESULT_INVALID_ARGUMENT &&
            registry.live_slots()[0] == nullptr;
    };
    const dtv::spi::model_information::receiver_information duplicate_indices[]{
        {0, 0, 1u << DTV_SYSTEM_ISDB_T, 188, 0, 0, 0},
        {0, 1, 1u << DTV_SYSTEM_ISDB_T, 188, 0, 0, 0},
    };
    const dtv::spi::model_information::receiver_information invalid_system[]{
        {0, 0, 1, 188, 0, 0, 0},
    };
    const dtv::spi::model_information::device_receiver_information bad_parent{
        2, true, nullptr, 0};
    const dtv::spi::model_information::device_receiver_information bad_duplicate{
        1, true, duplicate_indices, std::size(duplicate_indices)};
    const dtv::spi::model_information::device_receiver_information bad_systems{
        1, true, invalid_system, std::size(invalid_system)};
    return reject(bad_duplicate) && reject(bad_systems) && reject(bad_parent) ? 0 : 1;
}

}

int main()
{
    dtv_load_scanfixture();
    const auto registry = registry_contract();
    if (registry != 0) return registry;
    const auto journal = journal_contract();
    if (journal != 0) return 20 + journal;
    const auto unbounded = unbounded_journal_contract();
    if (unbounded != 0) return 30 + unbounded;
    const auto capacity = capacity_contract();
    if (capacity != 0) return 40 + capacity;
    const auto receiver_allocation = receiver_allocation_failure_contract();
    if (receiver_allocation != 0) return 50 + receiver_allocation;
    const auto known_zero = receiver_known_zero_contract();
    if (known_zero != 0) return 60 + known_zero;
    const auto invalid_receivers = invalid_receiver_declaration_contract();
    return invalid_receivers == 0 ? 0 : 70 + invalid_receivers;
}
