# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(dtv11_asicen LANGUAGES C CXX)

add_subdirectory(external/dtv11-core core)

set(PRODUCT_NAME asicen)
set(PRODUCT_COMMAND asicenctl)
set(PRODUCT_VERSION 2.0.0)
set(PRODUCT_DEFAULT_MODEL "")
dtv_add_product(NAME ${PRODUCT_NAME}
                COMMAND ${PRODUCT_COMMAND}
                VERSION ${PRODUCT_VERSION}
                DEFAULT_MODEL "${PRODUCT_DEFAULT_MODEL}"
                MODELS px_w3u3 px_w3u2
                SOURCES src/ground_tune_common.cpp src/px_w3u3_tune.cpp src/px_w3u3_satellite.cpp src/px_w3u2_tune.cpp src/px_w3u3_v2_tune.cpp src/px_s3u_tune.cpp
                FIRMWARE asicen_loader=firmware/asicen-loader.bin)
