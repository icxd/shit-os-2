/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- struct stat, as seen by both kernel and libc. */

#pragma once

#include <shitos/types.h>

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
    i64 st_atime;
    i64 st_mtime;
    i64 st_ctime;
};
