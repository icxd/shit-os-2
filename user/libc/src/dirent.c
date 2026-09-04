/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- directory reading over getdents. */

#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DIRENT_BUFFER 2048

struct _DIR {
    int fd;
    char buffer[DIRENT_BUFFER];
    size_t available;
    size_t position;
};

DIR* opendir(const char* path)
{
    int const fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    DIR* directory = malloc(sizeof(DIR));
    if (!directory) {
        close(fd);
        errno = ENOMEM;
        return 0;
    }

    directory->fd = fd;
    directory->available = 0;
    directory->position = 0;
    return directory;
}

struct dirent* readdir(DIR* directory)
{
    if (!directory)
        return 0;

    if (directory->position >= directory->available) {
        long const count = __syscall3(SYS_getdents, directory->fd, (long)directory->buffer,
            (long)sizeof(directory->buffer));
        if (count <= 0) {
            if (count < 0)
                errno = (int)-count;
            return 0;
        }
        directory->available = (size_t)count;
        directory->position = 0;
    }

    struct dirent* entry = (struct dirent*)(directory->buffer + directory->position);
    /* The kernel packs records back to back; step by the length it reported
     * rather than by sizeof, which does not include the name. */
    directory->position += entry->d_reclen;
    return entry;
}

void rewinddir(DIR* directory)
{
    if (!directory)
        return;
    lseek(directory->fd, 0, SEEK_SET);
    directory->available = 0;
    directory->position = 0;
}

int dirfd(DIR* directory) { return directory ? directory->fd : -1; }

int closedir(DIR* directory)
{
    if (!directory)
        return -1;
    int const result = close(directory->fd);
    free(directory);
    return result;
}
