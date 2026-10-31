// SPDX-License-Identifier: GPL-2.0-only
#include "torned/daemon.h"

#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/control_connection.h>
#include <dtv/userland/identity.h>
#include <dtv/userland/host_facade/px4d_signals.h>
#include <dtv/userland/host_listener_context/enclosure_context.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/host_runtime/family.h>
#include <dtv/userland/records.h>
#include <px4/ipc.h>
#include <px4/tuner_service.h>
#if defined(_WIN32)
#include <px4/windows_tuner_nonce.h>
#else
#include <px4/posix_tuner_nonce.h>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace torne::daemon {
namespace {

using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Result;
using dtv::userland::error_string;
using dtv::userland::wire::DiscoveryScan;
using dtv::userland::wire::encode_discovery_json;
using dtv::userland::wire::discovery_json_size;

namespace wire = dtv::userland::wire;
using dtv::userland::Timeout;

template <std::size_t N>
wire::WString<N> text(const char* value) noexcept
{
    wire::WString<N> output;
    output.length = static_cast<std::uint16_t>(std::strlen(value));
    std::memcpy(output.bytes.data(), value, output.length);
    return output;
}

wire::Epoch make_epoch() noexcept
{
    std::uint64_t seed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    wire::Epoch epoch{};
    for (std::uint8_t& byte : epoch) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        byte = static_cast<std::uint8_t>(seed >> 56U);
    }
    if (epoch[0] == 0U) epoch[0] = 1U;
    return epoch;
}

wire::ListResponse make_list(const wire::Epoch& epoch,
                             const wire::Producer& producer,
                             const wire::DiscoveryScan* scan,
                             const std::string& selected_path) noexcept
{
    wire::ListResponse list;
    list.epoch = epoch;
    list.producer = producer;
    list.topology.state = wire::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.functions.count = 1U;
    auto& function = list.functions.items[0U];
    function.id = 1U;
    function.vid = 0x054cU;
    function.pid = 0x04b2U;
    function.mode = wire::Mode::runtime;
    function.serial_state = wire::SerialState::absent;
    function.identification_reason =
        wire::IdentificationReason::descriptor_unreadable;
    function.present = true;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.identity.serial_state = wire::IdentitySerialState::absent;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    auto& receiver = list.receivers.value.items[0U];
    receiver.id = 0U;
    receiver.systems.mask = 1U;
    receiver.binding.present = true;
    receiver.binding.value.function_id = 1U;
    receiver.binding.value.local_id = 0U;
    receiver.capabilities.measurement_mask = wire::kMetricMaskLock;
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0U].id = 0U;
    list.readers.value.items[0U].binding.present = true;
    list.readers.value.items[0U].binding.value.function_id = 1U;
    list.readers.value.items[0U].binding.value.local_id = 0U;

    if (scan == nullptr || scan->enclosure_count == 0U) return list;
    std::size_t enclosure_index = 0U;
    std::size_t device_index = 0U;
    bool selected = selected_path.empty();
    if (!selected) {
        for (std::size_t e = 0U; e < scan->enclosure_count && !selected; ++e) {
            const wire::DiscoveryEnclosure& candidate = scan->enclosures[e];
            for (std::size_t d = 0U; d < candidate.device_count; ++d) {
                const auto& location = candidate.devices[d].location;
                if (!location.port.present) continue;
                const std::string_view path(
                    location.port.value.bytes.data(), location.port.value.length);
                if (path == selected_path) {
                    enclosure_index = e;
                    device_index = d;
                    selected = true;
                    break;
                }
            }
        }
    }
    if (!selected) return list;
    const wire::DiscoveryEnclosure& enclosure = scan->enclosures[enclosure_index];
    list.enclosure.value.id = enclosure.enclosure;
    list.enclosure.value.identity.serial_state =
        enclosure.serial.present ? wire::IdentitySerialState::present
                                 : wire::IdentitySerialState::absent;
    if (enclosure.serial.present) {
        list.enclosure.value.identity.serial = enclosure.serial;
        list.enclosure.value.identity.uniqueness =
            wire::Uniqueness::unique_in_scan;
    }
    if (device_index >= enclosure.device_count) return list;
    const wire::DiscoveryDevice& device = enclosure.devices[device_index];
    function.vid = device.vid;
    function.pid = device.pid;
    function.serial_state = device.serial.present
        ? wire::SerialState::present : wire::SerialState::absent;
    if (device.serial.present) function.serial = device.serial;
    if (device.location.port.present) {
        function.location.usb_path.present = true;
        function.location.usb_path.value = device.location.port.value;
    }
    if (device.location.bus.present) {
        function.location.bus.present = true;
        function.location.bus.value = device.location.bus.value;
    }
    if (device.location.address.present) {
        function.location.address.present = true;
        function.location.address.value = device.location.address.value;
    }
    function.identification_reason = device.serial.present
        ? wire::IdentificationReason::none
        : wire::IdentificationReason::descriptor_unreadable;
    list.receivers.value.count = 0U;
    const std::size_t receiver_count = device.receiver_count <
        list.receivers.value.items.size() ? device.receiver_count
                                          : list.receivers.value.items.size();
    for (std::size_t index = 0U; index < receiver_count; ++index) {
        const wire::DiscoveryReceiver& scanned = device.receivers[index];
        auto& entry = list.receivers.value.items[index];
        entry.id = scanned.receiver;
        entry.systems = scanned.systems;
        entry.binding.present = true;
        entry.binding.value.function_id = function.id;
        entry.binding.value.local_id = scanned.local;
        entry.capabilities.measurement_mask = wire::kMetricMaskLock;
        ++list.receivers.value.count;
    }
    list.readers.value.count = 0U;
    const std::size_t reader_count = enclosure.reader_count <
        list.readers.value.items.size() ? enclosure.reader_count
                                        : list.readers.value.items.size();
    for (std::size_t index = 0U; index < reader_count; ++index) {
        const wire::DiscoveryReader& scanned = enclosure.readers[index];
        auto& entry = list.readers.value.items[index];
        entry.id = scanned.reader;
        entry.binding.present = true;
        entry.binding.value.function_id = function.id;
        entry.binding.value.local_id = 0U;
        ++list.readers.value.count;
    }
    return list;
}

int exit_status(Error error) noexcept
{
    // Shared Core CLI contract; do not define a second exit-code table.
    return px4::userland::cli::px4_ts_exit_status(error);
}

bool write_all(std::FILE* output, const std::string& bytes) noexcept
{
    if (output == nullptr) return false;
    return std::fwrite(bytes.data(), 1U, bytes.size(), output) == bytes.size() &&
           std::fflush(output) == 0;
}

int run_list_json(const DaemonConfig& config, Backend* backend,
                  std::FILE* output, std::FILE* error) noexcept
{
    if (backend == nullptr) {
        std::fprintf(error,
            "torned: no hardware backend is available; --list-json is "
            "unsupported\n");
        return exit_status(Error::UNSUPPORTED);
    }
    const auto scan = backend->discover(config);
    if (!scan) {
        std::fprintf(error, "torned: discovery failed: %s\n",
                     error_string(scan.error()));
        return exit_status(scan.error());
    }
    const DiscoveryScan& value = scan.value();
    const auto size = discovery_json_size(value);
    if (!size) {
        std::fprintf(error, "torned: discovery JSON rejected: %s\n",
                     error_string(size.error()));
        return exit_status(size.error());
    }
    std::string bytes(size.value(), '\0');
    const auto encoded = encode_discovery_json(
        value, MutableByteView{reinterpret_cast<std::uint8_t*>(bytes.data()),
                               bytes.size()});
    if (!encoded || encoded.value() != bytes.size()) {
        std::fprintf(error, "torned: discovery JSON encoding failed\n");
        return exit_status(encoded ? Error::INTERNAL : encoded.error());
    }
    if (!write_all(output, bytes)) {
        std::fprintf(error, "torned: could not write discovery JSON\n");
        return exit_status(Error::INTERNAL);
    }
    return 0;
}

}  // namespace

Result<dtv::userland::identity::Selector> selected_selector(
    const DaemonConfig& config, const wire::DiscoveryScan* scan) noexcept
{
    namespace identity = dtv::userland::identity;
    if (config.has_fd)
        return Result<identity::Selector>::failure(Error::NOT_FOUND);
    if (config.has_device) {
        if (config.device.kind != identity::SelectorKind::usb_topology)
            return Result<identity::Selector>::failure(Error::INVALID_ARGUMENT);
        return Result<identity::Selector>::success(config.device);
    }
    if (scan == nullptr || scan->enclosure_count == 0U ||
        scan->enclosures[0U].device_count == 0U) {
        return Result<identity::Selector>::failure(Error::NOT_FOUND);
    }
    const auto& location = scan->enclosures[0U].devices[0U].location;
    if (!location.port.present) {
        return Result<identity::Selector>::failure(Error::NOT_FOUND);
    }
    std::string text_value = "usb:";
    text_value.append(location.port.value.bytes.data(),
                      location.port.value.length);
    return identity::parse_device_selector(text_value);
}

int run_daemon(const DaemonConfig& config, Backend* backend,
               std::FILE* output, std::FILE* error) noexcept
{
    if (config.list_json) return run_list_json(config, backend, output, error);

    if (backend == nullptr) {
        std::fprintf(error,
            "torned: no hardware backend is available; refusing to start\n");
        return exit_status(Error::UNSUPPORTED);
    }
    namespace listener = dtv::userland::host_listener_context;
    namespace host = dtv::userland::host_runtime;
    const wire::Epoch epoch = make_epoch();
    wire::Producer producer;
    producer.product = text<32>("torne-userland");
    producer.version = text<64>(TORNE_USERLAND_PRODUCT_VERSION);
    // A borrowed descriptor is opened directly. Do not make a general USB
    // scan (and its authority) a prerequisite for FD startup.
    Result<wire::DiscoveryScan> discovered =
        Result<wire::DiscoveryScan>::failure(Error::UNSUPPORTED);
    const wire::DiscoveryScan* scan = nullptr;
    if (!config.has_fd) {
        discovered = backend->discover(config);
        if (discovered) scan = &discovered.value();
    }
    const auto selector = selected_selector(config, scan);
    std::string selected_path;
    if (selector && selector.value().topology.count == 1U) {
        const auto& path = selector.value().topology.paths[0U];
        selected_path.assign(path.bytes.data(), path.length);
    }
    wire::ListResponse canonical_list =
        make_list(epoch, producer, scan, selected_path);

    std::string instance = config.instance;
    if (instance.empty() && selector) {
        const auto resolved = dtv::userland::identity::resolve_selector(
            dtv::userland::identity::Product::torne, selector.value());
        if (!resolved) {
            std::fprintf(error, "torned: instance identity failed: %s\n",
                         error_string(resolved.error()));
            return exit_status(resolved.error());
        }
        const auto automatic =
            dtv::userland::identity::auto_instance(resolved.value());
        if (!automatic) {
            std::fprintf(error, "torned: automatic instance failed: %s\n",
                         error_string(automatic.error()));
            return exit_status(automatic.error());
        }
        instance.assign(automatic.value().bytes.data(),
                        automatic.value().length);
    }

    auto family = backend->open_family(config);
    if (!family) {
        std::fprintf(error, "torned: hardware backend open failed: %s\n",
                     error_string(family.error()));
        return exit_status(family.error());
    }
    if (!family.value()) {
        std::fprintf(error, "torned: hardware backend returned no enclosure\n");
        return exit_status(Error::INTERNAL);
    }

    listener::ContextConfig context_config;
    context_config.runtime_directory = config.runtime_directory;
    context_config.product = "torne-userland";
    context_config.instance = instance;
    context_config.access = host::EndpointAccess::private_user;
    context_config.server.epoch = epoch;
    context_config.server.producer = producer;
    context_config.server.features = wire::kFeatureStateEvent;
    if (backend->card_service_available())
        context_config.server.features |= wire::kFeatureCardService;
    context_config.canonical_list = std::move(canonical_list);
    context_config.context.epoch = epoch;
    context_config.context.state = wire::ContextState::ready;
    context_config.context.instance.present = true;
    context_config.context.instance.value =
        text<dtv::userland::detail::kMaxInstanceBytes>(instance.c_str());
    context_config.stream_engine_epoch = 0x544f524eULL;
    context_config.stream_arm_budget_ms = 5000U;

#if defined(_WIN32)
    std::unique_ptr<host::TunerNonceSource> nonce(
        new px4::userland::ipc::windows::WindowsTunerNonceSource());
#else
    std::unique_ptr<host::TunerNonceSource> nonce(
        new px4::userland::ipc::posix::PosixTunerNonceSource());
#endif
    auto assembled = listener::EnclosureContext::assemble(
        std::move(family.value()), std::move(nonce), context_config);
    if (!assembled) {
        std::fprintf(error, "torned: Core enclosure assemble failed: %s\n",
                     error_string(assembled.error()));
        return exit_status(assembled.error());
    }
    if (!px4::userland::px4d::install_signal_handlers()) {
        std::fprintf(error, "torned: signal setup failed\n");
        return 70;
    }
    listener::ListenerDiagnostic diagnostic{};
    if (const auto reserved = assembled.value()->reserve_namespace(&diagnostic);
        !reserved) {
        std::fprintf(error, "torned: Core listener namespace failed: %s\n",
                     error_string(reserved.error()));
        for (;;) {
            const auto closed = assembled.value()->close(Timeout{100U});
            if (!closed || closed.value() == listener::CloseState::quiesced) break;
        }
        return exit_status(reserved.error());
    }
    if (const auto published = assembled.value()->publish(); !published) {
        std::fprintf(error, "torned: Core endpoint publish failed: %s\n",
                     error_string(published.error()));
        return exit_status(published.error());
    }
    Error loop_error = Error::OK;
    while (!px4::userland::px4d::stop_requested()) {
        const auto polled = assembled.value()->poll_once(Timeout{100U});
        if (!polled) {
            std::fprintf(error, "torned: Core poll failed: %s\n",
                         error_string(polled.error()));
            loop_error = polled.error();
            break;
        }
    }
    for (;;) {
        const auto closed = assembled.value()->close(Timeout{100U});
        if (!closed) {
            std::fprintf(error, "torned: Core close failed: %s\n",
                         error_string(closed.error()));
            if (loop_error == Error::OK) loop_error = closed.error();
            break;
        }
        if (closed.value() == listener::CloseState::quiesced) break;
    }
    return exit_status(loop_error);
}

}  // namespace torne::daemon
