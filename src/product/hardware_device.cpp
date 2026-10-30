// SPDX-License-Identifier: GPL-2.0-only
#include "product/device.h"

#include "product/packet_queue.h"

#include "core0/framer.h"
#include "core0/host_runtime.h"
#include "core0/ts_pump.h"
#include "card/uart.h"
#include "core1/host_runtime.h"
#include "core1/i2c_backend_cpp.h"
#include "dib/dib_frontend.h"
#include "fr30/generated/core0-images.h"
#include "fr30/generated/core1-images.h"
#include "fr30/generated/widen-images.h"
#include "usb/libusb_io.h"
#include "usb/transport.h"
#include "widen/bridge.h"
#include "widen/loader.h"
#include "widen/recognition.h"

#include <dtv/userland/detail/sha256.h>
#include <dtv/userland/error.h>
#include <dtv/userland/identity.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace torne::product {
namespace {

using dtv::userland::Error;
using dtv::userland::Result;

constexpr std::uint32_t kCore0Epoch = 0x00040001U;
constexpr std::size_t kQueueLimit = 1024U;

bool diag_enabled() noexcept
{
    const char* value = std::getenv("TORNE_DAEMON_DIAG");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}


class RuntimeClock final : public core1::Clock, public core0::Clock {
public:
    explicit RuntimeClock(torne::product::Clock& clock) noexcept : clock_(clock) {}
    std::uint64_t monotonic_ms() noexcept override
    {
        return clock_.monotonic_ms();
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        clock_.sleep_ms(milliseconds);
    }

private:
    torne::product::Clock& clock_;
};

class SystemClock final : public Clock {
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

class I2cFlushAdapter final : public core0::I2cFlush {
public:
    explicit I2cFlushAdapter(torne_i2c_backend& backend) noexcept
        : backend_(backend) {}
    bool flush() noexcept override
    {
        if (backend_.flush == nullptr) return true;
        torne_i2c_result first{};
        return backend_.flush(backend_.ctx, 1000U, &first) == 0 &&
               first.code == TORNE_I2C_OK;
    }

private:
    torne_i2c_backend& backend_;
};

Result<void> fail(Error error) noexcept
{
    return Result<void>::failure(error);
}

Error transfer_error(const widen::RecognitionResult& result) noexcept
{
    if (result.failure.cause == usb::TransferCause::no_device)
        return Error::DISCONNECTED;
    if (result.failure.cause == usb::TransferCause::timeout)
        return Error::TIMEOUT;
    return Error::USB_IO;
}

Error loader_error(widen::LoaderError error) noexcept
{
    switch (error) {
    case widen::LoaderError::functional_check_failed:
    case widen::LoaderError::readback_mismatch:
        return Error::PROTOCOL_ERROR;
    case widen::LoaderError::device_gone:
        return Error::DISCONNECTED;
    case widen::LoaderError::outcome_unknown:
    case widen::LoaderError::write_failed:
    case widen::LoaderError::read_failed:
        return Error::USB_IO;
    case widen::LoaderError::introduction_not_allowed:
    case widen::LoaderError::unknown_state:
    case widen::LoaderError::partial_state:
    case widen::LoaderError::missing_image:
    case widen::LoaderError::state_changed:
        return Error::UNSUPPORTED;
    default:
        return Error::INTERNAL;
    }
}

Error c_error(int rc) noexcept
{
    if (rc == 0) return Error::OK;
    if (rc == -EINVAL) return Error::INVALID_ARGUMENT;
    if (rc == -ETIMEDOUT) return Error::TIMEOUT;
    if (rc == -ENODEV) return Error::DISCONNECTED;
    if (rc == -ENOMEM) return Error::RESOURCE_LIMIT;
    if (rc == -EIO) return Error::USB_IO;
    return Error::PROTOCOL_ERROR;
}

widen::RegisteredImage widen_bank0() noexcept
{
    widen::RegisteredImage image;
    image.format = widen::ImageFormat::twdn;
    image.bank_base = TORNE_WIDEN_BANK0_BASE;
    image.image_bytes = static_cast<std::uint32_t>(sizeof(torne_widen_bank0_bytes));
    image.entry_read = TORNE_WIDEN_BANK0_ENTRY_READ;
    image.entry_write = TORNE_WIDEN_BANK0_ENTRY_WRITE;
    image.expected_image = torne_widen_bank0_bytes;
    return image;
}

template <typename Image>
Image make_image(const std::uint8_t* bytes, std::size_t length,
                 std::uint32_t build_id, std::uint32_t entry,
                 std::uint32_t capabilities)
{
    Image image;
    image.bytes = bytes;
    image.image_bytes = length;
    image.build_id = build_id;
    image.entry = entry;
    image.capabilities = capabilities;
    dtv::userland::detail::Sha256 digest;
    digest.update(bytes, length);
    image.sha256 = digest.finish();
    return image;
}

bool i2c_init(widen::Ep0Io& io) noexcept
{
    auto value = io.read32(0xFD8010U);
    if (!value.ok) return false;
    if (!io.write32(0xFD8010U, value.value | 0x10U, false).ok) return false;
    value = io.read32(0xFD8014U);
    if (!value.ok) return false;
    if (!io.write32(0xFD8014U, value.value & ~0x10U, false).ok) return false;
    if (!io.write32(0xFD80C0U, 1U, false).ok) return false;
    if (!io.write32(0xFD80C0U, 0U, false).ok) return false;
    if (!io.write32(0xFD80C4U, 0U, false).ok) return false;
    if (!io.write32(0x90114U, 0x80080000U, false).ok) return false;
    if (!io.write32(0xFD8FF8U, 6U, false).ok) return false;
    if (!io.write32(0xFD8FF4U, 8U, false).ok) return false;
    if (!io.write32(0xFD8FE8U, 0x3CU, false).ok) return false;
    return io.write32(0xFD8FE4U, 0x40U, false).ok;
}

bool write16_high(widen::Ep0Io& io, std::uint32_t address,
                  std::uint16_t value) noexcept
{
    const auto current = io.read32(address);
    if (!current.ok) return false;
    return io.write32(address,
                      (static_cast<std::uint32_t>(value) << 16U) |
                          (current.value & 0xFFFFU),
                      false).ok;
}

bool gpio_power_reset(widen::Ep0Io& io) noexcept
{
    const auto value = io.read32(0x90018U);
    const auto direction = io.read32(0x90014U);
    if (!value.ok || !direction.ok) return false;
    std::uint16_t v = static_cast<std::uint16_t>((value.value >> 16U) & 0xFFFFU);
    std::uint16_t d = static_cast<std::uint16_t>((direction.value >> 16U) & 0xFFFFU);
    v = static_cast<std::uint16_t>((v & ~1U) | (1U << 6U));
    d = static_cast<std::uint16_t>(d | 0x41U);
    if (!write16_high(io, 0x90018U, v)) return false;
    if (!write16_high(io, 0x90014U, d)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!write16_high(io, 0x90018U, static_cast<std::uint16_t>(v | 0x41U)))
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return true;
}

class CardClock final : public card::Clock {
public:
    explicit CardClock(torne::product::Clock& clock) noexcept : clock_(clock) {}
    std::uint64_t monotonic_ms() noexcept override
    {
        return clock_.monotonic_ms();
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        clock_.sleep_ms(milliseconds);
    }

private:
    torne::product::Clock& clock_;
};

class HardwareDevice final : public Device {
public:
    HardwareDevice(std::unique_ptr<usb::Transport> transport, Clock& clock) noexcept
        : transport_(std::move(transport)), clock_(clock), card_clock_(clock),
          card_uart_(*transport_, card_clock_), card_line_(card_uart_),
          bridge_(*transport_), runtime_clock_(clock), flush_(api_) {}
    HardwareDevice(std::unique_ptr<usb::Transport> transport,
                   std::unique_ptr<Clock> clock) noexcept
        : owned_clock_(std::move(clock)), transport_(std::move(transport)),
          clock_(*owned_clock_), card_clock_(*owned_clock_),
          card_uart_(*transport_, card_clock_), card_line_(card_uart_),
          bridge_(*transport_), runtime_clock_(*owned_clock_), flush_(api_) {}

    ~HardwareDevice() noexcept override { close(); }

    Result<void> startup() noexcept;

    Result<void> tune(std::uint64_t frequency_hz,
                      std::uint32_t timeout_ms) noexcept override;
    Result<void> start_stream() noexcept override;
    Result<void> stop_stream() noexcept override;
    Result<void> stop_capture() noexcept override;
    Result<void> read_packet(std::array<std::uint8_t, 188U>& packet,
                             std::uint32_t timeout_ms) noexcept override;
    Result<std::size_t> read_packets(std::uint8_t* output, std::size_t capacity,
                                     std::uint32_t timeout_ms) noexcept override;
    Result<Status> status() noexcept override;
    Result<StreamStats> stream_stats() noexcept override;
    CardPort card_port() noexcept override
    {
        return CardPort{&card_line_, &card_clock_};
    }
    void close() noexcept override;

private:
    static void sleep_cb(void* context, unsigned int ms) noexcept
    {
        static_cast<HardwareDevice*>(context)->clock_.sleep_ms(ms);
    }
    static std::uint64_t now_cb(void* context) noexcept
    {
        return static_cast<HardwareDevice*>(context)->clock_.monotonic_ms();
    }
    static void log_cb(void*, int, const char* message) noexcept
    {
        std::fprintf(stderr, "torne-dib: %s\n", message);
    }
    static void packet_sink(void* context, const std::uint8_t* data,
                            std::size_t length) noexcept
    {
        static_cast<HardwareDevice*>(context)->enqueue(data, length);
    }
    void enqueue(const std::uint8_t* data, std::size_t length) noexcept;
    void reset_device() noexcept;

    std::unique_ptr<Clock> owned_clock_;
    std::unique_ptr<usb::Transport> transport_;
    torne::product::Clock& clock_;
    CardClock card_clock_;
    card::Uart card_uart_;
    card::UartLine card_line_;
    widen::Bridge bridge_;
    RuntimeClock runtime_clock_;
    torne_i2c_backend api_{};
    I2cFlushAdapter flush_;
    std::unique_ptr<core1::RingBackend> i2c_;
    torne_dib_frontend dib_{};
    bool dib_initialized_ = false;
    core1::Runtime core1_;
    bool core1_started_ = false;
    core0::Framer framer_;
    core0::Image core0_image_{};
    std::unique_ptr<core0::TsPump> pump_;
    core0::Runtime core0_;
    bool core0_started_ = false;
    bool core0_start_attempted_ = false;
    std::atomic<std::uint64_t> stream_packets_{0U};
    std::atomic<std::uint64_t> stream_bytes_{0U};
    std::atomic<bool> diag_read_logged_{false};
    std::atomic<bool> diag_drop_logged_{false};
    std::uint32_t core1_session_ = 0U;
    PacketQueue queue_{kQueueLimit};
    bool closed_ = false;
};

Result<void> HardwareDevice::startup() noexcept
{
    const widen::RegisteredImage bank0 = widen_bank0();
    const widen::RecognitionResult recognition =
        widen::recognize_read_only(bridge_, &bank0, nullptr, 0U);
    if (!recognition.classified) return fail(transfer_error(recognition));

    if (recognition.state == widen::WidenState::pristine) {
        const widen::RegisteredImage images[] = {bank0};
        widen::IntroductionPolicy policy;
        policy.allow_bank_write = true;
        policy.allow_functional_check = false;
        const widen::LoaderResult installed = widen::introduce_widen(
            bridge_, &bank0, nullptr, 0U, images, 1U, policy);
        if (!installed.patches_applied)
            return fail(loader_error(installed.error));
    } else if (recognition.state != widen::WidenState::ours_active) {
        return fail(Error::UNSUPPORTED);
    }

    if (!i2c_init(bridge_)) return fail(Error::USB_IO);

    {
        const widen::RegisteredImage images[] = {bank0};
        widen::IntroductionPolicy policy;
        policy.allow_bank_write = false;
        policy.allow_functional_check = true;
        const widen::LoaderResult verified = widen::introduce_widen(
            bridge_, &bank0, nullptr, 0U, images, 1U, policy);
        if (!verified.ok)
            return fail(loader_error(verified.error));
    }

    const core1::Image core1_image = make_image<core1::Image>(
        torne_core1_bytes, sizeof(torne_core1_bytes), TORNE_CORE1_BUILD_ID,
        TORNE_CORE1_ENTRY, 1U);
    const core1::Result core1_result =
        core1_.start(bridge_, core1_image, true, runtime_clock_);
    if (!core1_result.ok) return fail(Error::PROTOCOL_ERROR);
    core1_started_ = true;
    core1_session_ = core1_result.session;

    if (!gpio_power_reset(bridge_)) return fail(Error::USB_IO);

    i2c_ = core1::RingBackend::create(bridge_, core1_session_, runtime_clock_);
    if (!i2c_) return fail(Error::INTERNAL);
    i2c_->fill_c_api(api_);

    torne_dib_frontend_config config{};
    config.backend = &api_;
    config.i2c_timeout_ms = 1000U;
    config.core1_and_gpio_ready = true;
    config.sleep = sleep_cb;
    config.now = now_cb;
    config.log = log_cb;
    config.priv = this;
    const int dib_rc = torne_dib_frontend_init(&dib_, &config);
    if (dib_rc != 0) return fail(c_error(dib_rc));

    core0_image_ = make_image<core0::Image>(
        torne_core0_bytes, sizeof(torne_core0_bytes), TORNE_CORE0_BUILD_ID,
        TORNE_CORE0_ENTRY, 1U);
    dib_initialized_ = true;
    return Result<void>::success();
}

Result<void> HardwareDevice::tune(std::uint64_t frequency_hz,
                                  std::uint32_t timeout_ms) noexcept
{
    const int tuned = torne_dib_frontend_tune_timeout(
        &dib_, static_cast<std::uint32_t>(frequency_hz), timeout_ms);
    if (tuned != 0) return fail(c_error(tuned));
    struct torne_dib_status status_value{};
    const int read = torne_dib_frontend_status(&dib_, &status_value);
    if (read != 0) return fail(c_error(read));
    if (!status_value.locked) return fail(Error::NOT_READY);
    return Result<void>::success();
}

Result<void> HardwareDevice::start_stream() noexcept
{
    if (core0_started_) return Result<void>::success();
    // A previous attempt may have reached RUN/TRIGGER before its response was
    // lost. Require an explicit stop before starting again so the owner retries
    // cleanup instead of silently starting a second Core0 epoch.
    if (core0_start_attempted_) return fail(Error::BUSY);
    core0_start_attempted_ = true;
    stream_packets_.store(0U);
    stream_bytes_.store(0U);
    queue_.reset();
    pump_.reset(new core0::TsPump(*transport_, framer_, runtime_clock_, 16U,
                                  65536U, packet_sink, this));
    const core0::Result started = core0_.start(
        bridge_, *transport_, core0_image_, kCore0Epoch, true, flush_,
        runtime_clock_, *pump_);
    if (!started.ok) {
        if (diag_enabled())
            std::fprintf(stderr,
                "torne-diag hw start error stage=%u error=%u input_open=%u active=%zu\n",
                static_cast<unsigned>(started.stage),
                static_cast<unsigned>(started.error),
                static_cast<unsigned>(started.input_open),
                pump_ ? pump_->active() : 0U);
        return fail(Error::PROTOCOL_ERROR);
    }
    core0_started_ = true;
    return Result<void>::success();
}

Result<void> HardwareDevice::stop_capture() noexcept
{
    if (!core0_started_ && !core0_start_attempted_)
        return Result<void>::success();
    const core0::Result stopped = core0_.stop(bridge_, *pump_);
    if (diag_enabled())
        std::fprintf(stderr,
            "torne-diag hw stop_capture ok=%u stage=%u error=%u active=%zu running=%u\n",
            static_cast<unsigned>(stopped.ok),
            static_cast<unsigned>(stopped.stage),
            static_cast<unsigned>(stopped.error),
            pump_ ? pump_->active() : 0U,
            static_cast<unsigned>(pump_ && pump_->running()));
    if (!stopped.ok) return fail(Error::USB_IO);
    core0_started_ = false;
    core0_start_attempted_ = false;
    return Result<void>::success();
}

Result<void> HardwareDevice::stop_stream() noexcept
{
    Result<void> result = stop_capture();
    const auto retain = [&result](Error error) noexcept {
        if (result) result = fail(error);
    };
    if (dib_initialized_) {
        const int slept = torne_dib_frontend_sleep(&dib_);
        if (diag_enabled())
            std::fprintf(stderr, "torne-diag hw dib_sleep rc=%d\n", slept);
        if (slept == 0) {
            dib_initialized_ = false;
        } else {
            retain(c_error(slept));
        }
    }
    if (core1_started_) {
        const core1::Result stopped = core1_.stop(bridge_);
        if (diag_enabled())
            std::fprintf(stderr,
                "torne-diag hw core1_stop ok=%u stage=%u error=%u\n",
                static_cast<unsigned>(stopped.ok),
                static_cast<unsigned>(stopped.stage),
                static_cast<unsigned>(stopped.error));
        if (stopped.ok) {
            core1_started_ = false;
        } else {
            retain(Error::PROTOCOL_ERROR);
        }
    }
    return result;
}

Result<void> HardwareDevice::read_packet(
    std::array<std::uint8_t, 188U>& packet, std::uint32_t timeout_ms) noexcept
{
    if (!core0_started_ || !pump_) return fail(Error::NOT_READY);
    const std::uint64_t start = clock_.monotonic_ms();
    for (;;) {
        if (pump_->pump(1U) < 0) {
            if (diag_enabled() && !diag_read_logged_.exchange(true))
                std::fprintf(stderr,
                    "torne-diag hw read error=USB_IO dropped=%llu packets=%llu bytes=%llu queue=%zu\n",
                    static_cast<unsigned long long>(queue_.dropped()),
                    static_cast<unsigned long long>(stream_packets_.load()),
                    static_cast<unsigned long long>(stream_bytes_.load()),
                    queue_.size());
            return fail(Error::USB_IO);
        }
        if (queue_.dropped() != 0U && diag_enabled() &&
            !diag_drop_logged_.exchange(true)) {
            std::fprintf(stderr,
                "torne-diag hw queue_drop dropped=%llu packets=%llu bytes=%llu queue=%zu\n",
                static_cast<unsigned long long>(queue_.dropped()),
                static_cast<unsigned long long>(stream_packets_.load()),
                static_cast<unsigned long long>(stream_bytes_.load()),
                queue_.size());
        }
        if (queue_.pop(packet)) return Result<void>::success();
        if (clock_.monotonic_ms() - start >= timeout_ms)
            return fail(Error::TIMEOUT);
        clock_.sleep_ms(1U);
    }
}

Result<std::size_t> HardwareDevice::read_packets(
    std::uint8_t* output, std::size_t capacity,
    std::uint32_t timeout_ms) noexcept
{
    if (!core0_started_ || !pump_)
        return Result<std::size_t>::failure(Error::NOT_READY);
    if (output == nullptr || capacity < 188U)
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    const std::size_t usable = capacity - (capacity % 188U);
    const std::uint64_t start = clock_.monotonic_ms();
    std::size_t bytes = 0U;
    for (;;) {
        if (pump_->pump(1U) < 0) {
            if (bytes != 0U) return Result<std::size_t>::success(bytes);
            if (diag_enabled() && !diag_read_logged_.exchange(true))
                std::fprintf(stderr,
                    "torne-diag hw read error=USB_IO dropped=%llu packets=%llu bytes=%llu queue=%zu\n",
                    static_cast<unsigned long long>(queue_.dropped()),
                    static_cast<unsigned long long>(stream_packets_.load()),
                    static_cast<unsigned long long>(stream_bytes_.load()),
                    queue_.size());
            return Result<std::size_t>::failure(Error::USB_IO);
        }
        if (queue_.dropped() != 0U && diag_enabled() &&
            !diag_drop_logged_.exchange(true)) {
            std::fprintf(stderr,
                "torne-diag hw queue_drop dropped=%llu packets=%llu bytes=%llu queue=%zu\n",
                static_cast<unsigned long long>(queue_.dropped()),
                static_cast<unsigned long long>(stream_packets_.load()),
                static_cast<unsigned long long>(stream_bytes_.load()),
                queue_.size());
        }
        while (bytes + 188U <= usable) {
            std::array<std::uint8_t, 188U> packet{};
            if (!queue_.pop(packet)) break;
            std::memcpy(output + bytes, packet.data(), packet.size());
            bytes += packet.size();
        }
        if (bytes != 0U) return Result<std::size_t>::success(bytes);
        if (clock_.monotonic_ms() - start >= timeout_ms)
            return Result<std::size_t>::failure(Error::TIMEOUT);
        clock_.sleep_ms(1U);
    }
}

Result<StreamStats> HardwareDevice::stream_stats() noexcept
{
    StreamStats stats;
    stats.packets = stream_packets_.load();
    stats.bytes = stream_bytes_.load();
    stats.dropped = queue_.dropped();
    return Result<StreamStats>::success(stats);
}

Result<Status> HardwareDevice::status() noexcept
{
    struct torne_dib_status status_value{};
    const int read = torne_dib_frontend_status(&dib_, &status_value);
    if (read != 0) return Result<Status>::failure(c_error(read));
    Status result;
    result.locked = status_value.locked;
    result.fe_status = status_value.status;
    result.has_cnr = status_value.has_cnr;
    result.cnr_x100 = status_value.cnr_x100;
    result.has_strength = status_value.has_strength;
    result.strength_dbm_x1000 = status_value.strength_dbm_x1000;
    return Result<Status>::success(result);
}

void HardwareDevice::enqueue(const std::uint8_t* data,
                             std::size_t length) noexcept
{
    if (data == nullptr || length != 188U) return;
    queue_.push(data, length);
    stream_packets_.fetch_add(1U);
    stream_bytes_.fetch_add(188U);
}

void HardwareDevice::reset_device() noexcept
{
    if (core0_started_ || core0_start_attempted_) {
        (void)core0_.stop(bridge_, *pump_);
        core0_started_ = false;
        core0_start_attempted_ = false;
    }
    pump_.reset();
    if (core1_started_) {
        (void)core1_.stop(bridge_);
        core1_started_ = false;
    }
    i2c_.reset();
    dib_initialized_ = false;
    card_line_.invalidate();
    transport_.reset();
}

void HardwareDevice::close() noexcept
{
    if (closed_) return;
    closed_ = true;
    (void)stop_stream();
    reset_device();
}

}  // namespace

Result<std::unique_ptr<Device>> open_hardware(
    const std::string& usb_selector, Clock& clock) noexcept
{
    const auto selector =
        dtv::userland::identity::parse_device_selector(usb_selector);
    if (!selector)
        return Result<std::unique_ptr<Device>>::failure(Error::INVALID_ARGUMENT);
    return open_hardware(selector.value(), clock);
}

Result<std::unique_ptr<Device>> open_hardware(
    const dtv::userland::identity::Selector& selector, Clock& clock) noexcept
{
    if (selector.kind != dtv::userland::identity::SelectorKind::usb_topology)
        return Result<std::unique_ptr<Device>>::failure(Error::INVALID_ARGUMENT);
    usb::DeviceLocation location;
    if (!usb::location_from_selector(selector, location))
        return Result<std::unique_ptr<Device>>::failure(Error::INVALID_ARGUMENT);
    auto opened = usb::Transport::open(usb::create_libusb_io(), location);
    if (opened.outcome.state != usb::OpenState::ok || !opened.transport)
        return Result<std::unique_ptr<Device>>::failure(Error::USB_IO);

    std::unique_ptr<HardwareDevice> device(
        new HardwareDevice(std::move(opened.transport), clock));
    const Result<void> started = device->startup();
    if (!started) {
        device->close();
        return Result<std::unique_ptr<Device>>::failure(started.error());
    }
    return Result<std::unique_ptr<Device>>::success(std::move(device));
}

Result<std::unique_ptr<Device>> open_hardware(
    const dtv::userland::identity::Selector& selector) noexcept
{
    if (selector.kind != dtv::userland::identity::SelectorKind::usb_topology)
        return Result<std::unique_ptr<Device>>::failure(Error::INVALID_ARGUMENT);
    usb::DeviceLocation location;
    if (!usb::location_from_selector(selector, location))
        return Result<std::unique_ptr<Device>>::failure(Error::INVALID_ARGUMENT);
    auto opened = usb::Transport::open(usb::create_libusb_io(), location);
    if (opened.outcome.state != usb::OpenState::ok || !opened.transport)
        return Result<std::unique_ptr<Device>>::failure(Error::USB_IO);

    std::unique_ptr<Clock> clock(new SystemClock());
    std::unique_ptr<HardwareDevice> device(
        new HardwareDevice(std::move(opened.transport), std::move(clock)));
    const Result<void> started = device->startup();
    if (!started) {
        device->close();
        return Result<std::unique_ptr<Device>>::failure(started.error());
    }
    return Result<std::unique_ptr<Device>>::success(std::move(device));
}

}  // namespace torne::product
