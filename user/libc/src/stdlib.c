/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- allocation, conversion, exit.
 *
 * malloc is a first-fit free list over sbrk with splitting and coalescing.
 * It is not fast, but it does not leak the way a bump allocator does, and its
 * behaviour is easy to reason about when something goes wrong.
 */

#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGNMENT 16
#define MIN_GROWTH 16384

typedef struct Block {
    size_t size; /* payload bytes, not counting this header */
    struct Block* next;
    int is_free;
} Block;

static Block* s_heap;
static Block* s_heap_tail;

static size_t align_up(size_t value)
{
    return (value + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1);
}

static Block* extend_heap(size_t payload)
{
    size_t total = align_up(sizeof(Block)) + payload;
    if (total < MIN_GROWTH)
        total = MIN_GROWTH;

    void* memory = sbrk((long)total);
    if (memory == (void*)-1)
        return 0;

    Block* block = memory;
    block->size = total - align_up(sizeof(Block));
    block->next = 0;
    block->is_free = 1;

    if (s_heap_tail)
        s_heap_tail->next = block;
    else
        s_heap = block;
    s_heap_tail = block;

    return block;
}

static void split(Block* block, size_t wanted)
{
    size_t const header = align_up(sizeof(Block));
    /* Only split when the remainder can hold a header plus something useful. */
    if (block->size < wanted + header + ALIGNMENT)
        return;

    Block* rest = (Block*)((char*)block + header + wanted);
    rest->size = block->size - wanted - header;
    rest->is_free = 1;
    rest->next = block->next;

    block->size = wanted;
    block->next = rest;

    if (s_heap_tail == block)
        s_heap_tail = rest;
}

void* malloc(size_t size)
{
    if (size == 0)
        return 0;

    size_t const wanted = align_up(size);

    for (Block* block = s_heap; block; block = block->next) {
        if (block->is_free && block->size >= wanted) {
            split(block, wanted);
            block->is_free = 0;
            return (char*)block + align_up(sizeof(Block));
        }
    }

    Block* block = extend_heap(wanted);
    if (!block) {
        errno = ENOMEM;
        return 0;
    }
    split(block, wanted);
    block->is_free = 0;
    return (char*)block + align_up(sizeof(Block));
}

void free(void* pointer)
{
    if (!pointer)
        return;

    Block* block = (Block*)((char*)pointer - align_up(sizeof(Block)));
    block->is_free = 1;

    /* Coalesce forward as far as possible. Blocks are kept in address order,
     * so a single pass over the list merges every adjacent free run. */
    for (Block* current = s_heap; current; current = current->next) {
        while (current->is_free && current->next && current->next->is_free) {
            Block* next = current->next;
            current->size += align_up(sizeof(Block)) + next->size;
            current->next = next->next;
            if (s_heap_tail == next)
                s_heap_tail = current;
        }
    }
}

void* calloc(size_t count, size_t size)
{
    /* Refuse a multiplication that would wrap rather than returning a buffer
     * smaller than the caller asked for. */
    if (count != 0 && size > (size_t)-1 / count) {
        errno = ENOMEM;
        return 0;
    }
    size_t const total = count * size;
    void* memory = malloc(total);
    if (memory)
        memset(memory, 0, total);
    return memory;
}

void* realloc(void* pointer, size_t size)
{
    if (!pointer)
        return malloc(size);
    if (size == 0) {
        free(pointer);
        return 0;
    }

    Block* block = (Block*)((char*)pointer - align_up(sizeof(Block)));
    if (block->size >= size)
        return pointer;

    void* replacement = malloc(size);
    if (!replacement)
        return 0;
    memcpy(replacement, pointer, block->size);
    free(pointer);
    return replacement;
}

/* --- exit -------------------------------------------------------------- */

#define MAX_ATEXIT 32
static void (*s_atexit[MAX_ATEXIT])(void);
static int s_atexit_count;

int atexit(void (*function)(void))
{
    if (s_atexit_count >= MAX_ATEXIT)
        return -1;
    s_atexit[s_atexit_count++] = function;
    return 0;
}

void exit(int status)
{
    /* Reverse registration order, as the standard requires. */
    for (int i = s_atexit_count - 1; i >= 0; --i)
        s_atexit[i]();
    __stdio_flush_all();
    _exit(status);
}

void abort(void)
{
    __stdio_flush_all();
    _exit(134); /* 128 + SIGABRT, the shell convention */
}

/* --- conversion -------------------------------------------------------- */

long strtol(const char* s, char** end, int base)
{
    const char* cursor = s;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')
        ++cursor;

    int negative = 0;
    if (*cursor == '-') {
        negative = 1;
        ++cursor;
    } else if (*cursor == '+') {
        ++cursor;
    }

    if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
        cursor += 2;
        base = 16;
    } else if (base == 0 && cursor[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    long value = 0;
    for (;; ++cursor) {
        int digit;
        if (*cursor >= '0' && *cursor <= '9')
            digit = *cursor - '0';
        else if (*cursor >= 'a' && *cursor <= 'z')
            digit = *cursor - 'a' + 10;
        else if (*cursor >= 'A' && *cursor <= 'Z')
            digit = *cursor - 'A' + 10;
        else
            break;

        if (digit >= base)
            break;
        value = value * base + digit;
    }

    if (end)
        *end = (char*)cursor;
    return negative ? -value : value;
}

unsigned long strtoul(const char* s, char** end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

int atoi(const char* s)
{
    return (int)strtol(s, 0, 10);
}
long atol(const char* s)
{
    return strtol(s, 0, 10);
}
int abs(int value)
{
    return value < 0 ? -value : value;
}

/* --- environment ------------------------------------------------------- */

char* getenv(const char* name)
{
    if (!environ)
        return 0;
    size_t const length = strlen(name);
    for (char** entry = environ; *entry; ++entry) {
        if (strncmp(*entry, name, length) == 0 && (*entry)[length] == '=')
            return *entry + length + 1;
    }
    return 0;
}

int setenv(const char* name, const char* value, int overwrite)
{
    /* environ points at the stack the kernel built, which cannot be grown in
     * place. Rebuilding it needs an allocation and a copy; nothing here needs
     * it yet, so it is honestly unimplemented rather than quietly wrong. */
    (void)name;
    (void)value;
    (void)overwrite;
    errno = ENOSYS;
    return -1;
}

/* --- sorting and searching ----------------------------------------------- */

static void swap_bytes(char* a, char* b, size_t size)
{
    while (size--) {
        char const t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*))
{
    /*
     * Shell sort with Ciura's gap sequence. Not the fastest option, but it
     * needs no recursion and no scratch allocation -- both of which matter in
     * a libc that may be called when memory is short -- and it has no
     * quadratic case on the adversarial inputs a naive quicksort chokes on.
     */
    static const size_t GAPS[] = { 701, 301, 132, 57, 23, 10, 4, 1 };

    if (count < 2 || size == 0)
        return;

    char* const array = base;

    for (size_t g = 0; g < sizeof(GAPS) / sizeof(GAPS[0]); ++g) {
        size_t const gap = GAPS[g];
        if (gap >= count)
            continue;

        for (size_t i = gap; i < count; ++i) {
            for (size_t j = i; j >= gap; j -= gap) {
                char* const left = array + (j - gap) * size;
                char* const right = array + j * size;
                if (compare(left, right) <= 0)
                    break;
                swap_bytes(left, right, size);
            }
        }
    }
}

void* bsearch(const void* key, const void* base, size_t count, size_t size,
    int (*compare)(const void*, const void*))
{
    const char* const array = base;
    size_t low = 0;
    size_t high = count;

    while (low < high) {
        size_t const middle = low + (high - low) / 2;
        const char* const candidate = array + middle * size;
        int const order = compare(key, candidate);
        if (order == 0)
            return (void*)candidate;
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }

    return 0;
}

int system(const char* command)
{
    /* Zero means "there is no command processor", which is exactly true and is
     * what a program is supposed to check before relying on one. */
    if (command == 0)
        return 0;
    errno = ENOSYS;
    return -1;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}
