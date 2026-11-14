# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(dtv11_userland_core VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0>=1.0.23)
endif()
if(TARGET PkgConfig::LIBUSB)
    set(DTV_LIBUSB_TARGET PkgConfig::LIBUSB)
else()
    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
    find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        message(FATAL_ERROR "libusb-1.0 development files are required")
    endif()
    add_library(dtv-libusb UNKNOWN IMPORTED)
    set_target_properties(dtv-libusb PROPERTIES
        IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}")
    set(DTV_LIBUSB_TARGET dtv-libusb)
endif()
include(CheckCXXSourceCompiles)
if(TARGET PkgConfig::LIBUSB)
    set(CMAKE_REQUIRED_INCLUDES ${LIBUSB_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${LIBUSB_LINK_LIBRARIES})
else()
    set(CMAKE_REQUIRED_INCLUDES ${LIBUSB_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${LIBUSB_LIBRARY})
endif()
unset(DTV_LIBUSB_HAS_FD_WRAPPER CACHE)
check_cxx_source_compiles("#include <libusb.h>
#if !defined(LIBUSB_API_VERSION) || LIBUSB_API_VERSION < 0x01000107
#error libusb 1.0.23 or newer is required
#endif
int main() { auto function = &libusb_wrap_sys_device; (void)function; }
" DTV_LIBUSB_HAS_FD_WRAPPER)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT DTV_LIBUSB_HAS_FD_WRAPPER)
    message(FATAL_ERROR "libusb 1.0.23 or newer with libusb_wrap_sys_device is required")
endif()

add_library(dtv-userland STATIC
    src/boot.cpp
    src/cli.cpp
    src/clock.cpp
    src/discovery.cpp
    src/firmware.cpp
    src/l3.cpp
    src/l3-registry.cpp
    src/result.cpp
    src/ts-framer.cpp
    src/usb.cpp)
add_library(dtv::userland ALIAS dtv-userland)
target_include_directories(dtv-userland PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(dtv-userland PUBLIC c_std_11 cxx_std_20)
target_link_libraries(dtv-userland PRIVATE ${DTV_LIBUSB_TARGET} Threads::Threads)
set_target_properties(dtv-userland PROPERTIES CXX_EXTENSIONS OFF)

include(cmake/DtvProduct.cmake)

if(BUILD_TESTING AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(tests/fixture)
    add_executable(usb_owner_contract tests/usb-owner.cpp)
    target_link_libraries(usb_owner_contract PRIVATE dtv::userland)
    add_test(NAME usb_owner_contract COMMAND usb_owner_contract)
    add_executable(usb_async_io_contract tests/usb-async-io.cpp)
    target_include_directories(usb_async_io_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(usb_async_io_contract PRIVATE dtv::userland ${DTV_LIBUSB_TARGET})
    add_test(NAME usb_async_io_contract COMMAND usb_async_io_contract)
    add_executable(firmware_preflight_contract tests/firmware-preflight.cpp)
    target_link_libraries(firmware_preflight_contract PRIVATE dtv::userland)
    add_test(NAME firmware_preflight_contract COMMAND firmware_preflight_contract)
    add_executable(i1c_boot_contract tests/i1c-boot-contract.cpp)
    target_link_libraries(i1c_boot_contract PRIVATE dtv::userland)
    add_test(NAME i1c_boot_contract COMMAND i1c_boot_contract)
    add_executable(i2c1_partial_boot_contract tests/i2c1-partial-boot.cpp)
    target_link_libraries(i2c1_partial_boot_contract PRIVATE dtv::userland)
    add_test(NAME i2c1_partial_boot_contract COMMAND i2c1_partial_boot_contract)
    add_executable(ts_framer_contract tests/ts-framer.cpp)
    target_link_libraries(ts_framer_contract PRIVATE dtv::userland)
    add_test(NAME ts_framer_contract COMMAND ts_framer_contract)
    add_executable(l3_c_consumer tests/l3-consumer.c)
    target_link_libraries(l3_c_consumer PRIVATE dtv::userland)
    set_target_properties(l3_c_consumer PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME l3_c_consumer COMMAND l3_c_consumer)
    add_executable(feature_declaration_contract tests/feature-declaration.cpp)
    target_link_libraries(feature_declaration_contract PRIVATE dtv::userland)
    add_test(NAME feature_declaration_contract COMMAND feature_declaration_contract)
endif()
