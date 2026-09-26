/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core_dispatch.c — the per-core dispatcher (Stage 9-evt, step 3).
 *
 * ── What this is for ────────────────────────────────────────────────────────
 *
 * seL4 has ONE kernel stack per core and no thread ever blocks inside the
 * kernel.  IRIS reached the second half of that first: since step 2 a syscall
 * that cannot finish ABANDONS its frame and resumes on a fresh stack, so a
 * blocked thread's kernel stack holds nothing.  What was left was the first
 * half — the stack still belonged to the thread rather than to the core, and
 * the reason it had to was that two paths still handed the CPU away with a
 * live C frame underneath them: the voluntary yield, and the timer interrupt.
 *
 * The dispatcher is what replaces both.  It runs on the core's own stack, and
 * every entry to it RESETS that stack — which is what makes "abandon the
 * frame" and "give the stack back" the same act rather than two that have to
 * agree.
 *
 * ── Why it is a loop and not a function ─────────────────────────────────────
 *
 * A thread is resumed in one of two shapes and only one of them returns:
 *
 *   · it was preempted in RING 3, so its whole register state is in its TCB
 *     (step 3's first half) and resuming it is an iretq — which never comes
 *     back here;
 *   · it is resuming in the KERNEL — a syscall that parked and must re-run
 *     from its restart trampoline, or a thread that has never run and must be
 *     launched into ring 3.  Those run ON THIS STACK, and when they park they
 *     re-enter the dispatcher, which resets the stack under them.
 *
 * So the loop's next iteration is reached either by a park or by nothing being
 * runnable.  It is not a function that returns a task; a dispatcher that
 * returned would need somewhere to return TO, and that somewhere is the stack
 * this exists to stop needing.
 */
#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/cpu_local.h>
#include <iris/klog.h>
#include <iris/panic.h>
#include <iris/tss.h>
#include <iris/syscall.h>
#include "scheduler_priv.h"

/*
 * The core stacks.  One page each, which is what a kernel that keeps nothing
 * across a block actually needs: the deepest path is a syscall handler and its
 * callees, and if that ever needs more than a page the answer is to find out
 * why rather than to widen it quietly.
 */
#define CORE_STACK_BYTES 4096u
static uint8_t core_stacks[MAX_CPUS][CORE_STACK_BYTES]
    __attribute__((aligned(16)));

/*
 * The canary at the bottom of each core stack.
 *
 * These stacks are a plain .bss array, so core N's grows down into core N-1's
 * — into the live end of it.  An overflow therefore does not fault; it
 * silently rewrites another processor's return addresses, which is a worse
 * outcome than a crash and an invisible one.  Nothing below a stack can be
 * unmapped cheaply here because the kernel image is mapped with 2 MiB pages.
 *
 * So the lowest eight bytes of every stack hold a known value, and the
 * dispatcher checks its own before doing anything else.  The core that
 * overflowed writes through its OWN canary first, on its way out of its
 * region, so the panic names the core that did it and not the victim.
 *
 * This is the "find out why" the comment above asks for: it does not widen
 * the stack and it does not make an overflow survivable.  It makes one
 * announce itself instead of being absorbed by a neighbour.
 */
#define CORE_STACK_CANARY 0x5354414b47554152ull   /* "STAKGUAR" */

static uint64_t *core_stack_canary(uint32_t cpu_id) {
    return (uint64_t *)(void *)core_stacks[cpu_id];
}

static void core_stack_canary_arm(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return;
    *core_stack_canary(cpu_id) = CORE_STACK_CANARY;
}

static void core_stack_canary_check(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return;
    if (*core_stack_canary(cpu_id) != CORE_STACK_CANARY)
        iris_panic("core stack overflow: this core wrote past the bottom of "
                   "its kernel stack and into the next core's");
}

uint64_t core_stack_top_for(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return 0;
    return (uint64_t)(uintptr_t)(core_stacks[cpu_id] + CORE_STACK_BYTES);
}

void core_dispatch_init(void) {
    struct iris_cpu_local *cl = cpu_self();
    uint64_t top = core_stack_top_for(cl->cpu_id);
    cl->core_stack_top = top;
    core_stack_canary_arm(cl->cpu_id);
    /*
     * Every entry from ring 3 lands here from now on — the syscall path reads
     * `%gs:48` and the CPU reads TSS.RSP0, and both are this, for the life of
     * the core.  They used to be rewritten on every context switch, because
     * the stack belonged to whichever thread was about to run.
     */
    tss_set_rsp0(top);
    syscall_set_kstack(top);
}

/*
 * The dispatcher.  Entered on the core stack by core_dispatch_enter, with the
 * stack reset under it; never returns.
 *
 * `outgoing` is the thread that just gave the CPU up, and it is here so that
 * `sched_pick_for_dispatch` can finish with it: save its FPU state, flush its
 * scheduling context, put it back in a queue if it is still runnable, and only
 * then release it to the other processors.  Everything else about it — where
 * it was in the kernel, what it had on the stack — is gone by construction,
 * which is the property step 2 established and this depends on.
 *
 * The idle case is a `hlt`, not a task.  IRIS used to make the boot thread the
 * idle task and yield to it, which is why the scheduler had a special case for
 * "the idle task is current" and why a per-thread stack could not go: idle was
 * a thread with a stack like any other.  A core with nothing to run has
 * nothing to run; it waits for an interrupt on the stack it already has.
 */
void core_dispatch(struct task *outgoing) {
    /* Entered with the stack reset, so whatever the last syscall did to it is
     * finished and its damage, if any, is measurable right here. */
    core_stack_canary_check(cpu_self()->cpu_id);
    for (;;) {
        struct task *next = sched_pick_for_dispatch(outgoing);
        if (next) sched_resume(next, outgoing);   /* never returns */

        /*
         * Nothing runnable.  Enable interrupts and wait for one — a timer that
         * wakes a sleeper, or a device that signals a notification.  `sti`
         * takes effect after the NEXT instruction, so the pair cannot race:
         * an interrupt that arrives between them is taken after the hlt is
         * entered, not before, and the hlt cannot be slept through.
         */
        sched_idle_account();
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
        /* The pick above already finished with it and released it to the other
         * processors; passing it again would be this core writing a thread
         * another core may by now be running. */
        outgoing = 0;
    }
}
