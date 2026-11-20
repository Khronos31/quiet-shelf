/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_S3U2_INITIALIZE_HPP
#define ASICEN_PX_S3U2_INITIALIZE_HPP

#include <dtv/spi/device_ops.hpp>

#include <atomic>

namespace asicen::px_s3u2::detail {

dtv_internal_result initialize_satellite_io(
    const dtv::spi::PublicOpenIo &, dtv_timeout, const std::atomic<bool> *);

}

#endif
