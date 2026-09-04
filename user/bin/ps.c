/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- ps.
 *
 * Reads the process table through the shitos_procs extension call rather than
 * a /proc filesystem. A procfs is the better answer and is on the roadmap;
 * this is the honest version of what exists today.
 */

#include <errno.h>
#include <shitos.h>
#include <stdio.h>
#include <string.h>

static const char* state_name(unsigned int state)
{
    switch (state) {
    case SHITOS_PROC_STATE_RUNNING: return "run";
    case SHITOS_PROC_STATE_READY: return "ready";
    case SHITOS_PROC_STATE_BLOCKED: return "block";
    case SHITOS_PROC_STATE_ZOMBIE: return "zombie";
    default: return "?";
    }
}

int main(int argc, char** argv, char** envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    struct shitos_procinfo processes[64];
    int const count = shitos_procs(processes, 64);
    if (count < 0) {
        fprintf(stderr, "ps: %s\n", strerror(errno));
        return 1;
    }

    printf("%5s %5s %-7s %4s %9s %8s  %s\n", "PID", "PPID", "STATE", "THR", "RSS", "TICKS", "NAME");
    for (int i = 0; i < count; ++i) {
        struct shitos_procinfo* p = &processes[i];
        printf("%5d %5d %-7s %4u %8lluK %8llu  %s\n", p->pid, p->ppid, state_name(p->state),
            p->thread_count, (unsigned long long)(p->rss_bytes / 1024),
            (unsigned long long)p->cpu_ticks, p->name);
    }

    return 0;
}
