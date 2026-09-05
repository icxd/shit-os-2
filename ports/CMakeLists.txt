# SPDX-License-Identifier: GPL-3.0-or-later
#
# Third-party software built against our libc.
#
# Ports are downloaded and checksummed rather than vendored, so this GPLv3 tree
# stays free of other people's source and it is obvious which code is ours. The
# first build of each port needs network access; after that it comes from
# .port-cache.
#
# A port that needs patching is a bug report about our libc. None of these are
# patched.

option(SHITOS_PORTS "Build third-party ports (needs network on first build)" ON)

if(NOT SHITOS_PORTS)
    message(STATUS "Ports disabled; no lua, dash, sbase or fonts")
    return()
endif()

find_program(CURL curl)
if(NOT CURL)
    message(WARNING "curl not found; ports cannot be fetched")
    return()
endif()

set(LUA_BINARY "${CMAKE_BINARY_DIR}/user/bin/lua")

add_custom_command(
    OUTPUT "${LUA_BINARY}"
    COMMAND "${CMAKE_COMMAND}" -E env
            "SHITOS_CLANG=${CMAKE_C_COMPILER}"
            "SHITOS_LIBC=$<TARGET_FILE:shitos_libc>"
            "SHITOS_PORT_CACHE=${CMAKE_SOURCE_DIR}/.port-cache"
            "${CMAKE_SOURCE_DIR}/ports/lua/build.sh" "${CMAKE_BINARY_DIR}/user/bin"
    DEPENDS shitos_libc "${CMAKE_SOURCE_DIR}/ports/lua/build.sh"
    COMMENT "Building port: lua"
    VERBATIM
)

add_custom_target(port-lua ALL DEPENDS "${LUA_BINARY}")
set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_TARGETS port-lua)
set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_FILES "${LUA_BINARY}")

# dash needs a compiler for *this* machine as well as one for the target: it
# builds five small generators, runs them, and compiles what they print.
set(DASH_BINARY "${CMAKE_BINARY_DIR}/user/bin/dash")

add_custom_command(
    OUTPUT "${DASH_BINARY}"
    COMMAND "${CMAKE_COMMAND}" -E env
            "SHITOS_CLANG=${CMAKE_C_COMPILER}"
            "SHITOS_HOST_CC=cc"
            "SHITOS_LIBC=$<TARGET_FILE:shitos_libc>"
            "SHITOS_PORT_CACHE=${CMAKE_SOURCE_DIR}/.port-cache"
            "${CMAKE_SOURCE_DIR}/ports/dash/build.sh" "${CMAKE_BINARY_DIR}/user/bin"
    DEPENDS shitos_libc "${CMAKE_SOURCE_DIR}/ports/dash/build.sh"
            "${CMAKE_SOURCE_DIR}/ports/dash/config.h"
    COMMENT "Building port: dash"
    VERBATIM
)

add_custom_target(port-dash ALL DEPENDS "${DASH_BINARY}")
set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_TARGETS port-dash)
set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_FILES "${DASH_BINARY}")

# The fonts. Not compiled, but fetched and checksummed like everything else in
# here, because three quarters of a megabyte of somebody else's binary does not
# belong in the tree either.
set(FONT_STAMP "${CMAKE_BINARY_DIR}/user/share/fonts/.dejavu-stamp")

add_custom_command(
    OUTPUT "${FONT_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E env
            "SHITOS_PORT_CACHE=${CMAKE_SOURCE_DIR}/.port-cache"
            "${CMAKE_SOURCE_DIR}/ports/dejavu/build.sh"
            "${CMAKE_BINARY_DIR}/user/share/fonts"
    COMMAND "${CMAKE_COMMAND}" -E touch "${FONT_STAMP}"
    DEPENDS "${CMAKE_SOURCE_DIR}/ports/dejavu/build.sh"
    COMMENT "Fetching port: dejavu fonts"
    VERBATIM
)

add_custom_target(port-dejavu ALL DEPENDS "${FONT_STAMP}")
set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_TARGETS port-dejavu)
set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_FILES "${FONT_STAMP}")

# sbase builds ninety-odd programs, so the stamp rather than any one of them is
# what the initrd depends on. suckless publishes no tarballs, so this one is
# cloned at a pinned commit and the tree is hashed -- see ports/sbase/build.sh.
find_program(GIT git)
if(NOT GIT)
    message(WARNING "git not found; the sbase port cannot be fetched")
else()
    set(SBASE_STAMP "${CMAKE_BINARY_DIR}/user/bin/.sbase-stamp")

    add_custom_command(
        OUTPUT "${SBASE_STAMP}"
        COMMAND "${CMAKE_COMMAND}" -E env
                "SHITOS_CLANG=${CMAKE_C_COMPILER}"
                "SHITOS_LIBC=$<TARGET_FILE:shitos_libc>"
                "SHITOS_PORT_CACHE=${CMAKE_SOURCE_DIR}/.port-cache"
                "${CMAKE_SOURCE_DIR}/ports/sbase/build.sh" "${CMAKE_BINARY_DIR}/user/bin"
        COMMAND "${CMAKE_COMMAND}" -E touch "${SBASE_STAMP}"
        DEPENDS shitos_libc "${CMAKE_SOURCE_DIR}/ports/sbase/build.sh"
        COMMENT "Building port: sbase"
        VERBATIM
    )

    add_custom_target(port-sbase ALL DEPENDS "${SBASE_STAMP}")
    set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_TARGETS port-sbase)
    set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_FILES "${SBASE_STAMP}")
endif()
