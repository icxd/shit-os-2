# SPDX-License-Identifier: GPL-3.0-or-later
#
# The C library and the programs built against it.
#
# Userland is freestanding in the same sense the kernel is -- there is no host
# libc -- but it does not carry the kernel's constraints: the red zone is fine
# in ring 3, and code is position dependent at a fixed load address until a
# dynamic linker exists.

add_library(shitos_user_flags INTERFACE)
target_link_libraries(shitos_user_flags INTERFACE shitos_freestanding)
target_compile_options(shitos_user_flags INTERFACE
    -fno-pic
    -fno-pie
    -mno-red-zone   # harmless here, and keeps one flag set across the tree
)
target_include_directories(shitos_user_flags INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/libc/include"
    "${SHITOS_ABI_INCLUDE}"
)

set(LIBC_SOURCES
    libc/src/assert.c
    libc/src/crt0.S
    libc/src/dirent.c
    libc/src/errno.c
    libc/src/locale.c
    libc/src/math.c
    libc/src/setjmp.S
    libc/src/shitos.c
    libc/src/start.c
    libc/src/stdio.c
    libc/src/stdlib.c
    libc/src/strtod.c
    libc/src/poll.c
    libc/src/posix.c
    libc/src/posix_files.c
    libc/src/regex.c
    libc/src/pwd.c
    libc/src/signal_sets.c
    libc/src/string.c
    libc/src/time.c
    libc/src/syscall.c
    libc/src/unistd.c
)

add_library(shitos_libc STATIC ${LIBC_SOURCES})
target_link_libraries(shitos_libc PUBLIC shitos_user_flags)
target_include_directories(shitos_libc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libc/src")

# add_shitos_program(<name> SOURCES <files...>)
#
# Links a static ET_EXEC at a fixed base. Everything lands in build/user/bin,
# which is where mkinitrd looks for /bin.
function(add_shitos_program PROGRAM_NAME)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})

    add_executable(${PROGRAM_NAME} ${ARG_SOURCES})
    target_link_libraries(${PROGRAM_NAME} PRIVATE shitos_libc shitos_user_flags)
    target_link_options(${PROGRAM_NAME} PRIVATE
        -nostdlib
        -static
        -Wl,--build-id=none
        -Wl,-z,noexecstack
        -Wl,-z,max-page-size=0x1000
        # A fixed load address well clear of the null page. The kernel's ELF
        # loader only handles ET_EXEC for now, so nothing is relocated.
        "-Wl,--image-base=0x400000"
        -Wl,-e,_start
    )
    set_target_properties(${PROGRAM_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/user/bin"
    )
    set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_TARGETS ${PROGRAM_NAME})
    set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_FILES "$<TARGET_FILE:${PROGRAM_NAME}>")
endfunction()

add_subdirectory(bin)
