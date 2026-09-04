/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- lsmod: what the kernel loaded at runtime. */

#include <errno.h>
#include <shitos.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv, char** envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    struct shitos_moduleinfo modules[32];
    int const count = shitos_modules(modules, 32);
    if (count < 0) {
        fprintf(stderr, "lsmod: %s\n", strerror(errno));
        return 1;
    }

    if (count == 0) {
        printf("no modules loaded\n");
        return 0;
    }

    printf("%-16s %4s %18s %8s\n", "MODULE", "ABI", "BASE", "SIZE");
    for (int i = 0; i < count; ++i) {
        printf("%-16s %4u %18p %7lluB\n", modules[i].name, modules[i].abi_version,
            (void*)modules[i].base, (unsigned long long)modules[i].size);
    }

    return 0;
}
