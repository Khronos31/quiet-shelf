// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/cli.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <dtv/internal/discovery-json.hpp>

namespace {

int print_help(const dtv_cli_config *config)
{
    if (std::printf("Usage: %s [--help] [--version] [--models] [--list-json]\n",
                    config->command_name) < 0 ||
        std::printf("Show product metadata or the discovery JSON.\n") < 0 ||
        std::fflush(stdout) != 0) {
        return 70;
    }
    return 0;
}

int print_models(dtv_model_at_fn model_at, uint32_t model_count)
{
    for (uint32_t i = 0; i < model_count; ++i) {
        const dtv_model_info *model = nullptr;
        try {
            model = model_at(i);
        } catch (...) {
            std::fprintf(stderr, "model provider failed\n");
            return 70;
        }
        if (model == nullptr || model->name == nullptr) {
            std::fprintf(stderr, "invalid model descriptor\n");
            return 70;
        }
        if (std::printf("%s %04x:%04x\n",
                        model->name,
                        static_cast<unsigned int>(model->vendor_id),
                        static_cast<unsigned int>(model->product_id)) < 0) {
            return 70;
        }
    }
    return std::fflush(stdout) == 0 ? 0 : 70;
}

bool has_argument(int argc, const char *const argv[], const char *argument)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], argument) == 0) {
            return true;
        }
    }
    return false;
}

int exit_code_for_error(int32_t code)
{
    switch (code) {
    case DTV_ERROR_OK: return 0;
    case DTV_ERROR_INVALID_ARGUMENT: return 2;
    case DTV_ERROR_NOT_FOUND:
    case DTV_ERROR_NOT_READY:
    case DTV_ERROR_UNSUPPORTED: return 3;
    case DTV_ERROR_BUSY: return 4;
    case DTV_ERROR_TIMEOUT: return 5;
    case DTV_ERROR_VERSION_MISMATCH:
    case DTV_ERROR_PROTOCOL_ERROR: return 6;
    case DTV_ERROR_USB_IO:
    case DTV_ERROR_DISCONNECTED: return 7;
    case DTV_ERROR_TS_ERROR:
    case DTV_ERROR_SLOW_CONSUMER: return 8;
    case DTV_ERROR_NO_CARD:
    case DTV_ERROR_CARD_REMOVED:
    case DTV_ERROR_CARD_IO: return 9;
    case DTV_ERROR_FIRMWARE_REJECTED: return 10;
    default: return 70;
    }
}

template <typename T>
std::size_t null_terminated_count(T *const *items)
{
    std::size_t count = 0;
    if (items == nullptr) return 0;
    while (items[count] != nullptr) ++count;
    return count;
}

struct EnclosureStore {
    dtv::internal::DiscoveryJsonEnclosure view{};
    std::vector<dtv::internal::DiscoveryJsonDevice> devices;
    std::vector<std::vector<dtv::internal::DiscoveryJsonReceiver>> receivers;
    std::vector<dtv::internal::DiscoveryJsonCandidate> candidates;
};

struct HeldEnclosures {
    std::vector<dtv_enclosure *> items;

    ~HeldEnclosures()
    {
        for (auto *item : items) dtv_drop_enclosure(item);
    }

    void hold(dtv_enclosure *enclosure)
    {
        dtv_hold_enclosure(enclosure);
        items.push_back(enclosure);
    }
};

int collect_receiver_array(dtv_device *device,
                           std::vector<dtv::internal::DiscoveryJsonReceiver> &out)
{
    dtv_receiver *const *receivers = dtv_get_receivers(device);
    if (receivers == nullptr) return 0; /* unknown */
    const auto count = null_terminated_count(receivers);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        dtv::internal::DiscoveryJsonReceiver receiver{};
        receiver.receiver = dtv_get_index(receivers[i]);
        receiver.device = dtv_get_device_id(device);
        receiver.local = dtv_get_local(receivers[i]);
        receiver.systems = dtv_get_systems(receivers[i]);
        receiver.ts_scrambling = dtv_get_ts_scrambling(receivers[i]);
        out.push_back(receiver);
    }
    return 1; /* known, including zero entries */
}

int collect_list_json(const dtv_cli_config *config)
{
    const dtv_model *const *models = dtv_list_models();
    if (models == nullptr || models[0] == nullptr || models[0]->product == nullptr ||
        models[0]->product[0] == '\0') {
        std::fprintf(stderr, "product metadata is unavailable\n");
        return 70;
    }

    HeldEnclosures held;
    dtv_error error{};
    error.struct_size = sizeof(error);
    dtv_enclosure *const *list = dtv_list_enclosures(&error);
    if (list == nullptr) {
        return error.code == DTV_ERROR_OK ? 70 : exit_code_for_error(error.code);
    }
    const auto capacity = dtv_get_list_capacity();

    std::vector<EnclosureStore> stores;
    stores.reserve(capacity);
    std::vector<dtv::internal::DiscoveryJsonEnclosure> enclosure_views;
    enclosure_views.reserve(capacity);

    for (std::uint32_t slot = 0; slot < capacity; ++slot) {
        dtv_enclosure *enclosure = list[slot];
        if (enclosure == nullptr) continue;
        held.hold(enclosure);
        stores.emplace_back();
        auto &store = stores.back();
        store.view.id = dtv_get_enclosure_id(enclosure);
        store.view.model = dtv_get_model(enclosure);
        store.view.serial = dtv_get_enclosure_serial(enclosure);
        store.view.serial_unique = dtv_get_serial_unique(enclosure);
        store.view.status = dtv_get_status(enclosure);

        std::uint32_t support = DTV_UNKNOWN;
        int32_t support_code = dtv_get_enclosure_support(
            enclosure, DTV_OP_LNB_POWER, &support, &error);
        if (support_code != DTV_ERROR_OK) return exit_code_for_error(support_code);
        store.view.lnb_15v_supported = support;
        support = DTV_UNKNOWN;
        support_code = dtv_get_enclosure_support(enclosure, DTV_OP_CARD, &support, &error);
        if (support_code != DTV_ERROR_OK) return exit_code_for_error(support_code);
        store.view.card_reader_supported = support;

        dtv_device *reader = dtv_get_card_reader_device(enclosure);
        if (reader != nullptr) {
            store.view.has_card_reader_device = true;
            store.view.card_reader_device = dtv_get_device_id(reader);
        }

        dtv_device *const *devices = dtv_get_devices(enclosure);
        const auto device_count = null_terminated_count(devices);
        store.devices.reserve(device_count);
        store.receivers.resize(device_count);
        for (std::size_t index = 0; index < device_count; ++index) {
            dtv::internal::DiscoveryJsonDevice device{};
            device.id = dtv_get_device_id(devices[index]);
            device.vid = dtv_get_vid(devices[index]);
            device.pid = dtv_get_pid(devices[index]);
            device.serial = dtv_get_device_serial(devices[index]);
            device.location.bus = dtv_get_bus(devices[index]);
            device.location.address = dtv_get_address(devices[index]);
            device.location.port = dtv_get_port(devices[index]);
            const int known = collect_receiver_array(devices[index], store.receivers[index]);
            device.receivers_known = known != 0;
            if (device.receivers_known) {
                device.receivers = store.receivers[index].data();
                device.receiver_count = store.receivers[index].size();
            }
            store.devices.push_back(device);
        }

        const dtv_candidate *const *candidates = dtv_get_candidates(enclosure);
        const auto candidate_count = null_terminated_count(candidates);
        store.candidates.reserve(candidate_count);
        for (std::size_t index = 0; index < candidate_count; ++index) {
            dtv::internal::DiscoveryJsonCandidate candidate{};
            candidate.device = candidates[index]->device;
            candidate.serial = candidates[index]->serial;
            candidate.location.bus = candidates[index]->bus;
            candidate.location.address = candidates[index]->address;
            candidate.location.port = candidates[index]->port;
            store.candidates.push_back(candidate);
        }

        store.view.devices = store.devices.data();
        store.view.device_count = store.devices.size();
        store.view.candidates = store.candidates.data();
        store.view.candidate_count = store.candidates.size();
        enclosure_views.push_back(store.view);
    }

    const dtv_ungrouped_device *const *ungrouped = dtv_list_ungrouped_devices();
    if (ungrouped == nullptr) {
        std::fprintf(stderr, "discovery result is unavailable\n");
        return 70;
    }
    const auto ungrouped_count = null_terminated_count(ungrouped);
    std::vector<dtv::internal::DiscoveryJsonUngrouped> ungrouped_views;
    ungrouped_views.reserve(ungrouped_count);
    for (std::size_t index = 0; index < ungrouped_count; ++index) {
        dtv::internal::DiscoveryJsonUngrouped value{};
        value.serial = ungrouped[index]->serial;
        value.model = ungrouped[index]->model;
        value.vid = ungrouped[index]->vid;
        value.pid = ungrouped[index]->pid;
        value.status = ungrouped[index]->status;
        value.location.bus = ungrouped[index]->bus;
        value.location.address = ungrouped[index]->address;
        value.location.port = ungrouped[index]->port;
        ungrouped_views.push_back(value);
    }

    dtv::internal::DiscoveryJsonInput input{};
    input.product = models[0]->product;
    input.version = config->version;
    input.complete = dtv_get_scan_complete() != 0;
    input.enclosures = enclosure_views.data();
    input.enclosure_count = enclosure_views.size();
    input.ungrouped = ungrouped_views.data();
    input.ungrouped_count = ungrouped_views.size();

    std::uint64_t bytes = 0;
    if (dtv::internal::discovery_json_size(&input, &bytes) !=
        dtv::internal::DiscoveryJsonCode::ok) {
        std::fprintf(stderr, "discovery JSON validation failed\n");
        return 70;
    }
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max()) {
        std::fprintf(stderr, "discovery JSON size is invalid\n");
        return 70;
    }
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(bytes));
    dtv_buffer output{buffer.data(), buffer.size(), 0};
    if (dtv::internal::encode_discovery_json(&input, &output) !=
            dtv::internal::DiscoveryJsonCode::ok ||
        output.size != bytes) {
        std::fprintf(stderr, "discovery JSON encoding failed\n");
        return 70;
    }
    if (std::fwrite(buffer.data(), 1, buffer.size(), stdout) != buffer.size() ||
        std::fflush(stdout) != 0) {
        return 70;
    }
    return 0;
}

}

extern "C" uint32_t dtv_api_version(void)
{
    return UINT32_C(0x00020000);
}

extern "C" int32_t dtv_cli_main(int32_t argc,
                                const char *const argv[],
                                const dtv_cli_config *config,
                                dtv_model_at_fn model_at,
                                uint32_t model_count)
{
    if (argc < 1 || argv == nullptr || config == nullptr ||
        config->struct_size < sizeof(dtv_cli_config) ||
        config->product_name == nullptr || config->command_name == nullptr ||
        config->version == nullptr || config->default_model == nullptr ||
        (model_count != 0 && model_at == nullptr)) {
        std::fprintf(stderr, "invalid CLI configuration\n");
        return 70;
    }

    const bool help = has_argument(argc, argv, "--help") ||
                      has_argument(argc, argv, "-h");
    const bool version = has_argument(argc, argv, "--version");
    const bool model_list = has_argument(argc, argv, "--models");
    const bool list_json = has_argument(argc, argv, "--list-json");

    if (help) {
        return print_help(config);
    }
    if (version) {
        if (std::printf("%s %s\n", config->product_name, config->version) < 0 ||
            std::fflush(stdout) != 0) {
            return 70;
        }
        return 0;
    }
    if (list_json) {
        try {
            return collect_list_json(config);
        } catch (...) {
            return 70;
        }
    }
    if (model_list) {
        if (argc != 2) {
            std::fprintf(stderr, "%s: --models cannot be combined with other arguments\n",
                         config->command_name);
            return 2;
        }
        return print_models(model_at, model_count);
    }

    std::fprintf(stderr, "%s: unknown argument\n", config->command_name);
    return 2;
}
