/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- allocation, conversion, exit.
 *
 * malloc keeps two structures over one sbrk arena. Every block, free or not,
 * sits on a doubly linked chain in address order, which is what makes
 * coalescing an O(1) look at the two neighbours. Free blocks are additionally
 * threaded onto one of BIN_COUNT size-class lists, so a request scans only
 * blocks that could satisfy it rather than the whole heap.
 *
 * The first version walked the entire chain on every free and every malloc.
 * That is fine until something allocates in earnest: Lua building 20000 small
 * tables and collecting them took 41 seconds, essentially all of it in that
 * walk. The links a free block needs live in its own payload, so the header
 * did not have to grow to fix it.
 */

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ALIGNMENT 16
#define MIN_GROWTH (128 * 1024)

/* 16 bytes up to 512 KiB in powers of two, and one bin for everything above. */
#define BIN_COUNT 16
#define SMALLEST_BIN_SHIFT 4

/*
 * Every block carries a magic word. The header is 8 + 8 + 8 + 4 bytes and
 * rounds up to 32 either way, so the check is free -- and it turns "the
 * allocator returned a wild pointer, and the program died somewhere else with
 * no explanation" into a message naming the block. That is not hypothetical:
 * it is how the corruption behind dash's crash was found.
 */
#define BLOCK_MAGIC 0x5AFEA110u

typedef struct Block {
    size_t size; /* payload bytes, not counting this header */
    struct Block* next; /* address order */
    struct Block* prev; /* address order */
    unsigned magic;
    int is_free;
} Block;

/*
 * A free block's payload is by definition unused, so the size-class links live
 * there. ALIGNMENT is 16, and every payload is rounded up to it, so there is
 * always room for these two pointers.
 */
typedef struct FreeLinks {
    struct Block* next;
    struct Block* prev;
} FreeLinks;

static Block* s_heap;
static Block* s_heap_tail;
static Block* s_bins[BIN_COUNT];

/* Reports a corrupt heap and stops, because carrying on means writing through
 * whatever the corrupt metadata pointed at. Writes directly rather than
 * through stdio: the corruption may be stdio's own buffer. */
static void heap_corrupt(const char* what, const void* block)
{
    static const char prefix[] = "libc: heap corruption: ";
    write(2, prefix, sizeof(prefix) - 1);
    write(2, what, strlen(what));

    char address[24];
    size_t position = sizeof(address);
    unsigned long value = (unsigned long)block;
    address[--position] = '\n';
    do {
        address[--position] = "0123456789abcdef"[value & 0xf];
        value >>= 4;
    } while (value != 0 && position > 3);
    address[--position] = 'x';
    address[--position] = '0';
    address[--position] = ' ';
    write(2, address + position, sizeof(address) - position);

    _exit(127);
}

static void check_block(const Block* block, const char* what)
{
    if (block->magic != BLOCK_MAGIC)
        heap_corrupt(what, block);
}

static size_t align_up(size_t value)
{
    return (value + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1);
}

static size_t header_size(void)
{
    return align_up(sizeof(Block));
}

static void* payload_of(Block* block)
{
    return (char*)block + header_size();
}

static FreeLinks* links_of(Block* block)
{
    return (FreeLinks*)payload_of(block);
}

/* Bin k holds free payloads of 2^(k+4) .. 2^(k+5)-1 bytes; the last bin holds
 * everything larger, which is rare enough that a short scan is fine. */
static unsigned bin_index(size_t payload)
{
    unsigned index = 0;
    size_t bound = (size_t)1 << SMALLEST_BIN_SHIFT;

    while (index + 1 < BIN_COUNT && payload >= bound * 2) {
        bound *= 2;
        ++index;
    }
    return index;
}

static void bin_insert(Block* block)
{
    unsigned const index = bin_index(block->size);
    FreeLinks* const links = links_of(block);

    links->prev = 0;
    links->next = s_bins[index];
    if (s_bins[index])
        links_of(s_bins[index])->prev = block;
    s_bins[index] = block;
}

static void bin_remove(Block* block)
{
    unsigned const index = bin_index(block->size);
    FreeLinks* const links = links_of(block);

    if (links->prev)
        links_of(links->prev)->next = links->next;
    else
        s_bins[index] = links->next;

    if (links->next)
        links_of(links->next)->prev = links->prev;
}

/* Two blocks can only merge if they are physically adjacent. Separate sbrk
 * calls usually are, but nothing promises it, and merging across a gap would
 * hand out memory that is not ours. */
static int adjacent(Block* first, Block* second)
{
    return (char*)first + header_size() + first->size == (char*)second;
}

static Block* extend_heap(size_t payload)
{
    size_t total = header_size() + payload;
    if (total < MIN_GROWTH)
        total = MIN_GROWTH;
    total = align_up(total);

    void* memory = sbrk((long)total);
    if (memory == (void*)-1)
        return 0;

    Block* block = memory;
    block->size = total - header_size();
    block->next = 0;
    block->prev = s_heap_tail;
    block->magic = BLOCK_MAGIC;
    block->is_free = 1;

    if (s_heap_tail)
        s_heap_tail->next = block;
    else
        s_heap = block;
    s_heap_tail = block;

    /* If the kernel handed back the page right after our last block, the two
     * are one block as far as the allocator is concerned. */
    if (block->prev && block->prev->is_free && adjacent(block->prev, block)) {
        Block* const previous = block->prev;
        bin_remove(previous);
        previous->size += header_size() + block->size;
        previous->next = block->next;
        s_heap_tail = previous;
        return previous;
    }

    return block;
}

/* Absorbs the following block if it is free and physically adjacent. `block`
 * must be off the free lists; the block it swallows is taken off them here. */
static void merge_with_next(Block* block)
{
    Block* const after = block->next;
    if (!after || !after->is_free || !adjacent(block, after))
        return;

    bin_remove(after);
    block->size += header_size() + after->size;
    block->next = after->next;
    if (after->next)
        after->next->prev = block;
    else
        s_heap_tail = block;
}

/* Splits `block` so it holds exactly `wanted` payload bytes, returning the
 * remainder to its size class. `block` must already be off the free lists. */
static void split(Block* block, size_t wanted)
{
    size_t const header = header_size();
    /* Only split when the remainder can hold a header plus the smallest
     * payload we are willing to hand out. */
    if (block->size < wanted + header + ALIGNMENT)
        return;

    Block* rest = (Block*)((char*)block + header + wanted);
    rest->size = block->size - wanted - header;
    rest->magic = BLOCK_MAGIC;
    rest->is_free = 1;
    rest->next = block->next;
    rest->prev = block;

    if (block->next)
        block->next->prev = rest;
    else
        s_heap_tail = rest;

    block->size = wanted;
    block->next = rest;

    /* The remainder may sit right in front of another free block -- realloc
     * shrinking an allocation is the usual way that happens. Two adjacent free
     * blocks must never both be on the lists, or the heap fragments for good.
     */
    merge_with_next(rest);
    bin_insert(rest);
}

/* First fit within the exact size class, then the head of the first larger
 * class -- anything in a larger class is guaranteed to fit. */
static Block* take_free_block(size_t wanted)
{
    unsigned const start = bin_index(wanted);

    for (Block* block = s_bins[start]; block; block = links_of(block)->next) {
        check_block(block, "free list entry");
        if (block->size >= wanted) {
            bin_remove(block);
            return block;
        }
    }

    for (unsigned index = start + 1; index < BIN_COUNT; ++index) {
        for (Block* block = s_bins[index]; block; block = links_of(block)->next) {
            check_block(block, "free list entry");
            if (block->size >= wanted) {
                bin_remove(block);
                return block;
            }
        }
    }

    return 0;
}

void* malloc(size_t size)
{
    if (size == 0)
        return 0;

    size_t const wanted = align_up(size);
    if (wanted < size) { /* the rounding wrapped */
        errno = ENOMEM;
        return 0;
    }

    /* Both paths hand back a block that is already off the size lists, so the
     * only thing left to do is trim it. */
    Block* block = take_free_block(wanted);
    if (!block) {
        block = extend_heap(wanted);
        if (!block) {
            errno = ENOMEM;
            return 0;
        }
    }

    split(block, wanted);
    block->is_free = 0;
    return payload_of(block);
}

void free(void* pointer)
{
    if (!pointer)
        return;

    Block* block = (Block*)((char*)pointer - header_size());
    check_block(block, "free");
    if (block->is_free)
        heap_corrupt("double free", block);
    block->is_free = 1;

    /* Merge with the block after, then the block before. Two O(1) checks,
     * where the first version walked every block in the heap. */
    merge_with_next(block);

    Block* const before = block->prev;
    if (before && before->is_free && adjacent(before, block)) {
        bin_remove(before);
        before->size += header_size() + block->size;
        before->next = block->next;
        if (block->next)
            block->next->prev = before;
        else
            s_heap_tail = before;
        block = before;
    }

    bin_insert(block);
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

    Block* block = (Block*)((char*)pointer - header_size());
    check_block(block, "realloc");
    size_t const wanted = align_up(size);

    if (block->size >= wanted) {
        split(block, wanted);
        return pointer;
    }

    /* Growing into a free neighbour avoids the copy entirely, which is most of
     * what a growing Lua table or string buffer does. */
    Block* const after = block->next;
    if (after && after->is_free && adjacent(block, after)
        && block->size + header_size() + after->size >= wanted) {
        merge_with_next(block);
        split(block, wanted);
        return pointer;
    }

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

/* --- the environment ------------------------------------------------------
 *
 * environ starts out pointing at the array the kernel built on the stack,
 * which cannot be grown or freed. The first modification copies the whole
 * thing to the heap -- strings included -- so that from then on every entry is
 * ours and can be replaced or removed without wondering who owns what. One
 * copy at the first setenv is a small price for not having to track it.
 */

static char** s_environment; /* our copy, once we have taken over */
static size_t s_environment_count;
static size_t s_environment_capacity;

static int take_over_environment(void)
{
    if (s_environment)
        return 0;

    size_t count = 0;
    if (environ) {
        while (environ[count])
            ++count;
    }

    size_t const capacity = count + 8;
    char** const copy = calloc(capacity + 1, sizeof(char*));
    if (!copy) {
        errno = ENOMEM;
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        copy[i] = strdup(environ[i]);
        if (!copy[i]) {
            for (size_t j = 0; j < i; ++j)
                free(copy[j]);
            free(copy);
            errno = ENOMEM;
            return -1;
        }
    }

    s_environment = copy;
    s_environment_count = count;
    s_environment_capacity = capacity;
    environ = copy;
    return 0;
}

/* Finds "name=" and returns its index, or -1. */
static long find_environment_entry(const char* name, size_t name_length)
{
    if (!environ)
        return -1;
    for (long i = 0; environ[i]; ++i) {
        if (strncmp(environ[i], name, name_length) == 0 && environ[i][name_length] == '=')
            return i;
    }
    return -1;
}

int setenv(const char* name, const char* value, int overwrite)
{
    if (!name || !*name || strchr(name, '=') || !value) {
        errno = EINVAL;
        return -1;
    }
    if (take_over_environment() < 0)
        return -1;

    size_t const name_length = strlen(name);
    long const existing = find_environment_entry(name, name_length);
    if (existing >= 0 && !overwrite)
        return 0;

    size_t const size = name_length + 1 + strlen(value) + 1;
    char* const entry = malloc(size);
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(entry, name, name_length);
    entry[name_length] = '=';
    memcpy(entry + name_length + 1, value, strlen(value) + 1);

    if (existing >= 0) {
        free(s_environment[existing]);
        s_environment[existing] = entry;
        return 0;
    }

    if (s_environment_count + 1 >= s_environment_capacity) {
        size_t const capacity = s_environment_capacity * 2;
        char** const grown = realloc(s_environment, (capacity + 1) * sizeof(char*));
        if (!grown) {
            free(entry);
            errno = ENOMEM;
            return -1;
        }
        s_environment = grown;
        s_environment_capacity = capacity;
        environ = grown;
    }

    s_environment[s_environment_count++] = entry;
    s_environment[s_environment_count] = NULL;
    return 0;
}

int unsetenv(const char* name)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (take_over_environment() < 0)
        return -1;

    size_t const name_length = strlen(name);
    for (;;) {
        long const existing = find_environment_entry(name, name_length);
        if (existing < 0)
            return 0;
        free(s_environment[existing]);
        /* Shift the tail down, terminator included. */
        for (size_t i = (size_t)existing; i < s_environment_count; ++i)
            s_environment[i] = s_environment[i + 1];
        --s_environment_count;
    }
}

int putenv(char* assignment)
{
    /*
     * POSIX says the caller's string *becomes* the environment entry, so a
     * later write through it is visible. This copies instead: with every entry
     * owned here, replacing one is a free and an assignment rather than a
     * question about who allocated what. Callers that rely on the aliasing are
     * rare and are relying on a footgun.
     */
    if (!assignment) {
        errno = EINVAL;
        return -1;
    }

    char* const equals = strchr(assignment, '=');
    if (!equals) {
        /* No '=' means remove it, which is a GNU extension everything uses. */
        return unsetenv(assignment);
    }

    size_t const name_length = (size_t)(equals - assignment);
    char* const name = strndup(assignment, name_length);
    if (!name) {
        errno = ENOMEM;
        return -1;
    }
    int const result = setenv(name, equals + 1, 1);
    free(name);
    return result;
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

/*
 * long long is long on x86-64, so these are the same conversion under a
 * different name. Spelling that out beats a second parser that could drift.
 */
long long strtoll(const char* s, char** end, int base)
{
    return strtol(s, end, base);
}

unsigned long long strtoull(const char* s, char** end, int base)
{
    return strtoul(s, end, base);
}

intmax_t strtoimax(const char* s, char** end, int base)
{
    return strtol(s, end, base);
}

uintmax_t strtoumax(const char* s, char** end, int base)
{
    return strtoul(s, end, base);
}

intmax_t imaxabs(intmax_t value)
{
    return value < 0 ? -value : value;
}

imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator)
{
    imaxdiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

/* --- division with both halves at once ----------------------------------- */

div_t div(int numerator, int denominator)
{
    div_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

ldiv_t ldiv(long numerator, long denominator)
{
    ldiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

lldiv_t lldiv(long long numerator, long long denominator)
{
    lldiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

long long llabs(long long value)
{
    return value < 0 ? -value : value;
}

/* --- temporary files ------------------------------------------------------
 *
 * Both replace the six X's at the end of the template in place, and both keep
 * trying until they find a name nothing else has. The counter is seeded from
 * the pid so two processes racing do not walk the same sequence.
 */

static int fill_template(char* template_path, unsigned attempt)
{
    size_t const length = strlen(template_path);
    if (length < 6 || strcmp(template_path + length - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }

    static const char ALPHABET[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned value = attempt * 2654435761u + (unsigned)getpid() * 40503u;
    for (int i = 0; i < 6; ++i) {
        template_path[length - 6 + i] = ALPHABET[value % (sizeof(ALPHABET) - 1)];
        value /= (sizeof(ALPHABET) - 1);
        value = value * 31 + 7;
    }
    return 0;
}

int mkstemp(char* template_path)
{
    if (!template_path) {
        errno = EINVAL;
        return -1;
    }

    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        if (fill_template(template_path, attempt) < 0)
            return -1;
        /* O_EXCL is what makes this safe: the open both creates the file and
         * proves nobody else got there first. */
        int const fd = open(template_path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }

    errno = EEXIST;
    return -1;
}

char* mkdtemp(char* template_path)
{
    if (!template_path) {
        errno = EINVAL;
        return NULL;
    }

    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        if (fill_template(template_path, attempt) < 0)
            return NULL;
        if (mkdir(template_path, 0700) == 0)
            return template_path;
        if (errno != EEXIST)
            return NULL;
    }

    errno = EEXIST;
    return NULL;
}
