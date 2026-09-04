/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- free: memory and a few other numbers worth seeing. */

#include <errno.h>
#include <shitos.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv, char** envp)
{
    (void)argv;
    (void)envp;

    struct shitos_sysinfo info;
    if (shitos_sysinfo(&info) < 0) {
        fprintf(stderr, "free: %s\n", strerror(errno));
        return 1;
    }

    unsigned long long const total = info.mem_total_bytes / 1024;
    unsigned long long const available = info.mem_free_bytes / 1024;
    unsigned long long const used = total - available;

    printf("%14s %12s %12s %12s\n", "", "total", "used", "free");
    printf("%14s %11lluK %11lluK %11lluK\n", "physical:", total, used, available);
    printf("%14s %11lluK\n", "kernel heap:", (unsigned long long)info.mem_kernel_heap_bytes / 1024);
    printf("\n");
    printf("uptime            %llu.%03llus\n", (unsigned long long)info.uptime_ms / 1000,
        (unsigned long long)info.uptime_ms % 1000);
    printf("processes         %llu\n", (unsigned long long)info.process_count);
    printf("threads           %llu\n", (unsigned long long)info.thread_count);
    printf("modules           %llu\n", (unsigned long long)info.module_count);
    printf("context switches  %llu\n", (unsigned long long)info.context_switches);
    printf("page size         %llu\n", (unsigned long long)info.page_size);

    (void)argc;
    return 0;
}
