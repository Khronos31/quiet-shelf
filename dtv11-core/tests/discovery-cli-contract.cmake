# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT DEFINED CLI)
    message(FATAL_ERROR "CLI executable is required")
endif()

function(check_list_json name)
    execute_process(
        COMMAND "${CLI}" ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    string(REPLACE "\r\n" "\n" out "${out}")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${name}: expected status 0, got ${status}; stderr=[${err}]")
    endif()
    string(FIND "${out}" "{\"schema\":{\"name\":\"userland.discovery\",\"major\":1,\"minor\":0}," prefix)
    if(NOT prefix EQUAL 0)
        message(FATAL_ERROR "${name}: discovery JSON prefix missing; stdout=[${out}]")
    endif()
    string(FIND "${out}" "\n" first_newline)
    if(first_newline LESS 0)
        message(FATAL_ERROR "${name}: missing trailing newline")
    endif()
    math(EXPR after_pos "${first_newline} + 1")
    string(SUBSTRING "${out}" ${after_pos} -1 after)
    string(FIND "${out}" "\n" second_newline)
    if(NOT after STREQUAL "" OR NOT second_newline EQUAL first_newline)
        message(FATAL_ERROR "${name}: expected one complete JSON document plus one newline")
    endif()
endfunction()

check_list_json(list_json --list-json)
check_list_json(list_json_unknown --list-json --unknown)
check_list_json(list_json_models --list-json --models)
