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
    message(STATUS "Ports disabled; /bin will not contain lua")
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
