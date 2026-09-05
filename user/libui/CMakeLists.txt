# SPDX-License-Identifier: GPL-3.0-or-later
#
# The user interface library. Text first, because nothing built on top of it is
# worth looking at until the text is.

add_library(shitos_libui STATIC
    truetype.c
    text.c
    paint.c
    widget.c
    controls.c
    terminal.c
    window.c
)

# It uses poll, malloc, open and mmap, so it depends on the libc and has to
# say so. Not just for tidiness: GNU ld resolves archives strictly left to
# right, so an unstated dependency is a link that succeeds or fails depending
# on the order CMake happened to emit. lld hides this by keeping archive
# members as lazy symbols; ld does not.
target_link_libraries(shitos_libui PUBLIC shitos_user_flags shitos_libc)
target_include_directories(shitos_libui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
