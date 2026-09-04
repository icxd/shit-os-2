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

/*
 * The one definition, shared by the kernel and the libc. struct stat carries
 * these, and software assigns those members to a struct timespec it declared
 * itself -- so the two have to be the same type, not merely the same shape.
 */
#ifndef __shitos_timespec_defined
#define __shitos_timespec_defined
struct timespec {
    i64 tv_sec;
    i64 tv_nsec;
};
#endif
