# SPDX-License-Identifier: GPL-3.0-or-later
#
# Loadable drivers. Each one is an independent .ko that links against nothing
# and reaches the kernel only through the KernelApi table it is handed.

include("${CMAKE_SOURCE_DIR}/cmake/module.cmake")

add_shitos_module(ps2kbd SOURCES ps2kbd/ps2kbd.c)
