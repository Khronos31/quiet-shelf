# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(dtv11_fixture LANGUAGES C CXX)
    include(CTest)
    add_subdirectory(../.. dtv-core)
endif()

set(PRODUCT_NAME fixture)
set(PRODUCT_COMMAND fixturectl)
set(PRODUCT_VERSION 1.2.3)
set(PRODUCT_DEFAULT_MODEL "")
dtv_add_product(NAME ${PRODUCT_NAME}
                COMMAND ${PRODUCT_COMMAND}
                VERSION ${PRODUCT_VERSION}
                DEFAULT_MODEL "${PRODUCT_DEFAULT_MODEL}"
                SOURCES lib/devices.cpp)

if(BUILD_TESTING)
    add_executable(c_consumer consumer.c)
    target_link_libraries(c_consumer PRIVATE dtv)
    add_test(NAME cli_contract_table
        COMMAND ${CMAKE_COMMAND}
            "-DCLI=$<TARGET_FILE:fixturectl>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../cli-contract.cmake")
endif()
