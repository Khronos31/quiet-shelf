// SPDX-License-Identifier: GPL-2.0-only
#include "usb/libusb_io.h"
#include "usb/transport.h"

#include <libusb.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

namespace usb = torne::usb;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

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

usb::TransferOutcome outcome(usb::TransferState state, usb::TransferCause cause,
                             std::size_t transferred = 0U)
{
    return usb::TransferOutcome{state, cause, transferred, false};
}

usb::DeviceLocation location_1_2()
{
    usb::DeviceLocation location;
    location.bus = 1U;
    location.port_count = 1U;
    location.ports[0] = 2U;
    return location;
}

struct Stats final {
    int open_calls = 0;
    int open_fd_calls = 0;
    int last_fd = -1;
    int claim_calls = 0;
    int release_calls = 0;
    int close_calls = 0;
    int control_in_calls = 0;
    int control_out_calls = 0;
    int bulk_in_calls = 0;
    int bulk_out_calls = 0;
    std::uint8_t last_request_type = 0U;
    std::uint8_t last_request = 0U;
    std::uint16_t last_value = 0U;
    std::uint16_t last_index = 0U;
    std::uint16_t last_length = 0U;
    std::uint8_t last_endpoint = 0U;
    usb::DeviceLocation opened_at{};
};

class FakeIo final : public usb::UsbIo {
public:
    explicit FakeIo(Stats& stats) noexcept : stats_(stats) {}
    ~FakeIo() noexcept override { close(); }

    usb::DeviceSummary summary = valid_summary();
    usb::OpenOutcome open_result{usb::OpenState::ok, usb::TransferCause::none};
    usb::OpenOutcome open_fd_result{usb::OpenState::ok, usb::TransferCause::none};
    usb::OpenOutcome claim_result{usb::OpenState::ok, usb::TransferCause::none};
    std::vector<usb::TransferOutcome> control_in_results;
    std::vector<usb::TransferOutcome> control_out_results;
    std::vector<usb::TransferOutcome> bulk_in_results;
    std::vector<usb::TransferOutcome> bulk_out_results;

    usb::OpenOutcome open(const usb::DeviceLocation& location,
                          usb::DeviceSummary& output) noexcept override
    {
        ++stats_.open_calls;
        stats_.opened_at = location;
        output = summary;
        return open_result;
    }

    usb::OpenOutcome open_fd(int fd, usb::DeviceSummary& output) noexcept override
    {
        ++stats_.open_fd_calls;
        stats_.last_fd = fd;
        output = summary;
        return open_fd_result;
    }

    usb::OpenOutcome claim_interface(std::uint8_t) noexcept override
    {
        ++stats_.claim_calls;
        return claim_result;
    }

    usb::TransferOutcome control_in(std::uint8_t request_type,
                                    std::uint8_t request,
                                    std::uint16_t value,
                                    std::uint16_t index,
                                    std::uint16_t length,
                                    std::uint8_t* data,
                                    std::uint32_t) noexcept override
    {
        ++stats_.control_in_calls;
        stats_.last_request_type = request_type;
        stats_.last_request = request;
        stats_.last_value = value;
        stats_.last_index = index;
        stats_.last_length = length;
        if (data != nullptr && length != 0U) data[0] = 0x5aU;
        return next(control_in_results, stats_.control_in_calls);
    }

    usb::TransferOutcome control_out(std::uint8_t request_type,
                                     std::uint8_t request,
                                     std::uint16_t value,
                                     std::uint16_t index,
                                     std::uint16_t length,
                                     const std::uint8_t*,
                                     std::uint32_t) noexcept override
    {
        ++stats_.control_out_calls;
        stats_.last_request_type = request_type;
        stats_.last_request = request;
        stats_.last_value = value;
        stats_.last_index = index;
        stats_.last_length = length;
        return next(control_out_results, stats_.control_out_calls);
    }

    usb::TransferOutcome bulk_in(std::uint8_t endpoint,
                                 std::uint8_t* data,
                                 std::size_t length,
                                 std::uint32_t) noexcept override
    {
        ++stats_.bulk_in_calls;
        stats_.last_endpoint = endpoint;
        if (data != nullptr && length != 0U) data[0] = 0x47U;
        return next(bulk_in_results, stats_.bulk_in_calls);
    }

    usb::TransferOutcome bulk_out(std::uint8_t endpoint,
                                  const std::uint8_t*,
                                  std::size_t,
                                  std::uint32_t) noexcept override
    {
        ++stats_.bulk_out_calls;
        stats_.last_endpoint = endpoint;
        return next(bulk_out_results, stats_.bulk_out_calls);
    }

    void release_interface(std::uint8_t) noexcept override { ++stats_.release_calls; }
    void close() noexcept override
    {
        if (closed_) return;
        closed_ = true;
        ++stats_.close_calls;
    }

private:
    static usb::TransferOutcome next(std::vector<usb::TransferOutcome>& results,
                                     int call)
    {
        if (static_cast<std::size_t>(call) > results.size())
            return outcome(usb::TransferState::failed, usb::TransferCause::invalid);
        return results[static_cast<std::size_t>(call - 1)];
    }

    Stats& stats_;
    bool closed_ = false;
};

std::unique_ptr<usb::UsbIo> make_io(Stats& stats)
{
    return std::unique_ptr<usb::UsbIo>(new FakeIo(stats));
}

void test_open_and_close()
{
    Stats stats;
    auto opened = usb::Transport::open(make_io(stats), location_1_2());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    CHECK(opened.transport != nullptr);
    CHECK(opened.transport->claimed());
    CHECK(stats.open_calls == 1);
    CHECK(stats.claim_calls == 1);
    CHECK(stats.opened_at.bus == 1U);
    CHECK(stats.opened_at.ports[0] == 2U);
    opened.transport.reset();
    CHECK(stats.release_calls == 1);
    CHECK(stats.close_calls == 1);
}

void test_open_fd_and_close()
{
    Stats stats;
    auto opened = usb::Transport::open_fd(make_io(stats), 7);
    CHECK(opened.outcome.state == usb::OpenState::ok);
    CHECK(opened.transport != nullptr);
    CHECK(opened.transport->claimed());
    CHECK(stats.open_fd_calls == 1);
    CHECK(stats.last_fd == 7);
    CHECK(stats.claim_calls == 1);
    opened.transport.reset();
    CHECK(stats.release_calls == 1);
    CHECK(stats.close_calls == 1);
}

void test_open_fd_failure_closes_io()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->open_fd_result = usb::OpenOutcome{usb::OpenState::busy,
                                          usb::TransferCause::busy};
    auto opened = usb::Transport::open_fd(std::move(io), 9);
    CHECK(opened.outcome.state == usb::OpenState::busy);
    CHECK(opened.transport == nullptr);
    CHECK(stats.open_fd_calls == 1);
    CHECK(stats.last_fd == 9);
    CHECK(stats.claim_calls == 0);
    CHECK(stats.close_calls == 1);
}

void test_open_fd_descriptor_mismatch_does_not_claim()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->summary.interface_zero.endpoints[0].max_packet = 64U;
    auto opened = usb::Transport::open_fd(std::move(io), 9);
    CHECK(opened.outcome.state == usb::OpenState::unsupported);
    CHECK(opened.transport == nullptr);
    CHECK(stats.claim_calls == 0);
    CHECK(stats.close_calls == 1);
}

void test_open_fd_invalid_argument_does_not_open()
{
    Stats stats;
    auto opened = usb::Transport::open_fd(make_io(stats), -1);
    CHECK(opened.outcome.state == usb::OpenState::invalid);
    CHECK(opened.transport == nullptr);
    CHECK(stats.open_fd_calls == 0);
}

void test_close_is_idempotent()
{
    Stats stats;
    auto opened = usb::Transport::open(make_io(stats), location_1_2());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    opened.transport->close();
    opened.transport->close();
    CHECK(!opened.transport->claimed());
    CHECK(stats.release_calls == 1);
    CHECK(stats.close_calls == 1);
    std::uint8_t data[4] = {};
    CHECK(opened.transport->control_in(0xc0U, 0xbcU, 0U, 0U, 4U, data, 1000U,
                                       usb::ControlKind::pure_read).cause ==
          usb::TransferCause::invalid);
}

void test_descriptor_mismatch_does_not_claim()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->summary.interface_zero.endpoints[0].max_packet = 64U;
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    CHECK(opened.outcome.state == usb::OpenState::unsupported);
    CHECK(opened.transport == nullptr);
    CHECK(stats.claim_calls == 0);
    CHECK(stats.close_calls == 1);
}

void test_open_failures_are_not_replaced()
{
    const usb::OpenState states[] = {
        usb::OpenState::not_found, usb::OpenState::access,
        usb::OpenState::disconnected, usb::OpenState::io,
        usb::OpenState::unsupported};
    for (const usb::OpenState state : states) {
        Stats stats;
        auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
        io->open_result = usb::OpenOutcome{state, usb::TransferCause::other};
        auto opened = usb::Transport::open(std::move(io), location_1_2());
        CHECK(opened.outcome.state == state);
        CHECK(opened.transport == nullptr);
        CHECK(stats.claim_calls == 0);
        CHECK(stats.close_calls == 1);
    }
}

void test_busy_claim_closes()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->claim_result = usb::OpenOutcome{usb::OpenState::busy, usb::TransferCause::busy};
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    CHECK(opened.outcome.state == usb::OpenState::busy);
    CHECK(opened.transport == nullptr);
    CHECK(stats.claim_calls == 1);
    CHECK(stats.close_calls == 1);
}

void test_pure_read_retries_once()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->control_in_results = {
        outcome(usb::TransferState::failed, usb::TransferCause::timeout),
        outcome(usb::TransferState::complete, usb::TransferCause::none, 4U)};
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    std::uint8_t data[4] = {};
    const auto result = opened.transport->control_in(
        0xc0U, 0xbcU, 0x0800U, 0x2008U, 4U, data, 1000U,
        usb::ControlKind::pure_read);
    CHECK(result.state == usb::TransferState::complete);
    CHECK(result.transferred == 4U);
    CHECK(result.retried);
    CHECK(stats.control_in_calls == 2);
    CHECK(stats.last_request == 0xbcU);
    CHECK(stats.last_value == 0x0800U);
    CHECK(stats.last_index == 0x2008U);
}

void test_abs_write_retries_once()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->control_out_results = {
        outcome(usb::TransferState::failed, usb::TransferCause::timeout),
        outcome(usb::TransferState::complete, usb::TransferCause::none, 4U)};
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    const std::uint8_t data[4] = {1U, 2U, 3U, 4U};
    const auto result = opened.transport->control_out(
        0x40U, 0xbcU, 0x0800U, 0x2008U, 4U, data, 1000U,
        usb::ControlKind::abs_write);
    CHECK(result.state == usb::TransferState::complete);
    CHECK(result.retried);
    CHECK(stats.control_out_calls == 2);
}

void test_retryable_failure_stops_after_one_retry()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->control_in_results = {
        outcome(usb::TransferState::failed, usb::TransferCause::timeout),
        outcome(usb::TransferState::failed, usb::TransferCause::pipe)};
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    std::uint8_t data[4] = {};
    const auto result = opened.transport->control_in(
        0xc0U, 0xbcU, 0U, 0U, 4U, data, 1000U,
        usb::ControlKind::pure_read);
    CHECK(result.state == usb::TransferState::failed);
    CHECK(result.cause == usb::TransferCause::pipe);
    CHECK(result.retried);
    CHECK(stats.control_in_calls == 2);
}

void test_trigger_failure_is_outcome_unknown_without_retry()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->control_out_results = {
        outcome(usb::TransferState::failed, usb::TransferCause::timeout)};
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    const std::uint8_t data[4] = {};
    const auto result = opened.transport->control_out(
        0x40U, 0xbcU, 0U, 0U, 4U, data, 1000U,
        usb::ControlKind::trigger);
    CHECK(result.state == usb::TransferState::outcome_unknown);
    CHECK(result.cause == usb::TransferCause::timeout);
    CHECK(!result.retried);
    CHECK(stats.control_out_calls == 1);
}

void test_consuming_short_is_outcome_unknown()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->control_in_results = {
        outcome(usb::TransferState::short_transfer, usb::TransferCause::none, 1U)};
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    std::uint8_t data[4] = {};
    const auto result = opened.transport->control_in(
        0xc0U, 0xbcU, 0U, 0U, 4U, data, 1000U,
        usb::ControlKind::consuming_read);
    CHECK(result.state == usb::TransferState::outcome_unknown);
    CHECK(!result.retried);
    CHECK(stats.control_in_calls == 1);
}

void test_unclassified_and_direction_mismatch_are_refused()
{
    Stats stats;
    auto opened = usb::Transport::open(make_io(stats), location_1_2());
    std::uint8_t data[4] = {};
    CHECK(opened.transport->control_in(0xc0U, 0xbcU, 0U, 0U, 4U, data, 1000U,
                                       usb::ControlKind::unclassified).cause ==
          usb::TransferCause::invalid);
    CHECK(opened.transport->control_out(0x40U, 0xbcU, 0U, 0U, 4U, data, 1000U,
                                        usb::ControlKind::pure_read).cause ==
          usb::TransferCause::invalid);
    CHECK(stats.control_in_calls == 0);
    CHECK(stats.control_out_calls == 0);
}

void test_bulk_outcome_conversion()
{
    auto timeout = usb::bulk_transfer_outcome(LIBUSB_ERROR_TIMEOUT, 188, 512U);
    CHECK(timeout.state == usb::TransferState::failed);
    CHECK(timeout.cause == usb::TransferCause::timeout);
    CHECK(timeout.transferred == 188U);

    auto no_device = usb::bulk_transfer_outcome(LIBUSB_ERROR_NO_DEVICE, 0, 512U);
    CHECK(no_device.state == usb::TransferState::failed);
    CHECK(no_device.cause == usb::TransferCause::no_device);
    CHECK(no_device.transferred == 0U);

    auto short_read = usb::bulk_transfer_outcome(LIBUSB_SUCCESS, 188, 512U);
    CHECK(short_read.state == usb::TransferState::short_transfer);
    CHECK(short_read.cause == usb::TransferCause::none);
    CHECK(short_read.transferred == 188U);

    auto complete = usb::bulk_transfer_outcome(LIBUSB_SUCCESS, 512, 512U);
    CHECK(complete.state == usb::TransferState::complete);
    CHECK(complete.cause == usb::TransferCause::none);
    CHECK(complete.transferred == 512U);
}

void test_control_entry_direction_mismatch_is_refused()
{
    Stats stats;
    auto opened = usb::Transport::open(make_io(stats), location_1_2());
    std::uint8_t data[4] = {};
    const auto wrong_in = opened.transport->control_in(
        0x40U, 0xbcU, 0U, 0U, 4U, data, 1000U,
        usb::ControlKind::abs_write);
    CHECK(wrong_in.state == usb::TransferState::failed);
    CHECK(wrong_in.cause == usb::TransferCause::invalid);
    const auto wrong_out = opened.transport->control_out(
        0xc0U, 0xbcU, 0U, 0U, 4U, data, 1000U,
        usb::ControlKind::pure_read);
    CHECK(wrong_out.state == usb::TransferState::failed);
    CHECK(wrong_out.cause == usb::TransferCause::invalid);
    CHECK(stats.control_in_calls == 0);
    CHECK(stats.control_out_calls == 0);
}

void test_missing_payload_is_refused()
{
    Stats stats;
    auto opened = usb::Transport::open(make_io(stats), location_1_2());
    const auto missing_in = opened.transport->control_in(
        0xc0U, 0xbcU, 0U, 0U, 4U, nullptr, 1000U,
        usb::ControlKind::pure_read);
    CHECK(missing_in.state == usb::TransferState::failed);
    CHECK(missing_in.cause == usb::TransferCause::invalid);
    const auto missing_out = opened.transport->control_out(
        0x40U, 0xbcU, 0U, 0U, 4U, nullptr, 1000U,
        usb::ControlKind::trigger);
    CHECK(missing_out.state == usb::TransferState::failed);
    CHECK(missing_out.cause == usb::TransferCause::invalid);
    CHECK(stats.control_in_calls == 0);
    CHECK(stats.control_out_calls == 0);
}

void test_zero_length_control_out_reaches_native()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->control_out_results = {
        outcome(usb::TransferState::complete, usb::TransferCause::none, 0U)};
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    const auto result = opened.transport->control_out(
        0x40U, 0xbcU, 0U, 0U, 0U, nullptr, 1000U,
        usb::ControlKind::trigger);
    CHECK(result.state == usb::TransferState::complete);
    CHECK(stats.control_out_calls == 1);
}

void test_bulk_short_and_timeout()
{
    Stats stats;
    auto io = std::unique_ptr<FakeIo>(new FakeIo(stats));
    io->bulk_in_results = {
        outcome(usb::TransferState::short_transfer, usb::TransferCause::none, 188U),
        outcome(usb::TransferState::failed, usb::TransferCause::timeout)};
    auto opened = usb::Transport::open(std::move(io), location_1_2());
    std::uint8_t data[512] = {};
    const auto short_result = opened.transport->bulk_in(
        0x81U, data, 512U, 1000U);
    CHECK(short_result.state == usb::TransferState::short_transfer);
    CHECK(short_result.transferred == 188U);
    CHECK(stats.bulk_in_calls == 1);
    const auto timeout_result = opened.transport->bulk_in(
        0x81U, data, 512U, 1000U);
    CHECK(timeout_result.state == usb::TransferState::failed);
    CHECK(timeout_result.cause == usb::TransferCause::timeout);
    CHECK(stats.bulk_in_calls == 2);
    CHECK(opened.transport->bulk_in(0x83U, data, 64U, 1000U).cause ==
          usb::TransferCause::invalid);
    CHECK(stats.bulk_in_calls == 2);
}

void test_location_parser()
{
    const auto selector = dtv::userland::identity::parse_device_selector("usb:1-4.2");
    CHECK(selector.has_value());
    usb::DeviceLocation location;
    CHECK(usb::location_from_selector(selector.value(), location));
    CHECK(location.bus == 1U);
    CHECK(location.port_count == 2U);
    CHECK(location.ports[0] == 4U);
    CHECK(location.ports[1] == 2U);

    const auto serial = dtv::userland::identity::parse_device_selector("serial:abc");
    CHECK(serial.has_value());
    CHECK(!usb::location_from_selector(serial.value(), location));
}

void test_fd_context_skips_discovery()
{
#if defined(__linux__)
    auto io = usb::create_libusb_io_fd();
    CHECK(io != nullptr);
    if (io) io->close();
#endif
}

}  // namespace

int main()
{
    test_open_and_close();
    test_open_fd_and_close();
    test_open_fd_failure_closes_io();
    test_open_fd_descriptor_mismatch_does_not_claim();
    test_open_fd_invalid_argument_does_not_open();
    test_close_is_idempotent();
    test_open_failures_are_not_replaced();
    test_descriptor_mismatch_does_not_claim();
    test_busy_claim_closes();
    test_pure_read_retries_once();
    test_abs_write_retries_once();
    test_retryable_failure_stops_after_one_retry();
    test_trigger_failure_is_outcome_unknown_without_retry();
    test_consuming_short_is_outcome_unknown();
    test_unclassified_and_direction_mismatch_are_refused();
    test_control_entry_direction_mismatch_is_refused();
    test_missing_payload_is_refused();
    test_zero_length_control_out_reaches_native();
    test_bulk_outcome_conversion();
    test_bulk_short_and_timeout();
    test_location_parser();
    test_fd_context_skips_discovery();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
