# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything of ours that ends up in /bin.
#
# Deliberately short. The coreutils used to live here too, written during
# bring-up because nothing else existed, and once ports/sbase built they
# collided: two `cat`s installed to the same path, and which one you got
# depended on the order ninja happened to run the two rules in. That is a real
# bug, not a cosmetic one, so the duplicates are gone -- sbase's are the ones
# with the options POSIX actually specifies.
#
# What is left is what sbase does not and could not provide: the two programs
# that define how userland starts, the three that report on this kernel
# specifically, the one that drives our termios, and the test.

add_shitos_program(init SOURCES init.c)
add_shitos_program(sh SOURCES sh.c)

# These read kernel state through interfaces nobody else has.
add_shitos_program(ps SOURCES ps.c)
add_shitos_program(free SOURCES free.c)
add_shitos_program(lsmod SOURCES lsmod.c)

add_shitos_program(stty SOURCES stty.c)

# Not utilities either: the POSIX surface and the graphics path, both checked
# from ring 3 and both run at boot by /etc/rc.
add_shitos_program(usertest SOURCES usertest.c)
add_shitos_program(gfxtest SOURCES gfxtest.c)
