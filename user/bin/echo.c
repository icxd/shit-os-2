/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- echo. */

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    int first = 1;
    int newline = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        first = 2;
    }

    for (int i = first; i < argc; ++i) {
        if (i > first)
            printf(" ");
        printf("%s", argv[i]);
    }

    if (newline)
        printf("\n");
    return 0;
}
