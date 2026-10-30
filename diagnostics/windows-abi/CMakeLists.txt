cmake_minimum_required(VERSION 3.16)
project(bondriver_windows_abi_probe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(BonDriver_ABIProbe SHARED fake/fake_driver.cpp)
target_include_directories(BonDriver_ABIProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/abi)
set_target_properties(BonDriver_ABIProbe PROPERTIES
	PREFIX ""
	OUTPUT_NAME "BonDriver_ABIProbe"
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN YES)

add_executable(abi_probe_host host/probe_host.cpp)
target_include_directories(abi_probe_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/abi)

if(MSVC)
	# The probe uses process-local file and environment helpers only.
	target_compile_definitions(BonDriver_ABIProbe PRIVATE _CRT_SECURE_NO_WARNINGS)
	target_compile_definitions(abi_probe_host PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(BonDriver_ABIProbe PRIVATE -Wall -Wextra)
	target_compile_options(abi_probe_host PRIVATE -Wall -Wextra)
endif()

if(MINGW)
	# Keep the probe independent of toolchain runtime DLLs so a missing
	# runtime is not mistaken for an ABI failure.
	target_link_options(BonDriver_ABIProbe PRIVATE -static)
	target_link_options(abi_probe_host PRIVATE -static)
endif()
