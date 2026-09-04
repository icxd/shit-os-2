/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- gettimeofday and friends. */

#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>
#include <time.h>

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/*
 * The timezone argument is obsolete and always ignored; there is no timezone
 * database, so local time is UTC and saying so beats inventing an offset.
 */
int gettimeofday(struct timeval* now, void* timezone_ignored);

#define timeradd(a, b, out)                                                                        \
    do {                                                                                           \
        (out)->tv_sec = (a)->tv_sec + (b)->tv_sec;                                                 \
        (out)->tv_usec = (a)->tv_usec + (b)->tv_usec;                                              \
        if ((out)->tv_usec >= 1000000) {                                                           \
            ++(out)->tv_sec;                                                                       \
            (out)->tv_usec -= 1000000;                                                             \
        }                                                                                          \
    } while (0)

#define timersub(a, b, out)                                                                        \
    do {                                                                                           \
        (out)->tv_sec = (a)->tv_sec - (b)->tv_sec;                                                 \
        (out)->tv_usec = (a)->tv_usec - (b)->tv_usec;                                              \
        if ((out)->tv_usec < 0) {                                                                  \
            --(out)->tv_sec;                                                                       \
            (out)->tv_usec += 1000000;                                                             \
        }                                                                                          \
    } while (0)

#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)

#endif /* _SYS_TIME_H */
