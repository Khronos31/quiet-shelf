// SPDX-License-Identifier: GPL-2.0-only
#include "card/uart.h"
#include "usb/transport.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace usb = torne::usb;
namespace card = torne::card;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

constexpr std::uint32_t card_enable = 0x00FEA000U;
constexpr std::uint32_t card_lines = 0x00FEA004U;
constexpr std::uint32_t card_baud = 0x00FEA008U;
constexpr std::uint32_t card_send = 0x00FEA00CU;
constexpr std::uint32_t card_control = 0x00FEA010U;
constexpr std::uint32_t card_fifo = 0x00FEA014U;
constexpr std::uint32_t card_irq_mask = 0x00FEA018U;
constexpr std::uint32_t card_irq = 0x00FEA01CU;
constexpr std::uint32_t card_cwt = 0x00FEA020U;
constexpr std::uint32_t card_bwt = 0x00FEA024U;
constexpr std::uint32_t card_power = 0x00FEA034U;
constexpr std::uint32_t card_recv_lrc = 0x00FEA03CU;

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

usb::TransferOutcome complete(std::size_t transferred = 4U)
{
    return usb::TransferOutcome{usb::TransferState::complete,
                                usb::TransferCause::none, transferred, false};
}

usb::TransferOutcome failed(usb::TransferCause cause)
{
    return usb::TransferOutcome{usb::TransferState::failed, cause, 0U, false};
}

std::uint32_t decode_address(std::uint16_t value, std::uint16_t index)
{
    return static_cast<std::uint32_t>(index) |
           (static_cast<std::uint32_t>(value & 0xFF00U) << 8U) |
           (static_cast<std::uint32_t>(value & 0x00FFU) << 24U);
}

struct Scripted final {
    usb::TransferOutcome outcome{};
    std::uint32_t value = 0U;
};

struct FakeIo final : usb::UsbIo {
    std::vector<std::string>& events;
    std::map<std::uint32_t, std::uint32_t> regs;
    std::deque<Scripted> in_script;
    std::deque<Scripted> out_script;
    int control_in_calls = 0;
    int control_out_calls = 0;
    std::uint8_t last_request_type = 0U;
    std::uint8_t last_request = 0U;
    std::uint16_t last_value = 0U;
    std::uint16_t last_index = 0U;
    std::uint16_t last_length = 0U;

    explicit FakeIo(std::vector<std::string>& log) noexcept : events(log) {}
    ~FakeIo() noexcept override = default;

    usb::OpenOutcome open(const usb::DeviceLocation&,
                          usb::DeviceSummary& summary) noexcept override
    {
        summary = valid_summary();
        return usb::OpenOutcome{usb::OpenState::ok, usb::TransferCause::none};
    }
    usb::OpenOutcome claim_interface(std::uint8_t) noexcept override
    {
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
        ++control_in_calls;
        last_request_type = request_type;
        last_request = request;
        last_value = value;
        last_index = index;
        last_length = length;
        const std::uint32_t address = decode_address(value, index);
        events.push_back("in:" + hex(address));
        usb::TransferOutcome outcome = complete(length);
        std::uint32_t result_value = regs[address];
        if (!in_script.empty()) {
            const Scripted script = in_script.front();
            in_script.pop_front();
            outcome = script.outcome;
            result_value = script.value;
        }
        if (outcome.state == usb::TransferState::complete && data != nullptr &&
            length >= 4U) {
            data[0] = static_cast<std::uint8_t>(result_value >> 24U);
            data[1] = static_cast<std::uint8_t>(result_value >> 16U);
            data[2] = static_cast<std::uint8_t>(result_value >> 8U);
            data[3] = static_cast<std::uint8_t>(result_value);
        }
        return outcome;
    }
    usb::TransferOutcome control_out(std::uint8_t request_type,
                                     std::uint8_t request,
                                     std::uint16_t value,
                                     std::uint16_t index,
                                     std::uint16_t length,
                                     const std::uint8_t* data,
                                     std::uint32_t) noexcept override
    {
        ++control_out_calls;
        last_request_type = request_type;
        last_request = request;
        last_value = value;
        last_index = index;
        last_length = length;
        const std::uint32_t address = decode_address(value, index);
        std::uint32_t word = 0U;
        if (data != nullptr && length >= 4U) {
            word = (static_cast<std::uint32_t>(data[0]) << 24U) |
                   (static_cast<std::uint32_t>(data[1]) << 16U) |
                   (static_cast<std::uint32_t>(data[2]) << 8U) |
                   static_cast<std::uint32_t>(data[3]);
        }
        events.push_back("out:" + hex(address) + "=" + hex(word));
        if (!out_script.empty()) {
            const Scripted script = out_script.front();
            out_script.pop_front();
            if (script.outcome.state == usb::TransferState::complete)
                regs[address] = word;
            return script.outcome;
        }
        regs[address] = word;
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
    void release_interface(std::uint8_t) noexcept override {}
    void close() noexcept override {}

    static std::string hex(std::uint32_t value)
    {
        char buffer[16] = {};
        std::snprintf(buffer, sizeof(buffer), "%06x", value);
        return std::string(buffer);
    }
};

struct FakeClock final : card::Clock {
    std::vector<std::string>& events;
    std::vector<std::uint32_t> sleeps;
    std::uint64_t now = 0U;

    explicit FakeClock(std::vector<std::string>& log) noexcept : events(log) {}
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        now += milliseconds;
        sleeps.push_back(milliseconds);
        events.push_back("sleep:" + std::to_string(milliseconds));
    }
};

struct Harness final {
    std::vector<std::string> events;
    FakeIo* fake = nullptr;
    FakeClock clock{events};
    std::unique_ptr<usb::Transport> transport;
    std::unique_ptr<card::Uart> uart;

    Harness()
    {
        auto io = std::make_unique<FakeIo>(events);
        fake = io.get();
        usb::OpenResult opened =
            usb::Transport::open(std::move(io), location_1_2());
        transport = std::move(opened.transport);
        if (transport)
            uart = std::make_unique<card::Uart>(*transport, clock);
    }
};

int index_of(const std::vector<std::string>& events, const std::string& event,
             int from = 0)
{
    for (std::size_t i = static_cast<std::size_t>(from); i < events.size(); ++i) {
        if (events[i] == event)
            return static_cast<int>(i);
    }
    return -1;
}

int out_index(const std::vector<std::string>& events, std::uint32_t address,
              std::uint32_t value, int from = 0)
{
    const std::string event = "out:" + FakeIo::hex(address) + "=" +
                              FakeIo::hex(value);
    return index_of(events, event, from);
}

void test_presence_and_count()
{
    Harness h;
    CHECK(h.uart != nullptr);
    h.fake->regs[card_irq] = 0x00000400U;  // bit 10: no card
    const card::PresenceResult absent = h.uart->detect_card();
    CHECK(absent.result.ok);
    CHECK(!absent.present);
    CHECK(absent.result.step == card::Step::presence);
    CHECK(h.fake->last_request_type == 0xC0U);
    CHECK(h.fake->last_request == 0xBCU);
    CHECK(h.fake->last_value == 0xFE00U);
    CHECK(h.fake->last_index == 0xA01CU);
    CHECK(h.fake->last_length == 4U);
    CHECK(index_of(h.events, "in:fea01c") == 0);

    h.fake->regs[card_irq] = 0U;
    const card::PresenceResult present = h.uart->detect_card();
    CHECK(present.result.ok);
    CHECK(present.present);

    h.fake->regs[card_fifo] = 0x00030000U;
    const card::CountResult ready = h.uart->data_ready();
    CHECK(ready.result.ok);
    CHECK(ready.count == 3U);
    CHECK(ready.result.step == card::Step::fifo_count);
    std::printf("presence/count ok\n");
}

void test_read_data()
{
    Harness h;
    h.fake->in_script.push_back({complete(), 0x00030000U});  // FIFO count
    h.fake->in_script.push_back({complete(), 0x00000011U});
    h.fake->in_script.push_back({complete(), 0x00000022U});
    h.fake->in_script.push_back({complete(), 0x00000033U});
    std::uint8_t buffer[8] = {};
    const card::ReadResult read = h.uart->read_data(buffer, sizeof(buffer));
    CHECK(read.result.ok);
    CHECK(read.bytes_read == 3U);
    CHECK(buffer[0] == 0x11U && buffer[1] == 0x22U && buffer[2] == 0x33U);
    CHECK(h.fake->control_in_calls == 4);
    CHECK(index_of(h.events, "in:fea014") == 0);
    CHECK(index_of(h.events, "in:fea02c", 1) == 1);

    Harness h2;
    h2.fake->in_script.push_back({complete(), 0x00010000U});  // one byte
    h2.fake->in_script.push_back({failed(usb::TransferCause::timeout), 0U});
    std::uint8_t one = 0U;
    const card::ReadResult unknown = h2.uart->read_data(&one, 1U);
    CHECK(!unknown.result.ok);
    CHECK(unknown.result.error == card::Error::outcome_unknown);
    CHECK(unknown.result.step == card::Step::read_rx);
    CHECK(h2.fake->control_in_calls == 2);  // CONSUMING_READ is not retried
    CHECK(!h2.uart->valid());
    const int before = h2.fake->control_in_calls;
    const card::CountResult blocked = h2.uart->data_ready();
    CHECK(!blocked.result.ok);
    CHECK(blocked.result.error == card::Error::session_invalid);
    CHECK(h2.fake->control_in_calls == before);

    Harness h3;
    h3.fake->in_script.push_back({complete(), 0x00010000U});
    h3.fake->in_script.push_back(
        {usb::TransferOutcome{usb::TransferState::short_transfer,
                              usb::TransferCause::none, 1U, false}, 0U});
    std::uint8_t short_byte = 0U;
    const card::ReadResult short_read = h3.uart->read_data(&short_byte, 1U);
    CHECK(!short_read.result.ok);
    CHECK(short_read.result.error == card::Error::outcome_unknown);
    CHECK(!h3.uart->valid());
    std::printf("read_data ok\n");
}

void test_reset_order()
{
    Harness h;
    h.fake->regs[card_irq] = 0U;
    h.fake->regs[card_lines] = 0x00000001U;
    h.fake->regs[card_enable] = 0x00000020U;
    const card::Result reset = h.uart->reset_card();
    CHECK(reset.ok);
    CHECK(reset.step == card::Step::reset_lines_release);
    CHECK(h.clock.now == 13U);
    CHECK(h.clock.sleeps == (std::vector<std::uint32_t>{10U, 2U, 1U}));
    CHECK(h.fake->regs[card_enable] == 7U);
    CHECK((h.fake->regs[card_lines] & 0x0000000EU) == 0x0000000EU);
    CHECK(h.fake->regs[card_baud] == 0x0000BA7FU);
    CHECK(h.fake->regs[card_cwt] == 9600U);
    CHECK(h.fake->regs[card_bwt] == 0U);
    CHECK(h.fake->regs[card_irq_mask] == 0x000001FFU);
    CHECK(h.fake->regs[card_power] == 1U);
    CHECK(h.fake->regs[card_recv_lrc] == 0x0000003BU);
    CHECK(h.fake->regs[card_fifo] == 0U);
    CHECK(h.fake->regs[card_irq] == 0x000003FFU);

    int cursor = index_of(h.events, "in:fea01c");
    CHECK(cursor == 0);
    const int power_down = out_index(h.events, card_power, 0U, cursor);
    const int sleep10 = index_of(h.events, "sleep:10", cursor);
    const int irq_clear = out_index(h.events, card_irq, 0x3FFU, cursor);
    const int power_up = out_index(h.events, card_power, 1U, cursor);
    const int sleep2 = index_of(h.events, "sleep:2", cursor);
    const int sleep1 = index_of(h.events, "sleep:1", cursor);
    CHECK(power_down > cursor);
    CHECK(sleep10 > power_down);
    CHECK(irq_clear > sleep10);
    CHECK(power_up > irq_clear);
    CHECK(sleep2 > power_up);
    CHECK(sleep1 > sleep2);
    CHECK(h.fake->regs[card_lines] == 0x0000000FU);
    CHECK(h.events.back() == "out:fea004=00000f");
    std::printf("reset order ok\n");
}

void test_no_card_reset()
{
    Harness h;
    h.fake->regs[card_irq] = 0x00000400U;
    const card::Result reset = h.uart->reset_card();
    CHECK(!reset.ok);
    CHECK(reset.error == card::Error::no_card);
    CHECK(reset.step == card::Step::presence);
    CHECK(h.fake->control_out_calls == 0);
    CHECK(h.clock.sleeps.empty());
    std::printf("no-card reset ok\n");
}

void test_write_data()
{
    Harness h;
    const std::uint8_t block[] = {0x01U, 0x02U};
    const card::Result write = h.uart->write_data(block, sizeof(block));
    CHECK(write.ok);
    CHECK(write.step == card::Step::send);
    const std::vector<std::string> expected{
        "out:fea03c=000000", "out:fea038=000000",
        "out:fea028=000001", "out:fea028=000002",
        "out:fea01c=0003ff", "out:fea010=000200",
        "out:fea00c=000001"};
    CHECK(h.events == expected);

    Harness h2;
    h2.fake->out_script.push_back({complete(), 0U});
    h2.fake->out_script.push_back({complete(), 0U});
    h2.fake->out_script.push_back({failed(usb::TransferCause::timeout), 0U});
    const card::Result tx_unknown = h2.uart->write_data(block, sizeof(block));
    CHECK(!tx_unknown.ok);
    CHECK(tx_unknown.error == card::Error::outcome_unknown);
    CHECK(tx_unknown.step == card::Step::write_tx);
    CHECK(!h2.uart->valid());
    CHECK(out_index(h2.events, card_send, 1U) == -1);
    CHECK(h2.fake->control_out_calls == 3);  // TX FIFO write is not retried

    Harness h3;
    for (int i = 0; i < 6; ++i)
        h3.fake->out_script.push_back({complete(), 0U});
    h3.fake->out_script.push_back({failed(usb::TransferCause::timeout), 0U});
    const card::Result send_unknown = h3.uart->write_data(block, sizeof(block));
    CHECK(!send_unknown.ok);
    CHECK(send_unknown.error == card::Error::outcome_unknown);
    CHECK(send_unknown.step == card::Step::send);
    CHECK(h3.fake->control_out_calls == 7);  // SEND trigger is not retried
    CHECK(!h3.uart->valid());

    Harness h4;
    h4.fake->out_script.push_back({failed(usb::TransferCause::timeout), 0U});
    h4.fake->out_script.push_back({failed(usb::TransferCause::timeout), 0U});
    const card::Result lrc_failed = h4.uart->write_data(block, sizeof(block));
    CHECK(!lrc_failed.ok);
    CHECK(lrc_failed.error == card::Error::io_failed);
    CHECK(lrc_failed.step == card::Step::write_lrc);
    CHECK(h4.fake->control_out_calls == 2);  // ABS_WRITE retries once, then stops
    CHECK(index_of(h4.events, "out:fea028=000001") == -1);
    CHECK(!h4.uart->valid());

    Harness h5;
    std::vector<std::uint8_t> long_block(0x100U, 0x55U);
    CHECK(h5.uart->write_data(long_block.data(), long_block.size()).ok);
    CHECK(h5.fake->regs[card_control] == 0x00000280U);
    std::printf("write_data ok\n");
}

void test_baud()
{
    Harness h;
    CHECK(h.uart->set_baud_rate(19200U).ok);
    CHECK(h.fake->regs[card_baud] == 0x00005D5DU);
    CHECK(h.fake->regs[card_control] == 0x00010000U);
    CHECK(h.fake->regs[card_cwt] == 107U);
    CHECK(h.fake->regs[card_bwt] == 36559U);

    CHECK(h.uart->set_baud_rate(38400U).ok);
    CHECK(h.fake->regs[card_baud] == 0x00002EAEU);
    CHECK(h.fake->regs[card_control] == 0x00010000U);
    CHECK(h.fake->regs[card_cwt] == 107U);
    CHECK(h.fake->regs[card_bwt] == 0x0000FFFFU);

    CHECK(h.uart->set_baud_rate(9600U).ok);
    CHECK(h.fake->regs[card_baud] == 0x0000BA7FU);
    CHECK(h.fake->regs[card_control] == 0U);
    CHECK(h.fake->regs[card_cwt] == 9600U);
    CHECK(h.fake->regs[card_bwt] == 0U);

    const int before = h.fake->control_out_calls;
    const card::Result unsupported = h.uart->set_baud_rate(115200U);
    CHECK(!unsupported.ok);
    CHECK(unsupported.error == card::Error::unsupported_baud);
    CHECK(h.fake->control_out_calls == before);

    Harness retry;
    retry.fake->out_script.push_back({failed(usb::TransferCause::timeout), 0U});
    const card::Result retried = retry.uart->set_baud_rate(9600U);
    CHECK(retried.ok);
    CHECK(retry.fake->control_out_calls == 5);  // one ABS_WRITE retry
    CHECK(retry.fake->regs[card_baud] == 0x0000BA7FU);
    std::printf("baud ok\n");
}

void test_io_failure_and_session()
{
    Harness h;
    h.fake->in_script.push_back({failed(usb::TransferCause::timeout), 0U});
    h.fake->in_script.push_back({failed(usb::TransferCause::timeout), 0U});
    const card::PresenceResult failed_presence = h.uart->detect_card();
    CHECK(!failed_presence.result.ok);
    CHECK(failed_presence.result.error == card::Error::io_failed);
    CHECK(h.fake->control_in_calls == 2);  // PURE_READ retries once
    CHECK(!h.uart->valid());

    const int before = h.fake->control_in_calls;
    const card::PresenceResult blocked = h.uart->detect_card();
    CHECK(!blocked.result.ok);
    CHECK(blocked.result.error == card::Error::session_invalid);
    CHECK(h.fake->control_in_calls == before);

    Harness gone;
    gone.fake->in_script.push_back(
        {failed(usb::TransferCause::no_device), 0U});
    const card::PresenceResult no_device = gone.uart->detect_card();
    CHECK(!no_device.result.ok);
    CHECK(no_device.result.error == card::Error::io_failed);
    CHECK(gone.fake->control_in_calls == 1);  // definite failure is not retried
    CHECK(!gone.uart->valid());

    Harness invalid;
    CHECK(!invalid.uart->write_data(nullptr, 1U).ok);
    CHECK(invalid.fake->control_out_calls == 0);
    CHECK(invalid.uart->valid());  // local argument errors do not poison the line
    std::printf("io/session ok\n");
}

}  // namespace

int main()
{
    test_presence_and_count();
    test_read_data();
    test_reset_order();
    test_no_card_reset();
    test_write_data();
    test_baud();
    test_io_failure_and_session();
    if (failures != 0) {
        std::fprintf(stderr, "%d card UART checks failed\n", failures);
        return 1;
    }
    std::printf("card UART tests: ALL OK\n");
    return 0;
}
