// SPDX-License-Identifier: GPL-2.0-only
#include "core1/i2c_backend_cpp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace torne::core1 {
namespace {

constexpr std::uint32_t kCtrlBase = 0x017A3840U;
constexpr std::uint32_t kStatBase = 0x017A3880U;
constexpr std::uint32_t kReqBase = 0x017A3900U;
constexpr std::uint32_t kCplBase = 0x017A3B00U;
constexpr std::uint32_t kProducer = kCtrlBase + 0x10U;
constexpr std::uint32_t kResume = kCtrlBase + 0x18U;
constexpr std::uint32_t kProgress = kStatBase + 0x1CU;
constexpr std::uint32_t kResumeAck = kStatBase + 0x30U;
constexpr std::uint32_t kHeartbeat = kStatBase + 0x04U;
constexpr std::uint32_t kRingSlots = 8U;
constexpr std::uint32_t kDeadlineMs = 100U;

std::uint32_t be32(const std::uint8_t* bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

void put32(std::uint8_t* bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t dist(std::uint32_t a, std::uint32_t b) noexcept
{
    return (a - b) & 0xFFFFU;
}

int read_error(const widen::ReadResult& result) noexcept
{
    if (result.transfer.cause == usb::TransferCause::no_device)
        return TORNE_I2C_ERR_DEVICE;
    return TORNE_I2C_ERR_IO;
}

int read_error(const widen::BlockReadResult& result) noexcept
{
    if (result.transfer.cause == usb::TransferCause::no_device)
        return TORNE_I2C_ERR_DEVICE;
    return TORNE_I2C_ERR_IO;
}

int write_error(const widen::WriteResult& result) noexcept
{
    if (result.transfer.cause == usb::TransferCause::no_device)
        return TORNE_I2C_ERR_DEVICE;
    return TORNE_I2C_ERR_IO;
}

bool valid_request(const torne_i2c_req& request) noexcept
{
    if ((request.address & 0x01U) != 0U ||
        request.wlen > 32U || request.rlen > 32U)
        return false;
    switch (request.op) {
    case TORNE_I2C_WRITE:
        return request.wlen >= 1U && request.wlen <= 32U && request.rlen == 0U;
    case TORNE_I2C_READ:
        return request.wlen == 0U && request.rlen >= 1U && request.rlen <= 32U;
    case TORNE_I2C_WRITE_READ:
        return request.wlen >= 1U && request.wlen <= 32U &&
               request.rlen >= 1U && request.rlen <= 32U;
    default:
        return false;
    }
}

void decode_status(std::uint32_t status, const std::uint8_t* data,
                   std::size_t length, torne_i2c_result& result) noexcept
{
    result.code = static_cast<std::uint8_t>(status & 0xFFU);
    result.phase = static_cast<std::uint8_t>((status >> 8U) & 0xFFU);
    result.acked = static_cast<std::uint8_t>((status >> 16U) & 0xFFU);
    result.bus_idle = static_cast<std::uint8_t>((status >> 24U) & 0x01U);
    const std::uint32_t rlen = be32(data + 8U);
    result.rlen = static_cast<std::uint8_t>(rlen > 32U ? 32U : rlen);
    if (length >= 0x10U)
        std::memcpy(result.rdata, data + 0x10U, result.rlen);
}

}  // namespace

RingBackend::RingBackend(widen::Ep0Io& io, std::uint32_t session,
                         Clock& clock, std::uint32_t base) noexcept
    : io_(io), clock_(clock), session_(session), base_(base & 0xFFFFU)
{
}

std::unique_ptr<RingBackend> RingBackend::create(widen::Ep0Io& io,
                                                 std::uint32_t session,
                                                 Clock& clock) noexcept
{
    if ((session & 0xFFFF0000U) == 0U) return nullptr;
    const widen::ReadResult base = io.read32_wide(kProducer);
    if (!base.ok) return nullptr;
    std::unique_ptr<RingBackend> backend(
        new RingBackend(io, session, clock, base.value));
    return backend;
}

void RingBackend::fill_c_api(torne_i2c_backend& out) noexcept
{
    out.ctx = this;
    out.caps = 0U;
    out.submit = &RingBackend::submit_cb;
    out.wait = &RingBackend::wait_cb;
    out.flush = &RingBackend::flush_cb;
    out.resume = &RingBackend::resume_cb;
}

int RingBackend::submit_cb(void* ctx, const torne_i2c_req* req,
                           std::uint32_t* ticket) noexcept
{
    if (ctx == nullptr || req == nullptr || ticket == nullptr)
        return TORNE_I2C_ERR_INVALID;
    return static_cast<RingBackend*>(ctx)->submit(*req, *ticket);
}

int RingBackend::wait_cb(void* ctx, std::uint32_t ticket,
                         torne_i2c_result* result,
                         std::uint32_t timeout_ms) noexcept
{
    if (ctx == nullptr || result == nullptr) return TORNE_I2C_ERR_INVALID;
    return static_cast<RingBackend*>(ctx)->wait(ticket, *result, timeout_ms);
}

int RingBackend::flush_cb(void* ctx, std::uint32_t timeout_ms,
                          torne_i2c_result* first_failure) noexcept
{
    if (ctx == nullptr || first_failure == nullptr)
        return TORNE_I2C_ERR_INVALID;
    return static_cast<RingBackend*>(ctx)->flush(timeout_ms, *first_failure);
}

int RingBackend::resume_cb(void* ctx, int discard) noexcept
{
    if (ctx == nullptr) return TORNE_I2C_ERR_INVALID;
    return static_cast<RingBackend*>(ctx)->resume(discard != 0);
}

RingBackend::Outstanding* RingBackend::find(std::uint32_t token) noexcept
{
    for (Outstanding& item : outstanding_) {
        if (item.token == token) return &item;
    }
    return nullptr;
}

int RingBackend::submit(const torne_i2c_req& req,
                        std::uint32_t& ticket) noexcept
{
    if (!valid_request(req)) return TORNE_I2C_ERR_INVALID;
    if (halted_) return TORNE_I2C_ERR_HALTED;
    if (outstanding_.size() >= kRingSlots) return TORNE_I2C_ERR_BUSY;

    const std::uint32_t wire =
        static_cast<std::uint32_t>((base_ + t_pub_) & 0xFFFFU);
    const std::uint32_t slot = wire % kRingSlots;
    const std::uint32_t tag = (session_ & 0xFFFF0000U) | wire;
    std::array<std::uint8_t, 64U> block{};
    put32(block.data(), tag);
    put32(block.data() + 4U,
          (static_cast<std::uint32_t>(req.address) << 16U) | req.op);
    put32(block.data() + 8U,
          (static_cast<std::uint32_t>(req.rlen) << 8U) | req.wlen);
    if (req.wlen != 0U)
        std::memcpy(block.data() + 0x10U, req.wdata, req.wlen);
    put32(block.data() + 0x3CU, tag);

    const widen::WriteResult slot_write =
        io_.write_block_wide(kReqBase + slot * 64U, block.data(), block.size());
    if (!slot_write.ok) return write_error(slot_write);
    const widen::WriteResult publish =
        io_.write32(kProducer, (wire + 1U) & 0xFFFFU, true);
    if (!publish.ok) return write_error(publish);

    Outstanding item;
    item.token = next_token_++;
    if (next_token_ == 0U) next_token_ = 1U;
    item.ticket = t_pub_;
    item.req = req;
    outstanding_.push_back(item);
    ticket = item.token;
    ++t_pub_;
    return 0;
}

int RingBackend::read_completion(std::uint64_t ticket,
                                 torne_i2c_result& result) noexcept
{
    const std::uint32_t wire =
        static_cast<std::uint32_t>((base_ + ticket) & 0xFFFFU);
    const std::uint32_t slot = wire % kRingSlots;
    const std::uint32_t tag = (session_ & 0xFFFF0000U) | wire;
    const widen::BlockReadResult block =
        io_.read_block_wide(kCplBase + slot * 64U, 64U);
    if (!block.ok || block.length != 64U) {
        if (!block.ok) return read_error(block);
        return TORNE_I2C_ERR_IO;
    }
    const std::uint32_t begin = be32(block.bytes.data());
    const std::uint32_t end = be32(block.bytes.data() + 0x3CU);
    if (begin != tag || end != tag) return TORNE_I2C_ERR_PROTOCOL;
    decode_status(be32(block.bytes.data() + 4U), block.bytes.data(),
                  block.bytes.size(), result);
    return 0;
}

int RingBackend::process_progress() noexcept
{
    const widen::ReadResult progress = io_.read32_wide(kProgress);
    if (!progress.ok) return read_error(progress);
    const bool halted = (progress.value & 0x80000000U) != 0U;
    const std::uint32_t current = progress.value & 0xFFFFU;
    const std::uint32_t wire_ret =
        static_cast<std::uint32_t>((base_ + t_ret_) & 0xFFFFU);
    const std::uint32_t completed = dist(current, wire_ret);
    if (completed > outstanding_.size()) return TORNE_I2C_ERR_PROTOCOL;

    for (std::uint32_t index = 0U; index < completed; ++index) {
        if (outstanding_.empty()) return TORNE_I2C_ERR_PROTOCOL;
        Outstanding item = outstanding_.front();
        const bool last_failure = halted && index + 1U == completed;
        if (discard_active_ && item.ticket >= discard_from_ &&
            item.ticket < discard_until_) {
            item.result.code = TORNE_I2C_SKIPPED;
            item.result.bus_idle = 0xFFU;
        } else if (last_failure) {
            const int status = read_completion(item.ticket, item.result);
            if (status != 0) return status;
            halted_ = true;
            halt_ticket_ = item.ticket;
            halt_wire_ = static_cast<std::uint32_t>(
                (base_ + item.ticket) & 0xFFFFU);
            halt_result_ = item.result;
            has_halt_result_ = true;
        } else if (item.req.op == TORNE_I2C_WRITE) {
            item.result.code = TORNE_I2C_OK;
            item.result.phase = 0U;
            item.result.acked = item.req.wlen;
            item.result.bus_idle = 1U;
            item.result.rlen = 0U;
        } else {
            const int status = read_completion(item.ticket, item.result);
            if (status != 0) return status;
            if (item.result.code != TORNE_I2C_OK)
                return TORNE_I2C_ERR_PROTOCOL;
        }
        item.done = true;
        completed_[item.token] = item.result;
        outstanding_.pop_front();
        ++t_ret_;
    }
    if (discard_active_ && t_ret_ >= discard_until_) discard_active_ = false;
    return 0;
}

int RingBackend::timeout_result(std::uint32_t timeout_ms,
                                torne_i2c_result& result) noexcept
{
    (void)timeout_ms;
    const widen::ReadResult first = io_.read32_wide(kHeartbeat);
    if (!first.ok) return read_error(first);
    clock_.sleep_ms(1U);
    const widen::ReadResult second = io_.read32_wide(kHeartbeat);
    if (!second.ok) return read_error(second);
    result.code = first.value == second.value ? TORNE_I2C_CORE_STALL
                                              : TORNE_I2C_NO_COMPLETION;
    result.bus_idle = 0xFFU;
    return 0;
}

int RingBackend::wait(std::uint32_t ticket, torne_i2c_result& result,
                      std::uint32_t timeout_ms) noexcept
{
    const auto done = completed_.find(ticket);
    if (done != completed_.end()) {
        result = done->second;
        completed_.erase(done);
        return 0;
    }
    const Outstanding* item = find(ticket);
    if (item == nullptr) return TORNE_I2C_ERR_INVALID;
    const std::uint64_t target_ticket = item->ticket;
    const std::uint64_t start = clock_.monotonic_ms();
    while (true) {
        if (halted_ && target_ticket > halt_ticket_)
            return TORNE_I2C_ERR_HALTED;
        const int processed = process_progress();
        if (processed != 0) return processed;
        const auto completed = completed_.find(ticket);
        if (completed != completed_.end()) {
            result = completed->second;
            completed_.erase(completed);
            return 0;
        }
        if (clock_.monotonic_ms() - start >= timeout_ms)
            return timeout_result(timeout_ms, result);
        clock_.sleep_ms(1U);
    }
}

int RingBackend::flush(std::uint32_t timeout_ms,
                       torne_i2c_result& first_failure) noexcept
{
    const std::uint64_t start = clock_.monotonic_ms();
    bool saw_failure = false;
    torne_i2c_result failure{};
    if (outstanding_.empty()) {
        for (auto item = completed_.begin(); item != completed_.end(); ++item) {
            if (item->second.code != TORNE_I2C_OK) {
                first_failure = item->second;
                completed_.erase(item);
                return 0;
            }
        }
        completed_.clear();
        first_failure = torne_i2c_result{};
        first_failure.code = TORNE_I2C_OK;
        return 0;
    }
    while (!outstanding_.empty()) {
        const int processed = process_progress();
        if (processed != 0) return processed;
        if (has_halt_result_) {
            saw_failure = true;
            failure = halt_result_;
        }
        if (outstanding_.empty()) break;
        if (halted_) {
            if (has_halt_result_) first_failure = halt_result_;
            return TORNE_I2C_ERR_HALTED;
        }
        if (clock_.monotonic_ms() - start >= timeout_ms)
            return timeout_result(timeout_ms, first_failure);
        clock_.sleep_ms(1U);
    }
    completed_.clear();
    if (saw_failure) {
        first_failure = failure;
        return 0;
    }
    first_failure = torne_i2c_result{};
    first_failure.code = TORNE_I2C_OK;
    return 0;
}

int RingBackend::resume(bool discard) noexcept
{
    if (!halted_) return TORNE_I2C_ERR_INVALID;
    std::uint32_t serial = next_serial_++;
    if (next_serial_ == 0U || next_serial_ > 0xFFU) next_serial_ = 1U;
    if (serial == 0U) serial = 1U;
    const std::uint32_t value =
        (serial << 24U) | ((discard ? 2U : 1U) << 16U) | halt_wire_;
    const widen::WriteResult write = io_.write32(kResume, value, true);
    if (!write.ok) return write_error(write);

    const std::uint32_t old_next = (halt_wire_ + 1U) & 0xFFFFU;
    const std::uint64_t start = clock_.monotonic_ms();
    while (true) {
        const widen::ReadResult ack = io_.read32_wide(kResumeAck);
        if (!ack.ok) return read_error(ack);
        const widen::ReadResult progress = io_.read32_wide(kProgress);
        if (!progress.ok) return read_error(progress);
        if (ack.value == value) {
            const bool halted = (progress.value & 0x80000000U) != 0U;
            const std::uint32_t consumer = progress.value & 0xFFFFU;
            if (!halted) break;
            if (!discard && consumer != old_next) {
                // The original halt was cleared and a later published request
                // failed before the host observed the clear. Keep that new
                // halt for the next wait() instead of timing out on the old one.
                const std::uint32_t new_wire = (consumer - 1U) & 0xFFFFU;
                std::uint64_t new_ticket = halt_ticket_ + 1U;
                for (const Outstanding& item : outstanding_) {
                    if (static_cast<std::uint32_t>(
                            (base_ + item.ticket) & 0xFFFFU) == new_wire) {
                        new_ticket = item.ticket;
                        break;
                    }
                }
                halted_ = true;
                halt_ticket_ = new_ticket;
                halt_wire_ = new_wire;
                has_halt_result_ = false;
                return 0;
            }
        }
        if (clock_.monotonic_ms() - start >= kDeadlineMs)
            return TORNE_I2C_ERR_TIMEOUT;
        clock_.sleep_ms(1U);
    }
    if (discard) {
        discard_active_ = true;
        discard_from_ = halt_ticket_ + 1U;
        discard_until_ = t_pub_;
    }
    halted_ = false;
    has_halt_result_ = false;
    if (discard) {
        const int processed = process_progress();
        if (processed != 0) return processed;
    }
    return 0;
}

}  // namespace torne::core1
