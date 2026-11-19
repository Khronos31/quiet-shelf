cmake_minimum_required(VERSION 3.20)
project(torne_userland VERSION 2.0.0 LANGUAGES C CXX)

add_subdirectory(external/dtv11-core core)
dtv_add_product(NAME torne
                COMMAND tornectl
                VERSION 2.0.0
                DEFAULT_MODEL ""
                MODELS cech_zd1j
                SOURCES src/product.cpp src/cech_tune_signal.cpp src/cech_tune_io.cpp
                FIRMWARE torne_fr30_v1=firmware/torne-fr30-v1.bin)
