#include "scheduler_priv.h"
#include <iris/smp.h>
#include <iris/panic.h>
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
#include <iris/nc/kfault.h>
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

/*
 * The tick counters, and why `volatile` was never the right word.
 *
 * `volatile` says "re-read this from memory", which is what a one-core kernel
 * needed from it: the idle loop had to see a counter the interrupt handler
 * bumped.  It says nothing about atomicity and nothing about ordering, so the
 * moment a second CPU can bump or read one, it is a claim the type does not
 * make.  On x86-64 an aligned 64-bit access happens to be atomic anyway, which
 * is exactly what makes this the kind of thing that survives review and then
 * is wrong somewhere else.
 *
 * Relaxed ordering is deliberate and sufficient: nothing is published THROUGH
 * these counters.  A reader wants a number that is not torn and not stale
 * forever; it does not want a happens-before edge, and paying for one on every
 * tick would be paying for a guarantee no reader uses.
 */
_Atomic uint64_t scheduler_ticks = 0;

static inline uint64_t sched_ticks_load(void) {
    return atomic_load_explicit(&scheduler_ticks, memory_order_relaxed);
}

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
static _Atomic uint64_t wall_ticks = 0;

/*
 * sched_handle_idle — fast-forward clock when the idle task is current and no
 * non-idle task is runnable.  Advances scheduler_ticks to the nearest
 * REPLENISHMENT so a budget-exhausted thread becomes READY even when the timer
 * ISR does not fire (QEMU TCG: no IRQs delivered during ring-0 spin).
 *
 * Ledger A-24: what is left here is MCS accounting and nothing else.  It used
 * to fast-forward to the nearest SLEEPING thread's deadline too, and to time
 * out blocked IPC waits — the kernel keeping a list of who wanted to be woken
 * when, on their behalf.  That is a service now, so the only deadline the
 * scheduler still knows about is when a scheduling context gets its budget
 * back, which is a fact about the budget rather than about anybody's patience.
 *
 * On return *out_chosen is set to the first runnable non-idle task found after
 * the fast-forward, or remains NULL if none.
 */
static void sched_handle_idle(struct task *idle, struct task **out_chosen) {
    /* Phase S2 Step C: iterate the registry, not the raw array — a TCB's
     * identity is its registry reference, never a position in tasks[]. */
    /* Fast-forward clock to nearest deadline so timed tasks wake even with no IRQs.
     *
     * Both walks below hold `sched_list_lock` (§9.1 rank 5): a thread can be
     * unlinked by a termination on another CPU while this one is following
     * `sched_next`, and the unlink resets the very field the walk is standing
     * on.  `task_wakeup` inside the second walk takes the run queue (rank 7),
     * which is down the order and therefore allowed. */
    uint64_t min_wake = UINT64_MAX;
    uint64_t lf = irq_spinlock_lock(&sched_list_lock);
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
    /*
     * Advance to the deadline — a monotonic MAX, not a store.
     *
     * It was `if (min_wake > ticks) ticks = min_wake;`, which is a read, a
     * decision and a write with the clock free to move between them.  With one
     * idle CPU there is exactly one fast-forwarder and the race has nobody to
     * lose to; with two, one can rewind the other's advance.  The compare and
     * the exchange become one act, and the loop retries against whatever the
     * clock has actually become.
     *
     * And it is now done only by the core that owns the timer (§9.3 step 4).
     * Making the exchange atomic keeps two fast-forwarders from corrupting the
     * counter; it does not make it RIGHT for three idle processors to drag the
     * machine's clock forward while a fourth is running a thread whose budget
     * is measured in it.  Advancing time is the timekeeper's act, exactly as
     * the tick's own counter is.
     */
    if (min_wake != UINT64_MAX && smp_is_timekeeper()) {
        uint64_t cur = sched_ticks_load();
        while (min_wake > cur &&
               !atomic_compare_exchange_weak_explicit(&scheduler_ticks, &cur,
                                                      min_wake,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) { }
    }

    /* Wake any thread whose BUDGET came back and enqueue it. */
    for (struct task *t = sched_thread_list; t; t = t->sched_next) {
        if (t == idle) continue;
        if (t->state == TASK_BUDGET_EXHAUSTED && t->sched_ctx &&
            kschedctx_apply_refills(t->sched_ctx, sched_ticks_load())) {
            /* Stage 8-mcs: woken by a REPLENISHMENT coming due, not by a
             * period-boundary reset.  The thread gets back exactly what it
             * spent, one period after it spent it. */
            t->wake_tick = 0;
            task_wakeup(t);
        }
    }
    irq_spinlock_unlock(&sched_list_lock, lf);
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
    struct task *t = task_current();
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
 * `outgoing` is no longer touched here at all.  Its FPU image was saved by the
 * pick, which is also where this core let go of it — by the time control
 * reaches this function another processor may already be running it, and a
 * write here would be a write to somebody else's thread.
 */
__attribute__((noreturn))
void sched_resume(struct task *next, struct task *outgoing) {
    (void)outgoing;   /* its FPU was saved by the pick, before it was released */
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

    /*
     * The outgoing thread's FPU, saved HERE and not in `sched_resume`.
     *
     * Two reasons, and the second is why it moved.  It has to happen before
     * this core lets go of the thread (`on_cpu`, below), and a core that finds
     * nothing to run never calls `sched_resume` at all — so a thread that
     * parked when the machine had nothing else to do had its SSE registers
     * saved by nobody.
     *
     * `outgoing` may be NULL: the dispatcher was entered with no thread to
     * account for, which is what an application processor's first pass is.
     */
    if (outgoing) fpu_save_to(outgoing->fpu_state);

    reap_pending_dead_task();

    if (outgoing && outgoing->timeout_pending) {
        struct task *tf = outgoing;
        tf->timeout_pending = 0;
        if (ktimeout_notify_fault(tf)) {
            tf->state = TASK_BLOCKED_FAULT;
        } else if (tf->sched_ctx) {
            tf->state     = TASK_BUDGET_EXHAUSTED;
            tf->wake_tick = sched_ticks_load() + tf->sched_ctx->period_ticks;
        }
    }

    if (outgoing) {
        if (outgoing->sched_ctx) kschedctx_flush_run(outgoing->sched_ctx);
        if (outgoing->state == TASK_RUNNING) {
            outgoing->state = TASK_READY;
            if (outgoing != sched_idle_thread) rq_enqueue(outgoing);
        }
        if (outgoing->state == TASK_DEAD) reap_enqueue_dead(outgoing);

        /*
         * Let it go — and this is the LAST thing done to it, deliberately.
         *
         * Everything above writes the outgoing thread: its FPU image, its
         * scheduling context, its state, its place in a run queue.  From this
         * store on, another processor may pick it up and resume it, so
         * anything added below this line would be a write to a thread somebody
         * else is running.  See `on_cpu` in task.h for what that costs.
         */
        atomic_store_explicit(&outgoing->on_cpu, 0u, memory_order_release);
    }

    struct task *chosen = rq_dequeue_best();
    if (!chosen) {
        sched_handle_idle(sched_idle_thread, &chosen);
        if (!chosen) return 0;
    }

    /*
     * Wait for the core that had it to finish with it.
     *
     * A thread reaches a run queue the moment its wait is satisfied, and the
     * core it was running on may still be several hundred instructions from
     * being done with it — that gap is exactly what `on_cpu` names.  Almost
     * always the flag is already clear and this is one load.
     *
     * It cannot deadlock, and the reason is that the spin holds NOTHING: the
     * run-queue lock was released by `rq_dequeue_best` before this line, and
     * the core being waited for needs no lock this one holds.  It is bounded
     * by that core's remaining release path, which runs with interrupts off
     * and does not block.
     */
    while (atomic_load_explicit(&chosen->on_cpu, memory_order_acquire))
        __asm__ volatile ("pause");
    atomic_store_explicit(&chosen->on_cpu, 1u, memory_order_relaxed);

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

/* ── the domain schedule ─────────────────────────────────────────────────
 *
 * FIXED, like seL4's.  There is no invocation that edits it, and that is the
 * design rather than an omission: a schedule somebody can influence is a
 * schedule that carries information, which is the one thing a time partition
 * exists to prevent.  What ring 3 can do is place a THREAD in a domain
 * (`Domain_Set`), gated by its own boot capability.
 *
 * The default is one entry — all of the CPU, to domain 0, for ever — so a
 * system that configures nothing dispatches exactly as it did before domains
 * existed.  seL4 ships CONFIG_NUM_DOMAINS = 1 for the same reason.
 */
static const struct iris_dom_slot dom_schedule[] = {
    { 0u, 0u },   /* ticks == 0 means "until something else happens": the
                   * single-domain default never advances the schedule at all,
                   * so the tick does no work and no switch is ever counted. */
};
/*
 * The schedule's CURSOR, and the current domain, are two different kinds of
 * shared state and get two different treatments.
 *
 * `iris_cur_domain` is READ by every CPU's dispatcher, on every dispatch, to
 * pick which set of run queues to look at.  It is one byte and it is hot, so
 * it is an atomic with relaxed ordering: nothing is published through it — the
 * queues it selects have their own lock — and a reader wants a value that is
 * not torn, not a happens-before edge it would never use.
 *
 * The cursor (`dom_sched_idx`, `dom_ticks_left`) is different: advancing it is
 * a read-modify-write across two variables, and it must happen ONCE per tick
 * however many CPUs are ticking.  Two CPUs each decrementing `dom_ticks_left`
 * would end a domain's slot at twice the rate the schedule says — the
 * partition would still be a partition, and it would not be the one anybody
 * wrote down.  So the cursor takes a lock and the tick that loses the race
 * does nothing, which is exactly right: the slot has already been advanced.
 */
static uint32_t dom_sched_idx;
static uint32_t dom_ticks_left;
static irq_spinlock_t dom_lock;          /* §9.1 rank 7 — guards the cursor */
_Atomic uint8_t iris_cur_domain;
static _Atomic uint64_t dom_switches;

uint32_t sched_domain_current(void) {
    return (uint32_t)atomic_load_explicit(&iris_cur_domain, memory_order_relaxed);
}
uint64_t sched_domain_switches(void) {
    return atomic_load_explicit(&dom_switches, memory_order_relaxed);
}

/*
 * One tick of the schedule.  Returns 1 when the domain CHANGED, which the
 * caller turns into a reschedule — the outgoing domain's thread must stop
 * running immediately, not at the end of its own quantum, or the partition
 * would be a suggestion.
 */
int sched_domain_tick(void) {
    const uint32_t n = (uint32_t)(sizeof(dom_schedule) / sizeof(dom_schedule[0]));
    /* A schedule of one entry with no length is the default: nothing to
     * advance, and the branch costs one comparison on every tick — taken
     * before the lock, so the default configuration never contends. */
    if (n <= 1u && dom_schedule[0].ticks == 0u) return 0;

    uint64_t df = irq_spinlock_lock(&dom_lock);
    if (dom_ticks_left > 0u) dom_ticks_left--;
    if (dom_ticks_left > 0u) { irq_spinlock_unlock(&dom_lock, df); return 0; }

    uint8_t prev = (uint8_t)atomic_load_explicit(&iris_cur_domain,
                                                 memory_order_relaxed);
    dom_sched_idx = (dom_sched_idx + 1u) % n;
    uint8_t next  = dom_schedule[dom_sched_idx].domain;
    dom_ticks_left = dom_schedule[dom_sched_idx].ticks;
    atomic_store_explicit(&iris_cur_domain, next, memory_order_relaxed);
    irq_spinlock_unlock(&dom_lock, df);

    if (next != prev) {
        atomic_fetch_add_explicit(&dom_switches, 1u, memory_order_relaxed);
        return 1;
    }
    return 0;
}

void scheduler_init(void) {
    irq_spinlock_init(&dom_lock);
    task_init();
}

/*
 * ── The tick, on a machine with more than one processor (§9.3 step 4) ───────
 *
 * It was one function, and it could be: one processor, one timer, one thread
 * running.  Four processors split it in two along a line that was always
 * there but never had to be drawn — what is true of the MACHINE and what is
 * true of a CORE.
 *
 * The clock, the domain schedule and the replenishment sweep are facts about
 * the machine.  Running them on every core would make time pass four times as
 * fast, advance the domain schedule four times per tick, and have four cores
 * walk the thread list to wake the same sleeper.  They belong to whoever owns
 * the timer, and that is the boot processor: IRQ0 comes from the PIT through
 * the PIC, and the PIC delivers to one core.
 *
 * Charging the running thread's budget, noticing a higher-priority thread has
 * become runnable, and counting down a time slice are facts about a core, and
 * every core has to do its own — a core whose thread is never charged never
 * preempts it.
 *
 * ── Why the PIT and an IPI, and not a timer per core ────────────────────────
 *
 * The alternative is the local APIC timer, one per core, which is what seL4
 * uses on x86 and what this should eventually be.  It is not what this is, for
 * one reason worth stating plainly: a per-core timer has to be CALIBRATED, and
 * four independently calibrated timers give four slightly different ideas of
 * how long a tick is — while `kschedctx_charge_tick` takes the tick NUMBER as
 * its argument and MCS deadlines are expressed in it.  One timer and an IPI
 * has one clock by construction.  The cost is three interrupts per tick that a
 * local timer would not need, which at 100 Hz is three hundred a second: real,
 * bounded, and the thing to fix when there is a reason to.
 */

/*
 * The half that belongs to the machine.  Called by the processor that owns the
 * timer, once per tick, and by nobody else.
 */
static void sched_tick_global(void) {
    atomic_fetch_add_explicit(&scheduler_ticks, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&wall_ticks,       1u, memory_order_relaxed);

    /* The domain schedule advances on the same tick that drives preemption,
     * and a domain change forces a reschedule: the outgoing domain's thread
     * must stop running now, not at the end of its own quantum, or the
     * partition would be a suggestion rather than a boundary.
     *
     * The reschedule is forced on EVERY core, not on this one: the domain is
     * machine-wide, so a thread of the outgoing domain running on CPU 2 has to
     * stop for exactly the same reason this core's does. */
    if (sched_domain_tick()) {
        for (uint32_t c = 0; c < MAX_CPUS; c++) {
            struct task *ct = cpu_local[c].current_task;
            if (ct) ct->need_resched = 1;
        }
        smp_reschedule_others();
    }

    /*
     * O(N) replenishment scan — Phase 1 TODO:
     *   Replace with a min-heap keyed on the next replenishment.  Current
     *   complexity: O(live threads) per tick — the walk is a list now, so it
     *   costs what the system actually has rather than a fixed 256.  The shape
     *   that removes it is seL4's release queue, ordered by release time, where
     *   the tick looks at the head and stops.
     *
     *   Ledger A-24: this scan used to look at SLEEPING threads too, and there
     *   is no such state any more — the kernel does not hold anybody's deadline
     *   but a scheduling context's.
     *
     *   SMP concern: this loop runs under CLI on the IRQ-handling CPU only.  On
     *   SMP, threads homed to other CPUs can have a replenishment fall due here,
     *   and task_wakeup sends an IPI to the home CPU — correct but wastes IRQ
     *   budget.  A per-CPU timer wheel removes the cross-CPU IPI.
     */
    uint64_t tf = irq_spinlock_lock(&sched_list_lock);
    for (struct task *t = sched_thread_list; t; t = t->sched_next) {
        /* Ph75: refill budget for exhausted tasks whose period has elapsed */
        if (t->state == TASK_BUDGET_EXHAUSTED &&
            t->wake_tick != 0 &&
            t->wake_tick <= sched_ticks_load()) {
            if (t->sched_ctx)
                t->sched_ctx->remaining_budget = t->sched_ctx->budget_ticks;
            t->wake_tick = 0;
            task_wakeup(t);
        }
    }
    irq_spinlock_unlock(&sched_list_lock, tf);
}

/*
 * The half that belongs to a core.  Every processor runs this on every tick —
 * the one that owns the timer from its own ISR, the others from the tick IPI.
 *
 * Everything it touches belongs to this core's thread, so there is no lock:
 * `need_resched`, `ticks_left` and the scheduling context of a RUNNING thread
 * are written only by the core running it.  `rq_top_priority` reads this
 * core's run queue under that queue's own lock.
 */
static void sched_tick_local(void) {
    struct task *current_task = task_current();

    if (current_task == sched_idle_thread)
        cpu_self()->idle_ticks++;

    if (!current_task) return;

    /* Ph75 / Stage 8-mcs: charge one tick to the running thread's SC.  The
     * charge is recorded with the tick it happened on, so the replenishment it
     * earns comes due exactly one period later. */
    if (current_task->sched_ctx && current_task->state == TASK_RUNNING) {
        struct KSchedContext *sc = current_task->sched_ctx;
        if (kschedctx_charge_tick(sc, sched_ticks_load())) {
            /*
             * Stage 8-mcs — a TIMEOUT FAULT, when one is armed.
             *
             * Without a handler the thread simply blocks until a
             * replenishment falls due.  Nobody is told, so no principal can
             * react to a thread overrunning — which is the gap that makes
             * budget enforcement not yet MCS.
             *
             * With a handler, the temporal supervisor is told and decides.
             * The delivery does NOT happen here: an endpoint call dequeues a
             * receiver and touches the run queue, and this runs in the PIT
             * ISR.  The tick records the fact; task_yield acts on it, the same
             * discipline the timed-block path above already follows.
             */
            if (current_task->timeout_ep) {
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

/*
 * What the timer ISR calls on the processor the PIT interrupts.  Both halves,
 * then the other processors are told the tick happened.
 */
void scheduler_tick(void) {
    reap_pending_dead_task();
    sched_tick_global();
    sched_tick_local();
    smp_tick_others();
}

/* What the tick IPI calls on every other processor: its own half, and nothing
 * of the machine's. */
void scheduler_tick_remote(void) {
    sched_tick_local();
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
    return sched_ticks_load();
}

uint64_t sched_wall_ticks(void) {
    return atomic_load_explicit(&wall_ticks, memory_order_relaxed);
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
