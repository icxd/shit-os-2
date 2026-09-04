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

#include <ctype.h>
#include <errno.h>
#include <shitos.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

int clock_gettime(clockid_t clock_id, struct timespec* out)
{
    if (!out) {
        errno = EFAULT;
        return -1;
    }

    /* The kernel fills in a struct timespec directly; it is the same type on
     * both sides, defined once in shitos/abi/time.h. */
    if (__syscall_return(__syscall2(SYS_clock_gettime, clock_id, (long)out)) < 0)
        return -1;
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
    /* Local time is UTC here, so there is no offset and one zone name. */
    s_broken_down.tm_gmtoff = 0;
    s_broken_down.tm_zone = "UTC";

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

int clock_settime(clockid_t clock_id, const struct timespec* value)
{
    (void)clock_id;
    (void)value;
    /* The clock is read once at boot from whatever driver offers one, and the
     * kernel keeps an offset from its own monotonic count. Nothing writes it
     * back, so `date -s` gets told no rather than appearing to work. */
    errno = EPERM;
    return -1;
}

int futimens(int fd, const struct timespec times[2])
{
    (void)times;
    /* Same as utime and utimensat: inodes are stamped when written. */
    struct stat status;
    if (fstat(fd, &status) < 0)
        return -1;
    errno = ENOSYS;
    return -1;
}

/*
 * Parses a date written the way strftime would have printed it. Only the
 * conversions strftime emits are understood, which is the useful subset: a
 * general parser would have to guess at ambiguous input, and guessing about
 * dates is how software ends up a month out.
 */
/* Reads up to `width` digits, at least one, within [low, high]. */
static int strptime_number(const char** cursor, int width, int low, int high, int* result)
{
    int value = 0;
    int digits = 0;
    while (digits < width && **cursor >= '0' && **cursor <= '9') {
        value = value * 10 + (**cursor - '0');
        ++*cursor;
        ++digits;
    }
    if (digits == 0 || value < low || value > high)
        return 0;
    *result = value;
    return 1;
}

char* strptime(const char* text, const char* format, struct tm* out)
{
    if (!text || !format || !out)
        return NULL;

    static const char* const MONTHS[12] = { "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December" };
    static const char* const DAYS[7]
        = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

    const char* cursor = text;

    for (const char* f = format; *f; ++f) {
        if (*f != '%') {
            if (isspace((unsigned char)*f)) {
                /* Whitespace in the format matches any run of it, or none. */
                while (isspace((unsigned char)*cursor))
                    ++cursor;
                continue;
            }
            if (*cursor != *f)
                return NULL;
            ++cursor;
            continue;
        }

        ++f;
        int value = 0;
        switch (*f) {
        case 'Y':
            if (!strptime_number(&cursor, 4, 0, 9999, &value))
                return NULL;
            out->tm_year = value - 1900;
            break;
        case 'y':
            if (!strptime_number(&cursor, 2, 0, 99, &value))
                return NULL;
            /* The usual windowing: 69 and up is the twentieth century. */
            out->tm_year = value >= 69 ? value : value + 100;
            break;
        case 'm':
            if (!strptime_number(&cursor, 2, 1, 12, &value))
                return NULL;
            out->tm_mon = value - 1;
            break;
        case 'd':
        case 'e':
            while (*cursor == ' ')
                ++cursor;
            if (!strptime_number(&cursor, 2, 1, 31, &value))
                return NULL;
            out->tm_mday = value;
            break;
        case 'H':
            if (!strptime_number(&cursor, 2, 0, 23, &value))
                return NULL;
            out->tm_hour = value;
            break;
        case 'M':
            if (!strptime_number(&cursor, 2, 0, 59, &value))
                return NULL;
            out->tm_min = value;
            break;
        case 'S':
            if (!strptime_number(&cursor, 2, 0, 60, &value))
                return NULL;
            out->tm_sec = value;
            break;
        case 'j':
            if (!strptime_number(&cursor, 3, 1, 366, &value))
                return NULL;
            out->tm_yday = value - 1;
            break;
        case 'b':
        case 'B':
        case 'h': {
            int found = -1;
            for (int i = 0; i < 12 && found < 0; ++i) {
                size_t const full = strlen(MONTHS[i]);
                if (strncasecmp(cursor, MONTHS[i], full) == 0) {
                    found = i;
                    cursor += full;
                } else if (strncasecmp(cursor, MONTHS[i], 3) == 0) {
                    found = i;
                    cursor += 3;
                }
            }
            if (found < 0)
                return NULL;
            out->tm_mon = found;
            break;
        }
        case 'a':
        case 'A': {
            int found = -1;
            for (int i = 0; i < 7 && found < 0; ++i) {
                size_t const full = strlen(DAYS[i]);
                if (strncasecmp(cursor, DAYS[i], full) == 0) {
                    found = i;
                    cursor += full;
                } else if (strncasecmp(cursor, DAYS[i], 3) == 0) {
                    found = i;
                    cursor += 3;
                }
            }
            if (found < 0)
                return NULL;
            out->tm_wday = found;
            break;
        }
        case 'n':
        case 't':
            while (isspace((unsigned char)*cursor))
                ++cursor;
            break;
        case '%':
            if (*cursor != '%')
                return NULL;
            ++cursor;
            break;
        default:
            /* An unknown conversion cannot be guessed at safely. */
            return NULL;
        }
    }

    return (char*)cursor;
}
