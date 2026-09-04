/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- pid 1.
 *
 * Two jobs, and it must never fail at either. It keeps a shell running, and it
 * reaps the orphans that get reparented to it when their own parent exits
 * without waiting. If init exits, userspace is over, so it does not.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void print_motd(void)
{
    int const fd = open("/etc/motd", O_RDONLY, 0);
    if (fd < 0)
        return;

    char buffer[512];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0)
        write(STDOUT_FILENO, buffer, (size_t)count);
    close(fd);
}

/*
 * If /etc/rc exists, run it to completion before the interactive shell comes
 * up. This is the only way to get work done at boot on a machine with no way
 * to script the keyboard, which is what the regression suite needs.
 */
static void run_boot_script(void)
{
    if (access("/etc/rc", R_OK) != 0)
        return;

    pid_t const child = fork();
    if (child < 0) {
        perror("init: fork");
        return;
    }

    if (child == 0) {
        char* argv[] = { (char*)"/bin/sh", (char*)"/etc/rc", 0 };
        execve("/bin/sh", argv, environ);
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        ;
}

static pid_t spawn_shell(void)
{
    pid_t const child = fork();
    if (child < 0) {
        perror("init: fork");
        return -1;
    }

    if (child == 0) {
        char* argv[] = { (char*)"/bin/sh", 0 };
        execve("/bin/sh", argv, environ);
        /* Only reached if exec failed, and there is no shell to report to. */
        perror("init: exec /bin/sh");
        _exit(127);
    }

    return child;
}

int main(int argc, char** argv, char** envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    /* init must not be killed by anything the terminal can send. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    printf("\n");
    print_motd();
    printf("\n");

    run_boot_script();

    pid_t shell = spawn_shell();

    for (;;) {
        int status = 0;
        pid_t const finished = waitpid(-1, &status, 0);

        if (finished < 0) {
            /* No children at all should be impossible while the shell is
             * running, but sleeping beats spinning if it happens. */
            sleep(1);
            continue;
        }

        if (finished == shell) {
            if (WIFSIGNALED(status))
                printf("\ninit: shell killed by signal %d, restarting\n", WTERMSIG(status));
            else
                printf("\ninit: shell exited with status %d, restarting\n", WEXITSTATUS(status));
            shell = spawn_shell();
        }
        /* Anything else was an orphan reparented here; reaping it was the
         * whole point and there is nothing to report. */
    }

    return 0;
}
