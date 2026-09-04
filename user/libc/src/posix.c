/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- the small POSIX corners.
 *
 * Path splitting, glob matching, the group database, and the handful of
 * interfaces that exist so portable software compiles and gets a true answer
 * rather than a plausible one. Where a facility genuinely is not here --
 * changing a file's timestamps, for instance -- the call reports ENOSYS
 * instead of quietly succeeding, because a caller that cares should find out.
 */

#include "internal.h"

#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <search.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <syslog.h>
#include <unistd.h>
#include <utime.h>

/* --- path splitting ------------------------------------------------------
 *
 * Both may modify the string and may return a pointer into it. That is the
 * POSIX contract; the two static buffers below are only for the cases where
 * there is no substring to point at.
 */

char* basename(char* path)
{
    static char dot[] = ".";
    static char root[] = "/";

    if (!path || !*path)
        return dot;

    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/')
        --end;
    path[end] = '\0';

    /* All slashes: the basename of "/" and "///" is "/". */
    if (end == 1 && path[0] == '/')
        return root;

    char* const slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char* dirname(char* path)
{
    static char dot[] = ".";
    static char root[] = "/";

    if (!path || !*path)
        return dot;

    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/')
        --end;
    path[end] = '\0';

    char* const slash = strrchr(path, '/');
    if (!slash)
        return dot;
    if (slash == path)
        return root;

    /* Trailing slashes on the directory part go too: dirname("/a//b") is "/a". */
    char* cut = slash;
    while (cut > path && cut[-1] == '/')
        --cut;
    *cut = '\0';
    return path;
}

/* --- glob matching -------------------------------------------------------
 *
 * Recursive, with the one case that matters for termination handled up front:
 * a '*' consumes as little as possible and recurses, so a pattern of many
 * stars against a long name backtracks rather than looping.
 */

static int fold(int c, int flags)
{
    if ((flags & FNM_CASEFOLD) == 0)
        return c;
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Matches a [...] set at `pattern` against `c`. On success `*end` is left just
 * past the closing bracket. */
static int match_set(const char* pattern, int c, int flags, const char** end)
{
    int negate = 0;
    if (*pattern == '!' || *pattern == '^') {
        negate = 1;
        ++pattern;
    }

    int matched = 0;
    int first = 1;

    /* A ']' immediately after the bracket (or its negation) is a literal. */
    for (; *pattern && (*pattern != ']' || first); ++pattern) {
        first = 0;

        if (pattern[0] == '[' && pattern[1] == ':') {
            char const* const class_start = pattern + 2;
            char const* class_end = class_start;
            while (*class_end && *class_end != ':')
                ++class_end;
            if (class_end[0] == ':' && class_end[1] == ']') {
                size_t const length = (size_t)(class_end - class_start);
                if ((length == 5 && memcmp(class_start, "alpha", 5) == 0
                        && ((c | 32) >= 'a' && (c | 32) <= 'z'))
                    || (length == 5 && memcmp(class_start, "digit", 5) == 0
                        && (c >= '0' && c <= '9'))
                    || (length == 5 && memcmp(class_start, "alnum", 5) == 0
                        && (((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9')))
                    || (length == 5 && memcmp(class_start, "space", 5) == 0
                        && (c == ' ' || (c >= '\t' && c <= '\r')))
                    || (length == 5 && memcmp(class_start, "upper", 5) == 0
                        && (c >= 'A' && c <= 'Z'))
                    || (length == 5 && memcmp(class_start, "lower", 5) == 0
                        && (c >= 'a' && c <= 'z'))
                    || (length == 5 && memcmp(class_start, "print", 5) == 0 && (c >= 32 && c < 127))
                    || (length == 5 && memcmp(class_start, "punct", 5) == 0
                        && (c >= 32 && c < 127 && !(((c | 32) >= 'a' && (c | 32) <= 'z'))
                            && !(c >= '0' && c <= '9') && c != ' '))
                    || (length == 5 && memcmp(class_start, "cntrl", 5) == 0 && (c < 32 || c == 127))
                    || (length == 6 && memcmp(class_start, "xdigit", 6) == 0
                        && ((c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f')))
                    || (length == 5 && memcmp(class_start, "blank", 5) == 0
                        && (c == ' ' || c == '\t')))
                    matched = 1;
                pattern = class_end + 1;
                continue;
            }
        }

        int low = fold((unsigned char)*pattern, flags);
        if ((flags & FNM_NOESCAPE) == 0 && *pattern == '\\' && pattern[1]) {
            ++pattern;
            low = fold((unsigned char)*pattern, flags);
        }

        if (pattern[1] == '-' && pattern[2] && pattern[2] != ']') {
            int const high = fold((unsigned char)pattern[2], flags);
            if (c >= low && c <= high)
                matched = 1;
            pattern += 2;
        } else if (c == low) {
            matched = 1;
        }
    }

    if (*pattern != ']')
        return -1; /* unterminated set: the '[' was a literal after all */

    *end = pattern + 1;
    return negate ? !matched : matched;
}

static int match_here(const char* pattern, const char* string, int flags, int at_start)
{
    while (*pattern) {
        int const c = fold((unsigned char)*string, flags);

        switch (*pattern) {
        case '?':
            if (!*string)
                return FNM_NOMATCH;
            if ((flags & FNM_PATHNAME) && *string == '/')
                return FNM_NOMATCH;
            if ((flags & FNM_PERIOD) && *string == '.' && at_start)
                return FNM_NOMATCH;
            ++pattern;
            ++string;
            at_start = 0;
            break;

        case '*': {
            /* Collapse a run of stars: they mean no more than one does. */
            while (*pattern == '*')
                ++pattern;
            if ((flags & FNM_PERIOD) && *string == '.' && at_start)
                return FNM_NOMATCH;
            if (!*pattern) {
                /* A trailing star matches the rest, but not across a slash
                 * when FNM_PATHNAME says a slash must be matched by a slash. */
                if (flags & FNM_PATHNAME)
                    return strchr(string, '/') ? FNM_NOMATCH : 0;
                return 0;
            }
            for (; *string; ++string) {
                if (match_here(pattern, string, flags, 0) == 0)
                    return 0;
                if ((flags & FNM_PATHNAME) && *string == '/')
                    break;
            }
            return match_here(pattern, string, flags, 0);
        }

        case '[': {
            if (!*string)
                return FNM_NOMATCH;
            if ((flags & FNM_PATHNAME) && *string == '/')
                return FNM_NOMATCH;
            if ((flags & FNM_PERIOD) && *string == '.' && at_start)
                return FNM_NOMATCH;

            const char* end = NULL;
            int const inside = match_set(pattern + 1, c, flags, &end);
            if (inside < 0) {
                /* Not a set after all; the bracket is a literal. */
                if (c != '[')
                    return FNM_NOMATCH;
                ++pattern;
                ++string;
                at_start = 0;
                break;
            }
            if (!inside)
                return FNM_NOMATCH;
            pattern = end;
            ++string;
            at_start = 0;
            break;
        }

        case '\\':
            if ((flags & FNM_NOESCAPE) == 0 && pattern[1]) {
                ++pattern;
                if (fold((unsigned char)*pattern, flags) != c)
                    return FNM_NOMATCH;
                ++pattern;
                ++string;
                at_start = 0;
                break;
            }
            /* fall through: a trailing backslash, or FNM_NOESCAPE */

        default:
            if (fold((unsigned char)*pattern, flags) != c)
                return FNM_NOMATCH;
            /* A period after a slash is at the start of a component too. */
            at_start = (flags & FNM_PATHNAME) && *pattern == '/';
            ++pattern;
            ++string;
            break;
        }
    }

    return *string ? FNM_NOMATCH : 0;
}

int fnmatch(const char* pattern, const char* string, int flags)
{
    if (!pattern || !string)
        return FNM_NOMATCH;
    return match_here(pattern, string, flags, 1);
}

/* --- the group database -------------------------------------------------- */

static char* s_no_members[] = { NULL };

static struct group s_root_group = {
    .gr_name = (char*)"root",
    .gr_passwd = (char*)"",
    .gr_gid = 0,
    .gr_mem = s_no_members,
};

static int s_group_enumerated;

struct group* getgrnam(const char* name)
{
    return (name && strcmp(name, "root") == 0) ? &s_root_group : NULL;
}

struct group* getgrgid(gid_t gid)
{
    return gid == 0 ? &s_root_group : NULL;
}

void setgrent(void)
{
    s_group_enumerated = 0;
}

struct group* getgrent(void)
{
    return s_group_enumerated++ == 0 ? &s_root_group : NULL;
}

void endgrent(void)
{
    s_group_enumerated = 0;
}

/* --- the rest ------------------------------------------------------------ */

int utime(const char* path, const struct utimbuf* times)
{
    (void)path;
    (void)times;
    /* Inodes are stamped when they are written and there is no call to say
     * otherwise. Succeeding without doing anything would be worse. */
    errno = ENOSYS;
    return -1;
}

int flock(int fd, int operation)
{
    (void)operation;
    /* Nothing contends, so an advisory lock nobody else can take is one you
     * already hold. The descriptor still has to be real. */
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

void* lfind(const void* key, const void* base, size_t* count, size_t width,
    int (*compare)(const void*, const void*))
{
    const char* cursor = base;
    for (size_t i = 0; i < *count; ++i, cursor += width) {
        if (compare(key, cursor) == 0)
            return (void*)cursor;
    }
    return NULL;
}

void* lsearch(const void* key, void* base, size_t* count, size_t width,
    int (*compare)(const void*, const void*))
{
    void* const found = lfind(key, base, count, width, compare);
    if (found)
        return found;

    /* Not there: append it. The caller owns the array and has promised there
     * is room for one more, which is the whole interface. */
    void* const slot = (char*)base + (*count * width);
    memcpy(slot, key, width);
    ++*count;
    return slot;
}

/* --- the system log, which is stderr ------------------------------------- */

static const char* s_log_identity;
static int s_log_options;
static int s_log_mask = 0xFF;

void openlog(const char* identity, int options, int facility)
{
    (void)facility;
    s_log_identity = identity;
    s_log_options = options;
}

void closelog(void)
{
    s_log_identity = NULL;
    s_log_options = 0;
}

int setlogmask(int mask)
{
    int const previous = s_log_mask;
    if (mask != 0)
        s_log_mask = mask;
    return previous;
}

void vsyslog(int priority, const char* format, va_list arguments)
{
    static const char* const NAMES[8]
        = { "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug" };

    int const level = priority & 7;
    if ((s_log_mask & (1 << level)) == 0)
        return;

    /* No daemon and no socket to reach one over, so the message goes where it
     * can still be read. */
    fprintf(stderr, "%s", s_log_identity ? s_log_identity : "");
    if (s_log_options & LOG_PID)
        fprintf(stderr, "[%d]", (int)getpid());
    fprintf(stderr, "%s%s: ", s_log_identity ? ": " : "", NAMES[level]);
    vfprintf(stderr, format, arguments);
    fprintf(stderr, "\n");
}

void syslog(int priority, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsyslog(priority, format, arguments);
    va_end(arguments);
}

/* --- the binary tree -----------------------------------------------------
 *
 * Unbalanced, because the interface hands the caller a bare root pointer and
 * gives no way to rebalance behind their back. That is a property of the
 * interface rather than a shortcut: a sorted insertion sequence degenerates to
 * a list, and every implementation of this has the same problem.
 */

typedef struct TreeNode {
    const void* key;
    struct TreeNode* left;
    struct TreeNode* right;
} TreeNode;

void* tsearch(const void* key, void** root, int (*compare)(const void*, const void*))
{
    if (!root || !compare)
        return NULL;

    TreeNode** slot = (TreeNode**)root;
    while (*slot) {
        int const order = compare(key, (*slot)->key);
        if (order == 0)
            return *slot;
        slot = order < 0 ? &(*slot)->left : &(*slot)->right;
    }

    TreeNode* const node = malloc(sizeof(TreeNode));
    if (!node)
        return NULL;
    node->key = key;
    node->left = NULL;
    node->right = NULL;
    *slot = node;

    /*
     * The node, not the slot holding it. Callers dereference the result as a
     * void** to get the key back -- `if (*fpp != &file)` is how sbase's du
     * asks whether the entry it just looked up was already there -- and that
     * only works because the key is the node's first member. Returning the
     * slot gives them the node pointer instead, and du walks off it.
     */
    return node;
}

void* tfind(const void* key, void* const* root, int (*compare)(const void*, const void*))
{
    if (!root || !compare)
        return NULL;

    TreeNode* const* slot = (TreeNode* const*)root;
    while (*slot) {
        int const order = compare(key, (*slot)->key);
        if (order == 0)
            return *slot;
        slot = order < 0 ? &(*slot)->left : &(*slot)->right;
    }
    return NULL;
}
