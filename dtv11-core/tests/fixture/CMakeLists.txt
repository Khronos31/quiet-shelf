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
dtv_product(NAME ${PRODUCT_NAME}
                COMMAND ${PRODUCT_COMMAND}
                VERSION ${PRODUCT_VERSION}
                DEFAULT_MODEL "${PRODUCT_DEFAULT_MODEL}"
                MODELS fixture_one fixture_two
                SOURCES product-to-all.cpp firmware-consumer.cpp
                FIRMWARE fixture_blob=firmware.bin)

dtv_product(NAME i0_asicen_fixture
                COMMAND i0asicenctl
                VERSION 0.0.0
                DEFAULT_MODEL ""
                MODELS px_w3u3
                SOURCES product-to-all.cpp)

dtv_product(NAME i0_torne_fixture
                COMMAND i0tornectl
                VERSION 0.0.0
                DEFAULT_MODEL ""
                MODELS cech_zd1j
                SOURCES product-to-all.cpp)

dtv_product(NAME i0_pix_fixture
                COMMAND i0pixctl
                VERSION 0.0.0
                DEFAULT_MODEL ""
                MODELS pix_dt295_custom_v25
                SOURCES product-to-all.cpp)

dtv_product(NAME assetfixture
                COMMAND assetfixturectl
                VERSION 0.0.0
                DEFAULT_MODEL ""
                MODELS asset_model
                SOURCES product-to-all.cpp
                FIRMWARE fixture_blob=firmware.bin)

dtv_product(NAME scanfixture
                COMMAND scanfixturectl
                VERSION 0.0.0
                DEFAULT_MODEL ""
                MODELS scan_model scan_model_alias
                SOURCES scanfixture-to-all.cpp)

if(BUILD_TESTING)
    add_library(spi_types_c_consumer OBJECT spi-types-consumer.c)
    target_compile_features(spi_types_c_consumer PRIVATE c_std_11)
    target_include_directories(spi_types_c_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
    add_executable(c_consumer consumer.c)
    dtv_add_product_aggregate(c_consumer_registry PRODUCTS fixture)
    target_link_libraries(c_consumer PRIVATE fixture c_consumer_registry)
    add_test(NAME l3_foundation_contract COMMAND c_consumer)
    add_executable(spi_state_lifecycle spi-state-lifecycle.cpp)
    target_link_libraries(spi_state_lifecycle PRIVATE fixture)
    add_test(NAME spi_state_lifecycle COMMAND spi_state_lifecycle)
    add_executable(combined_product_registry combined-product-registry.cpp)
    dtv_add_product_aggregate(combined_product_registry_aggregate
        PRODUCTS i0_asicen_fixture i0_torne_fixture)
    target_link_libraries(combined_product_registry PRIVATE combined_product_registry_aggregate)
    add_test(NAME combined_product_registry COMMAND combined_product_registry)
    add_test(NAME i0_pix_model_metadata
        COMMAND ${CMAKE_COMMAND}
            "-DCLI=$<TARGET_FILE:i0pixctl>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/pix-model-contract.cmake")
    add_test(NAME cli_contract_table
        COMMAND ${CMAKE_COMMAND}
            "-DCLI=$<TARGET_FILE:fixturectl>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../cli-contract.cmake")
endif()

if(BUILD_TESTING)
    add_executable(product_dispatch_contract product-dispatch.cpp)
    target_link_libraries(product_dispatch_contract PRIVATE fixture)
    add_test(NAME product_dispatch_contract COMMAND product_dispatch_contract)
    add_executable(asset_firmware_contract firmware-contract.cpp)
    target_link_libraries(asset_firmware_contract PRIVATE assetfixture)
    add_test(NAME asset_firmware_contract COMMAND asset_firmware_contract)
    add_executable(scan_complete_contract scan-complete.cpp)
    target_include_directories(scan_complete_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
    target_link_libraries(scan_complete_contract PRIVATE scanfixture ${DTV_LIBUSB_TARGET})
    add_test(NAME scan_complete_contract COMMAND scan_complete_contract)
    add_executable(discovery_registry_contract discovery-registry.cpp)
    target_link_libraries(discovery_registry_contract PRIVATE scanfixture ${DTV_LIBUSB_TARGET})
    add_test(NAME discovery_registry_contract COMMAND discovery_registry_contract)
    add_executable(usb_claim_lease_contract usb-claim-lease.cpp card-events-public.c)
    target_include_directories(usb_claim_lease_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
    target_compile_features(usb_claim_lease_contract PRIVATE c_std_11)
    target_link_libraries(usb_claim_lease_contract PRIVATE scanfixture ${DTV_LIBUSB_TARGET})
    add_test(NAME usb_claim_lease_contract COMMAND usb_claim_lease_contract)
    add_executable(discovery_public_contract discovery-public.c discovery-public.cpp)
    target_link_libraries(discovery_public_contract PRIVATE scanfixture ${DTV_LIBUSB_TARGET})
    add_test(NAME discovery_public_contract COMMAND discovery_public_contract)
    add_executable(product_list_public_contract product-list-public.c product-list-public.cpp)
    target_compile_features(product_list_public_contract PRIVATE c_std_11)
    target_link_libraries(product_list_public_contract PRIVATE combined_product_registry_aggregate)
    add_test(NAME product_list_public_contract COMMAND product_list_public_contract)
    add_executable(ungrouped_public_contract ungrouped-public.c ungrouped-public.cpp)
    target_compile_features(ungrouped_public_contract PRIVATE c_std_11)
    target_link_libraries(ungrouped_public_contract PRIVATE scanfixture fixture ${DTV_LIBUSB_TARGET})
    add_test(NAME ungrouped_public_contract COMMAND ungrouped_public_contract)
    add_executable(token_list_public_contract token-list-public.c token-list-public.cpp)
    target_compile_features(token_list_public_contract PRIVATE c_std_11)
    target_include_directories(token_list_public_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
    target_link_libraries(token_list_public_contract PRIVATE scanfixture ${DTV_LIBUSB_TARGET})
    add_test(NAME token_list_public_contract COMMAND token_list_public_contract)
    add_test(NAME token_list_unconfigured_contract COMMAND token_list_public_contract unconfigured)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(fd_list_public_contract fd-list-public.c fd-list-public.cpp)
        target_compile_features(fd_list_public_contract PRIVATE c_std_11)
        target_include_directories(fd_list_public_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
        target_link_libraries(fd_list_public_contract PRIVATE scanfixture ${DTV_LIBUSB_TARGET})
        add_test(NAME fd_list_public_contract COMMAND fd_list_public_contract)
    endif()
endif()
