# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything that ends up in /bin.

add_shitos_program(init SOURCES init.c)
add_shitos_program(sh SOURCES sh.c)
add_shitos_program(ls SOURCES ls.c)
add_shitos_program(cat SOURCES cat.c)
add_shitos_program(echo SOURCES echo.c)
add_shitos_program(mkdir SOURCES mkdir.c)
add_shitos_program(rm SOURCES rm.c)
add_shitos_program(ps SOURCES ps.c)
add_shitos_program(free SOURCES free.c)
add_shitos_program(lsmod SOURCES lsmod.c)
add_shitos_program(uname SOURCES uname.c)
