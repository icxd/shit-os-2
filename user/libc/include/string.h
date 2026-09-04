/* SPDX-License-Identifier: MIT */
#ifndef _STRING_H
#define _STRING_H

#include <sys/types.h>

void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memset(void* dest, int value, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* haystack, int needle, size_t n);

size_t strlen(const char* s);
size_t strnlen(const char* s, size_t max);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strdup(const char* s);
char* strtok(char* s, const char* delimiters);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);

char* strpbrk(const char* s, const char* accept);
int strcoll(const char* a, const char* b);
size_t strxfrm(char* dest, const char* src, size_t n);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
void* memccpy(void* dest, const void* src, int c, size_t n);

char* strerror(int code);

/*
 * The GNU extensions that portable software reaches for anyway. Each returns a
 * pointer to the *end* of what it wrote, which is what makes chaining them
 * cheaper than a strlen after every step.
 */
char* stpcpy(char* destination, const char* source);
char* stpncpy(char* destination, const char* source, size_t count);
void* mempcpy(void* destination, const void* source, size_t count);
char* strchrnul(const char* text, int c);

#endif /* _STRING_H */
