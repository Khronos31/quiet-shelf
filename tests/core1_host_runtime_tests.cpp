// SPDX-License-Identifier: GPL-2.0-only
#include "core1/host_runtime.h"

#include "usb/transport.h"
#include "widen/bridge.h"

#include <dtv/userland/detail/sha256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace {

namespace usb = torne::usb;
namespace core1 = torne::core1;
namespace widen = torne::widen;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

constexpr std::uint32_t kBase = 0x01700000U;
constexpr std::uint32_t kCodeBase = 0x017A0000U;
constexpr std::uint32_t kDataBase = 0x017A3800U;
constexpr std::uint32_t kCtrlBase = 0x017A3840U;
constexpr std::uint32_t kStatBase = 0x017A3880U;
constexpr std::uint32_t kHold = 0xFFFFFFFFU;
constexpr std::uint32_t kRun = 0xFFFCFFFFU;
constexpr std::uint32_t kInfoMagic = 0x54493243U;

struct ImageFixture final {
    std::vector<std::uint8_t> bytes;
    core1::Image image;
};

ImageFixture make_image(std::size_t size = 0x100U)
{
    ImageFixture fixture;
    fixture.bytes.resize(size);
    for (std::size_t index = 0U; index < size; ++index)
        fixture.bytes[index] = static_cast<std::uint8_t>(index * 7U + 3U);
    fixture.image.bytes = fixture.bytes.data();
    fixture.image.image_bytes = size;
    fixture.image.build_id = 0x11223344U;
    fixture.image.entry = 0x000A0000U;
    fixture.image.capabilities = 0x00000001U;
    dtv::userland::detail::Sha256 digest;
    digest.update(fixture.bytes.data(), fixture.bytes.size());
    fixture.image.sha256 = digest.finish();
    return fixture;
}

usb::DeviceSummary valid_summary()
{
    usb::DeviceSummary summary;
    summary.vid = 0x054cU;
    summary.pid = 0x04b2U;
    summary.num_configurations = 1U;
    summary.configuration_value = 1U;
    summary.num_interfaces = 1U;
    summary.interface_zero.number = 0U;
    summary.interface_zero.alternate = 0U;
    summary.interface_zero.interface_class = 0xffU;
    summary.interface_zero.endpoint_count = 4U;
    summary.interface_zero.endpoints[0] = {0x81U, 0x02U, 512U, 0U};
    summary.interface_zero.endpoints[1] = {0x02U, 0x02U, 512U, 0U};
    summary.interface_zero.endpoints[2] = {0x83U, 0x03U, 64U, 1U};
    summary.interface_zero.endpoints[3] = {0x84U, 0x02U, 512U, 0U};
    return summary;
}

usb::DeviceLocation location_1_2()
{
    usb::DeviceLocation location;
    location.bus = 1U;
    location.port_count = 1U;
    location.ports[0] = 2U;
    return location;
}

struct FakeState final {
    usb::DeviceSummary summary = valid_summary();
    std::map<std::uint32_t, std::uint8_t> memory;

    int open_calls = 0;
    int claim_calls = 0;
    int release_calls = 0;
    int close_calls = 0;
    int control_in_calls = 0;
    int control_out_calls = 0;
    int info_reads = 0;
    int stat_reads = 0;
    int code_reads = 0;
    int hold_writes = 0;
    int run_writes = 0;
    int ctrl_writes = 0;
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> writes;
    std::vector<std::uint16_t> read_lengths;
    std::vector<std::uint16_t> write_lengths;

    bool hold_fail_first = false;
    bool hold_no_device = false;
    bool run_outcome_unknown = false;
    bool heartbeat_stalled = false;
    bool nop_ack_enabled = true;
    std::uint32_t nop_result = 0U;
    bool info_never_magic = false;
    int info_magic_delay = 0;
    bool read_fail_info = false;
    bool read_short_info = false;
    bool corrupt_code_readback = false;
    std::uint64_t* clock_now = nullptr;
    std::uint32_t delay_info_ms = 0U;
    std::uint32_t delay_nop_ms = 0U;
    bool session_override = false;
    std::uint32_t session_echo_override = 0U;
    std::uint32_t info_abi = 0x00010000U;
    std::uint32_t info_build_id = 0U;
    std::uint32_t info_capabilities = 1U;
    std::uint32_t info_image_bytes = 0U;
    std::uint32_t info_data_base = 0x000A3800U;

    bool running = false;
    std::uint32_t session = 0U;
    std::uint32_t heartbeat = 0U;
    std::uint32_t cmd_ack = 0U;
    std::uint32_t cmd_result = 0U;
    int magic_pending = 0;
};

std::uint32_t be32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

void put32(std::uint8_t* bytes, std::uint32_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void write_memory(FakeState& state, std::uint32_t address,
                  const std::uint8_t* data, std::size_t length)
{
    for (std::size_t index = 0U; index < length; ++index)
        state.memory[address + static_cast<std::uint32_t>(index)] = data[index];
}

void read_memory(FakeState& state, std::uint32_t address, std::uint8_t* data,
                 std::size_t length)
{
    for (std::size_t index = 0U; index < length; ++index) {
        const auto found = state.memory.find(address + static_cast<std::uint32_t>(index));
        data[index] = found == state.memory.end() ? 0U : found->second;
    }
}

void set_info_memory(FakeState& state)
{
    std::uint8_t block[64] = {};
    put32(block + 0U, state.running && !state.info_never_magic &&
                              state.magic_pending == 0
                          ? kInfoMagic
                          : 0U);
    put32(block + 4U, state.info_abi);
    put32(block + 8U, state.info_build_id);
    put32(block + 12U, state.info_capabilities);
    put32(block + 16U, state.session_override ? state.session_echo_override
                                               : state.session);
    put32(block + 20U, state.info_image_bytes);
    put32(block + 24U, state.info_data_base);
    put32(block + 28U, 0U);
    write_memory(state, kDataBase, block, sizeof(block));
}

void set_stat_memory(FakeState& state)
{
    std::uint8_t block[64] = {};
    put32(block + 4U, state.heartbeat);
    put32(block + 16U, state.cmd_ack);
    put32(block + 20U, state.cmd_result);
    write_memory(state, kStatBase, block, sizeof(block));
}

class FakeUsbIo final : public usb::UsbIo {
public:
    explicit FakeUsbIo(FakeState& state) noexcept : state_(state) {}

    usb::OpenOutcome open(const usb::DeviceLocation&, usb::DeviceSummary& output) noexcept override
    {
        ++state_.open_calls;
        output = state_.summary;
        return usb::OpenOutcome{usb::OpenState::ok, usb::TransferCause::none};
    }

    usb::OpenOutcome claim_interface(std::uint8_t) noexcept override
    {
        ++state_.claim_calls;
        return usb::OpenOutcome{usb::OpenState::ok, usb::TransferCause::none};
    }

    usb::TransferOutcome control_in(std::uint8_t request_type,
                                    std::uint8_t request,
                                    std::uint16_t value,
                                    std::uint16_t index,
                                    std::uint16_t length,
                                    std::uint8_t* data,
                                    std::uint32_t) noexcept override
    {
        ++state_.control_in_calls;
        if (request_type != 0xC0U || request != 0xBCU || data == nullptr)
            return failed(usb::TransferCause::invalid);
        const std::uint32_t address =
            (static_cast<std::uint32_t>(value & 0xFF00U) << 8U) |
            (static_cast<std::uint32_t>(value & 0x00FFU) << 24U) |
            index;
        state_.reads.push_back(address);
        state_.read_lengths.push_back(length);
        if (address == kDataBase) {
            ++state_.info_reads;
            if (state_.read_fail_info)
                return failed(usb::TransferCause::timeout);
            if (state_.read_short_info)
                return usb::TransferOutcome{usb::TransferState::short_transfer,
                                            usb::TransferCause::none, 4U, false};
            if (state_.magic_pending > 0) --state_.magic_pending;
            set_info_memory(state_);
        } else if (address == kStatBase) {
            ++state_.stat_reads;
            if (state_.running && !state_.heartbeat_stalled) ++state_.heartbeat;
            set_stat_memory(state_);
        } else if (address >= kCodeBase && address < kCodeBase + 0x3000U) {
            ++state_.code_reads;
        }
        read_memory(state_, address, data, length);
        if (state_.corrupt_code_readback && address >= kCodeBase &&
            address < kCodeBase + 0x3000U && length != 0U)
            data[0] ^= 0x01U;
        if (state_.clock_now != nullptr) {
            if (address == kDataBase && state_.delay_info_ms != 0U)
                *state_.clock_now += state_.delay_info_ms;
            if (address == kStatBase && state_.cmd_ack != 0U &&
                state_.delay_nop_ms != 0U)
                *state_.clock_now += state_.delay_nop_ms;
        }
        return complete(length);
    }

    usb::TransferOutcome control_out(std::uint8_t request_type,
                                     std::uint8_t request,
                                     std::uint16_t value,
                                     std::uint16_t index,
                                     std::uint16_t length,
                                     const std::uint8_t* data,
                                     std::uint32_t) noexcept override
    {
        ++state_.control_out_calls;
        if (request_type != 0x40U || request != 0xBCU || data == nullptr)
            return failed(usb::TransferCause::invalid);
        const std::uint32_t address =
            (static_cast<std::uint32_t>(value & 0xFF00U) << 8U) |
            (static_cast<std::uint32_t>(value & 0x00FFU) << 24U) |
            index;
        state_.writes.push_back(address);
        state_.write_lengths.push_back(length);
        if (address == kBase && length == 4U) {
            const std::uint32_t word = be32(data);
            if (word == kHold) {
                ++state_.hold_writes;
                if (state_.hold_no_device)
                    return failed(usb::TransferCause::no_device);
                if (state_.hold_fail_first && state_.hold_writes == 1U)
                    return failed(usb::TransferCause::timeout);
                state_.running = false;
            } else if (word == kRun) {
                ++state_.run_writes;
                if (state_.run_outcome_unknown)
                    return usb::TransferOutcome{usb::TransferState::outcome_unknown,
                                                usb::TransferCause::timeout, 0U,
                                                false};
                state_.running = true;
                state_.heartbeat = 0U;
                state_.cmd_ack = 0U;
                state_.cmd_result = 0U;
                state_.magic_pending = state_.info_magic_delay;
                set_info_memory(state_);
                set_stat_memory(state_);
            }
        } else if (address == kCtrlBase && length == 64U) {
            ++state_.ctrl_writes;
            const std::uint32_t session = be32(data);
            const std::uint32_t cmd_seq = be32(data + 4U);
            const std::uint32_t cmd = be32(data + 8U);
            state_.session = session;
            if (cmd_seq != 0U && cmd == 0U && state_.nop_ack_enabled) {
                state_.cmd_ack = cmd_seq;
                state_.cmd_result = state_.nop_result;
            }
            set_stat_memory(state_);
        }
        write_memory(state_, address, data, length);
        return complete(length);
    }

    usb::TransferOutcome bulk_in(std::uint8_t, std::uint8_t*, std::size_t,
                                 std::uint32_t) noexcept override
    {
        return failed(usb::TransferCause::invalid);
    }

    usb::TransferOutcome bulk_out(std::uint8_t, const std::uint8_t*,
                                  std::size_t, std::uint32_t) noexcept override
    {
        return failed(usb::TransferCause::invalid);
    }

    void release_interface(std::uint8_t) noexcept override { ++state_.release_calls; }
    void close() noexcept override { ++state_.close_calls; }

private:
    static usb::TransferOutcome complete(std::size_t length) noexcept
    {
        return usb::TransferOutcome{usb::TransferState::complete,
                                    usb::TransferCause::none, length, false};
    }
    static usb::TransferOutcome failed(usb::TransferCause cause) noexcept
    {
        return usb::TransferOutcome{usb::TransferState::failed, cause, 0U, false};
    }

    FakeState& state_;
};

class TestClock final : public core1::Clock {
public:
    std::uint64_t monotonic_ms() noexcept override { return now_; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        now_ += milliseconds;
    }
    std::uint64_t now_ = 0U;
};

void configure_info(FakeState& state, const ImageFixture& image)
{
    state.info_build_id = image.image.build_id;
    state.info_image_bytes = static_cast<std::uint32_t>(image.image.image_bytes);
    state.info_capabilities = image.image.capabilities;
}

core1::Result start_once(FakeState& state, const ImageFixture& image,
                         bool widen_verified = true,
                         std::uint32_t previous_session = 0U,
                         core1::Runtime* runtime = nullptr,
                         TestClock* supplied_clock = nullptr)
{
    std::unique_ptr<usb::UsbIo> io(new FakeUsbIo(state));
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    CHECK(opened.transport != nullptr);
    if (!opened.transport) {
        core1::Result result;
        result.error = core1::Error::device_gone;
        return result;
    }
    widen::Bridge bridge(*opened.transport);
    TestClock local_clock;
    TestClock& clock = supplied_clock != nullptr ? *supplied_clock : local_clock;
    state.clock_now = &clock.now_;
    core1::Runtime local(previous_session);
    core1::Runtime& selected = runtime != nullptr ? *runtime : local;
    return selected.start(bridge, image.image, widen_verified, clock);
}

void test_happy_path_and_session()
{
    FakeState state;
    ImageFixture image = make_image();
    configure_info(state, image);
    core1::Runtime runtime;
    const auto first = start_once(state, image, true, 0U, &runtime);
    CHECK(first.ok);
    CHECK(first.stage == core1::Stage::done);
    CHECK(first.session == 0x00010000U);
    CHECK((first.session >> 16U) != 0U);
    CHECK(state.hold_writes == 1);
    CHECK(state.run_writes == 1);
    CHECK(state.ctrl_writes >= 2);
    CHECK(state.code_reads > 0);
    CHECK(state.info_reads > 0);
    CHECK(state.writes.size() >= 40U);
    CHECK(state.writes[0] == kBase);
    CHECK(state.write_lengths[0] == 4U);
    CHECK(state.writes[1] == kBase + 0x000FFFFCU);
    CHECK(state.write_lengths[1] == 4U);
    CHECK(state.writes[2] == kDataBase);
    CHECK(state.write_lengths[2] == 64U);
    CHECK(state.writes[34] == kCtrlBase);
    CHECK(state.write_lengths[34] == 64U);
    CHECK(state.writes[35] == kCodeBase);
    CHECK(state.writes[39] == kBase);
    CHECK(state.write_lengths[39] == 4U);

    const auto second = start_once(state, image, true, runtime.last_session(), &runtime);
    CHECK(second.ok);
    CHECK(second.session != first.session);
    CHECK((second.session >> 16U) != 0U);

    std::unique_ptr<usb::UsbIo> io(new FakeUsbIo(state));
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    widen::Bridge bridge(*opened.transport);
    const auto stopped = runtime.stop(bridge);
    CHECK(stopped.ok);
    CHECK(state.hold_writes >= 3);
}

void test_s2_not_verified_and_invalid_image_do_no_io()
{
    FakeState state;
    ImageFixture image = make_image();
    configure_info(state, image);
    auto result = start_once(state, image, false);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::widen_not_verified);
    CHECK(state.control_in_calls == 0);
    CHECK(state.control_out_calls == 0);

    FakeState invalid_state;
    ImageFixture invalid = make_image();
    invalid.image.sha256[0] ^= 0x01U;
    result = start_once(invalid_state, invalid, true);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::invalid_argument);
    CHECK(invalid_state.control_in_calls == 0);
    CHECK(invalid_state.control_out_calls == 0);
}

void test_info_magic_last()
{
    FakeState state;
    ImageFixture image = make_image();
    configure_info(state, image);
    state.info_magic_delay = 3;
    const auto result = start_once(state, image);
    CHECK(result.ok);
    CHECK(state.info_reads >= 3);
}

void test_info_mismatches()
{
    struct Case final {
        const char* name;
        void (*mutate)(FakeState&, const ImageFixture&);
        core1::Error expected;
    };
    const Case cases[] = {
        {"abi", [](FakeState& s, const ImageFixture&) { s.info_abi = 0x00020000U; },
         core1::Error::abi_mismatch},
        {"build", [](FakeState& s, const ImageFixture&) { s.info_build_id ^= 1U; },
         core1::Error::build_id_mismatch},
        {"session", [](FakeState& s, const ImageFixture&) {
             s.session_override = true;
             s.session_echo_override = 0xDEAD0001U;
         },
         core1::Error::session_mismatch},
        {"image", [](FakeState& s, const ImageFixture&) { s.info_image_bytes += 4U; },
         core1::Error::image_mismatch},
        {"data", [](FakeState& s, const ImageFixture&) { s.info_data_base = 0x000A3804U; },
         core1::Error::data_base_mismatch},
        {"cap", [](FakeState& s, const ImageFixture&) { s.info_capabilities = 0U; },
         core1::Error::capability_mismatch},
    };
    for (const Case& test : cases) {
        FakeState state;
        ImageFixture image = make_image();
        configure_info(state, image);
        test.mutate(state, image);
        const auto result = start_once(state, image);
        CHECK(!result.ok);
        if (result.error != test.expected) {
            std::fprintf(stderr, "info mismatch %s: got %d expected %d\n",
                         test.name, static_cast<int>(result.error),
                         static_cast<int>(test.expected));
        }
        CHECK(result.error == test.expected);
    }
}

void test_heartbeat_and_nop_failures()
{
    FakeState heartbeat_state;
    ImageFixture heartbeat_image = make_image();
    configure_info(heartbeat_state, heartbeat_image);
    heartbeat_state.heartbeat_stalled = true;
    auto result = start_once(heartbeat_state, heartbeat_image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::heartbeat_stalled);

    FakeState nop_state;
    ImageFixture nop_image = make_image();
    configure_info(nop_state, nop_image);
    nop_state.nop_ack_enabled = false;
    result = start_once(nop_state, nop_image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::nop_timeout);

    FakeState nop_fail_state;
    ImageFixture nop_fail_image = make_image();
    configure_info(nop_fail_state, nop_fail_image);
    nop_fail_state.nop_result = 1U;
    result = start_once(nop_fail_state, nop_fail_image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::nop_failed);
}

void test_run_outcome_unknown_is_not_retried()
{
    FakeState state;
    ImageFixture image = make_image();
    configure_info(state, image);
    state.run_outcome_unknown = true;
    const auto result = start_once(state, image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::run_outcome_unknown);
    CHECK(state.run_writes == 1);
    CHECK(state.info_reads == 0);
}

void test_hold_abs_write_retry_and_no_device()
{
    FakeState retry_state;
    ImageFixture retry_image = make_image();
    configure_info(retry_state, retry_image);
    retry_state.hold_fail_first = true;
    auto result = start_once(retry_state, retry_image);
    CHECK(result.ok);
    CHECK(retry_state.hold_writes == 2);

    FakeState gone_state;
    ImageFixture gone_image = make_image();
    configure_info(gone_state, gone_image);
    gone_state.hold_no_device = true;
    result = start_once(gone_state, gone_image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::device_gone);
    CHECK(gone_state.hold_writes == 1);
}

void test_read_failure_and_code_readback_mismatch()
{
    FakeState read_state;
    ImageFixture read_image = make_image();
    configure_info(read_state, read_image);
    read_state.read_fail_info = true;
    auto result = start_once(read_state, read_image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::read_failed);
    CHECK(read_state.info_reads == 2);

    FakeState code_state;
    ImageFixture code_image = make_image();
    configure_info(code_state, code_image);
    code_state.corrupt_code_readback = true;
    result = start_once(code_state, code_image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::readback_mismatch);
    CHECK(code_state.run_writes == 0);
}

void test_short_info_read_is_a_read_failure()
{
    FakeState state;
    ImageFixture image = make_image();
    configure_info(state, image);
    state.read_short_info = true;
    const auto result = start_once(state, image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::read_failed);
    CHECK(state.info_reads == 2);
}

void test_restart_uses_a_new_session()
{
    FakeState state;
    ImageFixture image = make_image();
    configure_info(state, image);
    state.nop_ack_enabled = false;
    core1::Runtime runtime;
    auto result = start_once(state, image, true, 0U, &runtime);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::nop_timeout);
    const std::uint32_t failed_session = runtime.last_session();
    state.nop_ack_enabled = true;
    result = start_once(state, image, true, runtime.last_session(), &runtime);
    CHECK(result.ok);
    CHECK(result.session != failed_session);
    CHECK((result.session >> 16U) != 0U);
}

void test_delayed_info_and_nop_reads_are_timeouts()
{
    FakeState info_state;
    ImageFixture info_image = make_image();
    configure_info(info_state, info_image);
    info_state.delay_info_ms = 200U;
    TestClock info_clock;
    auto result = start_once(info_state, info_image, true, 0U, nullptr,
                             &info_clock);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::info_timeout);

    FakeState nop_state;
    ImageFixture nop_image = make_image();
    configure_info(nop_state, nop_image);
    nop_state.delay_nop_ms = 200U;
    TestClock nop_clock;
    result = start_once(nop_state, nop_image, true, 0U, nullptr, &nop_clock);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::nop_timeout);
}

void test_info_timeout()
{
    FakeState state;
    ImageFixture image = make_image();
    configure_info(state, image);
    state.info_never_magic = true;
    const auto result = start_once(state, image);
    CHECK(!result.ok);
    CHECK(result.error == core1::Error::info_timeout);
    CHECK(state.info_reads > 0);
}

}  // namespace

int main()
{
    test_happy_path_and_session();
    test_s2_not_verified_and_invalid_image_do_no_io();
    test_info_magic_last();
    test_info_mismatches();
    test_heartbeat_and_nop_failures();
    test_run_outcome_unknown_is_not_retried();
    test_hold_abs_write_retry_and_no_device();
    test_read_failure_and_code_readback_mismatch();
    test_short_info_read_is_a_read_failure();
    test_restart_uses_a_new_session();
    test_delayed_info_and_nop_reads_are_timeouts();
    test_info_timeout();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
