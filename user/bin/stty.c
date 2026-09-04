/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- stty.
 *
 * Reports and changes terminal settings. Small, but it earns its place twice
 * over: it is the utility you reach for when a terminal is misbehaving, and it
 * is the only thing in userland that exercises ioctl with a real struct. That
 * path went untested long enough to hide a buffer bug, and a program using it
 * is what stops that happening again.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

struct Flag {
    const char* name;
    unsigned int bit;
};

static const struct Flag LOCAL_FLAGS[] = {
    { "isig", ISIG },
    { "icanon", ICANON },
    { "echo", ECHO },
};

static const struct Flag INPUT_FLAGS[] = {
    { "icrnl", ICRNL },
};

static const struct Flag OUTPUT_FLAGS[] = {
    { "opost", OPOST },
    { "onlcr", ONLCR },
};

static void print_flags(
    const char* label, const struct Flag* flags, size_t count, unsigned int value)
{
    printf("%-8s", label);
    for (size_t i = 0; i < count; ++i)
        printf(" %s%s", (value & flags[i].bit) ? "" : "-", flags[i].name);
    printf("\n");
}

static int apply(struct termios* settings, const char* word)
{
    int enable = 1;
    if (word[0] == '-') {
        enable = 0;
        ++word;
    }

    struct {
        const struct Flag* flags;
        size_t count;
        unsigned int* field;
    } groups[] = {
        { LOCAL_FLAGS, sizeof(LOCAL_FLAGS) / sizeof(LOCAL_FLAGS[0]), &settings->c_lflag },
        { INPUT_FLAGS, sizeof(INPUT_FLAGS) / sizeof(INPUT_FLAGS[0]), &settings->c_iflag },
        { OUTPUT_FLAGS, sizeof(OUTPUT_FLAGS) / sizeof(OUTPUT_FLAGS[0]), &settings->c_oflag },
    };

    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); ++g) {
        for (size_t i = 0; i < groups[g].count; ++i) {
            if (strcmp(groups[g].flags[i].name, word) != 0)
                continue;
            if (enable)
                *groups[g].field |= groups[g].flags[i].bit;
            else
                *groups[g].field &= ~groups[g].flags[i].bit;
            return 0;
        }
    }

    fprintf(stderr, "stty: unknown setting: %s\n", word);
    return 1;
}

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    /*
     * A canary either side of the settings. If ioctl ever goes back to moving
     * a fixed number of bytes, this is what notices before anything else does.
     */
    volatile unsigned long guard_before = 0x5A5A5A5A5A5A5A5AUL;
    struct termios settings;
    volatile unsigned long guard_after = 0xA5A5A5A5A5A5A5A5UL;

    if (tcgetattr(STDIN_FILENO, &settings) < 0) {
        fprintf(stderr, "stty: %s\n", strerror(errno));
        return 1;
    }

    if (guard_before != 0x5A5A5A5A5A5A5A5AUL || guard_after != 0xA5A5A5A5A5A5A5A5UL) {
        fprintf(stderr, "stty: tcgetattr wrote outside the struct it was given\n");
        return 1;
    }

    if (argc > 1) {
        int status = 0;
        for (int i = 1; i < argc; ++i)
            status |= apply(&settings, argv[i]);
        if (status != 0)
            return status;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) < 0) {
            fprintf(stderr, "stty: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }

    struct winsize size;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0)
        printf("size     %u rows, %u columns\n", size.ws_row, size.ws_col);

    print_flags(
        "input", INPUT_FLAGS, sizeof(INPUT_FLAGS) / sizeof(INPUT_FLAGS[0]), settings.c_iflag);
    print_flags(
        "output", OUTPUT_FLAGS, sizeof(OUTPUT_FLAGS) / sizeof(OUTPUT_FLAGS[0]), settings.c_oflag);
    print_flags(
        "local", LOCAL_FLAGS, sizeof(LOCAL_FLAGS) / sizeof(LOCAL_FLAGS[0]), settings.c_lflag);

    printf("special  intr=^%c erase=^%c kill=^%c eof=^%c\n", settings.c_cc[VINTR] + '@',
        settings.c_cc[VERASE] == 8 ? 'H' : settings.c_cc[VERASE] + '@', settings.c_cc[VKILL] + '@',
        settings.c_cc[VEOF] + '@');

    return 0;
}
