#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""shit os 2 -- drive QEMU's monitor to capture the framebuffer.

Boots the ISO headless, optionally types some keys, then asks QEMU for a
screendump. The PPM it produces is converted to PNG only if a converter is
available; otherwise the PPM is left in place, which every image viewer and
most browsers can read anyway.
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

iso, output, delay, keys = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
settle = float(sys.argv[5]) if len(sys.argv) > 5 else 1.5

with tempfile.TemporaryDirectory() as tmp:
    monitor = os.path.join(tmp, "monitor.sock")
    ppm = os.path.join(tmp, "shot.ppm")
    serial = os.path.join(tmp, "serial.log")

    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-cdrom", iso, "-m", "256M", "-smp", "1",
        "-machine", "q35", "-cpu", "max", "-no-reboot", "-display", "none",
        "-serial", "file:" + serial,
        "-monitor", "unix:%s,server,nowait" % monitor,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        # Wait for the monitor socket to exist before connecting.
        deadline = time.time() + 10
        while not os.path.exists(monitor) and time.time() < deadline:
            time.sleep(0.1)

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(monitor)
        time.sleep(delay)

        def command(text):
            sock.sendall((text + "\n").encode())
            time.sleep(0.35)

        for key in keys.split():
            # `wait:N` is a pause rather than a key. Capturing anything that
            # starts a program needs one: the keys after `wsys &` have to
            # arrive once the window server owns the screen, not while it is
            # still coming up, and 0.35s between keystrokes is not enough.
            if key.startswith("wait:"):
                time.sleep(float(key[len("wait:"):]))
                continue
            command("sendkey " + key)
        # Give whatever was typed time to finish before capturing.
        if keys:
            time.sleep(settle)

        command("screendump " + ppm)
        time.sleep(1.5)

        if not os.path.exists(ppm):
            print("screendump produced nothing", file=sys.stderr)
            sys.exit(1)

        if output.endswith(".png"):
            # ppm2png.py is ours and has no dependencies, so it is tried
            # first: a machine with neither ImageMagick nor netpbm installed
            # is the common case, and silently writing a .ppm instead of the
            # .png that was asked for is a surprise nobody needs.
            own = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ppm2png.py")
            for converter in ([sys.executable, own, ppm, output],
                              ["magick", ppm, output], ["convert", ppm, output],
                              ["pnmtopng", ppm]):
                try:
                    if converter[0] == "pnmtopng":
                        with open(output, "wb") as handle:
                            subprocess.run(converter, stdout=handle, check=True)
                    else:
                        subprocess.run(converter, check=True,
                                       stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
                    break
                except (FileNotFoundError, subprocess.CalledProcessError):
                    continue
            else:
                output = output[:-4] + ".ppm"
                subprocess.run(["cp", ppm, output], check=True)
        else:
            subprocess.run(["cp", ppm, output], check=True)

        print("wrote %s" % output)
        if os.path.exists(serial):
            sys.stderr.write(open(serial, errors="replace").read())
    finally:
        qemu.kill()
        qemu.wait()
