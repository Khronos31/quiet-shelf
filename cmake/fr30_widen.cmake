# SPDX-License-Identifier: GPL-2.0-only
#
# Explicit Widen candidate-image generation and checks. The normal product
# build does not run this script and does not need an FR30 cross assembler.
#
# Required:
#   TORNE_FR30_BINUTILS_DIR   binutils 2.47 build tree or install prefix
#   TORNE_WIDEN_OUTPUT_DIR    directory for the tracked ELF/bin/header/manifest
# Optional:
#   TORNE_WIDEN_SCRATCH_DIR   scratch root (default: ./widen-scratch)
#   TORNE_WIDEN_REFERENCE_DIR compare the generated files with this directory

cmake_minimum_required(VERSION 3.16)

set(_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_source "${_root}/fr30/src/widen.s")
set(_ld_dir "${_root}/fr30/ld")

if(NOT DEFINED TORNE_FR30_BINUTILS_DIR OR TORNE_FR30_BINUTILS_DIR STREQUAL "")
    message(FATAL_ERROR
        "Widen generation needs TORNE_FR30_BINUTILS_DIR pointing at a "
        "binutils 2.47 fr30-elf build tree. See fr30/README.md.")
endif()
if(NOT DEFINED TORNE_WIDEN_OUTPUT_DIR OR TORNE_WIDEN_OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR
        "Widen generation needs TORNE_WIDEN_OUTPUT_DIR for the tracked images.")
endif()
if(NOT DEFINED TORNE_WIDEN_SCRATCH_DIR OR TORNE_WIDEN_SCRATCH_DIR STREQUAL "")
    set(TORNE_WIDEN_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/widen-scratch")
endif()

function(_widen_find_tool out_name)
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
_widen_find_tool(WIDEN_AS
    "${_binutils}/gas/as-new"
    "${_binutils}/bin/fr30-elf-as"
    "${_binutils}/bin/as")
_widen_find_tool(WIDEN_LD
    "${_binutils}/ld/ld-new"
    "${_binutils}/bin/fr30-elf-ld"
    "${_binutils}/bin/ld")
_widen_find_tool(WIDEN_OBJCOPY
    "${_binutils}/binutils/objcopy"
    "${_binutils}/bin/fr30-elf-objcopy"
    "${_binutils}/bin/objcopy")

function(_widen_hex8 value out)
    set(_n "${value}")
    set(_s "")
    set(_digits "0123456789abcdef")
    foreach(_i RANGE 0 7)
        math(EXPR _d "${_n} & 0xF")
        string(SUBSTRING "${_digits}" ${_d} 1 _c)
        set(_s "${_c}${_s}")
        math(EXPR _n "${_n} >> 4")
    endforeach()
    set(${out} "${_s}" PARENT_SCOPE)
endfunction()

function(_widen_jump_word entry out)
    math(EXPR _word
        "((0x9B00 | ((((${entry}) >> 16) & 0xF) << 4) | 0xC) << 16) | ((${entry}) & 0xFFFF)")
    _widen_hex8(${_word} _hex)
    set(${out} "${_hex}" PARENT_SCOPE)
endfunction()

function(_widen_format_bytes hex out)
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

function(_widen_link ld build_id elf)
    execute_process(
        COMMAND "${WIDEN_LD}" -m elf32fr30 --fatal-warnings
            -T "${_ld_dir}/${ld}"
            "--defsym=__widen_build_id=${build_id}"
            -nostdlib -o "${elf}" "${WIDEN_SCRATCH_OBJECT}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR "Widen link failed (${ld}):\n${_out}${_err}")
    endif()
endfunction()

function(_widen_objcopy elf bin)
    execute_process(
        COMMAND "${WIDEN_OBJCOPY}" -O binary "${elf}" "${bin}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR "Widen objcopy failed (${bin}):\n${_out}${_err}")
    endif()
endfunction()

function(_widen_check_elf name elf entry_hex)
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
    if(NOT _entry STREQUAL "${entry_hex}")
        message(FATAL_ERROR
            "${name}: ELF entry is 0x${_entry}, expected 0x${entry_hex}")
    endif()
endfunction()

# Validate an image as a hex string. This keeps the negative checks able to
# corrupt a copy without writing a malformed binary.
function(_widen_validate_hex name hex base expected_build_id out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")
    string(LENGTH "${hex}" _hex_length)
    if(_hex_length LESS 64)
        set(_ok FALSE)
        set(_reason "image is shorter than the 0x20 B header")
        set(${out_ok} ${_ok} PARENT_SCOPE)
        set(${out_reason} "${_reason}" PARENT_SCOPE)
        return()
    endif()
    math(EXPR _bytes "${_hex_length} / 2")
    string(SUBSTRING "${hex}" 0 8 _magic)
    string(SUBSTRING "${hex}" 8 8 _abi)
    string(SUBSTRING "${hex}" 16 8 _build_id)
    string(SUBSTRING "${hex}" 24 8 _caps)
    string(SUBSTRING "${hex}" 32 8 _size_hex)
    string(SUBSTRING "${hex}" 40 8 _read_hex)
    string(SUBSTRING "${hex}" 48 8 _write_hex)
    string(SUBSTRING "${hex}" 56 8 _reserved)
    math(EXPR _image_bytes "0x${_size_hex}")
    math(EXPR _read "0x${_read_hex}")
    math(EXPR _write "0x${_write_hex}")
    if(NOT _magic STREQUAL "5457444e")
        set(_ok FALSE)
        set(_reason "header magic is not TWDN")
    elseif(NOT _abi STREQUAL "00010000")
        set(_ok FALSE)
        set(_reason "header ABI is not 0x00010000")
    elseif(NOT _build_id STREQUAL "${expected_build_id}")
        set(_ok FALSE)
        set(_reason "header build_id is not the hash prefix")
    elseif(NOT _caps STREQUAL "00000001")
        set(_ok FALSE)
        set(_reason "header capabilities are not WIDEN_RW only")
    elseif(NOT _image_bytes EQUAL _bytes)
        set(_ok FALSE)
        set(_reason "header image_bytes ${_image_bytes} does not match ${_bytes}")
    elseif(_image_bytes LESS_EQUAL 32)
        set(_ok FALSE)
        set(_reason "image has no code")
    elseif(_image_bytes GREATER 2048)
        set(_ok FALSE)
        set(_reason "image exceeds the 0x800 B bank")
    elseif(NOT _reserved STREQUAL "00000000")
        set(_ok FALSE)
        set(_reason "reserved header word is not zero")
    endif()

    if(_ok)
        math(EXPR _bytes_mod "${_bytes} % 4")
        math(EXPR _lower "${base} + 0x20")
        math(EXPR _upper "${base} + ${_image_bytes}")
        if(NOT _bytes_mod EQUAL 0)
            set(_ok FALSE)
            set(_reason "image length is not a multiple of 4")
        elseif(_read LESS _lower OR NOT _read LESS _upper)
            set(_ok FALSE)
            set(_reason "entry_read is outside the image code area")
        elseif(_write LESS _lower OR NOT _write LESS _upper)
            set(_ok FALSE)
            set(_reason "entry_write is outside the image code area")
        endif()
    endif()
    if(_ok)
        math(EXPR _read_mod "${_read} % 4")
        math(EXPR _write_mod "${_write} % 4")
        if(NOT _read_mod EQUAL 0 OR NOT _write_mod EQUAL 0)
            set(_ok FALSE)
            set(_reason "an entry is not 4-byte aligned")
        endif()
    endif()
    if(_ok)
        math(EXPR _read_offset "(${_read} - ${base}) * 2")
        math(EXPR _write_offset "(${_write} - ${base}) * 2")
        string(SUBSTRING "${hex}" ${_read_offset} 4 _read_sig)
        string(SUBSTRING "${hex}" ${_write_offset} 4 _write_sig)
        if(NOT _read_sig STREQUAL "8b81")
            set(_ok FALSE)
            set(_reason "entry_read does not start with the read-handler prologue")
        elseif(NOT _write_sig STREQUAL "8ba1")
            set(_ok FALSE)
            set(_reason "entry_write does not start with the write-handler prologue")
        endif()
    endif()
    set(${out_ok} ${_ok} PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

function(_widen_require_valid name hex base build_id)
    _widen_validate_hex("${name}" "${hex}" ${base} "${build_id}" _ok _reason)
    if(NOT _ok)
        message(FATAL_ERROR "${name}: ${_reason}")
    endif()
endfunction()

file(REMOVE_RECURSE "${TORNE_WIDEN_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${TORNE_WIDEN_SCRATCH_DIR}" "${TORNE_WIDEN_OUTPUT_DIR}")

set(WIDEN_SCRATCH_OBJECT "${TORNE_WIDEN_SCRATCH_DIR}/widen.o")
execute_process(
    COMMAND "${WIDEN_AS}" --fatal-warnings -o "${WIDEN_SCRATCH_OBJECT}" "widen.s"
    WORKING_DIRECTORY "${_root}/fr30/src"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "Widen assemble failed:\n${_out}${_err}")
endif()

set(_images "")
set(_header "")
set(_manifest_images "")
set(_banks bank0 bank1)
foreach(_bank IN LISTS _banks)
    if(_bank STREQUAL "bank0")
        set(_base 0x000E1000)
        set(_base_hex "000e1000")
        set(_bank_number 0)
        set(_ld "widen-bank0.ld")
    else()
        set(_base 0x000E1800)
        set(_base_hex "000e1800")
        set(_bank_number 1)
        set(_ld "widen-bank1.ld")
    endif()
    string(TOUPPER "${_bank}" _bank_upper)

    foreach(_run run1 run2)
        set(_dir "${TORNE_WIDEN_SCRATCH_DIR}/${_run}")
        file(MAKE_DIRECTORY "${_dir}")
        _widen_link("${_ld}" 0 "${_dir}/${_bank}-pass1.elf")
        _widen_objcopy("${_dir}/${_bank}-pass1.elf" "${_dir}/${_bank}-pass1.bin")
        file(SHA256 "${_dir}/${_bank}-pass1.bin" _pass1_sha)
        string(SUBSTRING "${_pass1_sha}" 0 8 _build_id)
        _widen_link("${_ld}" "0x${_build_id}" "${_dir}/${_bank}.elf")
        _widen_objcopy("${_dir}/${_bank}.elf" "${_dir}/${_bank}.bin")

        file(READ "${_dir}/${_bank}-pass1.bin" _pass1_hex HEX)
        file(READ "${_dir}/${_bank}.bin" _final_hex HEX)
        string(LENGTH "${_pass1_hex}" _pass1_length)
        math(EXPR _tail_start "16 + 8")
        math(EXPR _tail_length "${_pass1_length} - ${_tail_start}")
        string(SUBSTRING "${_pass1_hex}" 0 16 _pass1_head)
        string(SUBSTRING "${_pass1_hex}" ${_tail_start} ${_tail_length} _pass1_tail)
        string(SUBSTRING "${_final_hex}" 0 16 _final_head)
        string(SUBSTRING "${_final_hex}" ${_tail_start} ${_tail_length} _final_tail)
        string(SUBSTRING "${_final_hex}" 16 8 _final_build_id)
        if(NOT _final_build_id STREQUAL "${_build_id}")
            message(FATAL_ERROR "${_bank}: final build_id field is wrong")
        endif()
        if(NOT _pass1_head STREQUAL _final_head OR NOT _pass1_tail STREQUAL _final_tail)
            message(FATAL_ERROR "${_bank}: build_id pass changed bytes outside +0x08")
        endif()

        _widen_require_valid("${_bank} ${_run}" "${_final_hex}" ${_base} "${_build_id}")
        string(SUBSTRING "${_final_hex}" 40 8 _entry_read_hex)
        _widen_check_elf("${_bank} ${_run}" "${_dir}/${_bank}.elf" "${_entry_read_hex}")
    endforeach()

    file(SHA256 "${TORNE_WIDEN_SCRATCH_DIR}/run1/${_bank}.bin" _bin_hash)
    file(SHA256 "${TORNE_WIDEN_SCRATCH_DIR}/run2/${_bank}.bin" _bin_hash2)
    file(SHA256 "${TORNE_WIDEN_SCRATCH_DIR}/run1/${_bank}.elf" _elf_hash)
    file(SHA256 "${TORNE_WIDEN_SCRATCH_DIR}/run2/${_bank}.elf" _elf_hash2)
    if(NOT _bin_hash STREQUAL _bin_hash2)
        message(FATAL_ERROR "${_bank}: .bin differs between output directories")
    endif()
    if(NOT _elf_hash STREQUAL _elf_hash2)
        message(FATAL_ERROR "${_bank}: .elf differs between output directories")
    endif()

    file(COPY "${TORNE_WIDEN_SCRATCH_DIR}/run1/${_bank}.bin"
        DESTINATION "${TORNE_WIDEN_OUTPUT_DIR}"
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    file(RENAME "${TORNE_WIDEN_OUTPUT_DIR}/${_bank}.bin"
        "${TORNE_WIDEN_OUTPUT_DIR}/widen-${_bank}.bin")
    file(COPY "${TORNE_WIDEN_SCRATCH_DIR}/run1/${_bank}.elf"
        DESTINATION "${TORNE_WIDEN_OUTPUT_DIR}"
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    file(RENAME "${TORNE_WIDEN_OUTPUT_DIR}/${_bank}.elf"
        "${TORNE_WIDEN_OUTPUT_DIR}/widen-${_bank}.elf")

    file(READ "${TORNE_WIDEN_SCRATCH_DIR}/run1/${_bank}.bin" _final_hex HEX)
    string(SUBSTRING "${_final_hex}" 16 8 _build_id)
    string(SUBSTRING "${_final_hex}" 32 8 _size_hex)
    string(SUBSTRING "${_final_hex}" 40 8 _read_hex)
    string(SUBSTRING "${_final_hex}" 48 8 _write_hex)
    math(EXPR _image_bytes "0x${_size_hex}")
    math(EXPR _read "0x${_read_hex}")
    math(EXPR _write "0x${_write_hex}")
    _widen_jump_word(${_read} _jump_read)
    _widen_jump_word(${_write} _jump_write)
    file(SHA256 "${_source}" _source_hash)
    file(SHA256 "${_ld_dir}/${_ld}" _ld_hash)
    _widen_format_bytes("${_final_hex}" _c_bytes)

    string(APPEND _images
        "  {\n"
        "    \"bank\": ${_bank_number},\n"
        "    \"base\": \"0x${_base_hex}\",\n"
        "    \"image_bytes\": ${_image_bytes},\n"
        "    \"entry_read\": \"0x${_read_hex}\",\n"
        "    \"entry_write\": \"0x${_write_hex}\",\n"
        "    \"build_id\": \"0x${_build_id}\",\n"
        "    \"capabilities\": \"0x00000001\",\n"
        "    \"bin_file\": \"widen-${_bank}.bin\",\n"
        "    \"bin_sha256\": \"${_bin_hash}\",\n"
        "    \"elf_file\": \"widen-${_bank}.elf\",\n"
        "    \"elf_sha256\": \"${_elf_hash}\",\n"
        "    \"ld_script\": \"fr30/ld/${_ld}\",\n"
        "    \"ld_sha256\": \"${_ld_hash}\",\n"
        "    \"patches\": [\n"
        "      {\"address\": \"0x000e9ac0\", \"original\": \"0x9b800000\", \"new\": \"0x${_jump_read}\", \"role\": \"read jump word 1\"},\n"
        "      {\"address\": \"0x000e9ac4\", \"original\": \"0xaa04e40f\", \"new\": \"0x970c9fa0\", \"role\": \"read jump word 2\"},\n"
        "      {\"address\": \"0x000e9e0c\", \"original\": \"0x9b800000\", \"new\": \"0x${_jump_write}\", \"role\": \"write jump word 1\"},\n"
        "      {\"address\": \"0x000e9e10\", \"original\": \"0xaa04e40d\", \"new\": \"0x970c9fa0\", \"role\": \"write jump word 2\"},\n"
        "      {\"address\": \"0x000e9d94\", \"original\": \"0xb4279f8d\", \"new\": \"0x9fa09f8d\", \"role\": \"multi-word write shift\"}\n"
        "    ]\n"
        "  }")

    string(APPEND _header
        "#define TORNE_WIDEN_${_bank_upper}_BASE 0x${_base_hex}u\n"
        "#define TORNE_WIDEN_${_bank_upper}_ENTRY_READ 0x${_read_hex}u\n"
        "#define TORNE_WIDEN_${_bank_upper}_ENTRY_WRITE 0x${_write_hex}u\n"
        "#define TORNE_WIDEN_${_bank_upper}_BUILD_ID 0x${_build_id}u\n"
        "#define TORNE_WIDEN_${_bank_upper}_SHA256 \"${_bin_hash}\"\n"
        "static const unsigned char torne_widen_${_bank}_bytes[] = {\n"
        "${_c_bytes}"
        "};\n\n")
    if(_bank STREQUAL "bank0")
        set(_bank0_build_id "${_build_id}")
        set(_manifest_images "${_images}")
        set(_images "")
    endif()
    set(_final_hex_${_bank} "${_final_hex}")
endforeach()

# Rebuild the image list with comma separators between the two entries.
string(APPEND _manifest_images ",\n${_images}")
file(SHA256 "${_source}" _source_hash)
string(APPEND _header
    "#define TORNE_WIDEN_SOURCE_SHA256 \"${_source_hash}\"\n")

string(CONCAT _manifest
    "{\n"
    "  \"format\": \"torne-widen-manifest-v1\",\n"
    "  \"status\": \"candidate-unverified-on-device\",\n"
    "  \"source\": \"fr30/src/widen.s\",\n"
    "  \"source_sha256\": \"${_source_hash}\",\n"
    "  \"tool\": \"GNU binutils 2.47 fr30-elf\",\n"
    "  \"note\": \"Generated Widen candidate images. They are not loaded by the normal product build; ROM compatibility, bank1 use, and device behaviour remain unverified.\",\n"
    "  \"images\": [\n"
    "${_manifest_images}\n"
    "  ]\n"
    "}\n")
file(WRITE "${TORNE_WIDEN_SCRATCH_DIR}/widen-manifest.json" "${_manifest}")
file(WRITE "${TORNE_WIDEN_SCRATCH_DIR}/widen-images.h"
    "/* SPDX-License-Identifier: GPL-2.0-only */\n"
    "/* Generated by cmake/fr30_widen.cmake from fr30/src/widen.s. Do not edit. */\n"
    "#ifndef TORNE_WIDEN_IMAGES_H\n"
    "#define TORNE_WIDEN_IMAGES_H\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n\n"
    "${_header}"
    "#endif\n")
file(COPY "${TORNE_WIDEN_SCRATCH_DIR}/widen-manifest.json"
    "${TORNE_WIDEN_SCRATCH_DIR}/widen-images.h"
    DESTINATION "${TORNE_WIDEN_OUTPUT_DIR}"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# Reject malformed headers and entries before accepting the generated files.
string(SUBSTRING "${_final_hex_bank0}" 8 -1 _after_magic)
set(_bad_magic_hex "5457444f${_after_magic}")
_widen_validate_hex("bad magic (self-check)" "${_bad_magic_hex}" 0x000E1000
    "${_bank0_build_id}" _ok _reason)
if(_ok)
    message(FATAL_ERROR "bad-header self-check was not rejected")
endif()

string(SUBSTRING "${_final_hex_bank0}" 0 40 _before_entry)
string(SUBSTRING "${_final_hex_bank0}" 48 -1 _after_entry)
set(_bad_entry_hex "${_before_entry}000e0fff${_after_entry}")
_widen_validate_hex("bad entry (self-check)" "${_bad_entry_hex}" 0x000E1000
    "${_bank0_build_id}" _ok _reason)
if(_ok)
    message(FATAL_ERROR "bad-entry self-check was not rejected")
endif()

execute_process(
    COMMAND "${WIDEN_LD}" -m elf32fr30 --fatal-warnings
        -T "${_ld_dir}/widen-bank0.ld"
        "--defsym=__widen_build_id=0"
        "--defsym=__widen_capacity_limit=8"
        -nostdlib -o "${TORNE_WIDEN_SCRATCH_DIR}/overflow.elf" "${WIDEN_SCRATCH_OBJECT}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(_rc STREQUAL "0")
    message(FATAL_ERROR "capacity-overflow self-check was not rejected")
endif()
string(FIND "${_out}${_err}" "Widen image exceeds capacity" _found)
if(_found EQUAL -1)
    message(FATAL_ERROR "capacity-overflow self-check failed for another reason:\n${_out}${_err}")
endif()

if(DEFINED TORNE_WIDEN_REFERENCE_DIR AND NOT TORNE_WIDEN_REFERENCE_DIR STREQUAL "")
    foreach(_file widen-bank0.bin widen-bank0.elf widen-bank1.bin
                  widen-bank1.elf widen-images.h widen-manifest.json)
        if(NOT EXISTS "${TORNE_WIDEN_REFERENCE_DIR}/${_file}")
            message(FATAL_ERROR "tracked Widen file is missing: ${_file}")
        endif()
        file(SHA256 "${TORNE_WIDEN_OUTPUT_DIR}/${_file}" _new_hash)
        file(SHA256 "${TORNE_WIDEN_REFERENCE_DIR}/${_file}" _old_hash)
        if(NOT _new_hash STREQUAL _old_hash)
            message(FATAL_ERROR
                "tracked Widen file is stale: ${_file}\n"
                "  generated: ${_new_hash}\n"
                "  tracked:   ${_old_hash}")
        endif()
    endforeach()
endif()

message(STATUS "Widen candidate images: PASS")
