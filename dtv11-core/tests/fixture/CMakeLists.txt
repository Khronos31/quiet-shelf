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
                MODELS fixture_one fixture_two)

dtv_add_product(NAME i0_asicen_fixture
                COMMAND i0asicenctl
                VERSION 0.0.0
                DEFAULT_MODEL ""
                MODELS px_w3u3)

dtv_add_product(NAME i0_torne_fixture
                COMMAND i0tornectl
                VERSION 0.0.0
                DEFAULT_MODEL ""
                MODELS cech_zd1j)

dtv_add_product(NAME i0_pix_fixture
                COMMAND i0pixctl
                VERSION 0.0.0
                DEFAULT_MODEL ""
                MODELS pix_dt295_custom_v25)

if(BUILD_TESTING)
    add_library(spi_types_c_consumer OBJECT spi-types-consumer.c)
    target_include_directories(spi_types_c_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
    add_executable(c_consumer consumer.c)
    target_link_libraries(c_consumer PRIVATE dtv::userland)
    add_test(NAME i0_pix_model_metadata
        COMMAND ${CMAKE_COMMAND}
            "-DCLI=$<TARGET_FILE:i0pixctl>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/pix-model-contract.cmake")
    add_test(NAME cli_contract_table
        COMMAND ${CMAKE_COMMAND}
            "-DCLI=$<TARGET_FILE:fixturectl>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../cli-contract.cmake")
endif()
