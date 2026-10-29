# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.21)
project(torne_userland VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TORNE_BUILD_TESTS "Build tests that do not require hardware" ON)
option(TORNE_BUILD_LIBUSB "Build libusb transport and the user-space probe" ON)
option(TORNE_BUILD_CARD "Build the optional T=1 card UART adapter" OFF)

# Frozen v0.1.0 baseline: never track a moving tag or main branch.
# Consumers can override FETCHCONTENT_SOURCE_DIR_DTV_USERLAND_CORE to build offline.
include(FetchContent)
FetchContent_Declare(dtv_userland_core
    GIT_REPOSITORY https://github.com/Khronos31/dtv-userland-core.git
    GIT_TAG 32731433803f01903b4f198ddd94647c4d222ac0
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(dtv_userland_core)

add_library(torne_hardware STATIC
    third_party/torne_drv/torne_bridge.c
    third_party/torne_drv/torne_dib.c
)
target_include_directories(torne_hardware PUBLIC third_party/torne_drv)
if(UNIX)
    target_link_libraries(torne_hardware PUBLIC m)
endif()

if(TORNE_BUILD_CARD)
    add_library(torne_card STATIC third_party/torne_drv/torne_card.c)
    target_link_libraries(torne_card PUBLIC torne_hardware)
endif()

add_library(torne_contract STATIC src/product_contract.cpp)
target_include_directories(torne_contract PUBLIC include)
target_link_libraries(torne_contract PUBLIC dtv::core dtv::identity dtv::tuning dtv::json)

# The C chip driver remains independent of the C++17 common core and libusb.
# A single instance owns the bridge and demodulator; physical USB remains injected.
add_library(torne_device STATIC src/torne_device.cpp)
target_include_directories(torne_device PUBLIC include)
target_link_libraries(torne_device PUBLIC torne_hardware torne_contract)

if(TORNE_BUILD_LIBUSB)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
    endif()
    if(TARGET PkgConfig::LIBUSB)
        add_library(torne_usb STATIC src/libusb_transport.cpp)
        target_include_directories(torne_usb PUBLIC include)
        target_link_libraries(torne_usb PUBLIC torne_device PkgConfig::LIBUSB)
        add_executable(torne-probe tools/torne-probe.cpp)
        target_link_libraries(torne-probe PRIVATE torne_usb)
    else()
        message(STATUS "libusb-1.0 not found; disabling USB probe (pure tests remain available)")
    endif()
endif()

if(TORNE_BUILD_TESTS)
    enable_testing()
    add_executable(torne_contract_tests tests/product_contract_tests.cpp)
    target_link_libraries(torne_contract_tests PRIVATE torne_device)
    add_test(NAME torne_contract_tests COMMAND torne_contract_tests)
endif()
