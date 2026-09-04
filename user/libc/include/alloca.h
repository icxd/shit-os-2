/* SPDX-License-Identifier: MIT */
#ifndef _ALLOCA_H
#define _ALLOCA_H

#include <stddef.h>

/*
 * The compiler has to do this one: memory that goes away when the frame does
 * cannot be a function call. clang lowers __builtin_alloca to a stack pointer
 * adjustment, which is the whole of the implementation.
 */
#define alloca(size) __builtin_alloca(size)

#endif /* _ALLOCA_H */
