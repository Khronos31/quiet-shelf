/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_w3u3_initialize.hpp"

#include "ground_io_common.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace asicen::px_w3u3::detail {

namespace {

using ::asicen::detail::ground_io::IoContext;

constexpr std::uint8_t ground_demodulator = 0x30;
constexpr std::uint8_t satellite_demodulator = 0x32;
constexpr std::uint8_t controller = 0x4a;

constexpr const char *response_length_diagnostic = "PX-W3U3 初期化I2C応答長が不正です";
constexpr const char *response_status_diagnostic = "PX-W3U3 初期化I2C statusが失敗を示しました";
constexpr const char *gpio_readback_diagnostic = "PX-W3U3 初期化GPIOの読み戻しが一致しません";

struct RegisterWrite {
    std::uint8_t reg;
    std::uint8_t value;
};

constexpr RegisterWrite ground_demodulator_initialization[] = {
    {0x04, 0x00}, {0x11, 0x1a}, {0x12, 0x04}, {0x13, 0x33}, {0x14, 0x20},
    {0x31, 0x00}, {0x32, 0x00}, {0x38, 0x00}, {0x39, 0xaa}, {0x47, 0x00},
    {0x75, 0x02}, {0xb0, 0xa0}, {0xb2, 0x3d}, {0xb3, 0x25}, {0xb4, 0x8b},
    {0xb5, 0x4b}, {0xb6, 0x3f}, {0xb7, 0xff}, {0xb8, 0xff}, {0x22, 0x8f},
    {0x5f, 0x80}, {0xef, 0x01},
};

constexpr RegisterWrite satellite_demodulator_initialization[] = {
    {0x01, 0x90}, {0x03, 0x00}, {0x04, 0x02}, {0x06, 0x00}, {0x07, 0x41},
    {0x08, 0x00}, {0x09, 0x00}, {0x0a, 0xff}, {0x0c, 0x59}, {0x0d, 0xf2},
    {0x0e, 0xf0}, {0x0f, 0x50}, {0x10, 0xb2}, {0x11, 0x00}, {0x12, 0x30},
    {0x13, 0x80}, {0x14, 0x00}, {0x15, 0x00}, {0x17, 0x00}, {0x1a, 0x00},
    {0x1b, 0x00}, {0x1c, 0x00}, {0x1d, 0x00}, {0x1e, 0x00}, {0x1f, 0x00},
    {0x20, 0x00}, {0x38, 0x40}, {0x39, 0x10}, {0x3b, 0x90}, {0x51, 0xb0},
    {0x52, 0x89}, {0x53, 0xb3}, {0x5a, 0x2d}, {0x5b, 0xd3}, {0x85, 0x69},
    {0x87, 0x04}, {0x8d, 0x00}, {0x8e, 0x00}, {0xa3, 0x11}, {0xa4, 0x00},
    {0xa5, 0x40}, {0xa6, 0x04},
};

constexpr std::uint8_t rf_tuner_initialization[] = {
    0x05, 0x10, 0x00, 0x00, 0x0f, 0x00, 0x00, 0xff, 0x6e, 0xb8, 0x82,
    0xf8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x10, 0x00, 0x04,
};

struct GpioWrite {
    std::uint8_t value;
    std::uint8_t mask;
};

constexpr GpioWrite power_sequence[] = {
    {0x05, 0x05}, {0x00, 0x04}, {0x05, 0x05}, {0x10, 0x10},
    {0x00, 0x10}, {0x10, 0x10}, {0x20, 0x20},
};

dtv_internal_result make_result(std::int32_t code, const char *diagnostic = nullptr)
{
    auto result = dtv_internal_result_make(code, 0);
    if (diagnostic != nullptr) {
        std::size_t length = 0;
        while (diagnostic[length] != '\0') ++length;
        dtv_internal_result_set_diagnostic(&result, diagnostic, length);
    }
    return result;
}

dtv_timeout caller_deadline(const IoContext &context)
{
    auto timeout = context.timeout;
    if (timeout.operation_deadline_ns == 0 && timeout.timeout_ms != 0) {
        const auto now = context.io->clock->monotonic_now_ns(context.io->clock->user);
        constexpr auto max = UINT64_MAX;
        timeout.operation_deadline_ns = timeout.timeout_ms > (max - now) / UINT64_C(1000000)
            ? max : now + timeout.timeout_ms * UINT64_C(1000000);
    }
    return timeout;
}

dtv_internal_result wait_until(const IoContext &context, std::uint64_t wake_at)
{
    const auto &clock = *context.io->clock;
    if (clock.delay_until == nullptr)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    const auto deadline = context.timeout.operation_deadline_ns == 0
        ? UINT64_MAX : context.timeout.operation_deadline_ns;
    while (clock.monotonic_now_ns(clock.user) < wake_at) {
        if (context.cancelled != nullptr && context.cancelled->load(std::memory_order_acquire))
            return make_result(DTV_INTERNAL_RESULT_CANCELLED);
        const auto now = clock.monotonic_now_ns(clock.user);
        const auto wake = std::min(wake_at, now + UINT64_C(1000000));
        std::uint32_t cancelled = 0;
        const auto result = clock.delay_until(clock.user, wake, deadline, nullptr, &cancelled);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result wait_ms(const IoContext &context, std::uint64_t milliseconds)
{
    const auto now = context.io->clock->monotonic_now_ns(context.io->clock->user);
    const auto duration = milliseconds * UINT64_C(1000000);
    const auto wake_at = duration > UINT64_MAX - now ? UINT64_MAX : now + duration;
    return wait_until(context, wake_at);
}

dtv_internal_result fail_and_restore(const IoContext &context, std::uint8_t snapshot,
                                      FunctionInitializationState *state,
                                      dtv_internal_result failure)
{
    const auto restore = ::asicen::detail::ground_io::write_gpio_verified(
        context, snapshot, 0xdf, snapshot, "PX-W3U3 GPIO復元応答長が不正です",
        "PX-W3U3 GPIOスナップショット復元が一致しません");
    state->cleanup_failed = restore.code != DTV_INTERNAL_RESULT_OK;
    return failure;
}

}

dtv_internal_result initialize_function(
    FunctionRole role, const dtv::spi::PublicOpenIo &open_io, dtv_timeout timeout,
    const std::atomic<bool> *cancelled, FunctionInitializationState *state)
{
    if ((role != FunctionRole::main && role != FunctionRole::sub) ||
        open_io.io == nullptr || open_io.io->clock == nullptr ||
        open_io.io->clock->monotonic_now_ns == nullptr ||
        open_io.io->clock->delay_until == nullptr || state == nullptr)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);

    if (state->cleanup_failed) return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    if (state->initialized) return make_result(DTV_INTERNAL_RESULT_OK);

    auto context = ::asicen::detail::ground_io::context_from_open(open_io, timeout, cancelled);
    context.timeout = caller_deadline(context);
    std::uint8_t snapshot = 0;
    auto result = ::asicen::detail::ground_io::read_gpio(
        context, &snapshot, "PX-W3U3 GPIOスナップショット応答長が不正です");
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    result = ::asicen::detail::ground_io::write_lnb_gpio_verified(
        context, 0x20, "PX-W3U3 LNB GPIO応答長が不正です",
        "PX-W3U3 LNB off GPIOの読み戻しが一致しません");
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);

    result = ::asicen::detail::ground_io::write_gpio_verified(
        context, 0x27, 0xbb, 0x27, "PX-W3U3 起動GPIO応答長が不正です",
        gpio_readback_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);

    for (const auto &write : power_sequence) {
        result = ::asicen::detail::ground_io::write_gpio_verified(
            context, write.value, write.mask, write.value,
            "PX-W3U3 電源GPIO応答長が不正です", gpio_readback_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK)
            return fail_and_restore(context, snapshot, state, result);
        result = wait_ms(context, 10);
        if (result.code != DTV_INTERNAL_RESULT_OK)
            return fail_and_restore(context, snapshot, state, result);
    }

    std::uint8_t value = 0;
    result = ::asicen::detail::ground_io::read_demodulator(
        context, 0xa8, 0x00, 0, &value, response_length_diagnostic,
        response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);
    result = wait_ms(context, 100);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);

    result = ::asicen::detail::ground_io::read_demodulator(
        context, ground_demodulator, 0x1c, 0, &value, response_length_diagnostic,
        response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);
    result = ::asicen::detail::ground_io::write_demodulator(
        context, ground_demodulator, 0x1c, static_cast<std::uint8_t>(value | 0x30),
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);
    result = wait_ms(context, 10);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);
    result = ::asicen::detail::ground_io::read_demodulator(
        context, ground_demodulator, 0x1c, 0, &value, response_length_diagnostic,
        response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);
    result = ::asicen::detail::ground_io::write_demodulator(
        context, ground_demodulator, 0x1c, static_cast<std::uint8_t>(value & 0xef),
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);

    result = ::asicen::detail::ground_io::read_demodulator(
        context, controller, 0x09, 0, &value, response_length_diagnostic,
        response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);
    if (((value & 0x3e) >> 1) != 0x0f) {
        result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED,
                             "PX-W3U3 コントローラ種別が非対応です");
        return fail_and_restore(context, snapshot, state, result);
    }
    result = ::asicen::detail::ground_io::read_demodulator(
        context, controller, 0x05, 0, &value, response_length_diagnostic,
        response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);
    if (value != 0) {
        result = make_result(DTV_INTERNAL_RESULT_BUSY, "PX-W3U3 コントローラが使用中です");
        return fail_and_restore(context, snapshot, state, result);
    }

    for (const auto &write : ground_demodulator_initialization) {
        result = ::asicen::detail::ground_io::write_demodulator(
            context, ground_demodulator, write.reg, write.value,
            response_length_diagnostic, response_status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK)
            return fail_and_restore(context, snapshot, state, result);
    }
    for (std::size_t i = 0; i < sizeof(rf_tuner_initialization); ++i) {
        result = ::asicen::detail::ground_io::write_rf_tuner(
            context, ground_demodulator, static_cast<std::uint8_t>(i + 1),
            rf_tuner_initialization[i], response_length_diagnostic,
            response_status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK)
            return fail_and_restore(context, snapshot, state, result);
    }
    for (const auto &write : satellite_demodulator_initialization) {
        result = ::asicen::detail::ground_io::write_demodulator(
            context, satellite_demodulator, write.reg, write.value,
            response_length_diagnostic, response_status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK)
            return fail_and_restore(context, snapshot, state, result);
    }
    result = ::asicen::detail::ground_io::write_demodulator(
        context, ground_demodulator, 0x0f, 0x34,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK)
        return fail_and_restore(context, snapshot, state, result);

    state->gpio_snapshot = snapshot;
    state->initialized = true;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

}
