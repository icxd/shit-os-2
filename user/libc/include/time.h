/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- time.
 *
 * The kernel gets its calendar from whatever driver registers a time source --
 * modules/rtc reads the CMOS at boot -- so time() is a real date. If nothing
 * registered one, the clock reads as seconds since boot instead of inventing a
 * plausible wrong year, and a date printed then is obviously 1970 rather than
 * subtly off.
 *
 * There is no timezone database, so local time is UTC.
 */

#ifndef _TIME_H
#define _TIME_H

#include <shitos/abi/time.h>

#include <sys/types.h>

#define CLOCKS_PER_SEC 1000000L

typedef int clockid_t;

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
    /* BSD additions that portable software reads. Local time is UTC here, so
     * the offset is zero and the name is always the same. */
    long tm_gmtoff;
    const char* tm_zone;
};

int nanosleep(const struct timespec* request, struct timespec* remaining);

int clock_gettime(clockid_t clock_id, struct timespec* out);
/* The clock is read once at boot from whatever driver offers one and never
 * written, so this always refuses. */
int clock_settime(clockid_t clock_id, const struct timespec* value);

/* Parses a date according to a strftime-style format. Only the conversions
 * strftime emits are understood. */
char* strptime(const char* text, const char* format, struct tm* out);
int futimens(int fd, const struct timespec times[2]);

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
