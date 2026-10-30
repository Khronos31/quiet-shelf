# SPDX-License-Identifier: GPL-2.0-only
#
# Single source of truth for the dtv-userland-core input. 9d8e02c4 is the
# intermediate card/T=1 development pin, not the final five-product pin.
set(TORNE_CORE_REPOSITORY "https://github.com/Khronos31/dtv-userland-core.git")
set(TORNE_CORE_COMMIT "9d8e02c4f113568012e257ed46d72c9a55751958")

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
