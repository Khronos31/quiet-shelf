// SPDX-License-Identifier: GPL-2.0-only
#include "dib/dib_frontend.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

namespace {
int failures = 0;
}

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

struct Recorded final {
    torne_i2c_req req{};
};

struct Fake final {
    std::map<std::uint16_t, std::uint16_t> dib;
    std::map<std::uint8_t, std::uint16_t> tun;
    std::vector<Recorded> requests;
    int fail_at = -1;
    std::uint8_t fail_code = TORNE_I2C_OK;
    int sleep_calls = 0;
    int flush_calls = 0;
    std::uint64_t now = 1000;
    std::uint32_t wait_cost_ms = 0;
};

std::uint16_t be16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

void put16(std::uint8_t* p, std::uint16_t value)
{
    p[0] = static_cast<std::uint8_t>(value >> 8);
    p[1] = static_cast<std::uint8_t>(value);
}

int submit_cb(void* ctx, const torne_i2c_req* req, std::uint32_t* ticket)
{
    Fake* fake = static_cast<Fake*>(ctx);
    if (!req || !ticket) return TORNE_I2C_ERR_INVALID;
    Recorded record;
    record.req = *req;
    fake->requests.push_back(record);
    *ticket = static_cast<std::uint32_t>(fake->requests.size() - 1U);
    return 0;
}

int wait_cb(void* ctx, std::uint32_t ticket, torne_i2c_result* result,
            std::uint32_t)
{
    Fake* fake = static_cast<Fake*>(ctx);
    if (fake->wait_cost_ms != 0U) fake->now += fake->wait_cost_ms;
    if (!result || ticket >= fake->requests.size())
        return TORNE_I2C_ERR_INTERNAL;
    const torne_i2c_req& req = fake->requests[ticket].req;
    std::memset(result, 0, sizeof(*result));
    result->code = TORNE_I2C_OK;
    result->bus_idle = 1;

    if (fake->fail_at >= 0 &&
        static_cast<std::size_t>(fake->fail_at) == ticket) {
        result->code = fake->fail_code;
        return 0;
    }

    if (req.address == 0x12U) {
        if (req.op == TORNE_I2C_WRITE) {
            if (req.wlen != 4U) {
                result->code = TORNE_I2C_INVALID;
                return 0;
            }
            const std::uint16_t reg = be16(req.wdata);
            const std::uint16_t value = be16(req.wdata + 2);
            if (!(req.wdata[0] == 0x05U && req.wdata[1] == 0x07U &&
                  req.wdata[2] == 0x00U && req.wdata[3] == 0x03U))
                fake->dib[reg] = value;
            result->acked = static_cast<std::uint8_t>(req.wlen);
        } else if (req.op == TORNE_I2C_WRITE_READ) {
            if (req.wlen != 2U || req.rlen != 2U) {
                result->code = TORNE_I2C_INVALID;
                return 0;
            }
            const auto found = fake->dib.find(be16(req.wdata));
            put16(result->rdata,
                  found == fake->dib.end() ? 0U : found->second);
            result->rlen = 2;
            result->acked = 2;
        } else {
            result->code = TORNE_I2C_INVALID;
        }
    } else if (req.address == 0xC0U) {
        if (req.op == TORNE_I2C_WRITE) {
            if (req.wlen != 3U) {
                result->code = TORNE_I2C_INVALID;
                return 0;
            }
            fake->tun[req.wdata[0]] = be16(req.wdata + 1);
            result->acked = 3;
        } else if (req.op == TORNE_I2C_WRITE_READ) {
            if (req.wlen != 1U || req.rlen != 2U) {
                result->code = TORNE_I2C_INVALID;
                return 0;
            }
            const auto found = fake->tun.find(req.wdata[0]);
            put16(result->rdata,
                  found == fake->tun.end() ? 0U : found->second);
            result->rlen = 2;
            result->acked = 1;
        } else {
            result->code = TORNE_I2C_INVALID;
        }
    } else {
        result->code = TORNE_I2C_ADDR_NACK;
    }
    return 0;
}

int flush_cb(void* ctx, std::uint32_t, torne_i2c_result* first_failure)
{
    static_cast<Fake*>(ctx)->flush_calls++;
    if (first_failure) {
        std::memset(first_failure, 0, sizeof(*first_failure));
        first_failure->code = TORNE_I2C_OK;
    }
    return 0;
}

int resume_cb(void*, int)
{
    return 0;
}

void sleep_cb(void* ctx, unsigned int)
{
    static_cast<Fake*>(ctx)->sleep_calls++;
}

std::uint64_t now_cb(void* ctx)
{
    return static_cast<Fake*>(ctx)->now++;
}

void log_cb(void*, int, const char*)
{
}

Fake make_fake()
{
    Fake fake;
    fake.dib[896] = 0x01B3;
    fake.dib[897] = 0x8002;
    fake.tun[0x1A] = 0x0223;
    fake.dib[390] = 0x0000;
    return fake;
}

torne_i2c_backend make_backend(Fake& fake)
{
    torne_i2c_backend backend{};
    backend.ctx = &fake;
    backend.caps = 0;
    backend.submit = submit_cb;
    backend.wait = wait_cb;
    backend.flush = flush_cb;
    backend.resume = resume_cb;
    return backend;
}

torne_dib_frontend_config make_config(Fake& fake, torne_i2c_backend& backend)
{
    torne_dib_frontend_config config{};
    config.backend = &backend;
    config.i2c_timeout_ms = 1000;
    config.core1_and_gpio_ready = true;
    config.sleep = sleep_cb;
    config.now = now_cb;
    config.log = log_cb;
    config.priv = &fake;
    return config;
}

bool has_request(const Fake& fake, std::uint8_t address, std::uint8_t op,
                 std::uint8_t wlen, std::uint8_t rlen)
{
    for (const Recorded& record : fake.requests) {
        if (record.req.address == address && record.req.op == op &&
            record.req.wlen == wlen && record.req.rlen == rlen)
            return true;
    }
    return false;
}

bool has_register_write(const Fake& fake, std::uint16_t reg, std::uint16_t value)
{
    for (const Recorded& record : fake.requests) {
        if (record.req.address == 0x12U && record.req.op == TORNE_I2C_WRITE &&
            record.req.wlen == 4U && be16(record.req.wdata) == reg &&
            be16(record.req.wdata + 2) == value)
            return true;
    }
    return false;
}

void test_init_and_register_paths()
{
    Fake fake = make_fake();
    torne_i2c_backend backend = make_backend(fake);
    torne_dib_frontend_config config = make_config(fake, backend);
    torne_dib_frontend frontend{};
    const int rc = torne_dib_frontend_init(&frontend, &config);
    CHECK(rc == 0);
    CHECK(fake.sleep_calls > 0);
    CHECK(has_request(fake, 0x12U, TORNE_I2C_WRITE, 4U, 0U));
    CHECK(has_request(fake, 0x12U, TORNE_I2C_WRITE_READ, 2U, 2U));
    CHECK(has_request(fake, 0xC0U, TORNE_I2C_WRITE, 3U, 0U));
    CHECK(has_request(fake, 0xC0U, TORNE_I2C_WRITE_READ, 1U, 2U));
    CHECK(has_register_write(fake, 1025U, 0xC000U));
    CHECK(has_register_write(fake, 1029U, 0xFFFFU));
    CHECK(has_register_write(fake, 1030U, 0x0000U));
    CHECK(fake.dib[896] == 0x01B3U);
    CHECK(fake.dib[897] == 0x8002U);
    CHECK(fake.flush_calls > 0);

    bool gate_seen = false;
    bool tuner_before_gate = false;
    for (const Recorded& record : fake.requests) {
        if (record.req.address == 0x12U && record.req.op == TORNE_I2C_WRITE &&
            record.req.wlen == 4U && be16(record.req.wdata) == 1025U &&
            be16(record.req.wdata + 2) == 0xC000U)
            gate_seen = true;
        if (record.req.address == 0xC0U && !gate_seen)
            tuner_before_gate = true;
    }
    CHECK(!tuner_before_gate);
}

void test_set_gpio_uses_register_read_modify_write()
{
    Fake fake = make_fake();
    torne_i2c_backend backend = make_backend(fake);
    torne_dib_frontend_config config = make_config(fake, backend);
    torne_dib_frontend frontend{};
    CHECK(torne_dib_frontend_init(&frontend, &config) == 0);
    const std::size_t before = fake.requests.size();
    const std::uint16_t before_dir = fake.dib[1029];
    const std::uint16_t before_val = fake.dib[1030];
    CHECK(torne_dib_frontend_set_gpio(&frontend, 3U, true) == 0);
    CHECK(fake.requests.size() > before);
    CHECK(has_register_write(fake, 1029U,
                             static_cast<std::uint16_t>(before_dir & ~0x0008U)));
    CHECK(has_register_write(fake, 1030U,
                             static_cast<std::uint16_t>((before_val & ~0x0008U) | 0x0008U)));
}

void test_failure_propagates_and_stops()
{
    Fake fake = make_fake();
    fake.fail_at = 0;
    fake.fail_code = TORNE_I2C_DATA_NACK;
    torne_i2c_backend backend = make_backend(fake);
    torne_dib_frontend_config config = make_config(fake, backend);
    torne_dib_frontend frontend{};
    const int rc = torne_dib_frontend_init(&frontend, &config);
    CHECK(rc < 0);
    CHECK(fake.requests.size() <= 1U);
}

void test_tune_timeout_bounds_i2c_wait()
{
    Fake fake = make_fake();
    fake.wait_cost_ms = 200U;
    torne_i2c_backend backend = make_backend(fake);
    torne_dib_frontend_config config = make_config(fake, backend);
    torne_dib_frontend frontend{};
    CHECK(torne_dib_frontend_init(&frontend, &config) == 0);
    fake.now = 1000U;
    const int rc = torne_dib_frontend_tune_timeout(&frontend, 473142857U, 100U);
    CHECK(rc == -ETIMEDOUT);
    CHECK(fake.now >= 1100U);
}

void test_preconditions_and_tune_range()
{
    Fake fake = make_fake();
    torne_i2c_backend backend = make_backend(fake);
    torne_dib_frontend_config config = make_config(fake, backend);
    config.core1_and_gpio_ready = false;
    torne_dib_frontend frontend{};
    CHECK(torne_dib_frontend_init(&frontend, &config) < 0);

    config.core1_and_gpio_ready = true;
    CHECK(torne_dib_frontend_init(&frontend, &config) == 0);
    CHECK(torne_dib_frontend_tune(&frontend, 1U) < 0);
}

void test_status_exposes_only_measured_fields()
{
    Fake fake = make_fake();
    torne_i2c_backend backend = make_backend(fake);
    torne_dib_frontend_config config = make_config(fake, backend);
    torne_dib_frontend frontend{};
    CHECK(torne_dib_frontend_init(&frontend, &config) == 0);

    fake.dib[568] = static_cast<std::uint16_t>((1U << 13) | (1U << 8) |
                                               (0xFU << 1) | (1U << 12) |
                                               (1U << 5));
    fake.dib[554] = fake.dib[555] = fake.dib[556] = 1U;
    fake.dib[390] = 0U;
    fake.dib[542] = 0U;
    fake.dib[543] = 0U;

    struct torne_dib_status status{};
    CHECK(torne_dib_frontend_status(&frontend, &status) == 0);
    CHECK(status.locked);
    CHECK((status.status & TORNE_FE_HAS_SIGNAL) != 0);
    CHECK((status.status & TORNE_FE_HAS_CARRIER) != 0);
    CHECK((status.status & TORNE_FE_HAS_VITERBI) != 0);
    CHECK((status.status & TORNE_FE_HAS_SYNC) != 0);
    CHECK((status.status & TORNE_FE_HAS_LOCK) != 0);
    CHECK(status.has_strength);
    CHECK(status.has_cnr);
}

}  // namespace

int main()
{
    test_init_and_register_paths();
    test_set_gpio_uses_register_read_modify_write();
    test_failure_propagates_and_stops();
    test_tune_timeout_bounds_i2c_wait();
    test_preconditions_and_tune_range();
    test_status_exposes_only_measured_fields();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("dib frontend tests passed\n");
    return 0;
}
