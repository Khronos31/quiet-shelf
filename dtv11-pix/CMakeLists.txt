# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(pix LANGUAGES C CXX)

add_subdirectory(core core-build)
include(core/cmake/DtvProduct.cmake)

dtv_product(NAME pix
            COMMAND i0pixctl
            VERSION 2.0.0
            DEFAULT_MODEL ""
            MODELS pix_dt295_custom_v25
            SOURCES src/pix.cpp src/pix_common.cpp src/pix_dt295_tune_signal.cpp
            FIRMWARE pix_dt295_firmware=firmware/firmware.bin)
