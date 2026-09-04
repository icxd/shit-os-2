/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- the non-POSIX extension calls. */

#include "internal.h"

#include <shitos.h>

int shitos_sysinfo(struct shitos_sysinfo* out)
{
    return (int)__syscall_return(__syscall1(SYS_shitos_sysinfo, (long)out));
}

int shitos_procs(struct shitos_procinfo* out, size_t capacity)
{
    return (int)__syscall_return(__syscall2(SYS_shitos_procs, (long)out, (long)capacity));
}

int shitos_modules(struct shitos_moduleinfo* out, size_t capacity)
{
    return (int)__syscall_return(__syscall2(SYS_shitos_modules, (long)out, (long)capacity));
}

int shitos_shutdown(int mode)
{
    return (int)__syscall_return(__syscall1(SYS_shitos_shutdown, mode));
}
