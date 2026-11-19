# SPDX-License-Identifier: GPL-2.0-or-later
# This maintainer-only script assembles the tracked raw image set after the
# individual FR30 image generators have completed.

cmake_minimum_required(VERSION 3.16)

set(_root "${CMAKE_CURRENT_LIST_DIR}/..")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${_root}/scripts/fr30/generate_firmware_asset.py"
        --input-dir "${_root}/fr30/generated"
        --output-dir "${_root}/fr30/generated"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "FR30 asset generation failed:\n${_out}${_err}")
endif()
