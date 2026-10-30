# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0077 NEW)

project(torne_userland VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TORNE_BUILD_TESTS "Build offline CLI framework tests" ON)

# Core features needed by the three product commands and the daemon seam. The
# daemon does not assemble a listener yet, but the same input is already used
# so that the later integration does not add a second Core configuration.
set(DTV_USERLAND_BUILD_RECEIVER ON)
set(DTV_USERLAND_BUILD_CARD ON)
set(DTV_USERLAND_BUILD_CAPTURE ON)
set(DTV_USERLAND_BUILD_CONTROL ON)
set(DTV_USERLAND_BUILD_STATUS_PROJECTION ON)
set(DTV_USERLAND_BUILD_AUTHORITY ON)
set(DTV_USERLAND_BUILD_HOST_TUNER ON)
set(DTV_USERLAND_BUILD_HOST_WORKERS ON)
set(DTV_USERLAND_BUILD_HOST_RUNTIME ON)
set(DTV_USERLAND_BUILD_HOST_CTL ON)
set(DTV_USERLAND_BUILD_HOST_TS ON)
set(DTV_USERLAND_BUILD_HOST_FACADE ON)

include(cmake/TorneCoreDependency.cmake)
torne_add_core()

include(cmake/TorneLibusb.cmake)
torne_add_libusb()

function(torne_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
    endif()
endfunction()

add_library(torne_cli_common INTERFACE)
target_compile_features(torne_cli_common INTERFACE cxx_std_17)
target_include_directories(torne_cli_common INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(torne_cli_common INTERFACE
    TORNE_USERLAND_PRODUCT_VERSION="${PROJECT_VERSION}")
target_link_libraries(torne_cli_common INTERFACE
    dtv::identity dtv::json dtv::host_facade)

add_library(torne_daemon_core STATIC
    src/torned/config.cpp
    src/torned/daemon.cpp)
target_link_libraries(torne_daemon_core PUBLIC
    torne_cli_common dtv::host_runtime dtv::host_ts)
torne_set_warnings(torne_daemon_core)

add_library(torne_usb STATIC
    src/usb/discovery.cpp
    src/usb/backend.cpp)
target_link_libraries(torne_usb PUBLIC torne_daemon_core torne_libusb)
torne_set_warnings(torne_usb)

add_executable(torne_torned src/torned/main.cpp)
set_target_properties(torne_torned PROPERTIES OUTPUT_NAME torned)
target_link_libraries(torne_torned PRIVATE torne_usb)
torne_set_warnings(torne_torned)

add_executable(torne_tornectl src/tornectl/main.cpp)
set_target_properties(torne_tornectl PROPERTIES OUTPUT_NAME tornectl)
target_link_libraries(torne_tornectl PRIVATE torne_cli_common dtv::host_ctl)
torne_set_warnings(torne_tornectl)

add_executable(torne_torne_ts src/torne_ts/main.cpp)
set_target_properties(torne_torne_ts PROPERTIES OUTPUT_NAME torne-ts)
target_link_libraries(torne_torne_ts PRIVATE torne_cli_common dtv::host_ts)
torne_set_warnings(torne_torne_ts)

if(TORNE_BUILD_TESTS)
    enable_testing()

    add_executable(torne_daemon_tests tests/daemon_tests.cpp)
    target_link_libraries(torne_daemon_tests PRIVATE torne_daemon_core)
    torne_set_warnings(torne_daemon_tests)
    add_test(NAME torne_daemon_tests COMMAND torne_daemon_tests)

    add_executable(torne_usb_discovery_tests tests/usb_discovery_tests.cpp)
    target_link_libraries(torne_usb_discovery_tests PRIVATE torne_usb)
    torne_set_warnings(torne_usb_discovery_tests)
    add_test(NAME torne_usb_discovery_tests COMMAND torne_usb_discovery_tests)

    add_test(NAME torne_cli_smoke
        COMMAND ${CMAKE_COMMAND}
            -DTORNED=$<TARGET_FILE:torne_torned>
            -DTORNECTL=$<TARGET_FILE:torne_tornectl>
            -DTORNE_TS=$<TARGET_FILE:torne_torne_ts>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake")
endif()

install(TARGETS torne_torned torne_tornectl torne_torne_ts
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    LICENSE README.md PROVENANCE.md docs/cli-framework.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
