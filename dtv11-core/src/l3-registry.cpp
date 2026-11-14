// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/l3-registry.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <dtv/internal/result.hpp>
#include <dtv/internal/clock.hpp>
#include <dtv/internal/l3.hpp>
#include <dtv/internal/ts-framer.hpp>

#include "usb-transfer-backend.hpp"

struct dtv_enclosure {};
struct dtv_device {};
struct dtv_receiver {};
struct dtv_stream { std::shared_ptr<void> state; };

namespace dtv::internal {
namespace {

std::mutex log_callback_mutex;
dtv_log_callback log_callback = nullptr;
void *log_callback_user = nullptr;
bool log_callback_set = false;
std::atomic<std::uint32_t> next_public_object_id{1};

std::uint32_t allocate_public_object_id()
{
    return next_public_object_id.fetch_add(1, std::memory_order_relaxed);
}

void log_capacity_overflow(std::size_t omitted, std::size_t capacity) noexcept
{
    dtv_log_callback callback = nullptr;
    void *user = nullptr;
    {
        std::lock_guard lock(log_callback_mutex);
        callback = log_callback;
        user = log_callback_user;
    }
    if (callback == nullptr) return;
    char message[160]{};
    std::snprintf(message, sizeof(message),
                  "warning: omitted %zu complete enclosures; list capacity is %zu",
                  omitted, capacity);
    try {
        callback(user, 0, message);
    } catch (...) {
    }
}

struct GroupKey {
    const dtv::spi::ProductEntry *product;
    const dtv::spi::l3_model_record *model;
    std::vector<std::uint64_t> connections;

    bool operator==(const GroupKey &) const = default;
};

struct GroupKeyHash {
    std::size_t operator()(const GroupKey &key) const
    {
        auto value = std::hash<const void *>{}(key.product);
        auto combine = [&value](std::size_t next) {
            value ^= next + std::size_t{0x9e3779b9} + (value << 6) + (value >> 2);
        };
        combine(std::hash<const void *>{}(key.model));
        for (const auto connection : key.connections) {
            combine(std::hash<std::uint64_t>{}(connection));
        }
        return value;
    }
};

struct SelectedFunction {
    std::uint64_t connection;
    dtv_function function;
    std::uint8_t interface_number;
    std::uint8_t alternate_setting;

    bool operator==(const SelectedFunction &) const = default;
};

bool function_less(const SelectedFunction &left, const SelectedFunction &right)
{
    return std::tie(left.connection, left.function, left.interface_number,
                    left.alternate_setting) <
           std::tie(right.connection, right.function, right.interface_number,
                    right.alternate_setting);
}

bool key_less(const GroupKey &left, const GroupKey &right)
{
    if (left.product != right.product) {
        return std::less<const dtv::spi::ProductEntry *>{}(left.product, right.product);
    }
    if (left.model != right.model) {
        return std::less<const dtv::spi::l3_model_record *>{}(left.model, right.model);
    }
    return left.connections < right.connections;
}

dtv_internal_result invalid()
{
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
}

std::string ungrouped_port(const UsbDeviceSnapshot &snapshot)
{
    if (snapshot.port_path.empty()) return {};
    auto port = std::to_string(snapshot.bus_number) + "-";
    for (std::size_t index = 0; index < snapshot.port_path.size(); ++index) {
        if (index != 0) port.push_back('.');
        port += std::to_string(snapshot.port_path[index]);
    }
    return port;
}

}

struct EnclosureRegistry::State {
    struct Device {
        struct Receiver {
            dtv_receiver object;
            std::uint32_t index = 0;
            std::uint32_t local_index = 0;
            std::uint32_t systems = 0;
            std::uint16_t source_stride_bytes = 0;
            std::uint8_t source_sync_offset_bytes = 0;
            std::uint32_t known_operations = 0;
            std::uint32_t supported_operations = 0;
            std::uint32_t ts_scrambling = DTV_TS_UNKNOWN;
            bool lock_known = false;
            std::uint32_t lock_reason = DTV_MEASUREMENT_REASON_NOT_SAMPLED;
            std::uint32_t lock_state = 0;
            bool lock_baseline_known = false;
            std::uint32_t lock_baseline_state = 0;
            std::uint64_t operation_epoch = 0;
        };
        dtv_device object;
        std::uint32_t id = 0;
        std::uint64_t connection;
        SelectedFunction selection;
        UsbDeviceSnapshot snapshot;
        std::uint32_t bus = 0;
        std::uint32_t address = 0;
        std::string port;
        std::vector<std::unique_ptr<Receiver>> receivers;
        std::vector<dtv_receiver *> public_receiver_view;
        bool receiver_count_known = false;
    };

    struct Root {
        dtv_enclosure object;
        std::uint32_t id = 0;
        GroupKey key;
        const dtv::spi::ProductEntry *product;
        const dtv::spi::l3_model_record *model;
        std::uint32_t status = DTV_ENCLOSURE_READY;
        std::size_t slot = DTV_MAX_ENCLOSURES;
        std::size_t hold_references = 0;
        std::size_t open_references = 0;
        std::size_t event_references = 0;
        std::uint32_t card_state = DTV_CARD_UNKNOWN;
        std::uint32_t card_generation = 0;
        dtv_device *card_reader_device = nullptr;
        std::uint64_t card_reader_connection = 0;
        std::mutex card_function_mutex;
        std::uint64_t card_request_epoch = 0;
        std::vector<std::uint8_t> card_atr;
        std::vector<std::unique_ptr<Device>> devices;
        std::vector<dtv_device *> device_view;
        std::vector<dtv_device *> public_device_view;
        std::vector<std::unique_ptr<dtv_candidate>> candidate_storage;
        std::vector<const dtv_candidate *> candidate_view;
    };

    struct UngroupedEntry {
        std::uint64_t connection_token;
        std::string model;
        std::string port;
        std::uint16_t vendor_id;
        std::uint16_t product_id;
        std::uint32_t bus;
        std::uint32_t address;
        std::vector<std::uint8_t> port_path;
        dtv_ungrouped_device object;

        UngroupedEntry(const UsbDeviceSnapshot &snapshot, std::uint64_t token,
                       const char *model_name)
            : connection_token(token),
              model(model_name == nullptr ? "" : model_name),
              port(ungrouped_port(snapshot)),
              vendor_id(snapshot.vendor_id), product_id(snapshot.product_id),
              bus(snapshot.bus_number), address(snapshot.device_address),
              port_path(snapshot.port_path),
              object{sizeof(dtv_ungrouped_device), nullptr,
                     model.empty() ? nullptr : model.c_str(),
                     port.empty() ? nullptr : port.c_str(),
                     vendor_id, product_id, &bus, &address, DTV_UNGROUPED_UNKNOWN}
        {
        }

        bool same_values(const UsbDeviceSnapshot &snapshot, const char *model_name) const
        {
            const std::string_view expected_model = model_name == nullptr ? "" : model_name;
            return vendor_id == snapshot.vendor_id && product_id == snapshot.product_id &&
                   bus == snapshot.bus_number && address == snapshot.device_address &&
                   port_path == snapshot.port_path && model == expected_model;
        }
    };

    struct PreparedDevice {
        std::uint64_t connection;
        SelectedFunction selection;
        UsbDeviceSnapshot snapshot;
    };

    struct PreparedGroup {
        GroupKey key;
        const dtv::spi::ProductEntry *product;
        const dtv::spi::l3_model_record *model;
        std::vector<PreparedDevice> devices;
        std::vector<SelectedFunction> functions;
        std::shared_ptr<Root> root;
        std::size_t slot = DTV_MAX_ENCLOSURES;
        bool existed = false;
    };

    struct QueuedEvent {
        dtv_event value{};
        std::shared_ptr<Root> root;
        std::shared_ptr<void> receiver_owner;
    };

    explicit State(BeforeJournalAllocation hook) : before_journal_allocation(std::move(hook))
    {
        live_view.fill(nullptr);
        token_view.fill(nullptr);
        fd_view.fill(nullptr);
    }

    static constexpr std::size_t kCapacity = DTV_MAX_ENCLOSURES;
    static constexpr std::size_t kEventBatchCapacity = kCapacity * 2;

    std::array<std::shared_ptr<Root>, kCapacity> live_roots{};
    std::array<dtv_enclosure *, kCapacity> live_view{};
    std::array<dtv_enclosure *, kCapacity> token_view{};
    std::array<dtv_enclosure *, kCapacity> fd_view{};
    std::vector<GroupKey> token_selection;
    std::vector<GroupKey> fd_selection;
    std::unordered_map<GroupKey, std::size_t, GroupKeyHash> live_index;
    std::vector<std::shared_ptr<Root>> tombstones;
    std::map<std::uint64_t, std::shared_ptr<UngroupedEntry>> ungrouped_entries;
    std::vector<std::shared_ptr<UngroupedEntry>> retired_ungrouped;
    std::vector<const dtv_ungrouped_device *> ungrouped_view{nullptr};
    std::function<void()> before_ungrouped_allocation;
    std::function<void()> before_token_allocation;
    std::function<void()> before_receiver_allocation;
    std::list<QueuedEvent> events;
    std::optional<QueuedEvent> last_delivered;
    BeforeJournalAllocation before_journal_allocation;
    mutable std::mutex mutex;
    bool complete = false;
    bool resource_limit = false;

    void begin_boundary()
    {
        if (last_delivered) {
            if (last_delivered->root && last_delivered->root->event_references != 0) {
                --last_delivered->root->event_references;
            }
            last_delivered.reset();
        }
        tombstones.erase(std::remove_if(tombstones.begin(), tombstones.end(), [](const auto &root) {
            return root->hold_references == 0 && root->open_references == 0 &&
                   root->event_references == 0;
        }), tombstones.end());
    }

    std::shared_ptr<Root> find_root(const dtv_enclosure *object) const
    {
        if (object == nullptr) return {};
        for (const auto &root : live_roots) {
            if (root && &root->object == object) return root;
        }
        for (const auto &root : tombstones) {
            if (&root->object == object) return root;
        }
        return {};
    }

    Device *find_device(const dtv_device *object) const
    {
        if (object == nullptr) return nullptr;
        const auto find = [object](const auto &root) -> Device * {
            for (const auto &device : root->devices) {
                if (&device->object == object) return device.get();
            }
            return nullptr;
        };
        for (const auto &root : live_roots) {
            if (root) {
                if (auto *device = find(root)) return device;
            }
        }
        for (const auto &root : tombstones) {
            if (auto *device = find(root)) return device;
        }
        return nullptr;
    }

    Device::Receiver *find_receiver(const dtv_receiver *object) const
    {
        if (object == nullptr) return nullptr;
        const auto find = [object](const auto &root) -> Device::Receiver * {
            for (const auto &device : root->devices) {
                for (const auto &receiver : device->receivers) {
                    if (&receiver->object == object) return receiver.get();
                }
            }
            return nullptr;
        };
        for (const auto &root : live_roots) {
            if (root) {
                if (auto *receiver = find(root)) return receiver;
            }
        }
        for (const auto &root : tombstones) {
            if (auto *receiver = find(root)) return receiver;
        }
        return nullptr;
    }

    std::vector<PreparedGroup> prepare_groups(const CompleteGroupBatch &batch) const
    {
        std::vector<PreparedGroup> groups;
        groups.reserve(batch.groups.size());
        std::unordered_map<GroupKey, std::size_t, GroupKeyHash> by_key;
        by_key.reserve(batch.groups.size());
        std::unordered_map<std::uint64_t, std::size_t> token_owner;

        for (const auto &observation : batch.groups) {
            if (observation.product == nullptr || observation.product->product_id == nullptr ||
                observation.model == nullptr || observation.model->discovery == nullptr ||
                observation.anchor_candidate_index >= batch.snapshots.size() ||
                observation.functions.empty()) {
                throw std::invalid_argument("invalid complete group");
            }
            bool model_belongs_to_product = false;
            const auto *product = observation.product;
            if (product->model_count != 0 && product->models == nullptr) {
                throw std::invalid_argument("invalid product models");
            }
            for (std::size_t index = 0; index < product->model_count; ++index) {
                if (product->models[index] == observation.model->discovery) {
                    model_belongs_to_product = true;
                    break;
                }
            }
            if (!model_belongs_to_product) {
                throw std::invalid_argument("model is not in product");
            }

            std::set<dtv_function> functions;
            std::set<std::pair<std::uint64_t, std::uint8_t>> interfaces;
            std::set<std::uint64_t> connections;
            std::map<dtv_function, PreparedDevice> devices_by_function;
            std::vector<SelectedFunction> selected_functions;
            selected_functions.reserve(observation.functions.size());
            for (const auto &selection : observation.functions) {
                if (selection.candidate_index >= batch.snapshots.size() ||
                    !functions.insert(selection.function).second) {
                    throw std::invalid_argument("invalid selected function");
                }
                const auto &snapshot = batch.snapshots[selection.candidate_index];
                if (snapshot.connection_token == 0 ||
                    !interfaces.emplace(snapshot.connection_token, selection.interface_number).second) {
                    throw std::invalid_argument("invalid selected connection");
                }
                std::size_t interface_matches = 0;
                for (const auto &interface : snapshot.interfaces) {
                    if (interface.info.interface_number == selection.interface_number &&
                        interface.info.alternate_setting == selection.alternate_setting) {
                        ++interface_matches;
                    }
                }
                if (interface_matches != 1) {
                    throw std::invalid_argument("selected interface not present");
                }
                const SelectedFunction selected{snapshot.connection_token, selection.function,
                                                selection.interface_number,
                                                selection.alternate_setting};
                selected_functions.push_back(selected);
                connections.insert(snapshot.connection_token);
                devices_by_function.emplace(selection.function,
                                            PreparedDevice{snapshot.connection_token, selected,
                                                           snapshot});
            }
            std::sort(selected_functions.begin(), selected_functions.end(), function_less);
            GroupKey key{observation.product, observation.model, {}};
            key.connections.reserve(connections.size());
            key.connections.assign(connections.begin(), connections.end());
            std::vector<PreparedDevice> devices;
            devices.reserve(devices_by_function.size());
            for (auto &[function, device] : devices_by_function) {
                (void)function;
                devices.push_back(std::move(device));
            }
            const auto duplicate = by_key.find(key);
            if (duplicate != by_key.end()) {
                if (groups[duplicate->second].functions != selected_functions) {
                    throw std::invalid_argument("conflicting duplicate group");
                }
                continue;
            }
            const auto group_index = groups.size();
            for (const auto connection : key.connections) {
                const auto owner = token_owner.find(connection);
                if (owner != token_owner.end() && !(groups[owner->second].key == key)) {
                    throw std::invalid_argument("connection claimed by multiple groups");
                }
                token_owner.emplace(connection, group_index);
            }
            by_key.emplace(key, group_index);
            groups.push_back({std::move(key), observation.product, observation.model,
                              std::move(devices), std::move(selected_functions)});
        }
        std::sort(groups.begin(), groups.end(), [](const auto &left, const auto &right) {
            return key_less(left.key, right.key);
        });
        return groups;
    }

    std::shared_ptr<Root> make_root(PreparedGroup &group) const
    {
        const auto *information = group.model == nullptr ? nullptr : group.model->information;
        std::set<std::uint32_t> declared_functions;
        std::set<std::uint32_t> receiver_indices;
        if (information != nullptr) {
            if (information->device_receiver_count != 0 &&
                information->device_receivers == nullptr)
                throw std::invalid_argument("missing receiver declarations");
            for (std::size_t index = 0; index < information->device_receiver_count; ++index) {
                const auto &declaration = information->device_receivers[index];
                if (!declared_functions.insert(declaration.function).second)
                    throw std::invalid_argument("duplicate receiver declaration");
                if (std::none_of(group.devices.begin(), group.devices.end(), [&](const auto &device) {
                        return device.selection.function == declaration.function;
                    })) {
                    throw std::invalid_argument("receiver declaration parent not selected");
                }
                if (!declaration.receiver_count_known &&
                    (declaration.receivers != nullptr || declaration.receiver_count != 0))
                    throw std::invalid_argument("unknown receiver declaration has values");
                if (declaration.receiver_count_known && declaration.receiver_count != 0 &&
                    declaration.receivers == nullptr)
                    throw std::invalid_argument("receiver declaration values are missing");
                std::set<std::uint32_t> local_indices;
                for (std::size_t receiver_index = 0;
                     receiver_index < declaration.receiver_count; ++receiver_index) {
                    const auto &receiver = declaration.receivers[receiver_index];
                    if (!receiver_indices.insert(receiver.index).second ||
                        !local_indices.insert(receiver.local_index).second ||
                        receiver.systems == 0 || (receiver.systems & ~std::uint32_t{6}) != 0 ||
                        !((receiver.source_stride_bytes == 0 && receiver.source_sync_offset_bytes == 0) ||
                          (receiver.source_stride_bytes == 188 && receiver.source_sync_offset_bytes == 0) ||
                          (receiver.source_stride_bytes == 192 && receiver.source_sync_offset_bytes == 4)) ||
                        (receiver.known_operations & ~std::uint32_t{0x1ff}) != 0 ||
                        (receiver.supported_operations & ~receiver.known_operations) != 0 ||
                        receiver.ts_scrambling > DTV_TS_DESCRAMBLED) {
                        throw std::invalid_argument("invalid receiver metadata");
                    }
                }
            }
        } else if (group.model != nullptr) {
            throw std::invalid_argument("missing model information");
        }
        auto root = std::make_shared<Root>();
        root->id = allocate_public_object_id();
        root->key = group.key;
        root->product = group.product;
        root->model = group.model;
        root->devices.reserve(group.devices.size());
        root->device_view.reserve(group.devices.size());
        for (auto &prepared : group.devices) {
            auto device = std::make_unique<Device>();
            device->id = allocate_public_object_id();
            device->connection = prepared.connection;
            device->selection = prepared.selection;
            device->snapshot = std::move(prepared.snapshot);
            device->bus = device->snapshot.bus_number;
            device->address = device->snapshot.device_address;
            if (!device->snapshot.port_path.empty()) {
                device->port = std::to_string(device->bus) + "-";
                for (std::size_t index = 0; index < device->snapshot.port_path.size(); ++index) {
                    if (index != 0) device->port.push_back('.');
                    device->port += std::to_string(device->snapshot.port_path[index]);
                }
            }
            if (information != nullptr) {
                for (std::size_t declaration_index = 0;
                     declaration_index < information->device_receiver_count; ++declaration_index) {
                    const auto &declaration = information->device_receivers[declaration_index];
                    if (declaration.function != device->selection.function) continue;
                    device->receiver_count_known = declaration.receiver_count_known;
                    if (!declaration.receiver_count_known) break;
                    if (before_receiver_allocation) before_receiver_allocation();
                    device->receivers.reserve(declaration.receiver_count);
                    for (std::size_t receiver_index = 0;
                         receiver_index < declaration.receiver_count; ++receiver_index) {
                        const auto &metadata = declaration.receivers[receiver_index];
                        auto receiver = std::make_unique<Device::Receiver>();
                        receiver->index = metadata.index;
                        receiver->local_index = metadata.local_index;
                        receiver->systems = metadata.systems;
                        receiver->source_stride_bytes = metadata.source_stride_bytes;
                        receiver->source_sync_offset_bytes = metadata.source_sync_offset_bytes;
                        receiver->known_operations = metadata.known_operations;
                        receiver->supported_operations = metadata.supported_operations;
                        receiver->ts_scrambling = metadata.ts_scrambling;
                        device->public_receiver_view.push_back(&receiver->object);
                        device->receivers.push_back(std::move(receiver));
                    }
                    break;
                }
            }
            device->public_receiver_view.push_back(nullptr);
            root->device_view.push_back(&device->object);
            root->devices.push_back(std::move(device));
        }
        root->public_device_view = root->device_view;
        root->public_device_view.push_back(nullptr);
        std::set<std::uint64_t> candidate_connections;
        for (std::size_t index = 0; index < root->devices.size(); ++index) {
            const auto &device = *root->devices[index];
            if (!candidate_connections.insert(device.connection).second) continue;
            root->candidate_storage.push_back(std::make_unique<dtv_candidate>(dtv_candidate{
                sizeof(dtv_candidate), static_cast<std::uint32_t>(root->candidate_storage.size()),
                nullptr, &device.bus, &device.address,
                device.port.empty() ? nullptr : device.port.c_str()}));
        }
        root->candidate_view.reserve(root->candidate_storage.size() + 1);
        for (const auto &candidate : root->candidate_storage) {
            root->candidate_view.push_back(candidate.get());
        }
        root->candidate_view.push_back(nullptr);
        return root;
    }

    dtv_internal_result prepare_events(
        std::span<const QueuedEvent> pending, std::size_t count,
        std::list<QueuedEvent> &prepared)
    {
        try {
            for (std::size_t index = 0; index < count; ++index) {
                if (before_journal_allocation) before_journal_allocation();
                prepared.push_back(pending[index]);
            }
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        } catch (...) {
            const auto result = exception_result();
            if (result.code == DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED) resource_limit = true;
            return result;
        }
    }

    static QueuedEvent make_event(std::uint32_t kind, const std::shared_ptr<Root> &root)
    {
        QueuedEvent event{};
        event.value.struct_size = sizeof(dtv_event);
        event.value.kind = kind;
        event.value.enclosure = &root->object;
        event.value.receiver = nullptr;
        event.root = root;
        return event;
    }

    dtv_internal_result refresh(CompleteGroupBatch batch,
                                const CompleteGroupBatch *token_selection_batch = nullptr,
                                bool fd_selection_batch = false)
    {
        std::unique_lock lock(mutex);
        begin_boundary();
        complete = false;
        if (resource_limit) {
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
        }
        try {
            auto groups = prepare_groups(batch);
            std::unordered_map<GroupKey, std::size_t, GroupKeyHash> requested;
            requested.reserve(groups.size());
            for (std::size_t index = 0; index < groups.size(); ++index) {
                requested.emplace(groups[index].key, index);
            }
            std::vector<GroupKey> next_token_selection = token_selection;
            if (token_selection_batch != nullptr) {
                if (before_token_allocation) before_token_allocation();
                auto selected_groups = prepare_groups(*token_selection_batch);
                next_token_selection.clear();
                next_token_selection.reserve(selected_groups.size());
                for (const auto &group : selected_groups) {
                    if (requested.contains(group.key)) next_token_selection.push_back(group.key);
                }
            }
            std::vector<GroupKey> next_fd_selection = fd_selection;
            if (fd_selection_batch) {
                next_fd_selection.clear();
                next_fd_selection.reserve(groups.size());
                for (const auto &group : groups) next_fd_selection.push_back(group.key);
            }

            auto next_roots = live_roots;
            std::array<std::shared_ptr<Root>, kCapacity> removed{};
            std::array<QueuedEvent, kEventBatchCapacity> pending{};
            std::size_t pending_count = 0;
            std::size_t removed_count = 0;
            std::size_t omitted_count = 0;
            auto has_replaced_location = [&](const std::shared_ptr<Root> &root) {
                if (!fd_selection_batch) return false;
                for (const auto &old_device : root->devices) {
                    const auto &old = old_device->snapshot;
                    for (const auto &group : groups) {
                        for (const auto &new_device : group.devices) {
                            const auto &current = new_device.snapshot;
                            if (old.connection_token != current.connection_token &&
                                old.vendor_id == current.vendor_id &&
                                old.product_id == current.product_id &&
                                old.bus_number == current.bus_number &&
                                old.port_path == current.port_path &&
                                (!old.port_path.empty() || old.device_address == current.device_address)) {
                                return true;
                            }
                        }
                    }
                }
                return false;
            };
            for (std::size_t slot = 0; slot < kCapacity; ++slot) {
                const auto &root = live_roots[slot];
                if (!root || requested.contains(root->key) ||
                    (fd_selection_batch && !has_replaced_location(root))) continue;
                removed[slot] = root;
                ++removed_count;
                next_roots[slot].reset();
                pending[pending_count++] = make_event(DTV_EVENT_USB_DISCONNECTED, root);
            }

            std::unordered_map<GroupKey, std::size_t, GroupKeyHash> next_index;
            next_index.reserve(groups.size());
            if (fd_selection_batch) {
                for (std::size_t slot = 0; slot < live_roots.size(); ++slot) {
                    if (live_roots[slot] && !requested.contains(live_roots[slot]->key)) {
                        next_index.emplace(live_roots[slot]->key, slot);
                    }
                }
            }
            for (auto &group : groups) {
                const auto current = live_index.find(group.key);
                if (current != live_index.end()) {
                    group.existed = true;
                    group.slot = current->second;
                    group.root = live_roots[group.slot];
                    if (group.root->devices.size() != group.devices.size()) {
                        throw std::invalid_argument("changed device functions");
                    }
                    for (std::size_t index = 0; index < group.devices.size(); ++index) {
                        if (group.root->devices[index]->selection.function !=
                            group.devices[index].selection.function) {
                            throw std::invalid_argument("changed device functions");
                        }
                    }
                } else {
                    const auto free_slot = std::find(next_roots.begin(), next_roots.end(), nullptr);
                    if (free_slot == next_roots.end()) {
                        ++omitted_count;
                        continue;
                    }
                    group.slot = static_cast<std::size_t>(free_slot - next_roots.begin());
                    group.root = make_root(group);
                    next_roots[group.slot] = group.root;
                    pending[pending_count++] = make_event(DTV_EVENT_USB_CONNECTED, group.root);
                }
                next_index.emplace(group.key, group.slot);
            }

            std::map<std::uint64_t, std::shared_ptr<UngroupedEntry>> next_ungrouped;
            std::vector<std::shared_ptr<UngroupedEntry>> retired_ungrouped_next;
            std::vector<const dtv_ungrouped_device *> next_ungrouped_view;
            if (!fd_selection_batch) {
                next_ungrouped_view.reserve(batch.ungrouped.size() + 1);
                retired_ungrouped_next.reserve(ungrouped_entries.size());
                for (const auto &observation : batch.ungrouped) {
                    if (observation.snapshot_index >= batch.snapshots.size() ||
                        observation.connection_token == 0) return invalid();
                    const auto &snapshot = batch.snapshots[observation.snapshot_index];
                    if (snapshot.connection_token != observation.connection_token) return invalid();
                    const auto current = ungrouped_entries.find(observation.connection_token);
                    std::shared_ptr<UngroupedEntry> entry;
                    if (current != ungrouped_entries.end() &&
                        current->second->same_values(snapshot, observation.model)) {
                        entry = current->second;
                    } else {
                        if (before_ungrouped_allocation) before_ungrouped_allocation();
                        entry = std::make_shared<UngroupedEntry>(
                            snapshot, observation.connection_token, observation.model);
                        if (current != ungrouped_entries.end()) {
                            retired_ungrouped_next.push_back(current->second);
                        }
                    }
                    if (!next_ungrouped.emplace(observation.connection_token, entry).second) {
                        return invalid();
                    }
                    next_ungrouped_view.push_back(&entry->object);
                }
                for (const auto &[connection, entry] : ungrouped_entries) {
                    if (!next_ungrouped.contains(connection)) retired_ungrouped_next.push_back(entry);
                }
                next_ungrouped_view.push_back(nullptr);
            }
            retired_ungrouped.reserve(retired_ungrouped.size() + retired_ungrouped_next.size());
            tombstones.reserve(tombstones.size() + removed_count);
            std::list<QueuedEvent> prepared_events;
            const auto event_result = prepare_events(pending, pending_count, prepared_events);
            if (event_result.code != DTV_INTERNAL_RESULT_OK) return event_result;

            for (std::size_t slot = 0; slot < kCapacity; ++slot) {
                if (!removed[slot]) continue;
                removed[slot]->status = DTV_ENCLOSURE_DISCONNECTED;
                removed[slot]->slot = kCapacity;
                tombstones.push_back(removed[slot]);
            }
            for (auto &group : groups) {
                if (group.slot == kCapacity) continue;
                if (group.existed) {
                    for (std::size_t index = 0; index < group.devices.size(); ++index) {
                        auto &device = *group.root->devices[index];
                        device.selection = group.devices[index].selection;
                        device.snapshot = std::move(group.devices[index].snapshot);
                    }
                }
                group.root->status = DTV_ENCLOSURE_READY;
                group.root->slot = group.slot;
            }
            live_roots = std::move(next_roots);
            for (std::size_t slot = 0; slot < kCapacity; ++slot) {
                live_view[slot] = live_roots[slot] ? &live_roots[slot]->object : nullptr;
            }
            live_index.swap(next_index);
            token_selection.swap(next_token_selection);
            token_view.fill(nullptr);
            for (const auto &key : token_selection) {
                const auto selected = live_index.find(key);
                if (selected != live_index.end() && selected->second < kCapacity) {
                    token_view[selected->second] = &live_roots[selected->second]->object;
                }
            }
            if (fd_selection_batch) {
                fd_selection.swap(next_fd_selection);
            } else {
                for (auto &entry : retired_ungrouped_next) {
                    retired_ungrouped.push_back(std::move(entry));
                }
                ungrouped_entries.swap(next_ungrouped);
                ungrouped_view.swap(next_ungrouped_view);
            }
            fd_view.fill(nullptr);
            for (const auto &key : fd_selection) {
                const auto selected = live_index.find(key);
                if (selected != live_index.end() && selected->second < kCapacity) {
                    fd_view[selected->second] = &live_roots[selected->second]->object;
                }
            }
            for (const auto &event : prepared_events) {
                if (event.root) ++event.root->event_references;
            }
            events.splice(events.end(), prepared_events);
            complete = true;
            lock.unlock();
            if (omitted_count != 0) log_capacity_overflow(omitted_count, kCapacity);
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        } catch (const std::invalid_argument &) {
            return invalid();
        } catch (...) {
            return exception_result();
        }
    }

    dtv_internal_result next_event(dtv_event *out, bool *available)
    {
        if (out == nullptr || available == nullptr) return invalid();
        std::lock_guard lock(mutex);
        begin_boundary();
        *out = {};
        *available = false;
        if (!events.empty()) {
            last_delivered.emplace(std::move(events.front()));
            events.pop_front();
            *out = last_delivered->value;
            *available = true;
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        if (resource_limit) {
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
        }
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }

    dtv_internal_result enqueue_event(std::uint32_t kind, dtv_enclosure *object,
                                      dtv_receiver *receiver,
                                      std::shared_ptr<void> receiver_owner)
    {
        if (kind < DTV_EVENT_CARD_INSERTED || kind > DTV_EVENT_LOCK_LOST ||
            ((kind == DTV_EVENT_LOCK_ACQUIRED || kind == DTV_EVENT_LOCK_LOST) !=
             (receiver != nullptr)) ||
            ((receiver != nullptr) != (receiver_owner != nullptr)) ||
            (receiver != nullptr && receiver_owner.get() != receiver)) return invalid();
        std::lock_guard lock(mutex);
        if (resource_limit) {
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
        }
        const auto root = find_root(object);
        if (!root) return invalid();
        std::array<QueuedEvent, 1> pending{make_event(kind, root)};
        pending[0].value.receiver = receiver;
        pending[0].receiver_owner = std::move(receiver_owner);
        std::list<QueuedEvent> prepared;
        const auto result = prepare_events(pending, pending.size(), prepared);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        ++root->event_references;
        events.splice(events.end(), prepared);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
};

extern "C" void dtv_set_log_callback(dtv_log_callback callback, void *user)
{
    std::lock_guard lock(log_callback_mutex);
    if (log_callback_set) return;
    log_callback = callback;
    log_callback_user = user;
    log_callback_set = true;
}

EnclosureRegistry::EnclosureRegistry(BeforeJournalAllocation hook)
    : state_(std::make_unique<State>(std::move(hook)))
{
}

EnclosureRegistry::~EnclosureRegistry() = default;

dtv_internal_result EnclosureRegistry::refresh(CompleteGroupBatch batch,
                                               const CompleteGroupBatch *token_selection)
{
    return state_->refresh(std::move(batch), token_selection);
}

dtv_internal_result EnclosureRegistry::refresh_by_fds(CompleteGroupBatch batch)
{
    return state_->refresh(std::move(batch), nullptr, true);
}

const std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &EnclosureRegistry::live_slots() const
{
    return state_->live_view;
}

const dtv_ungrouped_device *const *EnclosureRegistry::ungrouped_view() const
{
    std::lock_guard lock(state_->mutex);
    return state_->ungrouped_view.data();
}

void EnclosureRegistry::project_product_slots(
    const char *product_id, std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &out) const
{
    std::lock_guard lock(state_->mutex);
    for (std::size_t index = 0; index < state_->live_roots.size(); ++index) {
        const auto &root = state_->live_roots[index];
        out[index] = root && root->product && root->product->product_id != nullptr &&
                             std::string_view(root->product->product_id) == product_id
                         ? &root->object
                         : nullptr;
    }
}

const std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &EnclosureRegistry::token_slots() const
{
    return state_->token_view;
}

const std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &EnclosureRegistry::fd_slots() const
{
    return state_->fd_view;
}

void EnclosureRegistry::project_token_product_slots(
    const char *product_id, std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &out) const
{
    std::lock_guard lock(state_->mutex);
    out.fill(nullptr);
    for (const auto &key : state_->token_selection) {
        if (key.product == nullptr || key.product->product_id == nullptr ||
            std::string_view(key.product->product_id) != product_id) continue;
        const auto found = state_->live_index.find(key);
        if (found != state_->live_index.end() && found->second < out.size()) {
            out[found->second] = &state_->live_roots[found->second]->object;
        }
    }
}

void EnclosureRegistry::project_fd_product_slots(
    const char *product_id, std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &out) const
{
    std::lock_guard lock(state_->mutex);
    out.fill(nullptr);
    for (const auto &key : state_->fd_selection) {
        if (key.product == nullptr || key.product->product_id == nullptr ||
            std::string_view(key.product->product_id) != product_id) continue;
        const auto found = state_->live_index.find(key);
        if (found != state_->live_index.end() && found->second < out.size()) {
            out[found->second] = &state_->live_roots[found->second]->object;
        }
    }
}

std::size_t EnclosureRegistry::capacity() const
{
    return DTV_MAX_ENCLOSURES;
}

bool EnclosureRegistry::scan_complete() const
{
    std::lock_guard lock(state_->mutex);
    return state_->complete;
}

void EnclosureRegistry::begin_public_boundary()
{
    std::lock_guard lock(state_->mutex);
    state_->retired_ungrouped.clear();
}

void EnclosureRegistry::set_ungrouped_allocation_hook_for_test(std::function<void()> hook)
{
    std::lock_guard lock(state_->mutex);
    state_->before_ungrouped_allocation = std::move(hook);
}

void EnclosureRegistry::set_token_allocation_hook_for_test(std::function<void()> hook)
{
    std::lock_guard lock(state_->mutex);
    state_->before_token_allocation = std::move(hook);
}

void EnclosureRegistry::set_receiver_allocation_hook_for_test(std::function<void()> hook)
{
    std::lock_guard lock(state_->mutex);
    state_->before_receiver_allocation = std::move(hook);
}

std::size_t EnclosureRegistry::slot_of(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto found = std::find(state_->live_view.begin(), state_->live_view.end(), object);
    return static_cast<std::size_t>(found - state_->live_view.begin());
}

std::uint32_t EnclosureRegistry::status(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root ? root->status : DTV_ENCLOSURE_UNKNOWN;
}

std::uint32_t EnclosureRegistry::card_state(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root ? root->card_state : DTV_CARD_UNKNOWN;
}

std::uint32_t EnclosureRegistry::card_generation(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root ? root->card_generation : 0;
}

dtv_device *EnclosureRegistry::card_reader_device(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root ? root->card_reader_device : nullptr;
}

dtv_internal_result EnclosureRegistry::begin_card_reset(
    dtv_enclosure *object, std::shared_ptr<void> *lease, std::uint64_t *connection_epoch,
    std::uint64_t *request_epoch)
{
    if (lease == nullptr || connection_epoch == nullptr || request_epoch == nullptr)
        return invalid();
    std::shared_ptr<State::Root> root;
    {
        std::lock_guard lock(state_->mutex);
        root = state_->find_root(object);
    }
    if (!root)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    struct CardFunctionLease {
        std::shared_ptr<State::Root> root;
        std::unique_lock<std::mutex> lock;

        explicit CardFunctionLease(std::shared_ptr<State::Root> value)
            : root(std::move(value)), lock(root->card_function_mutex) {}
    };
    std::shared_ptr<CardFunctionLease> held;
    try {
        held = std::make_shared<CardFunctionLease>(root);
    } catch (...) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
    }
    std::lock_guard lock(state_->mutex);
    if (state_->find_root(object) != root || root->status != DTV_ENCLOSURE_READY)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    if (root->card_reader_device == nullptr || root->card_reader_connection == 0)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    root->card_atr.clear();
    root->card_state = DTV_CARD_UNKNOWN;
    ++root->card_request_epoch;
    if (root->card_request_epoch == 0) ++root->card_request_epoch;
    *lease = std::move(held);
    *connection_epoch = root->card_reader_connection;
    *request_epoch = root->card_request_epoch;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result EnclosureRegistry::finish_card_reset(
    dtv_enclosure *object, std::uint64_t connection_epoch, std::uint64_t request_epoch,
    std::span<const std::uint8_t> atr)
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root || root->status != DTV_ENCLOSURE_READY)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    if (root->card_reader_connection != connection_epoch || connection_epoch == 0 ||
        root->card_request_epoch != request_epoch || request_epoch == 0)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    try {
        root->card_atr.assign(atr.begin(), atr.end());
    } catch (...) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
    }
    root->card_state = DTV_CARD_READY;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result EnclosureRegistry::card_atr_view(
    const dtv_enclosure *object, const std::uint8_t **data, std::uint64_t *size) const
{
    if (data == nullptr || size == nullptr) return invalid();
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root || root->status != DTV_ENCLOSURE_READY)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    if (root->card_state != DTV_CARD_READY || root->card_atr.empty())
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    *data = root->card_atr.data();
    *size = root->card_atr.size();
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

std::uint32_t EnclosureRegistry::enclosure_id(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root ? root->id : 0;
}

const char *EnclosureRegistry::product(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root && root->product ? root->product->product_id : nullptr;
}

const char *EnclosureRegistry::model(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root && root->model && root->model->model ? root->model->model->name : nullptr;
}

dtv_device *const *EnclosureRegistry::device_list(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root ? root->public_device_view.data() : nullptr;
}

const dtv_candidate *const *EnclosureRegistry::candidates(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    return root ? root->candidate_view.data() : nullptr;
}

std::span<dtv_device *const> EnclosureRegistry::devices(const dtv_enclosure *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root) return {};
    return root->device_view;
}

std::uint32_t EnclosureRegistry::device_id(const dtv_device *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *device = state_->find_device(object);
    return device ? device->id : 0;
}

dtv_receiver *const *EnclosureRegistry::receiver_list(const dtv_device *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *device = state_->find_device(object);
    return device != nullptr && device->receiver_count_known
        ? device->public_receiver_view.data() : nullptr;
}

dtv_internal_result EnclosureRegistry::update_receiver_declarations(
    dtv_enclosure *object,
    std::span<const dtv::spi::model_information::device_receiver_information> declarations)
{
    if (object == nullptr || declarations.empty()) return invalid();
    struct PendingReceiverList {
        State::Device *device;
        std::vector<std::unique_ptr<State::Device::Receiver>> receivers;
        std::vector<dtv_receiver *> view;
    };
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root || root->status != DTV_ENCLOSURE_READY) return invalid();
    try {
        std::set<std::uint32_t> functions;
        std::set<std::uint32_t> receiver_indices;
        std::set<std::uint32_t> existing_receiver_indices;
        for (const auto &device : root->devices) {
            for (const auto &receiver : device->receivers) {
                existing_receiver_indices.insert(receiver->index);
            }
        }
        std::vector<PendingReceiverList> pending;
        pending.reserve(declarations.size());
        for (const auto &declaration : declarations) {
            if (!functions.insert(declaration.function).second ||
                (!declaration.receiver_count_known &&
                 (declaration.receivers != nullptr || declaration.receiver_count != 0)) ||
                (declaration.receiver_count_known && declaration.receiver_count != 0 &&
                 declaration.receivers == nullptr)) return invalid();
            const auto device = std::find_if(root->devices.begin(), root->devices.end(),
                [&](const auto &entry) {
                    return entry->selection.function == declaration.function;
                });
            if (device == root->devices.end()) return invalid();
            auto &current = **device;
            if (current.receiver_count_known) {
                if (!declaration.receiver_count_known ||
                    current.receivers.size() != declaration.receiver_count) return invalid();
                for (std::size_t index = 0; index < declaration.receiver_count; ++index) {
                    const auto &expected = declaration.receivers[index];
                    const auto &actual = *current.receivers[index];
                    if (expected.index != actual.index || expected.local_index != actual.local_index ||
                        expected.systems != actual.systems ||
                        expected.source_stride_bytes != actual.source_stride_bytes ||
                        expected.source_sync_offset_bytes != actual.source_sync_offset_bytes ||
                        expected.known_operations != actual.known_operations ||
                        expected.supported_operations != actual.supported_operations ||
                        expected.ts_scrambling != actual.ts_scrambling ||
                        !receiver_indices.insert(expected.index).second) return invalid();
                }
                continue;
            }
            if (!declaration.receiver_count_known) continue;
            std::set<std::uint32_t> local_indices;
            for (std::size_t index = 0; index < declaration.receiver_count; ++index) {
                const auto &receiver = declaration.receivers[index];
                if (!receiver_indices.insert(receiver.index).second ||
                    existing_receiver_indices.contains(receiver.index) ||
                    !local_indices.insert(receiver.local_index).second ||
                    receiver.systems == 0 || (receiver.systems & ~std::uint32_t{6}) != 0 ||
                    !((receiver.source_stride_bytes == 0 && receiver.source_sync_offset_bytes == 0) ||
                      (receiver.source_stride_bytes == 188 && receiver.source_sync_offset_bytes == 0) ||
                      (receiver.source_stride_bytes == 192 && receiver.source_sync_offset_bytes == 4)) ||
                    (receiver.known_operations & ~std::uint32_t{0x1ff}) != 0 ||
                    (receiver.supported_operations & ~receiver.known_operations) != 0 ||
                    receiver.ts_scrambling > DTV_TS_DESCRAMBLED) return invalid();
            }
            PendingReceiverList list{&current, {}, {}};
            if (state_->before_receiver_allocation) state_->before_receiver_allocation();
            list.receivers.reserve(declaration.receiver_count);
            list.view.reserve(declaration.receiver_count + 1);
            for (std::size_t index = 0; index < declaration.receiver_count; ++index) {
                const auto &metadata = declaration.receivers[index];
                auto receiver = std::make_unique<State::Device::Receiver>();
                receiver->index = metadata.index;
                receiver->local_index = metadata.local_index;
                receiver->systems = metadata.systems;
                receiver->source_stride_bytes = metadata.source_stride_bytes;
                receiver->source_sync_offset_bytes = metadata.source_sync_offset_bytes;
                receiver->known_operations = metadata.known_operations;
                receiver->supported_operations = metadata.supported_operations;
                receiver->ts_scrambling = metadata.ts_scrambling;
                list.view.push_back(&receiver->object);
                list.receivers.push_back(std::move(receiver));
            }
            list.view.push_back(nullptr);
            pending.push_back(std::move(list));
        }
        for (auto &list : pending) {
            list.device->receivers.swap(list.receivers);
            list.device->public_receiver_view.swap(list.view);
            list.device->receiver_count_known = true;
        }
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    } catch (...) {
        return exception_result();
    }
}

std::uint32_t EnclosureRegistry::receiver_index(const dtv_receiver *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *receiver = state_->find_receiver(object);
    return receiver == nullptr ? 0 : receiver->index;
}

std::uint32_t EnclosureRegistry::receiver_local(const dtv_receiver *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *receiver = state_->find_receiver(object);
    return receiver == nullptr ? 0 : receiver->local_index;
}

std::uint32_t EnclosureRegistry::receiver_systems(const dtv_receiver *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *receiver = state_->find_receiver(object);
    return receiver == nullptr ? 0 : receiver->systems;
}

std::uint32_t EnclosureRegistry::receiver_ts_scrambling(const dtv_receiver *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *receiver = state_->find_receiver(object);
    return receiver == nullptr ? DTV_TS_UNKNOWN : receiver->ts_scrambling;
}

std::int32_t EnclosureRegistry::receiver_support(const dtv_receiver *object,
                                                  std::uint32_t operation,
                                                  std::uint32_t *out,
                                                  dtv_error *error) const
{
    const auto set_error = [error](std::int32_t code) {
        if (error == nullptr) return;
        if (error->struct_size >= offsetof(dtv_error, code) + sizeof(error->code))
            error->code = code;
        if (error->struct_size >= offsetof(dtv_error, cause_code) + sizeof(error->cause_code)) {
            error->cause_domain = DTV_ERROR_DOMAIN_NONE;
            error->cause_code = 0;
        }
    };
    if (out == nullptr || operation < DTV_OP_LNB_POWER || operation > DTV_OP_LNA) {
        set_error(DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(state_->mutex);
    const auto *receiver = state_->find_receiver(object);
    if (receiver == nullptr) {
        set_error(DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    const auto bit = std::uint32_t{1} << (operation - 1);
    if ((receiver->known_operations & bit) == 0) {
        *out = DTV_UNKNOWN;
    } else {
        *out = (receiver->supported_operations & bit) != 0 ? DTV_YES : DTV_NO;
    }
    set_error(DTV_ERROR_OK);
    return DTV_ERROR_OK;
}

dtv_internal_result EnclosureRegistry::receiver_signal_snapshot(
    const dtv_receiver *object, ReceiverSignalSnapshot *out) const
{
    if (object == nullptr || out == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    std::lock_guard lock(state_->mutex);
    const auto *receiver = state_->find_receiver(object);
    if (receiver == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    const auto find_root = [receiver](const auto &roots) -> const State::Root * {
        for (const auto &root : roots) {
            if (!root) continue;
            for (const auto &device : root->devices) {
                for (const auto &entry : device->receivers) {
                    if (entry.get() == receiver) return root.get();
                }
            }
        }
        return nullptr;
    };
    const auto *root = find_root(state_->live_roots);
    if (root == nullptr) root = find_root(state_->tombstones);
    if (root == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    const auto bit = std::uint32_t{1} << (DTV_OP_SIGNAL - 1);
    const auto *device = static_cast<const State::Device *>(nullptr);
    for (const auto &candidate : root->devices) {
        for (const auto &entry : candidate->receivers) {
            if (entry.get() == receiver) {
                device = candidate.get();
                break;
            }
        }
        if (device != nullptr) break;
    }
    if (device == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    ReceiverSignalSnapshot snapshot;
    snapshot.enclosure = const_cast<dtv_enclosure *>(&root->object);
    snapshot.connection_epoch = device->connection;
    snapshot.receiver_index = receiver->index;
    snapshot.source_stride_bytes = receiver->source_stride_bytes;
    snapshot.source_sync_offset_bytes = receiver->source_sync_offset_bytes;
    snapshot.receiver_systems = receiver->systems;
    snapshot.known_operations = receiver->known_operations;
    snapshot.supported_operations = receiver->supported_operations;
    snapshot.operation_epoch = receiver->operation_epoch;
    snapshot.enclosure_status = root->status;
    snapshot.signal_support = (receiver->known_operations & bit) == 0 ? DTV_UNKNOWN
        : ((receiver->supported_operations & bit) != 0 ? DTV_YES : DTV_NO);
    snapshot.lock_known = receiver->lock_known;
    snapshot.lock_reason = receiver->lock_reason;
    snapshot.lock_state = receiver->lock_state;
    *out = snapshot;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result EnclosureRegistry::begin_receiver_tune(
    const dtv_receiver *object, std::uint64_t connection_epoch,
    std::uint64_t previous_operation_epoch, std::uint64_t *operation_epoch)
{
    if (object == nullptr || operation_epoch == nullptr) return invalid();
    std::lock_guard lock(state_->mutex);
    auto *receiver = state_->find_receiver(object);
    if (receiver == nullptr) return invalid();
    const State::Root *root = nullptr;
    const State::Device *device = nullptr;
    const auto find = [receiver](const auto &roots, const State::Root **found_root,
                                  const State::Device **found_device) {
        for (const auto &entry : roots) {
            if (!entry) continue;
            for (const auto &candidate : entry->devices) {
                for (const auto &value : candidate->receivers) {
                    if (value.get() == receiver) {
                        *found_root = entry.get();
                        *found_device = candidate.get();
                        return;
                    }
                }
            }
        }
    };
    find(state_->live_roots, &root, &device);
    if (root == nullptr) find(state_->tombstones, &root, &device);
    if (root == nullptr || device == nullptr) return invalid();
    if (root->status != DTV_ENCLOSURE_READY)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    if (device->connection != connection_epoch ||
        receiver->operation_epoch != previous_operation_epoch)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
    if (receiver->operation_epoch == UINT64_MAX)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
    ++receiver->operation_epoch;
    receiver->lock_known = false;
    receiver->lock_reason = DTV_MEASUREMENT_REASON_TUNE_CHANGED;
    *operation_epoch = receiver->operation_epoch;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}


std::uint16_t EnclosureRegistry::vendor_id(const dtv_device *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *device = state_->find_device(object);
    return device ? device->snapshot.vendor_id : 0;
}

std::uint16_t EnclosureRegistry::product_id(const dtv_device *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *device = state_->find_device(object);
    return device ? device->snapshot.product_id : 0;
}

const std::uint32_t *EnclosureRegistry::bus(const dtv_device *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *device = state_->find_device(object);
    return device ? &device->bus : nullptr;
}

const std::uint32_t *EnclosureRegistry::address(const dtv_device *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *device = state_->find_device(object);
    return device ? &device->address : nullptr;
}

const char *EnclosureRegistry::port(const dtv_device *object) const
{
    std::lock_guard lock(state_->mutex);
    const auto *device = state_->find_device(object);
    return device && !device->port.empty() ? device->port.c_str() : nullptr;
}

bool EnclosureRegistry::hold(dtv_enclosure *object)
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root) return false;
    ++root->hold_references;
    return true;
}

bool EnclosureRegistry::drop(dtv_enclosure *object)
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root || root->hold_references == 0) return false;
    --root->hold_references;
    return true;
}

bool EnclosureRegistry::retain_open(dtv_enclosure *object)
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root) return false;
    ++root->open_references;
    return true;
}

bool EnclosureRegistry::release_open(dtv_enclosure *object)
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root || root->open_references == 0) return false;
    --root->open_references;
    return true;
}

dtv_internal_result EnclosureRegistry::bind_card_reader(dtv_enclosure *object,
                                                         std::uint64_t connection_epoch)
{
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root || root->status != DTV_ENCLOSURE_READY || connection_epoch == 0) return invalid();
    const auto found = std::find_if(root->devices.begin(), root->devices.end(),
        [connection_epoch](const auto &device) {
            return device && device->connection == connection_epoch;
        });
    if (found == root->devices.end()) return invalid();
    auto *const reader = &(*found)->object;
    if (root->card_reader_device != nullptr &&
        (root->card_reader_device != reader || root->card_reader_connection != connection_epoch)) {
        return invalid();
    }
    root->card_reader_device = reader;
    root->card_reader_connection = connection_epoch;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result EnclosureRegistry::observe_card_state(dtv_enclosure *object,
                                                           std::uint64_t connection_epoch,
                                                           std::uint32_t card_state)
{
    if (card_state > DTV_CARD_READY) return invalid();
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root) return invalid();
    if (root->status != DTV_ENCLOSURE_READY) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    }
    if (root->card_reader_device == nullptr ||
        root->card_reader_connection != connection_epoch) return invalid();
    if (root->card_state == card_state) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);

    const auto had_known_state = root->card_state != DTV_CARD_UNKNOWN;
    const auto was_present = root->card_state == DTV_CARD_PRESENT ||
        root->card_state == DTV_CARD_READY;
    const auto is_present = card_state == DTV_CARD_PRESENT || card_state == DTV_CARD_READY;
    std::uint32_t event_kind = 0;
    if (had_known_state && !was_present && is_present) {
        event_kind = DTV_EVENT_CARD_INSERTED;
    } else if (had_known_state && was_present && !is_present && card_state == DTV_CARD_ABSENT) {
        event_kind = DTV_EVENT_CARD_REMOVED;
    }
    root->card_state = card_state;
    if (event_kind == 0) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    ++root->card_generation;
    if (state_->resource_limit) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
    }

    std::array<State::QueuedEvent, 1> pending{State::make_event(event_kind, root)};
    std::list<State::QueuedEvent> prepared;
    const auto result = state_->prepare_events(pending, pending.size(), prepared);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    ++root->event_references;
    state_->events.splice(state_->events.end(), prepared);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result EnclosureRegistry::observe_receiver_lock(
    dtv_enclosure *object, std::uint64_t connection_epoch,
    std::uint64_t operation_epoch, std::uint32_t receiver_index, std::uint32_t lock_state)
{
    if (lock_state > 1) return invalid();
    std::lock_guard lock(state_->mutex);
    const auto root = state_->find_root(object);
    if (!root) return invalid();
    if (root->status != DTV_ENCLOSURE_READY) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    }
    const auto device = std::find_if(root->devices.begin(), root->devices.end(),
        [connection_epoch](const auto &entry) {
            return entry && entry->connection == connection_epoch;
        });
    if (device == root->devices.end()) return invalid();
    const auto receiver = std::find_if((*device)->receivers.begin(), (*device)->receivers.end(),
        [receiver_index](const auto &entry) { return entry->index == receiver_index; });
    if (receiver == (*device)->receivers.end()) return invalid();

    auto &observed = **receiver;
    if (observed.operation_epoch != operation_epoch) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (observed.lock_known && observed.lock_state == lock_state) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    observed.lock_known = true;
    observed.lock_reason = DTV_MEASUREMENT_REASON_NONE;
    observed.lock_state = lock_state;
    if (!observed.lock_baseline_known) {
        observed.lock_baseline_known = true;
        observed.lock_baseline_state = lock_state;
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    if (observed.lock_baseline_state == lock_state) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }

    const auto event_kind = observed.lock_baseline_state == 0
        ? DTV_EVENT_LOCK_ACQUIRED : DTV_EVENT_LOCK_LOST;
    observed.lock_baseline_state = lock_state;
    if (state_->resource_limit) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
    }
    std::array<State::QueuedEvent, 1> pending{State::make_event(event_kind, root)};
    pending[0].value.receiver = &observed.object;
    pending[0].receiver_owner = std::shared_ptr<void>(root, &observed.object);
    std::list<State::QueuedEvent> prepared;
    const auto result = state_->prepare_events(pending, pending.size(), prepared);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    ++root->event_references;
    state_->events.splice(state_->events.end(), prepared);
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result EnclosureRegistry::claim_inputs(
    dtv_enclosure *object, std::vector<UsbClaimDeviceInput> *out) const
{
    if (object == nullptr || out == nullptr) return invalid();
    try {
        std::lock_guard lock(state_->mutex);
        const auto root = state_->find_root(object);
        if (!root) return invalid();
        if (root->status != DTV_ENCLOSURE_READY || root->hold_references == 0 ||
            root->product == nullptr || root->model == nullptr || root->devices.empty()) {
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
        }
        std::vector<UsbClaimDeviceInput> inputs;
        inputs.reserve(root->devices.size());
        for (const auto &device : root->devices) {
            if (!device || device->connection == 0 ||
                device->selection.connection != device->connection ||
                device->snapshot.connection_token != device->connection) return invalid();
            auto input = std::find_if(inputs.begin(), inputs.end(), [&](const auto &entry) {
                return entry.connection_epoch == device->connection;
            });
            if (input == inputs.end()) {
                inputs.push_back({device->connection, device->snapshot, {}});
                input = std::prev(inputs.end());
            } else if (input->snapshot.vendor_id != device->snapshot.vendor_id ||
                       input->snapshot.product_id != device->snapshot.product_id ||
                       input->snapshot.device_version != device->snapshot.device_version ||
                       input->snapshot.bus_number != device->snapshot.bus_number ||
                       input->snapshot.device_address != device->snapshot.device_address) {
                return invalid();
            }
            const auto duplicate = std::find_if(input->interfaces.begin(), input->interfaces.end(),
                [&](const auto &claim) {
                    return claim.interface_number == device->selection.interface_number;
                });
            if (duplicate != input->interfaces.end()) return invalid();
            const auto matches = std::count_if(device->snapshot.interfaces.begin(),
                device->snapshot.interfaces.end(), [&](const auto &interface) {
                    return interface.info.interface_number == device->selection.interface_number &&
                           interface.info.alternate_setting == device->selection.alternate_setting;
                });
            if (matches != 1) return invalid();
            input->interfaces.push_back({device->selection.interface_number,
                                         device->selection.alternate_setting});
        }
        if (inputs.size() != root->key.connections.size()) return invalid();
        std::vector<std::uint64_t> epochs;
        epochs.reserve(inputs.size());
        for (const auto &input : inputs) epochs.push_back(input.connection_epoch);
        std::sort(epochs.begin(), epochs.end());
        if (epochs != root->key.connections) return invalid();
        *out = std::move(inputs);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    } catch (...) {
        return exception_result();
    }
}

dtv_internal_result EnclosureRegistry::open_description(
    dtv_enclosure *object, EnclosureOpenDescription *out) const
{
    if (object == nullptr || out == nullptr) return invalid();
    try {
        std::lock_guard lock(state_->mutex);
        const auto root = state_->find_root(object);
        if (!root) return invalid();
        if (root->status != DTV_ENCLOSURE_READY || root->hold_references == 0 ||
            root->model == nullptr || root->model->discovery == nullptr || root->devices.empty()) {
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
        }
        EnclosureOpenDescription description{};
        description.model = root->model->discovery;
        description.inputs.reserve(root->devices.size());
        description.functions.reserve(root->devices.size());
        for (const auto &device : root->devices) {
            if (!device || device->connection == 0 ||
                device->selection.connection != device->connection ||
                device->snapshot.connection_token != device->connection) return invalid();
            auto input = std::find_if(description.inputs.begin(), description.inputs.end(),
                [&](const auto &entry) { return entry.connection_epoch == device->connection; });
            if (input == description.inputs.end()) {
                description.inputs.push_back({device->connection, device->snapshot, {}});
                input = std::prev(description.inputs.end());
            } else if (input->snapshot.vendor_id != device->snapshot.vendor_id ||
                       input->snapshot.product_id != device->snapshot.product_id ||
                       input->snapshot.device_version != device->snapshot.device_version ||
                       input->snapshot.bus_number != device->snapshot.bus_number ||
                       input->snapshot.device_address != device->snapshot.device_address) {
                return invalid();
            }
            const auto selected_interface = std::count_if(device->snapshot.interfaces.begin(),
                device->snapshot.interfaces.end(), [&](const auto &interface) {
                    return interface.info.interface_number == device->selection.interface_number &&
                           interface.info.alternate_setting == device->selection.alternate_setting;
                });
            if (selected_interface != 1) return invalid();
            if (std::any_of(input->interfaces.begin(), input->interfaces.end(), [&](const auto &claim) {
                    return claim.interface_number == device->selection.interface_number;
                })) return invalid();
            input->interfaces.push_back({device->selection.interface_number,
                                         device->selection.alternate_setting});
            description.functions.push_back(device->selection.function);
        }
        if (description.inputs.size() != root->key.connections.size()) return invalid();
        std::vector<std::uint64_t> epochs;
        epochs.reserve(description.inputs.size());
        for (const auto &input : description.inputs) epochs.push_back(input.connection_epoch);
        std::sort(epochs.begin(), epochs.end());
        if (epochs != root->key.connections) return invalid();
        *out = std::move(description);
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    } catch (...) {
        return exception_result();
    }
}

dtv_internal_result EnclosureRegistry::enqueue_event(std::uint32_t kind,
                                                      dtv_enclosure *object,
                                                      dtv_receiver *receiver,
                                                      std::shared_ptr<void> receiver_owner)
{
    return state_->enqueue_event(kind, object, receiver, std::move(receiver_owner));
}

dtv_internal_result EnclosureRegistry::next_event(dtv_event *out, bool *available)
{
    return state_->next_event(out, available);
}

bool EnclosureRegistry::resource_limited() const
{
    std::lock_guard lock(state_->mutex);
    return state_->resource_limit;
}

dtv_internal_result EnclosureRegistry::scan_failed()
{
    std::lock_guard lock(state_->mutex);
    state_->begin_boundary();
    state_->complete = false;
    if (state_->resource_limit) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

DiscoverySession::DiscoverySession(EnclosureRegistry &registry)
    : context_(UsbContext::scan_only()), registry_(&registry)
{
}

dtv_internal_result DiscoverySession::refresh()
{
    if (registry_->resource_limited()) {
        CompleteGroupBatch empty;
        return registry_->refresh(std::move(empty));
    }
    CompleteGroupBatch batch;
    const auto result = scan_loaded_product_groups(context_, &batch);
    if (result.code != DTV_INTERNAL_RESULT_OK) {
        registry_->scan_failed();
        return result;
    }
    return registry_->refresh(std::move(batch));
}

dtv_internal_result DiscoverySession::refresh(CompleteGroupBatch batch)
{
    return registry_->refresh(std::move(batch));
}

dtv_internal_result DiscoverySession::refresh(CompleteGroupBatch batch,
                                               CompleteGroupBatch selection)
{
    return registry_->refresh(std::move(batch), &selection);
}

dtv_internal_result DiscoverySession::scan_with_token_callback(
    dtv_token_of_callback callback, void *user, CompleteGroupBatch *out)
{
    return scan_loaded_product_groups(context_, out, callback, user);
}

dtv_internal_result DiscoverySession::scan_with_fds(
    std::span<const std::int32_t> descriptors, CompleteGroupBatch *out)
{
    dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    auto snapshots = context_.enumerate_fds(descriptors, &result);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    return observe_loaded_product_groups(std::move(snapshots), out);
}

void DiscoverySession::set_context_for_test(UsbContext context)
{
    context_ = std::move(context);
}

dtv_internal_result DiscoverySession::fail_scan_for_test(dtv_internal_result result)
{
    registry_->scan_failed();
    return result;
}

std::size_t DiscoverySession::worker_count_for_test() const
{
    return UsbTestDeviceFactory::worker_count(context_);
}

dtv_internal_result DiscoverySession::acquire_claims(
    std::span<const UsbClaimDeviceInput> inputs, const dtv_runtime_clock *clock,
    const dtv_cancel *cancel, std::uint64_t deadline, UsbClaimLeaseGroup *out) const
{
    return context_.acquire_claims(inputs, clock, cancel, deadline, out);
}

namespace {

struct PublicStreamState;
bool begin_stream_delivery(const std::shared_ptr<PublicStreamState> &);
bool stream_generation_matches(const std::shared_ptr<PublicStreamState> &, std::uint64_t);
dtv_internal_result deliver_stream_message(const std::shared_ptr<PublicStreamState> &,
                                           const dtv::spi::PublicOpenMessage &);

struct PublicDiscoveryState {
    using ProductView = std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES>;

    struct SharedInOwner {
        struct Reply {
            std::uint64_t scope = 0;
            std::uint64_t transaction = 0;
            std::uint64_t generation = 0;
            bool occupied = false;
            std::size_t size = 0;
            std::vector<unsigned char> bytes;
        };
        struct Waiter {
            std::uint64_t token = 0;
            std::uint64_t scope = 0;
            std::uint64_t transaction = 0;
            std::uint64_t generation = 0;
            bool active = false;
            bool ready = false;
            dtv::spi::PublicOpenWaitResult result = dtv::spi::PublicOpenWaitResult::failed;
            std::size_t size = 0;
            std::vector<unsigned char> bytes;
        };
        struct Tombstone {
            std::uint64_t scope = 0;
            std::uint64_t transaction = 0;
            std::uint64_t generation = 0;
            bool occupied = false;
            std::uint64_t token = 0;
        };
        std::mutex mutex;
        std::condition_variable condition;
        const dtv::spi::PublicOpenOps *ops = nullptr;
        dtv_context *context = nullptr;
        EnclosureRegistry *registry = nullptr;
        dtv_enclosure *enclosure = nullptr;
        std::size_t card_reader_handle_index = (std::numeric_limits<std::size_t>::max)();
        std::vector<std::uint64_t> generations;
        std::vector<std::uint64_t> last_sequences;
        std::vector<std::pair<std::size_t, std::uint8_t>> endpoints;
        std::vector<std::pair<std::size_t, std::uint8_t>> lock_sources;
        std::vector<unsigned char> prestream;
        std::size_t prestream_size = 0;
        bool prestream_claimed = false;
        std::map<std::uint32_t, std::shared_ptr<PublicStreamState>> streams;
        std::uint64_t next_stream_generation = 1;
        std::array<Reply, 16> replies;
        std::array<Waiter, 16> waiters;
        std::array<Tombstone, 16> tombstones;
        std::size_t reply_capacity = 0;
        std::uint64_t next_token = 1;
        bool stopping = false;
        dtv_internal_result terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);

        static dtv_internal_result receive(void *user, const UsbRuntimePacket *raw)
        {
            auto *self = static_cast<SharedInOwner *>(user);
            if (self == nullptr || raw == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            if (raw->terminal != UsbRuntimeTerminal::completed ||
                raw->result.code != DTV_INTERNAL_RESULT_OK) {
                std::lock_guard lock(self->mutex);
                self->terminal = raw->result;
                self->stopping = true;
                self->condition.notify_all();
                return raw->result.code == DTV_INTERNAL_RESULT_OK
                    ? dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0) : raw->result;
            }
            const auto endpoint = std::find(self->endpoints.begin(), self->endpoints.end(),
                std::pair{raw->handle_index, raw->endpoint});
            if (raw->handle_index >= self->generations.size() ||
                raw->generation != self->generations[raw->handle_index] ||
                raw->actual_length > raw->bytes.size() || endpoint == self->endpoints.end()) {
                std::lock_guard lock(self->mutex);
                self->terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0);
                self->stopping = true;
                self->condition.notify_all();
                return self->terminal;
            }
            const auto endpoint_index = static_cast<std::size_t>(endpoint - self->endpoints.begin());
            if (raw->submit_sequence <= self->last_sequences[endpoint_index]) {
                std::lock_guard lock(self->mutex);
                self->terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0);
                self->stopping = true;
                self->condition.notify_all();
                return self->terminal;
            }
            self->last_sequences[endpoint_index] = raw->submit_sequence;
            dtv::spi::PublicOpenPacket packet{raw->handle_index, raw->endpoint, raw->submit_sequence,
                raw->generation, raw->actual_length, raw->bytes.first(raw->actual_length)};
            dtv::spi::PublicOpenMessage message;
            const auto classified = call_callback([&] {
                return self->ops->demux(self->context, &packet, &message);
            });
            if (classified.code != DTV_INTERNAL_RESULT_OK) {
                std::lock_guard lock(self->mutex);
                self->terminal = classified;
                self->stopping = true;
                self->condition.notify_all();
                return classified;
            }
            std::unique_lock lock(self->mutex);
            if (self->stopping) return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
            if (message.kind == dtv::spi::PublicOpenMessageKind::source_chunk ||
                message.kind == dtv::spi::PublicOpenMessageKind::source_end) {
                const auto stream = self->streams.find(message.receiver_index);
                if (stream != self->streams.end() &&
                    stream_generation_matches(stream->second, message.stream_generation)) {
                    auto target = stream->second;
                    if (!begin_stream_delivery(target))
                        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                    lock.unlock();
                    return deliver_stream_message(target, message);
                }
                if (message.kind == dtv::spi::PublicOpenMessageKind::source_end ||
                    self->prestream_claimed)
                    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                if (message.payload.size() > self->prestream.size() - self->prestream_size) {
                    self->terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
                    self->stopping = true;
                    self->condition.notify_all();
                    return self->terminal;
                }
                std::copy(message.payload.begin(), message.payload.end(),
                    self->prestream.begin() + self->prestream_size);
                self->prestream_size += message.payload.size();
            } else if (message.kind == dtv::spi::PublicOpenMessageKind::control_reply) {
                if (message.payload.size() > self->reply_capacity) {
                    self->terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
                    self->stopping = true;
                    self->condition.notify_all();
                    return self->terminal;
                }
                for (const auto &tombstone : self->tombstones) {
                    if (tombstone.occupied && tombstone.scope == message.resource_scope &&
                        tombstone.transaction == message.transaction_id &&
                        tombstone.generation == message.generation)
                        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                }
                for (auto &waiter : self->waiters) {
                    if (waiter.active && waiter.scope == message.resource_scope &&
                        waiter.transaction == message.transaction_id &&
                        waiter.generation == message.generation) {
                        std::copy(message.payload.begin(), message.payload.end(), waiter.bytes.begin());
                        waiter.size = message.payload.size();
                        waiter.ready = true;
                        waiter.result = dtv::spi::PublicOpenWaitResult::completed;
                        self->condition.notify_all();
                        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                    }
                }
                for (auto &reply : self->replies) {
                    if (!reply.occupied || (reply.scope == message.resource_scope &&
                        reply.transaction == message.transaction_id &&
                        reply.generation == message.generation)) {
                        reply.scope = message.resource_scope;
                        reply.transaction = message.transaction_id;
                        reply.generation = message.generation;
                        std::copy(message.payload.begin(), message.payload.end(), reply.bytes.begin());
                        reply.size = message.payload.size();
                        reply.occupied = true;
                        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                    }
                }
                self->terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
                self->stopping = true;
                self->condition.notify_all();
                return self->terminal;
            } else if (message.kind == dtv::spi::PublicOpenMessageKind::notification) {
                if (message.generation != raw->generation || self->registry == nullptr ||
                    self->enclosure == nullptr) {
                    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                }
                dtv_internal_result observed{};
                if (message.notification_kind ==
                    dtv::spi::PublicOpenNotificationKind::card_state) {
                    if (self->card_reader_handle_index != raw->handle_index) {
                        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                    }
                    observed = self->registry->observe_card_state(self->enclosure,
                        self->generations[raw->handle_index], message.card_state);
                } else if (message.notification_kind ==
                           dtv::spi::PublicOpenNotificationKind::receiver_lock) {
                    if (std::find(self->lock_sources.begin(), self->lock_sources.end(),
                            std::pair{raw->handle_index, raw->endpoint}) ==
                        self->lock_sources.end()) {
                        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                    }
                    observed = self->registry->observe_receiver_lock(self->enclosure,
                        self->generations[raw->handle_index], message.operation_epoch,
                        message.receiver_index,
                        message.lock_state);
                } else {
                    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                }
                if (observed.code == DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
                    observed.code == DTV_INTERNAL_RESULT_INVALID_ARGUMENT ||
                    observed.code == DTV_INTERNAL_RESULT_DISCONNECTED) {
                    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                }
                if (observed.code != DTV_INTERNAL_RESULT_OK) {
                    self->terminal = observed;
                    self->stopping = true;
                    self->condition.notify_all();
                    return observed;
                }
            }
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }

        static dtv_internal_result register_waiter(void *user, std::uint64_t scope,
            std::uint64_t transaction, std::uint64_t generation, std::uint64_t *token)
        {
            auto *self = static_cast<SharedInOwner *>(user);
            if (self == nullptr || token == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            std::lock_guard lock(self->mutex);
            if (self->stopping) return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
            if (std::find(self->generations.begin(), self->generations.end(), generation) ==
                self->generations.end())
                return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            for (auto &waiter : self->waiters) {
                if (waiter.active) continue;
                waiter.token = self->next_token++;
                waiter.scope = scope;
                waiter.transaction = transaction;
                waiter.generation = generation;
                waiter.active = true;
                waiter.ready = false;
                waiter.size = 0;
                for (auto &reply : self->replies) {
                    if (reply.occupied && reply.scope == scope && reply.transaction == transaction &&
                        reply.generation == generation) {
                        std::copy_n(reply.bytes.begin(), reply.size, waiter.bytes.begin());
                        waiter.size = reply.size;
                        waiter.ready = true;
                        waiter.result = dtv::spi::PublicOpenWaitResult::completed;
                        reply.occupied = false;
                        break;
                    }
                }
                *token = waiter.token;
                return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
            }
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
        }

        static dtv_internal_result wait_waiter(void *user, std::uint64_t token,
            const std::atomic<bool> *cancel, dtv_timeout timeout, dtv::spi::PublicOpenWaitResult *result,
            std::span<unsigned char> output, std::size_t *size)
        {
            auto *self = static_cast<SharedInOwner *>(user);
            if (self == nullptr || result == nullptr || size == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            std::unique_lock lock(self->mutex);
            auto find = [&]() -> Waiter * {
                for (auto &waiter : self->waiters) if (waiter.active && waiter.token == token) return &waiter;
                return nullptr;
            };
            auto *waiter = find();
            if (waiter == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            const auto deadline = timeout.timeout_ms == 0
                ? (std::chrono::steady_clock::time_point::max)()
                : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout.timeout_ms);
            while (!waiter->ready && !self->stopping) {
                if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
                    waiter->result = dtv::spi::PublicOpenWaitResult::cancelled;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    waiter->result = dtv::spi::PublicOpenWaitResult::deadline;
                    break;
                }
                self->condition.wait_for(lock, std::chrono::milliseconds(2));
            }
            if (self->stopping && !waiter->ready) waiter->result =
                self->terminal.code == DTV_INTERNAL_RESULT_OK ||
                self->terminal.code == DTV_INTERNAL_RESULT_NOT_READY
                    ? dtv::spi::PublicOpenWaitResult::stopped
                    : dtv::spi::PublicOpenWaitResult::failed;
            if (!waiter->ready && (waiter->result == dtv::spi::PublicOpenWaitResult::cancelled ||
                waiter->result == dtv::spi::PublicOpenWaitResult::deadline)) {
                bool saved = false;
                for (auto &tombstone : self->tombstones) {
                    if (tombstone.occupied) continue;
                    tombstone = {waiter->scope, waiter->transaction, waiter->generation, true,
                                 waiter->token};
                    saved = true;
                    break;
                }
                if (!saved) {
                    self->terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
                    self->stopping = true;
                    self->condition.notify_all();
                }
            }
            *result = waiter->result;
            *size = waiter->size;
            if (*size > output.size()) return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
            std::copy_n(waiter->bytes.begin(), *size, output.begin());
            waiter->active = false;
            return self->stopping && !waiter->ready &&
                self->terminal.code != DTV_INTERNAL_RESULT_OK &&
                self->terminal.code != DTV_INTERNAL_RESULT_NOT_READY
                ? self->terminal : dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }

        static dtv_internal_result cancel_waiter(void *user, std::uint64_t token)
        {
            auto *self = static_cast<SharedInOwner *>(user);
            if (self == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            std::lock_guard lock(self->mutex);
            for (auto &waiter : self->waiters) {
                if (waiter.active && waiter.token == token) {
                    if (waiter.ready) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                    bool saved = false;
                    for (auto &tombstone : self->tombstones) {
                        if (tombstone.occupied) continue;
                        tombstone = {waiter.scope, waiter.transaction, waiter.generation, true,
                                     waiter.token};
                        saved = true;
                        break;
                    }
                    if (!saved) {
                        self->terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
                        self->stopping = true;
                        self->condition.notify_all();
                        return self->terminal;
                    }
                    waiter.result = dtv::spi::PublicOpenWaitResult::cancelled;
                    waiter.ready = true;
                    self->condition.notify_all();
                    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                }
            }
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        }

        static dtv_internal_result abandon_waiter(void *user, std::uint64_t token)
        {
            auto *self = static_cast<SharedInOwner *>(user);
            if (self == nullptr) return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            std::lock_guard lock(self->mutex);
            for (auto &waiter : self->waiters) {
                if (!waiter.active || waiter.token != token) continue;
                for (auto &tombstone : self->tombstones) {
                    if (tombstone.occupied && tombstone.token == token) tombstone.occupied = false;
                }
                waiter.active = false;
                waiter.ready = false;
                waiter.size = 0;
                return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
            }
            for (auto &tombstone : self->tombstones) {
                if (tombstone.occupied && tombstone.token == token) {
                    tombstone.occupied = false;
                    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                }
            }
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        }

        static dtv_internal_result get_generation(void *user, std::size_t handle,
                                                   std::uint64_t *generation)
        {
            auto *self = static_cast<SharedInOwner *>(user);
            if (self == nullptr || generation == nullptr || handle >= self->generations.size())
                return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            *generation = self->generations[handle];
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
    };

    struct OpenSlot {
        enum class Phase { opening, open, pending, closed };
        struct OperationControl {
            std::uint64_t id = 0;
            dtv::spi::PublicOperationTargetKind target_kind =
                dtv::spi::PublicOperationTargetKind::enclosure;
            std::uint64_t target_id = 0;
            std::array<std::uint64_t, 8> resource_keys{};
            std::size_t resource_count = 0;
            std::atomic<bool> cancelled{false};
            std::uint64_t waiter_token = 0;
            std::uint64_t output_request = 0;
            bool admitted = false;
            bool request_submitted = false;
            bool output_retired = false;
            bool caller_done = false;
            bool reaping = false;
            bool stream_lease = false;
        };
        std::mutex mutex;
        std::condition_variable condition;
        Phase phase = Phase::opening;
        std::atomic<bool> cancelled{false};
        std::shared_ptr<EnclosureRuntime> runtime;
        std::shared_ptr<dtv::spi::ProductInstance> product_instance;
        const dtv::spi::PublicOpenOps *ops = nullptr;
        std::shared_ptr<SharedInOwner> shared_in;
        std::vector<std::uint64_t> requests;
        std::uint32_t first_error = DTV_ERROR_OK;
        dtv_internal_result first_error_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        std::vector<std::uint64_t> connections;
        std::mutex operation_mutex;
        std::condition_variable operation_condition;
        std::array<std::shared_ptr<OperationControl>, 16> operations{};
        std::uint64_t next_operation_id = 1;
        bool operation_closing = false;
        std::mutex stream_mutex;
        std::vector<std::shared_ptr<PublicStreamState>> streams;
        bool stream_close_pending = false;
        bool held = false;
        bool stop_called = false;
    };

    PublicDiscoveryState()
        : registry([this] {
              if (journal_allocation_hook) journal_allocation_hook();
          }),
          session(registry)
    {
    }

    dtv_internal_result refresh()
    {
        if (scan_error_for_test) return session.fail_scan_for_test(*scan_error_for_test);
        if (synthetic_batch) return session.refresh(*synthetic_batch);
        return session.refresh();
    }

    ProductView &product_view(const char *product_id)
    {
        const auto found = product_views.find(product_id);
        if (found != product_views.end()) return *found->second;
        auto view = std::make_unique<ProductView>();
        view->fill(nullptr);
        auto *result = view.get();
        product_views.emplace(product_id, std::move(view));
        return *result;
    }

    void project_product_views()
    {
        for (auto &[product_id, view] : product_views) {
            registry.project_product_slots(product_id.c_str(), *view);
        }
        for (auto &[product_id, view] : token_product_views) {
            registry.project_token_product_slots(product_id.c_str(), *view);
        }
        for (auto &[product_id, view] : fd_product_views) {
            registry.project_fd_product_slots(product_id.c_str(), *view);
        }
    }

    ProductView &token_product_view(const char *product_id)
    {
        const auto found = token_product_views.find(product_id);
        if (found != token_product_views.end()) return *found->second;
        auto view = std::make_unique<ProductView>();
        view->fill(nullptr);
        auto *result = view.get();
        token_product_views.emplace(product_id, std::move(view));
        return *result;
    }

    ProductView &fd_product_view(const char *product_id)
    {
        const auto found = fd_product_views.find(product_id);
        if (found != fd_product_views.end()) return *found->second;
        auto view = std::make_unique<ProductView>();
        view->fill(nullptr);
        auto *result = view.get();
        fd_product_views.emplace(product_id, std::move(view));
        return *result;
    }

    dtv_internal_result refresh_and_project()
    {
        const auto result = refresh();
        if (result.code == DTV_INTERNAL_RESULT_OK) project_product_views();
        return result;
    }

    std::mutex api_mutex;
    std::mutex open_mutex;
    std::map<dtv_enclosure *, std::shared_ptr<OpenSlot>> open_slots;
    std::unordered_map<std::uint64_t, dtv_enclosure *> claimed_connections;
    std::optional<CompleteGroupBatch> synthetic_batch;
    std::function<void()> journal_allocation_hook;
    std::atomic<bool> operation_allocation_failure_for_test{false};
    std::optional<dtv_internal_result> scan_error_for_test;
    std::map<std::string, std::unique_ptr<ProductView>> product_views;
    std::map<std::string, std::unique_ptr<ProductView>> token_product_views;
    std::map<std::string, std::unique_ptr<ProductView>> fd_product_views;
    dtv_token_of_callback token_callback = nullptr;
    void *token_user = nullptr;
    bool token_callback_set = false;
    bool list_started = false;
    EnclosureRegistry registry;
    DiscoverySession session;
};

struct PublicStreamState {
    std::mutex mutex;
    std::condition_variable condition;
    std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
    std::shared_ptr<PublicDiscoveryState::SharedInOwner> shared_in;
    std::shared_ptr<PublicDiscoveryState::OpenSlot::OperationControl> ticket;
    EnclosureRegistry *registry = nullptr;
    dtv_enclosure *enclosure = nullptr;
    dtv_receiver *receiver = nullptr;
    std::uint32_t receiver_index = 0;
    std::uint64_t connection_epoch = 0;
    std::uint64_t stream_generation = 0;
    std::size_t queue_capacity = 1024 * 1024;
    std::uint16_t source_stride_bytes = 0;
    std::uint8_t source_sync_offset_bytes = 0;
    std::unique_ptr<dtv::internal::TsFramer> framer;
    std::vector<unsigned char> bytes;
    std::size_t active_reads = 0;
    std::size_t pending_callbacks = 0;
    std::uint64_t dropped_bytes = 0;
    std::uint64_t desyncs = 0;
    std::atomic<bool> cancelled{false};
    bool closing = false;
    bool closed = false;
    bool opening = true;
    bool references_held = false;
    bool prepared = false;
    bool producer_started = false;
    bool stop_called = false;
    bool stop_complete = false;
    bool stop_in_progress = false;
    bool after_drain_complete = false;
    bool after_drain_in_progress = false;
    bool terminal = false;
    std::uint32_t terminal_error = DTV_ERROR_OK;
    dtv_internal_result terminal_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    std::uint32_t close_error = DTV_ERROR_OK;
    dtv_internal_result close_error_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
};

int32_t public_error_code(dtv_internal_result);

bool begin_stream_delivery(const std::shared_ptr<PublicStreamState> &state)
{
    std::lock_guard lock(state->mutex);
    if (state->closing || state->closed || state->cancelled.load(std::memory_order_acquire))
        return false;
    ++state->pending_callbacks;
    return true;
}

bool stream_generation_matches(const std::shared_ptr<PublicStreamState> &state,
                               std::uint64_t generation)
{
    return state->stream_generation == generation;
}

dtv::spi::PublicStreamRequest stream_request(const PublicStreamState &state);

dtv_internal_result deliver_stream_message(const std::shared_ptr<PublicStreamState> &state,
                                           const dtv::spi::PublicOpenMessage &message)
{
    dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    try {
        std::lock_guard lock(state->mutex);
        if (!state->closing && !state->closed && !state->terminal &&
            !state->cancelled.load(std::memory_order_acquire)) {
            if (message.kind == dtv::spi::PublicOpenMessageKind::source_chunk) {
                const auto pending_bytes = state->framer->pending_bytes();
                const auto max_size = (std::numeric_limits<std::size_t>::max)();
                if (pending_bytes > max_size - state->source_stride_bytes ||
                    message.payload.size() > max_size - pending_bytes - state->source_stride_bytes) {
                    result = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
                } else {
                    std::vector<unsigned char> aligned(message.payload.size() + pending_bytes +
                        state->source_stride_bytes);
                    const auto framed = state->framer->feed(message.payload, aligned);
                    if (framed.code != dtv::internal::TsFramerCode::ok ||
                        framed.consumed != message.payload.size()) {
                        result = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
                    } else {
                        if (state->source_stride_bytes == 0 ||
                            framed.produced % state->source_stride_bytes != 0) {
                            result = dtv_internal_result_make(DTV_INTERNAL_RESULT_INTERNAL, 0);
                        } else {
                            const auto source_units = framed.produced / state->source_stride_bytes;
                            if (source_units > (std::numeric_limits<std::size_t>::max)() / 188) {
                                result = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
                            } else {
                                std::vector<unsigned char> output(source_units * 188);
                                std::size_t consumed = 0;
                                std::size_t produced = 0;
                                if (framed.produced == 0) {
                                    result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                                } else if (state->source_stride_bytes == 188 &&
                                           state->source_sync_offset_bytes == 0) {
                                    std::copy_n(aligned.begin(), framed.produced, output.begin());
                                    consumed = framed.produced;
                                    produced = output.size();
                                    result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
                                } else if (state->slot->ops->transform_stream != nullptr) {
                                    const auto request = stream_request(*state);
                                    result = dtv::internal::call_callback([&] {
                                        return state->slot->ops->transform_stream(
                                            state->slot->product_instance->context(), &request,
                                            std::span<const unsigned char>(aligned.data(), framed.produced),
                                            output, &consumed, &produced);
                                    });
                                } else {
                                    result = dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
                                }
                                if (result.code == DTV_INTERNAL_RESULT_OK &&
                                    (consumed != framed.produced || produced != output.size() ||
                                     produced % 188 != 0)) {
                                    result = dtv_internal_result_make(DTV_INTERNAL_RESULT_INTERNAL, 0);
                                }
                                if (result.code == DTV_INTERNAL_RESULT_OK) {
                                    for (std::size_t offset = 0; offset < produced; offset += 188) {
                                        if (output[offset] != 0x47) {
                                            result = dtv_internal_result_make(DTV_INTERNAL_RESULT_INTERNAL, 0);
                                            break;
                                        }
                                    }
                                }
                                if (result.code == DTV_INTERNAL_RESULT_OK) {
                                    const auto available = state->queue_capacity - state->bytes.size();
                                    const auto accepted = (std::min)(produced,
                                        available - available % 188);
                                    state->bytes.insert(state->bytes.end(), output.begin(),
                                        output.begin() + accepted);
                                    const auto stats = state->framer->stats();
                                    state->dropped_bytes = stats.dropped_bytes;
                                    state->desyncs = stats.desyncs;
                                    if (accepted != produced) {
                                        state->terminal = true;
                                        state->terminal_error = DTV_ERROR_SLOW_CONSUMER;
                                        state->terminal_result = dtv_internal_result_make(
                                            DTV_INTERNAL_RESULT_OK, 0);
                                    }
                                }
                            }
                        }
                    }
                }
            } else if (message.kind == dtv::spi::PublicOpenMessageKind::source_end) {
                state->terminal = true;
            }
        }
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            state->terminal = true;
            state->terminal_error = public_error_code(result);
            state->terminal_result = result;
        }
        if (state->pending_callbacks != 0) --state->pending_callbacks;
        state->condition.notify_all();
    } catch (...) {
        std::lock_guard lock(state->mutex);
        state->terminal = true;
        state->terminal_error = public_error_code(exception_result());
        state->terminal_result = exception_result();
        if (state->pending_callbacks != 0) --state->pending_callbacks;
        state->condition.notify_all();
        return state->terminal_result;
    }
    return result;
}

PublicDiscoveryState &public_discovery_state()
{
    static PublicDiscoveryState state;
    return state;
}

int32_t public_error_code(dtv_internal_result result)
{
    switch (result.code) {
    case DTV_INTERNAL_RESULT_OK: return DTV_ERROR_OK;
    case DTV_INTERNAL_RESULT_INVALID_ARGUMENT: return DTV_ERROR_INVALID_ARGUMENT;
    case DTV_INTERNAL_RESULT_UNSUPPORTED: return DTV_ERROR_UNSUPPORTED;
    case DTV_INTERNAL_RESULT_USB_IO: return DTV_ERROR_USB_IO;
    case DTV_INTERNAL_RESULT_TIMEOUT: return DTV_ERROR_TIMEOUT;
    case DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED: return DTV_ERROR_RESOURCE_LIMIT;
    case DTV_INTERNAL_RESULT_BUSY: return DTV_ERROR_BUSY;
    case DTV_INTERNAL_RESULT_NOT_READY: return DTV_ERROR_NOT_READY;
    case DTV_INTERNAL_RESULT_DISCONNECTED: return DTV_ERROR_DISCONNECTED;
    case DTV_INTERNAL_RESULT_CANCELLED: return DTV_ERROR_CANCELLED;
    default: return DTV_ERROR_INTERNAL;
    }
}

int32_t public_error_code_from_exception()
{
    return public_error_code(exception_result());
}

int32_t public_libusb_cause(dtv_internal_result result)
{
    if (result.code != DTV_INTERNAL_RESULT_USB_IO && result.code != DTV_INTERNAL_RESULT_TIMEOUT) {
        return 0;
    }
    switch (result.detail) {
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_IO): return LIBUSB_ERROR_IO;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_INVALID_PARAM): return LIBUSB_ERROR_INVALID_PARAM;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_ACCESS): return LIBUSB_ERROR_ACCESS;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_NO_DEVICE): return LIBUSB_ERROR_NO_DEVICE;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_NOT_FOUND): return LIBUSB_ERROR_NOT_FOUND;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_BUSY): return LIBUSB_ERROR_BUSY;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_TIMEOUT): return LIBUSB_ERROR_TIMEOUT;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_OVERFLOW): return LIBUSB_ERROR_OVERFLOW;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_PIPE): return LIBUSB_ERROR_PIPE;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_INTERRUPTED): return LIBUSB_ERROR_INTERRUPTED;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_NO_MEM): return LIBUSB_ERROR_NO_MEM;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_NOT_SUPPORTED): return LIBUSB_ERROR_NOT_SUPPORTED;
    case static_cast<std::uint32_t>(-LIBUSB_ERROR_OTHER): return LIBUSB_ERROR_OTHER;
    default: return 0;
    }
}

void set_public_error(dtv_error *error, int32_t code, const char *message = nullptr,
                      const dtv_internal_result *result = nullptr)
{
    if (error != nullptr && error->struct_size >= offsetof(dtv_error, code) + sizeof(error->code)) {
        error->code = code;
    }
    if (error != nullptr &&
        error->struct_size >= offsetof(dtv_error, cause_code) + sizeof(error->cause_code)) {
        const auto cause_code = result == nullptr ? 0 : public_libusb_cause(*result);
        error->cause_domain = cause_code == 0 ? DTV_ERROR_DOMAIN_NONE : DTV_ERROR_DOMAIN_LIBUSB;
        error->cause_code = cause_code;
    }
    if (error == nullptr || message == nullptr ||
        error->struct_size < offsetof(dtv_error, message_capacity) + sizeof(error->message_capacity) ||
        error->message == nullptr || error->message_capacity == 0) return;
    std::uint64_t index = 0;
    while (index < error->message_capacity - 1 && message[index] != '\0') {
        error->message[index] = message[index];
        ++index;
    }
    error->message[index] = '\0';
}

std::uint64_t deadline_after(std::uint64_t now, std::uint64_t timeout_ms)
{
    if (timeout_ms == 0 || timeout_ms > (UINT64_MAX - now) / UINT64_C(1000000)) {
        return UINT64_MAX;
    }
    return now + timeout_ms * UINT64_C(1000000);
}

dtv_timeout remaining_timeout(const dtv_runtime_clock &clock, std::uint64_t deadline)
{
    if (deadline == UINT64_MAX) return {};
    const auto now = clock.monotonic_now_ns(clock.user);
    const auto remaining = deadline > now ? deadline - now : 0;
    return {remaining / UINT64_C(1000000) +
                (remaining % UINT64_C(1000000) != 0 ? 1 : 0), deadline};
}

bool operation_conflicts(const PublicDiscoveryState::OpenSlot::OperationControl &left,
                         const PublicDiscoveryState::OpenSlot::OperationControl &right);

void reap_retired_operations(const std::shared_ptr<PublicDiscoveryState::OpenSlot> &slot);

dtv::spi::PublicStreamRequest stream_request(const PublicStreamState &state)
{
    return {state.receiver_index, state.connection_epoch, state.stream_generation,
            state.source_stride_bytes, state.source_sync_offset_bytes, state.queue_capacity,
            &state.cancelled};
}

std::shared_ptr<PublicDiscoveryState::OpenSlot::OperationControl> acquire_public_stream_ticket(
    const std::shared_ptr<PublicDiscoveryState::OpenSlot> &slot, std::uint64_t stream_generation,
    std::span<const std::uint64_t> resource_keys, const dtv_runtime_clock &clock,
    std::uint64_t deadline, dtv_internal_result *out_result)
{
    if (out_result == nullptr || resource_keys.size() > 8) {
        if (out_result != nullptr)
            *out_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        return nullptr;
    }
    std::shared_ptr<PublicDiscoveryState::OpenSlot::OperationControl> operation;
    try {
        operation = std::make_shared<PublicDiscoveryState::OpenSlot::OperationControl>();
        operation->target_kind = dtv::spi::PublicOperationTargetKind::stream;
        operation->target_id = stream_generation;
        operation->stream_lease = true;
        operation->resource_count = resource_keys.size();
        std::copy(resource_keys.begin(), resource_keys.end(), operation->resource_keys.begin());
        std::sort(operation->resource_keys.begin(), operation->resource_keys.begin() +
            static_cast<std::ptrdiff_t>(operation->resource_count));
        const auto unique_end = std::unique(operation->resource_keys.begin(),
            operation->resource_keys.begin() + static_cast<std::ptrdiff_t>(operation->resource_count));
        operation->resource_count = static_cast<std::size_t>(unique_end - operation->resource_keys.begin());
        if (std::any_of(operation->resource_keys.begin(), unique_end,
            [](const auto key) { return key == 0; })) {
            *out_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            return nullptr;
        }
    } catch (...) {
        *out_result = exception_result();
        return nullptr;
    }
    {
        std::lock_guard lock(slot->operation_mutex);
        if (slot->operation_closing || slot->next_operation_id == 0) {
            *out_result = dtv_internal_result_make(slot->operation_closing
                ? DTV_INTERNAL_RESULT_NOT_READY : DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
            return nullptr;
        }
        const auto free = std::find(slot->operations.begin(), slot->operations.end(), nullptr);
        if (free == slot->operations.end()) {
            *out_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
            return nullptr;
        }
        operation->id = slot->next_operation_id++;
        *free = operation;
    }
    const auto remove = [&] {
        std::lock_guard lock(slot->operation_mutex);
        operation->caller_done = true;
        for (auto &entry : slot->operations) if (entry == operation) entry.reset();
        slot->operation_condition.notify_all();
    };
    for (;;) {
        reap_retired_operations(slot);
        std::unique_lock lock(slot->operation_mutex);
        if (operation->cancelled.load(std::memory_order_acquire) || slot->operation_closing) {
            lock.unlock();
            remove();
            *out_result = dtv_internal_result_make(operation->cancelled.load(std::memory_order_acquire)
                ? DTV_INTERNAL_RESULT_CANCELLED : DTV_INTERNAL_RESULT_NOT_READY, 0);
            return nullptr;
        }
        bool blocked = false;
        for (const auto &other : slot->operations) {
            if (!other || other == operation || !operation_conflicts(*operation, *other)) continue;
            if (other->admitted || other->id < operation->id) {
                blocked = true;
                break;
            }
        }
        if (!blocked) {
            operation->admitted = true;
            *out_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
            return operation;
        }
        if (deadline != UINT64_MAX && clock.monotonic_now_ns(clock.user) >= deadline) {
            lock.unlock();
            remove();
            *out_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
            return nullptr;
        }
        slot->operation_condition.wait_for(lock, std::chrono::milliseconds(2));
    }
}

void release_public_stream_ticket(const std::shared_ptr<PublicStreamState> &state)
{
    if (!state->ticket) return;
    std::lock_guard lock(state->slot->operation_mutex);
    state->ticket->caller_done = true;
    for (auto &entry : state->slot->operations) if (entry == state->ticket) entry.reset();
    state->slot->operation_condition.notify_all();
    state->ticket.reset();
}

void remove_public_stream_from_slot(const std::shared_ptr<PublicStreamState> &state)
{
    std::lock_guard lock(state->slot->stream_mutex);
    auto &streams = state->slot->streams;
    streams.erase(std::remove(streams.begin(), streams.end(), state), streams.end());
}

void detach_public_stream(const std::shared_ptr<PublicStreamState> &state)
{
    if (!state->shared_in) return;
    std::lock_guard lock(state->shared_in->mutex);
    const auto found = state->shared_in->streams.find(state->receiver_index);
    if (found != state->shared_in->streams.end() && found->second.get() == state.get())
        state->shared_in->streams.erase(found);
}

void release_public_stream_references(const std::shared_ptr<PublicStreamState> &state)
{
    bool held = false;
    {
        std::lock_guard lock(state->mutex);
        held = state->references_held;
        state->references_held = false;
    }
    if (!held) return;
    release_public_stream_ticket(state);
    remove_public_stream_from_slot(state);
    (void)state->registry->release_open(state->enclosure);
    (void)state->registry->drop(state->enclosure);
}

dtv_internal_result close_public_streams(
    const std::shared_ptr<PublicDiscoveryState::OpenSlot> &slot,
    const dtv_runtime_clock &clock, std::uint64_t deadline)
{
    std::vector<std::shared_ptr<PublicStreamState>> streams;
    {
        std::lock_guard lock(slot->stream_mutex);
        streams = slot->streams;
    }
    for (const auto &state : streams) {
        {
            std::unique_lock lock(state->mutex);
            state->cancelled.store(true, std::memory_order_release);
            state->condition.notify_all();
            while (state->opening) {
                if (deadline != UINT64_MAX && clock.monotonic_now_ns(clock.user) >= deadline)
                    return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
                state->condition.wait_for(lock, std::chrono::milliseconds(2));
            }
            if (state->closed) continue;
        }
        dtv_stream stream{};
        stream.state = state;
        const auto timeout = remaining_timeout(clock, deadline);
        const dtv_call_options options{sizeof(dtv_call_options), timeout.timeout_ms};
        dtv_error error{};
        error.struct_size = sizeof(error);
        if (dtv_close_stream(&stream, &options, &error) == DTV_CLOSE_PENDING)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

bool finish_public_stream_close(const std::shared_ptr<PublicStreamState> &state,
                                const dtv_runtime_clock &clock, std::uint64_t deadline)
{
    {
        std::unique_lock lock(state->mutex);
        if (state->closed) return true;
        while (state->active_reads != 0 || state->pending_callbacks != 0) {
            if (deadline != UINT64_MAX && clock.monotonic_now_ns(clock.user) >= deadline)
                return false;
            state->condition.wait_for(lock, std::chrono::milliseconds(2));
        }
        if (!state->stop_complete || state->after_drain_in_progress) return false;
        if (state->after_drain_complete) {
            state->closed = true;
            state->condition.notify_all();
            lock.unlock();
            release_public_stream_references(state);
            return true;
        }
        state->after_drain_in_progress = true;
    }
    const auto request = stream_request(*state);
    const auto result = call_callback([&] {
        return state->slot->ops->after_stream_drain(
            state->slot->product_instance->context(), &request);
    });
    {
        std::lock_guard lock(state->mutex);
        state->after_drain_in_progress = false;
        if (result.code == DTV_INTERNAL_RESULT_TIMEOUT) {
            if (state->close_error == DTV_ERROR_OK) {
                state->close_error = public_error_code(result);
                state->close_error_result = result;
            }
            state->condition.notify_all();
            return false;
        }
        state->after_drain_complete = true;
        if (result.code != DTV_INTERNAL_RESULT_OK && state->close_error == DTV_ERROR_OK) {
            state->close_error = public_error_code(result);
            state->close_error_result = result;
        }
        state->closed = true;
        state->condition.notify_all();
    }
    release_public_stream_references(state);
    return true;
}

bool has_bulk_in_endpoint(const UsbClaimDeviceInput &input, std::uint8_t endpoint)
{
    if ((endpoint & 0x80u) == 0) return false;
    for (const auto &claim : input.interfaces) {
        for (const auto &interface : input.snapshot.interfaces) {
            if (interface.info.interface_number != claim.interface_number ||
                interface.info.alternate_setting != claim.alternate_setting) continue;
            if (std::any_of(interface.endpoints.begin(), interface.endpoints.end(),
                [endpoint](const auto &value) {
                    return value.address == endpoint && (value.attributes & 0x03u) == 0x02u;
                })) return true;
        }
    }
    return false;
}

bool has_bulk_out_endpoint(const UsbClaimDeviceInput &input, std::uint8_t endpoint)
{
    if ((endpoint & 0x80u) != 0) return false;
    for (const auto &claim : input.interfaces) {
        for (const auto &interface : input.snapshot.interfaces) {
            if (interface.info.interface_number != claim.interface_number ||
                interface.info.alternate_setting != claim.alternate_setting) continue;
            if (std::any_of(interface.endpoints.begin(), interface.endpoints.end(),
                [endpoint](const auto &value) {
                    return value.address == endpoint && (value.attributes & 0x03u) == 0x02u;
                })) return true;
        }
    }
    return false;
}

dtv_internal_result stop_open_product(
    const std::shared_ptr<PublicDiscoveryState::OpenSlot> &slot,
    const dtv_runtime_clock &clock, std::uint64_t deadline)
{
    std::shared_ptr<dtv::spi::ProductInstance> instance;
    const dtv::spi::PublicOpenOps *ops = nullptr;
    {
        std::lock_guard lock(slot->mutex);
        if (slot->stop_called) return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        slot->stop_called = true;
        instance = slot->product_instance;
        ops = slot->ops;
    }
    if (!instance || ops == nullptr || ops->stop == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    return call_callback([&] {
        return ops->stop(instance->context(), remaining_timeout(clock, deadline));
    });
}

bool operation_targets_conflict(const PublicDiscoveryState::OpenSlot::OperationControl &left,
                                const PublicDiscoveryState::OpenSlot::OperationControl &right)
{
    if (left.target_kind == dtv::spi::PublicOperationTargetKind::enclosure ||
        right.target_kind == dtv::spi::PublicOperationTargetKind::enclosure) return true;
    return left.target_kind == right.target_kind && left.target_id == right.target_id;
}

bool operation_resources_conflict(
    const PublicDiscoveryState::OpenSlot::OperationControl &left,
    const PublicDiscoveryState::OpenSlot::OperationControl &right)
{
    for (std::size_t left_index = 0; left_index < left.resource_count; ++left_index) {
        for (std::size_t right_index = 0; right_index < right.resource_count; ++right_index) {
            if (left.resource_keys[left_index] == right.resource_keys[right_index]) return true;
        }
    }
    return false;
}

bool operation_conflicts(const PublicDiscoveryState::OpenSlot::OperationControl &left,
                         const PublicDiscoveryState::OpenSlot::OperationControl &right)
{
    if ((left.stream_lease || right.stream_lease) &&
        left.target_kind == dtv::spi::PublicOperationTargetKind::stream &&
        right.target_kind == dtv::spi::PublicOperationTargetKind::stream &&
        left.target_id == right.target_id) return false;
    return operation_targets_conflict(left, right) || operation_resources_conflict(left, right);
}

struct PublicTuneEffectContext {
    EnclosureRegistry *registry = nullptr;
    std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
    std::shared_ptr<PublicDiscoveryState::OpenSlot::OperationControl> operation;
    const dtv_runtime_clock *clock = nullptr;
    const dtv_receiver *receiver = nullptr;
    std::uint64_t connection_epoch = 0;
    std::uint64_t previous_operation_epoch = 0;
    std::uint64_t deadline = UINT64_MAX;
    std::uint64_t operation_epoch = 0;
    bool effect_started = false;
};

dtv_internal_result begin_public_tune_effect(void *user, std::uint64_t *operation_epoch)
{
    auto *context = static_cast<PublicTuneEffectContext *>(user);
    if (context == nullptr || operation_epoch == nullptr || context->registry == nullptr ||
        !context->slot || !context->operation || context->clock == nullptr ||
        context->clock->monotonic_now_ns == nullptr || context->effect_started)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    if (context->operation->cancelled.load(std::memory_order_acquire))
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
    if (context->deadline != UINT64_MAX &&
        context->clock->monotonic_now_ns(context->clock->user) >= context->deadline)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
    {
        std::lock_guard lock(context->slot->mutex);
        if (context->slot->phase != PublicDiscoveryState::OpenSlot::Phase::open)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    }
    const auto result = context->registry->begin_receiver_tune(context->receiver,
        context->connection_epoch, context->previous_operation_epoch, &context->operation_epoch);
    if (result.code == DTV_INTERNAL_RESULT_OK) {
        context->effect_started = true;
        *operation_epoch = context->operation_epoch;
    }
    return result;
}

void reap_retired_operations(const std::shared_ptr<PublicDiscoveryState::OpenSlot> &slot)
{
    std::shared_ptr<EnclosureRuntime> runtime;
    std::shared_ptr<PublicDiscoveryState::SharedInOwner> shared_in;
    {
        std::lock_guard lock(slot->mutex);
        runtime = slot->runtime;
        shared_in = slot->shared_in;
    }
    if (!runtime) return;
    std::array<std::shared_ptr<PublicDiscoveryState::OpenSlot::OperationControl>, 16> candidates{};
    std::size_t count = 0;
    {
        std::lock_guard lock(slot->operation_mutex);
        for (const auto &operation : slot->operations) {
            if (!operation || !operation->caller_done || !operation->request_submitted ||
                operation->output_retired || operation->reaping) continue;
            operation->reaping = true;
            candidates[count++] = operation;
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        auto &operation = candidates[index];
        UsbRuntimeCompletion completion;
        bool available = false;
        const auto result = runtime->take_completion(operation->output_request, &completion, &available);
        if (result.code == DTV_INTERNAL_RESULT_OK && available && !completion.reached_backend &&
            shared_in != nullptr && operation->waiter_token != 0) {
            (void)PublicDiscoveryState::SharedInOwner::abandon_waiter(
                shared_in.get(), operation->waiter_token);
        }
        std::lock_guard lock(slot->operation_mutex);
        operation->reaping = false;
        if (result.code == DTV_INTERNAL_RESULT_OK && available) operation->output_retired = true;
        if (operation->output_retired) {
            for (auto &entry : slot->operations) {
                if (entry == operation) {
                    entry.reset();
                    break;
                }
            }
        }
        slot->operation_condition.notify_all();
    }
}

void cancel_open_operations(const std::shared_ptr<PublicDiscoveryState::OpenSlot> &slot,
                            bool closing)
{
    std::array<std::uint64_t, 16> waiter_tokens{};
    std::array<std::uint64_t, 16> output_requests{};
    std::size_t count = 0;
    {
        std::lock_guard lock(slot->operation_mutex);
        if (closing) slot->operation_closing = true;
        for (const auto &operation : slot->operations) {
            if (!operation) continue;
            operation->cancelled.store(true, std::memory_order_release);
            waiter_tokens[count] = operation->waiter_token;
            output_requests[count] = operation->request_submitted && !operation->output_retired
                ? operation->output_request : 0;
            ++count;
        }
        slot->operation_condition.notify_all();
    }
    std::shared_ptr<EnclosureRuntime> runtime;
    std::shared_ptr<PublicDiscoveryState::SharedInOwner> shared_in;
    {
        std::lock_guard lock(slot->mutex);
        runtime = slot->runtime;
        shared_in = slot->shared_in;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (waiter_tokens[index] != 0 && shared_in != nullptr) {
            (void)PublicDiscoveryState::SharedInOwner::cancel_waiter(
                shared_in.get(), waiter_tokens[index]);
        }
        if (output_requests[index] != 0 && runtime != nullptr) {
            (void)runtime->cancel(output_requests[index]);
        }
    }
}

void cancel_public_stream_operations(const std::shared_ptr<PublicStreamState> &stream)
{
    if (!stream || !stream->slot) return;
    std::array<std::uint64_t, 16> waiter_tokens{};
    std::array<std::uint64_t, 16> output_requests{};
    std::size_t count = 0;
    {
        std::lock_guard lock(stream->slot->operation_mutex);
        for (const auto &operation : stream->slot->operations) {
            if (!operation || operation->stream_lease || operation->target_kind !=
                    dtv::spi::PublicOperationTargetKind::stream ||
                operation->target_id != stream->stream_generation) continue;
            operation->cancelled.store(true, std::memory_order_release);
            waiter_tokens[count] = operation->waiter_token;
            output_requests[count] = operation->request_submitted && !operation->output_retired
                ? operation->output_request : 0;
            ++count;
        }
        stream->slot->operation_condition.notify_all();
    }
    std::shared_ptr<EnclosureRuntime> runtime;
    std::shared_ptr<PublicDiscoveryState::SharedInOwner> shared_in;
    {
        std::lock_guard lock(stream->slot->mutex);
        runtime = stream->slot->runtime;
        shared_in = stream->slot->shared_in;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (waiter_tokens[index] != 0 && shared_in != nullptr) {
            (void)PublicDiscoveryState::SharedInOwner::cancel_waiter(
                shared_in.get(), waiter_tokens[index]);
        }
        if (output_requests[index] != 0 && runtime != nullptr) {
            (void)runtime->cancel(output_requests[index]);
        }
    }
}

bool wait_for_operation_callers(
    const std::shared_ptr<PublicDiscoveryState::OpenSlot> &slot,
    const dtv_runtime_clock &clock, std::uint64_t deadline)
{
    std::unique_lock lock(slot->operation_mutex);
    while (std::any_of(slot->operations.begin(), slot->operations.end(), [](const auto &operation) {
        return operation && !operation->caller_done;
    })) {
        if (deadline != UINT64_MAX && clock.monotonic_now_ns(clock.user) >= deadline) return false;
        slot->operation_condition.wait_for(lock, std::chrono::milliseconds(2));
    }
    return true;
}

void clear_operation_tickets(const std::shared_ptr<PublicDiscoveryState::OpenSlot> &slot)
{
    std::lock_guard lock(slot->operation_mutex);
    for (auto &operation : slot->operations) operation.reset();
    slot->operation_condition.notify_all();
}

}

extern "C" void dtv_set_token_of(dtv_token_of_callback callback, void *user)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    if (state.token_callback_set || state.list_started) return;
    state.token_callback = callback;
    state.token_user = user;
    state.token_callback_set = true;
}

void set_public_discovery_batch_for_test(CompleteGroupBatch batch)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    state.synthetic_batch = std::move(batch);
}

void set_public_discovery_context_for_test(UsbContext context, CompleteGroupBatch batch)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    state.session.set_context_for_test(std::move(context));
    state.synthetic_batch = std::move(batch);
}

void set_public_discovery_journal_hook_for_test(std::function<void()> hook)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    state.journal_allocation_hook = std::move(hook);
}

void set_public_discovery_scan_error_for_test(dtv_internal_result result)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    if (result.code == DTV_INTERNAL_RESULT_OK) {
        state.scan_error_for_test.reset();
    } else {
        state.scan_error_for_test = result;
    }
}

void set_public_discovery_ungrouped_allocation_hook_for_test(std::function<void()> hook)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    state.registry.set_ungrouped_allocation_hook_for_test(std::move(hook));
}

void set_public_discovery_usb_backend_for_test(std::shared_ptr<UsbTransferBackend> backend)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    state.session.set_context_for_test(UsbContext::scan_only(std::move(backend)));
}

void set_public_discovery_token_allocation_hook_for_test(std::function<void()> hook)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    state.registry.set_token_allocation_hook_for_test(std::move(hook));
}

void set_public_discovery_receiver_allocation_hook_for_test(std::function<void()> hook)
{
    auto &state = public_discovery_state();
    std::lock_guard lock(state.api_mutex);
    state.registry.set_receiver_allocation_hook_for_test(std::move(hook));
}

dtv_internal_result update_public_discovery_receiver_declarations_for_test(
    dtv_enclosure *root,
    std::span<const dtv::spi::model_information::device_receiver_information> declarations)
{
    return public_discovery_state().registry.update_receiver_declarations(root, declarations);
}

void set_public_operation_allocation_failure_for_test(bool fail)
{
    public_discovery_state().operation_allocation_failure_for_test.store(fail,
        std::memory_order_release);
}

dtv_internal_result run_public_operation_for_test(
    dtv_enclosure *object, const dtv::spi::PublicOperationRequest &request,
    dtv_timeout timeout, std::span<unsigned char> reply, std::size_t *reply_size)
{
    if (object == nullptr || reply_size == nullptr ||
        (!request.arguments.empty() && request.arguments.data() == nullptr))
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    *reply_size = 0;
    auto &state = public_discovery_state();
    const auto enclosure_status = state.registry.status(object);
    if (enclosure_status == DTV_ENCLOSURE_DISCONNECTED)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    if (enclosure_status != DTV_ENCLOSURE_READY)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);

    std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
    {
        std::lock_guard lock(state.open_mutex);
        const auto found = state.open_slots.find(object);
        if (found == state.open_slots.end())
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
        slot = found->second;
    }
    std::shared_ptr<dtv::spi::ProductInstance> instance;
    std::shared_ptr<PublicDiscoveryState::SharedInOwner> shared_in;
    std::shared_ptr<EnclosureRuntime> runtime;
    {
        std::lock_guard lock(slot->mutex);
        if (slot->phase != PublicDiscoveryState::OpenSlot::Phase::open)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
        instance = slot->product_instance;
        shared_in = slot->shared_in;
        runtime = slot->runtime;
    }
    if (!instance || !runtime || !shared_in || slot->ops == nullptr ||
        slot->ops->plan_operation == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);

    dtv::spi::PublicOperationPlan plan{};
    auto result = call_callback([&] {
        return slot->ops->plan_operation(instance->context(), &request, &plan);
    });
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if (!plan.supported) return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
    if (!plan.ready) return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    if (!plan.arguments_valid ||
        static_cast<std::uint32_t>(request.target_kind) >
            static_cast<std::uint32_t>(dtv::spi::PublicOperationTargetKind::stream))
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);

    EnclosureOpenDescription description{};
    result = state.registry.open_description(object, &description);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if (plan.handle_index >= description.inputs.size() || plan.bulk_out_endpoint == 0x00 ||
        plan.resource_scope == 0 || plan.transaction_id == 0 || plan.reply_capacity == 0 ||
        plan.reply_capacity > 1024 * 1024 || plan.resource_keys.size() > 8 ||
        plan.output.empty() || plan.output.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        !has_bulk_out_endpoint(description.inputs[plan.handle_index], plan.bulk_out_endpoint))
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    if (reply.size() < plan.reply_capacity)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);

    auto *clock = system_runtime_clock();
    if (clock == nullptr || clock->monotonic_now_ns == nullptr)
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INTERNAL, 0);
    auto deadline = deadline_after(clock->monotonic_now_ns(clock->user), timeout.timeout_ms);
    if (timeout.operation_deadline_ns != 0)
        deadline = (std::min)(deadline, timeout.operation_deadline_ns);

    std::shared_ptr<PublicDiscoveryState::OpenSlot::OperationControl> operation;
    std::vector<unsigned char> reply_buffer;
    try {
        if (state.operation_allocation_failure_for_test.exchange(false,
                std::memory_order_acq_rel)) throw std::bad_alloc();
        operation = std::make_shared<PublicDiscoveryState::OpenSlot::OperationControl>();
        reply_buffer.resize(plan.reply_capacity);
        operation->target_kind = request.target_kind;
        operation->target_id = request.target_id;
        operation->resource_count = plan.resource_keys.size();
        std::copy(plan.resource_keys.begin(), plan.resource_keys.end(), operation->resource_keys.begin());
        std::sort(operation->resource_keys.begin(),
                  operation->resource_keys.begin() + static_cast<std::ptrdiff_t>(operation->resource_count));
        const auto unique_end = std::unique(operation->resource_keys.begin(),
            operation->resource_keys.begin() + static_cast<std::ptrdiff_t>(operation->resource_count));
        operation->resource_count = static_cast<std::size_t>(unique_end - operation->resource_keys.begin());
        if (std::any_of(operation->resource_keys.begin(), unique_end,
            [](const auto key) { return key == 0; }))
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    } catch (...) {
        return exception_result();
    }
    reap_retired_operations(slot);
    {
        std::lock_guard lock(slot->operation_mutex);
        if (slot->operation_closing) return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
        const auto free = std::find(slot->operations.begin(), slot->operations.end(), nullptr);
        if (free == slot->operations.end() || slot->next_operation_id == 0)
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
        operation->id = slot->next_operation_id++;
        *free = operation;
    }

    auto erase_operation = [&] {
        std::lock_guard lock(slot->operation_mutex);
        operation->caller_done = true;
        if (!operation->request_submitted || operation->output_retired) {
            for (auto &entry : slot->operations) if (entry == operation) entry.reset();
        }
        slot->operation_condition.notify_all();
    };
    for (;;) {
        reap_retired_operations(slot);
        std::unique_lock lock(slot->operation_mutex);
        if (operation->cancelled.load(std::memory_order_acquire)) {
            lock.unlock();
            erase_operation();
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
        }
        if (slot->operation_closing) {
            lock.unlock();
            erase_operation();
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
        }
        bool blocked = false;
        for (const auto &other : slot->operations) {
            if (!other || other == operation || !operation_conflicts(*operation, *other)) continue;
            if (other->admitted || other->id < operation->id) {
                blocked = true;
                break;
            }
        }
        if (!blocked) {
            operation->admitted = true;
            break;
        }
        if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
            lock.unlock();
            erase_operation();
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
        }
        slot->operation_condition.wait_for(lock, std::chrono::milliseconds(2));
    }

    if (state.registry.status(object) == DTV_ENCLOSURE_DISCONNECTED) {
        erase_operation();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    }

    dtv::spi::PublicOpenSession session{shared_in.get(),
        PublicDiscoveryState::SharedInOwner::register_waiter,
        PublicDiscoveryState::SharedInOwner::wait_waiter,
        PublicDiscoveryState::SharedInOwner::cancel_waiter,
        PublicDiscoveryState::SharedInOwner::get_generation,
        PublicDiscoveryState::SharedInOwner::abandon_waiter};
    std::uint64_t generation = 0;
    result = session.generation_for_handle(session.user, plan.handle_index, &generation);
    if (result.code != DTV_INTERNAL_RESULT_OK) {
        erase_operation();
        return result;
    }
    std::uint64_t waiter_token = 0;
    result = session.register_reply(session.user, plan.resource_scope, plan.transaction_id,
                                    generation, &waiter_token);
    if (result.code != DTV_INTERNAL_RESULT_OK) {
        erase_operation();
        return result;
    }
    {
        std::lock_guard lock(slot->operation_mutex);
        operation->waiter_token = waiter_token;
    }
    auto abandon_waiter = [&] {
        if (waiter_token != 0) (void)session.abandon_reply(session.user, waiter_token);
    };
    auto cancel_and_consume_waiter = [&] {
        (void)session.cancel_reply(session.user, waiter_token);
        dtv::spi::PublicOpenWaitResult ignored_result{};
        std::size_t ignored_size = 0;
        (void)session.wait_reply(session.user, waiter_token, nullptr, dtv_timeout{1, 0},
                                 &ignored_result, reply_buffer, &ignored_size);
    };
    if (operation->cancelled.load(std::memory_order_acquire)) {
        abandon_waiter();
        erase_operation();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
    }
    if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
        abandon_waiter();
        erase_operation();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
    }
    if (state.registry.status(object) == DTV_ENCLOSURE_DISCONNECTED) {
        abandon_waiter();
        erase_operation();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
    }

    std::uint64_t output_request = 0;
    result = runtime->submit_bulk_out(plan.handle_index, plan.bulk_out_endpoint, plan.output,
        generation, clock, nullptr, deadline, &output_request);
    if (result.code != DTV_INTERNAL_RESULT_OK) {
        abandon_waiter();
        erase_operation();
        return result;
    }
    {
        std::lock_guard lock(slot->operation_mutex);
        operation->output_request = output_request;
        operation->request_submitted = true;
    }
    if (operation->cancelled.load(std::memory_order_acquire)) {
        (void)runtime->cancel(output_request);
        (void)session.cancel_reply(session.user, waiter_token);
    }

    for (;;) {
        if (operation->cancelled.load(std::memory_order_acquire)) {
            (void)runtime->cancel(output_request);
            cancel_and_consume_waiter();
            erase_operation();
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
        }
        if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
            (void)runtime->cancel(output_request);
            cancel_and_consume_waiter();
            erase_operation();
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
        }
        UsbRuntimeCompletion completion;
        bool available = false;
        result = runtime->take_completion(output_request, &completion, &available);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            (void)runtime->cancel(output_request);
            cancel_and_consume_waiter();
            erase_operation();
            return result;
        }
        if (available) {
            {
                std::lock_guard lock(slot->operation_mutex);
                operation->output_retired = true;
                slot->operation_condition.notify_all();
            }
            if (completion.terminal != UsbRuntimeTerminal::completed ||
                completion.result.code != DTV_INTERNAL_RESULT_OK) {
                if (completion.reached_backend) cancel_and_consume_waiter();
                else abandon_waiter();
                erase_operation();
                return completion.result.code == DTV_INTERNAL_RESULT_OK
                    ? dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0)
                    : completion.result;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
        cancel_and_consume_waiter();
        erase_operation();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
    }
    dtv::spi::PublicOpenWaitResult wait_result{};
    std::size_t actual_reply_size = 0;
    result = session.wait_reply(session.user, waiter_token, &operation->cancelled,
        remaining_timeout(*clock, deadline), &wait_result, reply_buffer, &actual_reply_size);
    if (result.code != DTV_INTERNAL_RESULT_OK) {
        erase_operation();
        return result;
    }
    if (wait_result == dtv::spi::PublicOpenWaitResult::cancelled) {
        erase_operation();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
    }
    if (wait_result == dtv::spi::PublicOpenWaitResult::deadline) {
        erase_operation();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
    }
    if (wait_result != dtv::spi::PublicOpenWaitResult::completed) {
        erase_operation();
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    }
    std::copy_n(reply_buffer.begin(), actual_reply_size, reply.begin());
    *reply_size = actual_reply_size;
    erase_operation();
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

std::int32_t run_public_stream_operation(
    dtv_stream *stream, std::uint32_t operation, std::span<const unsigned char> arguments,
    dtv_timeout timeout, std::span<unsigned char> reply, std::size_t *reply_size,
    dtv_error *error)
{
    if (stream == nullptr || stream->state == nullptr || reply_size == nullptr) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    auto shared = std::static_pointer_cast<PublicStreamState>(stream->state);
    {
        std::lock_guard lock(shared->mutex);
        if (shared->closed || shared->closing ||
            shared->cancelled.load(std::memory_order_acquire)) {
            set_public_error(error, DTV_ERROR_CANCELLED);
            return DTV_ERROR_CANCELLED;
        }
    }
    const dtv::spi::PublicOperationRequest request{operation,
        dtv::spi::PublicOperationTargetKind::stream, shared->stream_generation, arguments};
    const auto result = run_public_operation_for_test(shared->enclosure, request,
        timeout, reply, reply_size);
    const auto code = public_error_code(result);
    set_public_error(error, code, nullptr, &result);
    return code;
}

std::int32_t run_public_card_operation(
    dtv_enclosure *enclosure, std::uint32_t operation,
    std::span<const unsigned char> arguments, dtv_timeout timeout,
    std::span<unsigned char> reply, std::size_t *reply_size, dtv_error *error)
{
    if (enclosure == nullptr || reply_size == nullptr) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    const dtv::spi::PublicOperationRequest request{operation,
        dtv::spi::PublicOperationTargetKind::card,
        public_discovery_state().registry.enclosure_id(enclosure), arguments};
    const auto result = run_public_operation_for_test(enclosure, request,
        timeout, reply, reply_size);
    const auto code = public_error_code(result);
    set_public_error(error, code, nullptr, &result);
    return code;
}

namespace {

dtv_enclosure *const *list_enclosures_by_tokens(
    const char *product_id, const dtv_usb_token *tokens, std::uint32_t token_count,
    dtv_error *error)
{
    try {
        auto &state = public_discovery_state();
        std::lock_guard lock(state.api_mutex);
        state.list_started = true;
        if ((token_count != 0 && tokens == nullptr) ||
            (product_id != nullptr && product_id[0] == '\0')) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT, "invalid token list");
            return nullptr;
        }
        std::set<dtv_usb_token> requested;
        for (std::uint32_t index = 0; index < token_count; ++index) {
            if (tokens[index] == 0 || !requested.insert(tokens[index]).second) {
                set_public_error(error, DTV_ERROR_INVALID_ARGUMENT,
                                 "token list contains zero or duplicate token");
                return nullptr;
            }
        }
        if (!state.token_callback_set || state.token_callback == nullptr) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT,
                             "token callback is not configured");
            return nullptr;
        }
        auto *product_view = product_id == nullptr ? nullptr : &state.token_product_view(product_id);
        state.registry.begin_public_boundary();
        CompleteGroupBatch batch;
        auto result = state.session.scan_with_token_callback(
            state.token_callback, state.token_user, &batch);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            state.session.fail_scan_for_test(result);
            set_public_error(error, public_error_code(result));
            return nullptr;
        }

        std::map<dtv_usb_token, std::uint64_t> token_connections;
        std::set<dtv_usb_token> found;
        CompleteGroupBatch selection;
        for (const auto &snapshot : batch.snapshots) {
            if (snapshot.selection_token == 0) continue;
            const auto [owner, inserted] = token_connections.emplace(
                snapshot.selection_token, snapshot.connection_token);
            if (!inserted && owner->second != snapshot.connection_token) {
                set_public_error(error, DTV_ERROR_INVALID_ARGUMENT,
                                 "token callback returned duplicate token");
                return nullptr;
            }
            if (requested.contains(snapshot.selection_token)) {
                found.insert(snapshot.selection_token);
                selection.snapshots.push_back(snapshot);
            }
        }
        if (found.size() != requested.size()) {
            set_public_error(error, DTV_ERROR_NOT_FOUND, "requested token was not found");
            return nullptr;
        }
        result = observe_loaded_product_groups(std::move(selection.snapshots), &selection);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            state.session.fail_scan_for_test(result);
            set_public_error(error, public_error_code(result));
            return nullptr;
        }
        result = state.session.refresh(std::move(batch), std::move(selection));
        const auto code = public_error_code(result);
        set_public_error(error, code);
        if (code != DTV_ERROR_OK) return nullptr;
        state.project_product_views();
        return product_view == nullptr ? state.registry.token_slots().data() : product_view->data();
    } catch (...) {
        set_public_error(error, public_error_code_from_exception());
        return nullptr;
    }
}

dtv_enclosure *const *list_enclosures_by_fds(
    const char *product_id, const std::int32_t *fds, std::uint32_t fd_count,
    dtv_error *error)
{
    try {
        auto &state = public_discovery_state();
        std::lock_guard lock(state.api_mutex);
        state.list_started = true;
        if ((fd_count != 0 && fds == nullptr) ||
            (product_id != nullptr && product_id[0] == '\0')) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT, "invalid FD list");
            return nullptr;
        }
        auto *product_view = product_id == nullptr ? nullptr : &state.fd_product_view(product_id);
        std::set<std::int32_t> unique_fds;
        for (std::uint32_t index = 0; index < fd_count; ++index) {
            if (fds[index] < 0 || !unique_fds.insert(fds[index]).second) {
                set_public_error(error, DTV_ERROR_INVALID_ARGUMENT,
                                 "FD list contains invalid or duplicate descriptor");
                return nullptr;
            }
        }
        state.registry.begin_public_boundary();
        CompleteGroupBatch batch;
        const auto result = state.session.scan_with_fds(
            std::span<const std::int32_t>(fds, fd_count), &batch);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            state.session.fail_scan_for_test(result);
            const auto code = result.code == DTV_INTERNAL_RESULT_USB_IO &&
                                      result.detail == static_cast<std::uint32_t>(-LIBUSB_ERROR_ACCESS)
                                  ? DTV_ERROR_PERMISSION_DENIED
                                  : public_error_code(result);
            set_public_error(error, code);
            return nullptr;
        }
        std::set<std::uint64_t> connections;
        for (const auto &snapshot : batch.snapshots) {
            if (!connections.insert(snapshot.connection_token).second) {
                set_public_error(error, DTV_ERROR_INVALID_ARGUMENT,
                                 "FD list contains the same USB connection more than once");
                return nullptr;
            }
        }
        const auto update = state.registry.refresh_by_fds(std::move(batch));
        const auto code = public_error_code(update);
        set_public_error(error, code);
        if (code != DTV_ERROR_OK) return nullptr;
        state.project_product_views();
        if (product_id == nullptr) return state.registry.fd_slots().data();
        return product_view->data();
    } catch (...) {
        set_public_error(error, public_error_code_from_exception());
        return nullptr;
    }
}

}

dtv_enclosure *const *list_enclosures_for_product_by_tokens(
    const char *product_id, const dtv_usb_token *tokens, std::uint32_t token_count,
    dtv_error *error)
{
    return list_enclosures_by_tokens(product_id, tokens, token_count, error);
}

dtv_enclosure *const *list_enclosures_for_product_by_fds(
    const char *product_id, const std::int32_t *fds, std::uint32_t fd_count,
    dtv_error *error)
{
    return list_enclosures_by_fds(product_id, fds, fd_count, error);
}

extern "C" std::uint32_t dtv_get_list_capacity(void)
{
    return DTV_MAX_ENCLOSURES;
}

extern "C" dtv_enclosure *const *dtv_list_enclosures(dtv_error *error)
{
    try {
        auto &state = public_discovery_state();
        std::lock_guard lock(state.api_mutex);
        state.list_started = true;
        state.registry.begin_public_boundary();
        const auto result = state.refresh_and_project();
        const auto code = public_error_code(result);
        set_public_error(error, code);
        if (code != DTV_ERROR_OK) return nullptr;
        return state.registry.live_slots().data();
    } catch (...) {
        set_public_error(error, public_error_code_from_exception());
        return nullptr;
    }
}

extern "C" std::uint32_t dtv_get_scan_complete(void)
{
    try {
        return public_discovery_state().registry.scan_complete() ? 1u : 0u;
    } catch (...) {
        return 0;
    }
}

dtv_enclosure *const *list_enclosures_for_product(const char *product_id, dtv_error *error)
{
    if (product_id == nullptr || product_id[0] == '\0') {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return nullptr;
    }
    try {
        auto &state = public_discovery_state();
        std::lock_guard lock(state.api_mutex);
        state.list_started = true;
        state.registry.begin_public_boundary();
        auto &view = state.product_view(product_id);
        const auto result = state.refresh_and_project();
        const auto code = public_error_code(result);
        set_public_error(error, code);
        if (code != DTV_ERROR_OK) return nullptr;
        return view.data();
    } catch (...) {
        set_public_error(error, public_error_code_from_exception());
        return nullptr;
    }
}

extern "C" dtv_enclosure *const *dtv_list_enclosures_by_tokens(
    const dtv_usb_token *tokens, std::uint32_t token_count, dtv_error *error)
{
    return list_enclosures_by_tokens(nullptr, tokens, token_count, error);
}

extern "C" dtv_enclosure *const *dtv_list_enclosures_by_fds(
    const std::int32_t *fds, std::uint32_t fd_count, dtv_error *error)
{
    return list_enclosures_by_fds(nullptr, fds, fd_count, error);
}

extern "C" const dtv_ungrouped_device *const *dtv_list_ungrouped_devices(void)
{
    try {
        auto &state = public_discovery_state();
        std::lock_guard lock(state.api_mutex);
        state.list_started = true;
        state.registry.begin_public_boundary();
        const auto result = state.refresh_and_project();
        if (result.code != DTV_INTERNAL_RESULT_OK) return nullptr;
        return state.registry.ungrouped_view();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void dtv_hold_enclosure(dtv_enclosure *object)
{
    if (object == nullptr) return;
    try {
        (void)public_discovery_state().registry.hold(object);
    } catch (...) {
    }
}

extern "C" void dtv_drop_enclosure(dtv_enclosure *object)
{
    if (object == nullptr) return;
    try {
        (void)public_discovery_state().registry.drop(object);
    } catch (...) {
    }
}

extern "C" int32_t dtv_wait_event(dtv_event *out, const dtv_call_options *call,
                                   dtv_error *error)
{
    if (out == nullptr || (call != nullptr && call->struct_size < sizeof(dtv_call_options))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    *out = {};
    try {
        auto &state = public_discovery_state();
        std::lock_guard api_lock(state.api_mutex);
        state.list_started = true;
        state.registry.begin_public_boundary();
        const auto *clock = system_runtime_clock();
        if (clock == nullptr || clock->monotonic_now_ns == nullptr || clock->delay_until == nullptr) {
            set_public_error(error, DTV_ERROR_INTERNAL);
            return DTV_ERROR_INTERNAL;
        }
        const auto deadline = deadline_after(clock->monotonic_now_ns(clock->user),
                                             call == nullptr ? 0 : call->timeout_ms);
        for (;;) {
            bool available = false;
            auto result = state.registry.next_event(out, &available);
            if (result.code != DTV_INTERNAL_RESULT_OK) {
                const auto code = public_error_code(result);
                set_public_error(error, code);
                return code;
            }
            if (available) {
                set_public_error(error, DTV_ERROR_OK);
                return DTV_ERROR_OK;
            }

            result = state.refresh_and_project();
            if (result.code != DTV_INTERNAL_RESULT_OK &&
                result.code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED) {
                const auto code = public_error_code(result);
                set_public_error(error, code);
                return code;
            }
            if (result.code == DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED &&
                !state.registry.resource_limited()) {
                const auto code = public_error_code(result);
                set_public_error(error, code);
                return code;
            }
            result = state.registry.next_event(out, &available);
            if (result.code != DTV_INTERNAL_RESULT_OK) {
                const auto code = public_error_code(result);
                set_public_error(error, code);
                return code;
            }
            if (available) {
                set_public_error(error, DTV_ERROR_OK);
                return DTV_ERROR_OK;
            }
            if (result.code == DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
                state.registry.resource_limited()) {
                set_public_error(error, DTV_ERROR_RESOURCE_LIMIT);
                return DTV_ERROR_RESOURCE_LIMIT;
            }

            const auto now = clock->monotonic_now_ns(clock->user);
            if (deadline != UINT64_MAX && now >= deadline) {
                set_public_error(error, DTV_ERROR_TIMEOUT);
                return DTV_ERROR_TIMEOUT;
            }
            const auto next_poll = now > UINT64_MAX - UINT64_C(10000000)
                ? UINT64_MAX : now + UINT64_C(10000000);
            const auto wake_at = deadline == UINT64_MAX ? next_poll : (std::min)(next_poll, deadline);
            std::uint32_t cancelled = 0;
            result = clock->delay_until(clock->user, wake_at, deadline, nullptr, &cancelled);
            if (cancelled != 0) {
                set_public_error(error, DTV_ERROR_CANCELLED);
                return DTV_ERROR_CANCELLED;
            }
            if (result.code == DTV_INTERNAL_RESULT_TIMEOUT) {
                set_public_error(error, DTV_ERROR_TIMEOUT);
                return DTV_ERROR_TIMEOUT;
            }
            if (result.code != DTV_INTERNAL_RESULT_OK) {
                const auto code = public_error_code(result);
                set_public_error(error, code);
                return code;
            }
        }
    } catch (...) {
        const auto code = public_error_code_from_exception();
        set_public_error(error, code);
        return code;
    }
}

extern "C" std::uint32_t dtv_get_enclosure_id(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.enclosure_id(object); }
    catch (...) { return 0; }
}

extern "C" const char *dtv_get_product(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.product(object); }
    catch (...) { return nullptr; }
}

extern "C" const char *dtv_get_model(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.model(object); }
    catch (...) { return nullptr; }
}

extern "C" const char *dtv_get_enclosure_serial(dtv_enclosure *) { return nullptr; }
extern "C" std::uint32_t dtv_get_serial_unique(dtv_enclosure *) { return DTV_UNKNOWN; }
extern "C" std::uint32_t dtv_get_status(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.status(object); }
    catch (...) { return DTV_ENCLOSURE_UNKNOWN; }
}
extern "C" std::uint32_t dtv_get_lnb_state(dtv_enclosure *) { return DTV_LNB_UNKNOWN; }
extern "C" std::uint32_t dtv_get_card_state(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.card_state(object); }
    catch (...) { return DTV_CARD_UNKNOWN; }
}

extern "C" std::int32_t dtv_get_enclosure_support(
    dtv_enclosure *object, std::uint32_t operation, std::uint32_t *out_tribool,
    dtv_error *error)
{
    if (object == nullptr || out_tribool == nullptr || operation < DTV_OP_LNB_POWER ||
        operation > DTV_OP_LNA) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    auto &registry = public_discovery_state().registry;
    const auto status = registry.status(object);
    if (status == DTV_ENCLOSURE_UNKNOWN) {
        set_public_error(error, DTV_ERROR_NOT_FOUND);
        return DTV_ERROR_NOT_FOUND;
    }
    if (status == DTV_ENCLOSURE_DISCONNECTED) {
        set_public_error(error, DTV_ERROR_DISCONNECTED);
        return DTV_ERROR_DISCONNECTED;
    }
    const auto *model = registry.model(object);
    if (model == nullptr) {
        set_public_error(error, DTV_ERROR_NOT_READY);
        return DTV_ERROR_NOT_READY;
    }
    const auto result = dtv_get_model_support(model, operation, out_tribool, error);
    if (result == DTV_ERROR_OK) set_public_error(error, DTV_ERROR_OK);
    return result;
}

namespace {

bool valid_card_atr(std::span<const std::uint8_t> atr)
{
    if (atr.size() < 2 || (atr[0] != 0x3b && atr[0] != 0x3f)) return false;
    std::size_t offset = 2;
    auto interface_presence = static_cast<unsigned>(atr[1] >> 4);
    const auto historical_count = static_cast<std::size_t>(atr[1] & 0x0f);
    bool requires_checksum = false;
    bool has_next = true;
    unsigned groups = 0;
    while (has_next) {
        if (++groups > 16) return false;
        has_next = false;
        for (unsigned bit = 0; bit < 4; ++bit) {
            if ((interface_presence & (1u << bit)) == 0) continue;
            if (offset >= atr.size()) return false;
            const auto value = atr[offset++];
            if (bit == 3) {
                requires_checksum = requires_checksum || (value & 0x0f) != 0;
                interface_presence = value >> 4;
                has_next = interface_presence != 0;
            }
        }
    }
    if (historical_count > atr.size() - offset) return false;
    offset += historical_count;
    if (requires_checksum) {
        if (offset >= atr.size()) return false;
        std::uint8_t checksum = 0;
        for (std::size_t index = 1; index <= offset; ++index) checksum ^= atr[index];
        if (checksum != 0) return false;
        ++offset;
    }
    return offset == atr.size();
}

}

extern "C" std::int32_t dtv_reset_card(
    dtv_enclosure *object, const dtv_call_options *options,
    const std::uint8_t **out_atr, std::uint64_t *out_size, dtv_error *error)
{
    if (object == nullptr || out_atr == nullptr || out_size == nullptr ||
        (options != nullptr && options->struct_size < sizeof(*options))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    *out_atr = nullptr;
    *out_size = 0;
    std::uint32_t support = DTV_UNKNOWN;
    const auto support_result = dtv_get_enclosure_support(object, DTV_OP_CARD, &support, error);
    if (support_result != DTV_ERROR_OK) return support_result;
    if (support != DTV_YES) {
        set_public_error(error, DTV_ERROR_UNSUPPORTED);
        return DTV_ERROR_UNSUPPORTED;
    }
    auto &registry = public_discovery_state().registry;
    std::shared_ptr<void> card_lease;
    std::uint64_t connection_epoch = 0;
    std::uint64_t request_epoch = 0;
    auto result = registry.begin_card_reset(object, &card_lease, &connection_epoch,
                                             &request_epoch);
    if (result.code != DTV_INTERNAL_RESULT_OK) {
        const auto code = public_error_code(result);
        set_public_error(error, code, nullptr, &result);
        return code;
    }
    const auto *product = registry.product(object);
    const auto *model = registry.model(object);
    if (product == nullptr || model == nullptr) {
        set_public_error(error, DTV_ERROR_NOT_READY);
        return DTV_ERROR_NOT_READY;
    }
    try {
        std::vector<std::uint8_t> atr;
        auto code = dtv::internal::dispatch_reset_card(product, model, object, options, &atr, error);
        if (code != DTV_ERROR_OK) {
            set_public_error(error, code);
            return code;
        }
        if (!valid_card_atr(atr)) {
            set_public_error(error, DTV_ERROR_CARD_IO);
            return DTV_ERROR_CARD_IO;
        }
        result = registry.finish_card_reset(object, connection_epoch, request_epoch, atr);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        result = registry.card_atr_view(object, out_atr, out_size);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        set_public_error(error, DTV_ERROR_OK);
        return DTV_ERROR_OK;
    } catch (...) {
        const auto code = public_error_code_from_exception();
        set_public_error(error, code);
        return code;
    }
}
extern "C" std::uint32_t dtv_get_card_generation(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.card_generation(object); }
    catch (...) { return 0; }
}
extern "C" dtv_device *dtv_get_card_reader_device(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.card_reader_device(object); }
    catch (...) { return nullptr; }
}

extern "C" dtv_device *const *dtv_get_devices(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.device_list(object); }
    catch (...) { return nullptr; }
}

extern "C" const dtv_candidate *const *dtv_get_candidates(dtv_enclosure *object)
{
    try { return public_discovery_state().registry.candidates(object); }
    catch (...) { return nullptr; }
}

extern "C" std::uint32_t dtv_get_device_id(dtv_device *object)
{
    try { return public_discovery_state().registry.device_id(object); }
    catch (...) { return 0; }
}
extern "C" std::uint16_t dtv_get_vid(dtv_device *object)
{
    try { return public_discovery_state().registry.vendor_id(object); }
    catch (...) { return 0; }
}
extern "C" std::uint16_t dtv_get_pid(dtv_device *object)
{
    try { return public_discovery_state().registry.product_id(object); }
    catch (...) { return 0; }
}
extern "C" const char *dtv_get_device_serial(dtv_device *) { return nullptr; }
extern "C" const std::uint32_t *dtv_get_bus(dtv_device *object)
{
    try { return public_discovery_state().registry.bus(object); }
    catch (...) { return nullptr; }
}
extern "C" const std::uint32_t *dtv_get_address(dtv_device *object)
{
    try { return public_discovery_state().registry.address(object); }
    catch (...) { return nullptr; }
}
extern "C" const char *dtv_get_port(dtv_device *object)
{
    try { return public_discovery_state().registry.port(object); }
    catch (...) { return nullptr; }
}
extern "C" std::uint32_t dtv_get_mode(dtv_device *) { return DTV_DEVICE_UNKNOWN; }
extern "C" dtv_receiver *const *dtv_get_receivers(dtv_device *object)
{
    try { return public_discovery_state().registry.receiver_list(object); }
    catch (...) { return nullptr; }
}
extern "C" std::uint32_t dtv_get_index(dtv_receiver *object)
{
    try { return public_discovery_state().registry.receiver_index(object); }
    catch (...) { return 0; }
}
extern "C" std::uint32_t dtv_get_local(dtv_receiver *object)
{
    try { return public_discovery_state().registry.receiver_local(object); }
    catch (...) { return 0; }
}
extern "C" std::uint32_t dtv_get_systems(dtv_receiver *object)
{
    try { return public_discovery_state().registry.receiver_systems(object); }
    catch (...) { return 0; }
}
extern "C" std::uint32_t dtv_get_ts_scrambling(dtv_receiver *object)
{
    try { return public_discovery_state().registry.receiver_ts_scrambling(object); }
    catch (...) { return DTV_TS_UNKNOWN; }
}
extern "C" std::int32_t dtv_get_receiver_support(dtv_receiver *object,
                                                  std::uint32_t operation,
                                                  std::uint32_t *out, dtv_error *error)
{
    try { return public_discovery_state().registry.receiver_support(object, operation, out, error); }
    catch (...) {
        if (error != nullptr && error->struct_size >= sizeof(error->code))
            error->code = DTV_ERROR_RESOURCE_LIMIT;
        return DTV_ERROR_RESOURCE_LIMIT;
    }
}

namespace {

bool parse_channel_number(std::string_view text, std::uint32_t *out)
{
    if (text.empty() || out == nullptr) return false;
    std::uint32_t value = 0;
    for (const auto character : text) {
        if (character < '0' || character > '9') return false;
        const auto digit = static_cast<std::uint32_t>(character - '0');
        if (value > ((std::numeric_limits<std::uint32_t>::max)() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

struct ResolvedPublicChannel {
    std::uint32_t system = 0;
    std::uint64_t frequency_hz = 0;
    bool has_slot = false;
    std::uint32_t slot = 0;
    bool selector_required = false;
    bool default_slot = false;
};

bool resolve_public_channel(std::string_view channel, ResolvedPublicChannel *out)
{
    if (out == nullptr) return false;
    if ((channel.size() == 2 && channel[0] >= '1' && channel[0] <= '9') ||
        (channel.size() == 3 && channel[0] == 'T')) {
        const auto digits = channel.size() == 2 ? channel : channel.substr(1);
        std::uint32_t number = 0;
        if (!parse_channel_number(digits, &number) || number < 13 || number > 62) return false;
        out->system = DTV_SYSTEM_ISDB_T;
        out->frequency_hz = UINT64_C(473142857) +
            static_cast<std::uint64_t>(number - 13) * UINT64_C(6000000);
        return true;
    }
    if (channel.starts_with("BS") && channel.size() >= 4) {
        const auto digits = channel.substr(2, 2);
        std::uint32_t number = 0;
        if (!parse_channel_number(digits, &number) || number < 1 || number > 23 || number % 2 == 0)
            return false;
        if (channel.size() != 4) {
            if (channel.size() < 6 || channel[4] != '_' || channel.size() > 7) return false;
            const auto suffix = channel.substr(5);
            if ((suffix.size() > 1 && suffix.front() == '0') ||
                !parse_channel_number(suffix, &out->slot) || out->slot > 11) return false;
            out->has_slot = true;
        }
        out->selector_required = !out->has_slot;
        out->system = DTV_SYSTEM_ISDB_S;
        out->frequency_hz = UINT64_C(1049480000) +
            static_cast<std::uint64_t>((number - 1) / 2) * UINT64_C(38360000);
        return true;
    }
    if (channel.starts_with("CS")) {
        const auto digits = channel.substr(2);
        std::uint32_t number = 0;
        if ((digits.size() > 1 && digits.front() == '0' && digits.size() != 2) ||
            !parse_channel_number(digits, &number) || number < 2 || number > 24 || number % 2 != 0)
            return false;
        if (digits.size() == 2 && digits.front() == '0' && number > 8) return false;
        out->system = DTV_SYSTEM_ISDB_S;
        out->default_slot = true;
        out->frequency_hz = UINT64_C(1613000000) +
            static_cast<std::uint64_t>((number - 2) / 2) * UINT64_C(40000000);
        return true;
    }
    return false;
}

}

extern "C" std::int32_t dtv_tune_by_channel(dtv_receiver *object, const char *channel,
    const dtv_tune_options *tune_options, const dtv_call_options *options, dtv_error *error)
{
    if (object == nullptr || channel == nullptr ||
        (tune_options != nullptr && tune_options->struct_size < sizeof(*tune_options)) ||
        (options != nullptr && options->struct_size < sizeof(*options)) ||
        (tune_options != nullptr &&
            (tune_options->selector > DTV_TUNE_SELECTOR_TSID || tune_options->lna > DTV_LNA_ON ||
             (tune_options->selector == DTV_TUNE_SELECTOR_SLOT && tune_options->value > 11)))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    ResolvedPublicChannel resolved{};
    if (!resolve_public_channel(channel, &resolved)) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    dtv_tune_options resolved_options = tune_options == nullptr
        ? dtv_tune_options{sizeof(dtv_tune_options), DTV_TUNE_SELECTOR_NONE, 0, DTV_LNA_DEFAULT}
        : *tune_options;
    if (resolved.system == DTV_SYSTEM_ISDB_T) {
        if (resolved_options.selector != DTV_TUNE_SELECTOR_NONE) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
            return DTV_ERROR_INVALID_ARGUMENT;
        }
    } else if (resolved.has_slot) {
        if (resolved_options.selector != DTV_TUNE_SELECTOR_NONE) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
            return DTV_ERROR_INVALID_ARGUMENT;
        }
        resolved_options.selector = DTV_TUNE_SELECTOR_SLOT;
        resolved_options.value = resolved.slot;
    } else if (resolved_options.selector == DTV_TUNE_SELECTOR_NONE) {
        if (resolved.selector_required) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
            return DTV_ERROR_INVALID_ARGUMENT;
        }
        if (resolved.default_slot) {
            resolved_options.selector = DTV_TUNE_SELECTOR_SLOT;
            resolved_options.value = 0;
        }
    }
    const auto *forwarded_options = tune_options == nullptr && resolved.system == DTV_SYSTEM_ISDB_T
        ? nullptr : &resolved_options;
    return dtv_tune_by_frequency(object, resolved.system, resolved.frequency_hz,
        forwarded_options, options, error);
}

extern "C" std::int32_t dtv_tune_by_frequency(dtv_receiver *object, std::uint32_t system,
    std::uint64_t frequency_hz, const dtv_tune_options *tune_options,
    const dtv_call_options *options, dtv_error *error)
{
    if (object == nullptr || (tune_options != nullptr && tune_options->struct_size < sizeof(*tune_options)) ||
        (options != nullptr && options->struct_size < sizeof(*options)) ||
        (system != DTV_SYSTEM_ISDB_T && system != DTV_SYSTEM_ISDB_S) ||
        (tune_options != nullptr &&
            (tune_options->selector > DTV_TUNE_SELECTOR_TSID || tune_options->lna > DTV_LNA_ON ||
             (tune_options->selector == DTV_TUNE_SELECTOR_SLOT && tune_options->value > 11)))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto &state = public_discovery_state();
        EnclosureRegistry::ReceiverSignalSnapshot snapshot;
        auto result = state.registry.receiver_signal_snapshot(object, &snapshot);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            const auto code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        if (snapshot.enclosure_status == DTV_ENCLOSURE_DISCONNECTED) {
            set_public_error(error, DTV_ERROR_DISCONNECTED);
            return DTV_ERROR_DISCONNECTED;
        }
        const auto tune_operation = system == DTV_SYSTEM_ISDB_T
            ? DTV_OP_TUNE_ISDB_T : DTV_OP_TUNE_ISDB_S;
        std::array<std::uint32_t, 4> required_operations{tune_operation, 0, 0, 0};
        std::size_t required_count = 1;
        if (tune_options != nullptr && tune_options->selector == DTV_TUNE_SELECTOR_SLOT)
            required_operations[required_count++] = DTV_OP_SELECT_SLOT;
        if (tune_options != nullptr && tune_options->selector == DTV_TUNE_SELECTOR_TSID)
            required_operations[required_count++] = DTV_OP_SELECT_TSID;
        if (tune_options != nullptr && tune_options->lna != DTV_LNA_DEFAULT)
            required_operations[required_count++] = DTV_OP_LNA;
        bool unknown_declaration = false;
        for (std::size_t index = 0; index < required_count; ++index) {
            const auto bit = std::uint32_t{1} << (required_operations[index] - 1);
            if ((snapshot.known_operations & bit) != 0 &&
                (snapshot.supported_operations & bit) == 0) {
                set_public_error(error, DTV_ERROR_UNSUPPORTED);
                return DTV_ERROR_UNSUPPORTED;
            }
            if ((snapshot.known_operations & bit) == 0) unknown_declaration = true;
        }
        if ((snapshot.receiver_systems & (std::uint32_t{1} << system)) == 0) {
            set_public_error(error, DTV_ERROR_UNSUPPORTED);
            return DTV_ERROR_UNSUPPORTED;
        }
        if (unknown_declaration) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }

        std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
        std::shared_ptr<dtv::spi::ProductInstance> instance;
        const dtv::spi::PublicOpenOps *ops = nullptr;
        {
            std::lock_guard lock(state.open_mutex);
            const auto found = state.open_slots.find(snapshot.enclosure);
            if (found != state.open_slots.end()) slot = found->second;
        }
        if (!slot) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        {
            std::lock_guard lock(slot->mutex);
            if (slot->phase != PublicDiscoveryState::OpenSlot::Phase::open) {
                set_public_error(error, DTV_ERROR_NOT_READY);
                return DTV_ERROR_NOT_READY;
            }
            instance = slot->product_instance;
            ops = slot->ops;
        }
        if (!instance || ops == nullptr || ops->plan_tune == nullptr ||
            ops->tune_by_frequency == nullptr) {
            set_public_error(error, DTV_ERROR_UNSUPPORTED);
            return DTV_ERROR_UNSUPPORTED;
        }

        const auto *clock = system_runtime_clock();
        if (clock == nullptr || clock->monotonic_now_ns == nullptr) {
            set_public_error(error, DTV_ERROR_INTERNAL);
            return DTV_ERROR_INTERNAL;
        }
        const auto deadline = deadline_after(clock->monotonic_now_ns(clock->user),
            options == nullptr ? 0 : options->timeout_ms);
        dtv::spi::PublicTuneRequest request{};
        request.receiver_index = snapshot.receiver_index;
        request.connection_epoch = snapshot.connection_epoch;
        request.previous_operation_epoch = snapshot.operation_epoch;
        request.system = system;
        request.frequency_hz = frequency_hz;
        request.has_tune_options = tune_options != nullptr;
        request.tune_options = tune_options == nullptr
            ? dtv_tune_options{sizeof(dtv_tune_options), DTV_TUNE_SELECTOR_NONE, 0, DTV_LNA_DEFAULT}
            : *tune_options;
        request.timeout = remaining_timeout(*clock, deadline);
        dtv::spi::PublicTunePlan plan{};
        result = call_callback([&] {
            return ops->plan_tune(instance->context(), &request, &plan);
        });
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            const auto code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        if (!plan.supported) {
            set_public_error(error, DTV_ERROR_UNSUPPORTED);
            return DTV_ERROR_UNSUPPORTED;
        }
        if (!plan.ready) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        if (!plan.arguments_valid || plan.resource_keys.size() > 8) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
            return DTV_ERROR_INVALID_ARGUMENT;
        }
        if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
            set_public_error(error, DTV_ERROR_TIMEOUT);
            return DTV_ERROR_TIMEOUT;
        }

        std::shared_ptr<PublicDiscoveryState::OpenSlot::OperationControl> operation;
        try {
            if (state.operation_allocation_failure_for_test.exchange(false,
                    std::memory_order_acq_rel)) throw std::bad_alloc();
            operation = std::make_shared<PublicDiscoveryState::OpenSlot::OperationControl>();
            operation->target_kind = dtv::spi::PublicOperationTargetKind::receiver;
            operation->target_id = snapshot.receiver_index;
            operation->resource_count = plan.resource_keys.size();
            std::copy(plan.resource_keys.begin(), plan.resource_keys.end(), operation->resource_keys.begin());
            std::sort(operation->resource_keys.begin(),
                operation->resource_keys.begin() + static_cast<std::ptrdiff_t>(operation->resource_count));
            const auto unique_end = std::unique(operation->resource_keys.begin(),
                operation->resource_keys.begin() + static_cast<std::ptrdiff_t>(operation->resource_count));
            operation->resource_count = static_cast<std::size_t>(unique_end - operation->resource_keys.begin());
            if (std::any_of(operation->resource_keys.begin(), unique_end,
                [](const auto key) { return key == 0; })) {
                set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
                return DTV_ERROR_INVALID_ARGUMENT;
            }
        } catch (...) {
            const auto code = public_error_code_from_exception();
            set_public_error(error, code);
            return code;
        }
        {
            std::lock_guard lock(slot->operation_mutex);
            if (slot->operation_closing || slot->next_operation_id == 0) {
                const auto code = slot->operation_closing ? DTV_ERROR_NOT_READY
                                                          : DTV_ERROR_RESOURCE_LIMIT;
                set_public_error(error, code);
                return code;
            }
            const auto free = std::find(slot->operations.begin(), slot->operations.end(), nullptr);
            if (free == slot->operations.end()) {
                set_public_error(error, DTV_ERROR_RESOURCE_LIMIT);
                return DTV_ERROR_RESOURCE_LIMIT;
            }
            operation->id = slot->next_operation_id++;
            *free = operation;
        }
        const auto erase_ticket = [slot, operation](void *) {
            std::lock_guard lock(slot->operation_mutex);
            operation->caller_done = true;
            for (auto &entry : slot->operations) if (entry == operation) entry.reset();
            slot->operation_condition.notify_all();
        };
        std::unique_ptr<void, decltype(erase_ticket)> ticket_guard(
            reinterpret_cast<void *>(std::uintptr_t{1}), erase_ticket);
        for (;;) {
            std::unique_lock lock(slot->operation_mutex);
            if (operation->cancelled.load(std::memory_order_acquire)) {
                set_public_error(error, DTV_ERROR_CANCELLED);
                return DTV_ERROR_CANCELLED;
            }
            if (slot->operation_closing) {
                set_public_error(error, DTV_ERROR_NOT_READY);
                return DTV_ERROR_NOT_READY;
            }
            bool blocked = false;
            for (const auto &other : slot->operations) {
                if (!other || other == operation || !operation_conflicts(*operation, *other)) continue;
                if (other->admitted || other->id < operation->id) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                operation->admitted = true;
                break;
            }
            if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
                set_public_error(error, DTV_ERROR_TIMEOUT);
                return DTV_ERROR_TIMEOUT;
            }
            slot->operation_condition.wait_for(lock, std::chrono::milliseconds(2));
        }
        if (state.registry.status(snapshot.enclosure) == DTV_ENCLOSURE_DISCONNECTED) {
            set_public_error(error, DTV_ERROR_DISCONNECTED);
            return DTV_ERROR_DISCONNECTED;
        }
        {
            std::lock_guard lock(slot->mutex);
            if (slot->phase != PublicDiscoveryState::OpenSlot::Phase::open) {
                set_public_error(error, operation->cancelled.load(std::memory_order_acquire)
                    ? DTV_ERROR_CANCELLED : DTV_ERROR_NOT_READY);
                return operation->cancelled.load(std::memory_order_acquire)
                    ? DTV_ERROR_CANCELLED : DTV_ERROR_NOT_READY;
            }
        }
        request.cancelled = &operation->cancelled;
        request.timeout = remaining_timeout(*clock, deadline);
        PublicTuneEffectContext effect{&state.registry, slot, operation, clock, object,
            snapshot.connection_epoch, snapshot.operation_epoch, deadline};
        dtv::spi::PublicTuneSession session{&effect, begin_public_tune_effect};
        result = call_callback([&] {
            return ops->tune_by_frequency(instance->context(), &request, &session);
        });
        if (operation->cancelled.load(std::memory_order_acquire)) {
            result = dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
        } else if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
            result = dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
        } else if (result.code == DTV_INTERNAL_RESULT_OK && !effect.effect_started) {
            result = dtv_internal_result_make(DTV_INTERNAL_RESULT_INTERNAL, 0);
        } else if (state.registry.status(snapshot.enclosure) == DTV_ENCLOSURE_DISCONNECTED) {
            result = dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
        }
        const auto code = public_error_code(result);
        set_public_error(error, code, nullptr, &result);
        return code;
    } catch (...) {
        const auto code = public_error_code_from_exception();
        set_public_error(error, code);
        return code;
    }
}

extern "C" std::int32_t dtv_get_signal(dtv_receiver *object,
                                        const dtv_call_options *options,
                                        dtv_signal *out, dtv_error *error)
{
    if (object == nullptr || out == nullptr ||
        (options != nullptr && options->struct_size < sizeof(*options))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto &state = public_discovery_state();
        EnclosureRegistry::ReceiverSignalSnapshot snapshot;
        auto result = state.registry.receiver_signal_snapshot(object, &snapshot);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            const auto code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        if (snapshot.enclosure_status == DTV_ENCLOSURE_DISCONNECTED) {
            set_public_error(error, DTV_ERROR_DISCONNECTED);
            return DTV_ERROR_DISCONNECTED;
        }
        if (snapshot.signal_support == DTV_NO) {
            set_public_error(error, DTV_ERROR_UNSUPPORTED);
            return DTV_ERROR_UNSUPPORTED;
        }
        if (snapshot.signal_support != DTV_YES) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }

        std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
        {
            std::lock_guard lock(state.open_mutex);
            const auto found = state.open_slots.find(snapshot.enclosure);
            if (found != state.open_slots.end()) slot = found->second;
        }
        if (!slot) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        {
            std::lock_guard lock(slot->mutex);
            if (slot->phase == PublicDiscoveryState::OpenSlot::Phase::pending ||
                slot->phase == PublicDiscoveryState::OpenSlot::Phase::closed) {
                set_public_error(error, DTV_ERROR_NOT_READY);
                return DTV_ERROR_NOT_READY;
            }
            if (slot->phase != PublicDiscoveryState::OpenSlot::Phase::open) {
                set_public_error(error, DTV_ERROR_NOT_READY);
                return DTV_ERROR_NOT_READY;
            }
        }

        const auto *clock = system_runtime_clock();
        if (clock == nullptr || clock->monotonic_now_ns == nullptr) {
            set_public_error(error, DTV_ERROR_INTERNAL);
            return DTV_ERROR_INTERNAL;
        }
        const auto deadline = deadline_after(clock->monotonic_now_ns(clock->user),
            options == nullptr ? 0 : options->timeout_ms);
        auto operation = std::make_shared<PublicDiscoveryState::OpenSlot::OperationControl>();
        operation->target_kind = dtv::spi::PublicOperationTargetKind::receiver;
        operation->target_id = snapshot.receiver_index;
        {
            std::lock_guard lock(slot->operation_mutex);
            if (slot->operation_closing || slot->next_operation_id == 0) {
                set_public_error(error, slot->operation_closing ? DTV_ERROR_NOT_READY
                                                               : DTV_ERROR_RESOURCE_LIMIT);
                return slot->operation_closing ? DTV_ERROR_NOT_READY : DTV_ERROR_RESOURCE_LIMIT;
            }
            const auto free = std::find(slot->operations.begin(), slot->operations.end(), nullptr);
            if (free == slot->operations.end()) {
                set_public_error(error, DTV_ERROR_RESOURCE_LIMIT);
                return DTV_ERROR_RESOURCE_LIMIT;
            }
            operation->id = slot->next_operation_id++;
            *free = operation;
        }
        const auto erase_operation = [&] {
            std::lock_guard lock(slot->operation_mutex);
            operation->caller_done = true;
            for (auto &entry : slot->operations) if (entry == operation) entry.reset();
            slot->operation_condition.notify_all();
        };
        for (;;) {
            std::unique_lock lock(slot->operation_mutex);
            if (operation->cancelled.load(std::memory_order_acquire)) {
                lock.unlock();
                erase_operation();
                set_public_error(error, DTV_ERROR_CANCELLED);
                return DTV_ERROR_CANCELLED;
            }
            if (slot->operation_closing) {
                lock.unlock();
                erase_operation();
                set_public_error(error, DTV_ERROR_NOT_READY);
                return DTV_ERROR_NOT_READY;
            }
            bool blocked = false;
            for (const auto &other : slot->operations) {
                if (!other || other == operation || !operation_conflicts(*operation, *other)) continue;
                if (other->admitted || other->id < operation->id) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                operation->admitted = true;
                break;
            }
            if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
                lock.unlock();
                erase_operation();
                set_public_error(error, DTV_ERROR_TIMEOUT);
                return DTV_ERROR_TIMEOUT;
            }
            slot->operation_condition.wait_for(lock, std::chrono::milliseconds(2));
        }

        const auto operation_status = [&] {
            if (operation->cancelled.load(std::memory_order_acquire)) return DTV_ERROR_CANCELLED;
            std::lock_guard lock(slot->mutex);
            return slot->phase == PublicDiscoveryState::OpenSlot::Phase::open
                ? DTV_ERROR_OK : DTV_ERROR_NOT_READY;
        };
        auto operation_error = operation_status();
        if (operation_error != DTV_ERROR_OK) {
            erase_operation();
            set_public_error(error, operation_error);
            return operation_error;
        }

        result = state.registry.receiver_signal_snapshot(object, &snapshot);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            erase_operation();
            const auto code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        if (snapshot.enclosure_status == DTV_ENCLOSURE_DISCONNECTED) {
            erase_operation();
            set_public_error(error, DTV_ERROR_DISCONNECTED);
            return DTV_ERROR_DISCONNECTED;
        }
        if (snapshot.signal_support == DTV_NO) {
            erase_operation();
            set_public_error(error, DTV_ERROR_UNSUPPORTED);
            return DTV_ERROR_UNSUPPORTED;
        }
        if (snapshot.signal_support != DTV_YES) {
            erase_operation();
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        dtv_signal signal{};
        signal.struct_size = sizeof(signal);
        signal.lock.state = snapshot.lock_known ? DTV_MEASUREMENT_VALID : DTV_MEASUREMENT_UNKNOWN;
        signal.lock.reason = snapshot.lock_known ? DTV_MEASUREMENT_REASON_NONE
                                                 : snapshot.lock_reason;
        signal.lock.value = snapshot.lock_known ? static_cast<std::int32_t>(snapshot.lock_state) : 0;
        signal.cnr_mdb.state = DTV_MEASUREMENT_UNSUPPORTED;
        signal.cnr_mdb.reason = DTV_MEASUREMENT_REASON_NONE;
        signal.signal_mdbm.state = DTV_MEASUREMENT_UNSUPPORTED;
        signal.signal_mdbm.reason = DTV_MEASUREMENT_REASON_NONE;
        operation_error = operation_status();
        erase_operation();
        if (operation_error != DTV_ERROR_OK) {
            set_public_error(error, operation_error);
            return operation_error;
        }
        *out = signal;
        set_public_error(error, DTV_ERROR_OK);
        return DTV_ERROR_OK;
    } catch (...) {
        const auto code = public_error_code_from_exception();
        set_public_error(error, code);
        return code;
    }
}

extern "C" int32_t dtv_open_enclosure(dtv_enclosure *object,
                                        const dtv_call_options *options,
                                        dtv_error *error)
{
    if (object == nullptr || (options != nullptr && options->struct_size < sizeof(*options))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    auto &state = public_discovery_state();
    const auto *clock = system_runtime_clock();
    if (clock == nullptr || clock->monotonic_now_ns == nullptr || clock->delay_until == nullptr) {
        set_public_error(error, DTV_ERROR_INTERNAL);
        return DTV_ERROR_INTERNAL;
    }
    const auto deadline = deadline_after(clock->monotonic_now_ns(clock->user),
                                         options == nullptr ? 0 : options->timeout_ms);
    EnclosureOpenDescription description{};
    auto result = state.registry.open_description(object, &description);
    if (result.code != DTV_INTERNAL_RESULT_OK) {
        const auto code = public_error_code(result);
        set_public_error(error, code);
        return code;
    }
    const auto *open_ops = description.model == nullptr ? nullptr : description.model->public_open;

    std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
    try {
        slot = std::make_shared<PublicDiscoveryState::OpenSlot>();
        slot->ops = open_ops;
        slot->connections.reserve(description.inputs.size());
        for (const auto &input : description.inputs) slot->connections.push_back(input.connection_epoch);
    } catch (...) {
        set_public_error(error, DTV_ERROR_RESOURCE_LIMIT);
        return DTV_ERROR_RESOURCE_LIMIT;
    }
    {
        std::lock_guard lock(state.open_mutex);
        if (state.open_slots.contains(object) || std::any_of(slot->connections.begin(), slot->connections.end(),
            [&](const auto connection) { return state.claimed_connections.contains(connection); })) {
            set_public_error(error, DTV_ERROR_BUSY);
            return DTV_ERROR_BUSY;
        }
        try {
            state.open_slots.emplace(object, slot);
            for (const auto connection : slot->connections) {
                state.claimed_connections.emplace(connection, object);
            }
        } catch (...) {
            state.open_slots.erase(object);
            for (const auto connection : slot->connections) state.claimed_connections.erase(connection);
            set_public_error(error, DTV_ERROR_RESOURCE_LIMIT);
            return DTV_ERROR_RESOURCE_LIMIT;
        }
        if (!state.registry.hold(object)) {
            state.open_slots.erase(object);
            for (const auto connection : slot->connections) state.claimed_connections.erase(connection);
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        if (!state.registry.retain_open(object)) {
            (void)state.registry.drop(object);
            state.open_slots.erase(object);
            for (const auto connection : slot->connections) state.claimed_connections.erase(connection);
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        slot->held = true;
    }

    auto finish_failure = [&](dtv_internal_result failure) {
        if (failure.code == DTV_INTERNAL_RESULT_OK) {
            failure = dtv_internal_result_make(DTV_INTERNAL_RESULT_INTERNAL, 0);
        }
        std::shared_ptr<EnclosureRuntime> runtime;
        {
            std::shared_ptr<PublicDiscoveryState::SharedInOwner> owner;
            {
                std::lock_guard lock(slot->mutex);
                owner = slot->shared_in;
            }
            if (owner) {
                std::lock_guard lock(owner->mutex);
                owner->stopping = true;
                owner->terminal = failure;
                owner->condition.notify_all();
            }
        }
        const auto stopped_product = stop_open_product(slot, *clock, deadline);
        if (stopped_product.code != DTV_INTERNAL_RESULT_OK && failure.code == DTV_INTERNAL_RESULT_OK) {
            failure = stopped_product;
        }
        {
            std::lock_guard lock(slot->mutex);
            runtime = slot->runtime;
        }
        bool pending = false;
        if (runtime) {
            const auto stopped = runtime->stop(clock, deadline);
            pending = stopped.pending;
            if (stopped.result.code != DTV_INTERNAL_RESULT_OK && failure.code == DTV_INTERNAL_RESULT_OK) {
                failure = stopped.result;
            }
        }
        const auto code = slot->cancelled.load(std::memory_order_acquire)
            ? DTV_ERROR_CANCELLED : public_error_code(failure);
        const auto reported_result = code == public_error_code(failure)
            ? failure : dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        {
            std::lock_guard lock(slot->mutex);
            slot->first_error = code;
            slot->first_error_result = reported_result;
            slot->phase = pending ? PublicDiscoveryState::OpenSlot::Phase::pending
                                  : PublicDiscoveryState::OpenSlot::Phase::closed;
        }
        slot->condition.notify_all();
        set_public_error(error, code, nullptr, &reported_result);
        return code;
    };

    try {
        if (open_ops == nullptr || open_ops->plan == nullptr || open_ops->identify == nullptr) {
            return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0));
        }
        std::vector<std::vector<dtv_usb_endpoint_info>> endpoint_views(description.inputs.size());
        std::vector<std::vector<dtv_usb_interface_info>> interface_views(description.inputs.size());
        std::vector<dtv_usb_candidate_info> observations(description.inputs.size());
        for (std::size_t index = 0; index < description.inputs.size(); ++index) {
            const auto &snapshot = description.inputs[index].snapshot;
            auto &endpoint_view = endpoint_views[index];
            auto &interface_view = interface_views[index];
            std::size_t endpoint_count = 0;
            for (const auto &interface : snapshot.interfaces) endpoint_count += interface.endpoints.size();
            endpoint_view.reserve(endpoint_count);
            interface_view.reserve(snapshot.interfaces.size());
            for (const auto &interface : snapshot.interfaces) {
                endpoint_view.insert(endpoint_view.end(), interface.endpoints.begin(),
                                     interface.endpoints.end());
                auto info = interface.info;
                const auto endpoint_offset = endpoint_view.size() - interface.endpoints.size();
                info.endpoints = interface.endpoints.empty() ? nullptr :
                    endpoint_view.data() + endpoint_offset;
                info.endpoint_count = static_cast<std::uint32_t>(interface.endpoints.size());
                interface_view.push_back(info);
            }
            observations[index] = {snapshot.candidate_id, snapshot.vendor_id, snapshot.product_id,
                snapshot.device_version, snapshot.bus_number, snapshot.device_address,
                snapshot.port_path.empty() ? nullptr : snapshot.port_path.data(),
                static_cast<std::uint32_t>(snapshot.port_path.size()),
                interface_view.empty() ? nullptr : interface_view.data(),
                static_cast<std::uint32_t>(interface_view.size())};
        }
        std::vector<dtv::spi::PublicOpenPlan> plans(description.inputs.size());
        std::size_t plan_count = 0;
        result = call_callback([&] {
            return open_ops->plan(observations, description.functions, plans, &plan_count);
        });
        if (result.code != DTV_INTERNAL_RESULT_OK) return finish_failure(result);
        if (plan_count == 0 || plan_count > plans.size()) {
            return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0));
        }
        plans.resize(plan_count);
        constexpr std::size_t shared_in_limit = 1024 * 1024;
        constexpr std::size_t prestream_limit = 1024 * 1024;
        if (plans.size() > 64) {
            return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0));
        }
        std::size_t total_prestream = 0;
        std::size_t total_requests = 0;
        auto card_reader_handle = (std::numeric_limits<std::size_t>::max)();
        for (const auto &plan : plans) {
            if (plan.handle_index >= description.inputs.size() || plan.endpoint == 0 ||
                plan.buffer_capacity == 0 ||
                plan.buffer_capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
                (plan.receiver_lock_source && plan.shared_in_depth == 0) ||
                (plan.shared_in_depth != 0 && (plan.prestream_sink_limit == 0 ||
                    plan.reply_capacity == 0 || open_ops->demux == nullptr ||
                    open_ops->identify_shared == nullptr || plan.buffer_capacity > shared_in_limit ||
                    plan.prestream_sink_limit > prestream_limit ||
                    plan.reply_capacity > shared_in_limit ||
                    plan.prestream_sink_limit > prestream_limit - total_prestream)) ||
                (plan.shared_in_depth == 0 && (plan.prestream_sink_limit != 0 ||
                    plan.reply_capacity != 0)) ||
                !has_bulk_in_endpoint(description.inputs[plan.handle_index], plan.endpoint)) {
                return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0));
            }
            if (plan.card_reader_source) {
                if (plan.shared_in_depth == 0 ||
                    card_reader_handle != (std::numeric_limits<std::size_t>::max)()) {
                    return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0));
                }
                card_reader_handle = plan.handle_index;
            }
            const auto request_count = plan.shared_in_depth == 0 ? 1 : plan.shared_in_depth;
            if (request_count > 64 - total_requests) {
                return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0));
            }
            total_requests += request_count;
            if (plan.shared_in_depth != 0) total_prestream += plan.prestream_sink_limit;
        }
        std::vector<dtv::spi::PublicOpenPlan> request_plans;
        request_plans.reserve(total_requests);
        for (const auto &plan : plans) {
            const auto request_count = plan.shared_in_depth == 0 ? 1 : plan.shared_in_depth;
            for (std::size_t index = 0; index < request_count; ++index)
                request_plans.push_back(plan);
        }
        const bool has_shared_in = std::any_of(plans.begin(), plans.end(),
            [](const auto &plan) { return plan.shared_in_depth != 0; });
        if (card_reader_handle != (std::numeric_limits<std::size_t>::max)()) {
            result = state.registry.bind_card_reader(object,
                description.inputs[card_reader_handle].connection_epoch);
            if (result.code != DTV_INTERNAL_RESULT_OK) return finish_failure(result);
        }

        UsbClaimLeaseGroup claims;
        result = state.session.acquire_claims(description.inputs, clock, nullptr, deadline, &claims);
        if (result.code != DTV_INTERNAL_RESULT_OK) return finish_failure(result);
        dtv_internal_result start_result{};
        auto runtime = EnclosureRuntime::start(std::move(claims), &start_result);
        if (!runtime) return finish_failure(start_result);
        auto shared_runtime = std::shared_ptr<EnclosureRuntime>(std::move(runtime));
        {
            std::lock_guard lock(slot->mutex);
            slot->runtime = shared_runtime;
        }
        slot->condition.notify_all();

        std::shared_ptr<dtv::spi::ProductInstance> instance;
        std::shared_ptr<PublicDiscoveryState::SharedInOwner> shared_owner;
        if (has_shared_in) {
            instance = std::make_shared<dtv::spi::ProductInstance>(*description.model,
                dtv_get_device_id(dtv_get_devices(object)[0]));
            if (instance->result().code != DTV_INTERNAL_RESULT_OK) return finish_failure(instance->result());
            shared_owner = std::make_shared<PublicDiscoveryState::SharedInOwner>();
            shared_owner->ops = open_ops;
            shared_owner->context = instance->context();
            shared_owner->registry = &state.registry;
            shared_owner->enclosure = object;
            shared_owner->card_reader_handle_index = card_reader_handle;
            shared_owner->generations.resize(description.inputs.size());
            for (std::size_t index = 0; index < description.inputs.size(); ++index)
                shared_owner->generations[index] = description.inputs[index].connection_epoch;
            std::size_t sink_capacity = 0;
            for (const auto &plan : plans) {
                if (plan.shared_in_depth == 0) continue;
                if (plan.prestream_sink_limit > (std::numeric_limits<std::size_t>::max)() - sink_capacity)
                    return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0));
                const auto endpoint = std::pair{plan.handle_index, plan.endpoint};
                if (std::find(shared_owner->endpoints.begin(), shared_owner->endpoints.end(), endpoint) ==
                    shared_owner->endpoints.end())
                    shared_owner->endpoints.push_back(endpoint);
                if (plan.receiver_lock_source &&
                    std::find(shared_owner->lock_sources.begin(), shared_owner->lock_sources.end(), endpoint) ==
                        shared_owner->lock_sources.end())
                    shared_owner->lock_sources.push_back(endpoint);
                sink_capacity += plan.prestream_sink_limit;
                shared_owner->reply_capacity = (std::max)(shared_owner->reply_capacity, plan.reply_capacity);
            }
            shared_owner->last_sequences.resize(shared_owner->endpoints.size());
            shared_owner->prestream.resize(sink_capacity);
            for (auto &reply : shared_owner->replies) reply.bytes.resize(shared_owner->reply_capacity);
            for (auto &waiter : shared_owner->waiters) waiter.bytes.resize(shared_owner->reply_capacity);
            {
                std::lock_guard lock(slot->mutex);
                slot->product_instance = instance;
                slot->shared_in = shared_owner;
            }
        }

        std::vector<std::uint64_t> initial_requests;
        initial_requests.reserve(request_plans.size());
        for (const auto &plan : request_plans) {
            if (slot->cancelled.load(std::memory_order_acquire)) {
                return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0));
            }
            std::uint64_t request = 0;
            result = plan.shared_in_depth != 0
                ? shared_runtime->submit_shared_bulk_in(plan.handle_index, plan.endpoint,
                    plan.buffer_capacity, description.inputs[plan.handle_index].connection_epoch,
                    PublicDiscoveryState::SharedInOwner::receive, shared_owner.get(), &request)
                : shared_runtime->submit_bulk_in(plan.handle_index, plan.endpoint,
                    plan.buffer_capacity, description.inputs[plan.handle_index].connection_epoch,
                    clock, nullptr, deadline, &request);
            if (result.code != DTV_INTERNAL_RESULT_OK) return finish_failure(result);
            {
                std::lock_guard lock(slot->mutex);
                slot->requests.push_back(request);
            }
            if (slot->cancelled.load(std::memory_order_acquire)) (void)shared_runtime->cancel(request);
            initial_requests.push_back(request);
        }
        for (const auto request : initial_requests) {
            UsbRuntimeTerminal terminal{};
            result = shared_runtime->wait_request(request, clock, deadline, &terminal);
            if (result.code != DTV_INTERNAL_RESULT_OK || terminal != UsbRuntimeTerminal::completed) {
                return finish_failure(result.code == DTV_INTERNAL_RESULT_OK
                    ? dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0) : result);
            }
        }
        std::map<std::uint64_t, UsbRuntimeCompletion> completions;
        while (completions.size() < initial_requests.size()) {
            UsbRuntimeCompletion completion;
            bool available = false;
            result = shared_runtime->next_completion(&completion, &available);
            if (result.code != DTV_INTERNAL_RESULT_OK) return finish_failure(result);
            if (available) {
                completions.emplace(completion.request_id, std::move(completion));
                continue;
            }
            if (slot->cancelled.load(std::memory_order_acquire)) {
                return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0));
            }
            const auto now = clock->monotonic_now_ns(clock->user);
            if (deadline != UINT64_MAX && now >= deadline) {
                return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0));
            }
            const auto wake = deadline == UINT64_MAX ? now + UINT64_C(1000000)
                : (std::min)(deadline, now + UINT64_C(1000000));
            std::uint32_t cancelled = 0;
            result = clock->delay_until(clock->user, wake, deadline, nullptr, &cancelled);
            if (result.code != DTV_INTERNAL_RESULT_OK && result.code != DTV_INTERNAL_RESULT_TIMEOUT) {
                return finish_failure(result);
            }
        }
        std::vector<unsigned char> initial_bytes;
        for (std::size_t index = 0; index < initial_requests.size(); ++index) {
            const auto request = initial_requests[index];
            const auto found = completions.find(request);
            if (found == completions.end() ||
                found->second.terminal != UsbRuntimeTerminal::completed ||
                found->second.handle_index != request_plans[index].handle_index ||
                found->second.endpoint != request_plans[index].endpoint ||
                found->second.generation != description.inputs[request_plans[index].handle_index].connection_epoch ||
                found->second.result.code != DTV_INTERNAL_RESULT_OK) {
                return finish_failure(found == completions.end()
                    ? dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0)
                    : found->second.result);
            }
            initial_bytes.insert(initial_bytes.end(), found->second.bytes.begin(),
                                 found->second.bytes.end());
        }
        if (!instance) instance = std::make_shared<dtv::spi::ProductInstance>(*description.model,
            dtv_get_device_id(dtv_get_devices(object)[0]));
        if (instance->result().code != DTV_INTERNAL_RESULT_OK) return finish_failure(instance->result());
        if (!has_shared_in) {
            std::lock_guard lock(slot->mutex);
            slot->product_instance = instance;
        }
        if (shared_owner != nullptr) {
            dtv::spi::PublicOpenSession session{shared_owner.get(),
                PublicDiscoveryState::SharedInOwner::register_waiter,
                PublicDiscoveryState::SharedInOwner::wait_waiter,
                PublicDiscoveryState::SharedInOwner::cancel_waiter,
            PublicDiscoveryState::SharedInOwner::get_generation,
            PublicDiscoveryState::SharedInOwner::abandon_waiter};
            result = call_callback([&] {
                return open_ops->identify_shared(instance->context(), initial_bytes,
                    remaining_timeout(*clock, deadline), &slot->cancelled, &session);
            });
        } else {
            result = call_callback([&] {
                return open_ops->identify(instance->context(), initial_bytes,
                                          remaining_timeout(*clock, deadline), &slot->cancelled);
            });
        }
        if (result.code != DTV_INTERNAL_RESULT_OK) return finish_failure(result);
        if (slot->cancelled.load(std::memory_order_acquire)) {
            return finish_failure(dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0));
        }
        {
            std::lock_guard lock(slot->mutex);
            slot->phase = PublicDiscoveryState::OpenSlot::Phase::open;
        }
        slot->condition.notify_all();
        set_public_error(error, DTV_ERROR_OK);
        return DTV_ERROR_OK;
    } catch (...) {
        return finish_failure(exception_result());
    }
}

extern "C" uint32_t dtv_close_enclosure(dtv_enclosure *object,
                                           const dtv_call_options *options,
                                           dtv_error *error)
{
    if (object == nullptr || (options != nullptr && options->struct_size < sizeof(*options))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    auto &state = public_discovery_state();
    std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
    {
        std::lock_guard lock(state.open_mutex);
        const auto found = state.open_slots.find(object);
        if (found == state.open_slots.end()) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
            return 0;
        }
        slot = found->second;
    }
    const auto *clock = system_runtime_clock();
    const auto deadline = deadline_after(clock->monotonic_now_ns(clock->user),
                                         options == nullptr ? 0 : options->timeout_ms);
    slot->cancelled.store(true, std::memory_order_release);
    slot->cancelled.notify_all();
    cancel_open_operations(slot, true);
    const auto streams_closed = close_public_streams(slot, *clock, deadline);
    if (streams_closed.code != DTV_INTERNAL_RESULT_OK) {
        {
            std::lock_guard lock(slot->mutex);
            slot->phase = PublicDiscoveryState::OpenSlot::Phase::pending;
        }
        {
            std::lock_guard lock(slot->stream_mutex);
            slot->stream_close_pending = true;
        }
        set_public_error(error, DTV_ERROR_OK);
        return DTV_CLOSE_PENDING;
    }
    {
        std::lock_guard lock(slot->stream_mutex);
        slot->stream_close_pending = false;
    }
    std::shared_ptr<EnclosureRuntime> runtime;
    {
        std::lock_guard lock(slot->mutex);
        runtime = slot->runtime;
        if (slot->shared_in) {
            std::lock_guard owner_lock(slot->shared_in->mutex);
            slot->shared_in->stopping = true;
            slot->shared_in->terminal = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
            slot->shared_in->condition.notify_all();
        }
        for (const auto request : slot->requests) {
            if (runtime) (void)runtime->cancel(request);
        }
    }
    if (!runtime) {
        std::unique_lock lock(slot->mutex);
        while (slot->phase == PublicDiscoveryState::OpenSlot::Phase::opening) {
            if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline) {
                slot->phase = PublicDiscoveryState::OpenSlot::Phase::pending;
                set_public_error(error, DTV_ERROR_OK);
                return DTV_CLOSE_PENDING;
            }
            slot->condition.wait_for(lock, std::chrono::milliseconds(5));
        }
        runtime = slot->runtime;
    }
    if (!wait_for_operation_callers(slot, *clock, deadline)) {
        std::lock_guard lock(slot->mutex);
        slot->phase = PublicDiscoveryState::OpenSlot::Phase::pending;
        set_public_error(error, DTV_ERROR_OK);
        return DTV_CLOSE_PENDING;
    }
    const auto product_stop = stop_open_product(slot, *clock, deadline);
    if (runtime) {
        const auto stopped = runtime->stop(clock, deadline);
        if (stopped.pending) {
            std::lock_guard lock(slot->mutex);
            slot->phase = PublicDiscoveryState::OpenSlot::Phase::pending;
            const auto code = product_stop.code == DTV_INTERNAL_RESULT_OK
                ? DTV_ERROR_OK : public_error_code(product_stop);
            if (product_stop.code != DTV_INTERNAL_RESULT_OK) {
                slot->first_error = code;
                slot->first_error_result = product_stop;
            }
            set_public_error(error, code, nullptr,
                product_stop.code == DTV_INTERNAL_RESULT_OK ? nullptr : &product_stop);
            return DTV_CLOSE_PENDING;
        }
        {
            std::lock_guard lock(slot->mutex);
            slot->phase = PublicDiscoveryState::OpenSlot::Phase::closed;
            if (product_stop.code != DTV_INTERNAL_RESULT_OK) {
                slot->first_error = public_error_code(product_stop);
                slot->first_error_result = product_stop;
            } else if (stopped.result.code != DTV_INTERNAL_RESULT_OK) {
                slot->first_error = public_error_code(stopped.result);
                slot->first_error_result = stopped.result;
            }
        }
        clear_operation_tickets(slot);
        const auto stop_error = product_stop.code != DTV_INTERNAL_RESULT_OK
            ? public_error_code(product_stop) : public_error_code(stopped.result);
        set_public_error(error, stop_error, nullptr,
            product_stop.code != DTV_INTERNAL_RESULT_OK ? &product_stop : &stopped.result);
        return DTV_CLOSE_CLOSED;
    }
    std::uint32_t error_code = DTV_ERROR_OK;
    dtv_internal_result error_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    {
        std::lock_guard lock(slot->mutex);
        slot->phase = PublicDiscoveryState::OpenSlot::Phase::closed;
        error_code = slot->first_error;
        error_result = slot->first_error_result;
    }
    if (product_stop.code != DTV_INTERNAL_RESULT_OK) {
        error_code = public_error_code(product_stop);
        error_result = product_stop;
    }
    set_public_error(error, error_code, nullptr, &error_result);
    return DTV_CLOSE_CLOSED;
}

extern "C" uint32_t dtv_get_enclosure_close_state(dtv_enclosure *object, dtv_error *error)
{
    if (object == nullptr) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    auto &state = public_discovery_state();
    std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
    {
        std::lock_guard lock(state.open_mutex);
        const auto found = state.open_slots.find(object);
        if (found == state.open_slots.end()) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
            return 0;
        }
        slot = found->second;
    }
    bool stream_close_pending = false;
    {
        std::lock_guard lock(slot->stream_mutex);
        stream_close_pending = slot->stream_close_pending;
    }
    if (stream_close_pending) return dtv_close_enclosure(object, nullptr, error);
    std::shared_ptr<EnclosureRuntime> runtime;
    bool close_pending = false;
    {
        std::lock_guard lock(slot->mutex);
        if (slot->phase == PublicDiscoveryState::OpenSlot::Phase::closed) {
            set_public_error(error, slot->first_error, nullptr, &slot->first_error_result);
            return DTV_CLOSE_CLOSED;
        }
        runtime = slot->runtime;
        close_pending = slot->phase == PublicDiscoveryState::OpenSlot::Phase::pending;
    }
    if (close_pending) {
        const dtv_call_options continue_close{sizeof(dtv_call_options), 1};
        return dtv_close_enclosure(object, &continue_close, error);
    }
    if (!runtime || runtime->worker_count() != 0) {
        set_public_error(error, DTV_ERROR_OK);
        return DTV_CLOSE_PENDING;
    }
    {
        std::lock_guard lock(slot->operation_mutex);
        if (std::any_of(slot->operations.begin(), slot->operations.end(), [](const auto &operation) {
                return operation && !operation->caller_done;
            })) {
            set_public_error(error, DTV_ERROR_OK);
            return DTV_CLOSE_PENDING;
        }
    }
    const auto result = runtime->join_release();
    if (result.code != DTV_INTERNAL_RESULT_OK) {
        set_public_error(error, public_error_code(result), nullptr, &result);
        return DTV_CLOSE_PENDING;
    }
    std::uint32_t first_error = DTV_ERROR_OK;
    dtv_internal_result first_error_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    {
        std::lock_guard lock(slot->mutex);
        slot->phase = PublicDiscoveryState::OpenSlot::Phase::closed;
        first_error = slot->first_error;
        first_error_result = slot->first_error_result;
    }
    clear_operation_tickets(slot);
    set_public_error(error, first_error, nullptr, &first_error_result);
    return DTV_CLOSE_CLOSED;
}

extern "C" void dtv_release_enclosure(dtv_enclosure *object)
{
    if (object == nullptr) return;
    auto &state = public_discovery_state();
    std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
    {
        std::lock_guard lock(state.open_mutex);
        const auto found = state.open_slots.find(object);
        if (found == state.open_slots.end()) return;
        slot = found->second;
        {
            std::lock_guard slot_lock(slot->mutex);
            if (slot->phase != PublicDiscoveryState::OpenSlot::Phase::closed) return;
        }
        state.open_slots.erase(found);
        for (const auto connection : slot->connections) state.claimed_connections.erase(connection);
    }
    if (slot->held) {
        (void)state.registry.release_open(object);
        (void)state.registry.drop(object);
        slot->held = false;
    }
}

extern "C" int32_t dtv_open_stream(dtv_receiver *receiver, const dtv_call_options *options,
                                    dtv_stream **out, dtv_error *error)
{
    if (receiver == nullptr || out == nullptr ||
        (options != nullptr && options->struct_size < sizeof(*options))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    try {
        auto &state = public_discovery_state();
        EnclosureRegistry::ReceiverSignalSnapshot snapshot;
        auto result = state.registry.receiver_signal_snapshot(receiver, &snapshot);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            const auto code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        if (snapshot.enclosure_status == DTV_ENCLOSURE_DISCONNECTED) {
            set_public_error(error, DTV_ERROR_DISCONNECTED);
            return DTV_ERROR_DISCONNECTED;
        }
        EnclosureOpenDescription description{};
        result = state.registry.open_description(snapshot.enclosure, &description);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            const auto code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        if (description.model == nullptr || snapshot.source_stride_bytes == 0) {
            set_public_error(error, DTV_ERROR_UNSUPPORTED);
            return DTV_ERROR_UNSUPPORTED;
        }
        if (!((snapshot.source_stride_bytes == 188 && snapshot.source_sync_offset_bytes == 0) ||
              (snapshot.source_stride_bytes == 192 && snapshot.source_sync_offset_bytes == 4))) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
        {
            std::lock_guard lock(state.open_mutex);
            const auto found = state.open_slots.find(snapshot.enclosure);
            if (found != state.open_slots.end()) slot = found->second;
        }
        if (!slot) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        {
            std::lock_guard lock(slot->mutex);
            if (slot->phase != PublicDiscoveryState::OpenSlot::Phase::open ||
                !slot->product_instance || slot->ops == nullptr || !slot->shared_in) {
                set_public_error(error, DTV_ERROR_NOT_READY);
                return DTV_ERROR_NOT_READY;
            }
        }
        if (slot->ops->plan_stream == nullptr || slot->ops->prepare_stream == nullptr ||
            slot->ops->start_stream == nullptr || slot->ops->stop_stream == nullptr ||
            slot->ops->after_stream_drain == nullptr) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        if (snapshot.source_stride_bytes == 192 && slot->ops->transform_stream == nullptr) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        auto stream_state = std::make_shared<PublicStreamState>();
        stream_state->slot = slot;
        stream_state->shared_in = slot->shared_in;
        stream_state->registry = &state.registry;
        stream_state->enclosure = snapshot.enclosure;
        stream_state->receiver = receiver;
        stream_state->receiver_index = snapshot.receiver_index;
        stream_state->connection_epoch = snapshot.connection_epoch;
        stream_state->source_stride_bytes = snapshot.source_stride_bytes;
        stream_state->source_sync_offset_bytes = snapshot.source_sync_offset_bytes;
        stream_state->framer = std::make_unique<dtv::internal::TsFramer>(
            stream_state->source_stride_bytes, stream_state->source_sync_offset_bytes, 2, 4096);
        if (!stream_state->framer->valid()) {
            set_public_error(error, DTV_ERROR_INTERNAL);
            return DTV_ERROR_INTERNAL;
        }
        stream_state->bytes.reserve(stream_state->queue_capacity);
        auto handle = std::make_unique<dtv_stream>();
        if (!state.registry.hold(snapshot.enclosure)) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        if (!state.registry.retain_open(snapshot.enclosure)) {
            (void)state.registry.drop(snapshot.enclosure);
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        stream_state->references_held = true;
        auto *clock = system_runtime_clock();
        const auto deadline = deadline_after(clock->monotonic_now_ns(clock->user),
            options == nullptr ? 0 : options->timeout_ms);
        {
            std::lock_guard lock(stream_state->shared_in->mutex);
            if (stream_state->shared_in->stopping ||
                stream_state->shared_in->streams.contains(snapshot.receiver_index)) {
                (void)state.registry.release_open(snapshot.enclosure);
                (void)state.registry.drop(snapshot.enclosure);
                stream_state->references_held = false;
                set_public_error(error, stream_state->shared_in->stopping
                    ? DTV_ERROR_NOT_READY : DTV_ERROR_BUSY);
                return stream_state->shared_in->stopping ? DTV_ERROR_NOT_READY : DTV_ERROR_BUSY;
            }
            stream_state->stream_generation = stream_state->shared_in->next_stream_generation++;
            if (stream_state->stream_generation == 0)
                stream_state->stream_generation = stream_state->shared_in->next_stream_generation++;
            stream_state->shared_in->prestream_claimed = true;
            stream_state->shared_in->prestream_size = 0;
            stream_state->shared_in->streams.emplace(snapshot.receiver_index,
                stream_state);
        }
        const dtv::spi::PublicStreamRequest request{snapshot.receiver_index,
            snapshot.connection_epoch, stream_state->stream_generation,
            snapshot.source_stride_bytes, snapshot.source_sync_offset_bytes,
            stream_state->queue_capacity, &stream_state->cancelled};
        try {
            std::lock_guard lock(slot->stream_mutex);
            slot->streams.push_back(stream_state);
        } catch (...) {
            detach_public_stream(stream_state);
            {
                std::lock_guard lock(stream_state->mutex);
                stream_state->closed = true;
                stream_state->opening = false;
                stream_state->condition.notify_all();
            }
            release_public_stream_references(stream_state);
            const auto code = public_error_code_from_exception();
            set_public_error(error, code);
            return code;
        }
        dtv::spi::PublicStreamPlan plan{};
        auto callback_result = call_callback([&] {
            return slot->ops->plan_stream(slot->product_instance->context(), &request, &plan);
        });
        if (callback_result.code == DTV_INTERNAL_RESULT_OK && !plan.supported)
            callback_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
        if (callback_result.code == DTV_INTERNAL_RESULT_OK && !plan.ready)
            callback_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
        if (callback_result.code == DTV_INTERNAL_RESULT_OK && !plan.arguments_valid)
            callback_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        if (callback_result.code == DTV_INTERNAL_RESULT_OK) {
            stream_state->ticket = acquire_public_stream_ticket(slot, stream_state->stream_generation,
                plan.resource_keys, *clock, deadline, &callback_result);
        }
        if (callback_result.code == DTV_INTERNAL_RESULT_OK &&
            stream_state->cancelled.load(std::memory_order_acquire))
            callback_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
        if (callback_result.code == DTV_INTERNAL_RESULT_OK &&
            state.registry.status(snapshot.enclosure) == DTV_ENCLOSURE_DISCONNECTED)
            callback_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_DISCONNECTED, 0);
        if (callback_result.code == DTV_INTERNAL_RESULT_OK) {
            std::lock_guard lock(slot->mutex);
            if (slot->phase != PublicDiscoveryState::OpenSlot::Phase::open)
                callback_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
        }
        if (callback_result.code == DTV_INTERNAL_RESULT_OK) {
            callback_result = call_callback([&] {
                return slot->ops->prepare_stream(slot->product_instance->context(), &request);
            });
            if (callback_result.code == DTV_INTERNAL_RESULT_OK) stream_state->prepared = true;
        }
        bool producer_started = false;
        if (callback_result.code == DTV_INTERNAL_RESULT_OK) {
            callback_result = call_callback([&] {
                return slot->ops->start_stream(slot->product_instance->context(), &request,
                                                &producer_started);
            });
            stream_state->producer_started = producer_started;
        }
        if (callback_result.code == DTV_INTERNAL_RESULT_OK &&
            (stream_state->cancelled.load(std::memory_order_acquire) ||
             stream_state->ticket->cancelled.load(std::memory_order_acquire)))
            callback_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
        if (callback_result.code == DTV_INTERNAL_RESULT_OK) {
            std::lock_guard lock(slot->mutex);
            if (slot->phase != PublicDiscoveryState::OpenSlot::Phase::open)
                callback_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_CANCELLED, 0);
        }
        if (callback_result.code != DTV_INTERNAL_RESULT_OK) {
            {
                std::lock_guard lock(stream_state->shared_in->mutex);
                const auto found = stream_state->shared_in->streams.find(snapshot.receiver_index);
                if (found != stream_state->shared_in->streams.end())
                    stream_state->shared_in->streams.erase(found);
            }
            if (producer_started) {
                (void)call_callback([&] {
                    return slot->ops->stop_stream(slot->product_instance->context(), &request,
                                                  remaining_timeout(*clock, deadline));
                });
            }
            if (stream_state->prepared) {
                (void)call_callback([&] {
                    return slot->ops->after_stream_drain(slot->product_instance->context(), &request);
                });
            }
            {
                std::lock_guard lock(stream_state->mutex);
                stream_state->closed = true;
                stream_state->opening = false;
                stream_state->condition.notify_all();
            }
            release_public_stream_references(stream_state);
            const auto code = public_error_code(callback_result);
            set_public_error(error, code, nullptr, &callback_result);
            return code;
        }
        handle->state = stream_state;
        *out = handle.release();
        {
            std::lock_guard lock(stream_state->slot->operation_mutex);
            stream_state->ticket->caller_done = true;
            stream_state->slot->operation_condition.notify_all();
        }
        {
            std::lock_guard lock(stream_state->mutex);
            stream_state->opening = false;
            stream_state->condition.notify_all();
        }
        set_public_error(error, DTV_ERROR_OK);
        return DTV_ERROR_OK;
    } catch (...) {
        const auto code = public_error_code_from_exception();
        set_public_error(error, code);
        return code;
    }
}

extern "C" uint64_t dtv_get_dropped_bytes(dtv_stream *stream)
{
    if (stream == nullptr || stream->state == nullptr) return 0;
    const auto *state = static_cast<const PublicStreamState *>(stream->state.get());
    return state->dropped_bytes;
}

extern "C" uint64_t dtv_get_desyncs(dtv_stream *stream)
{
    if (stream == nullptr || stream->state == nullptr) return 0;
    const auto *state = static_cast<const PublicStreamState *>(stream->state.get());
    return state->desyncs;
}

extern "C" int32_t dtv_set_pid_filter(dtv_stream *stream, const uint16_t *pids,
                                       uint32_t count, dtv_error *error)
{
    if (stream == nullptr || stream->state == nullptr || (count != 0 && pids == nullptr)) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    auto shared = std::static_pointer_cast<PublicStreamState>(stream->state);
    {
        std::lock_guard lock(shared->mutex);
        if (shared->closed || shared->closing ||
            shared->cancelled.load(std::memory_order_acquire)) {
            set_public_error(error, DTV_ERROR_CANCELLED);
            return DTV_ERROR_CANCELLED;
        }
        if (shared->terminal) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
    }
    try {
        std::vector<std::uint16_t> owned_pids;
        owned_pids.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (pids[index] > 0x1fff) {
                set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
                return DTV_ERROR_INVALID_ARGUMENT;
            }
            owned_pids.push_back(pids[index]);
        }
        auto &registry = public_discovery_state().registry;
        EnclosureRegistry::ReceiverSignalSnapshot snapshot;
        auto result = registry.receiver_signal_snapshot(shared->receiver, &snapshot);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            const auto code = public_error_code(result);
            set_public_error(error, code, nullptr, &result);
            return code;
        }
        if (snapshot.enclosure_status == DTV_ENCLOSURE_DISCONNECTED ||
            snapshot.connection_epoch != shared->connection_epoch) {
            set_public_error(error, DTV_ERROR_DISCONNECTED);
            return DTV_ERROR_DISCONNECTED;
        }
        std::uint32_t support = DTV_UNKNOWN;
        const auto support_result = registry.receiver_support(shared->receiver,
            DTV_OP_OUTPUT_PID_FILTER, &support, error);
        if (support_result != DTV_ERROR_OK) return support_result;
        if (support == DTV_NO) {
            set_public_error(error, DTV_ERROR_UNSUPPORTED);
            return DTV_ERROR_UNSUPPORTED;
        }
        if (support != DTV_YES) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        const auto *product = registry.product(shared->enclosure);
        const auto *model = registry.model(shared->enclosure);
        if (product == nullptr || model == nullptr) {
            set_public_error(error, DTV_ERROR_NOT_READY);
            return DTV_ERROR_NOT_READY;
        }
        const auto result_code = dtv::internal::dispatch_set_pid_filter(product, model, stream,
            owned_pids.data(), count, error);
        if (result_code != DTV_ERROR_OK) set_public_error(error, result_code);
        return result_code;
    } catch (...) {
        const auto code = public_error_code_from_exception();
        set_public_error(error, code);
        return code;
    }
}

extern "C" int32_t dtv_read_stream(dtv_stream *stream, uint8_t *buffer, uint64_t capacity,
    const dtv_call_options *options, uint64_t *out_size, dtv_error *error)
{
    if (stream == nullptr || stream->state == nullptr || out_size == nullptr ||
        (capacity != 0 && buffer == nullptr) ||
        (options != nullptr && options->struct_size < sizeof(*options))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    *out_size = 0;
    if (capacity < 188) {
        set_public_error(error, DTV_ERROR_BUFFER_TOO_SMALL);
        return DTV_ERROR_BUFFER_TOO_SMALL;
    }
    auto &state = *static_cast<PublicStreamState *>(stream->state.get());
    const auto *clock = system_runtime_clock();
    const auto deadline = options == nullptr || options->timeout_ms == 0
        ? UINT64_MAX : deadline_after(clock->monotonic_now_ns(clock->user), options->timeout_ms);
    std::unique_lock lock(state.mutex);
    if (state.closed || state.closing || state.cancelled.load(std::memory_order_acquire)) {
        set_public_error(error, DTV_ERROR_CANCELLED);
        return DTV_ERROR_CANCELLED;
    }
    ++state.active_reads;
    const auto finish = [&](std::int32_t code, const dtv_internal_result *result = nullptr) {
        --state.active_reads;
        state.condition.notify_all();
        set_public_error(error, code, nullptr, result);
        return code;
    };
    while (state.bytes.size() < 188 && !state.terminal &&
           !state.cancelled.load(std::memory_order_acquire) && !state.closing) {
        if (deadline != UINT64_MAX && clock->monotonic_now_ns(clock->user) >= deadline)
            return finish(DTV_ERROR_TIMEOUT);
        state.condition.wait_for(lock, std::chrono::milliseconds(2));
    }
    if (state.cancelled.load(std::memory_order_acquire) || state.closing || state.closed)
        return finish(DTV_ERROR_CANCELLED);
    if (state.bytes.size() < 188) {
        if (state.terminal_error != DTV_ERROR_OK)
            return finish(static_cast<std::int32_t>(state.terminal_error), &state.terminal_result);
        return finish(DTV_ERROR_OK);
    }
    const auto capacity_size = capacity > (std::numeric_limits<std::size_t>::max)()
        ? (std::numeric_limits<std::size_t>::max)() : static_cast<std::size_t>(capacity);
    const auto count = (std::min)(state.bytes.size(), capacity_size - capacity_size % 188);
    std::copy_n(state.bytes.begin(), count, buffer);
    state.bytes.erase(state.bytes.begin(), state.bytes.begin() + count);
    *out_size = count;
    return finish(DTV_ERROR_OK);
}

extern "C" int32_t dtv_cancel_stream(dtv_stream *stream, dtv_error *error)
{
    if (stream == nullptr || stream->state == nullptr) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    auto &state = *static_cast<PublicStreamState *>(stream->state.get());
    {
        std::lock_guard lock(state.mutex);
        state.cancelled.store(true, std::memory_order_release);
    }
    cancel_public_stream_operations(
        std::static_pointer_cast<PublicStreamState>(stream->state));
    state.condition.notify_all();
    set_public_error(error, DTV_ERROR_OK);
    return DTV_ERROR_OK;
}

extern "C" uint32_t dtv_close_stream(dtv_stream *stream, const dtv_call_options *options,
                                     dtv_error *error)
{
    if (stream == nullptr || stream->state == nullptr ||
        (options != nullptr && options->struct_size < sizeof(*options))) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    auto &state = *static_cast<PublicStreamState *>(stream->state.get());
    const auto *clock = system_runtime_clock();
    const auto deadline = deadline_after(clock->monotonic_now_ns(clock->user),
        options == nullptr ? 0 : options->timeout_ms);
    bool call_stop = false;
    {
        std::lock_guard lock(state.mutex);
        if (state.closed) {
            set_public_error(error, static_cast<std::int32_t>(state.close_error), nullptr,
                             &state.close_error_result);
            return DTV_CLOSE_CLOSED;
        }
        state.closing = true;
        state.cancelled.store(true, std::memory_order_release);
        if (!state.producer_started) state.stop_complete = true;
        if (state.producer_started && !state.stop_complete && !state.stop_in_progress) {
            state.stop_in_progress = true;
            state.stop_called = true;
            call_stop = true;
        }
        state.condition.notify_all();
    }
    cancel_public_stream_operations(
        std::static_pointer_cast<PublicStreamState>(stream->state));
    if (call_stop) {
        const auto request = stream_request(state);
        const auto result = call_callback([&] {
            return state.slot->ops->stop_stream(state.slot->product_instance->context(), &request,
                                                 remaining_timeout(*clock, deadline));
        });
        std::lock_guard lock(state.mutex);
        state.stop_in_progress = false;
        if (result.code != DTV_INTERNAL_RESULT_TIMEOUT) state.stop_complete = true;
        if (result.code != DTV_INTERNAL_RESULT_OK && state.close_error == DTV_ERROR_OK) {
            state.close_error = public_error_code(result);
            state.close_error_result = result;
        }
        state.condition.notify_all();
    }
    detach_public_stream(std::static_pointer_cast<PublicStreamState>(stream->state));
    auto shared = std::static_pointer_cast<PublicStreamState>(stream->state);
    if (!finish_public_stream_close(shared, *clock, deadline)) {
        set_public_error(error, DTV_ERROR_OK);
        return DTV_CLOSE_PENDING;
    }
    set_public_error(error, static_cast<std::int32_t>(state.close_error), nullptr,
                     &state.close_error_result);
    return DTV_CLOSE_CLOSED;
}

extern "C" uint32_t dtv_get_stream_close_state(dtv_stream *stream, dtv_error *error)
{
    if (stream == nullptr || stream->state == nullptr) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    auto shared = std::static_pointer_cast<PublicStreamState>(stream->state);
    auto &state = *shared;
    const auto *clock = system_runtime_clock();
    bool closed = false;
    bool retry_stop = false;
    {
        std::lock_guard lock(state.mutex);
        closed = state.closed;
        retry_stop = state.closing && state.producer_started && !state.stop_complete &&
            !state.stop_in_progress;
        if (retry_stop) state.stop_in_progress = true;
    }
    if (!closed && retry_stop) {
        const auto request = stream_request(state);
        const auto result = call_callback([&] {
            return state.slot->ops->stop_stream(state.slot->product_instance->context(), &request, {});
        });
        std::lock_guard lock(state.mutex);
        state.stop_in_progress = false;
        if (result.code != DTV_INTERNAL_RESULT_TIMEOUT) state.stop_complete = true;
        if (result.code != DTV_INTERNAL_RESULT_OK && state.close_error == DTV_ERROR_OK) {
            state.close_error = public_error_code(result);
            state.close_error_result = result;
        }
        state.condition.notify_all();
    }
    if (!closed) closed = finish_public_stream_close(shared, *clock, UINT64_MAX);
    std::lock_guard lock(state.mutex);
    set_public_error(error, closed ? static_cast<std::int32_t>(state.close_error) : DTV_ERROR_OK,
                     nullptr, closed ? &state.close_error_result : nullptr);
    return closed ? DTV_CLOSE_CLOSED : DTV_CLOSE_PENDING;
}

extern "C" void dtv_release_stream(dtv_stream *stream)
{
    if (stream == nullptr || stream->state == nullptr) return;
    auto *state = static_cast<PublicStreamState *>(stream->state.get());
    std::lock_guard lock(state->mutex);
    if (!state->closed) return;
    delete stream;
}

extern "C" int32_t dtv_cancel_enclosure(dtv_enclosure *object, dtv_error *error)
{
    if (object == nullptr) {
        set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    auto &state = public_discovery_state();
    std::shared_ptr<PublicDiscoveryState::OpenSlot> slot;
    {
        std::lock_guard lock(state.open_mutex);
        const auto found = state.open_slots.find(object);
        if (found == state.open_slots.end()) {
            set_public_error(error, DTV_ERROR_INVALID_ARGUMENT);
            return DTV_ERROR_INVALID_ARGUMENT;
        }
        slot = found->second;
    }
    std::shared_ptr<EnclosureRuntime> runtime;
    bool opening = false;
    {
        std::lock_guard lock(slot->mutex);
        runtime = slot->runtime;
        opening = slot->phase == PublicDiscoveryState::OpenSlot::Phase::opening;
        if (opening) {
            slot->cancelled.store(true, std::memory_order_release);
            slot->cancelled.notify_all();
            if (runtime) {
                for (const auto request : slot->requests) {
                    (void)runtime->cancel(request);
                }
            }
        }
    }
    slot->condition.notify_all();
    if (!opening) cancel_open_operations(slot, false);
    set_public_error(error, DTV_ERROR_OK);
    return DTV_ERROR_OK;
}

}
