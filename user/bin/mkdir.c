/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- mkdir, with -p. */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int make_parents(char* path)
{
    /* Walk the path, terminating it at each separator in turn so that each
     * prefix can be created before the next one is tried. */
    for (char* cursor = path + 1; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(path, 0755) < 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir: %s: %s\n", path, strerror(errno));
            return 1;
        }
        *cursor = '/';
    }

    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir: %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    int parents = 0;
    int first = 1;

    if (argc > 1 && strcmp(argv[1], "-p") == 0) {
        parents = 1;
        first = 2;
    }

    if (first >= argc) {
        fprintf(stderr, "usage: mkdir [-p] directory...\n");
        return 2;
    }

    int status = 0;
    for (int i = first; i < argc; ++i) {
        if (parents) {
            status |= make_parents(argv[i]);
        } else if (mkdir(argv[i], 0755) < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
