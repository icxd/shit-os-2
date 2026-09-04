#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- build sbase against our libc.
#
# sbase is suckless's coreutils: ninety-odd small programs, each one a
# different corner of POSIX. Lua exercised the maths and the allocator; dash
# exercised process groups and signals; this exercises everything else --
# directories, stat, permissions, terminals, text processing, and the regex
# engine in user/libc/src/regex.c, which exists because sbase's util.h wants
# <regex.h> and every one of its programs includes it.
#
# Not vendored, like the other ports. suckless serves no release tarballs and
# no snapshots, so this clones at a pinned commit and then verifies the tree it
# got: a git commit id already commits to its contents, and hashing a
# deterministic archive of the checkout on top of that gives the same guarantee
# the tarball ports get from a published SHA-256.
#
# Not patched either. Four programs are skipped for reasons written down below,
# and every one of those reasons is a missing facility rather than a
# disagreement with sbase.
#
# usage: build.sh <output directory>

set -eu

COMMIT="c546c3a5724c81cee9a11d816a38ccdf17472129"
REPOSITORY="https://git.suckless.org/sbase"
# sha256 of `git archive --format=tar $COMMIT` -- see the check below.
TREE_SHA256="4a03522d2573cb86006fb44b8b0e407cb5de32b074e3b4eb5efc98f6faf1933b"

OUTPUT_DIR="${1:?usage: build.sh <output directory>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

CLANG="${SHITOS_CLANG:-clang}"
CACHE="${SHITOS_PORT_CACHE:-$ROOT/.port-cache}"
mkdir -p "$CACHE" "$OUTPUT_DIR"

CHECKOUT="$CACHE/sbase-$COMMIT"

# --- fetch ---------------------------------------------------------------

if [ ! -d "$CHECKOUT/.git" ]; then
    echo "sbase: cloning $REPOSITORY at $COMMIT"
    rm -rf "$CHECKOUT.partial"
    git clone --quiet "$REPOSITORY" "$CHECKOUT.partial"
    ( cd "$CHECKOUT.partial" && git checkout --quiet "$COMMIT" )
    mv "$CHECKOUT.partial" "$CHECKOUT"
fi

ACTUAL_COMMIT="$(cd "$CHECKOUT" && git rev-parse HEAD)"
if [ "$ACTUAL_COMMIT" != "$COMMIT" ]; then
    echo "sbase: checkout is at $ACTUAL_COMMIT, expected $COMMIT" >&2
    exit 1
fi

# The commit id already commits to the tree, but hashing the tree itself is the
# same promise the tarball ports make and costs nothing to check.
ACTUAL_TREE="$(cd "$CHECKOUT" && git archive --format=tar "$COMMIT" | sha256sum | cut -d' ' -f1)"
if [ "$ACTUAL_TREE" != "$TREE_SHA256" ]; then
    echo "sbase: tree hash mismatch" >&2
    echo "  expected $TREE_SHA256" >&2
    echo "  got      $ACTUAL_TREE" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
( cd "$CHECKOUT" && git archive --format=tar "$COMMIT" ) | tar x -C "$WORK"
SRC="$WORK"

# --- build ---------------------------------------------------------------

CFLAGS="--target=x86_64-elf -ffreestanding -nostdlibinc -std=c99 -O2 -w
        -fno-pic -fno-pie -mno-red-zone
        -D_DEFAULT_SOURCE
        -I$ROOT/user/libc/include -I$ROOT/include -I$SRC"

LDFLAGS="--target=x86_64-elf -nostdlib -static -fuse-ld=lld
         -Wl,--build-id=none -Wl,-z,noexecstack -Wl,-z,max-page-size=0x1000
         -Wl,--image-base=0x400000 -Wl,-e,_start"

# The support libraries every program links against. libutf is suckless's own
# UTF-8 handling, which is why none of these programs needs a locale.
SUPPORT=""
for source in "$SRC"/libutf/*.c "$SRC"/libutil/*.c; do
    name="$(basename "$(dirname "$source")")-$(basename "$source" .c)"
    # shellcheck disable=SC2086
    "$CLANG" $CFLAGS -c "$source" -o "$WORK/$name.o"
    SUPPORT="$SUPPORT $WORK/$name.o"
done

#
# Four programs are not built, and each absence is a real missing facility:
#
#   cron     needs a background daemon, a working /var and a clock to schedule
#            against; there is no init system to run one under.
#   getconf  generated from a header sbase's own configure step produces, which
#            enumerates limits we do not all have.
#   logger   wants syslog's facility and priority name tables, which describe a
#            daemon that does not exist here.
#   tftp     needs sockets. There is no network stack.
#
SKIP="cron getconf logger tftp"

BUILT=0
SKIPPED=0
for source in "$SRC"/*.c; do
    name="$(basename "$source" .c)"

    skip=0
    for excluded in $SKIP; do
        [ "$name" = "$excluded" ] && skip=1
    done
    if [ "$skip" = 1 ]; then
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    # shellcheck disable=SC2086
    if ! "$CLANG" $CFLAGS -c "$source" -o "$WORK/$name.main.o" 2>"$WORK/$name.log"; then
        echo "sbase: $name failed to compile:" >&2
        head -5 "$WORK/$name.log" >&2
        exit 1
    fi

    # shellcheck disable=SC2086
    "$CLANG" $LDFLAGS "$WORK/$name.main.o" $SUPPORT "$SHITOS_LIBC" -o "$OUTPUT_DIR/$name"
    BUILT=$((BUILT + 1))
done

echo "sbase: built $BUILT programs, skipped $SKIPPED ($SKIP)"
