/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- assertion failure. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char* expression, const char* file, unsigned line, const char* function)
{
    fprintf(stderr, "%s:%u: %s: assertion failed: %s\n", file, line, function, expression);
    abort();
}
