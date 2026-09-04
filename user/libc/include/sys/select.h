/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- select(2), built on poll.
 *
 * select is the older interface and the worse one: a fixed-size bitmap, three
 * separate sets, and a timeout it is allowed to modify. It exists here because
 * ported software asks for it, and it is a translation layer over poll rather
 * than a second implementation.
 */

#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <sys/time.h>
#include <sys/types.h>

/* The descriptor table is 64 entries, so one word covers it exactly. */
#define FD_SETSIZE 64

typedef struct {
    unsigned long
        bits[(FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_BITS_PER_WORD (8 * sizeof(unsigned long))

#define FD_ZERO(set)                                                                               \
    do {                                                                                           \
        for (unsigned long _i = 0; _i < sizeof((set)->bits) / sizeof((set)->bits[0]); ++_i)        \
            (set)->bits[_i] = 0;                                                                   \
    } while (0)

#define FD_SET(fd, set) ((set)->bits[(fd) / FD_BITS_PER_WORD] |= 1UL << ((fd) % FD_BITS_PER_WORD))
#define FD_CLR(fd, set)                                                                            \
    ((set)->bits[(fd) / FD_BITS_PER_WORD] &= ~(1UL << ((fd) % FD_BITS_PER_WORD)))
#define FD_ISSET(fd, set)                                                                          \
    (((set)->bits[(fd) / FD_BITS_PER_WORD] & (1UL << ((fd) % FD_BITS_PER_WORD))) != 0)

int select(
    int nfds, fd_set* readable, fd_set* writable, fd_set* exceptional, struct timeval* timeout);

#endif /* _SYS_SELECT_H */
