# SPDX-License-Identifier: GPL-2.0-or-later
function(dtv_add_product)
    cmake_parse_arguments(PRODUCT "" "NAME;COMMAND;VERSION;DEFAULT_MODEL" "SOURCES" ${ARGN})
    if(NOT PRODUCT_NAME OR NOT PRODUCT_COMMAND OR NOT PRODUCT_VERSION OR NOT PRODUCT_SOURCES)
        message(FATAL_ERROR "dtv_add_product requires NAME, COMMAND, VERSION, and SOURCES")
    endif()

    add_library(${PRODUCT_NAME} STATIC ${PRODUCT_SOURCES})
    target_link_libraries(${PRODUCT_NAME} PUBLIC dtv::userland)
    target_compile_features(${PRODUCT_NAME} PUBLIC cxx_std_20)
    set_target_properties(${PRODUCT_NAME} PROPERTIES CXX_EXTENSIONS OFF)

    set(cli_source "${CMAKE_CURRENT_BINARY_DIR}/${PRODUCT_COMMAND}_main.cpp")
    if(NOT DEFINED PRODUCT_DEFAULT_MODEL)
        set(PRODUCT_DEFAULT_MODEL "")
    endif()
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../cli/main.cpp.in"
                   "${cli_source}" @ONLY)
    add_executable(${PRODUCT_COMMAND} "${cli_source}")
    target_link_libraries(${PRODUCT_COMMAND} PRIVATE ${PRODUCT_NAME} dtv::userland)
    set_target_properties(${PRODUCT_COMMAND} PROPERTIES CXX_EXTENSIONS OFF)
endfunction()
