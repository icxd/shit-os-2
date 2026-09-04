/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the search functions that are worth having.
 *
 * The hash table (hcreate/hsearch) and the tree (tsearch) are not here: both
 * have interfaces built around a single global or a caller-managed void**,
 * and nothing in this tree wants either.
 */

#ifndef _SEARCH_H
#define _SEARCH_H

#include <stddef.h>

/* Linear search, optionally appending the key when it is not found -- which
 * is the only reason to prefer it over a loop you wrote yourself. */
void* lfind(const void* key, const void* base, size_t* count, size_t width,
    int (*compare)(const void*, const void*));
void* lsearch(const void* key, void* base, size_t* count, size_t width,
    int (*compare)(const void*, const void*));

/*
 * The binary tree. tdelete and twalk are not here: nothing wants them, and an
 * unbalanced tree with a caller-managed root is a poor interface to offer
 * more of than necessary.
 */
void* tsearch(const void* key, void** root, int (*compare)(const void*, const void*));
void* tfind(const void* key, void* const* root, int (*compare)(const void*, const void*));

#endif /* _SEARCH_H */
