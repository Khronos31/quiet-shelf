# SPDX-License-Identifier: GPL-2.0-only
#
# Single source of truth for the dtv-userland-core input. 1d5569ee is the
# adopted intermediate Core141 pin for the torne Unix IFD profile, not the
# final five-product pin.
set(TORNE_CORE_REPOSITORY "https://github.com/Khronos31/dtv-userland-core.git")
set(TORNE_CORE_COMMIT "1d5569ee303204cbb2d660517152d8a5b5878041")

set(TORNE_CORE_SOURCE_DIR "" CACHE PATH
    "Developer-supplied dtv-userland-core tree for offline builds; NOT SHA-verified, must be TORNE_CORE_COMMIT")

function(torne_add_core)
    include(FetchContent)
    if(TORNE_CORE_SOURCE_DIR)
        add_subdirectory("${TORNE_CORE_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/dtv-userland-core"
            EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(dtv_userland_core
            GIT_REPOSITORY "${TORNE_CORE_REPOSITORY}"
            GIT_TAG "${TORNE_CORE_COMMIT}")
        FetchContent_MakeAvailable(dtv_userland_core)
    endif()
endfunction()
