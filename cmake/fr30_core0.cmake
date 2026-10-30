# SPDX-License-Identifier: GPL-2.0-only
#
# Explicit Core 0 candidate-image generation and checks. The normal product
# build consumes tracked output only and never needs an FR30 cross assembler.
#
# Required:
#   TORNE_FR30_BINUTILS_DIR  binutils 2.47 build tree or install prefix
#   TORNE_CORE0_OUTPUT_DIR   directory for the tracked ELF/bin/header/manifest
# Optional:
#   TORNE_CORE0_SCRATCH_DIR  scratch root (default: ./core0-scratch)
#   TORNE_CORE0_REFERENCE_DIR compare the generated files with this directory

cmake_minimum_required(VERSION 3.16)

set(_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_source "${_root}/fr30/src/core0.s")
set(_ld "${_root}/fr30/ld/core0.ld")
set(_base 0x000A0000)
set(_stack_start 0x000A3000)
set(_entry_hex "000a0000")

if(NOT DEFINED TORNE_FR30_BINUTILS_DIR OR TORNE_FR30_BINUTILS_DIR STREQUAL "")
    message(FATAL_ERROR
        "Core 0 generation needs TORNE_FR30_BINUTILS_DIR pointing at a "
        "binutils 2.47 fr30-elf build tree. See fr30/README.md.")
endif()
if(NOT DEFINED TORNE_CORE0_OUTPUT_DIR OR TORNE_CORE0_OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR
        "Core 0 generation needs TORNE_CORE0_OUTPUT_DIR for the tracked image.")
endif()
if(NOT DEFINED TORNE_CORE0_SCRATCH_DIR OR TORNE_CORE0_SCRATCH_DIR STREQUAL "")
    set(TORNE_CORE0_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/core0-scratch")
endif()

function(_core0_find_tool out_name)
    foreach(_candidate IN LISTS ARGN)
        if(EXISTS "${_candidate}")
            set(${out_name} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "FR30 tool not found under ${TORNE_FR30_BINUTILS_DIR}: ${ARGN}")
endfunction()

set(_binutils "${TORNE_FR30_BINUTILS_DIR}")
_core0_find_tool(CORE0_AS
    "${_binutils}/gas/as-new"
    "${_binutils}/bin/fr30-elf-as"
    "${_binutils}/bin/as")
_core0_find_tool(CORE0_LD
    "${_binutils}/ld/ld-new"
    "${_binutils}/bin/fr30-elf-ld"
    "${_binutils}/bin/ld")
_core0_find_tool(CORE0_OBJCOPY
    "${_binutils}/binutils/objcopy"
    "${_binutils}/bin/fr30-elf-objcopy"
    "${_binutils}/bin/objcopy")

function(_core0_link build_id elf)
    execute_process(
        COMMAND "${CORE0_LD}" -m elf32fr30 --fatal-warnings
            -T "${_ld}"
            "--defsym=__core0_build_id=${build_id}"
            -nostdlib -o "${elf}" "${CORE0_SCRATCH_OBJECT}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR "Core 0 link failed:\n${_out}${_err}")
    endif()
endfunction()

function(_core0_objcopy elf bin)
    execute_process(
        COMMAND "${CORE0_OBJCOPY}" -O binary "${elf}" "${bin}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR "Core 0 objcopy failed:\n${_out}${_err}")
    endif()
endfunction()

function(_core0_check_elf name elf)
    file(READ "${elf}" _hex HEX)
    string(LENGTH "${_hex}" _hex_length)
    if(_hex_length LESS 104)
        message(FATAL_ERROR "${name}: ${elf} is too short to be an ELF32 header")
    endif()
    string(SUBSTRING "${_hex}" 0 8 _magic)
    string(SUBSTRING "${_hex}" 8 2 _class)
    string(SUBSTRING "${_hex}" 10 2 _data)
    string(SUBSTRING "${_hex}" 32 4 _type)
    string(SUBSTRING "${_hex}" 36 4 _machine)
    string(SUBSTRING "${_hex}" 48 8 _entry)
    if(NOT _magic STREQUAL "7f454c46")
        message(FATAL_ERROR "${name}: not an ELF file")
    endif()
    if(NOT _class STREQUAL "01")
        message(FATAL_ERROR "${name}: ELF class is not ELF32")
    endif()
    if(NOT _data STREQUAL "02")
        message(FATAL_ERROR "${name}: ELF data is not big endian")
    endif()
    if(NOT _type STREQUAL "0002")
        message(FATAL_ERROR "${name}: ELF type is not ET_EXEC")
    endif()
    if(NOT _machine STREQUAL "0054")
        message(FATAL_ERROR "${name}: ELF machine is not Fujitsu FR30 (EM_FR30=84)")
    endif()
    if(NOT _entry STREQUAL "${_entry_hex}")
        message(FATAL_ERROR
            "${name}: entry is 0x${_entry}, expected 0x${_entry_hex}")
    endif()
endfunction()

function(_core0_check_bin name bin)
    file(READ "${bin}" _hex HEX)
    string(LENGTH "${_hex}" _hex_length)
    math(EXPR _bytes "${_hex_length} / 2")
    math(EXPR _mod "${_bytes} % 4")
    if(_bytes LESS_EQUAL 0)
        message(FATAL_ERROR "${name}: empty image")
    endif()
    if(NOT _mod EQUAL 0)
        message(FATAL_ERROR "${name}: image length is not a multiple of 4")
    endif()
    if(_bytes GREATER 0x3000)
        message(FATAL_ERROR "${name}: image exceeds 12 KiB")
    endif()
    string(SUBSTRING "${_hex}" 0 8 _prologue)
    if(NOT _prologue STREQUAL "9baf37fc")
        message(FATAL_ERROR "${name}: entry does not start with the stack setup")
    endif()
    set(_core0_bytes ${_bytes} PARENT_SCOPE)
endfunction()

function(_core0_check_build_id_pass pass1_bin pass2_bin build_id)
    file(READ "${pass1_bin}" _p1 HEX)
    file(READ "${pass2_bin}" _p2 HEX)
    string(LENGTH "${_p1}" _len)
    string(LENGTH "${_p2}" _len2)
    if(NOT _len STREQUAL _len2)
        message(FATAL_ERROR "Core 0 build_id pass changed the image length")
    endif()
    if(build_id STREQUAL "00000000")
        message(FATAL_ERROR "Core 0 build_id is zero")
    endif()
    math(EXPR _bytes "${_len} / 2")
    set(_matches 0)
    set(_field_pos -1)
    set(_i 0)
    while(_i LESS _bytes)
        math(EXPR _pos "${_i} * 2")
        string(SUBSTRING "${_p2}" ${_pos} 8 _field)
        if(_field STREQUAL "${build_id}")
            string(SUBSTRING "${_p1}" ${_pos} 8 _zero_field)
            if(_zero_field STREQUAL "00000000")
                string(SUBSTRING "${_p2}" 0 ${_pos} _head)
                math(EXPR _tail_start "${_pos} + 8")
                math(EXPR _tail_length "${_len} - ${_tail_start}")
                string(SUBSTRING "${_p2}" ${_tail_start} ${_tail_length} _tail)
                if("${_head}${_zero_field}${_tail}" STREQUAL "${_p1}")
                    math(EXPR _matches "${_matches} + 1")
                    set(_field_pos ${_i})
                endif()
            endif()
        endif()
        math(EXPR _i "${_i} + 1")
    endwhile()
    if(NOT _matches EQUAL 1)
        message(FATAL_ERROR
            "Core 0 build_id field is not a unique zeroed field (${_matches})")
    endif()
    math(EXPR _pos "${_field_pos} * 2")
    string(SUBSTRING "${_p2}" ${_pos} 8 _field)
    if(NOT _field STREQUAL "${build_id}")
        message(FATAL_ERROR "Core 0 build_id field is not the hash prefix")
    endif()
endfunction()

file(REMOVE_RECURSE "${TORNE_CORE0_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${TORNE_CORE0_SCRATCH_DIR}" "${TORNE_CORE0_OUTPUT_DIR}")

set(CORE0_SCRATCH_OBJECT "${TORNE_CORE0_SCRATCH_DIR}/core0.o")
execute_process(
    COMMAND "${CORE0_AS}" --fatal-warnings -o "${CORE0_SCRATCH_OBJECT}" "core0.s"
    WORKING_DIRECTORY "${_root}/fr30/src"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "Core 0 assemble failed:\n${_out}${_err}")
endif()

foreach(_run run1 run2)
    set(_dir "${TORNE_CORE0_SCRATCH_DIR}/${_run}")
    file(MAKE_DIRECTORY "${_dir}")
    _core0_link(0 "${_dir}/core0-pass1.elf")
    _core0_objcopy("${_dir}/core0-pass1.elf" "${_dir}/core0-pass1.bin")
    file(SHA256 "${_dir}/core0-pass1.bin" _pass1_sha)
    string(SUBSTRING "${_pass1_sha}" 0 8 _build_id)
    _core0_link("0x${_build_id}" "${_dir}/core0.elf")
    _core0_objcopy("${_dir}/core0.elf" "${_dir}/core0.bin")

    _core0_check_build_id_pass(
        "${_dir}/core0-pass1.bin" "${_dir}/core0.bin" "${_build_id}")
    _core0_check_elf("core0 ${_run}" "${_dir}/core0.elf")
    _core0_check_bin("core0 ${_run}" "${_dir}/core0.bin")
endforeach()

file(SHA256 "${TORNE_CORE0_SCRATCH_DIR}/run1/core0.bin" _bin_hash)
file(SHA256 "${TORNE_CORE0_SCRATCH_DIR}/run2/core0.bin" _bin_hash2)
file(SHA256 "${TORNE_CORE0_SCRATCH_DIR}/run1/core0.elf" _elf_hash)
file(SHA256 "${TORNE_CORE0_SCRATCH_DIR}/run2/core0.elf" _elf_hash2)
if(NOT _bin_hash STREQUAL _bin_hash2)
    message(FATAL_ERROR "Core 0 .bin differs between output directories")
endif()
if(NOT _elf_hash STREQUAL _elf_hash2)
    message(FATAL_ERROR "Core 0 .elf differs between output directories")
endif()

file(COPY "${TORNE_CORE0_SCRATCH_DIR}/run1/core0.bin"
    DESTINATION "${TORNE_CORE0_OUTPUT_DIR}"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
file(COPY "${TORNE_CORE0_SCRATCH_DIR}/run1/core0.elf"
    DESTINATION "${TORNE_CORE0_OUTPUT_DIR}"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

file(READ "${TORNE_CORE0_SCRATCH_DIR}/run1/core0.bin" _final_hex HEX)
string(LENGTH "${_final_hex}" _final_hex_length)
math(EXPR _image_bytes "${_final_hex_length} / 2")
file(SHA256 "${TORNE_CORE0_SCRATCH_DIR}/run1/core0.bin" _final_bin_hash)
file(SHA256 "${TORNE_CORE0_SCRATCH_DIR}/run1/core0.elf" _final_elf_hash)
file(SHA256 "${_source}" _source_hash)
file(SHA256 "${_ld}" _ld_hash)
file(SHA256 "${TORNE_CORE0_SCRATCH_DIR}/run1/core0-pass1.bin" _pass1_hash)
string(SUBSTRING "${_pass1_hash}" 0 8 _build_id)

function(_core0_format_bytes hex out)
    string(LENGTH "${hex}" _hex_length)
    math(EXPR _bytes "${_hex_length} / 2")
    set(_body "")
    set(_line "")
    set(_count 0)
    set(_i 0)
    while(_i LESS _bytes)
        math(EXPR _pos "${_i} * 2")
        string(SUBSTRING "${hex}" ${_pos} 2 _byte)
        if(_line STREQUAL "")
            set(_line "    0x${_byte}")
        else()
            set(_line "${_line}, 0x${_byte}")
        endif()
        math(EXPR _count "${_count} + 1")
        if(_count EQUAL 12)
            string(APPEND _body "${_line},\n")
            set(_line "")
            set(_count 0)
        endif()
        math(EXPR _i "${_i} + 1")
    endwhile()
    if(NOT _line STREQUAL "")
        string(APPEND _body "${_line},\n")
    endif()
    set(${out} "${_body}" PARENT_SCOPE)
endfunction()

_core0_format_bytes("${_final_hex}" _c_bytes)

string(CONCAT _header
    "/* SPDX-License-Identifier: GPL-2.0-only */\n"
    "/* Generated by cmake/fr30_core0.cmake from fr30/src/core0.s. Do not edit. */\n"
    "#ifndef TORNE_CORE0_IMAGES_H\n"
    "#define TORNE_CORE0_IMAGES_H\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n\n"
    "#define TORNE_CORE0_BASE 0x000a0000u\n"
    "#define TORNE_CORE0_ENTRY 0x000a0000u\n"
    "#define TORNE_CORE0_IMAGE_BYTES ${_image_bytes}u\n"
    "#define TORNE_CORE0_BUILD_ID 0x${_build_id}u\n"
    "#define TORNE_CORE0_SHA256 \"${_final_bin_hash}\"\n"
    "static const unsigned char torne_core0_bytes[] = {\n"
    "${_c_bytes}"
    "};\n\n"
    "#endif\n")

string(CONCAT _manifest
    "{\n"
    "  \"format\": \"torne-core0-manifest-v1\",\n"
    "  \"status\": \"candidate-unverified-on-device\",\n"
    "  \"source\": \"fr30/src/core0.s\",\n"
    "  \"source_sha256\": \"${_source_hash}\",\n"
    "  \"linker\": \"fr30/ld/core0.ld\",\n"
    "  \"linker_sha256\": \"${_ld_hash}\",\n"
    "  \"tool\": \"GNU binutils 2.47 fr30-elf\",\n"
    "  \"base\": \"0x000a0000\",\n"
    "  \"entry\": \"0x000a0000\",\n"
    "  \"image_bytes\": ${_image_bytes},\n"
    "  \"build_id\": \"0x${_build_id}\",\n"
    "  \"bin_file\": \"core0.bin\",\n"
    "  \"bin_sha256\": \"${_final_bin_hash}\",\n"
    "  \"elf_file\": \"core0.elf\",\n"
    "  \"elf_sha256\": \"${_final_elf_hash}\",\n"
    "  \"stack\": {\"start\": \"0x000a3000\", \"initial_sp\": \"0x000a37fc\"},\n"
    "  \"shared\": {\"base\": \"0x000a3800\", \"end_exclusive\": \"0x000a4000\"},\n"
    "  \"reset_vector\": {\"host\": \"0x016ffffc\", \"local\": \"0x000ffffc\", \"value\": \"0x000a0000\"},\n"
    "  \"core0\": {\"host_base\": \"0x01600000\", \"magic\": \"0x54545330\", \"abi\": \"0x00010000\", \"capabilities\": \"0x00000001\", \"fifo_capacity\": \"0x00000840\"},\n"
    "  \"note\": \"Candidate only. Host load, HOLD/RUN, TS engine, USB DMA, V-MEM and V-C0 were not executed.\"\n"
    "}\n")
file(WRITE "${TORNE_CORE0_SCRATCH_DIR}/core0-images.h" "${_header}")
file(WRITE "${TORNE_CORE0_SCRATCH_DIR}/core0-manifest.json" "${_manifest}")
file(COPY "${TORNE_CORE0_SCRATCH_DIR}/core0-images.h"
    "${TORNE_CORE0_SCRATCH_DIR}/core0-manifest.json"
    DESTINATION "${TORNE_CORE0_OUTPUT_DIR}"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# Negative checks: capacity overflow and missing entry must both fail.
execute_process(
    COMMAND "${CORE0_LD}" -m elf32fr30 --fatal-warnings
        -T "${_ld}"
        "--defsym=__core0_build_id=0"
        "--defsym=__core0_capacity_limit=0x100"
        -nostdlib -o "${TORNE_CORE0_SCRATCH_DIR}/overflow.elf"
        "${CORE0_SCRATCH_OBJECT}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(_rc STREQUAL "0")
    message(FATAL_ERROR "Core 0 capacity-overflow self-check was not rejected")
endif()
string(FIND "${_out}${_err}" "Core 0 code exceeds capacity" _found)
if(_found EQUAL -1)
    message(FATAL_ERROR "Core 0 capacity self-check failed for another reason")
endif()

execute_process(
    COMMAND "${CORE0_LD}" -m elf32fr30 --fatal-warnings
        -T "${_ld}"
        "--defsym=__core0_build_id=0"
        -e _missing
        -nostdlib -o "${TORNE_CORE0_SCRATCH_DIR}/bad-entry.elf"
        "${CORE0_SCRATCH_OBJECT}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(_rc STREQUAL "0")
    message(FATAL_ERROR "Core 0 invalid-entry self-check was not rejected")
endif()
string(FIND "${_out}${_err}" "cannot find entry symbol _missing" _found)
if(_found EQUAL -1)
    message(FATAL_ERROR "Core 0 entry self-check failed for another reason")
endif()

if(DEFINED TORNE_CORE0_REFERENCE_DIR AND NOT TORNE_CORE0_REFERENCE_DIR STREQUAL "")
    foreach(_file core0.bin core0.elf core0-images.h core0-manifest.json)
        if(NOT EXISTS "${TORNE_CORE0_REFERENCE_DIR}/${_file}")
            message(FATAL_ERROR "tracked Core 0 file is missing: ${_file}")
        endif()
        file(SHA256 "${TORNE_CORE0_OUTPUT_DIR}/${_file}" _new_hash)
        file(SHA256 "${TORNE_CORE0_REFERENCE_DIR}/${_file}" _old_hash)
        if(NOT _new_hash STREQUAL _old_hash)
            message(FATAL_ERROR
                "tracked Core 0 file is stale: ${_file}\n"
                "  generated: ${_new_hash}\n"
                "  tracked:   ${_old_hash}")
        endif()
    endforeach()
endif()

message(STATUS "Core 0 candidate image: PASS")
