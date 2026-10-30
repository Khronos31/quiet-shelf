# SPDX-License-Identifier: GPL-2.0-only
#
# Single source of truth for the dtv-userland-core input. v1.0.1 is the
# reproducible reference for this framework, not the final product pin: the
# five products re-pin together after the Core cleanup.
set(TORNE_CORE_REPOSITORY "https://github.com/Khronos31/dtv-userland-core.git")
set(TORNE_CORE_COMMIT "301c4593fc34702729750477d93f9d9e0051b0cb")

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
