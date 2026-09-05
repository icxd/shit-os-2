# SPDX-License-Identifier: GPL-3.0-or-later
#
# The window server, and the clients that prove it works.

add_shitos_program(wsys SOURCES wsys.c LIBRARIES shitos_libui)
add_shitos_program(wsysdemo SOURCES wsysdemo.c)
add_shitos_program(uidemo SOURCES uidemo.c LIBRARIES shitos_libui)
