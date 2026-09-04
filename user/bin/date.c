/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- date.
 *
 * Only worth having since there is a real-time clock to ask; before
 * modules/rtc this would have printed 1970 with great confidence. If no driver
 * registered a clock it still will, and saying so is the point -- an obviously
 * wrong date beats a subtly wrong one.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    /* -u is accepted and ignored: there is no timezone database, so local
     * time is UTC and the flag asks for what you already have. */
    int argument = 1;
    if (argument < argc && strcmp(argv[argument], "-u") == 0)
        ++argument;

    /* +FORMAT, as date(1) spells it. The default is the traditional one. */
    const char* format = "%a %b %d %H:%M:%S %Z %Y";
    if (argument < argc && argv[argument][0] == '+')
        format = argv[argument] + 1;
    else if (argument < argc) {
        fprintf(stderr, "usage: date [-u] [+format]\n");
        return 2;
    }

    time_t const now = time(NULL);
    struct tm* const broken = gmtime(&now);
    if (!broken) {
        fprintf(stderr, "date: no clock\n");
        return 1;
    }

    char rendered[256];
    if (strftime(rendered, sizeof(rendered), format, broken) == 0 && format[0] != '\0') {
        fprintf(stderr, "date: format produced nothing, or is longer than %zu bytes\n",
            sizeof(rendered));
        return 1;
    }

    printf("%s\n", rendered);
    return 0;
}
