/*
 * tlb.c — cross-CPU TLB invalidation (SMP roadmap §9.3 step 2).
 *
 * See iris/tlb.h for which CPUs need telling and why that set is small.
 */

#include <iris/tlb.h>
#include <iris/smp.h>
#include <iris/cpu_local.h>
#include <iris/lapic.h>
#include <iris/paging.h>
#include <iris/task.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/spinlock.h>
#include <stdatomic.h>

/*
 * ONE request at a time, serialised by a lock.
 *
 * A per-CPU request slot would let several shootdowns run concurrently, and it
 * would be the right answer if this were hot.  It is not: an unmap is rare
 * compared to a map, and the alternative costs a mailbox per CPU plus the
 * reasoning about a CPU handling two requests at once.  One lock, held for the
 * length of an IPI round trip, is the version whose correctness is obvious —
 * and obvious is what a mechanism nobody can test yet should be.
 */
static irq_spinlock_t     tlb_lock;
static _Atomic uint64_t   tlb_req_va;
static _Atomic uint32_t   tlb_req_pending;   /* bitmask of CPUs yet to ack */
static _Atomic uint32_t   tlb_ipi_count;
static _Atomic uint32_t   tlb_max_targets;

uint32_t tlb_shootdown_count(void) {
    return atomic_load_explicit(&tlb_ipi_count, memory_order_relaxed);
}
uint32_t tlb_shootdown_max_targets(void) {
    return atomic_load_explicit(&tlb_max_targets, memory_order_relaxed);
}

void tlb_init(void) {
    irq_spinlock_init(&tlb_lock);
}

/*
 * The handler.  Runs with interrupts off on the target CPU.
 *
 * It invalidates unconditionally rather than re-checking which address space
 * it is in: by the time it runs it may have switched away, and an `invlpg` for
 * an address it no longer maps costs one instruction and is always safe.  The
 * check that matters — "is this CPU in the address space being changed" — was
 * made by the REQUESTER, which is the only place it can be made without a
 * second round trip.
 */
void tlb_shootdown_ipi(void) {
    uint64_t va = atomic_load_explicit(&tlb_req_va, memory_order_acquire);
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
    /*
     * Release: the requester's acquire on `pending` reaching zero must not be
     * allowed to observe the clear before the invalidation that precedes it.
     * On x86 the store is already ordered; the annotation is what makes the
     * requirement survive a reader who does not know that.
     */
    atomic_fetch_and_explicit(&tlb_req_pending,
                              ~(1u << cpu_self()->cpu_id),
                              memory_order_release);
}

void tlb_shootdown_page(struct KVSpace *vs, uint64_t va) {
    if (!vs) return;

    /*
     * Who is IN this address space right now?  Nobody else, on a machine with
     * one CPU — which is why this returns before taking the lock and why the
     * single-core path costs a loop over MAX_CPUS pointer comparisons and
     * nothing more.
     */
    uint32_t self = cpu_self()->cpu_id;
    uint32_t targets = 0u;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (i == self) continue;
        /* Did it arrive?  This used to ask whether the CPU had a run queue,
         * which stopped being the question when every processor's queue began
         * to be built at boot (SMP roadmap §9.3 step 4) — a queue has to exist
         * before its processor does, because a thread can be homed to a core
         * that has not started yet.  A processor that never arrived also has a
         * `lapic_id` of zero, which is the BOOT processor's on most machines,
         * so an IPI aimed at it would be a self-IPI this then spins waiting for
         * an answer to. */
        if (!smp_is_online(i)) continue;
        struct task *t = cpu_local[i].current_task;
        if (t && t->vspace == vs) targets |= (1u << i);
    }
    if (targets == 0u) return;

    uint64_t flags = irq_spinlock_lock(&tlb_lock);

    atomic_store_explicit(&tlb_req_va, va, memory_order_relaxed);
    atomic_store_explicit(&tlb_req_pending, targets, memory_order_release);

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (!(targets & (1u << i))) continue;
        lapic_send_ipi((uint8_t)cpu_local[i].lapic_id, TLB_SHOOTDOWN_IPI_VECTOR);
        atomic_fetch_add_explicit(&tlb_ipi_count, 1u, memory_order_relaxed);
    }

    /*
     * Wait.  There is no timeout and no fallback, deliberately: a CPU that
     * does not answer a shootdown is a CPU that is wedged with interrupts off,
     * and continuing past it would mean freeing memory it can still write.
     * Hanging here is a worse-looking failure and a better one.
     */
    while (atomic_load_explicit(&tlb_req_pending, memory_order_acquire) != 0u)
        __asm__ volatile ("pause");

    {
        uint32_t n = 0u, m = targets;
        while (m) { n += (m & 1u); m >>= 1; }
        uint32_t hw = atomic_load_explicit(&tlb_max_targets, memory_order_relaxed);
        while (n > hw &&
               !atomic_compare_exchange_weak_explicit(&tlb_max_targets, &hw, n,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) { }
    }

    irq_spinlock_unlock(&tlb_lock, flags);
}
