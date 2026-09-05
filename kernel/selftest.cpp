// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- boot-time self tests.

#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/fpu.h>
#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/arch/x86_64/percpu.h>
#include <kernel/arch/x86_64/pit.h>
#include <kernel/boot/boot_info.h>
#include <kernel/dev/console.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/tmpfs.h>
#include <kernel/fs/vfs.h>
#include <kernel/lib/elf.h>
#include <kernel/lib/string.h>
#include <kernel/mm/address_space.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/physical.h>
#include <kernel/module/loader.h>
#include <kernel/panic.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/waitqueue.h>
#include <kernel/selftest.h>
#include <kernel/sys/clock.h>

#include <shitos/abi/ioctl.h>
#include <shitos/abi/termios.h>

extern "C" {
extern u8 __text_start[];
extern u8 __rodata_start[];
extern u8 __data_start[];
}

namespace kernel {

namespace {

usize s_checks_run = 0;
usize s_checks_failed = 0;

void check(bool condition, char const* description)
{
    ++s_checks_run;
    if (condition)
        return;
    ++s_checks_failed;
    klog(LOG_ERROR, "selftest", "FAILED: %s", description);
}

volatile bool s_breakpoint_seen = false;

InterruptFrame* on_breakpoint(InterruptFrame* frame)
{
    s_breakpoint_seen = true;
    return frame;
}

void test_interrupts()
{
    // A breakpoint is the safest way to prove the whole path works: stub ->
    // dispatch -> handler -> iretq, with execution continuing afterwards.
    arch::register_trap_handler(3, on_breakpoint);
    s_breakpoint_seen = false;
    asm volatile("int3");
    check(s_breakpoint_seen, "int3 reached its handler and returned");
    arch::register_trap_handler(3, nullptr);
}

void test_physical_allocator()
{
    usize const free_before = mm::free_pages();

    auto first = mm::allocate_page();
    auto second = mm::allocate_page();
    check(!first.is_error() && !second.is_error(), "physical allocator returns pages");
    if (first.is_error() || second.is_error())
        return;

    check(raw(first.value()) != raw(second.value()), "two allocations are different pages");
    check(raw(first.value()) % PAGE_SIZE == 0, "allocated pages are page aligned");
    check(mm::free_pages() == free_before - 2, "free count tracks allocations");

    // Writing through the direct map must reach real memory.
    auto* bytes = static_cast<u8*>(phys_to_virt(first.value()));
    memset(bytes, 0xA5, PAGE_SIZE);
    check(bytes[0] == 0xA5 && bytes[PAGE_SIZE - 1] == 0xA5, "direct map is writable");

    auto zeroed = mm::allocate_zeroed_page();
    check(!zeroed.is_error(), "zeroed allocation succeeds");
    if (!zeroed.is_error()) {
        auto const* z = static_cast<u8 const*>(phys_to_virt(zeroed.value()));
        bool all_zero = true;
        for (usize i = 0; i < PAGE_SIZE; ++i)
            all_zero &= z[i] == 0;
        check(all_zero, "zeroed allocation really is zeroed");
        mm::free_page(zeroed.value());
    }

    auto run = mm::allocate_contiguous(4);
    check(!run.is_error(), "contiguous allocation succeeds");
    if (!run.is_error()) {
        check(raw(run.value()) % PAGE_SIZE == 0, "contiguous run is page aligned");
        mm::free_contiguous(run.value(), 4);
    }

    mm::free_page(first.value());
    mm::free_page(second.value());
    check(mm::free_pages() == free_before, "everything allocated was given back");
}

void test_heap()
{
    // Every size class, plus the large path, with a pattern written across the
    // whole payload so an off-by-one in the chunk maths shows up as corruption.
    usize const sizes[] = { 1, 15, 16, 17, 100, 500, 1000, 2000, 2032, 2033, 5000, 100000 };
    void* pointers[sizeof(sizes) / sizeof(sizes[0])] = {};

    for (usize i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        pointers[i] = kmalloc(sizes[i]);
        check(pointers[i] != nullptr, "kmalloc returns memory");
        if (pointers[i] == nullptr)
            continue;
        check(reinterpret_cast<u64>(pointers[i]) % 16 == 0, "kmalloc is 16-byte aligned");
        memset(pointers[i], static_cast<int>(0x40 + i), sizes[i]);
    }

    // Verify nothing overwrote anything else before freeing any of it.
    bool intact = true;
    for (usize i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        if (pointers[i] == nullptr)
            continue;
        auto const* bytes = static_cast<u8 const*>(pointers[i]);
        for (usize j = 0; j < sizes[i]; ++j)
            intact &= bytes[j] == static_cast<u8>(0x40 + i);
    }
    check(intact, "concurrent allocations do not overlap");

    for (void* pointer : pointers)
        kfree(pointer);

    auto* grown = static_cast<char*>(kmalloc(64));
    check(grown != nullptr, "kmalloc for realloc test");
    if (grown != nullptr) {
        memset(grown, 'x', 64);
        auto* bigger = static_cast<char*>(krealloc(grown, 4096));
        check(bigger != nullptr, "krealloc grows an allocation");
        if (bigger != nullptr) {
            bool preserved = true;
            for (usize i = 0; i < 64; ++i)
                preserved &= bigger[i] == 'x';
            check(preserved, "krealloc preserves the old contents");
            kfree(bigger);
        }
    }

    void* aligned = kmalloc_aligned(8192, PAGE_SIZE);
    check(aligned != nullptr, "kmalloc_aligned returns memory");
    if (aligned != nullptr) {
        check(reinterpret_cast<u64>(aligned) % PAGE_SIZE == 0, "kmalloc_aligned is page aligned");
        memset(aligned, 0x5A, 8192);
        kfree_aligned(aligned);
    }

    auto const stats = heap_stats();
    check(stats.allocation_count > 0 && stats.free_count > 0, "heap statistics move");
}

void test_cpu_configuration()
{
    u64 cr0, cr4;
    asm volatile("movq %%cr0, %0" : "=r"(cr0));
    asm volatile("movq %%cr4, %0" : "=r"(cr4));

    // Without CR0.WP the kernel can write through a read-only mapping, which
    // would quietly defeat both W^X and, later, copy-on-write.
    check((cr0 & (1ULL << 16)) != 0, "CR0.WP is set");

    auto const& features = arch::cpu_features();
    if (features.pge)
        check((cr4 & (1ULL << 7)) != 0, "CR4.PGE is set when the CPU supports it");
    if (features.smep)
        check((cr4 & (1ULL << 20)) != 0, "CR4.SMEP is set when the CPU supports it");
}

void test_address_space()
{
    auto& space = mm::AddressSpace::kernel_space();

    auto frame = mm::allocate_page();
    check(!frame.is_error(), "page for mapping test");
    if (frame.is_error())
        return;

    // Somewhere in the MMIO window that nothing else has claimed.
    auto const test_address = virt(mm::MMIO_WINDOW_BASE + mm::MMIO_WINDOW_SIZE - PAGE_SIZE);

    auto mapped = space.map(test_address, frame.value(),
        mm::PageFlags::Present | mm::PageFlags::Writable | mm::PageFlags::NoExecute);
    check(!mapped.is_error(), "mapping a page into the kernel space");

    if (!mapped.is_error()) {
        auto translated = space.translate(test_address);
        check(!translated.is_error() && raw(translated.value()) == raw(frame.value()),
            "translate round-trips the mapping");

        auto* through_mapping = reinterpret_cast<volatile u64*>(raw(test_address));
        *through_mapping = 0xDEADBEEFCAFEBABEULL;
        auto const* through_direct_map = static_cast<u64 const*>(phys_to_virt(frame.value()));
        check(*through_direct_map == 0xDEADBEEFCAFEBABEULL,
            "a write through the new mapping lands in the right frame");

        space.unmap(test_address);
        check(!space.is_mapped(test_address), "unmap removes the mapping");
    }

    mm::free_page(frame.value());

    // The low identity map from boot.S must be gone now that the real kernel
    // space is active; anything still relying on it would be a latent bug.
    check(space.translate(virt(0x100000)).is_error(), "the boot identity map is gone");

    // W^X, asserted by reading the page tables rather than by faulting.
    auto const text_flags = space.query(virt(reinterpret_cast<u64>(__text_start)));
    check(!text_flags.is_error(), ".text is mapped");
    if (!text_flags.is_error()) {
        check(!mm::has_flag(text_flags.value(), mm::PageFlags::Writable), ".text is not writable");
        check(!mm::has_flag(text_flags.value(), mm::PageFlags::NoExecute), ".text is executable");
        check(
            !mm::has_flag(text_flags.value(), mm::PageFlags::User), ".text is not user accessible");
    }

    auto const rodata_flags = space.query(virt(reinterpret_cast<u64>(__rodata_start)));
    check(!rodata_flags.is_error(), ".rodata is mapped");
    if (!rodata_flags.is_error()) {
        check(!mm::has_flag(rodata_flags.value(), mm::PageFlags::Writable),
            ".rodata is not writable");
        check(mm::has_flag(rodata_flags.value(), mm::PageFlags::NoExecute),
            ".rodata is not executable");
    }

    auto const data_flags = space.query(virt(reinterpret_cast<u64>(__data_start)));
    check(!data_flags.is_error(), ".data is mapped");
    if (!data_flags.is_error()) {
        check(mm::has_flag(data_flags.value(), mm::PageFlags::Writable), ".data is writable");
        check(
            mm::has_flag(data_flags.value(), mm::PageFlags::NoExecute), ".data is not executable");
    }

    // The direct map must never be executable: nothing should be able to jump
    // into a physical page just because it is addressable.
    auto const hhdm_flags = space.query(virt(HHDM_BASE + 0x200000));
    check(!hhdm_flags.is_error() && mm::has_flag(hhdm_flags.value(), mm::PageFlags::NoExecute),
        "the direct map is not executable");
}

void test_formatting()
{
    char buffer[128];

    snprintf(buffer, sizeof(buffer), "%d %u %x %s %c", -42, 42u, 0xbeefu, "str", 'z');
    check(strcmp(buffer, "-42 42 beef str z") == 0, "printf conversions");

    snprintf(buffer, sizeof(buffer), "[%8d][%-8d][%08d]", 42, 42, 42);
    check(strcmp(buffer, "[      42][42      ][00000042]") == 0, "printf padding");

    snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(-9223372036854775807LL - 1));
    check(strcmp(buffer, "-9223372036854775808") == 0, "printf handles the most negative i64");

    usize const needed = snprintf(buffer, 8, "0123456789");
    check(needed == 10 && strlen(buffer) == 7, "snprintf truncates but reports the full length");
}

// --- scheduler ----------------------------------------------------------

struct WorkerState {
    u32 iterations_done;
    bool finished;
};

WorkerState s_workers[3];

void worker_thread(void* argument)
{
    auto* state = static_cast<WorkerState*>(argument);
    for (u32 i = 0; i < 50; ++i) {
        __atomic_add_fetch(&state->iterations_done, 1, __ATOMIC_RELAXED);
        Scheduler::yield();
    }
    __atomic_store_n(&state->finished, true, __ATOMIC_RELEASE);
}

WaitQueue s_test_queue;
bool s_waiter_woke = false;
bool s_waiter_started = false;

void waiter_thread(void*)
{
    __atomic_store_n(&s_waiter_started, true, __ATOMIC_RELEASE);
    s_test_queue.wait();
    __atomic_store_n(&s_waiter_woke, true, __ATOMIC_RELEASE);
}

void test_scheduler()
{
    check(Scheduler::is_running(), "the scheduler is running");
    check(Scheduler::current() != nullptr, "there is a current thread");

    u64 const switches_before = Scheduler::context_switches();

    for (auto& state : s_workers)
        state = { 0, false };

    for (auto& state : s_workers) {
        auto thread = Thread::create_kernel_thread("selftest-worker", worker_thread, &state);
        check(!thread.is_error(), "creating a kernel thread");
        if (!thread.is_error())
            Scheduler::enqueue(thread.value());
    }

    // Give them room to run. If preemption is broken this loop never finishes
    // and the watchdog in run-qemu.sh is what reports it.
    for (u32 attempts = 0; attempts < 200; ++attempts) {
        bool all_done = true;
        for (auto& state : s_workers)
            all_done &= __atomic_load_n(&state.finished, __ATOMIC_ACQUIRE);
        if (all_done)
            break;
        Scheduler::sleep_ms(10);
    }

    bool all_finished = true;
    bool all_complete = true;
    for (auto& state : s_workers) {
        all_finished &= __atomic_load_n(&state.finished, __ATOMIC_ACQUIRE);
        all_complete &= __atomic_load_n(&state.iterations_done, __ATOMIC_RELAXED) == 50;
    }
    check(all_finished, "every worker thread ran to completion");
    check(all_complete, "every worker thread completed all its iterations");
    check(Scheduler::context_switches() > switches_before, "context switches happened");

    // Sleeping should take about as long as asked. The tolerance is wide
    // because the tick is 4 ms and an emulated machine is not precise.
    u64 const before = arch::pit_uptime_ms();
    Scheduler::sleep_ms(100);
    u64 const elapsed = arch::pit_uptime_ms() - before;
    check(elapsed >= 90 && elapsed <= 300, "sleep_ms sleeps roughly the requested time");

    // A thread blocked on a wait queue must not run until it is woken.
    s_waiter_woke = false;
    s_waiter_started = false;
    auto waiter = Thread::create_kernel_thread("selftest-waiter", waiter_thread, nullptr);
    check(!waiter.is_error(), "creating the wait queue test thread");
    if (!waiter.is_error()) {
        Scheduler::enqueue(waiter.value());
        while (!__atomic_load_n(&s_waiter_started, __ATOMIC_ACQUIRE))
            Scheduler::sleep_ms(4);
        Scheduler::sleep_ms(20);
        check(!__atomic_load_n(&s_waiter_woke, __ATOMIC_ACQUIRE), "a blocked thread stays blocked");
        check(s_test_queue.waiter_count() == 1, "the wait queue knows about its waiter");

        s_test_queue.wake_all();
        for (u32 attempts = 0; attempts < 100 && !__atomic_load_n(&s_waiter_woke, __ATOMIC_ACQUIRE);
             ++attempts)
            Scheduler::sleep_ms(4);
        check(__atomic_load_n(&s_waiter_woke, __ATOMIC_ACQUIRE),
            "waking a wait queue releases the waiter");
    }

    // Exited threads are reaped by the idle thread, so the count comes back
    // down once the system has had a moment to breathe.
    usize const count_before = Scheduler::thread_count();
    Scheduler::sleep_ms(50);
    check(Scheduler::thread_count() <= count_before, "finished threads get reaped");
}

// --- floating point across context switches -----------------------------
//
// The kernel is built with -mno-sse, so the compiler never touches these
// registers and this is the only code in the kernel that does. That is what
// makes the test meaningful: any corruption it sees came from a context
// switch, not from the surrounding C++.

void write_vector_registers(u64 pattern)
{
    asm volatile("movq %0, %%xmm0\n"
                 "movq %0, %%xmm1\n"
                 "movq %0, %%xmm7\n"
                 "movq %0, %%xmm15\n" ::"r"(pattern));
}

bool vector_registers_hold(u64 pattern)
{
    u64 xmm0, xmm1, xmm7, xmm15;
    asm volatile("movq %%xmm0, %0\n"
                 "movq %%xmm1, %1\n"
                 "movq %%xmm7, %2\n"
                 "movq %%xmm15, %3\n"
                 : "=r"(xmm0), "=r"(xmm1), "=r"(xmm7), "=r"(xmm15));
    return xmm0 == pattern && xmm1 == pattern && xmm7 == pattern && xmm15 == pattern;
}

struct VectorWorker {
    u64 pattern;
    u32 iterations;
    u32 corruptions;
    bool finished;
};

VectorWorker s_vector_workers[3];

void vector_worker(void* argument)
{
    auto* state = static_cast<VectorWorker*>(argument);

    write_vector_registers(state->pattern);
    for (u32 i = 0; i < 300; ++i) {
        // Yielding here is the whole point: something else runs, loads its own
        // values into the same registers, and this thread has to come back to
        // find its own still there.
        Scheduler::yield();
        if (!vector_registers_hold(state->pattern)) {
            ++state->corruptions;
            write_vector_registers(state->pattern);
        }
        ++state->iterations;
    }

    __atomic_store_n(&state->finished, true, __ATOMIC_RELEASE);
}

void test_fpu_context_switching()
{
    // Distinct patterns so a thread that sees another's value is detected
    // rather than coincidentally matching.
    u64 const patterns[3] = { 0x1111111122222222ULL, 0x3333333344444444ULL, 0x5555555566666666ULL };

    for (usize i = 0; i < 3; ++i) {
        s_vector_workers[i] = { patterns[i], 0, 0, false };
        auto thread
            = Thread::create_kernel_thread("selftest-fpu", vector_worker, &s_vector_workers[i]);
        check(!thread.is_error(), "creating a vector-register thread");
        if (!thread.is_error())
            Scheduler::enqueue(thread.value());
    }

    for (u32 attempts = 0; attempts < 400; ++attempts) {
        bool all_done = true;
        for (auto& worker : s_vector_workers)
            all_done &= __atomic_load_n(&worker.finished, __ATOMIC_ACQUIRE);
        if (all_done)
            break;
        Scheduler::sleep_ms(10);
    }

    u32 total_iterations = 0;
    u32 total_corruptions = 0;
    for (auto const& worker : s_vector_workers) {
        total_iterations += worker.iterations;
        total_corruptions += worker.corruptions;
    }

    check(total_iterations >= 900, "the vector threads all ran to completion");
    check(total_corruptions == 0, "xmm registers survive a context switch");

    if (total_corruptions != 0) {
        klog(LOG_ERROR, "selftest", "  %u of %u yields lost vector state", total_corruptions,
            total_iterations);
    }

    // A new thread must not inherit whatever the creating thread was holding.
    alignas(arch::FPU_STATE_ALIGNMENT) u8 fresh[arch::FPU_STATE_SIZE];
    write_vector_registers(0xDEADBEEFDEADBEEFULL);
    arch::fpu_initialize_state(fresh);
    // MXCSR sits at offset 24 of the FXSAVE area; the default masks every
    // exception, so an overflow in a user program is a value, not a fault.
    u32 const mxcsr = *reinterpret_cast<u32 const*>(fresh + 24);
    check((mxcsr & 0x1F80) == 0x1F80, "a fresh FPU state masks all SIMD exceptions");
}

// --- filesystems --------------------------------------------------------

void test_vfs()
{
    check(fs::root_inode() != nullptr, "there is a root inode");
    check(fs::mount_count() == 3, "three filesystems are mounted");

    // The initrd, read-only, with content the build put there.
    auto motd = fs::resolve("/etc/motd");
    check(!motd.is_error(), "resolving a path into the initrd");
    if (!motd.is_error()) {
        check(motd.value()->type() == fs::InodeType::Regular, "/etc/motd is a regular file");
        check(motd.value()->size() > 0, "/etc/motd is not empty");

        char buffer[64] = {};
        auto read = motd.value()->read(0, buffer, sizeof(buffer) - 1);
        check(!read.is_error() && read.value() > 0, "reading from the initrd");
        if (!read.is_error())
            check(strstr(buffer, "shit os 2") != nullptr, "the initrd content is what we packed");

        // Reading past the end returns zero bytes rather than failing.
        auto past_end = motd.value()->read(motd.value()->size() + 10, buffer, 8);
        check(!past_end.is_error() && past_end.value() == 0, "reading past EOF returns nothing");
    }

    auto missing = fs::resolve("/does/not/exist");
    check(missing.is_error() && missing.error().code() == ENOENT, "a missing path is ENOENT");

    // The initrd is read-only and must say so rather than pretending.
    auto readonly_write = fs::open("/etc/motd", O_WRONLY, 0);
    check(readonly_write.is_error(), "the initrd refuses to be opened for writing");

    // Mount traversal: /tmp resolves into tmpfs, not into the initrd's
    // empty placeholder directory.
    auto tmp = fs::resolve("/tmp");
    check(!tmp.is_error(), "resolving /tmp");
    if (!tmp.is_error())
        check(strcmp(tmp.value()->filesystem()->name(), "tmpfs") == 0, "/tmp crosses into tmpfs");

    auto dev = fs::resolve("/dev");
    check(!dev.is_error(), "resolving /dev");
    if (!dev.is_error())
        check(strcmp(dev.value()->filesystem()->name(), "devfs") == 0, "/dev crosses into devfs");
}

void test_tmpfs()
{
    char const* payload = "the quick brown fox jumps over the lazy dog";
    usize const payload_length = strlen(payload);

    auto created = fs::open("/tmp/selftest.txt", O_RDWR | O_CREAT, 0644);
    check(!created.is_error(), "creating a file on tmpfs");
    if (created.is_error())
        return;

    auto* file = created.value();
    auto written = file->write(payload, payload_length);
    check(!written.is_error() && written.value() == payload_length, "writing to tmpfs");

    check(!file->seek(0, SEEK_SET).is_error(), "seeking back to the start");

    char buffer[128] = {};
    auto read = file->read(buffer, sizeof(buffer) - 1);
    check(!read.is_error() && read.value() == payload_length, "reading back the same length");
    check(strcmp(buffer, payload) == 0, "reading back the same bytes");

    // Growing past the current end has to leave zeroes, not heap garbage.
    check(!file->seek(0, SEEK_END).is_error(), "seeking to the end");
    auto appended = file->write("!", 1);
    check(!appended.is_error(), "appending to tmpfs");

    auto reopened = fs::resolve("/tmp/selftest.txt");
    check(!reopened.is_error(), "the file is visible by path");
    if (!reopened.is_error())
        check(reopened.value()->size() == payload_length + 1, "the size reflects the append");

    // Directories.
    auto tmp = fs::resolve("/tmp");
    check(!tmp.is_error(), "resolving /tmp for mkdir");
    if (!tmp.is_error()) {
        auto directory = tmp.value()->create("subdir", fs::InodeType::Directory, 0755);
        check(!directory.is_error(), "creating a directory on tmpfs");

        auto duplicate = tmp.value()->create("subdir", fs::InodeType::Directory, 0755);
        check(duplicate.is_error() && duplicate.error().code() == EEXIST,
            "creating an existing name is EEXIST");

        auto nested = fs::open("/tmp/subdir/nested.txt", O_RDWR | O_CREAT, 0644);
        check(!nested.is_error(), "creating a file inside a new directory");

        auto non_empty = tmp.value()->unlink("subdir");
        check(non_empty.is_error() && non_empty.error().code() == ENOTEMPTY,
            "removing a non-empty directory is ENOTEMPTY");

        if (!nested.is_error()) {
            auto* subdir = fs::resolve("/tmp/subdir").value();
            check(!subdir->unlink("nested.txt").is_error(), "unlinking a file");
            check(!tmp.value()->unlink("subdir").is_error(), "removing the now-empty directory");
            check(fs::resolve("/tmp/subdir").is_error(), "the removed directory is gone");
        }
    }

    // getdents, through a real FileDescription.
    auto directory_fd = fs::open("/tmp", O_RDONLY | O_DIRECTORY, 0);
    check(!directory_fd.is_error(), "opening a directory");
    if (!directory_fd.is_error()) {
        u8 entries[512];
        auto bytes = directory_fd.value()->get_directory_entries(entries, sizeof(entries));
        check(!bytes.is_error() && bytes.value() > 0, "getdents returns entries");

        bool found_self = false;
        bool found_file = false;
        usize offset = 0;
        while (!bytes.is_error() && offset < bytes.value()) {
            auto const* entry = reinterpret_cast<struct dirent const*>(entries + offset);
            if (strcmp(entry->d_name, ".") == 0)
                found_self = true;
            if (strcmp(entry->d_name, "selftest.txt") == 0)
                found_file = true;
            check(entry->d_reclen % 8 == 0, "dirent records stay 8-aligned");
            offset += entry->d_reclen;
        }
        check(found_self, "getdents includes .");
        check(found_file, "getdents includes the file we created");
    }
}

/*
 * tmpfs holds file contents as a list of whole pages, so every offset
 * arithmetic bug lives at a page boundary and nowhere else. A file that fits
 * in one page -- which is every file the tests above make -- would never
 * notice.
 */
void test_tmpfs_pages()
{
    auto created = fs::open("/tmp/pages.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(!created.is_error(), "creating a multi-page file");
    if (created.is_error())
        return;
    auto* file = created.value();

    // A pattern that is different in every byte of a 3-page span, so a copy
    // that lands one page out cannot pass by accident.
    constexpr usize SPAN = 3 * PAGE_SIZE;
    auto* pattern = static_cast<u8*>(kmalloc(SPAN));
    check(pattern != nullptr, "allocating the pattern");
    if (pattern == nullptr)
        return;
    for (usize i = 0; i < SPAN; ++i)
        pattern[i] = static_cast<u8>((i * 31 + (i >> 8)) & 0xff);

    auto written = file->write(pattern, SPAN);
    check(!written.is_error() && written.value() == SPAN, "writing three pages at once");

    // Read it back in an awkward size, so no read starts or ends on a page.
    auto* readback = static_cast<u8*>(kzalloc(SPAN));
    check(readback != nullptr, "allocating the readback buffer");
    if (readback == nullptr) {
        kfree(pattern);
        return;
    }

    check(!file->seek(0, SEEK_SET).is_error(), "seeking back for the readback");
    usize total = 0;
    while (total < SPAN) {
        auto read = file->read(readback + total, min<usize>(1000, SPAN - total));
        if (read.is_error() || read.value() == 0)
            break;
        total += read.value();
    }
    check(total == SPAN, "reading three pages back in 1000-byte bites");
    check(memcmp(pattern, readback, SPAN) == 0, "the bytes survive the page boundaries");

    // A write that starts inside one page and ends inside the next.
    u8 straddle[64];
    for (usize i = 0; i < sizeof(straddle); ++i)
        straddle[i] = 0xA5;
    check(!file->seek(PAGE_SIZE - 32, SEEK_SET).is_error(), "seeking to a page boundary");
    check(!file->write(straddle, sizeof(straddle)).is_error(), "writing across a boundary");

    memset(readback, 0, 64);
    check(!file->seek(PAGE_SIZE - 32, SEEK_SET).is_error(), "seeking back to the boundary");
    check(!file->read(readback, sizeof(straddle)).is_error(), "reading across a boundary");
    check(memcmp(readback, straddle, sizeof(straddle)) == 0, "a straddling write reads back");

    // Shrinking must not leave the old bytes recoverable by growing again.
    auto* inode = &file->inode();
    check(!inode->truncate(PAGE_SIZE + 16).is_error(), "truncating down");
    check(inode->size() == PAGE_SIZE + 16, "the size follows the truncate");
    check(!inode->truncate(SPAN).is_error(), "growing back");

    memset(readback, 0xff, SPAN);
    check(!file->seek(PAGE_SIZE + 16, SEEK_SET).is_error(), "seeking into the regrown tail");
    auto tail = file->read(readback, 256);
    check(!tail.is_error() && tail.value() == 256, "reading the regrown tail");
    bool all_zero = true;
    for (usize i = 0; i < 256; ++i) {
        if (readback[i] != 0)
            all_zero = false;
    }
    check(all_zero, "shrinking and regrowing does not bring the old bytes back");

    // physical_page is what mmap will call. Two offsets in the same page must
    // give the same frame, and different pages must not.
    auto first = inode->physical_page(0, false);
    auto same = inode->physical_page(PAGE_SIZE - 1, false);
    auto second = inode->physical_page(PAGE_SIZE, false);
    check(!first.is_error() && !same.is_error() && !second.is_error(),
        "physical_page answers for a sized file");
    if (!first.is_error() && !same.is_error() && !second.is_error()) {
        check(first.value() == same.value(), "one page answers for all its offsets");
        check(first.value() != second.value(), "different pages are different frames");
    }
    check(inode->physical_page(SPAN + PAGE_SIZE, false).is_error(),
        "physical_page refuses an offset past the end");

    kfree(pattern);
    kfree(readback);
    if (auto tmp = fs::resolve("/tmp"); !tmp.is_error())
        (void)tmp.value()->unlink("pages.bin");
}

void test_inode_lifetime()
{
    // The regression this whole change exists for: a file removed while it is
    // still open used to be freed under its reader, and the next allocation
    // of the same size handed the reader somebody else's bytes.
    static constexpr char const CANARY[] = "CANARY-DATA-1234";
    static constexpr usize CANARY_LENGTH = sizeof(CANARY) - 1;

    auto writer = fs::open("/tmp/lifetime.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(!writer.is_error(), "creating a file to unlink while open");
    if (writer.is_error())
        return;

    check(!writer.value()->write(CANARY, CANARY_LENGTH).is_error(), "writing the canary");
    fs::release_description(writer.value());

    auto* inode = fs::resolve("/tmp/lifetime.txt").value();

    // One reference: the directory entry naming it.
    check(inode->reference_count() == 1, "a named, unopened inode has one reference");
    check(!inode->is_unlinked(), "a named inode is not unlinked");

    auto reader = fs::open("/tmp/lifetime.txt", O_RDONLY, 0);
    check(!reader.is_error(), "opening the file");
    if (reader.is_error())
        return;
    check(inode->reference_count() == 2, "an open description adds a reference");

    // dup() shares the description, so the inode count must not move.
    reader.value()->ref();
    check(inode->reference_count() == 2, "dup of a description does not add an inode reference");
    (void)reader.value()->unref();

    auto* tmp = fs::resolve("/tmp").value();
    check(!tmp->unlink("lifetime.txt").is_error(), "unlinking a file that is open");
    check(fs::resolve("/tmp/lifetime.txt").is_error(), "the unlinked name is gone");
    check(inode->reference_count() == 1, "unlink drops the directory reference");
    check(inode->is_unlinked(), "the inode knows it has lost its last name");
    check(inode->parent() == nullptr, "an unlinked inode has no parent");

    // Churn the heap hard enough that a freed inode would certainly have been
    // handed out again, then read through the descriptor we still hold.
    for (usize i = 0; i < 64; ++i) {
        void* scratch = kmalloc(sizeof(fs::Inode) + 64);
        if (scratch != nullptr) {
            memset(scratch, 0x5A, sizeof(fs::Inode) + 64);
            kfree(scratch);
        }
    }

    char buffer[64] = {};
    auto read = reader.value()->read(buffer, sizeof(buffer) - 1);
    check(!read.is_error() && read.value() == CANARY_LENGTH, "an unlinked file still reads back");
    check(strcmp(buffer, CANARY) == 0, "and reads back the data it had, not the heap's");

    // Closing the last description is what finally frees it.
    usize const before = heap_stats().bytes_in_use;
    fs::release_description(reader.value());
    usize const after = heap_stats().bytes_in_use;
    check(after < before, "the last close frees the unlinked inode");

    // A file unlinked with nothing open goes away immediately.
    auto immediate = fs::open("/tmp/immediate.txt", O_RDWR | O_CREAT, 0644);
    check(!immediate.is_error(), "creating a file to remove straight away");
    if (!immediate.is_error()) {
        fs::release_description(immediate.value());
        usize const held = heap_stats().bytes_in_use;
        check(!tmp->unlink("immediate.txt").is_error(), "unlinking a closed file");
        check(heap_stats().bytes_in_use < held, "removing a closed file frees it at once");
    }
}

void test_rename()
{
    // Everything here is on tmpfs, which is the only writable filesystem.
    auto* tmp = fs::resolve("/tmp").value();

    auto source = fs::open("/tmp/rename-a.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(!source.is_error(), "creating a file to rename");
    if (source.is_error())
        return;
    check(!source.value()->write("first", 5).is_error(), "writing to it");
    auto* inode = &source.value()->inode();
    fs::release_description(source.value());

    check(!fs::rename("/tmp/rename-a.txt", "/tmp/rename-b.txt").is_error(), "renaming a file");
    check(fs::resolve("/tmp/rename-a.txt").is_error(), "the old name is gone");
    auto renamed = fs::resolve("/tmp/rename-b.txt");
    check(!renamed.is_error(), "the new name resolves");
    check(!renamed.is_error() && renamed.value() == inode, "and to the same inode, not a copy");

    // Renaming onto an existing file replaces it silently, which is the
    // property that makes write-to-temp-then-rename an atomic update.
    auto victim = fs::open("/tmp/rename-c.txt", O_RDWR | O_CREAT, 0644);
    check(!victim.is_error(), "creating a file to be replaced");
    if (!victim.is_error()) {
        check(!victim.value()->write("second", 6).is_error(), "writing to the victim");
        fs::release_description(victim.value());
        check(!fs::rename("/tmp/rename-b.txt", "/tmp/rename-c.txt").is_error(),
            "renaming over an existing file");
        auto survivor = fs::resolve("/tmp/rename-c.txt");
        check(!survivor.is_error() && survivor.value() == inode, "the source won");

        char buffer[16] = {};
        auto reopened = fs::open("/tmp/rename-c.txt", O_RDONLY, 0);
        if (!reopened.is_error()) {
            auto read = reopened.value()->read(buffer, sizeof(buffer) - 1);
            check(!read.is_error() && strcmp(buffer, "first") == 0,
                "and kept its own contents, not the replaced file's");
            fs::release_description(reopened.value());
        }
    }

    // A directory and a file are not interchangeable, in either direction.
    check(!tmp->create("rename-dir", fs::InodeType::Directory, 0755).is_error(),
        "creating a directory to rename");
    auto onto_file = fs::rename("/tmp/rename-dir", "/tmp/rename-c.txt");
    check(onto_file.is_error() && onto_file.error().code() == ENOTDIR,
        "a directory will not replace a file");
    auto onto_directory = fs::rename("/tmp/rename-c.txt", "/tmp/rename-dir");
    check(onto_directory.is_error() && onto_directory.error().code() == EISDIR,
        "a file will not replace a directory");

    // Moving a directory into its own subtree would detach it from the root.
    auto* directory = fs::resolve("/tmp/rename-dir").value();
    check(!directory->create("inner", fs::InodeType::Directory, 0755).is_error(),
        "creating a nested directory");
    auto into_self = fs::rename("/tmp/rename-dir", "/tmp/rename-dir/inner/moved");
    check(into_self.is_error() && into_self.error().code() == EINVAL,
        "a directory will not move inside itself");

    // Across filesystems rename is a copy, and POSIX says it must not silently
    // become one.
    auto cross = fs::rename("/tmp/rename-c.txt", "/dev/rename-c.txt");
    check(
        cross.is_error() && cross.error().code() == EXDEV, "renaming across filesystems is EXDEV");

    // Renaming a name onto itself changes nothing and must not destroy it.
    check(!fs::rename("/tmp/rename-c.txt", "/tmp/rename-c.txt").is_error(),
        "renaming a file onto itself succeeds");
    check(!fs::resolve("/tmp/rename-c.txt").is_error(), "and the file is still there");

    (void)directory->unlink("inner");
    (void)tmp->unlink("rename-dir");
    (void)tmp->unlink("rename-c.txt");
}

void test_clock()
{
    u64 const first = clock_monotonic_ns();
    // A busy loop long enough that the 4 ms tick has certainly advanced.
    arch::pit_busy_wait_ms(10);
    u64 const second = clock_monotonic_ns();
    check(second > first, "the monotonic clock advances");

    // Real time is either a genuine date or, with no driver registered yet,
    // the monotonic clock. Modules load after this runs, so it is the latter.
    check(clock_realtime_ns() >= clock_monotonic_ns(),
        "real time is never behind the monotonic clock");
    check(!clock_realtime_is_set(), "no time source has registered before modules load");
}

void test_devfs()
{
    auto zero = fs::open("/dev/zero", O_RDONLY, 0);
    check(!zero.is_error(), "opening /dev/zero");
    if (!zero.is_error()) {
        u8 buffer[64];
        memset(buffer, 0xFF, sizeof(buffer));
        auto read = zero.value()->read(buffer, sizeof(buffer));
        check(!read.is_error() && read.value() == sizeof(buffer), "/dev/zero reads a full buffer");
        bool all_zero = true;
        for (u8 byte : buffer)
            all_zero &= byte == 0;
        check(all_zero, "/dev/zero really returns zeroes");
    }

    auto null_device = fs::open("/dev/null", O_RDWR, 0);
    check(!null_device.is_error(), "opening /dev/null");
    if (!null_device.is_error()) {
        u8 buffer[8];
        auto read = null_device.value()->read(buffer, sizeof(buffer));
        check(!read.is_error() && read.value() == 0, "/dev/null reads EOF");
        auto written = null_device.value()->write("discarded", 9);
        check(!written.is_error() && written.value() == 9, "/dev/null swallows writes");
    }

    auto stat_target = fs::resolve("/dev/zero");
    check(!stat_target.is_error(), "resolving /dev/zero");
    if (!stat_target.is_error()) {
        struct stat status;
        check(!stat_target.value()->stat(status).is_error(), "stat on a device node");
        check(S_ISCHR(status.st_mode), "stat reports a character device");
    }
}

// --- modules ------------------------------------------------------------

struct ModuleScan {
    bool found_ps2kbd;
    usize count;
    bool all_abi_current;
};

void inspect_module(LoadedModule const& module, void* context)
{
    auto* scan = static_cast<ModuleScan*>(context);
    ++scan->count;
    if (strcmp(module.name(), "ps2kbd") == 0)
        scan->found_ps2kbd = true;
    if (module.abi_version() != SHITOS_MODULE_ABI_VERSION)
        scan->all_abi_current = false;

    // A loaded module must live in the module window, or its 32-bit
    // displacements to kernel intrinsics could not have been in range.
    auto const base = reinterpret_cast<u64>(module.base());
    if (base < mm::MODULE_WINDOW_BASE || base >= mm::MODULE_WINDOW_BASE + mm::MODULE_WINDOW_SIZE)
        scan->all_abi_current = false;
}

void test_ioctl_encoding()
{
    // The encoding is what lets sys_ioctl move exactly the right number of
    // bytes for a request it has never seen. Getting the field widths wrong
    // would silently resize every driver's arguments.
    constexpr u32 read_request = _IOR(0x42, struct termios);
    check(_IOC_ARGUMENT_SIZE(read_request) == sizeof(struct termios),
        "an encoded request carries its argument size");
    check(_IOC_DIRECTION(read_request) == _IOC_READ, "an _IOR request reads back to userspace");

    constexpr u32 write_request = _IOW(0x43, struct winsize);
    check(_IOC_ARGUMENT_SIZE(write_request) == sizeof(struct winsize),
        "a small argument encodes its size");
    check(_IOC_DIRECTION(write_request) == _IOC_WRITE, "an _IOW request only reads from userspace");

    constexpr u32 both = _IOWR(0x44, u64);
    check(_IOC_DIRECTION(both) == (_IOC_READ | _IOC_WRITE), "an _IOWR request goes both ways");

    constexpr u32 valueless = _IO(0x45);
    check(_IOC_ARGUMENT_SIZE(valueless) == 0, "an _IO request has no pointer argument");

    // The request number itself has to survive the encoding, or a driver's
    // switch statement would never match.
    check((read_request & 0xFFFF) == 0x42, "the request number survives encoding");

    // A struct at the size limit must not overflow into the direction bits.
    check(_IOC_ARGUMENT_SIZE(_IOC(_IOC_READ, 1, _IOC_SIZE_MAX)) == _IOC_SIZE_MAX,
        "the maximum encodable size round-trips");
}

void test_modules()
{
    auto const& api = ModuleLoader::kernel_api();
    check(api.abi_version == SHITOS_MODULE_ABI_VERSION, "the KernelApi advertises the current ABI");

    // A null entry in the table is a module crashing at an unhelpful moment,
    // so check the whole surface rather than the parts we happen to use.
    check(api.log != nullptr && api.panic != nullptr, "logging entries are populated");
    check(api.kmalloc != nullptr && api.kzalloc != nullptr && api.kfree != nullptr,
        "memory entries are populated");
    check(api.map_mmio != nullptr && api.unmap_mmio != nullptr, "mmio entries are populated");
    check(api.inb != nullptr && api.outb != nullptr && api.inw != nullptr && api.outw != nullptr
            && api.inl != nullptr && api.outl != nullptr,
        "port io entries are populated");
    check(api.irq_register != nullptr && api.irq_unregister != nullptr,
        "interrupt entries are populated");
    check(api.device_register != nullptr && api.device_unregister != nullptr,
        "device entries are populated");
    check(api.waitqueue_create != nullptr && api.waitqueue_destroy != nullptr
            && api.waitqueue_wait != nullptr && api.waitqueue_wake_all != nullptr,
        "wait queue entries are populated");
    check(api.uptime_ms != nullptr && api.sleep_ms != nullptr && api.yield != nullptr,
        "timing entries are populated");

    ModuleScan scan { false, 0, true };
    ModuleLoader::for_each(inspect_module, &scan);

    check(scan.count == ModuleLoader::module_count(), "for_each visits every module");
    check(scan.count >= 1, "at least one module loaded");
    check(scan.found_ps2kbd, "ps2kbd loaded");
    check(scan.all_abi_current, "every module is at the current ABI and in the module window");

    // The driver registered a device node, which is the observable proof that
    // the whole path -- relocation, init, device_register -- worked.
    auto keyboard = fs::resolve("/dev/kbd0");
    check(!keyboard.is_error(), "the module's device node appeared in /dev");
    if (!keyboard.is_error())
        check(keyboard.value()->type() == fs::InodeType::CharacterDevice,
            "/dev/kbd0 is a character device");

    // The next two checks deliberately feed the loader bad images. It is
    // supposed to complain loudly about those, which in a boot log looks like
    // something went wrong, so quieten it for the duration.
    console_set_min_level(static_cast<LogLevel>(LOG_ERROR + 1));

    // Loading a bogus image must be refused rather than jumped into.
    u8 garbage[128];
    memset(garbage, 0xCC, sizeof(garbage));
    auto rejected = ModuleLoader::load("garbage.ko", garbage, sizeof(garbage));
    check(rejected.is_error() && rejected.error().code() == ENOEXEC, "a non-ELF image is rejected");

    // An ELF header that is valid but the wrong type must also be refused.
    u8 wrong_type[sizeof(elf::Elf64_Ehdr)] = {};
    auto* header = reinterpret_cast<elf::Elf64_Ehdr*>(wrong_type);
    header->e_ident[0] = elf::ELFMAG0;
    header->e_ident[1] = elf::ELFMAG1;
    header->e_ident[2] = elf::ELFMAG2;
    header->e_ident[3] = elf::ELFMAG3;
    header->e_ident[4] = elf::ELFCLASS64;
    header->e_ident[5] = elf::ELFDATA2LSB;
    header->e_machine = elf::EM_X86_64;
    header->e_type = elf::ET_EXEC;
    auto wrong = ModuleLoader::load("executable.ko", wrong_type, sizeof(wrong_type));
    check(wrong.is_error(), "an ET_EXEC image is rejected as a module");

    console_set_min_level(LOG_DEBUG);
}

} // namespace

void run_module_selftests()
{
    s_checks_run = 0;
    s_checks_failed = 0;

    test_ioctl_encoding();
    test_modules();

    if (s_checks_failed == 0) {
        klog(LOG_INFO, "selftest", "%zu module checks passed", s_checks_run);
    } else {
        klog(LOG_ERROR, "selftest", "%zu of %zu module checks FAILED", s_checks_failed,
            s_checks_run);
        panic("module self tests failed");
    }
}

void run_filesystem_selftests()
{
    s_checks_run = 0;
    s_checks_failed = 0;

    test_vfs();
    test_tmpfs();
    test_tmpfs_pages();
    test_inode_lifetime();
    test_rename();
    test_clock();
    test_devfs();

    if (s_checks_failed == 0) {
        klog(LOG_INFO, "selftest", "%zu filesystem checks passed", s_checks_run);
    } else {
        klog(LOG_ERROR, "selftest", "%zu of %zu filesystem checks FAILED", s_checks_failed,
            s_checks_run);
        panic("filesystem self tests failed");
    }
}

void run_scheduler_selftests()
{
    s_checks_run = 0;
    s_checks_failed = 0;

    test_scheduler();
    test_fpu_context_switching();

    if (s_checks_failed == 0) {
        klog(LOG_INFO, "selftest", "%zu scheduler checks passed", s_checks_run);
    } else {
        klog(LOG_ERROR, "selftest", "%zu of %zu scheduler checks FAILED", s_checks_failed,
            s_checks_run);
        panic("scheduler self tests failed");
    }
}

void run_boot_selftests()
{
    s_checks_run = 0;
    s_checks_failed = 0;

    test_formatting();
    test_interrupts();
    test_physical_allocator();
    test_heap();
    test_cpu_configuration();
    test_address_space();

    if (s_checks_failed == 0) {
        klog(LOG_INFO, "selftest", "%zu checks passed", s_checks_run);
    } else {
        klog(LOG_ERROR, "selftest", "%zu of %zu checks FAILED", s_checks_failed, s_checks_run);
        panic("boot self tests failed; the kernel is not in a state worth continuing from");
    }
}

} // namespace kernel
