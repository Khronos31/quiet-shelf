# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(dtv11_link_spike LANGUAGES CXX)

set(DTV_SPIKE_PRODUCT "sample" CACHE STRING "Product identifier")
set(DTV_SPIKE_COMMAND "samplectl" CACHE STRING "CLI command name")
set(DTV_SPIKE_VERSION "2.0.0" CACHE STRING "Product version")
set(DTV_SPIKE_MODEL "sample_model" CACHE STRING "Model identifier")
set(DTV_SPIKE_FIRMWARE "sample_fw" CACHE STRING "Firmware identifier")

foreach(value DTV_SPIKE_PRODUCT DTV_SPIKE_MODEL DTV_SPIKE_FIRMWARE)
    if(NOT "${${value}}" MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Invalid identifier in ${value}: ${${value}}")
    endif()
endforeach()
if(NOT "${DTV_SPIKE_COMMAND}" MATCHES "^[A-Za-z0-9][A-Za-z0-9-]*$" OR
   NOT "${DTV_SPIKE_VERSION}" MATCHES "^[0-9][A-Za-z0-9.-]*$")
    message(FATAL_ERROR "Invalid command name or version")
endif()

add_library(dtv_spike_core STATIC src/core.cpp)
target_include_directories(dtv_spike_core PUBLIC include)
target_compile_features(dtv_spike_core PUBLIC cxx_std_20)
set_target_properties(dtv_spike_core PROPERTIES CXX_EXTENSIONS OFF)

configure_file(src/product_registry.cpp.in generated/product_registry.cpp @ONLY)
configure_file(src/cli_main.cpp.in generated/cli_main.cpp @ONLY)

add_library(dtv_spike_product STATIC
    src/model.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/product_registry.cpp")
target_include_directories(dtv_spike_product PRIVATE include)
target_compile_features(dtv_spike_product PRIVATE cxx_std_20)
set_target_properties(dtv_spike_product PROPERTIES CXX_EXTENSIONS OFF)

add_executable("${DTV_SPIKE_COMMAND}" "${CMAKE_CURRENT_BINARY_DIR}/generated/cli_main.cpp")
target_include_directories("${DTV_SPIKE_COMMAND}" PRIVATE include)
target_compile_features("${DTV_SPIKE_COMMAND}" PRIVATE cxx_std_20)
target_link_libraries("${DTV_SPIKE_COMMAND}" PRIVATE dtv_spike_product dtv_spike_core)
set_target_properties("${DTV_SPIKE_COMMAND}" PROPERTIES CXX_EXTENSIONS OFF)
