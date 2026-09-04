/* SPDX-License-Identifier: MIT */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <sys/types.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* pointer, size_t size);
void free(void* pointer);

__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void abort(void);
int atexit(void (*function)(void));

int atoi(const char* s);
long atol(const char* s);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);

char* getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);

int abs(int value);
long labs(long value);

double strtod(const char* s, char** end);
float strtof(const char* s, char** end);
double atof(const char* s);

void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t count, size_t size,
    int (*compare)(const void*, const void*));

/* There is no command interpreter to hand a string to. system(NULL) correctly
 * reports that by returning 0; anything else fails. */
int system(const char* command);

#endif /* _STDLIB_H */
