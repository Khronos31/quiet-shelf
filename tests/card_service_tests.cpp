// SPDX-License-Identifier: GPL-2.0-only
#include "product/core_family.h"
#include "product/device.h"

#include "card/uart.h"

#include <dtv/userland/error.h>
#include <px4/card_service.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

namespace {

namespace card = torne::card;
using dtv::userland::Error;
using dtv::userland::Result;

int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

// Synthetic, non-card-specific ATR: TS=3B, T0=80 (one TD), TD1=01 (T=1),
// and TCK=81 makes the T=1 checksum zero. No B-CAS material.
constexpr std::array<std::uint8_t, 4U> kSyntheticAtr{
    0x3BU, 0x80U, 0x01U, 0x81U};

std::uint8_t lrc(const std::uint8_t* data, std::size_t length)
{
    std::uint8_t value = 0U;
    for (std::size_t i = 0U; i < length; ++i) value ^= data[i];
    return value;
}

class FakeClock final : public torne::product::Clock, public card::Clock {
public:
    std::uint64_t now = 0U;
    bool real_sleep = false;

    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        if (real_sleep)
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        now += milliseconds;
    }
};

class FakeLine final : public card::Line {
public:
    bool present = true;
    bool invalid = false;
    int reset_calls = 0;
    int power_off_calls = 0;
    int baud_rate = 0;
    std::uint32_t reset_sleep_ms = 1U;
    std::size_t read_chunk = 0U;
    bool fail_read = false;
    torne::usb::TransferCause read_cause = torne::usb::TransferCause::timeout;
    bool fail_write = false;
    std::atomic<bool> reset_entered{false};
    std::vector<std::vector<std::uint8_t>> writes;
    std::deque<std::uint8_t> rx;

    card::PresenceResult detect_card() noexcept override
    {
        card::PresenceResult result;
        result.result.ok = true;
        result.result.step = card::Step::presence;
        result.present = present;
        return result;
    }

    card::Result reset_card(card::Clock& clock) noexcept override
    {
        ++reset_calls;
        invalid = false;
        reset_entered.store(true);
        clock.sleep_ms(reset_sleep_ms);
        rx.assign(kSyntheticAtr.begin(), kSyntheticAtr.end());
        return card::Result{true, card::Step::reset_lines_release, card::Error::none, {}};
    }

    card::Result power_down() noexcept override
    {
        ++power_off_calls;
        rx.clear();
        return card::Result{true, card::Step::power, card::Error::none, {}};
    }

    card::CountResult data_ready() noexcept override
    {
        card::CountResult result;
        result.result.ok = true;
        result.result.step = card::Step::fifo_count;
        result.count = static_cast<std::uint32_t>(rx.size());
        return result;
    }

    card::ReadResult read_data(std::uint8_t* buffer,
                               std::size_t length) noexcept override
    {
        card::ReadResult result;
        if (fail_read) {
            result.result.step = card::Step::read_rx;
            result.result.error = card::Error::outcome_unknown;
            result.result.transfer = torne::usb::TransferOutcome{
                torne::usb::TransferState::outcome_unknown,
                read_cause, 0U, false};
            return result;
        }
        std::size_t count = length < rx.size() ? length : rx.size();
        if (read_chunk != 0U && count > read_chunk) count = read_chunk;
        for (std::size_t i = 0U; i < count; ++i) {
            buffer[i] = rx.front();
            rx.pop_front();
        }
        result.result.ok = true;
        result.result.step = card::Step::read_rx;
        result.bytes_read = count;
        return result;
    }

    card::Result write_data(const std::uint8_t* data,
                            std::size_t length) noexcept override
    {
        if (fail_write) {
            fail_write = false;
            card::Result result;
            result.step = card::Step::write_tx;
            result.error = card::Error::outcome_unknown;
            result.transfer = torne::usb::TransferOutcome{
                torne::usb::TransferState::outcome_unknown,
                torne::usb::TransferCause::timeout, 0U, false};
            return result;
        }
        std::vector<std::uint8_t> frame(data, data + length);
        writes.push_back(frame);
        if (frame.size() < 3U) {
            card::Result result;
            result.error = card::Error::invalid_argument;
            return result;
        }
        const std::uint8_t pcb = frame[1U];
        const std::uint8_t length_byte = frame[2U];
        if (frame.size() <= 3U + length_byte)
            return card::Result{false, card::Step::write_tx,
                                card::Error::short_transfer, {}};
        const std::uint8_t expected_lrc =
            lrc(frame.data(), 3U + length_byte);
        if (frame[3U + length_byte] != expected_lrc)
            return card::Result{false, card::Step::write_tx,
                                card::Error::invalid_argument, {}};
        if (pcb == 0xC0U) {
            rx.insert(rx.end(), {0x00U, 0xE0U, 0x00U, 0xE0U});
        } else if (pcb == 0xC1U) {
            const std::uint8_t ifsd = frame[3U];
            const std::uint8_t body[] = {0x00U, 0xE1U, 0x01U, ifsd};
            rx.insert(rx.end(), body, body + sizeof(body));
            rx.push_back(lrc(body, sizeof(body)));
        } else if ((pcb & 0xC0U) == 0U) {
            const std::uint8_t body[] = {0x00U,
                                         static_cast<std::uint8_t>(pcb & 0x40U),
                                         0x02U, 0x90U, 0x00U};
            rx.insert(rx.end(), body, body + sizeof(body));
            rx.push_back(lrc(body, sizeof(body)));
        } else {
            return card::Result{false, card::Step::write_tx,
                                card::Error::invalid_argument, {}};
        }
        return card::Result{true, card::Step::send, card::Error::none, {}};
    }

    card::Result set_baud_rate(std::uint32_t rate) noexcept override
    {
        baud_rate = static_cast<int>(rate);
        return card::Result{true, card::Step::set_baud, card::Error::none, {}};
    }

    void invalidate() noexcept override { invalid = true; }
    bool valid() const noexcept override { return !invalid; }
};

class FakeDevice final : public torne::product::Device {
public:
    FakeClock clock;
    FakeLine line;
    int close_calls = 0;
    int start_calls = 0;
    std::array<std::uint8_t, 188U> packet{};

    Result<void> tune(std::uint64_t hz, std::uint32_t) noexcept override
    {
        return hz == 0U ? Result<void>::failure(Error::INVALID_ARGUMENT)
                        : Result<void>::success();
    }
    Result<void> start_stream() noexcept override
    {
        ++start_calls;
        return Result<void>::success();
    }
    Result<void> stop_stream() noexcept override { return Result<void>::success(); }
    Result<void> read_packet(std::array<std::uint8_t, 188U>& output,
                             std::uint32_t) noexcept override
    {
        output = packet;
        return Result<void>::success();
    }
    Result<torne::product::Status> status() noexcept override
    {
        torne::product::Status value;
        return Result<torne::product::Status>::success(value);
    }
    torne::product::CardPort card_port() noexcept override
    {
        return torne::product::CardPort{&line, &clock};
    }
    void close() noexcept override { ++close_calls; }
};

px4::userland::TunerAttachment attachment()
{
    px4::userland::TunerAttachment value{};
    value.owner_client_id = 7U;
    value.lease_id = 8U;
    value.attachment_id = 9U;
    value.receiver = 0U;
    value.system = px4::userland::ipc::System::ISDB_T;
    value.nonce[0] = 1U;
    return value;
}

struct FamilyFixture final {
    FakeDevice* device = nullptr;
    std::unique_ptr<dtv::userland::host_runtime::EnclosureFamily> family;
    std::unique_ptr<px4::userland::CardService> service;

    FamilyFixture()
    {
        auto device_ptr = std::make_unique<FakeDevice>();
        device = device_ptr.get();
        family = torne::product::make_family(std::move(device_ptr));
        if (family)
            service = std::make_unique<px4::userland::CardService>(
                family->card_backend(), family->card_protocol());
    }
};

const std::uint8_t kApdu[] = {0x00U, 0x11U, 0x22U, 0x33U};

void test_normal_t1_and_service()
{
    FamilyFixture fixture;
    CHECK(fixture.family != nullptr);
    CHECK(fixture.service != nullptr);
    if (!fixture.family || !fixture.service) return;

    auto connected = fixture.service->connect(
        1U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(connected);
    if (!connected) return;
    CHECK(connected.value().atr.length == kSyntheticAtr.size());
    CHECK(fixture.device->line.reset_calls == 1);
    CHECK(fixture.device->line.baud_rate == 9600);
    CHECK(fixture.device->line.writes.size() == 2U);  // S-resync + S-IFS

    std::array<std::uint8_t, 16U> response{};
    const auto apdu = dtv::userland::ByteView{kApdu, sizeof(kApdu)};
    const auto transmitted = fixture.service->transmit(
        1U, connected.value().handle, apdu,
        dtv::userland::MutableByteView{response.data(), response.size()});
    CHECK(transmitted);
    if (transmitted) {
        CHECK(transmitted.value() == 2U);
        CHECK(response[0] == 0x90U && response[1] == 0x00U);
    }
    CHECK(fixture.device->line.writes.size() == 3U);  // one I-block, no retry

    const auto reset = fixture.service->reset(1U, connected.value().handle);
    CHECK(reset);
    CHECK(fixture.device->line.reset_calls == 2);
    CHECK(fixture.service->transmit(
        1U, connected.value().handle, apdu,
        dtv::userland::MutableByteView{response.data(), response.size()}));

    const auto second = fixture.service->connect(
        2U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(second.error() == Error::BUSY);
    const auto shared = fixture.service->connect(
        2U, px4::userland::ipc::ShareMode::shared, true);
    CHECK(shared.error() == Error::BUSY);

    const auto disconnected = fixture.service->disconnect(
        1U, connected.value().handle, px4::userland::ipc::Disposition::leave);
    CHECK(disconnected);
    CHECK(fixture.device->line.power_off_calls == 1);
    std::printf("normal T1/CardService ok\n");
}

void test_shared_and_shutdown()
{
    FamilyFixture fixture;
    CHECK(fixture.service != nullptr);
    if (!fixture.service) return;
    auto first = fixture.service->connect(
        1U, px4::userland::ipc::ShareMode::shared, true);
    auto second = fixture.service->connect(
        2U, px4::userland::ipc::ShareMode::shared, true);
    CHECK(first);
    CHECK(second);
    if (!first || !second) return;
    CHECK(first.value().handle != second.value().handle);
    CHECK(fixture.device->line.reset_calls == 1);
    CHECK(fixture.service->handle_count() == 2U);

    CHECK(fixture.service->disconnect(
        1U, first.value().handle, px4::userland::ipc::Disposition::leave));
    CHECK(fixture.device->line.power_off_calls == 0);
    CHECK(fixture.service->disconnect(
        2U, second.value().handle, px4::userland::ipc::Disposition::leave));
    CHECK(fixture.device->line.power_off_calls == 1);
    CHECK(fixture.service->shutdown());
    std::printf("shared/shutdown ok\n");
}

void test_absent_and_usb_loss()
{
    FamilyFixture absent;
    absent.device->line.present = false;
    const auto no_card = absent.service->connect(
        1U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(no_card.error() == Error::NO_CARD);
    CHECK(absent.device->line.reset_calls == 0);

    FamilyFixture lost;
    lost.device->line.fail_read = true;
    lost.device->line.read_cause = torne::usb::TransferCause::no_device;
    const auto disconnected = lost.service->connect(
        1U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(disconnected.error() == Error::DISCONNECTED);
    CHECK(lost.device->line.reset_calls == 1);
    std::printf("absent/USB loss ok\n");
}

void test_removal_and_generation()
{
    FamilyFixture fixture;
    CHECK(fixture.service != nullptr);
    if (!fixture.service) return;
    auto connected = fixture.service->connect(
        1U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(connected);
    if (!connected) return;
    const auto generation = fixture.service->reader_generation();

    fixture.device->line.present = false;
    const auto change = fixture.service->poll_presence();
    CHECK(change);
    if (change) {
        CHECK(change.value().changed);
        CHECK(!change.value().present);
        CHECK(change.value().reader_generation == generation + 1U);
    }
    std::array<std::uint8_t, 16U> response{};
    CHECK(fixture.service->transmit(
        1U, connected.value().handle,
        dtv::userland::ByteView{kApdu, sizeof(kApdu)},
        dtv::userland::MutableByteView{response.data(), response.size()})
        .error() == Error::CARD_REMOVED);

    CHECK(fixture.service->handle_status(
        1U, connected.value().handle).error() == Error::CARD_REMOVED);
    CHECK(fixture.service->disconnect(
        1U, connected.value().handle, px4::userland::ipc::Disposition::leave));
    fixture.device->line.present = true;
    auto replacement = fixture.service->connect(
        2U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(replacement);
    if (replacement) {
        CHECK(replacement.value().handle != connected.value().handle);
        CHECK(fixture.service->transmit(
            2U, replacement.value().handle,
            dtv::userland::ByteView{kApdu, sizeof(kApdu)},
            dtv::userland::MutableByteView{response.data(), response.size()}));
    }
    std::printf("removal/generation ok\n");
}

void test_short_reads_and_failures()
{
    FamilyFixture short_read;
    short_read.device->line.read_chunk = 1U;
    auto short_connected = short_read.service->connect(
        1U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(short_connected);
    if (short_connected)
        CHECK(short_connected.value().atr.length == kSyntheticAtr.size());
    CHECK(short_read.device->line.reset_calls == 1);

    FamilyFixture read_failure;
    read_failure.device->line.fail_read = true;
    const auto read_connect = read_failure.service->connect(
        1U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(read_connect.error() == Error::USB_IO);
    CHECK(read_failure.device->line.reset_calls == 1);  // no ATR reset retry

    FamilyFixture write_failure;
    auto connected = write_failure.service->connect(
        1U, px4::userland::ipc::ShareMode::exclusive, true);
    CHECK(connected);
    if (!connected) return;
    std::array<std::uint8_t, 16U> response{};
    const std::size_t writes_before = write_failure.device->line.writes.size();
    write_failure.device->line.fail_write = true;
    const auto failed = write_failure.service->transmit(
        1U, connected.value().handle,
        dtv::userland::ByteView{kApdu, sizeof(kApdu)},
        dtv::userland::MutableByteView{response.data(), response.size()});
    CHECK(failed.error() == Error::USB_IO);
    CHECK(write_failure.device->line.writes.size() == writes_before);
    CHECK(write_failure.device->line.reset_calls == 1);  // no inner retry

    // Known Core 9d8 boundary: session.invalidate() does not stale a bound
    // handle. The same handle is reinitialized on the next transmit.
    const auto reinit = write_failure.service->transmit(
        1U, connected.value().handle,
        dtv::userland::ByteView{kApdu, sizeof(kApdu)},
        dtv::userland::MutableByteView{response.data(), response.size()});
    if (reinit) {
        std::printf("KNOWN GAP: session invalidate reused the same generation-bound handle\n");
        CHECK(write_failure.device->line.reset_calls == 2);
    }
    std::printf("short/failure boundaries ok\n");
}

void test_card_wait_does_not_block_ts()
{
    FamilyFixture fixture;
    CHECK(fixture.family != nullptr);
    CHECK(fixture.service != nullptr);
    if (!fixture.family || !fixture.service) return;
    fixture.device->clock.real_sleep = true;
    fixture.device->line.reset_sleep_ms = 50U;
    fixture.device->packet[0] = 0x47U;
    fixture.device->packet[1] = 0x5aU;

    auto& tuner = fixture.family->tuner_backend();
    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    auto& stream = *fixture.family->stream_control();
    const auto identity = attachment();
    CHECK(stream.attach(identity));

    std::atomic<bool> card_done{false};
    std::thread card_thread([&] {
        (void)fixture.service->connect(
            1U, px4::userland::ipc::ShareMode::exclusive, true);
        card_done.store(true);
    });
    for (int attempt = 0; attempt < 500 &&
                          !fixture.device->line.reset_entered.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(fixture.device->line.reset_entered.load());
    std::array<std::uint8_t, 188U> output{};
    const auto read = stream.read(identity, {output.data(), output.size()},
                                  dtv::userland::Timeout{100U});
    CHECK(read);
    if (read) {
        CHECK(read.value().bytes == 188U);
        CHECK(output[0] == 0x47U && output[1] == 0x5aU);
    }
    CHECK(!card_done.load());
    card_thread.join();
    CHECK(card_done.load());
    CHECK(stream.detach(identity));
    std::printf("TS during card wait ok\n");
}

}  // namespace

int main()
{
    test_normal_t1_and_service();
    test_shared_and_shutdown();
    test_removal_and_generation();
    test_short_reads_and_failures();
    test_absent_and_usb_loss();
    test_card_wait_does_not_block_ts();
    if (failures != 0) {
        std::fprintf(stderr, "%d card service checks failed\n", failures);
        return 1;
    }
    std::printf("card service tests: ALL OK\n");
    return 0;
}
