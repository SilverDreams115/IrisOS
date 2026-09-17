/*
 * smp.c — starting application processors (§9.3 step 3) and handing them the
 * dispatcher (§9.3 step 4).
 *
 * See iris/smp.h for the shape of both steps.
 */

#include <iris/smp.h>
#include <iris/acpi.h>
#include <iris/lapic.h>
#include <iris/cpu_local.h>
#include <iris/paging.h>
#include <iris/boot_info.h>
#include <iris/pmm.h>
#include <iris/klog.h>
#include <iris/gdt.h>
#include <iris/idt.h>
#include <iris/syscall.h>
#include <iris/scheduler.h>
#include <stdatomic.h>

extern char ap_trampoline_start[], ap_trampoline_end[];
extern char tramp_gdtr[], tramp_cr3[], tramp_stack[], tramp_entry[];
extern char tramp_progress[], tramp_pm_ptr[], tramp_lm_ptr[];
extern char tramp_cr4[], tramp_cr0[];
extern char tramp_pm[], tramp_lm[];

extern uint64_t core_stack_top_for(uint32_t cpu_id);

static _Atomic uint32_t smp_online = 1u;   /* the BSP is running this */
static uint32_t         smp_progress_last;

/*
 * Which processors can be sent an IPI, as a bitmask of cpu ids.
 *
 * Not "everything below smp_online_count()": the count says how many arrived,
 * and the cpu ids come from the MADT's order, so a machine where the third
 * entry fails to start has two processors online with ids 0 and 1 — or 0 and
 * 2.  A mask records which, and it is set by the arriving processor itself,
 * which is the only code that knows for certain that it got here.
 */
static _Atomic uint32_t smp_ipi_mask = 1u;   /* the BSP */

/* How many IPIs this kernel has sent for each purpose.  Relaxed and
 * independent: they are evidence, not synchronisation.  The tick count is what
 * makes "the other processors are being told the time" checkable from ring 3
 * — the alternative is inferring it from threads making progress, which is a
 * much weaker statement about a much later effect. */
static _Atomic uint32_t smp_tick_ipis;
static _Atomic uint32_t smp_resched_ipis;

uint32_t smp_tick_ipi_count(void) {
    return atomic_load_explicit(&smp_tick_ipis, memory_order_relaxed);
}
uint32_t smp_reschedule_ipi_count(void) {
    return atomic_load_explicit(&smp_resched_ipis, memory_order_relaxed);
}

/*
 * How many processors have actually DISPATCHED a thread.
 *
 * Not the same question as how many are online, and it is the one step 4 is
 * about: an AP that arrived, took its GDT and halted was online too.  Every
 * dispatch bumps its core's `context_switches`, so a core that has ever run a
 * thread is a core with a non-zero count, and counting those counts the
 * processors that are doing the work rather than the ones that answered the
 * roll call.
 */
uint32_t smp_dispatching_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        if (cpu_local[i].context_switches != 0u) n++;
    return n;
}

static void smp_send_others(uint8_t vector) {
    uint32_t mask = atomic_load_explicit(&smp_ipi_mask, memory_order_acquire);
    if ((mask & (mask - 1u)) == 0u) return;   /* only one processor: nobody to tell */
    uint32_t self = cpu_self()->cpu_id;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (i == self || !(mask & (1u << i))) continue;
        lapic_send_ipi((uint8_t)cpu_local[i].lapic_id, vector);
    }
}

int  smp_is_timekeeper(void)      { return cpu_self()->cpu_id == 0u; }

int smp_is_online(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return 0;
    return (atomic_load_explicit(&smp_ipi_mask, memory_order_acquire)
            & (1u << cpu_id)) != 0u;
}

void smp_tick_others(void) {
    if (smp_online_count() <= 1u) return;
    atomic_fetch_add_explicit(&smp_tick_ipis, 1u, memory_order_relaxed);
    smp_send_others(SCHED_TICK_IPI_VECTOR);
}

void smp_reschedule_others(void) {
    if (smp_online_count() <= 1u) return;
    atomic_fetch_add_explicit(&smp_resched_ipis, 1u, memory_order_relaxed);
    smp_send_others(RESCHEDULE_IPI_VECTOR);
}

/*
 * Poke ONE processor.
 *
 * Every cross-CPU reschedule goes through here rather than reaching for
 * `lapic_send_ipi` directly, and the reason is the counter: two call sites
 * were sending this IPI by hand — a wakeup whose thread lives on another core,
 * and a suspend or kill of a thread another core is running — so a gauge that
 * counted only the broadcast form reported zero on a machine sending hundreds.
 * A number that is structurally unable to see the common case is worse than no
 * number, because something will eventually assert on it.
 */
void smp_send_reschedule(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return;
    if (cpu_id == cpu_self()->cpu_id) return;
    if (!(atomic_load_explicit(&smp_ipi_mask, memory_order_acquire) & (1u << cpu_id)))
        return;
    atomic_fetch_add_explicit(&smp_resched_ipis, 1u, memory_order_relaxed);
    lapic_send_ipi((uint8_t)cpu_local[cpu_id].lapic_id, RESCHEDULE_IPI_VECTOR);
}

uint32_t smp_online_count(void)  { return atomic_load_explicit(&smp_online, memory_order_acquire); }
uint32_t smp_last_progress(void) { return smp_progress_last; }

/*
 * The GDT the trampoline uses, built in the low page beside the code.
 *
 * Three entries and they are the minimum: a 32-bit code segment to leave real
 * mode through, a 32-bit data segment for the interval, and a 64-bit code
 * segment to land in.  The kernel's real GDT is loaded by `gdt_init_ap` once
 * there is C running; this one exists for the twenty instructions in between,
 * and lives in the low page because that is the only memory the processor can
 * address before paging.
 */
struct tramp_gdt_entry { uint16_t limit_lo, base_lo; uint8_t base_mid, access, gran, base_hi; }
    __attribute__((packed));

/*
 * Where the GDT sits in the page, and why it is not just "after the code".
 *
 * The first version put it at offset 0x20 — which is 32 bytes in, INSIDE the
 * trampoline's real-mode entry sequence.  The APs ran the first few
 * instructions and then executed a descriptor table, which is a failure with
 * no symptom beyond a processor that never arrives.  A fixed offset well past
 * the code, asserted against the code's actual length, is the version that
 * cannot drift when the assembly grows.
 */
#define TRAMP_GDT_OFF 0x800u

static void tramp_gdt_build(uint8_t *page) {
    struct tramp_gdt_entry *g = (struct tramp_gdt_entry *)(page + TRAMP_GDT_OFF);
    /* 0: null */
    g[0] = (struct tramp_gdt_entry){0, 0, 0, 0, 0, 0};
    /* 1 (0x08): 32-bit code, ring 0, 4 GiB */
    g[1] = (struct tramp_gdt_entry){0xFFFF, 0, 0, 0x9A, 0xCF, 0};
    /* 2 (0x10): 32-bit data, ring 0, 4 GiB */
    g[2] = (struct tramp_gdt_entry){0xFFFF, 0, 0, 0x92, 0xCF, 0};
    /* 3 (0x18): 64-bit code, ring 0 (L bit in the granularity byte) */
    g[3] = (struct tramp_gdt_entry){0, 0, 0, 0x9A, 0x20, 0};
}

uint32_t smp_start_aps(const struct iris_boot_info *bi) {
    uint32_t n = acpi_cpu_count();
    if (n <= 1u) return smp_online_count();
    if (!lapic_is_active()) {
        klog_write("[IRIS][SMP] no LAPIC; staying uniprocessor\n");
        return smp_online_count();
    }

    /*
     * The trampoline page is ALLOCATED, not chosen.
     *
     * The first version read the memory map for a usable page below 1 MiB —
     * and the PMM initialises from that same map, so the page it picked was
     * one the allocator had already handed to somebody.  Copying the
     * trampoline over it corrupted live memory and hung the boot with no
     * output, which is what a page chosen rather than claimed will always
     * eventually do.
     */
    (void)bi;
    uint64_t page_phys = pmm_alloc_page_below(0x100000ull);
    if (!page_phys) {
        klog_write("[IRIS][SMP] no free page below 1 MiB for the trampoline\n");
        return smp_online_count();
    }

    uint8_t *page = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(page_phys);
    uint64_t code_len = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    if (code_len > TRAMP_GDT_OFF) {
        /* The GDT would land inside the code.  Refusing beats starting
         * processors that will execute a descriptor table. */
        klog_write("[IRIS][SMP] trampoline outgrew its page layout\n");
        return smp_online_count();
    }
    for (uint64_t i = 0; i < code_len; i++) page[i] = (uint8_t)ap_trampoline_start[i];

    tramp_gdt_build(page);

    /* Offsets are computed by subtraction, so the assembly is free to move its
     * parameters without anything here needing to know. */
    uint64_t off_gdtr = (uint64_t)(tramp_gdtr     - ap_trampoline_start);
    uint64_t off_cr3  = (uint64_t)(tramp_cr3      - ap_trampoline_start);
    uint64_t off_sp   = (uint64_t)(tramp_stack    - ap_trampoline_start);
    uint64_t off_ent  = (uint64_t)(tramp_entry    - ap_trampoline_start);
    uint64_t off_prog = (uint64_t)(tramp_progress - ap_trampoline_start);
    uint64_t off_pmp  = (uint64_t)(tramp_pm_ptr   - ap_trampoline_start);
    uint64_t off_lmp  = (uint64_t)(tramp_lm_ptr   - ap_trampoline_start);
    uint64_t off_pm   = (uint64_t)(tramp_pm       - ap_trampoline_start);
    uint64_t off_lm   = (uint64_t)(tramp_lm       - ap_trampoline_start);
    uint64_t off_cr4  = (uint64_t)(tramp_cr4      - ap_trampoline_start);
    uint64_t off_cr0  = (uint64_t)(tramp_cr0      - ap_trampoline_start);

    /* The GDTR: limit and the LINEAR base of the table we just built. */
    *(uint16_t *)(page + off_gdtr)     = (uint16_t)(4u * 8u - 1u);
    *(uint32_t *)(page + off_gdtr + 2) = (uint32_t)(page_phys + TRAMP_GDT_OFF);

    /* The two far jumps take their destination as an immediate, which is the
     * one thing a base register cannot solve. */
    *(uint32_t *)(page + off_pmp) = (uint32_t)(page_phys + off_pm);
    *(uint32_t *)(page + off_lmp) = (uint32_t)(page_phys + off_lm);

    *(uint64_t *)(page + off_cr3) = pml4_get_current() & PAGE_PA_MASK;
    *(uint64_t *)(page + off_ent) = (uint64_t)(uintptr_t)&ap_main;

    /*
     * What kind of processor this machine's processors are — read off the one
     * that is already running, and read HERE rather than written as constants.
     *
     * By this point the BSP's CR4 carries everything the firmware left (SSE
     * through OSFXSR above all) and everything the kernel added (SMEP, SMAP,
     * PCIDE).  An AP out of INIT has none of it, and the trampoline copies
     * these two registers verbatim so there is exactly one description of the
     * machine's configuration rather than two that must be kept in step.
     */
    {
        uint64_t cr4, cr0;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        *(uint64_t *)(page + off_cr4) = cr4;
        *(uint64_t *)(page + off_cr0) = cr0;
    }

    /*
     * The page has to be EXECUTABLE, and only now.
     *
     * Physical 0..2 MiB is mapped NX because no kernel code runs there — and
     * the trampoline is the exception that cannot be anywhere else.  The
     * instruction right after `mov cr0` (enabling paging) fetches from this
     * linear address, so an NX mapping faults a processor that has no IDT
     * yet: total silence, which is exactly what it gave until the trampoline
     * started writing to COM1.
     */
    if (paging_set_low_exec(page_phys, 1) != 0) {
        klog_write("[IRIS][SMP] cannot make the trampoline page executable\n");
        return smp_online_count();
    }

    uint8_t vector = (uint8_t)(page_phys >> 12);
    uint8_t self   = (uint8_t)cpu_self()->lapic_id;

    for (uint32_t i = 0; i < n && i < MAX_CPUS; i++) {
        const struct iris_cpu_desc *d = acpi_cpu(i);
        if (!d || !d->enabled || d->lapic_id == self) continue;

        uint32_t cpu_id = i;
        /*
         * One at a time.  The trampoline page holds ONE processor's stack and
         * entry point, so two APs starting together would race for the same
         * stack — and serialising bring-up costs microseconds once, at boot,
         * against a class of bug that only appears on machines with enough
         * cores to hit it.
         */
        *(uint64_t *)(page + off_sp)   = core_stack_top_for(cpu_id);
        *(uint64_t *)(page + off_prog) = 0;
        atomic_thread_fence(memory_order_release);

        uint32_t before = smp_online_count();

        lapic_send_init(d->lapic_id);
        /* The specification asks for a pause after INIT; a spin on the local
         * tick is not available this early, so this is a delay loop and is
         * labelled as one rather than dressed up. */
        for (volatile uint32_t w = 0; w < 200000u; w++) { }

        lapic_send_startup(d->lapic_id, vector);
        for (volatile uint32_t w = 0; w < 200000u; w++) { }

        /* Some firmware needs the second STARTUP; sending it to a processor
         * that already started is harmless, because it is already out of its
         * wait-for-SIPI state and ignores it. */
        if (smp_online_count() == before) {
            lapic_send_startup(d->lapic_id, vector);
        }

        /* Bounded wait.  A processor that does not arrive is left alone and
         * reported — the alternative is a boot that hangs because one core of
         * sixteen is wedged, which trades a degraded system for no system. */
        for (uint32_t spin = 0; spin < 20000000u; spin++) {
            if (smp_online_count() != before) break;
            __asm__ volatile ("pause");
        }

        if (smp_online_count() == before) {
            smp_progress_last = (uint32_t)*(uint64_t *)(page + off_prog);
            klog_write("[IRIS][SMP] cpu ");
            klog_write_dec(cpu_id);
            klog_write(" did not start (trampoline progress ");
            klog_write_dec(smp_progress_last);
            klog_write(")\n");
        }
    }

    /* Close the window.  The processors are up and nothing executes there
     * again; leaving it open would be an executable page at physical zero for
     * the life of the system, to run one page of code once. */
    (void)paging_set_low_exec(page_phys, 0);

    klog_write("[IRIS][SMP] processors online: ");
    klog_write_dec(smp_online_count());
    klog_write("\n");
    return smp_online_count();
}

/*
 * Where an application processor arrives, in 64-bit long mode on its own
 * stack.  It never returns.
 *
 * Step 3 stopped here with a `cli; hlt`.  Step 4 gives it the rest of what a
 * processor needs and then hands it the dispatcher — the SAME dispatcher the
 * boot processor runs, on this core's own stack, picking from this core's own
 * run queue.  There is no AP-specific scheduler and no AP idle loop; if there
 * were, "the scheduler" would be two things that have to agree.
 *
 * The order below is not arbitrary.  Each line is something that must be true
 * before the next can be:
 *
 *   gdt_init_ap        this core's GDT, TSS and GS base.  Everything after
 *                      this reads %gs, including cpu_self().
 *   idt_load_ap        the IDT register is per-processor even though the table
 *                      is not.  Until this, any fault here is a triple fault.
 *   lapic_software_enable  a LAPIC that is not software-enabled accepts no
 *                      IPIs — so the tick would never arrive.
 *   syscall_init       LSTAR/STAR/SFMASK are per-processor MSRs.  Without
 *                      them a ring-3 thread on this core would `syscall` into
 *                      whatever address zero happens to be.
 *   core_dispatch_init TSS.RSP0 and the syscall stack pointer, both this
 *                      core's own stack.  The dispatcher reads it GS-relative
 *                      and has no fallback.
 *
 * Only then does it announce itself, and announcing is the last thing before
 * the dispatcher: the BSP's bring-up loop waits on that count and starts the
 * next processor when it moves, so a core that announced and then failed to
 * reach the dispatcher would be counted as running and never run anything.
 * There is nothing between the two but the store.
 *
 * Interrupts are still off here.  They are enabled by `core_dispatch` itself,
 * in the `sti; hlt` it uses when there is nothing to run — which means this
 * core takes its first interrupt only once it is inside the dispatcher and
 * able to answer it.
 */
void ap_main(void) {
    uint32_t cpu_id = 0;
    uint8_t  id     = lapic_id();
    for (uint32_t i = 0; i < acpi_cpu_count(); i++) {
        const struct iris_cpu_desc *d = acpi_cpu(i);
        if (d && d->lapic_id == id) { cpu_id = i; break; }
    }

    gdt_init_ap(cpu_id);
    cpu_local[cpu_id].lapic_id = id;

    idt_load_ap();
    if (lapic_is_active()) lapic_software_enable();
    syscall_init();
    core_dispatch_init();

    atomic_fetch_or_explicit(&smp_ipi_mask, 1u << cpu_id, memory_order_release);
    atomic_fetch_add_explicit(&smp_online, 1u, memory_order_release);

    /* Nothing is running on this core, so there is no outgoing thread and no
     * FPU state to save. */
    core_dispatch(0);
}
