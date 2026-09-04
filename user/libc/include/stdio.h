/* SPDX-License-Identifier: MIT */
#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 1024

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _FILE FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* fopen(const char* path, const char* mode);
int fclose(FILE* stream);
int fflush(FILE* stream);

size_t fread(void* buffer, size_t size, size_t count, FILE* stream);
size_t fwrite(const void* buffer, size_t size, size_t count, FILE* stream);

int fgetc(FILE* stream);
char* fgets(char* buffer, int size, FILE* stream);
int fputc(int c, FILE* stream);
int fputs(const char* s, FILE* stream);
int getchar(void);
int putchar(int c);
int puts(const char* s);
int ungetc(int c, FILE* stream);

int feof(FILE* stream);
int ferror(FILE* stream);
int fileno(FILE* stream);

int printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE* stream, const char* format, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char* buffer, const char* format, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char* buffer, size_t size, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
int vprintf(const char* format, va_list args);
int vfprintf(FILE* stream, const char* format, va_list args);
int vsnprintf(char* buffer, size_t size, const char* format, va_list args);

void perror(const char* prefix);

#endif /* _STDIO_H */
