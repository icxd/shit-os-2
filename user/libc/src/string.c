/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- strings and memory. */

#include <stdlib.h>
#include <string.h>

void* memcpy(void* dest, const void* src, size_t n)
{
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n >= sizeof(unsigned long)) {
        *(unsigned long*)d = *(const unsigned long*)s;
        d += sizeof(unsigned long);
        s += sizeof(unsigned long);
        n -= sizeof(unsigned long);
    }
    while (n--)
        *d++ = *s++;
    return dest;
}

void* memmove(void* dest, const void* src, size_t n)
{
    unsigned char* d = dest;
    const unsigned char* s = src;
    if (d == s || n == 0)
        return dest;
    if (d < s)
        return memcpy(dest, src, n);
    /* Overlapping and moving up: copy backwards so each byte is read before
     * the write that would clobber it. */
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dest;
}

void* memset(void* dest, int value, size_t n)
{
    unsigned char* d = dest;
    unsigned long pattern = (unsigned char)value;
    pattern |= pattern << 8;
    pattern |= pattern << 16;
    pattern |= pattern << 32;
    while (n >= sizeof(unsigned long)) {
        *(unsigned long*)d = pattern;
        d += sizeof(unsigned long);
        n -= sizeof(unsigned long);
    }
    while (n--)
        *d++ = (unsigned char)value;
    return dest;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* x = a;
    const unsigned char* y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        ++x;
        ++y;
    }
    return 0;
}

void* memchr(const void* haystack, int needle, size_t n)
{
    const unsigned char* p = haystack;
    while (n--) {
        if (*p == (unsigned char)needle)
            return (void*)p;
        ++p;
    }
    return 0;
}

size_t strlen(const char* s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

size_t strnlen(const char* s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        ++n;
    return n;
}

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n)
{
    while (n && *a && *a == *b) {
        ++a;
        ++b;
        --n;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char* strcpy(char* dest, const char* src)
{
    char* out = dest;
    while ((*out++ = *src++))
        ;
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; ++i)
        dest[i] = src[i];
    for (; i < n; ++i)
        dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, const char* src)
{
    char* out = dest + strlen(dest);
    while ((*out++ = *src++))
        ;
    return dest;
}

char* strncat(char* dest, const char* src, size_t n)
{
    char* out = dest + strlen(dest);
    while (n-- && *src)
        *out++ = *src++;
    *out = '\0';
    return dest;
}

char* strchr(const char* s, int c)
{
    for (; *s; ++s) {
        if (*s == (char)c)
            return (char*)s;
    }
    return c == '\0' ? (char*)s : 0;
}

char* strrchr(const char* s, int c)
{
    const char* found = 0;
    for (;; ++s) {
        if (*s == (char)c)
            found = s;
        if (!*s)
            return (char*)found;
    }
}

char* strstr(const char* haystack, const char* needle)
{
    if (!*needle)
        return (char*)haystack;
    for (; *haystack; ++haystack) {
        const char* h = haystack;
        const char* n = needle;
        while (*h == *n && *n) {
            ++h;
            ++n;
        }
        if (!*n)
            return (char*)haystack;
    }
    return 0;
}

char* strpbrk(const char* s, const char* accept)
{
    for (; *s; ++s) {
        if (strchr(accept, *s))
            return (char*)s;
    }
    return 0;
}

/* The C locale is the only locale, and in it collation order is byte order. */
int strcoll(const char* a, const char* b)
{
    return strcmp(a, b);
}

size_t strxfrm(char* dest, const char* src, size_t n)
{
    size_t const length = strlen(src);
    if (n > 0) {
        size_t const copied = length < n - 1 ? length : n - 1;
        memcpy(dest, src, copied);
        dest[copied] = '\0';
    }
    return length;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char* a, const char* b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n)
{
    while (n && *a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        ++a;
        ++b;
        --n;
    }
    if (n == 0)
        return 0;
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

void* memccpy(void* dest, const void* src, int c, size_t n)
{
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n--) {
        *d++ = *s;
        if (*s++ == (unsigned char)c)
            return d;
    }
    return 0;
}

char* strdup(const char* s)
{
    size_t length = strlen(s) + 1;
    char* copy = malloc(length);
    if (copy)
        memcpy(copy, s, length);
    return copy;
}

size_t strspn(const char* s, const char* accept)
{
    size_t n = 0;
    for (; s[n]; ++n) {
        if (!strchr(accept, s[n]))
            break;
    }
    return n;
}

size_t strcspn(const char* s, const char* reject)
{
    size_t n = 0;
    for (; s[n]; ++n) {
        if (strchr(reject, s[n]))
            break;
    }
    return n;
}

char* strtok(char* s, const char* delimiters)
{
    static char* saved;
    if (s == 0)
        s = saved;
    if (s == 0)
        return 0;

    s += strspn(s, delimiters);
    if (*s == '\0') {
        saved = 0;
        return 0;
    }

    char* end = s + strcspn(s, delimiters);
    if (*end != '\0') {
        *end = '\0';
        saved = end + 1;
    } else {
        saved = 0;
    }
    return s;
}
