# SPDX-License-Identifier: GPL-2.0-only
# Reconstruct byte-identical GPL-compatible portability changes in
# torne-userland Draft PR head ec9b2f7d1e735835529ff9febd959fe54a3865b5 from public upstream code.
# Fail closed if the upstream source layout changes.
set(dib "${CMAKE_CURRENT_LIST_DIR}/../third_party/torne_drv/torne_dib.c")
file(READ "${dib}" dib_data)
set(dib_old [=[	return dib_set_frontend(d) ?: d->err;]=])
set(dib_new [=[	{
		int frontend_result = dib_set_frontend(d);
		return frontend_result != 0 ? frontend_result : d->err;
	}]=])
string(FIND "${dib_data}" "${dib_old}" dib_at)
if(dib_at LESS 0)
  message(FATAL_ERROR "Unexpected pinned torne_dib.c: GNU C expression not found")
endif()
string(REPLACE "${dib_old}" "${dib_new}" dib_updated "${dib_data}")
file(WRITE "${dib}" "${dib_updated}")

set(card "${CMAKE_CURRENT_LIST_DIR}/../third_party/torne_drv/torne_card.c")
file(READ "${card}" card_data)
set(card_old [=[#include "torne_card.h"]=])
set(card_new [=[#include "torne_card.h"

/* Some non-Linux C runtimes omit the Linux ENOMEDIUM constant. The
 * driver callbacks use Linux-style negative errno values internally.
 * Preserve the distinct "no smart card" error on these platforms. */
#ifndef ENOMEDIUM
#define ENOMEDIUM 123
#endif]=])
string(FIND "${card_data}" "${card_old}" card_at)
if(card_at LESS 0)
  message(FATAL_ERROR "Unexpected pinned torne_card.c: include not found")
endif()
string(REPLACE "${card_old}" "${card_new}" card_updated "${card_data}")
file(WRITE "${card}" "${card_updated}")
message(STATUS "Restored pinned upstream C and applied documented portability-only changes")
