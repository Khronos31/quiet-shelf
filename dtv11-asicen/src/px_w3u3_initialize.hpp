/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_PX_W3U3_INITIALIZE_HPP
#define ASICEN_PX_W3U3_INITIALIZE_HPP

#include <dtv/spi/device_ops.hpp>

#include <atomic>
#include <cstdint>

namespace asicen::px_w3u3::detail {

/* handle_indexとの対応は呼び出し側で確定してroleを渡す。 */
enum class FunctionRole : std::uint8_t { main, sub };

struct FunctionInitializationState {
    bool initialized = false;
    std::uint8_t gpio_snapshot = 0;
    bool cleanup_failed = false;
};

dtv_internal_result initialize_function(
    FunctionRole, const dtv::spi::PublicOpenIo &, dtv_timeout,
    const std::atomic<bool> *, FunctionInitializationState *);

}

#endif
