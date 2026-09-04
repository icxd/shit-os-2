/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- sleep.
 *
 * Accepts a fractional count of seconds, because every script that waits for
 * something writes `sleep 0.1` and a version that only understands integers
 * turns that into either an error or a full second.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 2;
    }

    double total = 0;
    for (int i = 1; i < argc; ++i) {
        char* end = NULL;
        errno = 0;
        double const seconds = strtod(argv[i], &end);
        if (end == argv[i] || (end && *end != '\0') || seconds < 0) {
            fprintf(stderr, "sleep: invalid interval '%s'\n", argv[i]);
            return 1;
        }
        total += seconds;
    }

    struct timespec interval;
    interval.tv_sec = (time_t)total;
    interval.tv_nsec = (long)((total - (double)interval.tv_sec) * 1e9);

    /* A signal cuts the sleep short, and the remaining time is what we go back
     * to sleep for -- otherwise a caught SIGCHLD would end the wait early. */
    while (nanosleep(&interval, &interval) < 0) {
        if (errno != EINTR)
            return 1;
    }
    return 0;
}
