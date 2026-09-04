/* SPDX-License-Identifier: MIT */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <shitos/abi/mman.h>

#include <sys/types.h>

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void* address, size_t length);

#endif /* _SYS_MMAN_H */
