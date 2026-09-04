/* SPDX-License-Identifier: MIT */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <shitos/abi/termios.h>

int ioctl(int fd, unsigned long request, void* argument);

#endif /* _SYS_IOCTL_H */
