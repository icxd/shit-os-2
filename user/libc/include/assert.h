/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- assert. Deliberately re-includable, as the standard says. */

#include <sys/types.h>

#undef assert

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression)                                                                         \
    ((expression) ? (void)0 : __assert_fail(#expression, __FILE__, __LINE__, __func__))
#endif

#ifndef _ASSERT_H
#define _ASSERT_H
__attribute__((noreturn)) void __assert_fail(
    const char* expression, const char* file, unsigned line, const char* function);
#endif
