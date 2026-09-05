#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- fetch the fonts the desktop draws with.
#
# Not a build: there is nothing to compile. It is a port all the same, and for
# the same reason as the others -- a megabyte of somebody else's binary does
# not belong in this tree, and a checksum is a better claim about what you are
# running than a file that happens to be in git.
#
# Inter for the interface, because the desktop is styled after macOS and Inter
# is the closest freely licensed face to the one macOS uses: the same tall
# x-height, the same open apertures, the same slightly condensed feel at small
# sizes. Regular and semibold, because macOS emphasises with semibold and a
# full bold looks heavy-handed next to it.
#
# DejaVu Sans Mono for the terminal. Inter has no monospace, and a terminal
# needs one whose zero is distinguishable from its capital O.
#
# usage: build.sh <output directory>

set -eu

INTER_VERSION="4.0"
INTER_ARCHIVE="Inter-${INTER_VERSION}.zip"
INTER_URL="https://github.com/rsms/inter/releases/download/v${INTER_VERSION}/${INTER_ARCHIVE}"
INTER_SHA256="ff970a5d4561a04f102a7cb781adbd6ac4e9b6c460914c7a101f15acb7f7d1a4"

REGULAR_SHA256="64f8be6e55c37e32ef03da99714bf3aa58b8f2099bfe4f759a7578e3b8291123"
SEMIBOLD_SHA256="0dc98e8aa59585394880f25ab89e6d915ad5134522e961b046ca51fad3a18255"

DEJAVU_VERSION="2.37"
DEJAVU_ARCHIVE="dejavu-fonts-ttf-${DEJAVU_VERSION}.zip"
DEJAVU_URL="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/${DEJAVU_ARCHIVE}"
DEJAVU_SHA256="7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a"
MONO_SHA256="b4a6c3e4faab8773f4ff761d56451646409f29abedd68f05d38c2df667d3c582"

OUTPUT_DIR="${1:?usage: build.sh <output directory>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

CACHE="${SHITOS_PORT_CACHE:-$ROOT/.port-cache}"
mkdir -p "$CACHE" "$OUTPUT_DIR"

verify() {
    actual="$(sha256sum "$1" | cut -d' ' -f1)"
    if [ "$actual" != "$2" ]; then
        echo "fonts: $1 has the wrong hash" >&2
        echo "  expected $2" >&2
        echo "  got      $actual" >&2
        exit 1
    fi
}

fetch() {
    if [ ! -f "$CACHE/$1" ]; then
        echo "fonts: downloading $1"
        curl -sSfL --retry 3 -o "$CACHE/$1.partial" "$2"
        mv "$CACHE/$1.partial" "$CACHE/$1"
    fi
    verify "$CACHE/$1" "$3"
}

fetch "$INTER_ARCHIVE" "$INTER_URL" "$INTER_SHA256"
fetch "$DEJAVU_ARCHIVE" "$DEJAVU_URL" "$DEJAVU_SHA256"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Only the three files that are wanted, rather than unpacking two archives of
# fonts nothing selects.
unzip -q -o -j "$CACHE/$INTER_ARCHIVE" \
    "extras/ttf/Inter-Regular.ttf" "extras/ttf/Inter-SemiBold.ttf" -d "$WORK"
unzip -q -o -j "$CACHE/$DEJAVU_ARCHIVE" \
    "dejavu-fonts-ttf-$DEJAVU_VERSION/ttf/DejaVuSansMono.ttf" \
    "dejavu-fonts-ttf-$DEJAVU_VERSION/LICENSE" -d "$WORK"

# Checked individually as well as by archive: the archive hash says nobody
# swapped the download, these say nobody swapped a file inside a cache an
# earlier run wrote.
verify "$WORK/Inter-Regular.ttf" "$REGULAR_SHA256"
verify "$WORK/Inter-SemiBold.ttf" "$SEMIBOLD_SHA256"
verify "$WORK/DejaVuSansMono.ttf" "$MONO_SHA256"

cp "$WORK/Inter-Regular.ttf" "$OUTPUT_DIR/sans.ttf"
cp "$WORK/Inter-SemiBold.ttf" "$OUTPUT_DIR/bold.ttf"
cp "$WORK/DejaVuSansMono.ttf" "$OUTPUT_DIR/mono.ttf"

# The licences travel with the fonts, because both licences require it.
cp "$WORK/LICENSE" "$OUTPUT_DIR/LICENSE.dejavu"
unzip -q -o -j "$CACHE/$INTER_ARCHIVE" "LICENSE.txt" -d "$WORK" 2>/dev/null || true
[ -f "$WORK/LICENSE.txt" ] && cp "$WORK/LICENSE.txt" "$OUTPUT_DIR/LICENSE.inter"

echo "fonts: installed sans.ttf and bold.ttf (Inter), mono.ttf (DejaVu Sans Mono)"
