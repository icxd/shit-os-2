/* SPDX-License-Identifier: MIT */
#ifndef _DIRENT_H
#define _DIRENT_H

#include <shitos/abi/dirent.h>

#include <sys/types.h>

typedef struct _DIR DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* directory);
int closedir(DIR* directory);
void rewinddir(DIR* directory);
int dirfd(DIR* directory);

/* Adopts an already-open descriptor. The DIR takes ownership: closedir is
 * what closes it from then on. */
DIR* fdopendir(int fd);
int dirfd(DIR* directory);

#endif /* _DIRENT_H */
