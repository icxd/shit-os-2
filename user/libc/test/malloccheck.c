/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- exercise the libc allocator on the host.
 *
 * The boot-time self tests cannot say much about an allocator: a wrong answer
 * looks like a working one until the heap is under pressure, and measuring how
 * long something takes inside QEMU tells you about QEMU. Here the real
 * stdlib.c is compiled against a fake sbrk, so the same code that ships is
 * checked for correctness and timed against a pattern that looks like Lua's.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* stdlib.c is compiled with our headers and its symbols prefixed, so declare
 * what we call rather than including two conflicting stdlib.h. */
void* shitos_malloc(size_t size);
void shitos_free(void* pointer);
void* shitos_calloc(size_t count, size_t size);
void* shitos_realloc(void* pointer, size_t size);

/* The arena the prefixed sbrk hands out. */
extern char shitos_test_arena[];
extern size_t shitos_test_arena_used;

static int s_checks;
static int s_failures;

static void check(int condition, char const* what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        printf("  FAIL %s\n", what);
    }
}

static double seconds_since(struct timespec start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
}

static void test_basics(void)
{
    check(shitos_malloc(0) == NULL, "malloc(0) returns NULL");
    shitos_free(NULL); /* must not crash */

    char* a = shitos_malloc(100);
    check(a != NULL, "a small allocation succeeds");
    check(((uintptr_t)a % 16) == 0, "allocations are 16-byte aligned");
    memset(a, 0xAB, 100);

    char* b = shitos_malloc(100);
    check(b != NULL, "a second allocation succeeds");
    check(b < a || b >= a + 100, "two live allocations do not overlap");

    /* Writing through b must not disturb a. */
    memset(b, 0xCD, 100);
    int intact = 1;
    for (int i = 0; i < 100; ++i)
        intact &= (unsigned char)a[i] == 0xAB;
    check(intact, "a neighbouring allocation does not corrupt the first");

    shitos_free(a);
    shitos_free(b);

    char* zeroed = shitos_calloc(64, 4);
    check(zeroed != NULL, "calloc succeeds");
    int all_zero = 1;
    for (int i = 0; i < 64 * 4; ++i)
        all_zero &= zeroed[i] == 0;
    check(all_zero, "calloc zeroes what it returns");
    shitos_free(zeroed);

    check(shitos_calloc((size_t)-1 / 2, 4) == NULL, "calloc refuses a wrapping product");
}

static void test_realloc(void)
{
    char* p = shitos_malloc(32);
    memset(p, 'x', 32);

    p = shitos_realloc(p, 4096);
    check(p != NULL, "realloc grows");
    int kept = 1;
    for (int i = 0; i < 32; ++i)
        kept &= p[i] == 'x';
    check(kept, "realloc keeps the old contents");

    p = shitos_realloc(p, 16);
    check(p != NULL, "realloc shrinks");
    check(p[0] == 'x', "realloc keeps contents when shrinking");

    check(shitos_realloc(p, 0) == NULL, "realloc to zero frees and returns NULL");
    check(shitos_realloc(NULL, 64) != NULL, "realloc of NULL is malloc");
}

/* Free everything and the heap should come back to one block: if coalescing
 * misses a case the chain grows without bound across a program's lifetime. */
static void test_coalescing(void)
{
    enum { COUNT = 512 };
    void* blocks[COUNT];

    size_t const before = shitos_test_arena_used;

    for (int round = 0; round < 8; ++round) {
        for (int i = 0; i < COUNT; ++i)
            blocks[i] = shitos_malloc(64 + (i % 7) * 48);
        for (int i = 0; i < COUNT; ++i)
            shitos_free(blocks[i]);
    }

    check(shitos_test_arena_used == before, "repeated fill-and-drain cycles do not grow the heap");

    /* And the whole span must be reusable as one allocation again. */
    void* whole = shitos_malloc(COUNT * 64);
    check(whole != NULL, "a large allocation fits in the coalesced space");
    shitos_free(whole);
}

/* Lua's pattern: many small objects, a churn of allocation and release, and a
 * bulk free at the end. This is what took 41 seconds inside QEMU. */
static void test_throughput(void)
{
    enum { LIVE = 4000, OPERATIONS = 200000 };
    void* live[LIVE];
    memset(live, 0, sizeof(live));

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    unsigned seed = 1;
    for (long i = 0; i < OPERATIONS; ++i) {
        seed = seed * 1103515245u + 12345u;
        int const slot = (int)((seed >> 8) % LIVE);
        if (live[slot]) {
            shitos_free(live[slot]);
            live[slot] = NULL;
        } else {
            live[slot] = shitos_malloc(16 + (seed >> 16) % 200);
        }
    }

    double const elapsed = seconds_since(start);
    for (int i = 0; i < LIVE; ++i)
        shitos_free(live[i]);

    printf("  %d malloc/free pairs in %.3f s (%.0f ns each)\n", OPERATIONS, elapsed,
        elapsed * 1e9 / OPERATIONS);

    /* Generous: the point is to catch a return to linear scanning, not to
     * benchmark the host. The first version needed 1.7 s here. */
    check(elapsed < 0.5, "allocation throughput is not linear in heap size");
}

int main(void)
{
    test_basics();
    test_realloc();
    test_coalescing();
    test_throughput();

    printf("%d checks, %d failed\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
