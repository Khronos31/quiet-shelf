/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_s3u2_initialize.hpp"

#include "ground_io_common.hpp"
#include "satellite_initialize_common.hpp"

#include <array>
#include <cstdint>

namespace asicen::px_s3u2::detail {

namespace ground_io = ::asicen::detail::ground_io;

namespace {

using ::asicen::detail::satellite_initialize::satellite_demodulator_initialization;

constexpr std::uint8_t satellite_demodulator = 0x32;
constexpr std::uint8_t ground_demodulator = 0x30;
constexpr char response_length_diagnostic[] = "PX-S3U2 衛星初期化I2C応答長が不正です";
constexpr char response_status_diagnostic[] = "PX-S3U2 衛星初期化I2C statusが不正です";

}

dtv_internal_result initialize_satellite_io(
    const dtv::spi::PublicOpenIo &open_io, dtv_timeout timeout,
    const std::atomic<bool> *cancelled)
{
    const auto context = ground_io::context_from_open(open_io, timeout, cancelled);
    for (const auto &write : satellite_demodulator_initialization) {
        const auto result = ground_io::write_demodulator(
            context, satellite_demodulator, write.reg, write.value,
            response_length_diagnostic, response_status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }

    constexpr std::uint8_t pulse_values[] = {0x00, 0x10, 0x00};
    for (const auto value : pulse_values) {
        const std::array<std::uint8_t, 4> bytes{0xfe, 0xc6, 0x10, value};
        const auto result = ground_io::write_stage(
            context, ground_demodulator, false, bytes,
            response_length_diagnostic, response_status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }

    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

}
