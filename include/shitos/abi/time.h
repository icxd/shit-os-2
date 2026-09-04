/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- clocks, as seen by both kernel and libc. */

#pragma once

#include <shitos/types.h>

/*
 * Wall clock. Jumps if something sets the date, which nothing does yet, and
 * reads as seconds since boot until a real-time clock driver registers.
 */
#define CLOCK_REALTIME 0
/* Since boot. Never goes backwards; what a timeout should be measured on. */
#define CLOCK_MONOTONIC 1

struct shitos_timespec {
    i64 tv_sec;
    i64 tv_nsec;
};
