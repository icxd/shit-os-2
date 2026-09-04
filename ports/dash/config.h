/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- dash's config.h, written by hand.
 *
 * dash configures with autoconf, and autoconf cross-compiles by running its
 * probes and then being told to ignore half of them. Every answer here is one
 * we already know: this is the only libc dash will ever be built against in
 * this tree, and a cache file full of ac_cv_ overrides would say the same
 * things less legibly.
 *
 * Everything defined is a claim about our libc, and everything absent is a
 * gap. A HAVE_ that has to be removed later is the honest way to record one.
 */

#define SHELL 1

/* Job control. The whole reason dash is here: it is the thing that exercises
 * process groups, terminal ownership and stop signals in anger. */
#define JOBS 1

/* No line editor. dash's own is optional and needs libedit, which would be a
 * second port to justify the first. */
#define SMALL 1

/* Headers we have. */
#define HAVE_ALLOCA_H 1
#define HAVE_PATHS_H 1
#define HAVE_DECL_ISBLANK 1

/* Functions we have. Each of these was added to the libc for this port, which
 * is what a port is for. */
#define HAVE_BSEARCH 1
#define HAVE_GETPWNAM 1
#define HAVE_GETRLIMIT 1
#define HAVE_ISALPHA 1
#define HAVE_KILLPG 1
#define HAVE_MEMPCPY 1
#define HAVE_SETRLIMIT 1
#define HAVE_STPCPY 1
#define HAVE_STPNCPY 1
#define HAVE_STRCHRNUL 1
#define HAVE_STRSIGNAL 1
#define HAVE_STRSTR 1
#define HAVE_STRTOD 1
#define HAVE_STRTOIMAX 1
#define HAVE_STRTOUMAX 1
#define HAVE_SYSCONF 1

/*
 * Deliberately absent, and each absence is a real one rather than an
 * oversight:
 *
 *   HAVE_FNMATCH, HAVE_GLOB   dash has its own pattern matcher and globber,
 *                             and ours would be a worse copy of them.
 *   HAVE_SIGSETMASK           the BSD spelling; sigprocmask is what we have.
 *   HAVE_FACCESSAT            no directory-relative calls yet.
 *   HAVE_ST_MTIM              struct stat carries st_mtime, not a timespec.
 */

#define SIZEOF_INTMAX_T 8
#define SIZEOF_LONG_LONG_INT 8

/*
 * There is one size of everything: long is 64 bits and off_t is long, so the
 * *64 interfaces are the same functions under a longer name.
 */
#define fstat64 fstat
#define lstat64 lstat
#define stat64 stat
#define open64 open
#define readdir64 readdir
#define dirent64 dirent

/*
 * No vfork. Its point is to avoid copying an address space that is about to be
 * replaced, and our fork copies eagerly -- so vfork would be a slower lie. The
 * only legal thing a vforked child may do is exec or _exit, which is exactly
 * what a forked one may do too, so this substitution is sound for every use
 * dash makes of it.
 */
#define vfork fork
