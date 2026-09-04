#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- pack the initial ramdisk.
#
# The initrd is an ordinary ustar archive mounted read-only at /. Using plain
# tar means the build needs no bespoke tooling and a broken image can be
# inspected with `tar -tvf` like anything else.
#
# usage: mkinitrd.sh <output.tar>
#   SHITOS_BUILD_DIR   build tree to take compiled userland from (optional)

set -eu

OUTPUT="${1:?usage: mkinitrd.sh <output.tar>}"
BUILD_DIR="${SHITOS_BUILD_DIR:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

mkdir -p "$STAGING/bin" "$STAGING/dev" "$STAGING/tmp" "$STAGING/etc"

cat > "$STAGING/etc/motd" <<'MOTD'
shit os 2

Everything here was written from scratch. Very little of it is a good idea.
Type `help` if you want to know what is actually implemented.
MOTD

echo "shit-os-2" > "$STAGING/etc/hostname"

# An overlay of files kept in the source tree: /etc content, scripts, anything
# that is not a compiled artifact. Copied first so a build product of the same
# name wins.
if [ -d "$ROOT/rootfs" ]; then
    ( cd "$ROOT/rootfs" && find . -mindepth 1 -print0 | cpio -0pdm --quiet "$STAGING" 2>/dev/null ) \
        || cp -r "$ROOT/rootfs/." "$STAGING/"
fi

# Compiled userland, if there is any yet.
if [ -n "$BUILD_DIR" ] && [ -d "$BUILD_DIR/user/bin" ]; then
    for binary in "$BUILD_DIR"/user/bin/*; do
        [ -f "$binary" ] && [ -x "$binary" ] || continue
        cp "$binary" "$STAGING/bin/$(basename "$binary")"
    done
fi

# Loadable modules, if there are any yet. Stage E fills this in.
if [ -n "$BUILD_DIR" ] && [ -d "$BUILD_DIR/modules" ]; then
    mkdir -p "$STAGING/lib/modules"
    find "$BUILD_DIR/modules" -name '*.ko' -exec cp {} "$STAGING/lib/modules/" \; 2>/dev/null || true
fi

# --format=ustar is the whole point: our reader implements ustar and nothing
# else, so a GNU-format archive with long-name extensions would not parse.
# Sorted order keeps the image byte-identical across rebuilds.
( cd "$STAGING" && find . -mindepth 1 | LC_ALL=C sort \
    | tar --format=ustar --no-recursion --owner=0 --group=0 --numeric-owner \
          --mtime=@0 -cf "$OUTPUT" -T - )

echo "initrd: $(wc -c < "$OUTPUT") bytes -> $OUTPUT"
