// SPDX-License-Identifier: GPL-2.0-only
#include "product/core_family.h"

#include "card/service.h"

#include <dtv/userland/detail/stream_session_types.h>
#include <dtv/userland/host_card_t1/card_protocol_session.h>
#include <dtv/userland/error.h>
#include <dtv/userland/transport.h>
#include <dtv/userland/wire.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace torne::product {
namespace {

using dtv::userland::Error;
using dtv::userland::Result;
using px4::userland::TunerAttachment;
using px4::userland::TunerHzProgramming;
using px4::userland::TunerServiceBackend;
using px4::userland::TunerServiceTime;
using px4::userland::TunerStrictProfile;
using px4::userland::TunerStreamControl;
using px4::userland::TunerStreamCounters;
using px4::userland::TunerStreamFinalSnapshot;
using px4::userland::TunerStreamReadResult;
using px4::userland::TunerStreamTerminal;

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
};

class Tuner final : public TunerServiceBackend {
public:
    Tuner(Device& device, std::mutex& io_mutex,
          std::atomic<bool>& capture_stopping) noexcept
        : device_(device), io_mutex_(io_mutex),
          capture_stopping_(capture_stopping) {}

    std::uint8_t receiver_count() const noexcept override { return 1U; }
    bool receiver_supports(std::uint8_t receiver,
                           px4::userland::ipc::System system) const noexcept override
    {
        return receiver == 0U &&
               system == px4::userland::ipc::System::ISDB_T;
    }

    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        return receiver == 0U ? Result<void>::success()
                              : Result<void>::failure(Error::NOT_FOUND);
    }
    Result<void> tune_terrestrial(std::uint8_t receiver,
                                  std::uint32_t frequency_khz,
                                  std::uint32_t timeout_ms) noexcept override
    {
        if (receiver != 0U) return Result<void>::failure(Error::NOT_FOUND);
        return device_.tune(static_cast<std::uint64_t>(frequency_khz) * 1000U,
                            timeout_ms);
    }
    Result<TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t receiver, std::uint64_t frequency_hz,
        std::uint32_t timeout_ms) noexcept override
    {
        if (receiver != 0U)
            return Result<TunerHzProgramming>::failure(Error::NOT_FOUND);
        const Result<void> tuned = device_.tune(frequency_hz, timeout_ms);
        if (!tuned) return Result<TunerHzProgramming>::failure(tuned.error());
        TunerHzProgramming programming{};
        programming.programmed_hz = frequency_hz;
        programming.lock.state = dtv::userland::wire::MeasureState::valid;
        programming.lock.value = true;
        programming.ts_synchronized = true;
        return Result<TunerHzProgramming>::success(programming);
    }
    TunerStrictProfile strict_profile(
        std::uint8_t receiver,
        px4::userland::ipc::System system) const noexcept override
    {
        TunerStrictProfile profile;
        if (receiver != 0U ||
            system != px4::userland::ipc::System::ISDB_T)
            return profile;
        profile.native_hz = true;
        profile.readiness =
            dtv::userland::detail::TuneReadiness::terminal_lock;
        profile.min_hz = 45000000U;
        profile.max_hz = 860000000U;
        return profile;
    }
    Result<bool> is_locked(std::uint8_t receiver,
                           px4::userland::ipc::System system) noexcept override
    {
        if (receiver != 0U ||
            system != px4::userland::ipc::System::ISDB_T)
            return Result<bool>::failure(Error::NOT_FOUND);
        const auto status = device_.status();
        if (!status) return Result<bool>::failure(status.error());
        return Result<bool>::success(status.value().locked);
    }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        return receiver == 0U ? Result<void>::success()
                              : Result<void>::failure(Error::NOT_FOUND);
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> start_capture(std::uint8_t receiver,
                               px4::userland::ipc::System system) noexcept override
    {
        if (receiver != 0U ||
            system != px4::userland::ipc::System::ISDB_T)
            return Result<void>::failure(Error::NOT_FOUND);
        std::lock_guard<std::mutex> lock(io_mutex_);
        const auto started = device_.start_stream();
        if (started) capture_stopping_.store(false);
        return started;
    }
    Result<void> stop_capture(std::uint8_t receiver,
                              px4::userland::ipc::System system) noexcept override
    {
        if (receiver != 0U ||
            system != px4::userland::ipc::System::ISDB_T)
            return Result<void>::failure(Error::NOT_FOUND);
        std::lock_guard<std::mutex> lock(io_mutex_);
        const auto stopped = device_.stop_capture();
        if (stopped) capture_stopping_.store(true);
        return stopped;
    }

private:
    Device& device_;
    std::mutex& io_mutex_;
    std::atomic<bool>& capture_stopping_;
};

TunerStreamCounters empty_counters() noexcept
{
    TunerStreamCounters counters;
    counters.availability.fill(dtv::userland::wire::MeasureState::unknown);
    // Only these counters are measured by the product stream path. Sync/TEI/
    // continuity remain unknown rather than being reported as zero.
    counters.availability[0U] = dtv::userland::wire::MeasureState::valid;
    counters.availability[1U] = dtv::userland::wire::MeasureState::valid;
    counters.availability[5U] = dtv::userland::wire::MeasureState::valid;
    counters.availability[6U] = dtv::userland::wire::MeasureState::valid;
    counters.availability[7U] = dtv::userland::wire::MeasureState::valid;
    return counters;
}

bool diag_enabled() noexcept
{
    const char* value = std::getenv("TORNE_DAEMON_DIAG");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool same_attachment(const TunerAttachment& left,
                     const TunerAttachment& right) noexcept
{
    return left.owner_client_id == right.owner_client_id &&
           left.lease_id == right.lease_id &&
           left.attachment_id == right.attachment_id &&
           left.receiver == right.receiver &&
           left.system == right.system &&
           left.nonce == right.nonce;
}

class Stream final : public TunerStreamControl {
public:
    Stream(Device& device, std::mutex& io_mutex,
           std::atomic<bool>& capture_stopping) noexcept
        : device_(device), io_mutex_(io_mutex),
          capture_stopping_(capture_stopping) {}

    Result<void> attach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (attached_) return Result<void>::failure(Error::BUSY);
        if (attachment.receiver != 0U ||
            attachment.system != px4::userland::ipc::System::ISDB_T)
            return Result<void>::failure(Error::NOT_FOUND);
        attachment_ = attachment;
        attached_ = true;
        counters_ = empty_counters();
        final_valid_ = false;
        return Result<void>::success();
    }
    Result<void> detach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!attached_ || !same_attachment(attachment_, attachment))
            return Result<void>::failure(Error::NOT_FOUND);
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        const Result<void> stopped = device_.stop_capture();
        if (const auto usage = device_.stream_stats())
            counters_.queue_drops = usage.value().dropped;
        final_ = TunerStreamFinalSnapshot{counters_,
            static_cast<std::uint8_t>(stopped
                ? TunerStreamTerminal::stopped
                : TunerStreamTerminal::usb_error)};
        final_valid_ = true;
        attached_ = false;
        if (diag_enabled())
            std::fprintf(stderr,
                "torne-diag stream detach stop=%u final_valid=%u terminal=%u packets=%llu bytes=%llu drops=%llu\n",
                static_cast<unsigned>(stopped.error()),
                static_cast<unsigned>(final_valid_),
                static_cast<unsigned>(final_.terminal),
                static_cast<unsigned long long>(final_.counters.packets),
                static_cast<unsigned long long>(final_.counters.bytes),
                static_cast<unsigned long long>(final_.counters.queue_drops));
        return Result<void>::success();
    }
    Result<TunerStreamCounters> stats(
        const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (attached_ && same_attachment(attachment_, attachment)) {
            if (const auto usage = device_.stream_stats())
                counters_.queue_drops = usage.value().dropped;
            return Result<TunerStreamCounters>::success(counters_);
        }
        if (final_valid_ && same_attachment(attachment_, attachment))
            return Result<TunerStreamCounters>::success(final_.counters);
        return Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
    }
    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (final_valid_ && same_attachment(attachment_, attachment))
            return Result<TunerStreamFinalSnapshot>::success(final_);
        return Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    }
    Result<void> release_final(
        const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!final_valid_ || !same_attachment(attachment_, attachment))
            return Result<void>::failure(Error::NOT_FOUND);
        final_valid_ = false;
        return Result<void>::success();
    }
    Result<TunerStreamReadResult> read(
        const TunerAttachment& attachment,
        dtv::userland::MutableByteView output,
        dtv::userland::Timeout timeout) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!same_attachment(attachment_, attachment))
            return Result<TunerStreamReadResult>::failure(Error::NOT_FOUND);
        if (!attached_) {
            TunerStreamReadResult result;
            result.eof = true;
            result.terminal = TunerStreamTerminal::stopped;
            return Result<TunerStreamReadResult>::success(result);
        }
        if (output.size < 188U)
            return Result<TunerStreamReadResult>::failure(Error::BUFFER_TOO_SMALL);
        TunerStreamReadResult result;
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        const auto read = device_.read_packets(
            output.data, output.size, timeout.milliseconds);
        if (!read) {
            if (read.error() == Error::TIMEOUT) {
                result.timed_out = true;
                ++counters_.empty_intervals;
                return Result<TunerStreamReadResult>::success(result);
            }
            if (read.error() == Error::NOT_READY && capture_stopping_.load()) {
                result.eof = true;
                result.terminal = TunerStreamTerminal::stopped;
                return Result<TunerStreamReadResult>::success(result);
            }
            ++counters_.usb_errors;
            if (diag_enabled() && !diag_read_error_logged_) {
                diag_read_error_logged_ = true;
                std::fprintf(stderr,
                    "torne-diag stream read error=%u packets=%llu bytes=%llu drops=%llu\n",
                    static_cast<unsigned>(read.error()),
                    static_cast<unsigned long long>(counters_.packets),
                    static_cast<unsigned long long>(counters_.bytes),
                    static_cast<unsigned long long>(counters_.queue_drops));
            }
            return Result<TunerStreamReadResult>::failure(read.error());
        }
        result.bytes = read.value();
        counters_.packets += read.value() / 188U;
        counters_.bytes += read.value();
        if (const auto usage = device_.stream_stats()) {
            counters_.queue_drops = usage.value().dropped;
            result.cumulative_queue_dropped_packets =
                counters_.queue_drops;
        }
        return Result<TunerStreamReadResult>::success(result);
    }
    Result<TunerStreamTerminal> terminal(
        const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!same_attachment(attachment_, attachment))
            return Result<TunerStreamTerminal>::failure(Error::NOT_FOUND);
        return Result<TunerStreamTerminal>::success(
            attached_ ? TunerStreamTerminal::none
                      : TunerStreamTerminal::stopped);
    }

private:
    Device& device_;
    std::mutex& io_mutex_;
    std::atomic<bool>& capture_stopping_;
    mutable std::mutex mutex_;
    TunerAttachment attachment_{};
    bool attached_ = false;
    bool final_valid_ = false;
    bool diag_read_error_logged_ = false;
    mutable TunerStreamCounters counters_{};
    TunerStreamFinalSnapshot final_{};
};

class Family final : public dtv::userland::host_runtime::EnclosureFamily {
public:
    explicit Family(std::unique_ptr<Device> device) noexcept
        : device_(std::move(device)),
          card_line_(device_->card_port().line),
          card_time_(device_->card_port().clock),
          card_hardware_(card_line_),
          card_session_(card_hardware_, card_time_),
          card_backend_(card_line_),
          card_protocol_(card_session_),
          tuner_(*device_, io_mutex_, capture_stopping_),
          stream_(*device_, io_mutex_, capture_stopping_) {}

    Result<void> shutdown() noexcept override
    {
        card_backend_.request_stop();
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (device_) device_->close();
        return Result<void>::success();
    }
    TunerServiceBackend& tuner_backend() noexcept override { return tuner_; }
    px4::userland::CardServiceBackend& card_backend() noexcept override
    {
        return card_backend_;
    }
    px4::userland::CardProtocolSession& card_protocol() noexcept override
    {
        return card_protocol_;
    }
    TunerServiceTime& time() noexcept override { return time_; }
    TunerStreamControl* stream_control() noexcept override { return &stream_; }

private:
    std::unique_ptr<Device> device_;
    card::Line* card_line_;
    card::CoreTime card_time_;
    card::Hardware card_hardware_;
    dtv::userland::host_card_t1::CardSession card_session_;
    card::Backend card_backend_;
    dtv::userland::host_card_t1::CardProtocolSessionAdapter card_protocol_;
    std::mutex io_mutex_;
    std::atomic<bool> capture_stopping_{false};
    Tuner tuner_;
    Stream stream_;
    Time time_;
};

}  // namespace

std::unique_ptr<dtv::userland::host_runtime::EnclosureFamily> make_family(
    std::unique_ptr<Device> device) noexcept
{
    if (!device) return nullptr;
    return std::unique_ptr<dtv::userland::host_runtime::EnclosureFamily>(
        new Family(std::move(device)));
}

}  // namespace torne::product
