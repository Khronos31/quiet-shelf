/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_w3u2_ground_io.hpp"

#include "ground_io_common.hpp"
#include "px_w3u2_tune.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace asicen::px_w3u2::detail {

namespace {

constexpr std::uint8_t ground_demodulator = 0x30;
constexpr const char *response_length_diagnostic = "PX-W3U2 I2C応答長が不正です";
constexpr const char *response_status_diagnostic = "PX-W3U2 I2C statusが失敗を示しました";

constexpr DemodulatorWrite rf_tuner_initialization[] = {
    {0, 0x01, 0x05}, {0, 0x02, 0x10}, {0, 0x03, 0x00}, {0, 0x04, 0x00},
    {0, 0x05, 0x0f}, {0, 0x06, 0x00}, {0, 0x07, 0x00}, {0, 0x08, 0xff},
    {0, 0x09, 0x6e}, {0, 0x0a, 0xb8}, {0, 0x0b, 0x82}, {0, 0x0c, 0xf8},
    {0, 0x0d, 0x02}, {0, 0x0e, 0x00}, {0, 0x0f, 0x00}, {0, 0x10, 0x00},
    {0, 0x11, 0x00}, {0, 0x12, 0x1b}, {0, 0x13, 0x10}, {0, 0x14, 0x00},
    {0, 0x15, 0x04},
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

dtv::spi::PublicTuneRequest caller_deadline_request(
    const dtv::spi::PublicTuneRequest &request, const dtv_runtime_clock &clock)
{
    auto timed_request = request;
    if (timed_request.timeout.operation_deadline_ns == 0 &&
        timed_request.timeout.timeout_ms != 0) {
        const auto now = clock.monotonic_now_ns(clock.user);
        constexpr auto max = UINT64_MAX;
        const auto timeout_ms = timed_request.timeout.timeout_ms;
        timed_request.timeout.operation_deadline_ns =
            timeout_ms > (max - now) / UINT64_C(1000000)
                ? max : now + timeout_ms * UINT64_C(1000000);
    }
    return timed_request;
}

dtv_internal_result read_demodulator(const dtv::spi::PublicTuneRequest &request,
                                     const dtv::spi::PublicTuneSession &session,
                                     std::uint8_t reg, std::uint8_t mode, std::uint8_t *value)
{
    return ::asicen::detail::ground_io::read_demodulator(
        request, session, ground_demodulator, reg, mode, value,
        response_length_diagnostic, response_status_diagnostic);
}

dtv_internal_result write_demodulator(const dtv::spi::PublicTuneRequest &request,
                                      const dtv::spi::PublicTuneSession &session,
                                      std::uint8_t reg, std::uint8_t value)
{
    return ::asicen::detail::ground_io::write_demodulator(
        request, session, ground_demodulator, reg, value,
        response_length_diagnostic, response_status_diagnostic);
}

dtv_internal_result read_rf_tuner(const dtv::spi::PublicTuneRequest &request,
                                  const dtv::spi::PublicTuneSession &session,
                                  std::uint8_t reg, std::uint8_t *value)
{
    return ::asicen::detail::ground_io::read_rf_tuner(
        request, session, ground_demodulator, reg, value,
        response_length_diagnostic, response_status_diagnostic);
}

dtv_internal_result write_rf_tuner(const dtv::spi::PublicTuneRequest &request,
                                   const dtv::spi::PublicTuneSession &session,
                                   std::uint8_t reg, std::uint8_t value)
{
    return ::asicen::detail::ground_io::write_rf_tuner(
        request, session, ground_demodulator, reg, value,
        response_length_diagnostic, response_status_diagnostic);
}

dtv_internal_result wait_until_ns(const dtv::spi::PublicTuneRequest &request,
                                  const dtv::spi::PublicTuneSession &session,
                                  std::uint64_t wake_at)
{
    if (session.io == nullptr || session.io->clock == nullptr ||
        session.io->clock->monotonic_now_ns == nullptr ||
        session.io->clock->delay_until == nullptr)
        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    const auto &clock = *session.io->clock;
    const auto deadline = request.timeout.operation_deadline_ns == 0
        ? UINT64_MAX : request.timeout.operation_deadline_ns;
    while (clock.monotonic_now_ns(clock.user) < wake_at) {
        if (request.cancelled != nullptr && request.cancelled->load(std::memory_order_acquire))
            return make_result(DTV_INTERNAL_RESULT_CANCELLED);
        const auto now = clock.monotonic_now_ns(clock.user);
        const auto wake = std::min(wake_at, now + UINT64_C(1000000));
        std::uint32_t cancelled = 0;
        const auto result = clock.delay_until(clock.user, wake, deadline, nullptr, &cancelled);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result wait_ms(const dtv::spi::PublicTuneRequest &request,
                            const dtv::spi::PublicTuneSession &session,
                            std::uint64_t milliseconds)
{
    if (session.io == nullptr || session.io->clock == nullptr ||
        session.io->clock->monotonic_now_ns == nullptr)
        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    const auto now = session.io->clock->monotonic_now_ns(session.io->clock->user);
    const auto duration = milliseconds * UINT64_C(1000000);
    const auto wake_at = duration > UINT64_MAX - now ? UINT64_MAX : now + duration;
    return wait_until_ns(request, session, wake_at);
}

dtv_internal_result calibrate_vco(const dtv::spi::PublicTuneRequest &request,
                                  const dtv::spi::PublicTuneSession &session)
{
    auto result = write_rf_tuner(request, session, 0x0e, 0x80);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x0e, 0x00);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = wait_ms(request, session, 1);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    return write_rf_tuner(request, session, 0x0e, 0x00);
}

dtv_internal_result write_tuner_registers(const dtv::spi::PublicTuneRequest &request,
                                          const dtv::spi::PublicTuneSession &session,
                                          const GroundTuneValues &values)
{
    const std::array<std::uint8_t, 6> registers{
        values.tuner.reg1, values.tuner.reg2, values.tuner.reg3,
        values.tuner.reg4, values.tuner.reg5, values.tuner.reg6};
    const ::asicen::detail::ground_io::IoContext context{
        session.io, request.timeout, request.cancelled};
    return ::asicen::detail::ground_io::write_rf_tuner_registers(
        context, ground_demodulator, registers,
        response_length_diagnostic, response_status_diagnostic);
}

dtv_internal_result tune_rf_tuner(const dtv::spi::PublicTuneRequest &request,
                                  const dtv::spi::PublicTuneSession &session,
                                  const GroundTuneValues &values)
{
    std::uint8_t value = 0;
    auto result = read_rf_tuner(request, session, 0x09, &value);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x09, static_cast<std::uint8_t>(value | 0x10));
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = read_rf_tuner(request, session, 0x06, &value);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x06, static_cast<std::uint8_t>(value | 0x01));
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = wait_ms(request, session, 1);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = read_rf_tuner(request, session, 0x09, &value);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x09, static_cast<std::uint8_t>(value & 0xef));
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = read_rf_tuner(request, session, 0x06, &value);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x06, static_cast<std::uint8_t>(value & 0xef));
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_tuner_registers(request, session, values);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = calibrate_vco(request, session);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = read_rf_tuner(request, session, 0x0e, &value);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    value &= 0x3f;
    const bool recalibrate = values.tuner.vco_selected ? value > 0x3c : value < 0x02;
    if (recalibrate) {
        std::uint8_t reg6 = values.tuner.reg6;
        reg6 = values.tuner.vco_selected
            ? static_cast<std::uint8_t>(reg6 & 0xf7)
            : static_cast<std::uint8_t>(reg6 | 0x08);
        result = write_rf_tuner(request, session, 0x06, reg6);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = write_rf_tuner(request, session, 0x0e, 0x80);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = write_rf_tuner(request, session, 0x0e, 0x00);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = wait_ms(request, session, 1);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    result = read_demodulator(request, session, 0x1e, 1, &value);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    return write_demodulator(request, session, 0x1e,
                             ground_agc_value(value, values.center_frequency_khz));
}

dtv_internal_result write_gpio_lna_off(const dtv::spi::PublicTuneRequest &request,
                                       const dtv::spi::PublicTuneSession &session)
{
    return ::asicen::detail::ground_io::write_gpio_verified(
        request, session, 0x00, 0x01, 0x00, "PX-W3U2 GPIO応答長が不正です",
        "PX-W3U2 LNA GPIOの読み戻しが一致しません");
}

dtv_internal_result adjust_gain(const dtv::spi::PublicTuneRequest &request,
                                const dtv::spi::PublicTuneSession &session)
{
    auto result = write_rf_tuner(request, session, 0x12, 0x00);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    std::uint8_t gain_register = 0;
    std::uint8_t mode = 0;
    std::uint8_t agc_register = 0;
    result = read_rf_tuner(request, session, 0x12, &gain_register);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = read_rf_tuner(request, session, 0x13, &mode);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    mode &= 0x1f;
    result = read_rf_tuner(request, session, 0x0d, &agc_register);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if ((agc_register & 0x10) == 0) {
        result = write_rf_tuner(request, session, 0x10, 0x00);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    std::uint8_t tuner_gain = 0;
    result = read_rf_tuner(request, session, 0x10, &tuner_gain);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    constexpr std::uint8_t gain_additions[] = {10, 8, 6, 4, 2, 0, 0, 0};
    const auto gain = static_cast<unsigned>((gain_register & 0x1f) * 2) +
                      gain_additions[(gain_register >> 5) & 0x07];
    std::uint8_t final_mode = mode;
    bool clear_and_set = false;
    bool adjust = false;
    switch (mode) {
    case 0x0a:
        if (gain > 56) { final_mode = 0x14; clear_and_set = true; }
        else if (gain < 32) { final_mode = 0x02; adjust = true; }
        break;
    case 0x02:
        if (gain > 54) { final_mode = 0x0a; adjust = true; }
        break;
    case 0x14:
        if (gain > 54) { final_mode = 0x10; clear_and_set = true; }
        else if (gain < 42) { final_mode = 0x0a; adjust = true; }
        break;
    case 0x10:
        if (gain < 52) { final_mode = 0x14; clear_and_set = true; }
        break;
    default:
        final_mode = 0x10;
        clear_and_set = true;
        break;
    }

    if (clear_and_set) {
        result = read_rf_tuner(request, session, 0x0d, &agc_register);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = write_rf_tuner(request, session, 0x0d,
                                static_cast<std::uint8_t>(agc_register & 0xef));
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        return write_rf_tuner(request, session, 0x13, final_mode);
    }
    if (!adjust) return make_result(DTV_INTERNAL_RESULT_OK);

    result = read_rf_tuner(request, session, 0x0d, &agc_register);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x0d,
                            static_cast<std::uint8_t>(agc_register & 0xef));
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x10, 0x00);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = read_rf_tuner(request, session, 0x10, &tuner_gain);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = read_rf_tuner(request, session, 0x0d, &agc_register);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x0d,
                            static_cast<std::uint8_t>(agc_register | 0x10));
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_rf_tuner(request, session, 0x10,
                            static_cast<std::uint8_t>(static_cast<unsigned>(tuner_gain) - 3u));
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    return write_rf_tuner(request, session, 0x13, final_mode);
}

dtv_internal_result wait_ground_readiness(const dtv::spi::PublicTuneRequest &request,
                                          const dtv::spi::PublicTuneSession &session)
{
    if (session.io == nullptr || session.io->clock == nullptr ||
        session.io->clock->monotonic_now_ns == nullptr ||
        session.io->clock->delay_until == nullptr)
        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
    constexpr auto window_ns = UINT64_C(340000000);
    const auto &clock = *session.io->clock;
    bool has_lock_start = false;
    bool has_previous_sample = false;
    std::uint64_t lock_start = 0;
    std::uint64_t previous_sample = 0;
    while (true) {
        if (request.cancelled != nullptr && request.cancelled->load(std::memory_order_acquire))
            return make_result(DTV_INTERNAL_RESULT_CANCELLED);
        std::uint8_t lock_value = 0;
        const auto result = read_demodulator(request, session, 0xb0, 1, &lock_value);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        const auto sample_time = clock.monotonic_now_ns(clock.user);
        const bool time_reversed = has_previous_sample && sample_time < previous_sample;
        previous_sample = sample_time;
        has_previous_sample = true;
        if (classify_ground_lock(lock_value) != GroundLock::locked) {
            has_lock_start = false;
            continue;
        }
        if (!has_lock_start || time_reversed) {
            lock_start = sample_time;
            has_lock_start = true;
        } else if (sample_time >= lock_start && sample_time - lock_start >= window_ns) {
            return make_result(DTV_INTERNAL_RESULT_OK);
        }
        const auto target = lock_start > UINT64_MAX - window_ns
            ? UINT64_MAX : lock_start + window_ns;
        const auto wait_result = wait_until_ns(request, session, target);
        if (wait_result.code != DTV_INTERNAL_RESULT_OK) return wait_result;
    }
}

}

dtv_internal_result tune_ground_by_frequency_khz(
    dtv_context *, const dtv::spi::PublicTuneRequest *request,
    dtv::spi::PublicTuneSession *session, std::uint64_t frequency_khz)
{
    if (request == nullptr || session == nullptr || session->begin_effect == nullptr ||
        request->system != DTV_SYSTEM_ISDB_T || session->io == nullptr ||
        session->io->clock == nullptr || session->io->clock->monotonic_now_ns == nullptr ||
        session->io->clock->delay_until == nullptr)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (request->has_tune_options &&
        (request->tune_options.selector != DTV_TUNE_SELECTOR_NONE ||
         request->tune_options.lna != DTV_LNA_DEFAULT))
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);

    GroundTuneValues values{};
    if (!calculate_ground_tune(frequency_khz, &values))
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    const auto timed_request = caller_deadline_request(*request, *session->io->clock);
    std::uint64_t operation_epoch = 0;
    auto result = session->begin_effect(session->user, &operation_epoch);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    (void)operation_epoch;

    for (const auto &reg : ground_tune_prepare) {
        result = write_demodulator(timed_request, *session, reg.reg, reg.value);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    for (std::size_t count = 0; count < ground_tune_max_attempts; ++count) {
        result = write_rf_tuner(timed_request, *session, 0x13, 0x02);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = write_gpio_lna_off(timed_request, *session);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = tune_rf_tuner(timed_request, *session, values);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        std::uint8_t status = 0;
        result = read_rf_tuner(timed_request, *session, 0x0e, &status);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        if (classify_tuner_ready(status) == TunerReady::ready ||
            count + 1 == ground_tune_max_attempts) break;

        std::uint8_t demodulator_value = 0;
        result = read_demodulator(timed_request, *session, 0x1c, 0, &demodulator_value);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        const auto reset = ground_retry_reset_values(demodulator_value);
        result = write_demodulator(timed_request, *session, 0x1c, reset.asserted);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = wait_ms(timed_request, *session, ground_retry_wait_ms);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = write_demodulator(timed_request, *session, 0x1c, reset.released);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        result = wait_ms(timed_request, *session, ground_retry_wait_ms);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        for (const auto &reg : rf_tuner_initialization) {
            result = write_rf_tuner(timed_request, *session, reg.reg, reg.value);
            if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        }
    }
    for (const auto &reg : ground_tune_finish) {
        result = write_demodulator(timed_request, *session, reg.reg, reg.value);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    while (true) {
        if (timed_request.cancelled != nullptr &&
            timed_request.cancelled->load(std::memory_order_acquire))
            return make_result(DTV_INTERNAL_RESULT_CANCELLED);
        std::uint8_t lock_value = 0;
        result = read_demodulator(timed_request, *session, 0xb0, 1, &lock_value);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        if (classify_ground_lock(lock_value) == GroundLock::locked) break;
    }
    result = adjust_gain(timed_request, *session);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    return wait_ground_readiness(timed_request, *session);
}

}
