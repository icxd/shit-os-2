// SPDX-License-Identifier: GPL-3.0-or-later
// shit os 2 -- boot-time self tests.

#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/interrupts.h>
#include <kernel/boot/boot_info.h>
#include <kernel/dev/console.h>
#include <kernel/lib/string.h>
#include <kernel/mm/address_space.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/physical.h>
#include <kernel/arch/x86_64/percpu.h>
#include <kernel/arch/x86_64/pit.h>
#include <kernel/panic.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/waitqueue.h>
#include <kernel/selftest.h>

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
        check(!mm::has_flag(text_flags.value(), mm::PageFlags::User), ".text is not user accessible");
    }

    auto const rodata_flags = space.query(virt(reinterpret_cast<u64>(__rodata_start)));
    check(!rodata_flags.is_error(), ".rodata is mapped");
    if (!rodata_flags.is_error()) {
        check(!mm::has_flag(rodata_flags.value(), mm::PageFlags::Writable), ".rodata is not writable");
        check(mm::has_flag(rodata_flags.value(), mm::PageFlags::NoExecute), ".rodata is not executable");
    }

    auto const data_flags = space.query(virt(reinterpret_cast<u64>(__data_start)));
    check(!data_flags.is_error(), ".data is mapped");
    if (!data_flags.is_error()) {
        check(mm::has_flag(data_flags.value(), mm::PageFlags::Writable), ".data is writable");
        check(mm::has_flag(data_flags.value(), mm::PageFlags::NoExecute), ".data is not executable");
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
        for (u32 attempts = 0; attempts < 100 && !__atomic_load_n(&s_waiter_woke, __ATOMIC_ACQUIRE); ++attempts)
            Scheduler::sleep_ms(4);
        check(__atomic_load_n(&s_waiter_woke, __ATOMIC_ACQUIRE), "waking a wait queue releases the waiter");
    }

    // Exited threads are reaped by the idle thread, so the count comes back
    // down once the system has had a moment to breathe.
    usize const count_before = Scheduler::thread_count();
    Scheduler::sleep_ms(50);
    check(Scheduler::thread_count() <= count_before, "finished threads get reaped");
}

} // namespace

void run_scheduler_selftests()
{
    s_checks_run = 0;
    s_checks_failed = 0;

    test_scheduler();

    if (s_checks_failed == 0) {
        klog(LOG_INFO, "selftest", "%zu scheduler checks passed", s_checks_run);
    } else {
        klog(LOG_ERROR, "selftest", "%zu of %zu scheduler checks FAILED", s_checks_failed, s_checks_run);
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
