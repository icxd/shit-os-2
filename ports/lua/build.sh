#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- build Lua against our libc.
#
# Lua is not vendored into this repository. It is downloaded, checksummed and
# built here, which keeps 30,000 lines of somebody else's MIT-licensed source
# out of a GPLv3 tree and makes it obvious what is ours and what is not.
#
# Nothing is patched. Lua's generic ISO C build -- no LUA_USE_POSIX, no
# readline, no dlopen -- compiles unmodified against our headers, which is the
# entire point of the exercise: if it needed patches, the gap would be in our
# libc rather than in Lua.
#
# usage: build.sh <output directory>

set -eu

VERSION="5.4.7"
ARCHIVE="lua-${VERSION}.tar.gz"
URL="https://www.lua.org/ftp/${ARCHIVE}"
SHA256="9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30"

OUTPUT_DIR="${1:?usage: build.sh <output directory>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

CLANG="${SHITOS_CLANG:-clang}"
CACHE="${SHITOS_PORT_CACHE:-$ROOT/.port-cache}"
mkdir -p "$CACHE" "$OUTPUT_DIR"

# --- fetch ---------------------------------------------------------------

if [ ! -f "$CACHE/$ARCHIVE" ]; then
    echo "lua: downloading $URL"
    curl -sSfL "$URL" -o "$CACHE/$ARCHIVE.partial"
    mv "$CACHE/$ARCHIVE.partial" "$CACHE/$ARCHIVE"
fi

ACTUAL="$(sha256sum "$CACHE/$ARCHIVE" | cut -d' ' -f1)"
if [ "$ACTUAL" != "$SHA256" ]; then
    echo "lua: checksum mismatch for $ARCHIVE" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $ACTUAL" >&2
    rm -f "$CACHE/$ARCHIVE"
    exit 1
fi

# --- build ---------------------------------------------------------------

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

tar xzf "$CACHE/$ARCHIVE" -C "$WORK"
SRC="$WORK/lua-${VERSION}/src"

# Freestanding, our headers only, and no LUA_USE_* so Lua takes its generic
# ISO C path rather than reaching for POSIX facilities we do not have.
CFLAGS="--target=x86_64-elf -ffreestanding -nostdlibinc -std=gnu99 -O2 -w
        -fno-pic -fno-pie -mno-red-zone
        -I$ROOT/user/libc/include -I$ROOT/include"

for source in "$SRC"/*.c; do
    name="$(basename "$source" .c)"
    # luac is the bytecode compiler and needs a host filesystem to be useful;
    # only the interpreter is built.
    [ "$name" = "luac" ] && continue
    # shellcheck disable=SC2086
    "$CLANG" $CFLAGS -c "$source" -o "$WORK/$name.o"
done

"$CLANG" --target=x86_64-elf -nostdlib -static -fuse-ld=lld \
    -Wl,--build-id=none -Wl,-z,noexecstack -Wl,-z,max-page-size=0x1000 \
    -Wl,--image-base=0x400000 -Wl,-e,_start \
    "$WORK"/*.o "$SHITOS_LIBC" -o "$OUTPUT_DIR/lua"

echo "lua: built $(wc -c < "$OUTPUT_DIR/lua") bytes -> $OUTPUT_DIR/lua"
