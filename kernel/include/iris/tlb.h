#ifndef IRIS_TLB_H
#define IRIS_TLB_H

/*
 * tlb.h — cross-CPU TLB invalidation (SMP roadmap §9.3 step 2).
 *
 * THE PROBLEM.  CPU A removes a page-table entry for address space X.  Its own
 * `invlpg` reaches its own TLB and nothing else.  If CPU B is running a thread
 * in X, B can keep using the translation A just deleted — reading or writing a
 * frame that has been unmapped and possibly handed to somebody else.  An unmap
 * that does not reach every CPU is not an unmap; it is a suggestion.
 *
 * WHICH CPUs ACTUALLY NEED TELLING, and the answer is smaller than "all".
 *
 * IRIS loads CR3 on every dispatch, and it loads it with bit 63 CLEAR:
 *
 *     mov cr3, <pml4 | pcid>          (scheduler.c)
 *
 * With CR4.PCIDE set, a CR3 write whose bit 63 is clear INVALIDATES every TLB
 * entry for the PCID it is loading.  So a CPU that switches INTO address space
 * X throws away whatever it had cached for X on the way in — for free, on a
 * write it was making anyway.  Without PCID a CR3 write flushes everything,
 * which is the same conclusion more bluntly.
 *
 * That leaves exactly one case needing an IPI: a CPU running a thread in X
 * RIGHT NOW, which will not reload CR3 until it switches away.  Every other
 * CPU is either not going to touch X again, or is going to flush it as it
 * arrives.  So the shootdown is targeted at the CPUs whose `current_task`
 * names X, and on a machine where nobody else is in X it sends nothing.
 *
 * SYNCHRONOUS, and it has to be.  The caller's next act is typically to free
 * the frame or hand the page table back to an Untyped.  Returning before every
 * CPU has dropped the translation would mean a CPU can still write through it
 * into memory that now belongs to somebody else — which is the whole bug,
 * arriving slightly later.
 *
 * WHAT IS NOT TESTED YET.  There is one CPU, so `tlb_shootdown_range` always
 * takes its early return and the cross-CPU path has never executed.  It lands
 * here because it must exist before a second CPU can hold a different CR3
 * (§9.3), and it is exercised in step 3 when there is a second CPU to exercise
 * it with.  Saying so is cheaper than implying coverage that does not exist.
 */

#include <stdint.h>

struct KVSpace;

/* Invalidate `va` in `vs` on every CPU that currently has `vs` loaded, and
 * return only once they have.  The LOCAL invalidation is the caller's — this
 * is the cross-CPU half, so that a caller with nothing to tell pays a
 * comparison and no IPI. */
void tlb_shootdown_page(struct KVSpace *vs, uint64_t va);

/* Vector 0xF1: a CPU has been asked to drop a translation.  Called from the
 * ISR path with interrupts off. */
void tlb_shootdown_ipi(void);
void tlb_init(void);

/* Diagnostics: IPIs sent, and the deepest a requester ever had to wait for
 * acknowledgements (0 on a single-CPU machine, by construction). */
uint32_t tlb_shootdown_count(void);
uint32_t tlb_shootdown_max_targets(void);

#define TLB_SHOOTDOWN_IPI_VECTOR 0xF1u

#endif /* IRIS_TLB_H */
