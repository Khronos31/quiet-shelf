// SPDX-License-Identifier: GPL-2.0-only
#include "product/core_family.h"
#include "product/daemon_backend.h"

#include <dtv/userland/error.h>
#include <px4/tuner_service.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

using dtv::userland::Error;
using dtv::userland::Result;
using px4::userland::TunerAttachment;
using px4::userland::TunerStreamTerminal;

int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

class FakeDevice final : public torne::product::Device {
public:
    bool locked = true;
    bool read_timeout = false;
    bool read_failure = false;
    bool tune_failure = false;
    int tune_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;
    int close_calls = 0;
    std::array<std::uint8_t, 188U> next_packet{};
    std::atomic<bool> block_read{false};
    std::atomic<bool> read_entered{false};
    std::atomic<bool> release_read{false};
    std::uint64_t dropped = 0U;
    bool stop_failure = false;
    bool not_ready_after_stop = false;

    Result<void> tune(std::uint64_t hz, std::uint32_t) noexcept override
    {
        ++tune_calls;
        if (tune_failure) return Result<void>::failure(Error::NOT_READY);
        if (hz == 0U) return Result<void>::failure(Error::INVALID_ARGUMENT);
        return Result<void>::success();
    }
    Result<void> start_stream() noexcept override
    {
        ++start_calls;
        return Result<void>::success();
    }
    Result<void> stop_stream() noexcept override
    {
        ++stop_calls;
        return stop_failure ? Result<void>::failure(Error::USB_IO)
                            : Result<void>::success();
    }
    Result<void> read_packet(std::array<std::uint8_t, 188U>& packet,
                             std::uint32_t) noexcept override
    {
        if (not_ready_after_stop && stop_calls != 0)
            return Result<void>::failure(Error::NOT_READY);
        if (block_read.load()) {
            read_entered.store(true);
            while (!release_read.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (read_failure) return Result<void>::failure(Error::USB_IO);
        if (read_timeout) return Result<void>::failure(Error::TIMEOUT);
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
        value.locked = locked;
        return Result<torne::product::Status>::success(value);
    }
    void close() noexcept override { ++close_calls; }
};

TunerAttachment attachment()
{
    TunerAttachment value{};
    value.owner_client_id = 7U;
    value.lease_id = 8U;
    value.attachment_id = 9U;
    value.receiver = 0U;
    value.system = px4::userland::ipc::System::ISDB_T;
    value.nonce[0] = 1U;
    return value;
}

std::unique_ptr<dtv::userland::host_runtime::EnclosureFamily> family(FakeDevice** fake)
{
    auto device = std::unique_ptr<torne::product::Device>(new FakeDevice());
    *fake = static_cast<FakeDevice*>(device.get());
    return torne::product::make_family(std::move(device));
}

void test_tune_lock_capture_and_stream()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& tuner = value->tuner_backend();
    auto& stream = *value->stream_control();

    CHECK(tuner.receiver_count() == 1U);
    CHECK(tuner.receiver_supports(0U, px4::userland::ipc::System::ISDB_T));
    CHECK(!tuner.receiver_supports(1U, px4::userland::ipc::System::ISDB_T));
    CHECK(tuner.strict_profile(0U, px4::userland::ipc::System::ISDB_T).native_hz);
    CHECK(tuner.strict_profile(0U, px4::userland::ipc::System::ISDB_S).native_hz == false);

    CHECK(tuner.open_receiver(0U));
    const auto tuned = tuner.tune_terrestrial_hz(
        0U, 557142857U, 1000U);
    CHECK(tuned);
    CHECK(tuned.value().programmed_hz == 557142857U);
    CHECK(tuned.value().lock.value);
    CHECK(fake->tune_calls == 1);
    const auto locked = tuner.is_locked(0U, px4::userland::ipc::System::ISDB_T);
    CHECK(locked && locked.value());
    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    CHECK(fake->start_calls == 1);

    const TunerAttachment identity = attachment();
    CHECK(stream.attach(identity));
    fake->next_packet[0] = 0x47U;
    fake->next_packet[1] = 0x5aU;
    std::array<std::uint8_t, 188U> output{};
    const auto read = stream.read(identity, {output.data(), output.size()},
                                  dtv::userland::Timeout{100U});
    CHECK(read);
    CHECK(read.value().bytes == 188U);
    CHECK(!read.value().timed_out);
    CHECK(output[0] == 0x47U && output[1] == 0x5aU);
    const auto stats = stream.stats(identity);
    CHECK(stats);
    CHECK(stats.value().packets == 1U);
    CHECK(stats.value().bytes == 188U);
    CHECK(stats.value().availability[0U] ==
          dtv::userland::wire::MeasureState::valid);
    CHECK(stats.value().availability[5U] ==
          dtv::userland::wire::MeasureState::valid);
    CHECK(stats.value().availability[2U] ==
          dtv::userland::wire::MeasureState::unknown);
    CHECK(stream.detach(identity));
    CHECK(fake->stop_calls >= 1);
    // The same family must be reusable after a normal capture stop.
    CHECK(tuner.tune_terrestrial_hz(0U, 557142857U, 1000U));
    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    CHECK(fake->start_calls == 2);
    const auto eof = stream.read(identity, {output.data(), output.size()},
                                 dtv::userland::Timeout{10U});
    CHECK(eof);
    CHECK(eof.value().eof);
    CHECK(eof.value().terminal == TunerStreamTerminal::stopped);
    const auto final = stream.final_snapshot(identity);
    CHECK(final);
    CHECK(final.value().counters.packets == 1U);
    CHECK(final.value().counters.availability[0U] ==
          dtv::userland::wire::MeasureState::valid);
    CHECK(final.value().terminal ==
          static_cast<std::uint8_t>(TunerStreamTerminal::stopped));
    CHECK(stream.release_final(identity));
    CHECK(stream.terminal(identity).value() == TunerStreamTerminal::stopped);
    const auto shutdown = value->shutdown();
    CHECK(shutdown);
    CHECK(fake->close_calls == 1);
}

void test_read_and_stop_are_serialized()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& tuner = value->tuner_backend();
    auto& stream = *value->stream_control();
    CHECK(tuner.open_receiver(0U));
    CHECK(tuner.tune_terrestrial_hz(0U, 557142857U, 1000U));
    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    const TunerAttachment identity = attachment();
    CHECK(stream.attach(identity));

    fake->block_read.store(true);
    fake->release_read.store(false);
    std::array<std::uint8_t, 188U> output{};
    std::atomic<bool> read_done{false};
    std::thread reader([&] {
        (void)stream.read(identity, {output.data(), output.size()},
                          dtv::userland::Timeout{1000U});
        read_done.store(true);
    });
    for (int attempt = 0; attempt < 500 && !fake->read_entered.load(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(fake->read_entered.load());

    std::atomic<bool> stop_done{false};
    std::thread stopper([&] {
        (void)tuner.stop_capture(0U, px4::userland::ipc::System::ISDB_T);
        stop_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!stop_done.load());
    fake->release_read.store(true);
    reader.join();
    stopper.join();
    CHECK(read_done.load());
    CHECK(stop_done.load());
    CHECK(stream.detach(identity));
}

void test_read_after_normal_stop_returns_eof()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& tuner = value->tuner_backend();
    auto& stream = *value->stream_control();
    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    const TunerAttachment identity = attachment();
    CHECK(stream.attach(identity));
    fake->not_ready_after_stop = true;
    CHECK(tuner.stop_capture(0U, px4::userland::ipc::System::ISDB_T));
    std::array<std::uint8_t, 188U> output{};
    const auto read = stream.read(identity, {output.data(), output.size()},
                                  dtv::userland::Timeout{100U});
    CHECK(read);
    if (read) {
        CHECK(read.value().eof);
        CHECK(read.value().terminal == TunerStreamTerminal::stopped);
        CHECK(read.value().bytes == 0U);
    }
    CHECK(stream.detach(identity));
}

void test_read_packets_batches_ready_queue()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& tuner = value->tuner_backend();
    auto& stream = *value->stream_control();
    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    const TunerAttachment identity = attachment();
    CHECK(stream.attach(identity));
    fake->next_packet[0] = 0x47U;
    std::array<std::uint8_t, 188U * 4U> output{};
    const auto read = stream.read(identity, {output.data(), output.size()},
                                  dtv::userland::Timeout{100U});
    CHECK(read);
    if (read) {
        CHECK(read.value().bytes == output.size());
        CHECK(read.value().cumulative_queue_dropped_packets == 0U);
    }
    const auto stats = stream.stats(identity);
    CHECK(stats && stats.value().packets == 4U);
    CHECK(stats && stats.value().bytes == output.size());
    CHECK(stream.detach(identity));
}

void test_queue_drop_is_reported_without_closing_stream()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& tuner = value->tuner_backend();
    auto& stream = *value->stream_control();
    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    const TunerAttachment identity = attachment();
    CHECK(stream.attach(identity));
    fake->dropped = 7U;
    fake->next_packet[0] = 0x47U;
    std::array<std::uint8_t, 188U> output{};
    const auto read = stream.read(identity, {output.data(), output.size()},
                                  dtv::userland::Timeout{100U});
    CHECK(read);
    CHECK(read.value().bytes == 188U);
    CHECK(read.value().cumulative_queue_dropped_packets == 7U);
    const auto stats = stream.stats(identity);
    CHECK(stats);
    CHECK(stats.value().queue_drops == 7U);
    CHECK(stream.detach(identity));
}

void test_diagnostics_stop_failure()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& tuner = value->tuner_backend();
    auto& stream = *value->stream_control();
    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    const TunerAttachment identity = attachment();
    CHECK(stream.attach(identity));
    fake->stop_failure = true;
    CHECK(stream.detach(identity));
}

void test_invalid_receiver_and_unsupported_systems()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& tuner = value->tuner_backend();
    CHECK(tuner.open_receiver(1U).error() == Error::NOT_FOUND);
    CHECK(tuner.tune_terrestrial_hz(1U, 557142857U, 100U).error() == Error::NOT_FOUND);
    CHECK(tuner.is_locked(1U, px4::userland::ipc::System::ISDB_T).error() ==
          Error::NOT_FOUND);
    CHECK(tuner.tune_terrestrial_hz(0U, 557142857U, 100U));
    CHECK(!tuner.receiver_supports(0U, px4::userland::ipc::System::ISDB_S));
    CHECK(tuner.tune_satellite(0U, 1000U, 100U).error() == Error::UNSUPPORTED);
    CHECK(tuner.select_satellite_slot(0U, 0U, 100U).error() == Error::UNSUPPORTED);
    CHECK(tuner.select_satellite_tsid(0U, 1U, 100U).error() == Error::UNSUPPORTED);
    CHECK(fake->tune_calls == 1);
}

void test_stream_timeout_and_failure()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& stream = *value->stream_control();
    const TunerAttachment identity = attachment();
    CHECK(stream.attach(identity));

    fake->read_timeout = true;
    std::array<std::uint8_t, 188U> output{};
    const auto timeout = stream.read(identity, {output.data(), output.size()},
                                     dtv::userland::Timeout{10U});
    CHECK(timeout);
    CHECK(timeout.value().timed_out);
    CHECK(timeout.value().bytes == 0U);
    const auto stats = stream.stats(identity);
    CHECK(stats && stats.value().usb_errors == 0U);

    fake->read_timeout = false;
    fake->read_failure = true;
    const auto failed = stream.read(identity, {output.data(), output.size()},
                                    dtv::userland::Timeout{10U});
    CHECK(!failed);
    CHECK(failed.error() == Error::USB_IO);
    const auto failed_stats = stream.stats(identity);
    CHECK(failed_stats && failed_stats.value().usb_errors == 1U);
    CHECK(stream.detach(identity));
}

void test_attachment_and_card_errors()
{
    FakeDevice* fake = nullptr;
    auto value = family(&fake);
    CHECK(value != nullptr);
    if (!value) return;
    auto& stream = *value->stream_control();
    const TunerAttachment identity = attachment();
    std::array<std::uint8_t, 188U> output{};
    CHECK(stream.read(identity, {output.data(), output.size()},
                      dtv::userland::Timeout{1U}).error() == Error::NOT_FOUND);
    CHECK(stream.attach(identity));
    TunerAttachment wrong = identity;
    wrong.nonce[0] = 2U;
    CHECK(stream.read(wrong, {output.data(), output.size()},
                      dtv::userland::Timeout{1U}).error() == Error::NOT_FOUND);
    CHECK(stream.detach(wrong).error() == Error::NOT_FOUND);
    CHECK(stream.detach(identity));

    auto& card = value->card_backend();
    CHECK(card.set_power(true).error() == Error::UNSUPPORTED);
    CHECK(card.initialize_uart().error() == Error::UNSUPPORTED);
    CHECK(card.detect_card().error() == Error::UNSUPPORTED);
    auto& protocol = value->card_protocol();
    CHECK(protocol.initialize().error() == Error::UNSUPPORTED);
    CHECK(protocol.transmit({}, {}).error() == Error::UNSUPPORTED);
    CHECK(!protocol.initialized());
}

void test_daemon_backend_requires_device()
{
    torne::product::DaemonBackend backend;
    torne::daemon::DaemonConfig config;
    const auto selector =
        dtv::userland::identity::parse_device_selector("usb:255-99");
    CHECK(selector);
    if (selector) {
        config.has_device = true;
        config.device = selector.value();
        const auto opened = backend.open_family(config);
        CHECK(!opened);
        if (!opened) CHECK(opened.error() == Error::USB_IO);
    }
    CHECK(torne::product::make_family(nullptr) == nullptr);
}

}  // namespace

int main()
{
    test_tune_lock_capture_and_stream();
    test_read_and_stop_are_serialized();
    test_read_after_normal_stop_returns_eof();
    test_read_packets_batches_ready_queue();
    test_queue_drop_is_reported_without_closing_stream();
    test_diagnostics_stop_failure();
    test_invalid_receiver_and_unsupported_systems();
    test_stream_timeout_and_failure();
    test_attachment_and_card_errors();
    test_daemon_backend_requires_device();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("product core family tests passed\n");
    return 0;
}
