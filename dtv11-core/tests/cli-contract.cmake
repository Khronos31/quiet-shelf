# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT DEFINED CLI)
    message(FATAL_ERROR "CLI executable is required")
endif()

set(cases
    [[help|--help|0|Usage: fixturectl [--help] [--version] [--models]\nShow product metadata. Other operations are not implemented yet.\n|@empty@]]
    [[help_unknown|--help,--unknown|0|Usage: fixturectl [--help] [--version] [--models]\nShow product metadata. Other operations are not implemented yet.\n|@empty@]]
    [[version_help|--version,--help|0|Usage: fixturectl [--help] [--version] [--models]\nShow product metadata. Other operations are not implemented yet.\n|@empty@]]
    [[help_version|--help,--version|0|Usage: fixturectl [--help] [--version] [--models]\nShow product metadata. Other operations are not implemented yet.\n|@empty@]]
    [[models_help|--models,--help|0|Usage: fixturectl [--help] [--version] [--models]\nShow product metadata. Other operations are not implemented yet.\n|@empty@]]
    [[version|--version|0|fixture 1.2.3\n|@empty@]]
    [[version_unknown|--version,--unknown|0|fixture 1.2.3\n|@empty@]]
    [[models_version|--models,--version|0|fixture 1.2.3\n|@empty@]]
    [[models|--models|0|Fixture One 1234:0001\nFixture Two 5678:9abc\n|@empty@]]
    [[unknown_subcommand|tune|2|@empty@|fixturectl: unknown argument\n]]
    [[unknown_option|--unknown|2|@empty@|fixturectl: unknown argument\n]]
    [[models_unknown|--models,--unknown|2|@empty@|fixturectl: --models cannot be combined with other arguments\n]]
    [[models_models|--models,--models|2|@empty@|fixturectl: --models cannot be combined with other arguments\n]]
)

foreach(case IN LISTS cases)
    string(REPLACE "|" ";" fields "${case}")
    list(GET fields 0 name)
    list(GET fields 1 arguments)
    list(GET fields 2 expected_status)
    list(GET fields 3 expected_stdout)
    list(GET fields 4 expected_stderr)

    set(cli_arguments)
    if(NOT arguments STREQUAL "@none@")
        string(REPLACE "," ";" cli_arguments "${arguments}")
    endif()
    if(expected_stdout STREQUAL "@empty@")
        set(expected_stdout "")
    else()
        string(REPLACE "\\n" "\n" expected_stdout "${expected_stdout}")
    endif()
    if(expected_stderr STREQUAL "@empty@")
        set(expected_stderr "")
    else()
        string(REPLACE "\\n" "\n" expected_stderr "${expected_stderr}")
    endif()

    execute_process(
        COMMAND "${CLI}" ${cli_arguments}
        RESULT_VARIABLE actual_status
        OUTPUT_VARIABLE actual_stdout
        ERROR_VARIABLE actual_stderr)
    string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
    string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")

    if(NOT "${actual_status}" STREQUAL "${expected_status}" OR
       NOT "${actual_stdout}" STREQUAL "${expected_stdout}" OR
       NOT "${actual_stderr}" STREQUAL "${expected_stderr}")
        message(FATAL_ERROR
            "${name}: expected status=${expected_status}, stdout=[${expected_stdout}], stderr=[${expected_stderr}]; got status=${actual_status}, stdout=[${actual_stdout}], stderr=[${actual_stderr}]")
    endif()
endforeach()
