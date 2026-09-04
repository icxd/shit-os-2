#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- exercise the libc allocator on the host.
#
# An allocator is the one part of the libc where "it booted" proves nothing:
# corruption shows up much later and somewhere else, and timing it inside QEMU
# measures QEMU. So the real stdlib.c is built for the host against a fake
# sbrk, with every symbol renamed so it can sit next to the host's own.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CC="${CC:-clang}"
OBJCOPY="${OBJCOPY:-llvm-objcopy}"

"$CC" -std=gnu99 -O2 -nostdlibinc \
    -I"$ROOT/user/libc/include" -I"$ROOT/include" \
    -c "$ROOT/user/libc/src/stdlib.c" -o "$WORK/ours.o"

"$OBJCOPY" --prefix-symbols=shitos_ "$WORK/ours.o" "$WORK/ours-prefixed.o"

# The renaming catches undefined symbols too, so the arena and every libc call
# stdlib.c makes are satisfied here under their prefixed names.
cat > "$WORK/host-stubs.c" <<'STUBS'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char shitos_test_arena[256u << 20];
size_t shitos_test_arena_used;

static int s_errno;
int* shitos___errno_location(void) { return &s_errno; }

char** shitos_environ;
void shitos___stdio_flush_all(void) { }

void* shitos_sbrk(long increment)
{
    if (increment < 0)
        return (void*)-1;
    if (shitos_test_arena_used + (size_t)increment > sizeof(shitos_test_arena))
        return (void*)-1;
    void* const block = shitos_test_arena + shitos_test_arena_used;
    shitos_test_arena_used += (size_t)increment;
    return block;
}

void* shitos_memset(void* d, int c, size_t n) { return memset(d, c, n); }
void* shitos_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
size_t shitos_strlen(const char* s) { return strlen(s); }
int shitos_strcmp(const char* a, const char* b) { return strcmp(a, b); }
int shitos_strncmp(const char* a, const char* b, size_t n) { return strncmp(a, b, n); }
void shitos__exit(int status) { exit(status); }
long shitos___syscall_return(long value) { return value; }
STUBS

"$CC" -std=gnu99 -O2 -c "$WORK/host-stubs.c" -o "$WORK/host-stubs.o"
"$CC" -std=gnu99 -O2 "$ROOT/user/libc/test/malloccheck.c" \
    "$WORK/ours-prefixed.o" "$WORK/host-stubs.o" -o "$WORK/malloccheck"

exec "$WORK/malloccheck"
