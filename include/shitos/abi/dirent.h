/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- getdents(2) records.
 *
 * getdents fills a caller-supplied buffer with a packed run of these; walk it
 * by stepping d_reclen bytes at a time until you have consumed the byte count
 * the call returned.
 */

#pragma once

#include <shitos/types.h>

#define NAME_MAX 255

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10

struct dirent {
    u64 d_ino;
    u16 d_reclen;
    u8 d_type;
    char d_name[]; /* NUL-terminated, padded so d_reclen stays 8-aligned */
};
