# SPDX-License-Identifier: GPL-2.0-only
#
# Stage a candidate PC/SC IFD bundle from the Core Unix IFD target. This is
# package construction only; it does not install, register, or load the bundle
# with the macOS PC/SC service.

if(NOT TARGET dtv::unix_pcsc_ifd)
    message(FATAL_ERROR "Core did not provide dtv::unix_pcsc_ifd")
endif()

get_target_property(TORNE_CORE_UNIX_IFD_SOURCE_DIR
    dtv_userland_unix_pcsc_ifd SOURCE_DIR)
if(NOT TORNE_CORE_UNIX_IFD_SOURCE_DIR)
    message(FATAL_ERROR "Could not locate the Core Unix IFD source directory")
endif()
set(TORNE_PCSC_BUNDLE_DIR "${CMAKE_BINARY_DIR}/pcsc/torne-userland-ifd.bundle")
set(TORNE_PCSC_BUNDLE_EXECUTABLE "torne-userland-ifd")
set(TORNE_PCSC_BUNDLE_IDENTIFIER "org.khronos31.torne-userland.pcsc-ifd")
set(TORNE_PCSC_BUNDLE_VERSION "${PROJECT_VERSION}")

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
    "${TORNE_PCSC_BUNDLE_DIR}/Contents/Info.plist"
    @ONLY)

add_custom_target(torne_pcsc_bundle
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${TORNE_PCSC_BUNDLE_DIR}/Contents/MacOS"
        "${TORNE_PCSC_BUNDLE_DIR}/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:dtv::unix_pcsc_ifd>"
        "${TORNE_PCSC_BUNDLE_DIR}/Contents/MacOS/${TORNE_PCSC_BUNDLE_EXECUTABLE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/LICENSE"
        "${TORNE_PCSC_BUNDLE_DIR}/Contents/Resources/LICENSE"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TORNE_CORE_UNIX_IFD_SOURCE_DIR}/adapters/unix_pcsc_ifd/COPYING"
        "${TORNE_PCSC_BUNDLE_DIR}/Contents/Resources/COPYING"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TORNE_CORE_UNIX_IFD_SOURCE_DIR}/adapters/unix_pcsc_ifd/UNIX_PCSC_IFD_PROVENANCE.json"
        "${TORNE_PCSC_BUNDLE_DIR}/Contents/Resources/UNIX_PCSC_IFD_PROVENANCE.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TORNE_CORE_ROOT}/LICENSE"
        "${TORNE_PCSC_BUNDLE_DIR}/Contents/Resources/LICENSE.dtv-userland-core"
    COMMENT "Staging candidate torne-userland PC/SC IFD bundle (not registered)"
    VERBATIM)
