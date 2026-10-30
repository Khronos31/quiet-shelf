// SPDX-License-Identifier: GPL-2.0-only
#include "core1/i2c_backend_cpp.h"

#include "usb/transport.h"
#include "widen/bridge.h"

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
namespace core1 = torne::core1;
namespace widen = torne::widen;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

constexpr std::uint32_t kCtrl = 0x017A3840U;
constexpr std::uint32_t kStat = 0x017A3880U;
constexpr std::uint32_t kReq = 0x017A3900U;
constexpr std::uint32_t kCpl = 0x017A3B00U;
constexpr std::uint32_t kProducer = kCtrl + 0x10U;
constexpr std::uint32_t kResume = kCtrl + 0x18U;
constexpr std::uint32_t kProgress = kStat + 0x1CU;

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

struct Completion final {
    std::uint32_t status = 0U;
    std::uint8_t rlen = 0U;
    std::array<std::uint8_t, 32U> rdata{};
};

struct RingState final {
    usb::DeviceSummary descriptor = summary();
    std::map<std::uint32_t, std::uint8_t> memory;
    std::uint32_t session = 0x00030000U;
    std::uint32_t expected_session = 0x00030000U;
    std::uint32_t base = 0U;
    std::uint32_t prod = 0U;
    std::uint32_t cons = 0U;
    std::uint32_t progress = 0U;
    std::uint32_t halt_index = 0U;
    std::uint32_t halt_status = 0U;
    std::uint32_t resume_ack = 0U;
    std::uint32_t heartbeat = 0U;
    bool halted = false;
    bool heartbeat_stalled = false;
    bool never_progress = false;
    bool fail_completion = false;
    bool fail_progress = false;
    bool corrupt_completion_tag = false;
    std::deque<Completion> script;
    std::vector<std::array<std::uint8_t, 64U>> request_slots;
    std::vector<std::uint32_t> producers;
    int open_calls = 0;
    int claim_calls = 0;
    int release_calls = 0;
    int close_calls = 0;
};

void write_mem(RingState& s, std::uint32_t a, const std::uint8_t* d, std::size_t n)
{
    for (std::size_t i = 0U; i < n; ++i)
        s.memory[a + static_cast<std::uint32_t>(i)] = d[i];
}

void read_mem(RingState& s, std::uint32_t a, std::uint8_t* d, std::size_t n)
{
    for (std::size_t i = 0U; i < n; ++i) {
        const auto it = s.memory.find(a + static_cast<std::uint32_t>(i));
        d[i] = it == s.memory.end() ? 0U : it->second;
    }
}

void update_stat(RingState& s)
{
    std::uint8_t block[64] = {};
    put32(block + 0x04U, s.heartbeat);
    put32(block + 0x18U, s.cons & 0xFFFFU);
    put32(block + 0x1CU, s.progress);
    put32(block + 0x20U, s.halt_index);
    put32(block + 0x24U, s.halt_status);
    put32(block + 0x30U, s.resume_ack);
    write_mem(s, kStat, block, sizeof(block));
}

void write_completion(RingState& s, std::uint32_t wire, const Completion& c)
{
    const std::uint32_t slot = wire % 8U;
    const std::uint32_t tag = (s.expected_session & 0xFFFF0000U) | wire;
    std::uint8_t block[64] = {};
    put32(block, tag);
    put32(block + 4U, c.status);
    put32(block + 8U, c.rlen);
    std::memcpy(block + 0x10U, c.rdata.data(), c.rlen);
    put32(block + 0x3CU, s.corrupt_completion_tag ? (tag ^ 1U) : tag);
    write_mem(s, kCpl + slot * 64U, block, sizeof(block));
}

void process(RingState& s)
{
    if (s.halted || s.never_progress) return;
    while ((s.cons & 0xFFFFU) != (s.prod & 0xFFFFU)) {
        const std::uint32_t wire = s.cons & 0xFFFFU;
        const std::uint32_t slot = wire % 8U;
        std::uint8_t block[64] = {};
        read_mem(s, kReq + slot * 64U, block, sizeof(block));
        const std::uint32_t tag = (s.expected_session & 0xFFFF0000U) | wire;
        Completion c;
        if (be32(block) != tag || be32(block + 0x3CU) != tag) {
            c.status = TORNE_I2C_TORN;
        } else {
            c = s.script.empty() ? Completion{} : s.script.front();
            if (!s.script.empty()) s.script.pop_front();
            if (c.status == 0U && c.rlen == 0U) {
                const std::uint32_t op = be32(block + 4U);
                const std::uint32_t lengths = be32(block + 8U);
                if ((op & 0xFFU) != TORNE_I2C_WRITE) {
                    c.rlen = static_cast<std::uint8_t>(lengths >> 8U);
                    for (std::size_t i = 0U; i < c.rlen; ++i)
                        c.rdata[i] = static_cast<std::uint8_t>(0xA0U + i);
                }
            }
        }
        write_completion(s, wire, c);
        s.cons = (s.cons + 1U) & 0xFFFFU;
        if ((c.status & 0xFFU) == TORNE_I2C_OK) {
            s.progress = s.cons;
        } else {
            s.halted = true;
            s.halt_index = wire;
            s.halt_status = c.status;
            s.progress = 0x80000000U | s.cons;
            break;
        }
    }
    update_stat(s);
}

void handle_resume(RingState& s, std::uint32_t value)
{
    const std::uint32_t index = value & 0xFFFFU;
    const std::uint32_t action = (value >> 16U) & 0xFFU;
    if (s.halted && index == s.halt_index) {
        if (action == 2U) {
            while ((s.cons & 0xFFFFU) != (s.prod & 0xFFFFU)) {
                const std::uint32_t wire = s.cons & 0xFFFFU;
                Completion skipped;
                skipped.status = TORNE_I2C_SKIPPED;
                write_completion(s, wire, skipped);
                s.cons = (s.cons + 1U) & 0xFFFFU;
            }
            s.progress = s.cons;
            s.halted = false;
        } else if (action == 1U) {
            s.halted = false;
        }
    }
    s.resume_ack = value;
    update_stat(s);
}

class FakeRingIo final : public usb::UsbIo {
public:
    explicit FakeRingIo(RingState& state) : state_(state)
    {
        state_.prod = state_.base & 0xFFFFU;
        state_.cons = state_.base & 0xFFFFU;
        state_.progress = state_.base & 0xFFFFU;
        std::uint8_t p[4] = {};
        put32(p, state_.base & 0xFFFFU);
        write_mem(state_, kProducer, p, 4U);
        update_stat(state_);
    }

    usb::OpenOutcome open(const usb::DeviceLocation&, usb::DeviceSummary& out) noexcept override
    {
        ++state_.open_calls;
        out = state_.descriptor;
        return usb::OpenOutcome{usb::OpenState::ok, usb::TransferCause::none};
    }
    usb::OpenOutcome claim_interface(std::uint8_t) noexcept override
    {
        ++state_.claim_calls;
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
        if (address == kProgress) {
            if (state_.fail_progress) return fail(usb::TransferCause::no_device);
            process(state_);
        }
        if (address >= kStat && address < kStat + 64U) {
            if (!state_.heartbeat_stalled) ++state_.heartbeat;
            update_stat(state_);
        }
        if (address >= kCpl && address < kCpl + 8U * 64U) {
            if (state_.fail_completion) return fail(usb::TransferCause::timeout);
        }
        read_mem(state_, address, data, length);
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
        if (address == kProducer && length == 4U) {
            state_.prod = be32(data) & 0xFFFFU;
            state_.producers.push_back(state_.prod);
        } else if (address == kResume && length == 4U) {
            handle_resume(state_, be32(data));
        } else if (address >= kReq && address < kReq + 8U * 64U &&
                   length == 64U) {
            std::array<std::uint8_t, 64U> slot{};
            std::memcpy(slot.data(), data, 64U);
            state_.request_slots.push_back(slot);
        }
        write_mem(state_, address, data, length);
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
    void release_interface(std::uint8_t) noexcept override { ++state_.release_calls; }
    void close() noexcept override { ++state_.close_calls; }

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
    RingState& state_;
};

class TestClock final : public core1::Clock {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t ms) noexcept override { now += ms; }
    std::uint64_t now = 0U;
};

struct Harness final {
    RingState state;
    TestClock clock;
    std::unique_ptr<usb::UsbIo> io;
    usb::OpenResult opened;
    std::unique_ptr<widen::Bridge> bridge;
    std::unique_ptr<core1::RingBackend> backend;
    torne_i2c_backend c{};

    explicit Harness(std::uint32_t base = 0U, std::uint32_t session = 0x00030000U)
    {
        state.base = base;
        state.session = session;
        state.expected_session = session;
        io = std::unique_ptr<usb::UsbIo>(new FakeRingIo(state));
        opened = usb::Transport::open(std::move(io), location());
        bridge = std::unique_ptr<widen::Bridge>(new widen::Bridge(*opened.transport));
        backend = core1::RingBackend::create(*bridge, session, clock);
        if (backend) backend->fill_c_api(c);
    }
};

torne_i2c_req request(std::uint8_t op, std::uint8_t address,
                      std::uint8_t wlen, std::uint8_t rlen,
                      std::initializer_list<std::uint8_t> data = {})
{
    torne_i2c_req req{};
    req.op = op;
    req.address = address;
    req.wlen = wlen;
    req.rlen = rlen;
    std::size_t i = 0U;
    for (const std::uint8_t byte : data) {
        if (i < sizeof(req.wdata)) req.wdata[i++] = byte;
    }
    return req;
}

std::uint32_t submit(Harness& h, const torne_i2c_req& req)
{
    std::uint32_t ticket = 0U;
    const int status = h.c.submit(h.c.ctx, &req, &ticket);
    CHECK(status == 0);
    return ticket;
}

torne_i2c_result wait(Harness& h, std::uint32_t ticket, std::uint32_t timeout = 10U)
{
    torne_i2c_result result{};
    const int status = h.c.wait(h.c.ctx, ticket, &result, timeout);
    CHECK(status == 0);
    return result;
}

void test_measured_d896_d897_t1a_and_discard()
{
    Harness h;
    CHECK(h.backend != nullptr);
    h.state.script.push_back(Completion{0x01020000U, 2U, {0x01U, 0xB3U}});
    h.state.script.push_back(Completion{0x01020000U, 2U, {0x80U, 0x02U}});
    h.state.script.push_back(Completion{0x01000101U, 0U, {}});
    h.state.script.push_back(Completion{0x01020000U, 2U, {0x01U, 0xB3U}});

    const std::uint32_t d896 = submit(h, request(TORNE_I2C_WRITE_READ, 0x12U, 2U, 2U, {0x03U, 0x80U}));
    const std::uint32_t d897 = submit(h, request(TORNE_I2C_WRITE_READ, 0x12U, 2U, 2U, {0x03U, 0x81U}));
    const std::uint32_t t1a = submit(h, request(TORNE_I2C_WRITE_READ, 0xC0U, 1U, 2U, {0x1AU}));

    const auto first = wait(h, d896);
    CHECK(first.code == TORNE_I2C_OK);
    CHECK(first.rlen == 2U);
    CHECK(first.rdata[0] == 0x01U && first.rdata[1] == 0xB3U);
    const auto second = wait(h, d897);
    CHECK(second.code == TORNE_I2C_OK);
    CHECK(second.rdata[0] == 0x80U && second.rdata[1] == 0x02U);
    const auto nack = wait(h, t1a);
    CHECK(nack.code == TORNE_I2C_ADDR_NACK);
    CHECK(nack.phase == 1U);
    CHECK(nack.acked == 0U);
    CHECK(nack.bus_idle == 1U);

    CHECK(h.state.request_slots.size() >= 3U);
    CHECK(be32(h.state.request_slots[0].data()) == 0x00030000U);
    CHECK(be32(h.state.request_slots[0].data() + 4U) == 0x00120003U);
    CHECK(be32(h.state.request_slots[0].data() + 8U) == 0x00000202U);
    CHECK(h.state.request_slots[0][0x10] == 0x03U &&
          h.state.request_slots[0][0x11] == 0x80U);
    CHECK(be32(h.state.request_slots[1].data() + 4U) == 0x00120003U);
    CHECK(h.state.request_slots[1][0x11] == 0x81U);
    CHECK(be32(h.state.request_slots[2].data() + 4U) == 0x00C00003U);
    CHECK(be32(h.state.request_slots[2].data() + 8U) == 0x00000201U);
    CHECK(h.state.request_slots[2][0x10] == 0x1AU);

    CHECK(h.c.resume(h.c.ctx, 1) == 0);
    const std::uint32_t reread = submit(
        h, request(TORNE_I2C_WRITE_READ, 0x12U, 2U, 2U, {0x03U, 0x80U}));
    const auto reread_result = wait(h, reread);
    CHECK(reread_result.code == TORNE_I2C_OK);
    CHECK(reread_result.rdata[0] == 0x01U && reread_result.rdata[1] == 0xB3U);
}

void test_continue_and_skipped()
{
    Harness h;
    h.state.script.push_back(Completion{0x01020000U, 2U, {0x01U, 0xB3U}});
    h.state.script.push_back(Completion{0x01000101U, 0U, {}});
    const std::uint32_t first = submit(h, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    const std::uint32_t second = submit(h, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    const std::uint32_t third = submit(h, request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {0x00U}));
    CHECK(wait(h, first).code == TORNE_I2C_OK);
    CHECK(wait(h, second).code == TORNE_I2C_ADDR_NACK);
    CHECK(h.c.resume(h.c.ctx, 0) == 0);
    CHECK(wait(h, third).code == TORNE_I2C_OK);

    Harness s;
    s.state.script.push_back(Completion{0x01020000U, 2U, {0x01U, 0xB3U}});
    s.state.script.push_back(Completion{0x01000101U, 0U, {}});
    const std::uint32_t a = submit(s, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    const std::uint32_t b = submit(s, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    const std::uint32_t c = submit(s, request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {0x00U}));
    CHECK(wait(s, a).code == TORNE_I2C_OK);
    CHECK(wait(s, b).code == TORNE_I2C_ADDR_NACK);
    CHECK(s.c.resume(s.c.ctx, 1) == 0);
    CHECK(wait(s, c).code == TORNE_I2C_SKIPPED);
}

void test_limits_wrap_and_base()
{
    Harness full;
    std::uint32_t tickets[8] = {};
    for (int i = 0; i < 8; ++i) {
        const torne_i2c_req req = request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {0U});
        CHECK(full.c.submit(full.c.ctx, &req, &tickets[i]) == 0);
    }
    const torne_i2c_req ninth = request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {0U});
    std::uint32_t extra = 0U;
    CHECK(full.c.submit(full.c.ctx, &ninth, &extra) == TORNE_I2C_ERR_BUSY);
    for (int i = 0; i < 8; ++i) CHECK(wait(full, tickets[i]).code == TORNE_I2C_OK);

    Harness wrap(0xFFFEU);
    std::uint32_t w1 = submit(wrap, request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {1U}));
    std::uint32_t w2 = submit(wrap, request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {2U}));
    std::uint32_t w3 = submit(wrap, request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {3U}));
    CHECK(wait(wrap, w1).code == TORNE_I2C_OK);
    CHECK(wait(wrap, w2).code == TORNE_I2C_OK);
    CHECK(wait(wrap, w3).code == TORNE_I2C_OK);
    CHECK(wrap.state.request_slots.size() == 3U);

    Harness odd(1U);
    const std::uint32_t odd_ticket = submit(odd, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    CHECK(wait(odd, odd_ticket).code == TORNE_I2C_OK);
}

void test_flush_and_errors()
{
    Harness h;
    const std::uint32_t a = submit(h, request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {1U}));
    const std::uint32_t b = submit(h, request(TORNE_I2C_WRITE, 0x12U, 1U, 0U, {2U}));
    (void)a;
    (void)b;
    torne_i2c_result first{};
    CHECK(h.c.flush(h.c.ctx, 10U, &first) == 0);
    CHECK(first.code == TORNE_I2C_OK);

    Harness invalid;
    const torne_i2c_req bad = request(TORNE_I2C_WRITE, 0x13U, 1U, 0U, {1U});
    std::uint32_t ticket = 0U;
    CHECK(invalid.c.submit(invalid.c.ctx, &bad, &ticket) == TORNE_I2C_ERR_INVALID);
    CHECK(invalid.state.request_slots.empty());

    Harness gone;
    gone.state.fail_progress = true;
    const std::uint32_t g = submit(gone, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    torne_i2c_result gone_result{};
    CHECK(gone.c.wait(gone.c.ctx, g, &gone_result, 10U) == TORNE_I2C_ERR_DEVICE);
}

void test_flush_last_failure_and_continue_next_nack()
{
    Harness h;
    h.state.script.push_back(Completion{0x01000101U, 0U, {}});
    (void)submit(h, request(TORNE_I2C_WRITE, 0xC0U, 1U, 0U, {0x1AU}));
    torne_i2c_result first{};
    const int rc = h.c.flush(h.c.ctx, 10U, &first);
    CHECK(rc != 0 || first.code != TORNE_I2C_OK);

    Harness ok;
    (void)submit(ok, request(TORNE_I2C_WRITE, 0xC0U, 1U, 0U, {0x1AU}));
    torne_i2c_result ok_first{};
    CHECK(ok.c.flush(ok.c.ctx, 10U, &ok_first) == 0);
    CHECK(ok_first.code == TORNE_I2C_OK);

    Harness chain;
    chain.state.script.push_back(Completion{0x01000101U, 0U, {}});
    chain.state.script.push_back(Completion{0x01000101U, 0U, {}});
    const std::uint32_t a = submit(chain, request(TORNE_I2C_WRITE, 0xC0U, 1U, 0U, {0x1AU}));
    const std::uint32_t b = submit(chain, request(TORNE_I2C_WRITE, 0xC0U, 1U, 0U, {0x1BU}));
    CHECK(wait(chain, a).code == TORNE_I2C_ADDR_NACK);
    CHECK(chain.c.resume(chain.c.ctx, 0) == 0);
    const auto second = wait(chain, b);
    CHECK(second.code == TORNE_I2C_ADDR_NACK);

    Harness chain_ok;
    chain_ok.state.script.push_back(Completion{0x01000101U, 0U, {}});
    const std::uint32_t c = submit(chain_ok, request(TORNE_I2C_WRITE, 0xC0U, 1U, 0U, {0x1AU}));
    const std::uint32_t d = submit(chain_ok, request(TORNE_I2C_WRITE, 0xC0U, 1U, 0U, {0x1BU}));
    CHECK(wait(chain_ok, c).code == TORNE_I2C_ADDR_NACK);
    CHECK(chain_ok.c.resume(chain_ok.c.ctx, 0) == 0);
    CHECK(wait(chain_ok, d).code == TORNE_I2C_OK);
}

void test_flush_reports_failure_collected_by_prior_wait()
{
    Harness h;
    h.state.script.push_back(Completion{});
    h.state.script.push_back(Completion{0x01010202U, 0U, {}});
    const std::uint32_t a = submit(h, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    (void)submit(h, request(TORNE_I2C_WRITE, 0xC0U, 2U, 0U, {0x1AU, 0x02U}));
    CHECK(wait(h, a).code == TORNE_I2C_OK);
    torne_i2c_result first{};
    const int rc = h.c.flush(h.c.ctx, 10U, &first);
    CHECK(rc == 0);
    CHECK(first.code == TORNE_I2C_DATA_NACK);
    CHECK(first.phase == 2U);
    CHECK(first.acked == 1U);
    CHECK(first.bus_idle == 1U);

    Harness ok;
    ok.state.script.push_back(Completion{});
    const std::uint32_t ok_a = submit(ok, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    (void)submit(ok, request(TORNE_I2C_WRITE, 0xC0U, 2U, 0U, {0x1AU, 0x02U}));
    CHECK(wait(ok, ok_a).code == TORNE_I2C_OK);
    torne_i2c_result ok_first{};
    CHECK(ok.c.flush(ok.c.ctx, 10U, &ok_first) == 0);
    CHECK(ok_first.code == TORNE_I2C_OK);
}

void test_32_byte_and_protocol_errors()
{
    Harness h;
    std::uint8_t data[32] = {};
    for (std::size_t i = 0U; i < 32U; ++i) data[i] = static_cast<std::uint8_t>(i);
    const std::uint32_t t = submit(h, request(TORNE_I2C_WRITE_READ, 0x12U, 32U, 32U,
                                              {data[0], data[1], data[2], data[3]}));
    const auto r = wait(h, t);
    CHECK(r.code == TORNE_I2C_OK);
    CHECK(r.rlen == 32U);

    Harness tag;
    tag.state.corrupt_completion_tag = true;
    const std::uint32_t tag_ticket = submit(tag, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    torne_i2c_result tag_result{};
    CHECK(tag.c.wait(tag.c.ctx, tag_ticket, &tag_result, 10U) ==
          TORNE_I2C_ERR_PROTOCOL);

    Harness old;
    old.state.expected_session = 0x00040000U;
    const std::uint32_t old_ticket = submit(old, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    torne_i2c_result old_result{};
    CHECK(old.c.wait(old.c.ctx, old_ticket, &old_result, 10U) ==
          TORNE_I2C_ERR_PROTOCOL);
}

void test_timeout_and_stall()
{
    Harness h;
    h.state.never_progress = true;
    const std::uint32_t t = submit(h, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    const auto result = wait(h, t, 5U);
    CHECK(result.code == TORNE_I2C_NO_COMPLETION);

    Harness stall;
    stall.state.never_progress = true;
    stall.state.heartbeat_stalled = true;
    const std::uint32_t s = submit(stall, request(TORNE_I2C_READ, 0x12U, 0U, 2U));
    const auto stalled = wait(stall, s, 5U);
    CHECK(stalled.code == TORNE_I2C_CORE_STALL);
}

}  // namespace

int main()
{
    test_measured_d896_d897_t1a_and_discard();
    test_continue_and_skipped();
    test_limits_wrap_and_base();
    test_flush_and_errors();
    test_flush_last_failure_and_continue_next_nack();
    test_flush_reports_failure_collected_by_prior_wait();
    test_32_byte_and_protocol_errors();
    test_timeout_and_stall();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
