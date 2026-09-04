#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- boot the ISO headless and capture what is on the screen.
#
# usage: screenshot.sh [--after SECONDS] [--keys "a b c"] <output.png> [iso]

set -eu

DELAY=6
KEYS=""
OUTPUT=""
ISO=""

while [ $# -gt 0 ]; do
    case "$1" in
        --after) DELAY="$2"; shift ;;
        --keys) KEYS="$2"; shift ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) if [ -z "$OUTPUT" ]; then OUTPUT="$1"; else ISO="$1"; fi ;;
    esac
    shift
done

[ -n "$OUTPUT" ] || { echo "usage: screenshot.sh <output.png> [iso]" >&2; exit 2; }
[ -n "$ISO" ] || ISO="build/shit-os-2.iso"

exec python3 "$(dirname "$0")/screenshot.py" "$ISO" "$OUTPUT" "$DELAY" "$KEYS"
