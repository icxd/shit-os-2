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
    window.c
)

target_link_libraries(shitos_libui PUBLIC shitos_user_flags)
target_include_directories(shitos_libui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
