#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- check the libm against the host's glibc, in ULPs.
#
# Numerical accuracy is not something to take on faith, and it is not something
# the boot-time self tests can check: the kernel is built without SSE and has no
# reference implementation to compare against. The host has both.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CC="${CC:-clang}"

# Build our libm for the host, using our headers rather than the system's, then
# rename every symbol so it can sit alongside glibc's in one binary.
"$CC" -std=gnu99 -O1 -nostdlibinc \
    -I"$ROOT/user/libc/include" -I"$ROOT/include" \
    -c "$ROOT/user/libc/src/math.c" -o "$WORK/ours.o"

OBJCOPY="${OBJCOPY:-llvm-objcopy}"
"$OBJCOPY" --prefix-symbols=shitos_ "$WORK/ours.o" "$WORK/ours-prefixed.o"

"$CC" -std=gnu99 -O1 "$ROOT/user/libc/test/mathcheck.c" "$WORK/ours-prefixed.o" -lm \
    -o "$WORK/mathcheck"

exec "$WORK/mathcheck"
