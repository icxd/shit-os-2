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
char* getcwd(char* buffer, size_t size);
int rmdir(const char* path);
int unlink(const char* path);

int isatty(int fd);

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
