// SPDX-License-Identifier: GPL-2.0-only
#include "card/uart.h"
#include "product/core_family.h"
#include "torne_ts/profile.h"
#include "torned/daemon.h"

#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/control_client.h>
#include <dtv/userland/identity.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/records.h>
#include <dtv/userland/wire.h>
#include <px4/tuner_service.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

using dtv::userland::Error;
using dtv::userland::Result;
using dtv::userland::Timeout;

int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)


class FakeCardClock final : public torne::card::Clock {
public:
    std::uint64_t now = 0U;
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        now += milliseconds;
    }
};

class FakeCardLine final : public torne::card::Line {
public:
    static constexpr std::array<std::uint8_t, 4U> atr{0x3BU, 0x80U, 0x01U, 0x81U};
    std::deque<std::uint8_t> rx;

    torne::card::PresenceResult detect_card() noexcept override
    {
        torne::card::PresenceResult result;
        result.result.ok = true;
        result.result.step = torne::card::Step::presence;
        result.present = true;
        return result;
    }
    torne::card::Result reset_card(torne::card::Clock& clock) noexcept override
    {
        clock.sleep_ms(1U);
        rx.assign(atr.begin(), atr.end());
        return torne::card::Result{true, torne::card::Step::reset_lines_release,
                                   torne::card::Error::none, {}};
    }
    torne::card::Result power_down() noexcept override
    {
        rx.clear();
        return torne::card::Result{true, torne::card::Step::power,
                                   torne::card::Error::none, {}};
    }
    torne::card::CountResult data_ready() noexcept override
    {
        torne::card::CountResult result;
        result.result.ok = true;
        result.result.step = torne::card::Step::fifo_count;
        result.count = static_cast<std::uint32_t>(rx.size());
        return result;
    }
    torne::card::ReadResult read_data(std::uint8_t* buffer,
                                      std::size_t length) noexcept override
    {
        torne::card::ReadResult result;
        const std::size_t count = length < rx.size() ? length : rx.size();
        for (std::size_t i = 0U; i < count; ++i) {
            buffer[i] = rx.front();
            rx.pop_front();
        }
        result.result.ok = true;
        result.result.step = torne::card::Step::read_rx;
        result.bytes_read = count;
        return result;
    }
    torne::card::Result write_data(const std::uint8_t* data,
                                   std::size_t length) noexcept override
    {
        if (length < 3U)
            return torne::card::Result{false, torne::card::Step::write_tx,
                                       torne::card::Error::invalid_argument, {}};
        const std::uint8_t pcb = data[1U];
        const std::uint8_t count = data[2U];
        if (length < 3U + count + 1U)
            return torne::card::Result{false, torne::card::Step::write_tx,
                                       torne::card::Error::short_transfer, {}};
        auto push = [this](const std::uint8_t* body, std::size_t size,
                           std::uint8_t value) {
            rx.insert(rx.end(), body, body + size);
            rx.push_back(value);
        };
        if (pcb == 0xC0U) {
            const std::uint8_t body[] = {0x00U, 0xE0U, 0x00U};
            push(body, sizeof(body), 0xE0U);
        } else if (pcb == 0xC1U) {
            const std::uint8_t body[] = {0x00U, 0xE1U, 0x01U, data[3U]};
            std::uint8_t lrc = 0U;
            for (std::uint8_t byte : body) lrc ^= byte;
            push(body, sizeof(body), lrc);
        } else if ((pcb & 0xC0U) == 0U) {
            const std::uint8_t body[] = {0x00U,
                                         static_cast<std::uint8_t>(pcb & 0x40U),
                                         0x02U, 0x90U, 0x00U};
            std::uint8_t lrc = 0U;
            for (std::uint8_t byte : body) lrc ^= byte;
            push(body, sizeof(body), lrc);
        } else {
            return torne::card::Result{false, torne::card::Step::write_tx,
                                       torne::card::Error::invalid_argument, {}};
        }
        return torne::card::Result{true, torne::card::Step::send,
                                   torne::card::Error::none, {}};
    }
    torne::card::Result set_baud_rate(std::uint32_t) noexcept override
    {
        return torne::card::Result{true, torne::card::Step::set_baud,
                                   torne::card::Error::none, {}};
    }
    void invalidate() noexcept override {}
    bool valid() const noexcept override { return true; }
};

class FakeDevice final : public torne::product::Device {
public:
    static std::atomic<int> close_calls;
    int packets_left = 0;
    std::uint64_t dropped = 0U;
    std::array<std::uint8_t, 188U> next_packet{};
    FakeCardClock card_clock;
    FakeCardLine card_line;
    Result<void> tune(std::uint64_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> start_stream() noexcept override { return Result<void>::success(); }
    Result<void> stop_stream() noexcept override { return Result<void>::success(); }
    Result<void> read_packet(std::array<std::uint8_t, 188U>& packet,
                             std::uint32_t) noexcept override
    {
        if (packets_left <= 0) return Result<void>::failure(Error::TIMEOUT);
        --packets_left;
        packet = next_packet;
        return Result<void>::success();
    }
    Result<torne::product::StreamStats> stream_stats() noexcept override
    {
        torne::product::StreamStats stats;
        stats.dropped = dropped;
        return Result<torne::product::StreamStats>::success(stats);
    }
    Result<torne::product::Status> status() noexcept override
    {
        torne::product::Status value;
        value.locked = true;
        return Result<torne::product::Status>::success(value);
    }
    torne::product::CardPort card_port() noexcept override
    {
        return torne::product::CardPort{&card_line, &card_clock};
    }
    void close() noexcept override { close_calls.fetch_add(1); }
};

std::atomic<int> FakeDevice::close_calls{0};

class FakeBackend final : public torne::daemon::Backend {
public:
    FakeDevice* device = nullptr;
    Result<dtv::userland::wire::DiscoveryScan> discover(
        const torne::daemon::DaemonConfig&) noexcept override
    {
        return Result<dtv::userland::wire::DiscoveryScan>::success({});
    }
    Result<FamilyPtr> open_family(
        const torne::daemon::DaemonConfig&) noexcept override
    {
        auto owned = std::unique_ptr<torne::product::Device>(new FakeDevice());
        device = static_cast<FakeDevice*>(owned.get());
        auto family = torne::product::make_family(std::move(owned));
        if (!family) return Result<FamilyPtr>::failure(Error::INTERNAL);
        return Result<FamilyPtr>::success(std::move(family));
    }
    bool card_service_available() const noexcept override { return true; }
};


class IpcOutput final : public px4::userland::cli::Px4TsOutput {
public:
    Result<void> write(dtv::userland::ByteView bytes,
                       const px4::userland::cli::Px4TsSignal&) noexcept override
    {
        return bytes.size == 188U ? Result<void>::success()
                                  : Result<void>::failure(Error::PROTOCOL_ERROR);
    }
};

class IpcClock final : public px4::userland::cli::Px4TsClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

class IpcSignal final : public px4::userland::cli::Px4TsSignal {
public:
    bool stop_requested() const noexcept override { return false; }
};

bool read_frame(dtv::userland::wire::TimedClientTransport& transport,
                dtv::userland::wire::Frame& frame) noexcept
{
    namespace w = dtv::userland::wire;
    std::array<std::uint8_t, w::kHeaderSize> header_bytes{};
    const auto read_exact = [&transport](std::uint8_t* bytes,
                                          std::size_t size) noexcept {
        std::size_t offset = 0U;
        while (offset < size) {
            const auto read = transport.read_some({bytes + offset, size - offset},
                                                   Timeout{5000U});
            if (!read || read.value() == 0U) return false;
            offset += read.value();
        }
        return true;
    };
    if (!read_exact(header_bytes.data(), header_bytes.size())) return false;
    const auto header = w::decode_header({header_bytes.data(), header_bytes.size()});
    if (!header || header.value().payload_length > w::kMaxTsDataPayload) return false;
    static std::array<std::uint8_t, w::kMaxTsDataPayload> payload{};
    if (!read_exact(payload.data(), header.value().payload_length)) return false;
    frame.header = header.value();
    frame.payload = {payload.data(), header.value().payload_length};
    return true;
}

void test_daemon_publishes_and_closes()
{
#if defined(_WIN32)
    return;
#else
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("tda-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    CHECK(fs::create_directories(root, error) && !error);
    fs::permissions(root, fs::perms::owner_all,
                    fs::perm_options::replace, error);
    CHECK(!error);

    const auto selector = dtv::userland::identity::parse_device_selector("usb:1-2");
    CHECK(selector);
    if (!selector) return;
    const auto resolved = dtv::userland::identity::resolve_selector(
        dtv::userland::identity::Product::torne, selector.value());
    CHECK(resolved);
    if (!resolved) return;
    const auto automatic = dtv::userland::identity::auto_instance(resolved.value());
    CHECK(automatic);
    if (!automatic) return;
    const std::string instance(automatic.value().bytes.data(),
                               automatic.value().length);

    torne::daemon::DaemonConfig config;
    config.runtime_directory = root.string();
    config.has_device = true;
    config.device = selector.value();
    FakeDevice::close_calls.store(0);
    FakeBackend backend;
    std::atomic<bool> finished{false};
    int exit_code = -1;
    std::thread worker([&] {
        exit_code = torne::daemon::run_daemon(config, &backend, stdout, stderr);
        finished.store(true, std::memory_order_release);
    });

    const std::string runtime = root.string();
    const dtv::userland::host_runtime::EndpointConfig endpoint{
        runtime, "torne-userland", instance,
        dtv::userland::host_runtime::EndpointAccess::private_user};
    const auto endpoint_path = dtv::userland::host_runtime::control_endpoint_path(endpoint);
    CHECK(endpoint_path);
    if (endpoint_path) {
        for (int attempt = 0; attempt < 500; ++attempt) {
            if (fs::exists(endpoint_path.value())) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    auto control = dtv::userland::host_runtime::connect_control(
        endpoint, Timeout{5000U});
    CHECK(control);
    if (control) {
        namespace w = dtv::userland::wire;
        const auto fetched = w::fetch_status(*control.value(), Timeout{5000U});
        CHECK(fetched);
        if (fetched) {
            const auto& list = fetched.snapshot.list;
            CHECK(list.receivers.present);
            CHECK(list.receivers.value.count == 1U);
            CHECK(list.receivers.value.items[0U].id == 0U);
            CHECK(list.receivers.value.items[0U].systems.mask == 1U);
            CHECK(list.readers.present);
            CHECK(list.readers.value.count == 1U);
        }
        if (fetched) {
            auto card_control = dtv::userland::host_runtime::connect_control(
                endpoint, Timeout{5000U});
            CHECK(card_control);
            if (card_control) {
                w::CardClientOptions options;
                options.reader_id = 0U;
                options.share = w::Share::exclusive;
                options.preferred_protocols = 1U;
                auto card = w::begin_card(*card_control.value(), options,
                                          Timeout{5000U});
                if (!card) std::fprintf(stderr, "begin_card error=%u remote=%u\n",
                    static_cast<unsigned>(card.error), card.remote_error_code);
                CHECK(card);
                if (card) {
                    const std::uint8_t apdu[] = {0x00U, 0x11U, 0x22U, 0x33U};
                    auto transmitted = w::card_transmit(
                        *card_control.value(), card.session, w::CardProtocol::t1,
                        {apdu, sizeof(apdu)}, Timeout{5000U});
                    CHECK(transmitted);
                    if (transmitted) {
                        CHECK(transmitted.response.response.length == 2U);
                        CHECK(transmitted.response.response.bytes[0] == 0x90U);
                        CHECK(transmitted.response.response.bytes[1] == 0x00U);
                    }
                    auto disconnected = w::card_disconnect(
                        *card_control.value(), card.session,
                        w::Disposition::leave, Timeout{5000U});
                    CHECK(disconnected);
                }
            }
        }
        if (fetched && backend.device != nullptr) {
            backend.device->next_packet[0] = 0x47U;
            backend.device->next_packet[187] = 187U;
            backend.device->packets_left = 1;
            auto recording_control = dtv::userland::host_runtime::connect_control(
                endpoint, Timeout{5000U});
            CHECK(recording_control);
            if (recording_control) {
            w::RecordingOptions options;
            options.receiver_id = 0U;
            options.tune.system = w::System::isdb_t;
            options.tune.frequency_hz = 557142857U;
            options.tune.timeout_ms = 1000U;
            auto recording = w::begin_recording(*recording_control.value(), options,
                                                Timeout{10000U});
            if (!recording) std::fprintf(stderr,
                "begin_recording error=%u remote=%u\n",
                static_cast<unsigned>(recording.error), recording.remote_error_code);
            CHECK(recording);
            if (recording) {
                const dtv::userland::host_runtime::EndpointConfig data_endpoint{
                    runtime, "torne-userland", instance,
                    dtv::userland::host_runtime::EndpointAccess::private_user,
                    -1, "stream.sock"};
                auto data = dtv::userland::host_runtime::connect_endpoint(
                    data_endpoint, Timeout{5000U});
                CHECK(data);
                if (data) {
                    w::AttachRequest attach;
                    attach.epoch = recording.session.epoch;
                    attach.lease_id = recording.session.lease_id;
                    attach.stream_id = recording.session.stream_id;
                    attach.attach_token = recording.session.attach_token;
                    std::array<std::uint8_t, 64U> payload{};
                    std::array<std::uint8_t, w::kHeaderSize + 64U> bytes{};
                    const auto size = w::encode_attach_request(
                        attach, {payload.data(), payload.size()});
                    CHECK(size && w::encode_header(
                        {w::Kind::request, w::kOpcodeAttachStream, 1U,
                         static_cast<std::uint32_t>(size.value())},
                        {bytes.data(), w::kHeaderSize}));
                    if (size) {
                        std::memcpy(bytes.data() + w::kHeaderSize, payload.data(),
                                    size.value());
                        CHECK(data.value()->write_all(
                            {bytes.data(), w::kHeaderSize + size.value()},
                            Timeout{5000U}));
                        w::Frame attached_frame;
                        CHECK(read_frame(*data.value(), attached_frame));
                        CHECK(attached_frame.header.kind == w::Kind::response);
                        CHECK(attached_frame.header.opcode == w::kOpcodeAttachStream);
                        w::Frame ts_frame;
                        CHECK(read_frame(*data.value(), ts_frame));
                        CHECK(ts_frame.header.kind == w::Kind::event);
                        CHECK(ts_frame.header.opcode == w::kOpcodeTsDataEvent);
                        const auto ts = w::decode_ts_data(ts_frame.payload);
                        CHECK(ts);
                        if (ts) {
                            CHECK(ts.value().stream_id == recording.session.stream_id);
                            CHECK(ts.value().packets.size == 188U);
                            CHECK(ts.value().packets.data[187U] == 187U);
                        }
                        const auto stopped = w::stop_recording(
                            *recording_control.value(), recording.session, Timeout{5000U});
                        CHECK(stopped);
                        bool saw_end = false;
                        for (int frame_index = 0; frame_index < 5; ++frame_index) {
                            w::Frame end_frame;
                            if (!read_frame(*data.value(), end_frame)) break;
                            if (end_frame.header.kind == w::Kind::event &&
                                end_frame.header.opcode == w::kOpcodeStreamEndEvent) {
                                const auto ended = w::decode_stream_end_event(
                                    end_frame.payload);
                                CHECK(ended);
                                if (ended) {
                                    bool packets_valid = false;
                                    bool bytes_valid = false;
                                    for (std::size_t i = 0U;
                                         i < ended.value().final.statistics.entries.count;
                                         ++i) {
                                        const auto& entry =
                                            ended.value().final.statistics.entries.items[i];
                                        if (entry.id == w::kCounterIdTransmittedPackets &&
                                            entry.counter.measurement.state ==
                                                w::MeasureState::valid) {
                                            packets_valid =
                                                entry.counter.measurement.value == 1U;
                                        }
                                        if (entry.id == w::kCounterIdStreamBytes &&
                                            entry.counter.measurement.state ==
                                                w::MeasureState::valid) {
                                            bytes_valid =
                                                entry.counter.measurement.value == 188U;
                                        }
                                    }
                                    CHECK(packets_valid);
                                    CHECK(bytes_valid);
                                }
                                saw_end = true;
                                break;
                            }
                        }
                        CHECK(saw_end);
                        const auto released = w::release_recording(
                            *recording_control.value(), recording.session, Timeout{5000U});
                        CHECK(released);
                    }
                    data.value()->close();
                }
            }
            recording_control.value()->close();
            }

            const char* ipc_argv[] = {
                "torne-ts", "--runtime-dir", runtime.c_str(), "--instance",
                instance.c_str(), "--receiver", "0", "-c", "27",
                "--packet-count", "1", "-o", "-"};
            auto ipc_arguments = px4::userland::cli::parse_px4_ts_product_arguments(
                static_cast<int>(sizeof(ipc_argv) / sizeof(ipc_argv[0])),
                ipc_argv, torne::ts::kProfile);
            CHECK(ipc_arguments.valid);
            if (ipc_arguments.valid) {
                IpcOutput ipc_output;
                IpcClock ipc_clock;
                IpcSignal ipc_signal;
                backend.device->next_packet[0] = 0x47U;
                backend.device->next_packet[187] = 187U;
                backend.device->packets_left = 1;
                backend.device->dropped = 0U;
                auto ipc = px4::userland::cli::Px4TsRunner::run_wire(
                    ipc_arguments, torne::ts::kProfile, ipc_output, ipc_clock,
                    ipc_signal);
                CHECK(ipc);
                if (ipc) {
                    const auto drops = torne::ts::measured_queue_drops(
                        ipc.value().counters);
                    CHECK(drops && drops.value() == 0U);
                }

                backend.device->packets_left = 1;
                backend.device->dropped = 7U;
                auto dropped_run = px4::userland::cli::Px4TsRunner::run_wire(
                    ipc_arguments, torne::ts::kProfile, ipc_output, ipc_clock,
                    ipc_signal);
                CHECK(dropped_run);
                if (dropped_run) {
                    const auto drops = torne::ts::measured_queue_drops(
                        dropped_run.value().counters);
                    CHECK(drops && drops.value() == 7U);
                    std::FILE* drop_error = std::tmpfile();
                    CHECK(drop_error != nullptr);
                    if (drop_error != nullptr) {
                        CHECK(torne::ts::finalize_ipc_success(
                            dropped_run.value().counters, drop_error) == 8);
                        std::fflush(drop_error);
                        std::rewind(drop_error);
                        char text[128] = {};
                        CHECK(std::fread(text, 1U, sizeof(text) - 1U,
                                         drop_error) > 0U);
                        CHECK(std::string(text).find("queue drops: 7") !=
                              std::string::npos);
                        std::fclose(drop_error);
                    }
                }
            }
        }
    }
    CHECK(!finished.load(std::memory_order_acquire));
    std::raise(SIGTERM);
    worker.join();
    CHECK(finished.load(std::memory_order_acquire));
    CHECK(exit_code == 0);
    CHECK(FakeDevice::close_calls.load() == 1);
    CHECK(!fs::exists(endpoint_path.value()));

    fs::remove_all(root, error);
#endif
}

}  // namespace

int main()
{
    test_daemon_publishes_and_closes();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("product daemon tests passed\n");
    return 0;
}
