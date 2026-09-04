/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- time.
 *
 * The calendar comes from the kernel, which gets it from whatever driver
 * registered a time source; modules/rtc reads the CMOS once at boot and the
 * kernel keeps the offset from its own monotonic clock. If nothing registered
 * one the realtime clock reads as seconds since boot, which puts a printed
 * date in 1970 -- obviously fake rather than subtly wrong.
 *
 * The two clocks answer different questions and this file keeps them apart.
 * CLOCK_REALTIME is a date and can jump; CLOCK_MONOTONIC only counts forward
 * from boot and is what an elapsed-time measurement wants.
 */

#include "internal.h"

#include <errno.h>
#include <shitos.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

int clock_gettime(clockid_t clock_id, struct timespec* out)
{
    if (!out) {
        errno = EFAULT;
        return -1;
    }

    struct shitos_timespec value;
    if (__syscall_return(__syscall2(SYS_clock_gettime, clock_id, (long)&value)) < 0)
        return -1;

    out->tv_sec = (time_t)value.tv_sec;
    out->tv_nsec = (long)value.tv_nsec;
    return 0;
}

int gettimeofday(struct timeval* now, void* timezone_ignored)
{
    (void)timezone_ignored;
    if (!now) {
        errno = EFAULT;
        return -1;
    }

    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) < 0)
        return -1;

    now->tv_sec = value.tv_sec;
    now->tv_usec = value.tv_nsec / 1000;
    return 0;
}

time_t time(time_t* out)
{
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) < 0)
        value.tv_sec = 0;

    if (out)
        *out = value.tv_sec;
    return value.tv_sec;
}

clock_t clock(void)
{
    /*
     * Should be processor time charged to this process; there is no per-process
     * accounting yet, so this is monotonic time since boot. That makes a
     * difference of two clock() calls right -- which is what almost every
     * caller wants -- and the absolute value wrong for anything but the first
     * process. CLOCKS_PER_SEC is a microsecond and the tick is 4 ms, so the low
     * digits are always zero: the unit is right, the resolution is not.
     */
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
        return (clock_t)0;
    return (clock_t)(value.tv_sec * 1000000L + value.tv_nsec / 1000);
}

double difftime(time_t later, time_t earlier)
{
    return (double)(later - earlier);
}

/* --- broken-down time ---------------------------------------------------- */

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static const int DAYS_IN_MONTH[2][12] = {
    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
    { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
};

static struct tm s_broken_down;

struct tm* gmtime(const time_t* when)
{
    long seconds = when ? (long)*when : 0;
    if (seconds < 0)
        seconds = 0;

    long days = seconds / 86400;
    long remainder = seconds % 86400;

    s_broken_down.tm_hour = (int)(remainder / 3600);
    s_broken_down.tm_min = (int)((remainder % 3600) / 60);
    s_broken_down.tm_sec = (int)(remainder % 60);

    /* 1 January 1970 was a Thursday. */
    s_broken_down.tm_wday = (int)((days + 4) % 7);

    int year = 1970;
    for (;;) {
        int const length = is_leap_year(year) ? 366 : 365;
        if (days < length)
            break;
        days -= length;
        ++year;
    }

    s_broken_down.tm_year = year - 1900;
    s_broken_down.tm_yday = (int)days;

    int month = 0;
    int const leap = is_leap_year(year);
    while (days >= DAYS_IN_MONTH[leap][month]) {
        days -= DAYS_IN_MONTH[leap][month];
        ++month;
    }

    s_broken_down.tm_mon = month;
    s_broken_down.tm_mday = (int)days + 1;
    s_broken_down.tm_isdst = 0;

    return &s_broken_down;
}

/* No timezone database, so local time is UTC. Saying so is better than
 * applying an offset we invented. */
struct tm* localtime(const time_t* when)
{
    return gmtime(when);
}

time_t mktime(struct tm* broken_down)
{
    if (!broken_down)
        return (time_t)-1;

    int const year = broken_down->tm_year + 1900;
    if (year < 1970)
        return (time_t)-1;

    long days = 0;
    for (int y = 1970; y < year; ++y)
        days += is_leap_year(y) ? 366 : 365;

    int const leap = is_leap_year(year);
    for (int m = 0; m < broken_down->tm_mon && m < 12; ++m)
        days += DAYS_IN_MONTH[leap][m];

    days += broken_down->tm_mday - 1;

    time_t const result = (time_t)days * 86400 + broken_down->tm_hour * 3600
        + broken_down->tm_min * 60 + broken_down->tm_sec;

    /* Fill in the derived fields, as mktime is specified to do. */
    time_t normalised = result;
    struct tm* const rebuilt = gmtime(&normalised);
    broken_down->tm_wday = rebuilt->tm_wday;
    broken_down->tm_yday = rebuilt->tm_yday;

    return result;
}

static const char* const WEEKDAY_NAMES[7]
    = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char* const MONTH_NAMES[12] = { "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December" };

size_t strftime(char* out, size_t capacity, const char* format, const struct tm* t)
{
    if (!out || capacity == 0 || !format || !t)
        return 0;

    size_t written = 0;
    char scratch[64];

    for (const char* p = format; *p; ++p) {
        if (*p != '%') {
            if (written + 1 >= capacity)
                return 0;
            out[written++] = *p;
            continue;
        }

        ++p;
        const char* piece = scratch;
        switch (*p) {
        case 'Y': snprintf(scratch, sizeof(scratch), "%d", t->tm_year + 1900); break;
        case 'y': snprintf(scratch, sizeof(scratch), "%02d", (t->tm_year + 1900) % 100); break;
        case 'm': snprintf(scratch, sizeof(scratch), "%02d", t->tm_mon + 1); break;
        case 'd': snprintf(scratch, sizeof(scratch), "%02d", t->tm_mday); break;
        case 'H': snprintf(scratch, sizeof(scratch), "%02d", t->tm_hour); break;
        case 'M': snprintf(scratch, sizeof(scratch), "%02d", t->tm_min); break;
        case 'S': snprintf(scratch, sizeof(scratch), "%02d", t->tm_sec); break;
        case 'j': snprintf(scratch, sizeof(scratch), "%03d", t->tm_yday + 1); break;
        case 'p': piece = t->tm_hour < 12 ? "AM" : "PM"; break;
        case 'A': piece = WEEKDAY_NAMES[t->tm_wday % 7]; break;
        case 'B': piece = MONTH_NAMES[t->tm_mon % 12]; break;
        case 'a': snprintf(scratch, sizeof(scratch), "%.3s", WEEKDAY_NAMES[t->tm_wday % 7]); break;
        case 'b':
        case 'h': snprintf(scratch, sizeof(scratch), "%.3s", MONTH_NAMES[t->tm_mon % 12]); break;
        case 'c':
            snprintf(scratch, sizeof(scratch), "%.3s %.3s %2d %02d:%02d:%02d %d",
                WEEKDAY_NAMES[t->tm_wday % 7], MONTH_NAMES[t->tm_mon % 12], t->tm_mday, t->tm_hour,
                t->tm_min, t->tm_sec, t->tm_year + 1900);
            break;
        case 'x':
            snprintf(scratch, sizeof(scratch), "%02d/%02d/%02d", t->tm_mon + 1, t->tm_mday,
                (t->tm_year + 1900) % 100);
            break;
        case 'X':
            snprintf(scratch, sizeof(scratch), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
            break;
        case 'Z': piece = "UTC"; break;
        case '%': piece = "%"; break;
        case '\0': return written;
        default:
            /* An unknown conversion is emitted literally, which is what most
             * implementations do and is easier to debug than dropping it. */
            snprintf(scratch, sizeof(scratch), "%%%c", *p);
            break;
        }

        size_t const length = strlen(piece);
        if (written + length >= capacity)
            return 0;
        memcpy(out + written, piece, length);
        written += length;
    }

    out[written] = '\0';
    return written;
}

char* asctime(const struct tm* t)
{
    static char buffer[32];
    strftime(buffer, sizeof(buffer), "%c", t);
    return buffer;
}

char* ctime(const time_t* when)
{
    return asctime(gmtime(when));
}
