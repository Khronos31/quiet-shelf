/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_CLOCK_HPP
#define DTV_INTERNAL_CLOCK_HPP

#include <dtv/spi/types.h>

namespace dtv::internal {

const dtv_runtime_clock *system_runtime_clock();
void set_system_clock_delay_hook_for_test(void (*hook)(void *), void *user);

}

#endif
