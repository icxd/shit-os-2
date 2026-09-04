/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- struct stat, as seen by both kernel and libc. */

#pragma once

#include <shitos/abi/time.h>
#include <shitos/types.h>

/*
 * POSIX.1-2008 spells the timestamps as struct timespec and keeps the old
 * time_t names as macros over their tv_sec. Software uses both spellings, so
 * both work here rather than one being translated at every call site.
 */

struct stat {
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    i64 st_size;
    i64 st_blksize;
    i64 st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
