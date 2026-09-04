// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- the legacy 8259 pair.
//
// The PIC is remapped away from vectors 0-15 (where it collides with the CPU's
// own exceptions) and then left masked except for the lines a driver asks for.
// An APIC driver will replace this; the IRQ interface above it does not change.

#pragma once

#include <shitos/types.h>

namespace kernel::arch {

void pic_initialize();
void pic_mask(u8 irq);
void pic_unmask(u8 irq);
void pic_send_eoi(u8 irq);
void pic_mask_all();

// True when the interrupt was a spurious one from either chip, which must not
// be acknowledged as though it were real.
bool pic_is_spurious(u8 irq);

} // namespace kernel::arch
