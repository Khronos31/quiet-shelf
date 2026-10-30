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
option(TORNE_BUILD_LIVE_USB_CHECK
    "Build the developer USB transport live check (requires the assigned device)" OFF)

# FR30 regeneration is an explicit maintainer operation. The normal build and
# install do not require a cross assembler; this variable is only consulted by
# the torne_fr30_generate target and the optional generation test.
set(TORNE_FR30_BINUTILS_DIR "" CACHE PATH
    "binutils 2.47 fr30-elf build tree for explicit FR30 generation")

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
    src/usb/backend.cpp
    src/usb/libusb_io.cpp
    src/usb/transport.cpp)
target_link_libraries(torne_usb PUBLIC torne_daemon_core torne_libusb)
torne_set_warnings(torne_usb)

add_library(torne_widen STATIC
    src/widen/bridge.cpp
    src/widen/loader.cpp
    src/widen/recognition.cpp)
target_link_libraries(torne_widen PUBLIC torne_usb)
torne_set_warnings(torne_widen)

add_library(torne_core1 STATIC
    src/core1/host_runtime.cpp
    src/core1/i2c_backend.cpp)
target_link_libraries(torne_core1 PUBLIC torne_widen)
torne_set_warnings(torne_core1)

add_library(torne_dib_frontend STATIC
    src/dib/dib_frontend.c
    src/dib/torne_dib.c
    src/dib/torne_i2c_adapter.c)
target_include_directories(torne_dib_frontend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
torne_set_warnings(torne_dib_frontend)

add_executable(torne_torned src/torned/main.cpp)
set_target_properties(torne_torned PROPERTIES OUTPUT_NAME torned)
target_link_libraries(torne_torned PRIVATE torne_usb)
torne_set_warnings(torne_torned)

add_executable(torne_tornectl src/tornectl/main.cpp)
set_target_properties(torne_tornectl PROPERTIES OUTPUT_NAME tornectl)
target_link_libraries(torne_tornectl PRIVATE torne_cli_common dtv::host_ctl)
torne_set_warnings(torne_tornectl)

add_library(torne_ts_entry STATIC src/torne_ts/profile.cpp)
target_link_libraries(torne_ts_entry PUBLIC torne_cli_common dtv::host_ts)
torne_set_warnings(torne_ts_entry)

add_executable(torne_torne_ts src/torne_ts/main.cpp)
set_target_properties(torne_torne_ts PROPERTIES OUTPUT_NAME torne-ts)
target_link_libraries(torne_torne_ts PRIVATE torne_ts_entry)
torne_set_warnings(torne_torne_ts)

add_custom_target(torne_fr30_generate
    COMMAND ${CMAKE_COMMAND}
        -DTORNE_FR30_BINUTILS_DIR=${TORNE_FR30_BINUTILS_DIR}
        -DTORNE_FR30_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-generated
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fr30_generate.cmake"
    COMMENT "Generating and checking FR30 fixtures with explicit GNU binutils"
    VERBATIM)

add_custom_target(torne_fr30_widen
    COMMAND ${CMAKE_COMMAND}
        -DTORNE_FR30_BINUTILS_DIR=${TORNE_FR30_BINUTILS_DIR}
        -DTORNE_WIDEN_OUTPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fr30/generated
        -DTORNE_WIDEN_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-widen-scratch
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fr30_widen.cmake"
    COMMENT "Generating tracked Widen candidate images with explicit GNU binutils"
    VERBATIM)

add_custom_target(torne_fr30_core1
    COMMAND ${CMAKE_COMMAND}
        -DTORNE_FR30_BINUTILS_DIR=${TORNE_FR30_BINUTILS_DIR}
        -DTORNE_CORE1_OUTPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fr30/generated
        -DTORNE_CORE1_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-core1-scratch
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fr30_core1.cmake"
    COMMENT "Generating tracked Core 1 candidate image with explicit GNU binutils"
    VERBATIM)

add_custom_target(torne_fr30_core0
    COMMAND ${CMAKE_COMMAND}
        -DTORNE_FR30_BINUTILS_DIR=${TORNE_FR30_BINUTILS_DIR}
        -DTORNE_CORE0_OUTPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fr30/generated
        -DTORNE_CORE0_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-core0-scratch
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fr30_core0.cmake"
    COMMENT "Generating tracked Core 0 candidate image with explicit GNU binutils"
    VERBATIM)

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

    add_executable(torne_ts_profile_tests tests/torne_ts_profile_tests.cpp)
    target_link_libraries(torne_ts_profile_tests PRIVATE torne_ts_entry)
    torne_set_warnings(torne_ts_profile_tests)
    add_test(NAME torne_ts_profile_tests COMMAND torne_ts_profile_tests)

    add_executable(torne_usb_transport_tests tests/usb_transport_tests.cpp)
    target_link_libraries(torne_usb_transport_tests PRIVATE torne_usb)
    torne_set_warnings(torne_usb_transport_tests)
    add_test(NAME torne_usb_transport_tests COMMAND torne_usb_transport_tests)

    add_executable(torne_widen_recognition_tests tests/widen_recognition_tests.cpp)
    target_link_libraries(torne_widen_recognition_tests PRIVATE torne_widen)
    torne_set_warnings(torne_widen_recognition_tests)
    add_test(NAME torne_widen_recognition_tests COMMAND torne_widen_recognition_tests)

    add_executable(torne_core1_host_runtime_tests
        tests/core1_host_runtime_tests.cpp)
    target_link_libraries(torne_core1_host_runtime_tests PRIVATE torne_core1)
    torne_set_warnings(torne_core1_host_runtime_tests)
    add_test(NAME torne_core1_host_runtime_tests
        COMMAND torne_core1_host_runtime_tests)

    add_executable(torne_core1_i2c_backend_tests
        tests/core1_i2c_backend_tests.cpp)
    target_link_libraries(torne_core1_i2c_backend_tests PRIVATE torne_core1)
    torne_set_warnings(torne_core1_i2c_backend_tests)
    add_test(NAME torne_core1_i2c_backend_tests
        COMMAND torne_core1_i2c_backend_tests)

    add_executable(torne_dib_frontend_tests tests/dib_frontend_tests.cpp)
    target_link_libraries(torne_dib_frontend_tests PRIVATE torne_dib_frontend)
    torne_set_warnings(torne_dib_frontend_tests)
    add_test(NAME torne_dib_frontend_tests COMMAND torne_dib_frontend_tests)

    add_executable(torne_widen_loader_tests tests/widen_loader_tests.cpp)
    target_include_directories(torne_widen_loader_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(torne_widen_loader_tests PRIVATE torne_widen)
    torne_set_warnings(torne_widen_loader_tests)
    add_test(NAME torne_widen_loader_tests COMMAND torne_widen_loader_tests)

    add_test(NAME torne_cli_smoke
        COMMAND ${CMAKE_COMMAND}
            -DTORNED=$<TARGET_FILE:torne_torned>
            -DTORNECTL=$<TARGET_FILE:torne_tornectl>
            -DTORNE_TS=$<TARGET_FILE:torne_torne_ts>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake")

    if(TORNE_FR30_BINUTILS_DIR)
        add_test(NAME torne_fr30_generation
            COMMAND ${CMAKE_COMMAND}
                -DTORNE_FR30_BINUTILS_DIR=${TORNE_FR30_BINUTILS_DIR}
                -DTORNE_FR30_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-test-generated
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fr30_generate.cmake")

        add_test(NAME torne_widen_firmware
            COMMAND ${CMAKE_COMMAND}
                -DTORNE_FR30_BINUTILS_DIR=${TORNE_FR30_BINUTILS_DIR}
                -DTORNE_WIDEN_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-widen-test
                -DTORNE_WIDEN_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-widen-test-scratch
                -DTORNE_WIDEN_REFERENCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fr30/generated
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fr30_widen.cmake")

        add_test(NAME torne_core1_firmware
            COMMAND ${CMAKE_COMMAND}
                -DTORNE_FR30_BINUTILS_DIR=${TORNE_FR30_BINUTILS_DIR}
                -DTORNE_CORE1_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-core1-test
                -DTORNE_CORE1_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-core1-test-scratch
                -DTORNE_CORE1_REFERENCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fr30/generated
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fr30_core1.cmake")

        add_test(NAME torne_core0_firmware
            COMMAND ${CMAKE_COMMAND}
                -DTORNE_FR30_BINUTILS_DIR=${TORNE_FR30_BINUTILS_DIR}
                -DTORNE_CORE0_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-core0-test
                -DTORNE_CORE0_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/fr30-core0-test-scratch
                -DTORNE_CORE0_REFERENCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fr30/generated
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fr30_core0.cmake")
    endif()
endif()

if(TORNE_BUILD_LIVE_USB_CHECK)
    add_executable(torne_usb_transport_live_check
        tests/usb_transport_live_check.cpp)
    target_link_libraries(torne_usb_transport_live_check PRIVATE torne_usb)
    torne_set_warnings(torne_usb_transport_live_check)

    add_executable(torne_widen_recognition_live_check
        tests/widen_recognition_live_check.cpp)
    target_link_libraries(torne_widen_recognition_live_check PRIVATE torne_widen)
    torne_set_warnings(torne_widen_recognition_live_check)
endif()

install(TARGETS torne_torned torne_tornectl torne_torne_ts
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    LICENSE README.md PROVENANCE.md docs/cli-framework.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
