// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/result.hpp>

namespace dtv::internal {

dtv_internal_result exception_result(std::exception_ptr exception) noexcept
{
    try {
        if (exception) {
            std::rethrow_exception(exception);
        }
    } catch (const std::bad_alloc &) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED, 0);
    } catch (...) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INTERNAL, 0);
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_INTERNAL, 0);
}

}
