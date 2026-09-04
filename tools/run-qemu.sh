#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- boot the ISO in QEMU.
#
# usage: run-qemu.sh [--headless] [--debug] [--expect-ok] [iso]
#
#   --headless   no window; serial goes to stdout (what CI and scripts use)
#   --debug      wait for gdb on :1234 before executing anything
#   --expect-ok  headless, bounded runtime, exit non-zero unless the boot
#                markers appear on the serial console

set -eu

HEADLESS=0
DEBUG=0
EXPECT_OK=0
ISO=""
MEMORY="${SHITOS_QEMU_MEMORY:-256M}"
CPUS="${SHITOS_QEMU_CPUS:-1}"
TIMEOUT="${SHITOS_QEMU_TIMEOUT:-30}"

while [ $# -gt 0 ]; do
    case "$1" in
        --headless) HEADLESS=1 ;;
        --debug) DEBUG=1 ;;
        --expect-ok) EXPECT_OK=1; HEADLESS=1 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) ISO="$1" ;;
    esac
    shift
done

if [ -z "$ISO" ]; then
    for candidate in build/shit-os-2.iso shit-os-2.iso; do
        [ -f "$candidate" ] && ISO="$candidate" && break
    done
fi
[ -n "$ISO" ] && [ -f "$ISO" ] || { echo "no ISO found; run: ninja -C build iso" >&2; exit 1; }

# -cpu max exposes SMEP, SMAP and 1 GiB pages, which the default qemu64 model
# does not. The kernel copes either way, but there is no reason to test the
# weaker configuration by default.
set -- \
    -cdrom "$ISO" \
    -cpu "${SHITOS_QEMU_CPU:-max}" \
    -m "$MEMORY" \
    -smp "$CPUS" \
    -machine q35 \
    -no-reboot \
    -no-shutdown \
    -d guest_errors \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04

[ "$DEBUG" = 1 ] && set -- "$@" -s -S

if [ "$EXPECT_OK" = 1 ]; then
    LOG="$(mktemp)"
    trap 'rm -f "$LOG"' EXIT
    timeout "$TIMEOUT" qemu-system-x86_64 "$@" -display none -serial "file:$LOG" >/dev/null 2>&1 || true
    cat "$LOG"
    # The shell's own banner is the marker, because reaching it proves the
    # whole chain: kernel, init, fork, execve, the TTY and stdout.
    if grep -q 'type .help. for what actually works' "$LOG"; then
        echo "run-qemu: reached a userland shell prompt" >&2
        exit 0
    fi
    echo "run-qemu: never reached a shell prompt within ${TIMEOUT}s" >&2
    exit 1
fi

if [ "$HEADLESS" = 1 ]; then
    exec qemu-system-x86_64 "$@" -display none -serial stdio
fi

exec qemu-system-x86_64 "$@" -serial stdio
