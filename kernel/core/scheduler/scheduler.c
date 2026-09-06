#include "scheduler_priv.h"
#include <iris/panic.h>
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kschedctx.h>
#include <iris/nc/kvspace.h>
#include <stdatomic.h>
#include <stdint.h>

/*
 * scheduler.c — scheduler loop: yield, tick, sleep, diagnostics.
 *
 * Task creation/teardown lives in task_lifecycle.c.
 * Kernel stack management lives in kstack.c.
 */

volatile uint64_t scheduler_ticks = 0;

/*
 * Phase 17 — scheduling-decision counter (additive instrumentation, exposed
 * via the SYS_SCHED_INFO ext2 tier).  A strictly-monotonic progress signal
 * used by the T119/T122 selftests to prove cooperative tasks actually reach
 * the scheduler (no lost/stuck worker).  It never influences scheduling.
 *
 * It counted `task_yield()` entries until Stage 9-evt step 3, when a yield
 * stopped being how a thread reaches the scheduler: SYS_YIELD parks and the
 * DISPATCHER makes the decision, on the core's stack.  Counting both entry
 * points keeps the signal meaning what the tests read it as — "the scheduler
 * ran" — rather than "a particular function was called", which is what made
 * it break when the function stopped being the one.
 */
static _Atomic uint32_t sched_yield_ctr;

uint32_t sched_yield_count(void) {
    return atomic_load_explicit(&sched_yield_ctr, memory_order_relaxed);
}
/* wall_ticks is incremented only by the real PIT ISR path (scheduler_tick).
 * Unlike scheduler_ticks it is never fast-forwarded by the idle-loop clock
 * workaround, so it reflects real elapsed time and is used by SYS_CLOCK_GET. */
static volatile uint64_t wall_ticks = 0;

/*
 * sched_handle_idle — fast-forward clock when the idle task is current and no
 * non-idle task is runnable.  Advances scheduler_ticks to the nearest sleeping
 * task's wake_tick so timed-out tasks become READY even when the timer ISR does
 * not fire (QEMU TCG: no IRQs delivered during ring-0 spin).
 *
 * On return *out_chosen is set to the first runnable non-idle task found after
 * the fast-forward, or remains NULL if none.
 */
static void sched_handle_idle(struct task *idle, struct task **out_chosen) {
    /* Phase S2 Step C: iterate the registry, not the raw array — a TCB's
     * identity is its registry reference, never a position in tasks[]. */
    /* Fast-forward clock to nearest deadline so timed tasks wake even with no IRQs. */
    uint64_t min_wake = UINT64_MAX;
    for (struct task *t = sched_thread_list; t; t = t->sched_next) {
        if (t->wake_tick != 0 && t->wake_tick < min_wake)
            min_wake = t->wake_tick;
        /*
         * Stage 8-mcs: a budget-exhausted thread is woken by a REPLENISHMENT
         * falling due, and a replenishment has no wake_tick.  Without this it
         * would never be a fast-forward target, so on a tickless-looking guest
         * (QEMU TCG delivers no IRQs while ring 0 spins) an exhausted thread
         * would sleep for ever and the system would look wedged.
         */
        if (t->state == TASK_BUDGET_EXHAUSTED && t->sched_ctx &&
            t->sched_ctx->refill_count > 0) {
            uint64_t due = t->sched_ctx->refills[t->sched_ctx->refill_head].at;
            if (due < min_wake) min_wake = due;
        }
    }
    if (min_wake != UINT64_MAX && min_wake > scheduler_ticks)
        scheduler_ticks = min_wake;

    /* Wake any tasks whose deadlines passed and enqueue them. */
    for (struct task *t = sched_thread_list; t; t = t->sched_next) {
        if (t == idle) continue;
        if (t->state == TASK_SLEEPING &&
            t->wake_tick != 0 &&
            t->wake_tick <= scheduler_ticks) {
            t->wake_tick = 0;
            task_wakeup(t);
        } else if (t->state == TASK_BUDGET_EXHAUSTED && t->sched_ctx &&
                   kschedctx_apply_refills(t->sched_ctx, scheduler_ticks)) {
            /* Stage 8-mcs: woken by a REPLENISHMENT coming due, not by a
             * period-boundary reset.  The thread gets back exactly what it
             * spent, one period after it spent it. */
            t->wake_tick = 0;
            task_wakeup(t);
        } else if ((t->state == TASK_BLOCKED_IPC ||
                    t->state == TASK_BLOCKED_IRQ) &&
                   t->wake_tick != 0 &&
                   t->wake_tick <= scheduler_ticks) {
            t->timed_out = 1;
            t->wake_tick = 0;
            task_wakeup(t);
        }
    }
    *out_chosen = rq_dequeue_best();
}

/*
 * Stage 9-evt Step 2 — the abandoning park (ledger D-1).
 *
 * `abandon` means: do not preserve this frame.  The outgoing thread is set to
 * resume at syscall_restart_trampoline on a FRESH stack, and the integer
 * context the switch would normally save is thrown away — so the kernel stack
 * of a blocked thread holds nothing, which is the property D-1 exists to get.
 *
 * The FPU state is still saved into the real buffer.  The thread parked with
 * its user's SSE registers live, and losing them would corrupt a computation
 * that merely happened to make a blocking syscall — the frame is disposable,
 * the user's registers are not.
 *
 * `old->ctx.rflags` is set to interrupts-off, which is the state a syscall
 * runs in (SFMASK clears IF at entry).  Resuming the trampoline with interrupts
 * on would let a timer land on a stack that is being rebuilt.
 */
/*
 * task_yield / task_yield_impl are DELETED (Stage 9-evt step 3).
 *
 * They were how a thread reached the scheduler: switch from inside the
 * caller's frame, come back when the thread runs again.  Every caller is now
 * either a park (the syscall layer), a mark (the timer), or a wait (the two
 * kernel-internal notification waits, which had nothing to switch TO), and
 * `context_switch` is on no thread's path at all.
 *
 * What made them impossible to keep is the same thing that made them
 * necessary before: they need a stack to come back to, and a kernel stack
 * belongs to the core now, not to whoever was standing on it.
 */

/* Defined in syscall_dispatch.c; the parked syscall's resume point. */
__attribute__((noreturn)) void syscall_restart_trampoline(void);

/*
 * Stage 9-evt step 3 — parking leaves the thread's stack entirely.
 *
 * Step 2 made the frame disposable; this stops using it at all.  The thread's
 * resume point is recorded in its TCB and the CPU moves to the CORE's stack
 * before a single further decision is made, so from here on nothing that
 * matters is below the old rsp — which is the difference between a stack that
 * is merely unused and one that belongs to the core.
 *
 * It does not return.  The fallback it used to have — "nobody else can run, so
 * yield through your own frame" — is gone, because the dispatcher's answer to
 * "nobody else can run" is to wait for an interrupt on the core's stack, which
 * is what the idle task used to be for.
 */
__attribute__((noreturn)) void task_park_restart(void) {
    struct task *t = current_task;
    if (t) {
        t->kentry      = syscall_restart_trampoline;
        t->resume_user = TASK_RESUME_KERNEL;
    }
    core_dispatch_enter(t);
}

/*
 * Stage 9-evt step 3 — give the CPU to `next`, saving nothing of `outgoing`
 * but its FPU.
 *
 * Two shapes, and which one applies is a fact about how the thread LEFT ring 3
 * rather than a choice made here:
 *
 *   · `resume_user` — it was interrupted in ring 3, so its whole register
 *     state is in its TCB and resuming it is an iretq off the core stack.  No
 *     kernel stack of its own is involved at any point.
 *   · otherwise — it is resuming INSIDE the kernel: a syscall that parked and
 *     must re-run from its restart trampoline.  That is a CALL, on the stack
 *     this is already standing on, which is the core's.
 *
 * `outgoing` may be NULL (the dispatcher was entered with no thread to account
 * for); its FPU is saved because the thread left ring 3 with its user's SSE
 * registers live, and losing them would corrupt a computation that merely
 * happened to be interrupted.
 */
__attribute__((noreturn))
void sched_resume(struct task *next, struct task *outgoing) {
    if (outgoing) fpu_save_to(outgoing->fpu_state);
    fpu_restore_from(next->fpu_state);

    switch (next->resume_user) {
    case TASK_RESUME_USER:
        restore_user_ctx_and_iretq(&next->user_ctx);
    case TASK_RESUME_USER_FIRST:
        start_user_ctx_and_iretq(&next->user_ctx);
    default:
        break;
    }

    /*
     * A kernel resume, called on the stack we are standing on — the core's.
     * It does not return: a restart trampoline either reaches ring 3 or parks,
     * and parking re-enters the dispatcher with the stack reset underneath it.
     */
    IRIS_ASSERT(next->kentry != 0, "sched_resume: kernel resume with no entry");
    next->kentry();
    for (;;) { }
}

/*
 * ── Stage 9-evt step 3: choosing a thread, for the DISPATCHER ───────────────
 *
 * The same decision `task_yield_impl` makes, without the half that assumes a
 * stack to come back to.  It commits — current_task, TSS, the syscall stack
 * pointer and CR3 are all correct by the time it returns — because the
 * dispatcher's next act is to resume the thread and there is nowhere left to
 * put a half-made decision.
 *
 * NULL means the core has nothing to run.  There is no idle TASK here: idle
 * used to be the boot thread, which is why the scheduler had a special case
 * for "the idle task is current" and why per-thread stacks could not go — idle
 * was a thread with a stack like any other.  A core with nothing to run waits
 * on the stack it already has.
 *
 * The clock fast-forward stays, and is not an optimisation: QEMU's TCG
 * delivers no interrupts while ring 0 spins, so a guest with every thread
 * asleep would never be woken by the timer it is waiting for.
 */
struct task *sched_pick_for_dispatch(struct task *outgoing) {
    __asm__ volatile ("cli" : : : "memory");

    atomic_fetch_add_explicit(&sched_yield_ctr, 1u, memory_order_relaxed);

    reap_pending_dead_task();

    if (outgoing && outgoing->timeout_pending) {
        struct task *tf = outgoing;
        tf->timeout_pending = 0;
        if (ktimeout_notify_fault(tf)) {
            tf->state = TASK_BLOCKED_FAULT;
        } else if (tf->sched_ctx) {
            tf->state     = TASK_BUDGET_EXHAUSTED;
            tf->wake_tick = scheduler_ticks + tf->sched_ctx->period_ticks;
        }
    }

    if (outgoing) {
        if (outgoing->sched_ctx) kschedctx_flush_run(outgoing->sched_ctx);
        if (outgoing->state == TASK_RUNNING) {
            outgoing->state = TASK_READY;
            if (outgoing != task_list_head) rq_enqueue(outgoing);
        }
        if (outgoing->state == TASK_DEAD) reap_enqueue_dead(outgoing);
    }

    struct task *chosen = rq_dequeue_best();
    if (!chosen) {
        sched_handle_idle(task_list_head, &chosen);
        if (!chosen) return 0;
    }

    chosen->state        = TASK_RUNNING;
    chosen->ticks_left   = chosen->time_slice;
    chosen->need_resched = 0;
    set_current_task(chosen);
    cpu_self()->context_switches++;

    /*
     * Step 3: the ring-3 entry stack is the CORE's, and it does not change
     * when the thread does — which is the whole of "one kernel stack per
     * core".  TSS.RSP0 and the syscall stack pointer are set once, at
     * core_dispatch_init; what still changes per thread is only its address
     * space.
     */
    syscall_set_user_cr3(chosen->vspace ? chosen->vspace->user_cr3 : 0);

    if (chosen->vspace && chosen->vspace->cr3 != 0) {
        uint64_t cr3 = chosen->vspace->cr3;
        if (iris_pcid_enabled) cr3 |= (uint64_t)chosen->vspace->pcid;
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    } else {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }
    return chosen;
}

void sched_idle_account(void) {
    cpu_self()->idle_ticks++;
}

void scheduler_init(void) {
    task_init();
}

void scheduler_tick(void) {
    reap_pending_dead_task();

    scheduler_ticks++;
    wall_ticks++;
    if (current_task == task_list_head)
        cpu_self()->idle_ticks++;

    /*
     * O(N) timeout scan — Phase 1 TODO:
     *   Replace with a min-heap (binary heap or pairing heap) keyed on wake_tick.
     *   Current complexity: O(live threads) per tick — the walk is a list now,
     *   so it costs what the system actually has rather than a fixed 256.  It
     *   is still a scan; the shape that removes it is seL4's release queue,
     *   ordered by wake time, where the tick looks at the head and stops.
     *
     *   SMP concern: this loop runs under CLI on the IRQ-handling CPU only.  On SMP,
     *   tasks homed to other CPUs can have their wake_tick expire here, but task_wakeup
     *   sends an IPI to the home CPU — correct but wastes IRQ budget.  A per-CPU timer
     *   wheel (one wheel per CPU, drained on that CPU's tick) removes the cross-CPU IPI.
     *
     *   Do NOT restructure this loop as a "shortcut early exit" — tasks[i].wake_tick == 0
     *   is the common case for non-sleeping tasks and the branch predictor handles it well.
     */
    for (struct task *t = sched_thread_list; t; t = t->sched_next) {
        if (t->state == TASK_SLEEPING && t->wake_tick <= scheduler_ticks) {
            t->wake_tick = 0;
            task_wakeup(t);
        }
        /* Ph75: refill budget for exhausted tasks whose period has elapsed */
        if (t->state == TASK_BUDGET_EXHAUSTED &&
            t->wake_tick != 0 &&
            t->wake_tick <= scheduler_ticks) {
            if (t->sched_ctx)
                t->sched_ctx->remaining_budget = t->sched_ctx->budget_ticks;
            t->wake_tick = 0;
            task_wakeup(t);
        }
        /* Timed block expired (channel or notification).
         * spinlock_lock uses CAS without CLI — calling kchannel_cancel_waiter /
         * knotification_cancel_waiter here would deadlock if any task holds
         * live_lock at this IRQ boundary.  We only set the signal; the woken
         * task removes itself from the waiter list in task context. */
        if ((t->state == TASK_BLOCKED_IPC ||
             t->state == TASK_BLOCKED_IRQ) &&
            t->wake_tick != 0 &&
            t->wake_tick <= scheduler_ticks) {
            t->timed_out = 1;
            t->wake_tick = 0;
            task_wakeup(t);
        }
    }

    if (!current_task) return;

    /* Ph75 / Stage 8-mcs: charge one tick to the running thread's SC.  The
     * charge is recorded with the tick it happened on, so the replenishment it
     * earns comes due exactly one period later. */
    if (current_task->sched_ctx && current_task->state == TASK_RUNNING) {
        struct KSchedContext *sc = current_task->sched_ctx;
        if (kschedctx_charge_tick(sc, scheduler_ticks)) {
            /*
             * Stage 8-mcs — a TIMEOUT FAULT, when one is armed.
             *
             * Without a handler the thread simply blocks until a
             * replenishment falls due.  Nobody is told, so no principal can
             * react to a thread overrunning — which is the gap that makes
             * budget enforcement not yet MCS.
             *
             * With a handler, the temporal supervisor is told and decides.
             * The delivery does NOT happen here: signalling a notification
             * wakes a task and touches the run queue, and this runs in the PIT
             * ISR.  The tick records the fact; task_yield acts on it, the same
             * discipline the timed-block path above already follows.
             */
            if (current_task->timeout_notif) {
                current_task->timeout_pending = 1;
                current_task->need_resched    = 1;
                return;
            }
            /* Turn what was just spent into a pending replenishment before
             * parking: the thread is woken by that refill coming due, not by
             * a period-boundary reset that would hand back a full budget it
             * did not earn. */
            kschedctx_flush_run(sc);
            current_task->state        = TASK_BUDGET_EXHAUSTED;
            current_task->wake_tick    = 0;
            current_task->need_resched = 1;
            return;
        }
    }

    /* O(1) preemption check via ready_mask bitmap */
    if (current_task->state == TASK_RUNNING) {
        if (rq_top_priority() > (int)(uint8_t)current_task->priority)
            current_task->need_resched = 1;
    }

    if (current_task->ticks_left > 0)
        current_task->ticks_left--;
    if (current_task->ticks_left == 0)
        current_task->need_resched = 1;
}

/* scheduler_add_task / task_create are DELETED: nothing called them.  A kernel
 * thread built from the static pool was the last non-retyped way to make a
 * thread, and the pool it drew from is the bootstrap pair now. */

/*
 * scheduler_sleep_current is DELETED (Stage 9-evt step 3).
 *
 * It was the shape every blocking path used to have: set a deadline, then
 * yield from inside the caller's frame and return there when the thread woke
 * — so the continuation was "the rest of the caller", on the thread's kernel
 * stack, for the whole sleep.  Both its callers (SYS_SLEEP, then
 * SYS_CLOCK_NANOSLEEP) are restartable now and keep the deadline in the TCB,
 * which leaves this with nothing to do and nowhere to return to.
 */

uint32_t sched_live_task_count(void) {
    return atomic_load_explicit(&sched_live_count, memory_order_relaxed);
}

uint64_t sched_current_ticks(void) {
    return scheduler_ticks;
}

uint64_t sched_wall_ticks(void) {
    return wall_ticks;
}

uint64_t sched_context_switches(void) {
    uint64_t total = 0;
    for (int i = 0; i < MAX_CPUS; i++)
        if (cpu_local[i].rq) total += cpu_local[i].context_switches;
    return total;
}

uint64_t sched_idle_ticks(void) {
    uint64_t total = 0;
    for (int i = 0; i < MAX_CPUS; i++)
        if (cpu_local[i].rq) total += cpu_local[i].idle_ticks;
    return total;
}
