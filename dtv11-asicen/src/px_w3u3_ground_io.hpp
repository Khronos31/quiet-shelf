/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U3_GROUND_IO_HPP
#define ASICEN_PX_W3U3_GROUND_IO_HPP

#include <dtv/spi/device_ops.hpp>

namespace asicen::px_w3u3::detail {

dtv_internal_result tune_ground_by_frequency(
    dtv_context *, const dtv::spi::PublicTuneRequest *, dtv::spi::PublicTuneSession *);

}

#endif
