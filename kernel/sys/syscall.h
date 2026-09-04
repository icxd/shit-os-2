// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- system call dispatch.

#pragma once

#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/lib/error.h>
#include <shitos/types.h>

namespace kernel::sys {

// Installs the STAR/LSTAR/SFMASK MSRs so that `syscall` from ring 3 lands in
// syscall_entry. Must run after the GDT, whose layout the STAR value assumes.
void syscall_initialize();

// Copying to and from userspace. Every syscall that takes a pointer goes
// through these; they validate the range against the calling process's page
// tables first, so a bad pointer is EFAULT rather than a kernel fault.
ErrorOr<void> copy_from_user(void* destination, u64 user_source, usize length);
ErrorOr<void> copy_to_user(u64 user_destination, void const* source, usize length);
ErrorOr<usize> copy_string_from_user(char* destination, u64 user_source, usize capacity);

u64 syscall_count();

// Applies the current process's next pending signal to `frame`: ignore it, run
// its handler, or terminate. Called on the way out of a syscall and out of a
// fault, which are the two points where a process is about to re-enter ring 3.
InterruptFrame* deliver_pending_signal(InterruptFrame* frame);

// True when the default action for this signal is to kill the process.
bool signal_terminates_by_default(int signal);

// Ends the calling process with an already-encoded wait status.
[[noreturn]] void do_exit(int wait_status);

// Installs handlers that turn a fault in ring 3 into a signal instead of a
// kernel panic. Must run after the IDT.
void faults_initialize();

} // namespace kernel::sys
