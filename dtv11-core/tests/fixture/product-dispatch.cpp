/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/internal/l3.hpp>

#include <fixture_api.h>

int main()
{
    dtv_load_fixture();
    if (dtv::internal::dispatch_lnb_on("fixture", "Fixture One", nullptr, nullptr, nullptr) !=
        DTV_ERROR_OK) {
        return 1;
    }
    if (dtv::internal::dispatch_set_pid_filter("fixture", "Fixture Two", nullptr, nullptr, 0,
                                                nullptr) != DTV_ERROR_UNSUPPORTED) {
        return 2;
    }
    if (dtv::internal::dispatch_lnb_on("fixture", nullptr, nullptr, nullptr, nullptr) !=
        DTV_ERROR_NOT_READY) {
        return 3;
    }
    return 0;
}
