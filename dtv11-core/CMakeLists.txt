# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(dtv11_userland_core VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

add_library(dtv STATIC src/cli.cpp)
add_library(dtv11::dtv ALIAS dtv)
target_include_directories(dtv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(dtv PUBLIC c_std_11 cxx_std_20)
set_target_properties(dtv PROPERTIES CXX_EXTENSIONS OFF)

include(cmake/DtvProduct.cmake)

if(BUILD_TESTING AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(tests/fixture)
endif()
