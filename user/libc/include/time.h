/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- time.
 *
 * There is no real-time clock driver yet, so the epoch this reports is boot,
 * not 1970. time() returns seconds since the machine started. That is a lie
 * about the date and an honest answer about elapsed time, which is the half
 * that programs mostly use it for -- but a file's timestamp or a date printed
 * by a script will be wrong until a CMOS RTC module exists. See
 * docs/roadmap.md.
 */

#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

#define CLOCKS_PER_SEC 1000000L

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

int nanosleep(const struct timespec* request, struct timespec* remaining);

time_t time(time_t* out);
clock_t clock(void);
double difftime(time_t later, time_t earlier);

struct tm* gmtime(const time_t* when);
struct tm* localtime(const time_t* when);
time_t mktime(struct tm* broken_down);
size_t strftime(char* out, size_t capacity, const char* format, const struct tm* broken_down);
char* asctime(const struct tm* broken_down);
char* ctime(const time_t* when);

#endif /* _TIME_H */
