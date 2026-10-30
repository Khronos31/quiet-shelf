# SPDX-License-Identifier: GPL-2.0-only
#
# Explicit FR30 fixture generation and checks. This script is run only by the
# torne_fr30_generate target or the torne_fr30_generation test; the normal
# product build and install never require an FR30 cross assembler.
#
# Required input:
#   TORNE_FR30_BINUTILS_DIR  binutils 2.47 build tree (or install prefix) with
#                            the fr30-elf assembler, linker, and objcopy.
# Optional:
#   TORNE_FR30_OUTPUT_DIR    output root (default: ./fr30-generated).

cmake_minimum_required(VERSION 3.16)

set(_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_fixtures "${_root}/tests/fixtures/fr30")

if(NOT DEFINED TORNE_FR30_OUTPUT_DIR OR TORNE_FR30_OUTPUT_DIR STREQUAL "")
    set(TORNE_FR30_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/fr30-generated")
endif()

if(NOT DEFINED TORNE_FR30_BINUTILS_DIR OR TORNE_FR30_BINUTILS_DIR STREQUAL "")
    message(FATAL_ERROR
        "FR30 generation needs TORNE_FR30_BINUTILS_DIR pointing at a "
        "binutils 2.47 fr30-elf build tree. See fr30/README.md.")
endif()

function(_fr30_find_tool out_name)
    set(_candidates ${ARGN})
    foreach(_candidate IN LISTS _candidates)
        if(EXISTS "${_candidate}")
            set(${out_name} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "FR30 tool not found under ${TORNE_FR30_BINUTILS_DIR}: ${ARGN}")
endfunction()

set(_binutils "${TORNE_FR30_BINUTILS_DIR}")
_fr30_find_tool(FR30_AS
    "${_binutils}/gas/as-new"
    "${_binutils}/bin/fr30-elf-as"
    "${_binutils}/bin/as")
_fr30_find_tool(FR30_LD
    "${_binutils}/ld/ld-new"
    "${_binutils}/bin/fr30-elf-ld"
    "${_binutils}/bin/ld")
_fr30_find_tool(FR30_OBJCOPY
    "${_binutils}/binutils/objcopy"
    "${_binutils}/bin/fr30-elf-objcopy"
    "${_binutils}/bin/objcopy")

# The fixture limits below are bounds for this probe only. Widen's 0x800 B is
# the fixed-coax candidate, not a confirmed contract. Core-local RAM sizes are
# not fixed by the contract, so no product image uses these values.
function(_fr30_generate target ld entry limit run)
    set(_out "${TORNE_FR30_OUTPUT_DIR}/${run}")
    file(MAKE_DIRECTORY "${_out}")
    set(_obj "${_out}/${target}.o")
    set(_elf "${_out}/${target}.elf")
    set(_bin "${_out}/${target}.bin")

    execute_process(
        COMMAND "${FR30_AS}" --fatal-warnings -o "${_obj}"
            "${_fixtures}/probe.s"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out_text
        ERROR_VARIABLE _err_text)
    if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR
            "FR30 assemble failed for ${target} (${run}):\n${_out_text}${_err_text}")
    endif()

    execute_process(
        COMMAND "${FR30_LD}" -m elf32fr30 --fatal-warnings
            -T "${_fixtures}/${ld}"
            "--defsym=__fr30_text_limit=${limit}"
            -nostdlib -o "${_elf}" "${_obj}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out_text
        ERROR_VARIABLE _err_text)
    if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR
            "FR30 link failed for ${target} (${run}):\n${_out_text}${_err_text}")
    endif()

    execute_process(
        COMMAND "${FR30_OBJCOPY}" -O binary "${_elf}" "${_bin}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out_text
        ERROR_VARIABLE _err_text)
    if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR
            "FR30 objcopy failed for ${target} (${run}):\n${_out_text}${_err_text}")
    endif()
endfunction()

function(_fr30_check_elf target elf entry_hex)
    file(READ "${elf}" _hex HEX)
    string(LENGTH "${_hex}" _hex_length)
    if(_hex_length LESS 104)
        message(FATAL_ERROR "${elf} is too short to be an ELF32 header")
    endif()
    string(SUBSTRING "${_hex}" 0 8 _magic)
    string(SUBSTRING "${_hex}" 8 2 _class)
    string(SUBSTRING "${_hex}" 10 2 _data)
    string(SUBSTRING "${_hex}" 32 4 _type)
    string(SUBSTRING "${_hex}" 36 4 _machine)
    string(SUBSTRING "${_hex}" 48 8 _entry)
    if(NOT _magic STREQUAL "7f454c46")
        message(FATAL_ERROR "${target}: not an ELF file")
    endif()
    if(NOT _class STREQUAL "01")
        message(FATAL_ERROR "${target}: ELF class is not ELF32")
    endif()
    if(NOT _data STREQUAL "02")
        message(FATAL_ERROR "${target}: ELF data is not big endian")
    endif()
    if(NOT _type STREQUAL "0002")
        message(FATAL_ERROR "${target}: ELF type is not ET_EXEC")
    endif()
    if(NOT _machine STREQUAL "0054")
        message(FATAL_ERROR "${target}: ELF machine is not Fujitsu FR30 (EM_FR30=84)")
    endif()
    if(NOT _entry STREQUAL "${entry_hex}")
        message(FATAL_ERROR
            "${target}: entry is 0x${_entry}, expected 0x${entry_hex}")
    endif()
endfunction()

function(_fr30_check_bin target bin)
    file(READ "${bin}" _hex HEX)
    string(LENGTH "${_hex}" _hex_length)
    math(EXPR _bytes "${_hex_length} / 2")
    math(EXPR _remainder "${_bytes} % 4")
    if(NOT _remainder EQUAL 0)
        message(FATAL_ERROR
            "${target}: ${_bytes} B is not aligned to the 4 B transfer unit")
    endif()
    # ldi:8 #0x2a,r0 / ldi:20 #0x12345,r1 / nop / ret
    set(_expected "c2a09b1123459fa097209fa0")
    if(NOT _hex STREQUAL "${_expected}")
        message(FATAL_ERROR
            "${target}: unexpected encoding ${_hex}, expected ${_expected}")
    endif()
endfunction()

function(_fr30_expect_failure label expected_text)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out_text
        ERROR_VARIABLE _err_text)
    if(_rc STREQUAL "0")
        message(FATAL_ERROR "${label}: expected failure but the command succeeded")
    endif()
    string(FIND "${_out_text}${_err_text}" "${expected_text}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR
            "${label}: failure did not report '${expected_text}':\n${_out_text}${_err_text}")
    endif()
endfunction()

file(REMOVE_RECURSE
    "${TORNE_FR30_OUTPUT_DIR}/run1"
    "${TORNE_FR30_OUTPUT_DIR}/run2")

set(_targets widen widen-bank1 core0 core1)
foreach(_target IN LISTS _targets)
    if(_target STREQUAL "widen")
        set(_ld "widen.ld")
        set(_entry "000e1000")
        set(_limit "0x800")
    elseif(_target STREQUAL "widen-bank1")
        set(_ld "widen-bank1.ld")
        set(_entry "000e1800")
        set(_limit "0x800")
    elseif(_target STREQUAL "core0")
        set(_ld "core0.ld")
        set(_entry "000a0000")
        set(_limit "0x1000")
    else()
        set(_ld "core1.ld")
        set(_entry "000a0000")
        set(_limit "0x1000")
    endif()

    foreach(_run run1 run2)
        _fr30_generate("${_target}" "${_ld}" "${_entry}" "${_limit}" "${_run}")
    endforeach()

    _fr30_check_elf("${_target}" "${TORNE_FR30_OUTPUT_DIR}/run1/${_target}.elf" "${_entry}")
    _fr30_check_bin("${_target}" "${TORNE_FR30_OUTPUT_DIR}/run1/${_target}.bin")

    file(SHA256 "${TORNE_FR30_OUTPUT_DIR}/run1/${_target}.bin" _bin1)
    file(SHA256 "${TORNE_FR30_OUTPUT_DIR}/run2/${_target}.bin" _bin2)
    file(SHA256 "${TORNE_FR30_OUTPUT_DIR}/run1/${_target}.elf" _elf1)
    file(SHA256 "${TORNE_FR30_OUTPUT_DIR}/run2/${_target}.elf" _elf2)
    if(NOT _bin1 STREQUAL _bin2)
        message(FATAL_ERROR "${_target}: .bin differs between output directories")
    endif()
    if(NOT _elf1 STREQUAL _elf2)
        message(FATAL_ERROR "${_target}: .elf differs between output directories")
    endif()
    message(STATUS "${_target}: PASS (entry 0x${_entry}, bin ${_bin1})")
endforeach()

# Failure checks. Use one successfully assembled object so that only the link
# step under test can fail.
set(_probe_object "${TORNE_FR30_OUTPUT_DIR}/run1/widen.o")

_fr30_expect_failure("capacity overflow"
    "FR30 .text exceeds limit"
    "${FR30_LD}" -m elf32fr30 --fatal-warnings
        -T "${_fixtures}/widen.ld"
        "--defsym=__fr30_text_limit=8"
        -nostdlib
        -o "${TORNE_FR30_OUTPUT_DIR}/overflow.elf" "${_probe_object}")

_fr30_expect_failure("invalid entry"
    "cannot find entry symbol _missing"
    "${FR30_LD}" -m elf32fr30 --fatal-warnings
        -T "${_fixtures}/widen.ld"
        "--defsym=__fr30_text_limit=0x800"
        -e _missing
        -nostdlib
        -o "${TORNE_FR30_OUTPUT_DIR}/bad-entry.elf" "${_probe_object}")

_fr30_expect_failure("missing capacity limit"
    "undefined symbol"
    "${FR30_LD}" -m elf32fr30 --fatal-warnings
        -T "${_fixtures}/widen.ld"
        -nostdlib
        -o "${TORNE_FR30_OUTPUT_DIR}/no-limit.elf" "${_probe_object}")

message(STATUS "FR30 fixture generation: PASS")
