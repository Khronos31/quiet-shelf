# SPDX-License-Identifier: GPL-2.0-only
foreach(required TORNED TORNECTL TORNE_TS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is not set")
    endif()
endforeach()

function(run_cli result_var output_var error_var)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    set(${result_var} ${result} PARENT_SCOPE)
    set(${output_var} "${output}" PARENT_SCOPE)
    set(${error_var} "${error}" PARENT_SCOPE)
endfunction()

function(expect_result name expected)
    if(NOT "${${name}_result}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "${name}: expected exit ${expected}, got ${${name}_result}\n"
            "stdout:\n${${name}_output}\nstderr:\n${${name}_error}")
    endif()
endfunction()

function(expect_contains name haystack_var needle)
    string(FIND "${${haystack_var}}" "${needle}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${name}: missing '${needle}'\n${${haystack_var}}")
    endif()
endfunction()

function(expect_not_contains name haystack_var needle)
    string(FIND "${${haystack_var}}" "${needle}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${name}: unexpected '${needle}'\n${${haystack_var}}")
    endif()
endfunction()

function(make_private_instance_dir root instance)
    set(instance_dir "${root}/torne-userland/v1/${instance}")
    file(MAKE_DIRECTORY "${instance_dir}")
    file(WRITE "${instance_dir}/control.sock" "")
    if(UNIX)
        execute_process(COMMAND chmod 700 "${root}" "${root}/torne-userland"
            "${root}/torne-userland/v1" "${instance_dir}")
    endif()
endfunction()

run_cli(torned_help_result torned_help_output torned_help_error ${TORNED} --help)
expect_result(torned_help 0)
expect_contains(torned_help torned_help_output "usage:")

run_cli(torned_version_result torned_version_output torned_version_error ${TORNED} --version)
expect_result(torned_version 0)
expect_contains(torned_version torned_version_output "torned ")

run_cli(torned_list_result torned_list_output torned_list_error ${TORNED} --list-json)
expect_result(torned_list 0)
expect_contains(torned_list torned_list_output "\"schema\":{\"name\":\"userland.discovery\"")
expect_contains(torned_list torned_list_output "\"product\":\"torne-userland\"")

run_cli(torned_serial_result torned_serial_output torned_serial_error
    ${TORNED} --device serial:abc)
expect_result(torned_serial 2)

run_cli(tornectl_help_result tornectl_help_output tornectl_help_error ${TORNECTL} --help)
expect_result(tornectl_help 0)
expect_contains(tornectl_help tornectl_help_output "usage: tornectl")

run_cli(tornectl_version_result tornectl_version_output tornectl_version_error
    ${TORNECTL} --version)
expect_result(tornectl_version 0)
expect_contains(tornectl_version tornectl_version_output "torne-userland ")

run_cli(tornectl_serial_result tornectl_serial_output tornectl_serial_error
    ${TORNECTL} --device serial:abc list)
expect_result(tornectl_serial 2)

run_cli(ts_help_result ts_help_output ts_help_error ${TORNE_TS} --help)
expect_result(ts_help 0)
expect_contains(ts_help ts_help_output "usage: torne-ts")

run_cli(ts_version_result ts_version_output ts_version_error ${TORNE_TS} --version)
expect_result(ts_version 0)
expect_contains(ts_version ts_version_output "torne-ts ")

run_cli(ts_system_result ts_system_output ts_system_error ${TORNE_TS} -c CS2)
expect_result(ts_system 3)
expect_contains(ts_system ts_system_error "only ISDB-T")

# Core canonicalize_instance rules are shared by all three entrances.
foreach(instance com1 COM1 .abc abc.)
    run_cli(torned_bad_instance_result torned_bad_instance_output
        torned_bad_instance_error ${TORNED} --instance ${instance})
    expect_result(torned_bad_instance 2)

    run_cli(tornectl_bad_instance_result tornectl_bad_instance_output
        tornectl_bad_instance_error ${TORNECTL} --instance ${instance} list)
    expect_result(tornectl_bad_instance 2)

    run_cli(ts_bad_instance_result ts_bad_instance_output
        ts_bad_instance_error ${TORNE_TS} --instance ${instance} -c 27)
    expect_result(ts_bad_instance 2)
endforeach()

# A syntactically valid but absent topology keeps this smoke away from any
# attached receiver. PR16's UsbBackend still returns UNSUPPORTED without USB.
run_cli(torned_case_result torned_case_output torned_case_error
    ${TORNED} --device usb:255-99 --instance Abc)
expect_result(torned_case 3)
expect_contains(torned_case torned_case_error "hardware backend open failed")

run_cli(tornectl_case_result tornectl_case_output tornectl_case_error
    ${TORNECTL} --instance Abc list)
expect_result(tornectl_case 3)

run_cli(ts_case_result ts_case_output ts_case_error ${TORNE_TS} --instance Abc -c 27)
expect_result(ts_case 3)
expect_not_contains(ts_case ts_case_error "invalid --instance")

if(UNIX)
    # Runtime smoke only: the exact instance directory must be found. The
    # spelling itself is verified by torne_ts_profile_tests without relying on
    # the filesystem's case sensitivity (macOS volumes may fold case).
    string(RANDOM LENGTH 8 smoke_suffix)
    set(smoke_root "/tmp/torne-cli-smoke-${smoke_suffix}")
    make_private_instance_dir("${smoke_root}" Abc)
    run_cli(ts_instance_dir_result ts_instance_dir_output ts_instance_dir_error
        ${TORNE_TS} --runtime-dir "${smoke_root}" --instance Abc -c 27)
    expect_result(ts_instance_dir 3)
    expect_not_contains(ts_instance_dir ts_instance_dir_error
        "runtime namespace is missing")
    file(REMOVE_RECURSE "${smoke_root}")
endif()
