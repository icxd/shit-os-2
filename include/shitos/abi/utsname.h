/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- uname(2). */

#pragma once

#define UTSNAME_LEN 65

struct utsname {
    char sysname[UTSNAME_LEN];
    char nodename[UTSNAME_LEN];
    char release[UTSNAME_LEN];
    char version[UTSNAME_LEN];
    char machine[UTSNAME_LEN];
};
