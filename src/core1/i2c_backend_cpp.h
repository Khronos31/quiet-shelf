// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_CORE1_I2C_BACKEND_CPP_H
#define TORNE_CORE1_I2C_BACKEND_CPP_H

#include "core1/host_runtime.h"
#include "core1/i2c_backend.h"
#include "widen/bridge.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>

namespace torne::core1 {

// C++ implementation behind the C11 torne_i2c_backend. It uses the existing
// single-owner Ep0Io; it never opens USB or retries EP0 by itself.
class RingBackend final {
public:
    static std::unique_ptr<RingBackend> create(widen::Ep0Io& io,
                                               std::uint32_t session,
                                               Clock& clock) noexcept;

    void fill_c_api(torne_i2c_backend& out) noexcept;

private:
    struct Outstanding final {
        std::uint32_t token = 0U;
        std::uint64_t ticket = 0U;
        torne_i2c_req req{};
        torne_i2c_result result{};
        bool done = false;
    };

    RingBackend(widen::Ep0Io& io, std::uint32_t session, Clock& clock,
                std::uint32_t base) noexcept;

    static int submit_cb(void* ctx, const torne_i2c_req* req,
                         std::uint32_t* ticket) noexcept;
    static int wait_cb(void* ctx, std::uint32_t ticket,
                       torne_i2c_result* result, std::uint32_t timeout_ms) noexcept;
    static int flush_cb(void* ctx, std::uint32_t timeout_ms,
                        torne_i2c_result* first_failure) noexcept;
    static int resume_cb(void* ctx, int discard) noexcept;

    int submit(const torne_i2c_req& req, std::uint32_t& ticket) noexcept;
    int wait(std::uint32_t ticket, torne_i2c_result& result,
             std::uint32_t timeout_ms) noexcept;
    int flush(std::uint32_t timeout_ms, torne_i2c_result& first_failure) noexcept;
    int resume(bool discard) noexcept;

    int process_progress() noexcept;
    int read_completion(std::uint64_t ticket, torne_i2c_result& result) noexcept;
    int timeout_result(std::uint32_t timeout_ms, torne_i2c_result& result) noexcept;
    Outstanding* find(std::uint32_t token) noexcept;

    widen::Ep0Io& io_;
    Clock& clock_;
    std::uint32_t session_ = 0U;
    std::uint32_t base_ = 0U;
    std::uint64_t t_pub_ = 0U;
    std::uint64_t t_ret_ = 0U;
    std::uint32_t next_token_ = 1U;
    std::uint32_t next_serial_ = 1U;
    bool halted_ = false;
    std::uint64_t halt_ticket_ = 0U;
    std::uint32_t halt_wire_ = 0U;
    torne_i2c_result halt_result_{};
    bool has_halt_result_ = false;
    bool discard_active_ = false;
    std::uint64_t discard_from_ = 0U;
    std::uint64_t discard_until_ = 0U;
    std::deque<Outstanding> outstanding_;
    std::map<std::uint32_t, torne_i2c_result> completed_;
};

}  // namespace torne::core1

#endif  // TORNE_CORE1_I2C_BACKEND_CPP_H
