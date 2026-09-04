/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- the shell.
 *
 * Enough of one to be useful: builtins, external commands found on PATH,
 * pipelines, and input/output redirection. No job control, no globbing, no
 * variables, no quoting beyond the obvious -- each of those is a real feature
 * rather than a missing line, and pretending otherwise would make the shell
 * lie about what it can do.
 */

#include <errno.h>
#include <fcntl.h>
#include <shitos.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGUMENTS 32
#define MAX_PIPELINE 8
#define LINE_MAX 1024

struct Command {
    char* argv[MAX_ARGUMENTS + 1];
    int argc;
    const char* input_path;
    const char* output_path;
    int output_append;
};

static int s_last_status;

/* --- jobs ----------------------------------------------------------------
 *
 * A job is a pipeline, and every process in it shares one process group. That
 * group is the unit everything else works on: the terminal hands it to
 * exactly one group at a time, ^C and ^Z reach the whole of it, and waiting
 * for a job means waiting for the group rather than for each pid in turn.
 *
 * The shell puts itself in its own group and session at startup so that it is
 * never in the same group as a job it started -- otherwise ^C aimed at the
 * foreground job would kill the shell too.
 */

#define MAX_JOBS 32

struct Job {
    pid_t pgid;
    int used;
    int stopped;
    char command[LINE_MAX];
};

static struct Job s_jobs[MAX_JOBS];
static int s_job_control; /* off when the shell is not on a terminal */
static pid_t s_shell_pgid;

static struct Job* find_job(pid_t pgid)
{
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (s_jobs[i].used && s_jobs[i].pgid == pgid)
            return &s_jobs[i];
    }
    return NULL;
}

static struct Job* add_job(pid_t pgid, const char* command, int stopped)
{
    struct Job* job = find_job(pgid);
    if (!job) {
        for (int i = 0; i < MAX_JOBS; ++i) {
            if (!s_jobs[i].used) {
                job = &s_jobs[i];
                break;
            }
        }
    }
    if (!job)
        return NULL;

    job->used = 1;
    job->pgid = pgid;
    job->stopped = stopped;
    strncpy(job->command, command ? command : "", sizeof(job->command) - 1);
    job->command[sizeof(job->command) - 1] = '\0';
    return job;
}

static void remove_job(pid_t pgid)
{
    struct Job* job = find_job(pgid);
    if (job)
        job->used = 0;
}

static int job_number(const struct Job* job)
{
    return (int)(job - s_jobs) + 1;
}

static struct Job* job_by_number(int number)
{
    if (number < 1 || number > MAX_JOBS)
        return NULL;
    struct Job* job = &s_jobs[number - 1];
    return job->used ? job : NULL;
}

/* The most recently stopped or backgrounded job: what bare `fg` and `bg`
 * mean. */
static struct Job* current_job(void)
{
    for (int i = MAX_JOBS - 1; i >= 0; --i) {
        if (s_jobs[i].used)
            return &s_jobs[i];
    }
    return NULL;
}

/* Takes the terminal back after a job stops or finishes. Doing this before
 * printing the prompt is what stops the shell being read-blocked in the
 * background of its own terminal. */
static void reclaim_terminal(void)
{
    if (s_job_control)
        tcsetpgrp(STDIN_FILENO, s_shell_pgid);
}

/* Waits for a whole job. Returns the last process's status, and records the
 * job as stopped rather than reaping it if ^Z arrived. */
static int wait_for_job(pid_t pgid, const char* description, int foreground)
{
    int status = 0;
    int live = 0;

    for (;;) {
        int child_status = 0;
        pid_t const finished = waitpid(-pgid, &child_status, WUNTRACED);
        if (finished < 0)
            break;

        if (WIFSTOPPED(child_status)) {
            struct Job* const job = add_job(pgid, description, 1);
            if (foreground) {
                reclaim_terminal();
                printf("\n[%d]+  %-8s  %s\n", job ? job_number(job) : 0, "Stopped",
                    description ? description : "");
            }
            return 128 + WSTOPSIG(child_status);
        }

        status = child_status;
        ++live;
        /* waitpid(-pgid) returns ECHILD once the group is empty, which is how
         * this loop ends. */
    }

    (void)live;
    remove_job(pgid);
    if (foreground)
        reclaim_terminal();
    return status;
}

/* --- parsing ------------------------------------------------------------ */

/* Splits on whitespace, honouring single and double quotes so that
 * `echo "hello world"` is one argument. */
static int tokenize(char* line, char** tokens, int capacity)
{
    int count = 0;
    char* cursor = line;

    while (*cursor && count < capacity) {
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        if (!*cursor)
            break;

        /* An unquoted # starts a comment, which is the rest of the line.
         * Scripts are full of them; interactive lines rarely are, but the
         * rule is the same either way. */
        if (*cursor == '#')
            break;

        char quote = 0;
        if (*cursor == '"' || *cursor == '\'') {
            quote = *cursor;
            ++cursor;
        }

        tokens[count++] = cursor;

        while (*cursor) {
            if (quote && *cursor == quote)
                break;
            if (!quote && (*cursor == ' ' || *cursor == '\t'))
                break;
            ++cursor;
        }

        if (*cursor) {
            *cursor = '\0';
            ++cursor;
        }
    }

    tokens[count] = 0;
    return count;
}

/* Pulls redirections out of a token list, leaving only the argument vector. */
static int build_command(char** tokens, int count, struct Command* command)
{
    memset(command, 0, sizeof(*command));

    for (int i = 0; i < count; ++i) {
        if (strcmp(tokens[i], "<") == 0) {
            if (i + 1 >= count) {
                fprintf(stderr, "sh: expected a file after <\n");
                return -1;
            }
            command->input_path = tokens[++i];
        } else if (strcmp(tokens[i], ">") == 0 || strcmp(tokens[i], ">>") == 0) {
            if (i + 1 >= count) {
                fprintf(stderr, "sh: expected a file after %s\n", tokens[i]);
                return -1;
            }
            command->output_append = (tokens[i][1] == '>');
            command->output_path = tokens[++i];
        } else if (command->argc < MAX_ARGUMENTS) {
            command->argv[command->argc++] = tokens[i];
        }
    }

    command->argv[command->argc] = 0;
    return command->argc;
}

/* --- builtins ----------------------------------------------------------- */

static void print_help(void)
{
    printf("shit os 2 shell. builtins:\n");
    printf("  cd [dir]        change directory (no argument means /)\n");
    printf("  jobs            list stopped and background jobs\n");
    printf("  fg [%%n]         bring a job to the foreground\n");
    printf("  bg [%%n]         continue a stopped job in the background\n");
    printf("  pwd             print the working directory\n");
    printf("  exit [status]   leave the shell (init will start another)\n");
    printf("  help            this\n");
    printf("  shutdown        halt the machine\n");
    printf("  reboot          reset the machine\n");
    printf("\n");
    printf("everything else is looked up on PATH. pipelines with | and\n");
    printf("redirection with < > >> work; quoting works; globbing does not.\n");
    printf("a trailing & backgrounds a job. ^C interrupts the foreground job,\n");
    printf("^Z stops it.\n");
}

/* Returns 1 if the command was a builtin and has been handled. */
static int run_builtin(struct Command* command, int* should_exit)
{
    const char* name = command->argv[0];

    if (strcmp(name, "exit") == 0) {
        *should_exit = 1;
        s_last_status = command->argc > 1 ? atoi(command->argv[1]) : 0;
        return 1;
    }

    if (strcmp(name, "cd") == 0) {
        const char* target = command->argc > 1 ? command->argv[1] : "/";
        if (chdir(target) < 0) {
            fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
            s_last_status = 1;
        } else {
            s_last_status = 0;
        }
        return 1;
    }

    if (strcmp(name, "pwd") == 0) {
        char buffer[512];
        if (getcwd(buffer, sizeof(buffer)))
            printf("%s\n", buffer);
        else
            perror("pwd");
        s_last_status = 0;
        return 1;
    }

    if (strcmp(name, "jobs") == 0) {
        for (int i = 0; i < MAX_JOBS; ++i) {
            if (!s_jobs[i].used)
                continue;
            printf("[%d]+  %-8s  %s\n", i + 1, s_jobs[i].stopped ? "Stopped" : "Running",
                s_jobs[i].command);
        }
        s_last_status = 0;
        return 1;
    }

    if (strcmp(name, "fg") == 0 || strcmp(name, "bg") == 0) {
        int const to_foreground = strcmp(name, "fg") == 0;
        struct Job* job = command->argc > 1
            ? job_by_number(atoi(command->argv[1] + (command->argv[1][0] == '%' ? 1 : 0)))
            : current_job();
        if (!job) {
            fprintf(stderr, "sh: %s: no such job\n", name);
            s_last_status = 1;
            return 1;
        }
        if (!s_job_control) {
            fprintf(stderr, "sh: %s: no job control on this terminal\n", name);
            s_last_status = 1;
            return 1;
        }

        printf("%s\n", job->command);
        job->stopped = 0;

        /* SIGCONT before the terminal handover: a job that is still stopped
         * cannot react to owning it. */
        if (to_foreground) {
            pid_t const pgid = job->pgid;
            tcsetpgrp(STDIN_FILENO, pgid);
            killpg(pgid, SIGCONT);
            char text[LINE_MAX];
            strncpy(text, job->command, sizeof(text) - 1);
            text[sizeof(text) - 1] = '\0';
            int const status = wait_for_job(pgid, text, 1);
            s_last_status = status >= 128 ? status : WEXITSTATUS(status);
        } else {
            killpg(job->pgid, SIGCONT);
            s_last_status = 0;
        }
        return 1;
    }

    if (strcmp(name, "help") == 0) {
        print_help();
        s_last_status = 0;
        return 1;
    }

    if (strcmp(name, "shutdown") == 0 || strcmp(name, "halt") == 0) {
        shitos_shutdown(SHITOS_SHUTDOWN_POWEROFF);
        return 1;
    }

    if (strcmp(name, "reboot") == 0) {
        shitos_shutdown(SHITOS_SHUTDOWN_REBOOT);
        return 1;
    }

    return 0;
}

/* --- execution ---------------------------------------------------------- */

static void apply_redirections(struct Command* command)
{
    if (command->input_path) {
        int const fd = open(command->input_path, O_RDONLY, 0);
        if (fd < 0) {
            fprintf(stderr, "sh: %s: %s\n", command->input_path, strerror(errno));
            _exit(1);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    if (command->output_path) {
        int const flags = O_WRONLY | O_CREAT | (command->output_append ? O_APPEND : O_TRUNC);
        int const fd = open(command->output_path, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "sh: %s: %s\n", command->output_path, strerror(errno));
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

static int run_pipeline(struct Command* commands, int count, int background, const char* text)
{
    pid_t children[MAX_PIPELINE];
    int child_count = 0;
    int input_fd = -1;
    pid_t pgid = 0;

    for (int i = 0; i < count; ++i) {
        int pipe_fds[2] = { -1, -1 };
        int const is_last = (i == count - 1);

        if (!is_last && pipe(pipe_fds) < 0) {
            perror("sh: pipe");
            return 1;
        }

        pid_t const child = fork();
        if (child < 0) {
            perror("sh: fork");
            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
                close(pipe_fds[1]);
            }
            return 1;
        }

        if (child == 0) {
            /* A child of the shell is in the foreground, so it gets the
             * default reaction to ^C rather than the shell's -- and to ^Z,
             * which the shell also ignores. */
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            /* Join the job's group -- the first child creates it. Both the
             * parent and the child do this, because whichever runs first must
             * find the group already in place: the parent needs it to hand
             * over the terminal, the child needs it before it can be signalled
             * as part of the job. Doing it twice is harmless. */
            if (s_job_control) {
                pid_t const group = pgid != 0 ? pgid : getpid();
                setpgid(0, group);
                if (!background)
                    tcsetpgrp(STDIN_FILENO, group);
            }

            if (input_fd >= 0) {
                dup2(input_fd, STDIN_FILENO);
                close(input_fd);
            }
            if (!is_last) {
                close(pipe_fds[0]);
                dup2(pipe_fds[1], STDOUT_FILENO);
                close(pipe_fds[1]);
            }

            apply_redirections(&commands[i]);

            execvp(commands[i].argv[0], commands[i].argv);
            fprintf(stderr, "sh: %s: %s\n", commands[i].argv[0], strerror(errno));
            _exit(127);
        }

        children[child_count++] = child;

        if (s_job_control) {
            if (pgid == 0)
                pgid = child;
            setpgid(child, pgid);
        }

        /* The shell must close its copies, or the reader at the far end of
         * the pipe never sees end of file. */
        if (input_fd >= 0)
            close(input_fd);
        if (!is_last) {
            close(pipe_fds[1]);
            input_fd = pipe_fds[0];
        }
    }

    if (!s_job_control) {
        /* No terminal to hand around, so wait for each child by pid. */
        int status = 0;
        for (int i = 0; i < child_count; ++i) {
            int child_status = 0;
            waitpid(children[i], &child_status, 0);
            if (i == child_count - 1)
                status = child_status;
        }
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        return WEXITSTATUS(status);
    }

    if (!background) {
        tcsetpgrp(STDIN_FILENO, pgid);
        int const status = wait_for_job(pgid, text, 1);
        if (WIFSIGNALED(status)) {
            if (WTERMSIG(status) == SIGINT)
                printf("\n");
            return 128 + WTERMSIG(status);
        }
        /* wait_for_job hands back 128+signal directly when the job stopped. */
        if (status >= 128)
            return status;
        return WEXITSTATUS(status);
    }

    struct Job* const job = add_job(pgid, text, 0);
    printf("[%d] %d\n", job ? job_number(job) : 0, (int)pgid);
    return 0;
}

static void run_line(char* line, int* should_exit)
{
    /* Keep the text as typed: the job table shows it, and splitting the line
     * below writes NULs into it. */
    char original[LINE_MAX];
    strncpy(original, line, sizeof(original) - 1);
    original[sizeof(original) - 1] = '\0';

    /* A trailing & runs the pipeline in the background. Stripped before
     * anything else looks at the line, because it is not an argument. */
    int background = 0;
    {
        size_t end = strlen(line);
        while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t'))
            --end;
        if (end > 0 && line[end - 1] == '&') {
            background = 1;
            line[end - 1] = '\0';
            /* And out of the remembered text too. */
            size_t shown = strlen(original);
            while (shown > 0 && (original[shown - 1] == ' ' || original[shown - 1] == '\t'))
                --shown;
            if (shown > 0 && original[shown - 1] == '&')
                original[shown - 1] = '\0';
        }
    }

    /* Split on | first, then tokenize each stage. */
    char* stages[MAX_PIPELINE];
    int stage_count = 0;

    char* cursor = line;
    stages[stage_count++] = cursor;
    while (*cursor && stage_count < MAX_PIPELINE) {
        if (*cursor == '|') {
            *cursor = '\0';
            stages[stage_count++] = cursor + 1;
        }
        ++cursor;
    }

    struct Command commands[MAX_PIPELINE];
    int command_count = 0;

    for (int i = 0; i < stage_count; ++i) {
        char* tokens[MAX_ARGUMENTS + 1];
        int const token_count = tokenize(stages[i], tokens, MAX_ARGUMENTS);
        if (token_count == 0)
            continue;
        if (build_command(tokens, token_count, &commands[command_count]) <= 0)
            return;
        ++command_count;
    }

    if (command_count == 0)
        return;

    /* A builtin only runs in the shell itself when it is alone; in a pipeline
     * it would need a subshell, and running it in one would make `cd` useless. */
    if (command_count == 1 && run_builtin(&commands[0], should_exit))
        return;

    s_last_status = run_pipeline(commands, command_count, background, original);
}

/* Notices background jobs that finished or stopped while we were not looking.
 * Called before each prompt, which is where a shell is allowed to be chatty
 * about it. */
static void reap_background_jobs(void)
{
    for (;;) {
        int status = 0;
        pid_t const finished = waitpid(-1, &status, WNOHANG | WUNTRACED);
        if (finished <= 0)
            return;

        pid_t const group = getpgid(finished);
        struct Job* const job = find_job(group > 0 ? group : finished);
        if (!job)
            continue;

        if (WIFSTOPPED(status)) {
            job->stopped = 1;
            printf("[%d]+  %-8s  %s\n", job_number(job), "Stopped", job->command);
        } else {
            printf("[%d]+  %-8s  %s\n", job_number(job), "Done", job->command);
            job->used = 0;
        }
    }
}

/* Runs every line of an already-open stream. Used for `sh script` and for
 * /etc/rc, neither of which wants a prompt or a banner. */
static int run_stream(FILE* stream)
{
    char line[LINE_MAX];
    int should_exit = 0;

    while (!should_exit && fgets(line, sizeof(line), stream)) {
        size_t const length = strlen(line);
        if (length > 0 && line[length - 1] == '\n')
            line[length - 1] = '\0';
        run_line(line, &should_exit);
    }

    return s_last_status;
}

int main(int argc, char** argv, char** envp)
{
    (void)envp;

    /* ^C interrupts whatever is running, not the shell itself. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    /* ^Z likewise -- and a shell that stopped itself on SIGTTOU while handing
     * the terminal back and forth would deadlock the terminal it owns. */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    /* sh -c 'line': one command, no prompt. What system() would use. */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        int should_exit = 0;
        char line[LINE_MAX];
        strncpy(line, argv[2], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        run_line(line, &should_exit);
        return s_last_status;
    }

    /* sh script: run it and stop. This is how /etc/rc runs at boot and how
     * the test suite drives the system without a keyboard. */
    if (argc >= 2) {
        FILE* script = fopen(argv[1], "r");
        if (!script) {
            fprintf(stderr, "sh: %s: %s\n", argv[1], strerror(errno));
            return 127;
        }
        int const status = run_stream(script);
        fclose(script);
        return status;
    }

    /*
     * Job control needs three things and only makes sense with all of them: a
     * terminal, a session to own it, and a group for the shell that is never
     * the group of a job it starts. Without the last, ^C aimed at the
     * foreground job would reach the shell as well.
     *
     * setsid() fails when the shell is already a group leader, which is the
     * normal case for the shell init spawns -- that is fine, it just means the
     * session already exists.
     */
    s_job_control = isatty(STDIN_FILENO);
    if (s_job_control) {
        (void)setsid();
        s_shell_pgid = getpid();
        if (setpgid(0, s_shell_pgid) < 0 && getpgrp() != s_shell_pgid) {
            /* Cannot get a group of its own, so there is no safe way to hand
             * the terminal to a job. Run without job control rather than
             * signalling the shell along with everything it starts. */
            s_job_control = 0;
        }
    }
    if (s_job_control && tcsetpgrp(STDIN_FILENO, s_shell_pgid) < 0)
        s_job_control = 0;

    struct utsname name;
    if (uname(&name) == 0)
        printf("%s %s on %s\n", name.sysname, name.release, name.machine);
    printf("type `help` for what actually works.\n\n");

    char line[LINE_MAX];
    int should_exit = 0;

    while (!should_exit) {
        reap_background_jobs();

        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");

        printf("%s $ ", cwd);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            /* An interrupted read is not end of input. ^C at an idle prompt
             * used to end the shell here, because a NULL from fgets looks the
             * same either way until you ask which it was. */
            if (errno == EINTR) {
                clearerr(stdin);
                printf("\n");
                continue;
            }
            /* End of input: ^D at the prompt. */
            printf("\n");
            break;
        }

        size_t const length = strlen(line);
        if (length > 0 && line[length - 1] == '\n')
            line[length - 1] = '\0';

        run_line(line, &should_exit);
    }

    return s_last_status;
}
