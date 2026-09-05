// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- /dev/fb0.

#pragma once

#include <kernel/lib/error.h>

namespace kernel::dev {

// Registers /dev/fb0 if there is a linear framebuffer to expose. Returns
// success and registers nothing when the mode is a text buffer, because that
// is a fact about the machine rather than a failure to start.
ErrorOr<void> framebuffer_device_initialize();

// Gives the screen back to the console, whoever had it. Called when the last
// descriptor onto /dev/fb0 closes, so a compositor that exits without
// releasing -- or crashes -- does not leave the console mute.
void framebuffer_device_release();

} // namespace kernel::dev
