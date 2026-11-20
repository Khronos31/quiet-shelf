/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_S3U2_GROUND_IO_HPP
#define ASICEN_PX_S3U2_GROUND_IO_HPP

#include "ground_io_common.hpp"

#include <cstdint>
#include <span>

namespace asicen::px_s3u2::detail {

dtv_internal_result write_ground_tuner_registers(
    const ::asicen::detail::ground_io::IoContext &context,
    std::uint8_t demodulator, std::span<const std::uint8_t, 6> values,
    const char *length_diagnostic, const char *status_diagnostic);

}

#endif
