/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- what runs between _start and main. */

#include "internal.h"

#include <stdlib.h>

extern int main(int argc, char** argv, char** envp);

__attribute__((noreturn)) void __libc_start(int argc, char** argv, char** envp)
{
    environ = envp;
    __stdio_initialize();
    exit(main(argc, argv, envp));
}
