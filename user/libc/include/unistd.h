/* SPDX-License-Identifier: MIT */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <shitos/abi/fcntl.h>

#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

extern char** environ;

ssize_t read(int fd, void* buffer, size_t count);
ssize_t write(int fd, const void* buffer, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);

pid_t fork(void);
int execve(const char* path, char* const argv[], char* const envp[]);
int execv(const char* path, char* const argv[]);
int execvp(const char* file, char* const argv[]);
__attribute__((noreturn)) void _exit(int status);

pid_t getpid(void);
pid_t getppid(void);

int dup(int fd);
int dup2(int fd, int to);
int pipe(int fds[2]);

int chdir(const char* path);

/*
 * Job control. A session is a login, a process group is a job within it, and
 * both are named by the pid of whichever process started them -- so there is
 * no allocator, just setsid() and setpgid(0, 0).
 */
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
pid_t setsid(void);
pid_t getsid(pid_t pid);

/* Which process group owns the terminal on `fd`. The shell moves it as jobs
 * come to the foreground; a background group that reads gets SIGTTIN. */
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgid);
char* getcwd(char* buffer, size_t size);
int rmdir(const char* path);
int unlink(const char* path);

int isatty(int fd);

/*
 * There are no users. All four of these report 0, which is true -- everything
 * runs as root -- rather than a placeholder.
 */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

/* One group, and it is root's. Returns 1 and fills in gid 0, or reports the
 * count when asked for none. */
int getgroups(int count, gid_t* groups);

/* The file creation mask. Inherited across fork and kept across exec. */
mode_t umask(mode_t mask);

long sysconf(int name);
#define _SC_OPEN_MAX 4
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE 30
#define _SC_CLK_TCK 2
#define _SC_NPROCESSORS_ONLN 84
#define _SC_ARG_MAX 0

/* There is no permission model yet, so access() answers "does it exist and is
 * it the right kind of thing", which is what every caller here wants. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char* path, int mode);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int microseconds);

void* sbrk(long increment);

#endif /* _UNISTD_H */
