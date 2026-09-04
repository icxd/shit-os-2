/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the BSD string functions that never made it into
 * <string.h>. Only the case-insensitive comparisons are worth having; bcopy
 * and bzero are memmove and memset under older names and are not provided.
 */

#ifndef _STRINGS_H
#define _STRINGS_H

#include <stddef.h>

int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t count);

#endif /* _STRINGS_H */
