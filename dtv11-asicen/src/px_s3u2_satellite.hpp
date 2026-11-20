/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_S3U2_SATELLITE_HPP
#define ASICEN_PX_S3U2_SATELLITE_HPP

#include <array>
#include <cstdint>

namespace asicen::px_s3u2::detail {

struct SatelliteRfSetting {
    std::uint8_t row;
    std::uint32_t center_frequency_khz;
    std::array<std::uint8_t, 8> tuner_data;
};

// 入力は仕様のRF中心周波数(kHz)。Hz写像や丸めはここで行わない。
bool select_satellite_rf_setting(std::uint32_t rf_center_frequency_khz,
                                 SatelliteRfSetting *out);

}

#endif
