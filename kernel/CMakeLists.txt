# SPDX-License-Identifier: GPL-3.0-or-later

set(KERNEL_SOURCES
    arch/x86_64/boot.S
    arch/x86_64/cpu.cpp
    arch/x86_64/fpu.cpp
    arch/x86_64/gdt.cpp
    arch/x86_64/idt.cpp
    arch/x86_64/isr.S
    arch/x86_64/percpu.cpp
    arch/x86_64/pic.cpp
    arch/x86_64/pit.cpp
    arch/x86_64/serial.cpp
    arch/x86_64/syscall_entry.S
    boot/boot_info.cpp
    dev/console.cpp
    dev/fbdev.cpp
    dev/framebuffer.cpp
    dev/tty.cpp
    fs/boot_mounts.cpp
    fs/devfs.cpp
    fs/pipe.cpp
    fs/tmpfs.cpp
    fs/ustar.cpp
    fs/vfs.cpp
    lib/cxx_support.cpp
    lib/error.cpp
    lib/format.cpp
    lib/string.cpp
    mm/address_space.cpp
    mm/heap.cpp
    mm/physical.cpp
    module/kernel_api.cpp
    module/loader.cpp
    sched/process.cpp
    sched/scheduler.cpp
    sched/thread.cpp
    sched/waitqueue.cpp
    sys/elf_loader.cpp
    sys/fault.cpp
    sys/clock.cpp
    sys/syscall.cpp
    sys/userland.cpp
    main.cpp
    panic.cpp
    selftest.cpp
)

set(KERNEL_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/${SHITOS_ARCH}/linker.ld")

add_executable(kernel.elf ${KERNEL_SOURCES})
target_link_libraries(kernel.elf PRIVATE shitos_kernel_flags)
target_include_directories(kernel.elf PRIVATE "${CMAKE_SOURCE_DIR}")

target_compile_options(kernel.elf PRIVATE
    # The kernel lives in the top 2 GiB, which is exactly what this model
    # assumes; it lets the compiler use 32-bit sign-extended addressing.
    $<$<COMPILE_LANGUAGE:C,CXX>:-mcmodel=kernel>
    # Keep frame pointers so panic() can produce a backtrace.
    $<$<COMPILE_LANGUAGE:C,CXX>:-fno-omit-frame-pointer>
)

target_link_options(kernel.elf PRIVATE
    "-Wl,-T,${KERNEL_LINKER_SCRIPT}"
    -nostdlib
    -static
    -Wl,--build-id=none
    -Wl,-z,max-page-size=0x1000
    -Wl,-z,noexecstack
)
set_target_properties(kernel.elf PROPERTIES LINK_DEPENDS "${KERNEL_LINKER_SCRIPT}")
