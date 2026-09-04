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
    printf("  pwd             print the working directory\n");
    printf("  exit [status]   leave the shell (init will start another)\n");
    printf("  help            this\n");
    printf("  shutdown        halt the machine\n");
    printf("  reboot          reset the machine\n");
    printf("\n");
    printf("everything else is looked up on PATH. pipelines with | and\n");
    printf("redirection with < > >> work; quoting works; globbing does not.\n");
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

static int run_pipeline(struct Command* commands, int count)
{
    pid_t children[MAX_PIPELINE];
    int child_count = 0;
    int input_fd = -1;

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
             * default reaction to ^C rather than the shell's. */
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

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

        /* The shell must close its copies, or the reader at the far end of
         * the pipe never sees end of file. */
        if (input_fd >= 0)
            close(input_fd);
        if (!is_last) {
            close(pipe_fds[1]);
            input_fd = pipe_fds[0];
        }
    }

    int status = 0;
    for (int i = 0; i < child_count; ++i) {
        int child_status = 0;
        waitpid(children[i], &child_status, 0);
        /* The pipeline's status is the last command's, as POSIX specifies. */
        if (i == child_count - 1)
            status = child_status;
    }

    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == SIGINT)
            printf("\n");
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

static void run_line(char* line, int* should_exit)
{
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

    s_last_status = run_pipeline(commands, command_count);
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

    struct utsname name;
    if (uname(&name) == 0)
        printf("%s %s on %s\n", name.sysname, name.release, name.machine);
    printf("type `help` for what actually works.\n\n");

    char line[LINE_MAX];
    int should_exit = 0;

    while (!should_exit) {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");

        printf("%s $ ", cwd);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
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
