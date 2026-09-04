/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- cat. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int copy_stream(int fd, const char* name)
{
    char buffer[4096];
    for (;;) {
        ssize_t const count = read(fd, buffer, sizeof(buffer));
        if (count == 0)
            return 0;
        if (count < 0) {
            fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
            return 1;
        }

        ssize_t written = 0;
        while (written < count) {
            ssize_t const step = write(STDOUT_FILENO, buffer + written, (size_t)(count - written));
            if (step <= 0) {
                fprintf(stderr, "cat: write: %s\n", strerror(errno));
                return 1;
            }
            written += step;
        }
    }
}

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    if (argc < 2)
        return copy_stream(STDIN_FILENO, "stdin");

    int status = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-") == 0) {
            status |= copy_stream(STDIN_FILENO, "stdin");
            continue;
        }

        int const fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        status |= copy_stream(fd, argv[i]);
        close(fd);
    }
    return status;
}
