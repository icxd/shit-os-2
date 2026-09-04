/* SPDX-License-Identifier: MIT */
#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

int nanosleep(const struct timespec* request, struct timespec* remaining);

#endif /* _TIME_H */
