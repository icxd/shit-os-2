/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 -- the non-POSIX extensions.
 *
 * Everything here is specific to this system. Nothing portable should include
 * this header, which is exactly why the calls live behind their own prefix
 * rather than being smuggled into a POSIX one.
 */

#ifndef _SHITOS_H
#define _SHITOS_H

#include <shitos/abi/syscall.h>

#include <sys/types.h>

int shitos_sysinfo(struct shitos_sysinfo* out);
int shitos_procs(struct shitos_procinfo* out, size_t capacity);
int shitos_modules(struct shitos_moduleinfo* out, size_t capacity);
int shitos_shutdown(int mode);

#endif /* _SHITOS_H */
