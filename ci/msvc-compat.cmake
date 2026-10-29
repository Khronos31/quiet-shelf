# CI-only source normalization for MSVC: GNU C omitted-middle-operand ?: is not ISO C11.
# The authoritative third_party/ source is unchanged in the private PR.
# This patch operates on a public GPL-2.0-only upstream C source restored
# byte-for-byte from mihalinn/torne_drv@fc662902.
set(path "${CMAKE_CURRENT_LIST_DIR}/../third_party/torne_drv/torne_dib.c")
file(READ "${path}" data)
set(original "return dib_set_frontend(d) ?: d->err;")
set(replacement "int frontend_result = dib_set_frontend(d); return frontend_result != 0 ? frontend_result : d->err;")
string(FIND "${data}" "${original}" where)
if(where LESS 0)
  message(FATAL_ERROR "Expected upstream GNU ?: expression not found; refuse to patch")
endif()
string(REPLACE "${original}" "${replacement}" patched "${data}")
file(WRITE "${path}" "${patched}")
message(STATUS "Applied portable single-evaluation C11 conditional for MSVC")
