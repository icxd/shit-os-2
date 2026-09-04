/* SPDX-License-Identifier: MIT */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

/*
 * Only the widths that exist on x86-64, spelled for a 64-bit long. There is no
 * 32-bit target to keep these honest for, so anything conditional would be
 * untested by construction.
 */
#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIdMAX "ld"
#define PRIdPTR "ld"

#define PRIi8 "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIiMAX "li"

#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIuMAX "lu"
#define PRIuPTR "lu"

#define PRIo64 "lo"
#define PRIx8 "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIxMAX "lx"
#define PRIxPTR "lx"
#define PRIX64 "lX"

#define SCNd64 "ld"
#define SCNi64 "li"
#define SCNu64 "lu"
#define SCNx64 "lx"

typedef long intmax_t;
typedef unsigned long uintmax_t;

intmax_t strtoimax(const char* text, char** end, int base);
uintmax_t strtoumax(const char* text, char** end, int base);

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

intmax_t imaxabs(intmax_t value);
imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator);

#endif /* _INTTYPES_H */
