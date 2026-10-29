// SPDX-License-Identifier: GPL-2.0-only
#include <torne/torne_device.h>

#include <cerrno>
#include <climits>
#include <algorithm>

namespace torne {
namespace {
using dtv::userland::Error;
using dtv::userland::Result;
}

Error map_driver_error(int code) noexcept
{
    if (code >= 0) return Error::OK;
    switch (-code) {
    case ETIMEDOUT: return Error::TIMEOUT;
    case ENODEV: return Error::DISCONNECTED;
    case EBUSY: return Error::BUSY;
    case ENXIO: return Error::NOT_FOUND;
    case EINVAL: return Error::INVALID_ARGUMENT;
    case EPIPE: return Error::PROTOCOL_ERROR;
    default: return Error::USB_IO;
    }
}

Device::Device(const struct torne_ops& callbacks, void* context) noexcept
    : callbacks_(callbacks), context_(context)
{
    torne_bridge_init(&bridge_, &callbacks_, context_);
    torne_dib_setup(&demod_, &bridge_);
    torne_framer_reset(&framer_);
}

Device::~Device() noexcept
{
    // Ownership remains with the USB adapter. Shutdown is idempotent.
    (void)shutdown();
}

Result<void> Device::initialize() noexcept
{
    if (initialized_) return Result<void>::failure(Error::BUSY);
    if (!callbacks_.control || !callbacks_.bulk || !callbacks_.msleep || !callbacks_.now_ms)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    int ret = torne_bridge_start(&bridge_);
    if (!ret) ret = torne_bridge_reset_demodulator(&bridge_);
    if (!ret) ret = torne_dib_init(&demod_);
    if (ret) return Result<void>::failure(map_driver_error(ret));
    initialized_ = true;
    return Result<void>::success();
}

Result<void> Device::tune(std::uint32_t frequency_hz) noexcept
{
    if (!initialized_) return Result<void>::failure(Error::NOT_READY);
    if (streaming_) return Result<void>::failure(Error::BUSY);
    tuned_ = false;
    const int prepare_error = torne_bridge_prepare_stream(&bridge_);
    if (prepare_error) return Result<void>::failure(map_driver_error(prepare_error));
    const int ret = torne_dib_tune(&demod_, frequency_hz);
    if (ret) return Result<void>::failure(map_driver_error(ret));
    tuned_ = true;
    return Result<void>::success();
}

Result<struct torne_dib_status> Device::status() noexcept
{
    if (!initialized_) return Result<struct torne_dib_status>::failure(Error::NOT_READY);
    struct torne_dib_status status{};
    const int ret = torne_dib_status(&demod_, &status);
    if (ret) return Result<struct torne_dib_status>::failure(map_driver_error(ret));
    return Result<struct torne_dib_status>::success(status);
}

Result<void> Device::start_stream() noexcept
{
    if (!initialized_ || !tuned_) return Result<void>::failure(Error::NOT_READY);
    if (streaming_) return Result<void>::failure(Error::BUSY);
    int ret = torne_dib_set_output(&demod_, TORNE_OUT_PARALLEL_GATED);
    if (!ret) ret = torne_bridge_start_stream(&bridge_);
    if (ret) {
        (void)torne_dib_set_output(&demod_, TORNE_OUT_HIGHZ);
        return Result<void>::failure(map_driver_error(ret));
    }
    torne_framer_reset(&framer_);
    streaming_ = true;
    return Result<void>::success();
}

Result<void> Device::stop_stream() noexcept
{
    if (!streaming_) return Result<void>::success();
    const int ret = torne_bridge_stop_stream(&bridge_);
    const int output_error = torne_dib_set_output(&demod_, TORNE_OUT_HIGHZ);
    streaming_ = false;
    return ret || output_error
        ? Result<void>::failure(map_driver_error(ret ? ret : output_error))
        : Result<void>::success();
}

Result<std::size_t> Device::read_ts(dtv::userland::MutableByteView output,
                                    std::uint32_t timeout_ms) noexcept
{
    if (!streaming_) return Result<std::size_t>::failure(Error::NOT_READY);
    if (!output.data || output.size < 376U)
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    // Keep enough room for an incomplete record from the previous read.
    const auto packets = output.size / 188U;
    const std::size_t requested =
        std::min(raw_.size(), (packets - 1U) * 192U);
    if (requested == 0U || requested > static_cast<std::size_t>(INT_MAX))
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    const int got = callbacks_.bulk(context_, 0x81, raw_.data(),
                                    static_cast<int>(requested), timeout_ms);
    if (got < 0) return Result<std::size_t>::failure(map_driver_error(got));
    if (static_cast<std::size_t>(got) > requested)
        return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
    const auto n = torne_framer_feed(&framer_, raw_.data(),
                                    static_cast<std::size_t>(got), output.data);
    if (n > output.size || (n % 188U) != 0U)
        return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
    return Result<std::size_t>::success(n);
}

Result<void> Device::shutdown() noexcept
{
    const auto stopped = stop_stream();
    if (!initialized_) return stopped;
    const int ret = torne_dib_sleep(&demod_);
    initialized_ = false;
    tuned_ = false;
    if (!stopped) return stopped;
    return ret ? Result<void>::failure(map_driver_error(ret))
               : Result<void>::success();
}

} // namespace torne
