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

uint64_t core_stack_top_for(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return 0;
    return (uint64_t)(uintptr_t)(core_stacks[cpu_id] + CORE_STACK_BYTES);
}

void core_dispatch_init(void) {
    struct iris_cpu_local *cl = cpu_self();
    cl->core_stack_top = core_stack_top_for(cl->cpu_id);
}

/*
 * The dispatcher.  Entered on the core stack by core_dispatch_enter, with the
 * stack reset under it; never returns.
 *
 * `outgoing` is the thread that just gave the CPU up, and it is here for one
 * reason: its FPU state has to be saved before another thread's is loaded.
 * Everything else about it — where it was in the kernel, what it had on the
 * stack — is gone by construction, which is the property step 2 established
 * and this depends on.
 *
 * The idle case is a `hlt`, not a task.  IRIS used to make the boot thread the
 * idle task and yield to it, which is why the scheduler had a special case for
 * "the idle task is current" and why a per-thread stack could not go: idle was
 * a thread with a stack like any other.  A core with nothing to run has
 * nothing to run; it waits for an interrupt on the stack it already has.
 */
void core_dispatch(struct task *outgoing) {
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
        outgoing = 0;    /* its FPU was saved on the way in, once */
    }
}
