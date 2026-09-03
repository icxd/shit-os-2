// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- the freestanding string.h the compiler assumes exists.
//
// Clang lowers struct copies and array initialisation to memcpy/memset calls
// even with -ffreestanding, so these are not optional.

#pragma once

#include <shitos/types.h>

extern "C" {

void* memcpy(void* dest, void const* src, usize n);
void* memmove(void* dest, void const* src, usize n);
void* memset(void* dest, int value, usize n);
int memcmp(void const* a, void const* b, usize n);
void* memchr(void const* haystack, int needle, usize n);

usize strlen(char const* s);
usize strnlen(char const* s, usize max);
int strcmp(char const* a, char const* b);
int strncmp(char const* a, char const* b, usize n);
char* strcpy(char* dest, char const* src);
char* strncpy(char* dest, char const* src, usize n);
char* strcat(char* dest, char const* src);
char const* strchr(char const* s, int c);
char const* strrchr(char const* s, int c);
char const* strstr(char const* haystack, char const* needle);
}
