// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- string and memory primitives.

#include <kernel/lib/string.h>

extern "C" {

void* memcpy(void* dest, void const* src, usize n)
{
    auto* d = static_cast<u8*>(dest);
    auto const* s = static_cast<u8 const*>(src);

    // Copy whole words while the tail allows it. Nothing clever, but it makes
    // page-sized copies roughly eight times less miserable.
    while (n >= sizeof(u64)) {
        *reinterpret_cast<u64*>(d) = *reinterpret_cast<u64 const*>(s);
        d += sizeof(u64);
        s += sizeof(u64);
        n -= sizeof(u64);
    }
    while (n--)
        *d++ = *s++;
    return dest;
}

void* memmove(void* dest, void const* src, usize n)
{
    auto* d = static_cast<u8*>(dest);
    auto const* s = static_cast<u8 const*>(src);
    if (d == s || n == 0)
        return dest;

    if (d < s)
        return memcpy(dest, src, n);

    // Overlapping and moving upward: copy backwards so we read each byte
    // before the write that would clobber it.
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dest;
}

void* memset(void* dest, int value, usize n)
{
    auto* d = static_cast<u8*>(dest);
    u8 const byte = static_cast<u8>(value);

    u64 pattern = byte;
    pattern |= pattern << 8;
    pattern |= pattern << 16;
    pattern |= pattern << 32;

    while (n >= sizeof(u64)) {
        *reinterpret_cast<u64*>(d) = pattern;
        d += sizeof(u64);
        n -= sizeof(u64);
    }
    while (n--)
        *d++ = byte;
    return dest;
}

int memcmp(void const* a, void const* b, usize n)
{
    auto const* x = static_cast<u8 const*>(a);
    auto const* y = static_cast<u8 const*>(b);
    while (n--) {
        if (*x != *y)
            return static_cast<int>(*x) - static_cast<int>(*y);
        ++x;
        ++y;
    }
    return 0;
}

void* memchr(void const* haystack, int needle, usize n)
{
    auto const* p = static_cast<u8 const*>(haystack);
    while (n--) {
        if (*p == static_cast<u8>(needle))
            return const_cast<u8*>(p);
        ++p;
    }
    return nullptr;
}

usize strlen(char const* s)
{
    usize n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

usize strnlen(char const* s, usize max)
{
    usize n = 0;
    while (n < max && s[n] != '\0')
        ++n;
    return n;
}

int strcmp(char const* a, char const* b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<int>(static_cast<u8>(*a)) - static_cast<int>(static_cast<u8>(*b));
}

int strncmp(char const* a, char const* b, usize n)
{
    while (n > 0 && *a != '\0' && *a == *b) {
        ++a;
        ++b;
        --n;
    }
    if (n == 0)
        return 0;
    return static_cast<int>(static_cast<u8>(*a)) - static_cast<int>(static_cast<u8>(*b));
}

char* strcpy(char* dest, char const* src)
{
    char* out = dest;
    while ((*out++ = *src++) != '\0') { }
    return dest;
}

char* strncpy(char* dest, char const* src, usize n)
{
    usize i = 0;
    for (; i < n && src[i] != '\0'; ++i)
        dest[i] = src[i];
    for (; i < n; ++i)
        dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, char const* src)
{
    char* out = dest + strlen(dest);
    while ((*out++ = *src++) != '\0') { }
    return dest;
}

char const* strchr(char const* s, int c)
{
    for (; *s != '\0'; ++s) {
        if (*s == static_cast<char>(c))
            return s;
    }
    return c == '\0' ? s : nullptr;
}

char const* strrchr(char const* s, int c)
{
    char const* found = nullptr;
    for (;; ++s) {
        if (*s == static_cast<char>(c))
            found = s;
        if (*s == '\0')
            return found;
    }
}

char const* strstr(char const* haystack, char const* needle)
{
    if (*needle == '\0')
        return haystack;
    for (; *haystack != '\0'; ++haystack) {
        char const* h = haystack;
        char const* n = needle;
        while (*h == *n && *n != '\0') {
            ++h;
            ++n;
        }
        if (*n == '\0')
            return haystack;
    }
    return nullptr;
}
}
