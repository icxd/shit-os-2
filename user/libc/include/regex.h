/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- POSIX regular expressions.
 *
 * Both dialects: basic (BRE), where grouping is \( \) and there is no
 * alternation unless you ask for the GNU spelling \|, and extended (ERE),
 * where grouping is ( ) and + ? | are metacharacters. grep, sed, ed and expr
 * all need one or the other, which is why this exists.
 *
 * The matcher backtracks. It finds the leftmost match, and the longest one at
 * that position, which is what POSIX asks for and what a Perl-style engine
 * does not do. See the comment at the top of regex.c for where it stops short.
 */

#ifndef _REGEX_H
#define _REGEX_H

#include <stddef.h>

typedef ptrdiff_t regoff_t;

typedef struct {
    size_t re_nsub; /* how many \( \) or ( ) groups the pattern has */
    void* re_program; /* opaque; owned by regcomp, released by regfree */
    int re_cflags;
} regex_t;

typedef struct {
    regoff_t rm_so; /* byte offset of the match, or -1 */
    regoff_t rm_eo; /* byte offset just past it, or -1 */
} regmatch_t;

/* regcomp flags */
#define REG_EXTENDED 0x01 /* ERE rather than BRE */
#define REG_ICASE 0x02
#define REG_NOSUB 0x04 /* the caller wants no capture offsets */
#define REG_NEWLINE 0x08 /* . and [^...] do not match a newline; ^ $ anchor to one */

/* regexec flags */
#define REG_NOTBOL 0x10 /* the string does not begin a line, so ^ must not match */
#define REG_NOTEOL 0x20 /* ...and does not end one */

/* Return values. REG_NOMATCH is the only one that is not an error. */
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13

int regcomp(regex_t* compiled, const char* pattern, int cflags);
int regexec(const regex_t* compiled, const char* string, size_t match_count, regmatch_t matches[],
    int eflags);
size_t regerror(int code, const regex_t* compiled, char* buffer, size_t capacity);
void regfree(regex_t* compiled);

#endif /* _REGEX_H */
