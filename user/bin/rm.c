/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- rm, with -r. */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int remove_path(const char* path, int recursive, int force);

static int remove_directory_contents(const char* path, int force)
{
    DIR* directory = opendir(path);
    if (!directory) {
        if (!force)
            fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return force ? 0 : 1;
    }

    int status = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)) != 0) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char child[512];
        if (strcmp(path, "/") == 0)
            snprintf(child, sizeof(child), "/%s", entry->d_name);
        else
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        status |= remove_path(child, 1, force);
        /* Removing an entry invalidates the position we were reading from,
         * so start the enumeration again. */
        rewinddir(directory);
    }

    closedir(directory);
    return status;
}

static int remove_path(const char* path, int recursive, int force)
{
    struct stat status;
    if (stat(path, &status) < 0) {
        if (!force)
            fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return force ? 0 : 1;
    }

    if (S_ISDIR(status.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        if (remove_directory_contents(path, force) != 0)
            return 1;
        if (rmdir(path) < 0) {
            if (!force)
                fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
            return force ? 0 : 1;
        }
        return 0;
    }

    if (unlink(path) < 0) {
        if (!force)
            fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return force ? 0 : 1;
    }
    return 0;
}

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    int recursive = 0;
    int force = 0;
    int first = 1;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        for (const char* flag = argv[first] + 1; *flag; ++flag) {
            if (*flag == 'r' || *flag == 'R')
                recursive = 1;
            else if (*flag == 'f')
                force = 1;
            else {
                fprintf(stderr, "rm: unknown option -%c\n", *flag);
                return 2;
            }
        }
        ++first;
    }

    if (first >= argc) {
        if (force)
            return 0;
        fprintf(stderr, "usage: rm [-rf] file...\n");
        return 2;
    }

    int status = 0;
    for (int i = first; i < argc; ++i)
        status |= remove_path(argv[i], recursive, force);
    return status;
}
