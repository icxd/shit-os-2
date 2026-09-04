/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the syscall layer.
 *
 * One inline helper per argument count, and a single place where a negative
 * return is turned into -1 plus errno. Every other file in the library goes
 * through here rather than writing `syscall` itself.
 */

#include "internal.h"

#include <errno.h>

static int s_errno;

int* __errno_location(void)
{
    return &s_errno;
}

long __syscall_return(long value)
{
    /*
     * The kernel returns -errno on failure. Values in [-4095, -1] are errors;
     * anything else is a real result, which matters because mmap and sbrk can
     * legitimately return addresses with the top bit set.
     */
    if (value < 0 && value > -4096) {
        s_errno = (int)-value;
        return -1;
    }
    return value;
}
