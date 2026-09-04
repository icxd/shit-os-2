/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- ls, with -l and -a. */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void format_mode(unsigned int mode, char* out)
{
    out[0] = S_ISDIR(mode) ? 'd'
        : S_ISCHR(mode)    ? 'c'
        : S_ISBLK(mode)    ? 'b'
        : S_ISFIFO(mode)   ? 'p'
        : S_ISLNK(mode)    ? 'l'
                           : '-';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static int list_one(
    const char* path, int long_format, int show_hidden, int print_header, int multiple)
{
    struct stat status;
    if (stat(path, &status) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    /* A plain file argument is listed as itself, not opened as a directory. */
    if (!S_ISDIR(status.st_mode)) {
        if (long_format) {
            char mode[11];
            format_mode(status.st_mode, mode);
            printf("%s %8lld %s\n", mode, (long long)status.st_size, path);
        } else {
            printf("%s\n", path);
        }
        return 0;
    }

    DIR* directory = opendir(path);
    if (!directory) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (multiple && print_header)
        printf("%s:\n", path);

    int column = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)) != 0) {
        if (!show_hidden && entry->d_name[0] == '.')
            continue;

        if (!long_format) {
            printf("%-20s", entry->d_name);
            if (++column % 4 == 0)
                printf("\n");
            continue;
        }

        char full[512];
        if (strcmp(path, "/") == 0)
            snprintf(full, sizeof(full), "/%s", entry->d_name);
        else
            snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);

        struct stat child;
        if (stat(full, &child) < 0) {
            printf("?????????? %8s %s\n", "?", entry->d_name);
            continue;
        }

        char mode[11];
        format_mode(child.st_mode, mode);
        printf("%s %8lld %s\n", mode, (long long)child.st_size, entry->d_name);
    }

    if (!long_format && column % 4 != 0)
        printf("\n");

    closedir(directory);
    return 0;
}

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    int long_format = 0;
    int show_hidden = 0;
    int first_path = argc;
    int path_count = 0;

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char* flag = argv[i] + 1; *flag; ++flag) {
                if (*flag == 'l')
                    long_format = 1;
                else if (*flag == 'a')
                    show_hidden = 1;
                else {
                    fprintf(stderr, "ls: unknown option -%c\n", *flag);
                    return 2;
                }
            }
        } else {
            if (first_path == argc)
                first_path = i;
            ++path_count;
        }
    }

    if (path_count == 0)
        return list_one(".", long_format, show_hidden, 0, 0);

    int status = 0;
    for (int i = first_path; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;
        status |= list_one(argv[i], long_format, show_hidden, 1, path_count > 1);
    }
    return status;
}
