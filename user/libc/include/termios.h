/* SPDX-License-Identifier: MIT */
#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <shitos/abi/termios.h>

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios* out);
int tcsetattr(int fd, int actions, const struct termios* in);

#endif /* _TERMIOS_H */
