/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_RESULT_HPP
#define DTV_INTERNAL_RESULT_HPP

#include <exception>
#include <new>
#include <type_traits>
#include <utility>

#include <dtv/spi/types.h>

namespace dtv::internal {

dtv_internal_result exception_result(std::exception_ptr exception = std::current_exception()) noexcept;

template <typename Callback>
dtv_internal_result call_callback(Callback &&callback) noexcept
{
    try {
        return std::forward<Callback>(callback)();
    } catch (...) {
        return exception_result();
    }
}

}

#endif
