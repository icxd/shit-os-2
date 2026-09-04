/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- shell glob matching.
 *
 * The same pattern language a shell uses for filenames: ? for one character,
 * * for any run, [abc] and [!a-z] for a set. Not a regex, and deliberately
 * not implemented in terms of one -- the two languages disagree about almost
 * every metacharacter, and a translation layer between them is how subtle
 * matching bugs happen.
 */

#ifndef _FNMATCH_H
#define _FNMATCH_H

#define FNM_NOMATCH 1

#define FNM_NOESCAPE 0x01 /* treat backslash literally */
#define FNM_PATHNAME 0x02 /* a slash must be matched by a slash */
#define FNM_PERIOD 0x04 /* a leading period must be matched explicitly */
#define FNM_CASEFOLD 0x10

int fnmatch(const char* pattern, const char* string, int flags);

#endif /* _FNMATCH_H */
