// SPDX-License-Identifier: GPL-2.0-only
#include "core0/host_runtime.h"

#include "core0/framer.h"
#include "core0/ts_pump.h"
#include "usb/transport.h"
#include "widen/bridge.h"

#include <dtv/userland/detail/sha256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace {

namespace usb = torne::usb;
namespace core0 = torne::core0;
namespace widen = torne::widen;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

constexpr std::uint32_t kBase = 0x01600000U;
constexpr std::uint32_t kData = 0x016A3800U;
constexpr std::uint32_t kCtrl = 0x016A3840U;
constexpr std::uint32_t kStat = 0x016A3880U;

std::uint32_t be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

void put32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 24U);
    p[1] = static_cast<std::uint8_t>(v >> 16U);
    p[2] = static_cast<std::uint8_t>(v >> 8U);
    p[3] = static_cast<std::uint8_t>(v);
}

usb::DeviceSummary summary()
{
    usb::DeviceSummary s;
    s.vid = 0x054cU;
    s.pid = 0x04b2U;
    s.num_configurations = 1U;
    s.configuration_value = 1U;
    s.num_interfaces = 1U;
    s.interface_zero.number = 0U;
    s.interface_zero.alternate = 0U;
    s.interface_zero.interface_class = 0xffU;
    s.interface_zero.endpoint_count = 4U;
    s.interface_zero.endpoints[0] = {0x81U, 0x02U, 512U, 0U};
    s.interface_zero.endpoints[1] = {0x02U, 0x02U, 512U, 0U};
    s.interface_zero.endpoints[2] = {0x83U, 0x03U, 64U, 1U};
    s.interface_zero.endpoints[3] = {0x84U, 0x02U, 512U, 0U};
    return s;
}

usb::DeviceLocation location()
{
    usb::DeviceLocation l;
    l.bus = 1U;
    l.port_count = 1U;
    l.ports[0] = 2U;
    return l;
}

struct ImageFixture final {
    std::vector<std::uint8_t> bytes;
    core0::Image image;
};

ImageFixture make_image()
{
    ImageFixture fixture;
    fixture.bytes.resize(0x100U);
    for (std::size_t i = 0U; i < fixture.bytes.size(); ++i)
        fixture.bytes[i] = static_cast<std::uint8_t>(i * 3U + 1U);
    fixture.image.bytes = fixture.bytes.data();
    fixture.image.image_bytes = fixture.bytes.size();
    fixture.image.build_id = 0x55667788U;
    fixture.image.entry = 0x000A0000U;
    fixture.image.capabilities = 1U;
    fixture.image.param0 = 0x840U;
    dtv::userland::detail::Sha256 digest;
    digest.update(fixture.bytes.data(), fixture.bytes.size());
    fixture.image.sha256 = digest.finish();
    return fixture;
}

struct PendingEvent final {
    std::uint64_t id = 0U;
    std::uint8_t* data = nullptr;
    std::size_t length = 0U;
    usb::TransferOutcome transfer{};
};

struct FakeState final {
    usb::DeviceSummary descriptor = summary();
    std::map<std::uint32_t, std::uint8_t> memory;
    std::map<std::uint64_t, std::pair<std::uint8_t*, std::size_t>> active;
    std::deque<PendingEvent> events;
    std::uint64_t next_id = 1U;
    std::uint32_t image_bytes = 0U;
    std::uint32_t build_id = 0U;
    std::uint32_t param0 = 0x840U;
    std::uint32_t heartbeat = 0U;
    bool heartbeat_stalled = false;
    bool run_outcome_unknown = false;
    int boot_reads = 0;
    int stat_reads = 0;
    bool fault = false;
    std::uint32_t fail_trigger_address = 0U;
    bool trigger_failed = false;
    int trigger_writes = 0;
    bool hold_callbacks = false;
    bool hold_fail = false;
    bool poll_error = false;
    std::uint32_t fail_abs_address = 0U;
    std::uint32_t fail_abs_once_address = 0U;
    bool abs_once_failed = false;
    std::vector<std::uint32_t> writes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> word_writes;
};

void write_mem(FakeState& s, std::uint32_t a, const std::uint8_t* d, std::size_t n)
{
    for (std::size_t i = 0U; i < n; ++i)
        s.memory[a + static_cast<std::uint32_t>(i)] = d[i];
}

bool has_word(const FakeState& s, std::uint32_t address, std::uint32_t value)
{
    return std::find(s.word_writes.begin(), s.word_writes.end(),
                     std::make_pair(address, value)) != s.word_writes.end();
}

void read_mem(FakeState& s, std::uint32_t a, std::uint8_t* d, std::size_t n)
{
    for (std::size_t i = 0U; i < n; ++i) {
        const auto it = s.memory.find(a + static_cast<std::uint32_t>(i));
        d[i] = it == s.memory.end() ? 0U : it->second;
    }
}

std::uint32_t mem32(const FakeState& s, std::uint32_t address)
{
    std::uint8_t bytes[4] = {};
    for (std::size_t i = 0U; i < 4U; ++i) {
        const auto it = s.memory.find(address + static_cast<std::uint32_t>(i));
        bytes[i] = it == s.memory.end() ? 0U : it->second;
    }
    return be32(bytes);
}

void set_info(FakeState& s)
{
    std::uint8_t block[64] = {};
    put32(block + 0x00U, 0x54545330U);
    put32(block + 0x04U, 0x00010000U);
    put32(block + 0x08U, s.build_id);
    put32(block + 0x0CU, 1U);
    put32(block + 0x10U, mem32(s, kCtrl));
    put32(block + 0x14U, s.image_bytes);
    put32(block + 0x18U, 0x000A3800U);
    put32(block + 0x1CU, s.param0);
    write_mem(s, kData, block, sizeof(block));
}

void set_stat(FakeState& s)
{
    std::uint8_t block[64] = {};
    put32(block + 0x00U, s.fault ? 0xE0U : (s.stat_reads < s.boot_reads ? 1U : 3U));
    put32(block + 0x04U, s.heartbeat);
    put32(block + 0x18U, mem32(s, kCtrl + 0x10U));
    write_mem(s, kStat, block, sizeof(block));
}

class FakeIo final : public usb::UsbIo {
public:
    explicit FakeIo(FakeState& s) : s_(s) {}

    usb::OpenOutcome open(const usb::DeviceLocation&, usb::DeviceSummary& out) noexcept override
    {
        out = s_.descriptor;
        return usb::OpenOutcome{usb::OpenState::ok, usb::TransferCause::none};
    }
    usb::OpenOutcome claim_interface(std::uint8_t) noexcept override
    {
        return usb::OpenOutcome{usb::OpenState::ok, usb::TransferCause::none};
    }

    usb::TransferOutcome control_in(std::uint8_t type, std::uint8_t request,
                                    std::uint16_t value, std::uint16_t index,
                                    std::uint16_t length, std::uint8_t* data,
                                    std::uint32_t) noexcept override
    {
        if (type != 0xC0U || request != 0xBCU || data == nullptr)
            return fail(usb::TransferCause::invalid);
        const std::uint32_t address =
            (static_cast<std::uint32_t>(value & 0xFF00U) << 8U) |
            (static_cast<std::uint32_t>(value & 0x00FFU) << 24U) | index;
        if (address == 0x00FDBD08U) {
            std::uint8_t zero[4] = {};
            std::memcpy(data, zero, length < 4U ? length : 4U);
            return ok(length);
        }
        if (address == kStat) {
            ++s_.stat_reads;
            if (!s_.heartbeat_stalled) ++s_.heartbeat;
            set_stat(s_);
        }
        read_mem(s_, address, data, length);
        return ok(length);
    }

    usb::TransferOutcome control_out(std::uint8_t type, std::uint8_t request,
                                     std::uint16_t value, std::uint16_t index,
                                     std::uint16_t length,
                                     const std::uint8_t* data,
                                     std::uint32_t) noexcept override
    {
        if (type != 0x40U || request != 0xBCU || data == nullptr)
            return fail(usb::TransferCause::invalid);
        const std::uint32_t address =
            (static_cast<std::uint32_t>(value & 0xFF00U) << 8U) |
            (static_cast<std::uint32_t>(value & 0x00FFU) << 24U) | index;
        s_.writes.push_back(address);
        if (length == 4U)
            s_.word_writes.emplace_back(address, be32(data));
        if (address == s_.fail_abs_once_address && !s_.abs_once_failed) {
            s_.abs_once_failed = true;
            write_mem(s_, address, data, length);
            return usb::TransferOutcome{usb::TransferState::failed,
                                        usb::TransferCause::timeout, 0U, false};
        }
        if (address == s_.fail_abs_address)
            return fail(usb::TransferCause::no_device);
        if (address == s_.fail_trigger_address && length == 4U &&
            !s_.trigger_failed) {
            s_.trigger_failed = true;
            ++s_.trigger_writes;
            write_mem(s_, address, data, length);
            return usb::TransferOutcome{usb::TransferState::outcome_unknown,
                                        usb::TransferCause::timeout, 0U, false};
        }
        if (address == kBase && length == 4U) {
            const std::uint32_t word = be32(data);
            if (word == 0xFFFFFFFFU && s_.hold_fail)
                return fail(usb::TransferCause::no_device);
            if (word == 0xFFFCFFFFU) {
                if (s_.run_outcome_unknown)
                    return usb::TransferOutcome{usb::TransferState::outcome_unknown,
                                                usb::TransferCause::timeout, 0U,
                                                false};
                set_info(s_);
                set_stat(s_);
            }
        }
        write_mem(s_, address, data, length);
        return ok(length);
    }

    usb::TransferOutcome bulk_in(std::uint8_t, std::uint8_t*, std::size_t,
                                 std::uint32_t) noexcept override
    {
        return fail(usb::TransferCause::invalid);
    }
    usb::TransferOutcome bulk_out(std::uint8_t, const std::uint8_t*,
                                  std::size_t, std::uint32_t) noexcept override
    {
        return fail(usb::TransferCause::invalid);
    }

    int async_bulk_in_submit(std::uint8_t endpoint, std::uint8_t* data,
                             std::size_t length, std::uint32_t,
                             std::uint64_t* id) noexcept override
    {
        if (endpoint != 0x81U || data == nullptr || id == nullptr) return -1;
        *id = s_.next_id++;
        s_.active[*id] = {data, length};
        return 0;
    }
    int async_bulk_cancel_all() noexcept override
    {
        if (s_.hold_callbacks) return 0;
        for (const auto& entry : s_.active) {
            PendingEvent event;
            event.id = entry.first;
            event.data = entry.second.first;
            event.length = 0U;
            event.transfer = usb::TransferOutcome{
                usb::TransferState::failed, usb::TransferCause::other, 0U, false};
            s_.events.push_back(event);
        }
        s_.active.clear();
        return 0;
    }
    int async_bulk_poll(usb::UsbIo::AsyncBulkEvent& event) noexcept override
    {
        if (s_.poll_error) return -1;
        if (s_.events.empty()) return 0;
        PendingEvent pending = s_.events.front();
        s_.events.pop_front();
        event.id = pending.id;
        event.data = pending.data;
        event.length = pending.length;
        event.transfer = pending.transfer;
        return 1;
    }
    std::size_t async_bulk_active() const noexcept override
    {
        return s_.active.size();
    }

    void release_interface(std::uint8_t) noexcept override {}
    void close() noexcept override {}

    void release_callbacks()
    {
        for (const auto& entry : s_.active) {
            PendingEvent event;
            event.id = entry.first;
            event.data = entry.second.first;
            event.length = 0U;
            event.transfer = usb::TransferOutcome{
                usb::TransferState::failed, usb::TransferCause::other, 0U, false};
            s_.events.push_back(event);
        }
        s_.active.clear();
    }

    void complete_one(std::size_t length = 192U)
    {
        if (s_.active.empty()) return;
        auto it = s_.active.begin();
        PendingEvent event;
        event.id = it->first;
        event.data = it->second.first;
        event.length = length;
        event.transfer = usb::TransferOutcome{
            usb::TransferState::complete, usb::TransferCause::none, length, false};
        if (length >= 192U) {
            std::memset(event.data, 0, 4U);
            event.data[4] = 0x47U;
            for (std::size_t i = 5U; i < 192U; ++i)
                event.data[i] = static_cast<std::uint8_t>(i);
        }
        s_.active.erase(it);
        s_.events.push_back(event);
    }

private:
    static usb::TransferOutcome ok(std::size_t length) noexcept
    {
        return usb::TransferOutcome{usb::TransferState::complete,
                                    usb::TransferCause::none, length, false};
    }
    static usb::TransferOutcome fail(usb::TransferCause cause) noexcept
    {
        return usb::TransferOutcome{usb::TransferState::failed, cause, 0U, false};
    }
    FakeState& s_;
};

class TestClock final : public core0::Clock {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t ms) noexcept override { now += ms; }
    std::uint64_t now = 0U;
};

class Flush final : public core0::I2cFlush {
public:
    bool flush() noexcept override { return ok; }
    bool ok = true;
};

struct PacketSinkState final {
    std::size_t count = 0U;
};

void packet_sink(void* context, const std::uint8_t* packet,
                 std::size_t length) noexcept
{
    auto* state = static_cast<PacketSinkState*>(context);
    if (packet != nullptr && length == 188U) ++state->count;
}

const std::array<std::pair<std::uint32_t, std::uint32_t>, 32U>& expected_t2()
{
    static const std::array<std::pair<std::uint32_t, std::uint32_t>, 32U> values = {{
        {0x00D000C4U, 0x00000000U}, {0x00D00000U, 0xFFFFFFFFU},
        {0x00D00004U, 0x001F33FFU}, {0x00D00000U, 0x30030FFFU},
        {0x00D00000U, 0x00030000U}, {0x00D00004U, 0x00003305U},
        {0x00D01040U, 0x00000000U}, {0x00D01064U, 0x00000000U},
        {0x00D010A0U, 0x00000000U}, {0x00D010A4U, 0x00000000U},
        {0x00D010A8U, 0x00000000U}, {0x00D01020U, 0x02000000U},
        {0x00D01004U, 0x00000000U}, {0x00D01044U, 0x00BC0000U},
        {0x00D01048U, 0x47000000U}, {0x00D0104CU, 0x00FFFFFFU},
        {0x00D01050U, 0x00000000U}, {0x00D01060U, 0x00000000U},
        {0x00D01000U, 0x06070000U}, {0x00D01080U, 0x01000000U},
        {0x00D01088U, 0x00000000U}, {0x00D0108CU, 0x000000FFU},
        {0x00D01084U, 0x00000000U}, {0x00D01090U, 0x01000000U},
        {0x00D02028U, 0x00000000U}, {0x00D0202CU, 0x00000000U},
        {0x00D02030U, 0x00000000U}, {0x00D02038U, 0x00000000U},
        {0x00D02000U, 0x03020300U}, {0x00D02020U, 0x00AE4000U},
        {0x00D02024U, 0x00600000U}, {0x00D00004U, 0x00043305U}}};
    return values;
}

void test_framer_cross_boundary_lookahead_and_cc()
{
    std::array<std::uint8_t, 192U> record{};
    record[4] = 0x47U;
    for (std::size_t i = 5U; i < record.size(); ++i)
        record[i] = static_cast<std::uint8_t>(i);
    std::array<std::uint8_t, 384U> pair{};
    std::memcpy(pair.data(), record.data(), record.size());
    std::memcpy(pair.data() + 192U, record.data(), record.size());

    core0::Framer framer;
    std::vector<std::array<std::uint8_t, 188U>> packets;
    framer.push(pair.data(), 100U, packets);
    framer.push(pair.data() + 100U, pair.size() - 100U, packets);
    CHECK(packets.size() == 1U);
    CHECK(packets[0][0] == 0x47U);
    CHECK(framer.stats().packets == 1U);

    std::array<std::uint8_t, 384U> bad = pair;
    bad[192U + 4U] = 0x00U;
    core0::Framer bad_framer;
    std::vector<std::array<std::uint8_t, 188U>> bad_packets;
    bad_framer.push(bad.data(), bad.size(), bad_packets);
    CHECK(bad_packets.empty());
    CHECK(bad_framer.stats().sync_errors > 0U);

    std::array<std::uint8_t, 576U> three{};
    for (std::size_t i = 0U; i < 3U; ++i) {
        std::memcpy(three.data() + i * 192U, record.data(), record.size());
        three[i * 192U + 4U] = 0x47U;
        three[i * 192U + 5U] = 0x00U;  // payload PID 0x0100
        three[i * 192U + 6U] = 0x00U;
        three[i * 192U + 7U] = static_cast<std::uint8_t>(i == 0U ? 0x10U : (i == 1U ? 0x20U : 0x11U));
    }
    core0::Framer cc_framer;
    std::vector<std::array<std::uint8_t, 188U>> cc_packets;
    cc_framer.push(three.data(), three.size(), cc_packets);
    CHECK(cc_packets.size() == 2U);
    CHECK(cc_framer.stats().cc_errors == 0U);
}

void test_framer_null_pid_cc_is_ignored()
{
    const auto make_record = [](std::uint16_t pid, std::uint8_t cc, bool tei) {
        std::array<std::uint8_t, 192U> record{};
        record[4] = 0x47U;
        record[5] = static_cast<std::uint8_t>(((pid >> 8U) & 0x1FU) |
                                              (tei ? 0x80U : 0x00U));
        record[6] = static_cast<std::uint8_t>(pid & 0xFFU);
        record[7] = static_cast<std::uint8_t>(0x10U | (cc & 0x0FU));
        return record;
    };
    const std::array<std::uint16_t, 5U> pids = {
        0x1FFFU, 0x1FFFU, 0x0100U, 0x0100U, 0x0100U};
    const std::array<std::uint8_t, 5U> ccs = {0U, 9U, 0U, 1U, 2U};
    std::array<std::uint8_t, 5U * 192U> stream{};
    for (std::size_t i = 0U; i < pids.size(); ++i) {
        const auto record = make_record(pids[i], ccs[i], i < 2U);
        std::memcpy(stream.data() + i * 192U, record.data(), record.size());
    }
    core0::Framer framer;
    std::vector<std::array<std::uint8_t, 188U>> packets;
    framer.push(stream.data(), stream.size(), packets);
    CHECK(packets.size() == 4U);
    CHECK(framer.stats().packets == 4U);
    CHECK(framer.stats().tei_packets == 2U);
    CHECK(framer.stats().cc_errors == 0U);
}

void test_runtime_start_pump_stop()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.param0 = image.image.param0;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;

    const auto result = runtime.start(bridge, *opened.transport, image.image, 7U,
                                      true, flush, clock, pump);
    CHECK(result.ok);
    CHECK(result.input_open);
    CHECK(result.session != 0U && (result.session >> 16U) != 0U);
    CHECK(state.word_writes.size() >= expected_t2().size());
    for (std::size_t i = 0U; i < expected_t2().size(); ++i)
        CHECK(state.word_writes[i] == expected_t2()[i]);
    CHECK(has_word(state, 0x00D010A4U, 0x13170400U));
    CHECK(pump.running());
    CHECK(state.active.size() == pump.depth());
    CHECK(!state.writes.empty());
    CHECK(state.writes.front() == 0x00D000C4U);

}

void test_runtime_held_start_keeps_input_closed_5s()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.param0 = image.image.param0;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;

    const auto started = runtime.start(bridge, *opened.transport, image.image, 2U,
                                       true, flush, clock, pump,
                                       core0::StartOptions{false});
    CHECK(started.ok);
    CHECK(!started.input_open);
    CHECK(started.stage == core0::Stage::bulk_start);
    CHECK(pump.running());
    CHECK(std::find(state.writes.begin(), state.writes.end(), 0x00D00484U) ==
          state.writes.end());
    CHECK(std::find(state.writes.begin(), state.writes.end(), 0x00D00488U) ==
          state.writes.end());
    CHECK(!has_word(state, 0x00D01040U, 0x03000000U));
    CHECK(!has_word(state, 0x00D010A4U, 0x13170400U));
    CHECK(!has_word(state, 0x00D02010U, 0x02000000U));
    CHECK(mem32(state, 0x00D01020U) == 0x02000000U);
    CHECK(mem32(state, 0x00D01044U) == 0x00BC0000U);
    CHECK(mem32(state, 0x00D01048U) == 0x47000000U);
    CHECK(mem32(state, 0x00D01080U) == 0x01000000U);
    CHECK(mem32(state, 0x00D02020U) == 0x00AE4000U);
    CHECK(mem32(state, 0x00D02024U) == 0x00600000U);
    CHECK(mem32(state, 0x00D02038U) == 0x00000000U);

    clock.now += 5000U;
    CHECK(pump.pump() == 0);
    CHECK(framer.stats().input_bytes == 0U);
    CHECK(framer.stats().packets == 0U);
    CHECK(pump.running());

    const auto stopped = runtime.stop(bridge, pump);
    CHECK(stopped.ok);
    CHECK(!pump.running());
    CHECK(state.active.empty());
}

void test_full_t2_added_mmio_failure_stops_before_bulk()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.fail_abs_address = 0x00D02000U;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    const auto result = runtime.start(bridge, *opened.transport, image.image, 4U,
                                      true, flush, clock, pump);
    CHECK(!result.ok);
    CHECK(result.stage == core0::Stage::engine_setup);
    CHECK(result.error == core0::Error::device_gone);
    CHECK(!pump.running());
    CHECK(state.active.empty());
    CHECK(std::find(state.writes.begin(), state.writes.end(), 0x00D00484U) ==
          state.writes.end());
}

void test_full_t2_added_mmio_is_abs_write()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.fail_abs_once_address = 0x00D00000U;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    const auto result = runtime.start(bridge, *opened.transport, image.image, 5U,
                                      true, flush, clock, pump);
    CHECK(result.ok);
    CHECK(result.input_open);
    const auto count = std::count_if(
        state.word_writes.begin(), state.word_writes.end(),
        [](const std::pair<std::uint32_t, std::uint32_t>& item) {
            return item.first == 0x00D00000U;
        });
    CHECK(count == 4);
    CHECK(pump.running());
}

void test_t7_trigger_is_not_retried()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.fail_trigger_address = 0x00D02010U;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    const auto result = runtime.start(bridge, *opened.transport, image.image, 6U,
                                      true, flush, clock, pump);
    CHECK(!result.ok);
    CHECK(result.error == core0::Error::outcome_unknown);
    CHECK(state.trigger_writes == 1);
    CHECK(!pump.running());
    CHECK(state.active.empty());
}

void test_pump_and_stop_with_fake()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.param0 = image.image.param0;
    auto* raw = new FakeIo(state);
    std::unique_ptr<usb::UsbIo> io(raw);
    auto opened = usb::Transport::open(std::move(io), location());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    PacketSinkState sink;
    core0::TsPump pump(*opened.transport, framer, clock, 16U, 65536U,
                       packet_sink, &sink);
    core0::Runtime runtime;
    Flush flush;
    const auto started = runtime.start(bridge, *opened.transport, image.image, 3U,
                                       true, flush, clock, pump);
    CHECK(started.ok);
    raw->complete_one(192U);
    CHECK(pump.pump() == 0);
    CHECK(framer.stats().packets == 0U);
    raw->complete_one(192U);
    CHECK(pump.pump() == 0);
    CHECK(framer.stats().packets == 1U);
    CHECK(sink.count == 1U);
    const std::uint64_t before_stop = framer.stats().packets;
    const auto stopped = runtime.stop(bridge, pump);
    CHECK(stopped.ok);
    CHECK(!pump.running());
    CHECK(state.active.empty());
    CHECK(framer.stats().packets == before_stop);
    CHECK(sink.count == 1U);
}

void test_run_outcome_unknown()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.run_outcome_unknown = true;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    const auto result = runtime.start(bridge, *opened.transport, image.image, 1U,
                                      true, flush, clock, pump);
    CHECK(!result.ok);
    CHECK(result.error == core0::Error::run_outcome_unknown);
    CHECK(!pump.running());
    CHECK(state.active.empty());
}

void test_epoch_partial_is_discarded()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    auto* raw = new FakeIo(state);
    std::unique_ptr<usb::UsbIo> io(raw);
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    Flush flush;
    CHECK(pump.start(1U) == 0);
    raw->complete_one(100U);
    CHECK(pump.pump() == 0);
    CHECK(pump.stop() == 0);
    CHECK(pump.start(2U) == 0);
    raw->complete_one(92U);
    CHECK(pump.pump() == 0);
    CHECK(framer.stats().packets == 0U);
    CHECK(framer.stats().input_bytes == 92U);
    CHECK(pump.stop() == 0);
}

void test_trigger_failure_is_not_retried()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.fail_trigger_address = 0x00FDBD08U;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    const auto result = runtime.start(bridge, *opened.transport, image.image, 1U,
                                      true, flush, clock, pump);
    CHECK(!result.ok);
    CHECK(result.error == core0::Error::outcome_unknown);
    CHECK(state.trigger_writes == 1);
}

void test_boot_transition_and_fault()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    state.boot_reads = 2;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    auto result = runtime.start(bridge, *opened.transport, image.image, 1U,
                                true, flush, clock, pump);
    CHECK(result.ok);
    CHECK(state.stat_reads >= 2);
    CHECK(pump.stop() == 0);

    FakeState fault;
    ImageFixture fault_image = make_image();
    fault.image_bytes = static_cast<std::uint32_t>(fault_image.image.image_bytes);
    fault.build_id = fault_image.image.build_id;
    fault.fault = true;
    std::unique_ptr<usb::UsbIo> fault_io(new FakeIo(fault));
    auto fault_opened = usb::Transport::open(std::move(fault_io), location());
    widen::Bridge fault_bridge(*fault_opened.transport);
    core0::Framer fault_framer;
    TestClock fault_clock;
    core0::TsPump fault_pump(*fault_opened.transport, fault_framer, fault_clock);
    core0::Runtime fault_runtime;
    Flush fault_flush;
    result = fault_runtime.start(fault_bridge, *fault_opened.transport,
                                 fault_image.image, 1U, true, fault_flush,
                                 fault_clock, fault_pump);
    CHECK(!result.ok);
    CHECK(result.error == core0::Error::state_mismatch);
}

void test_stop_timeout_keeps_resources_then_recovers()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    auto* raw = new FakeIo(state);
    std::unique_ptr<usb::UsbIo> io(raw);
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    CHECK(runtime.start(bridge, *opened.transport, image.image, 1U, true,
                        flush, clock, pump).ok);
    state.hold_callbacks = true;
    CHECK(pump.stop() == -1);
    CHECK(pump.running());
    CHECK(!state.active.empty());
    state.hold_callbacks = false;
    raw->release_callbacks();
    CHECK(pump.stop() == 0);
    CHECK(!pump.running());
    CHECK(state.active.empty());
}

void test_stop_poll_error_keeps_resources_then_recovers()
{
    FakeState state;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    CHECK(pump.start(1U) == 0);
    state.poll_error = true;
    CHECK(pump.stop() == -1);
    CHECK(pump.running());
    CHECK(!state.events.empty());
    state.poll_error = false;
    CHECK(pump.stop() == 0);
    CHECK(!pump.running());
    CHECK(state.active.empty());
}

void test_runtime_stop_cleans_bulk_after_ep0_failure()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    CHECK(runtime.start(bridge, *opened.transport, image.image, 1U, true,
                        flush, clock, pump).ok);
    state.hold_fail = true;
    const auto stopped = runtime.stop(bridge, pump);
    CHECK(!stopped.ok);
    CHECK(!pump.running());
    CHECK(state.active.empty());
}

void test_runtime_preconditions_and_flush()
{
    FakeState state;
    ImageFixture image = make_image();
    state.image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.build_id = image.image.build_id;
    std::unique_ptr<usb::UsbIo> io(new FakeIo(state));
    auto opened = usb::Transport::open(std::move(io), location());
    widen::Bridge bridge(*opened.transport);
    core0::Framer framer;
    TestClock clock;
    core0::TsPump pump(*opened.transport, framer, clock);
    core0::Runtime runtime;
    Flush flush;
    auto result = runtime.start(bridge, *opened.transport, image.image, 1U,
                                false, flush, clock, pump);
    CHECK(!result.ok);
    CHECK(result.error == core0::Error::widen_not_verified);
    flush.ok = false;
    result = runtime.start(bridge, *opened.transport, image.image, 1U,
                           true, flush, clock, pump);
    CHECK(!result.ok);
    CHECK(result.error == core0::Error::i2c_flush_failed);
}

}  // namespace

int main()
{
    test_framer_cross_boundary_lookahead_and_cc();
    test_framer_null_pid_cc_is_ignored();
    test_runtime_start_pump_stop();
    test_runtime_held_start_keeps_input_closed_5s();
    test_full_t2_added_mmio_failure_stops_before_bulk();
    test_full_t2_added_mmio_is_abs_write();
    test_t7_trigger_is_not_retried();
    test_pump_and_stop_with_fake();
    test_run_outcome_unknown();
    test_epoch_partial_is_discarded();
    test_trigger_failure_is_not_retried();
    test_boot_transition_and_fault();
    test_stop_timeout_keeps_resources_then_recovers();
    test_stop_poll_error_keeps_resources_then_recovers();
    test_runtime_stop_cleans_bulk_after_ep0_failure();
    test_runtime_preconditions_and_flush();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
