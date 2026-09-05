#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- fetch the fonts the desktop draws with.
#
# Not a build: there is nothing to compile. It is a port all the same, and for
# the same reason as the others -- three quarters of a megabyte of somebody
# else's binary does not belong in this tree, and a checksum is a better claim
# about what you are running than a file that happens to be in git.
#
# Two faces, and only two. DejaVu Sans for interface text and DejaVu Sans Mono
# for the terminal; the release carries fourteen more, and shipping fonts
# nothing selects would just make the image bigger.
#
# usage: build.sh <output directory>

set -eu

VERSION="2.37"
ARCHIVE="dejavu-fonts-ttf-${VERSION}.zip"
URL="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/${ARCHIVE}"
SHA256="7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a"

# The two files, checked individually as well as the archive. The archive hash
# says nobody swapped the download; these say nobody swapped a file inside it
# for a build that unpacked from a cache written by an earlier run.
SANS_SHA256="7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954"
MONO_SHA256="b4a6c3e4faab8773f4ff761d56451646409f29abedd68f05d38c2df667d3c582"

OUTPUT_DIR="${1:?usage: build.sh <output directory>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

CACHE="${SHITOS_PORT_CACHE:-$ROOT/.port-cache}"
mkdir -p "$CACHE" "$OUTPUT_DIR"

verify() {
    actual="$(sha256sum "$1" | cut -d' ' -f1)"
    if [ "$actual" != "$2" ]; then
        echo "dejavu: $1 has the wrong hash" >&2
        echo "  expected $2" >&2
        echo "  got      $actual" >&2
        exit 1
    fi
}

if [ ! -f "$CACHE/$ARCHIVE" ]; then
    echo "dejavu: downloading $ARCHIVE"
    curl -sSfL --retry 3 -o "$CACHE/$ARCHIVE.partial" "$URL"
    mv "$CACHE/$ARCHIVE.partial" "$CACHE/$ARCHIVE"
fi
verify "$CACHE/$ARCHIVE" "$SHA256"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

unzip -q -o "$CACHE/$ARCHIVE" -d "$WORK"
SRC="$WORK/dejavu-fonts-ttf-$VERSION"

verify "$SRC/ttf/DejaVuSans.ttf" "$SANS_SHA256"
verify "$SRC/ttf/DejaVuSansMono.ttf" "$MONO_SHA256"

mkdir -p "$OUTPUT_DIR"
cp "$SRC/ttf/DejaVuSans.ttf" "$OUTPUT_DIR/sans.ttf"
cp "$SRC/ttf/DejaVuSansMono.ttf" "$OUTPUT_DIR/mono.ttf"

# The licence travels with the fonts, because the licence requires it to.
cp "$SRC/LICENSE" "$OUTPUT_DIR/LICENSE"

echo "dejavu: installed sans.ttf and mono.ttf"
