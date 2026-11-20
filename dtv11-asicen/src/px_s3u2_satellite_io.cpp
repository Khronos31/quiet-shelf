/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_s3u2_satellite_io.hpp"

#include "ground_io_common.hpp"
#include "px_s3u2_satellite.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace asicen::px_s3u2::detail {

namespace ground_io = ::asicen::detail::ground_io;

namespace {

constexpr std::uint8_t satellite_demodulator = 0x32;
constexpr std::uint8_t ground_demodulator = 0x30;
constexpr std::uint8_t read_mode = 0x01;
constexpr std::uint8_t readback_mode = 0x00;
constexpr std::uint64_t ns_per_ms = UINT64_C(1000000);
constexpr char response_length_diagnostic[] = "PX-S3U2 衛星選局I2C応答長が不正です";
constexpr char response_status_diagnostic[] = "PX-S3U2 衛星選局I2C statusが不正です";
constexpr char slot_poll_timeout_diagnostic[] = "PX-S3U2 要求TSIDをスロット表から確認できません";
constexpr char tsid_readback_diagnostic[] = "PX-S3U2 TSID書戻し値が一致しません";

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

bool valid_context(const dtv::spi::PublicTuneRequest &request,
                   const dtv::spi::PublicTuneSession &session,
                   const SatelliteTuneIoRequest &tune_request,
                   const SatelliteTuneIoOutcome *outcome)
{
    return outcome != nullptr && tune_request.slot_poll_interval_ms != 0 &&
           tune_request.slot_poll_attempts != 0 && session.begin_effect != nullptr &&
           session.io != nullptr && session.io->control != nullptr &&
           session.io->clock != nullptr &&
           session.io->clock->monotonic_now_ns != nullptr &&
           session.io->clock->delay_until != nullptr &&
           (request.cancelled == nullptr ||
            !request.cancelled->load(std::memory_order_acquire));
}

dtv_internal_result wait_ms(const dtv::spi::PublicTuneRequest &request,
                            const dtv::spi::PublicTuneSession &session,
                            std::uint32_t delay_ms)
{
    if (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);

    const auto *clock = session.io->clock;
    const auto now = clock->monotonic_now_ns(clock->user);
    const auto delay_ns = static_cast<std::uint64_t>(delay_ms) * ns_per_ms;
    if (now > std::numeric_limits<std::uint64_t>::max() - delay_ns)
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);

    const auto operation_deadline = request.timeout.operation_deadline_ns == 0
        ? std::numeric_limits<std::uint64_t>::max()
        : request.timeout.operation_deadline_ns;
    if (now >= operation_deadline)
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);

    std::uint32_t cancelled = 0;
    const auto result = clock->delay_until(clock->user, now + delay_ns,
                                           operation_deadline, nullptr, &cancelled);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if (cancelled != 0 || (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire)))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);
    if (request.timeout.operation_deadline_ns != 0 &&
        clock->monotonic_now_ns(clock->user) >= request.timeout.operation_deadline_ns)
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    return result;
}

bool operation_deadline_reached(const dtv::spi::PublicTuneRequest &request,
                                const dtv::spi::PublicTuneSession &session)
{
    return request.timeout.operation_deadline_ns != 0 &&
           session.io->clock->monotonic_now_ns(session.io->clock->user) >=
               request.timeout.operation_deadline_ns;
}

dtv_internal_result write_tsid(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t tsid)
{
    // 奇数レジスタ開始のchunk規則に合わせ、2byteを個別writeする。
    const std::array<std::uint8_t, 2> bytes{
        static_cast<std::uint8_t>(tsid >> 8), static_cast<std::uint8_t>(tsid)};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto result = ground_io::write_demodulator(
            request, session, satellite_demodulator,
            static_cast<std::uint8_t>(0x8f + index), bytes[index],
            response_length_diagnostic, response_status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

bool slot_contains_tsid(const std::array<std::uint8_t, 16> &slots,
                        std::uint16_t target_tsid)
{
    for (std::size_t index = 0; index < slots.size(); index += 2) {
        const auto tsid = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(slots[index]) << 8) | slots[index + 1]);
        if (tsid != 0x0000 && tsid != 0xffff && tsid == target_tsid) return true;
    }
    return false;
}

}

dtv_internal_result tune_satellite_io(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session,
    const SatelliteTuneIoRequest &tune_request, SatelliteTuneIoOutcome *outcome)
{
    if (!valid_context(request, session, tune_request, outcome))
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);

    SatelliteRfSetting setting{};
    if (!select_satellite_rf_setting(tune_request.rf_center_frequency_khz, &setting))
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    if (setting.row >= 12 && tune_request.initial_tsid != 0xffff)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);

    if (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);
    if (operation_deadline_reached(request, session))
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);

    std::uint64_t operation_epoch = 0;
    auto result = session.begin_effect(session.user, &operation_epoch);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    result = ground_io::write_demodulator(
        request, session, satellite_demodulator, 0x25, 0x00,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = ground_io::write_demodulator(
        request, session, satellite_demodulator, 0x23, 0x4d,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_tsid(request, session, tune_request.initial_tsid);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    const auto &tuner_data = setting.tuner_data;
    const std::array<std::uint8_t, 6> message1{
        0xfe, 0xc0, tuner_data[0], tuner_data[1], tuner_data[2], tuner_data[3]};
    result = ground_io::write_stage(
        request, session, satellite_demodulator, false, message1,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    const std::array<std::uint8_t, 3> message2{0xfe, 0xc0, tuner_data[4]};
    result = ground_io::write_stage(
        request, session, satellite_demodulator, false, message2,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = wait_ms(request, session, 10);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    const std::array<std::uint8_t, 4> message3{
        0xfe, 0xc0, tuner_data[5], tuner_data[6]};
    result = ground_io::write_stage(
        request, session, satellite_demodulator, false, message3,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    const std::array<std::uint8_t, 3> message4{0xfe, 0xc0, tuner_data[7]};
    result = ground_io::write_stage(
        request, session, satellite_demodulator, false, message4,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    result = ground_io::write_demodulator(
        request, session, ground_demodulator, 0x0f, 0x34,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = ground_io::write_demodulator(
        request, session, satellite_demodulator, 0x03, 0x01,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = ground_io::write_demodulator(
        request, session, satellite_demodulator, 0x23, 0x4c,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    bool target_tsid_found = false;
    for (std::uint32_t attempt = 0; attempt < tune_request.slot_poll_attempts; ++attempt) {
        if (request.cancelled != nullptr &&
            request.cancelled->load(std::memory_order_acquire))
            return make_result(DTV_INTERNAL_RESULT_CANCELLED);
        if (operation_deadline_reached(request, session))
            return make_result(DTV_INTERNAL_RESULT_TIMEOUT);

        std::array<std::uint8_t, 16> slots{};
        result = ground_io::read_demodulator_bytes(
            request, session, satellite_demodulator, 0xce, read_mode, slots,
            response_length_diagnostic, response_status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        if (operation_deadline_reached(request, session))
            return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
        if (slot_contains_tsid(slots, tune_request.target_tsid)) {
            target_tsid_found = true;
            break;
        }
        if (attempt + 1 < tune_request.slot_poll_attempts) {
            result = wait_ms(request, session, tune_request.slot_poll_interval_ms);
            if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        }
    }
    if (!target_tsid_found)
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT, slot_poll_timeout_diagnostic);

    result = write_tsid(request, session, tune_request.target_tsid);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    std::array<std::uint8_t, 2> readback{};
    result = ground_io::read_demodulator_bytes(
        request, session, satellite_demodulator, 0x8f, readback_mode, readback,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    const auto expected_tsid = std::array<std::uint8_t, 2>{
        static_cast<std::uint8_t>(tune_request.target_tsid >> 8),
        static_cast<std::uint8_t>(tune_request.target_tsid)};
    if (readback != expected_tsid)
        return make_result(DTV_INTERNAL_RESULT_USB_IO, tsid_readback_diagnostic);

    std::uint8_t lock_register = 0;
    result = ground_io::read_demodulator(
        request, session, satellite_demodulator, 0xc3, read_mode, &lock_register,
        response_length_diagnostic, response_status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    outcome->selected_tsid = tune_request.target_tsid;
    outcome->lock_register = lock_register;
    outcome->locked = (lock_register & 0x10) == 0;
    return result;
}

}
