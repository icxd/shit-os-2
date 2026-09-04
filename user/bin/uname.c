/* SPDX-License-Identifier: GPL-3.0-or-later */
/* shit os 2 -- uname. */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    struct utsname name;
    if (uname(&name) < 0) {
        fprintf(stderr, "uname: %s\n", strerror(errno));
        return 1;
    }

    int all = 0, sysname = 0, nodename = 0, release = 0, version = 0, machine = 0;

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-')
            continue;
        for (const char* flag = argv[i] + 1; *flag; ++flag) {
            switch (*flag) {
            case 'a': all = 1; break;
            case 's': sysname = 1; break;
            case 'n': nodename = 1; break;
            case 'r': release = 1; break;
            case 'v': version = 1; break;
            case 'm': machine = 1; break;
            default:
                fprintf(stderr, "uname: unknown option -%c\n", *flag);
                return 2;
            }
        }
    }

    if (all) {
        printf("%s %s %s %s %s\n", name.sysname, name.nodename, name.release, name.version,
            name.machine);
        return 0;
    }

    /* No flags at all means -s, as everywhere else. */
    if (!sysname && !nodename && !release && !version && !machine)
        sysname = 1;

    int printed = 0;
    if (sysname) printf("%s%s", printed++ ? " " : "", name.sysname);
    if (nodename) printf("%s%s", printed++ ? " " : "", name.nodename);
    if (release) printf("%s%s", printed++ ? " " : "", name.release);
    if (version) printf("%s%s", printed++ ? " " : "", name.version);
    if (machine) printf("%s%s", printed++ ? " " : "", name.machine);
    printf("\n");

    return 0;
}
