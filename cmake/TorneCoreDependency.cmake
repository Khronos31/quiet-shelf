# SPDX-License-Identifier: GPL-2.0-only
#
# Single source of truth for the dtv-userland-core input. 548a046 is the
# adopted Core main pin with the shared Windows WinSCard SDK import fixture
# and the torne Unix IFD profile; it is not the final five-product pin.
set(TORNE_CORE_REPOSITORY "https://github.com/Khronos31/dtv-userland-core.git")
set(TORNE_CORE_COMMIT "548a04605d9e224be179547de30f590ff693a128")

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
