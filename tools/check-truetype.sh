#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- render text with our own rasteriser, on the host.
#
# There is no reference implementation to diff against the way check-libm and
# check-regex have glibc: "correct" for a glyph is a judgement, not a number.
# So this checks the things that *are* facts -- that the tables parse, that
# cmap finds the glyph a character maps to, that metrics are sane, that
# coverage stays inside its bitmap and inside 0..255, that a space has no
# pixels and a letter does -- and then writes a PNG so a human can look at it
# once and see whether the shapes are letters.
#
# Running it on the host rather than in QEMU means a change to the rasteriser
# is checked in a second instead of a minute, which is the difference between
# checking every change and checking some of them.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-clang}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Any font will do; these are where the usual distributions put one. The
# rasteriser must not care which it gets.
FONT="${SHITOS_TEST_FONT:-}"
if [ -z "$FONT" ]; then
    for candidate in \
        /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
        /usr/share/fonts/dejavu/DejaVuSans.ttf \
        /usr/share/fonts/TTF/DejaVuSans.ttf \
        /usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf \
        /System/Library/Fonts/Helvetica.ttc
    do
        [ -f "$candidate" ] && FONT="$candidate" && break
    done
fi

if [ -z "$FONT" ]; then
    echo "check-truetype: no TrueType font found on this machine; skipping" >&2
    echo "  set SHITOS_TEST_FONT to one to run these checks" >&2
    exit 0
fi

echo "rasterising with $(basename "$FONT")"

# The sanitizers are most of the point -- a rasteriser is array indexing driven
# by file contents -- but their runtime is a separate package on some machines,
# so a build without them is better than no check at all. CI installs it.
SANITIZE="-fsanitize=address,undefined"
if ! "$CC" $SANITIZE -x c /dev/null -o /dev/null 2>/dev/null; then
    echo "check-truetype: no sanitizer runtime here; checking without it" >&2
    SANITIZE=""
fi

# shellcheck disable=SC2086
"$CC" -std=gnu17 -O2 -g -Wall -Wextra -Werror $SANITIZE \
    -I"$ROOT/user/libui" \
    "$ROOT/user/libui/truetype.c" "$ROOT/user/libui/test/truetypecheck.c" \
    -lm -o "$WORK/check"

"$WORK/check" "$FONT" "$WORK/sample.ppm"

if [ -n "${SHITOS_TRUETYPE_OUT:-}" ]; then
    python3 "$ROOT/tools/ppm2png.py" "$WORK/sample.ppm" "$SHITOS_TRUETYPE_OUT"
fi
