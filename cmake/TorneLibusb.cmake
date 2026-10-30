# SPDX-License-Identifier: GPL-2.0-only
#
# Reproducible static libusb input. libusb's own Autotools build is used; the
# product does not add a parallel Makefile. udev is disabled so the final
# executable does not gain a non-OS shared dependency from libudev.
set(TORNE_LIBUSB_VERSION "1.0.30")
set(TORNE_LIBUSB_URL
    "https://github.com/libusb/libusb/releases/download/v${TORNE_LIBUSB_VERSION}/libusb-${TORNE_LIBUSB_VERSION}.tar.bz2")
set(TORNE_LIBUSB_SHA256 "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf")

set(TORNE_LIBUSB_SOURCE_DIR "${CMAKE_BINARY_DIR}/libusb-src")
set(TORNE_LIBUSB_BINARY_DIR "${CMAKE_BINARY_DIR}/libusb-build")
set(TORNE_LIBUSB_LIBRARY "${TORNE_LIBUSB_BINARY_DIR}/libusb/.libs/libusb-1.0.a")

function(torne_shell_quote output value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${output} "\"${value}\"" PARENT_SCOPE)
endfunction()

function(torne_add_libusb)
    include(ExternalProject)

    # Carry the target/sysroot CMake selected into libusb's own configure.
    # NDK-style toolchains keep the architecture in CMAKE_C_COMPILER_TARGET and
    # CMAKE_SYSROOT, not only in the compiler path. A small generated wrapper
    # keeps each argument separate, so paths and spaces survive.
    set(libusb_cc_args)
    if(CMAKE_C_COMPILER_ARG1)
        separate_arguments(compiler_arg1 UNIX_COMMAND "${CMAKE_C_COMPILER_ARG1}")
        list(APPEND libusb_cc_args ${compiler_arg1})
    endif()
    if(CMAKE_C_COMPILER_TARGET AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        list(APPEND libusb_cc_args "--target=${CMAKE_C_COMPILER_TARGET}")
    endif()
    if(CMAKE_SYSROOT)
        list(APPEND libusb_cc_args "--sysroot=${CMAKE_SYSROOT}")
    endif()

    set(libusb_cc "${CMAKE_C_COMPILER}")
    if(libusb_cc_args)
        if(UNIX)
            set(libusb_cc_wrapper "${CMAKE_BINARY_DIR}/torne-libusb-cc")
            torne_shell_quote(compiler_quoted "${CMAKE_C_COMPILER}")
            set(wrapper "#!/bin/sh\nexec ${compiler_quoted}")
            foreach(argument IN LISTS libusb_cc_args)
                torne_shell_quote(argument_quoted "${argument}")
                string(APPEND wrapper " ${argument_quoted}")
            endforeach()
            string(APPEND wrapper " \"$@\"\n")
            file(WRITE "${libusb_cc_wrapper}" "${wrapper}")
            execute_process(COMMAND chmod +x "${libusb_cc_wrapper}")
            set(libusb_cc "${libusb_cc_wrapper}")
        else()
            string(JOIN " " libusb_cc_arg_text ${libusb_cc_args})
            set(libusb_cc "${CMAKE_C_COMPILER} ${libusb_cc_arg_text}")
        endif()
    endif()

    set(libusb_host_args)
    if(WIN32)
        list(APPEND libusb_host_args --host=x86_64-w64-mingw32)
    elseif(CMAKE_CROSSCOMPILING AND CMAKE_C_COMPILER_TARGET)
        list(APPEND libusb_host_args --host=${CMAKE_C_COMPILER_TARGET})
    endif()

    set(configure_args
        --prefix=${TORNE_LIBUSB_BINARY_DIR}/install
        --disable-shared
        --enable-static
        --disable-udev
        --disable-tests-build
        --disable-examples-build
        ${libusb_host_args})

    set(configure_env
        ${CMAKE_COMMAND} -E env
        "CC=${libusb_cc}"
        "CFLAGS=${CMAKE_C_FLAGS}")
    if(CMAKE_AR)
        list(APPEND configure_env "AR=${CMAKE_AR}")
    endif()
    if(CMAKE_RANLIB)
        list(APPEND configure_env "RANLIB=${CMAKE_RANLIB}")
    endif()

    ExternalProject_Add(torne_libusb_external
        URL "${TORNE_LIBUSB_URL}"
        URL_HASH "SHA256=${TORNE_LIBUSB_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR "${TORNE_LIBUSB_SOURCE_DIR}"
        BINARY_DIR "${TORNE_LIBUSB_BINARY_DIR}"
        CONFIGURE_COMMAND
            ${configure_env}
            "${TORNE_LIBUSB_SOURCE_DIR}/configure"
            ${configure_args}
        BUILD_COMMAND make -j4
        INSTALL_COMMAND ""
        UPDATE_COMMAND ""
        BUILD_BYPRODUCTS "${TORNE_LIBUSB_LIBRARY}")

    add_library(torne_libusb INTERFACE)
    add_dependencies(torne_libusb torne_libusb_external)
    target_include_directories(torne_libusb SYSTEM INTERFACE
        "${TORNE_LIBUSB_SOURCE_DIR}/libusb")
    target_link_libraries(torne_libusb INTERFACE "${TORNE_LIBUSB_LIBRARY}")
    if(WIN32)
        target_link_libraries(torne_libusb INTERFACE setupapi ole32)
    endif()
    if(APPLE)
        # libusb 1.0.30 configure.ac adds these to LIBS for Darwin and exposes
        # them through Libs.private in libusb-1.0.pc. A raw static archive does
        # not retain them, so propagate the same OS-standard dependencies.
        target_link_libraries(torne_libusb INTERFACE "-lobjc")
        target_link_options(torne_libusb INTERFACE
            "-Wl,-framework,IOKit"
            "-Wl,-framework,CoreFoundation"
            "-Wl,-framework,Security")
    endif()
endfunction()
