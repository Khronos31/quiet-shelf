# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT DEFINED CLI)
    message(FATAL_ERROR "PIX CLI executable is required")
endif()

execute_process(
    COMMAND "${CLI}" --models
    RESULT_VARIABLE actual_status
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr)

if(NOT "${actual_status}" STREQUAL "0" OR
   NOT "${actual_stdout}" STREQUAL "PIX-DT295 06b8:103b\n" OR
   NOT "${actual_stderr}" STREQUAL "")
    message(FATAL_ERROR
        "expected PIX-DT295 06b8:103b; got status=${actual_status}, stdout=[${actual_stdout}], stderr=[${actual_stderr}]")
endif()
