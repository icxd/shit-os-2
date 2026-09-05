#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- lay out and paint the widget toolkit on the host.
#
# The painter, the widgets and the text layer touch nothing but memory, so they
# run here exactly as they do on the target -- in about a second, rather than
# the two minutes a QEMU round trip costs. That difference is the whole reason
# this exists: an interface nobody can iterate on stays plain.
#
# usage: check-ui.sh [output.png]

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-clang}"
OUTPUT="${1:-}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The same faces the image ships, fetched by the same port, so what is rendered
# here is what the target renders. Anything else would be checking a different
# interface from the one that gets shipped.
FONTS="${SHITOS_UI_FONTS:-$ROOT/.port-cache/uicheck-fonts}"
if [ ! -f "$FONTS/sans.ttf" ]; then
    if ! "$ROOT/ports/fonts/build.sh" "$FONTS" >/dev/null 2>&1; then
        echo "check-ui: cannot fetch the fonts; skipping" >&2
        exit 0
    fi
fi
export SHITOS_UI_FONTS="$FONTS"

SANITIZE="-fsanitize=address,undefined"
if ! "$CC" $SANITIZE -x c /dev/null -o /dev/null 2>/dev/null; then
    SANITIZE=""
fi

# window.c is left out on purpose: it is the only file that needs a window
# server, and everything worth looking at is in the other four.
# shellcheck disable=SC2086
"$CC" -std=gnu17 -O1 -g -Wall -Wextra -Werror $SANITIZE \
    -I"$ROOT/user/libui" -I"$ROOT/include" \
    "$ROOT/user/libui/truetype.c" \
    "$ROOT/user/libui/text.c" \
    "$ROOT/user/libui/paint.c" \
    "$ROOT/user/libui/widget.c" \
    "$ROOT/user/libui/controls.c" \
    "$ROOT/user/libui/terminal.c" \
    "$ROOT/user/libui/test/uicheck.c" \
    -lm -o "$WORK/check"

"$WORK/check" "$WORK/ui.ppm"

if [ -n "$OUTPUT" ]; then
    python3 "$ROOT/tools/ppm2png.py" "$WORK/ui.ppm" "$OUTPUT"
fi
