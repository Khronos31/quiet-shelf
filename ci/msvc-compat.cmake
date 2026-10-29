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

# Windows CRT does not define the Linux-specific ENOMEDIUM errno.
# The numeric fallback is the Linux errno value so the callback contract
# continues to distinguish "no smart card" from generic I/O failures.
set(card "${CMAKE_CURRENT_LIST_DIR}/../third_party/torne_drv/torne_card.c")
file(READ "${card}" card_data)
set(card_marker "#include \"torne_card.h\"")
string(FIND "${card_data}" "${card_marker}" card_at)
if(card_at LESS 0)
  message(FATAL_ERROR "Expected upstream card include not found; refuse to patch")
endif()
set(card_new "#include \"torne_card.h\"\n#ifndef ENOMEDIUM\n#define ENOMEDIUM 123 /* Linux: no medium / smart card */\n#endif")
string(REPLACE "${card_marker}" "${card_new}" card_patched "${card_data}")
file(WRITE "${card}" "${card_patched}")
message(STATUS "Added ENOMEDIUM portability fallback for MSVC CI")

