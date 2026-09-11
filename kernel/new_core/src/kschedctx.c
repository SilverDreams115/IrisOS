#include <iris/nc/kschedctx.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kschedctx_live;
static _Atomic uint32_t kschedctx_hwm;
static _Atomic uint32_t kschedctx_retyped;
static _Atomic uint32_t kschedctx_destroyed;

/* Phase 17/S2 — live KSchedContext count + diagnostics.  Exposed via the
 * SYS_SCHED_INFO ext2 tier (T123) and SYS_UNTYPED_QUERY kind 4 (Phase S2). */
uint32_t kschedctx_live_count(void) {
    return atomic_load_explicit(&kschedctx_live, memory_order_relaxed);
}

void kschedctx_stats(uint32_t *live, uint32_t *hwm,
                     uint32_t *retyped, uint32_t *destroyed) {
    if (live)      *live      = atomic_load_explicit(&kschedctx_live,      memory_order_relaxed);
    if (hwm)       *hwm       = atomic_load_explicit(&kschedctx_hwm,       memory_order_relaxed);
    if (retyped)   *retyped   = atomic_load_explicit(&kschedctx_retyped,   memory_order_relaxed);
    if (destroyed) *destroyed = atomic_load_explicit(&kschedctx_destroyed, memory_order_relaxed);
}

static void kschedctx_live_inc(void) {
    uint32_t n = atomic_fetch_add_explicit(&kschedctx_live, 1u, memory_order_relaxed) + 1u;
    atomic_fetch_add_explicit(&kschedctx_retyped, 1u, memory_order_relaxed);
    uint32_t hw = atomic_load_explicit(&kschedctx_hwm, memory_order_relaxed);
    while (n > hw &&
           !atomic_compare_exchange_weak_explicit(&kschedctx_hwm, &hw, n,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) { }
}

static void kschedctx_obj_close(struct KObject *obj) {
    (void)obj;
    /* No tasks to wake: the bound task holds a ref; releasing sched_ctx drops
     * the refcount and triggers destroy. */
}

/* Phase S2: the ONLY storage path is untyped-backed — the payload returns to
 * its source region (kslab is never involved). */
static void kschedctx_obj_destroy_ut(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kschedctx_live, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&kschedctx_destroyed, 1u, memory_order_relaxed);
    struct KSchedContext *sc = (struct KSchedContext *)obj;
    kuntyped_release_child(obj, kschedctx_bytes(sc->refill_max));
}

static const struct KObjectOps kschedctx_ops_ut = {
    .close   = kschedctx_obj_close,
    .destroy = kschedctx_obj_destroy_ut,
};

struct KSchedContext *kschedctx_alloc_at(void *mem, uint32_t refill_max) {
    if (!mem) return 0;
    if (refill_max == 0u) refill_max = KSCHEDCTX_REFILL_DEFAULT;
    struct KSchedContext *sc = (struct KSchedContext *)mem;
    sc->refill_max       = refill_max;
    sc->refill_head      = 0;
    sc->refill_count     = 0;
    sc->consumed_run     = 0;
    sc->consume_start    = 0;
    kobject_init(&sc->base, KOBJ_SCHED_CONTEXT, &kschedctx_ops_ut);
    irq_spinlock_init(&sc->lock);
    /* Phase S2 (B2): a freshly retyped SC is born UNCONFIGURED and unbound —
     * explicitly invalid budget until SC_CONFIGURE. */
    sc->budget_ticks     = 0;
    sc->period_ticks     = 0;
    sc->remaining_budget = 0;
    sc->bound_task       = 0;
    sc->configured       = 0;
    kschedctx_live_inc();
    return sc;
}

void kschedctx_close(struct KSchedContext *sc) {
    if (!sc) return;
    kobject_release(&sc->base);
}

/* Defined below with the rest of the replenishment family; declared here
 * because `configure` resets the ring while already holding the lock. */
static void kschedctx_refill_reset_locked(struct KSchedContext *sc);

iris_error_t kschedctx_configure(struct KSchedContext *sc,
                                   uint64_t budget, uint64_t period) {
    /* Phase S2 (fix I1): budget <= period, MCS style.  budget == period is a
     * full (100%) CPU reservation: valid.  The scheduler decrements remaining
     * per tick and refills at the period boundary; with budget==period the
     * thread never exhausts its budget within a period, which is exactly the
     * behavior of a full reservation — it breaks no invariant. */
    if (!sc || budget == 0 || period == 0 || budget > period)
        return IRIS_ERR_INVALID_ARG;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    sc->budget_ticks     = budget;
    sc->period_ticks     = period;
    sc->remaining_budget = budget;
    sc->configured       = 1;
    /* Stage 8-mcs: reconfiguring discards the old replenishment schedule —
     * those entries were denominated in the OLD period and would hand back
     * time on a cadence nobody asked for.  The new budget is available now,
     * which is what the invariant sum == budget_ticks requires with an empty
     * queue. */
    kschedctx_refill_reset_locked(sc);   /* the lock is already held here */
    irq_spinlock_unlock(&sc->lock, flags);
    return IRIS_OK;
}

/*
 * Phase S2 (B4): one-to-one binding.  Fails IRIS_ERR_BUSY if the SC is already
 * bound to a different task; idempotent if bound to the same task.  Atomic
 * under the SC lock (the caller must ensure the task↔SC pointers are set
 * consistently — sys_thread_set_sc does).
 */
iris_error_t kschedctx_bind(struct KSchedContext *sc, struct task *t) {
    if (!sc || !t) return IRIS_ERR_INVALID_ARG;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    if (sc->bound_task && sc->bound_task != t) {
        irq_spinlock_unlock(&sc->lock, flags);
        return IRIS_ERR_BUSY;
    }
    sc->bound_task = t;
    irq_spinlock_unlock(&sc->lock, flags);
    return IRIS_OK;
}

void kschedctx_unbind(struct KSchedContext *sc, struct task *t) {
    if (!sc) return;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    if (sc->bound_task == t || t == 0)
        sc->bound_task = 0;
    irq_spinlock_unlock(&sc->lock, flags);
}

/* ── Stage 8-mcs: sporadic replenishment ─────────────────────────────────
 *
 * The invariant every function here maintains:
 *
 *     remaining_budget + consumed_run + sum(pending refills) == budget_ticks
 *
 * `consumed_run` is the part that has been spent but not yet turned into a
 * pending refill, which happens when the thread stops running.  Keeping it
 * separate is what lets a whole run cost ONE queue entry instead of one per
 * tick, and it is why a queue of 8 is enough for budgets far larger than 8.
 */

/*
 * THE REPLENISHMENT STATE TAKES THE LOCK THE OBJECT ALREADY HAD.
 *
 * `sc->lock` existed and three functions used it — configure, bind, unbind.
 * The sporadic-replenishment family did not, and it is the half that a
 * RUNNING system touches: the tick charges, the dispatcher flushes, the idle
 * walk applies.  Three call sites on one core cannot interleave; on two they
 * are three CPUs writing one ring buffer.
 *
 * The split into `_locked` inner functions is forced rather than stylistic:
 * `kschedctx_configure` already holds the lock when it resets the ring, and
 * these spinlocks are not reentrant (see nc/spinlock.h).  A single locking
 * version would deadlock the configure path against itself on the first call.
 */
static void kschedctx_refill_reset_locked(struct KSchedContext *sc) {
    sc->refill_head   = 0;
    sc->refill_count  = 0;
    sc->consumed_run  = 0;
    sc->consume_start = 0;
}

void kschedctx_refill_reset(struct KSchedContext *sc) {
    if (!sc) return;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    kschedctx_refill_reset_locked(sc);
    irq_spinlock_unlock(&sc->lock, flags);
}

int kschedctx_charge_tick(struct KSchedContext *sc, uint64_t now) {
    if (!sc) return 0;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    if (sc->remaining_budget > 0) {
        if (sc->consumed_run == 0) sc->consume_start = now;
        sc->remaining_budget--;
        sc->consumed_run++;
    }
    int exhausted = (sc->remaining_budget == 0);
    irq_spinlock_unlock(&sc->lock, flags);
    return exhausted;
}

void kschedctx_flush_run(struct KSchedContext *sc) {
    if (!sc) return;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    if (sc->consumed_run == 0) { irq_spinlock_unlock(&sc->lock, flags); return; }

    uint64_t due = sc->consume_start + sc->period_ticks;
    uint64_t amt = sc->consumed_run;
    sc->consumed_run  = 0;
    sc->consume_start = 0;
    /* A-28: what was SPENT, as distinct from what is owed back. */
    sc->consumed_total += amt;

    if (sc->refill_count < sc->refill_max) {
        uint32_t idx = (sc->refill_head + sc->refill_count) % sc->refill_max;
        sc->refills[idx].at     = due;
        sc->refills[idx].amount = amt;
        sc->refill_count++;
        irq_spinlock_unlock(&sc->lock, flags);
        return;
    }

    /*
     * Queue full — merge into the NEWEST entry, taking the later due time.
     *
     * Deliberately conservative: merging delays part of a replenishment, so
     * the thread gets its budget back late.  It can never get it back early,
     * which is the direction that would break the guarantee.  A thread only
     * reaches this by blocking and resuming more than eight times inside one
     * period, and the cost of that is a slightly later refill rather than a
     * lost one.
     */
    uint32_t last = (sc->refill_head + sc->refill_count - 1u) % sc->refill_max;
    if (due > sc->refills[last].at) sc->refills[last].at = due;
    sc->refills[last].amount += amt;
    irq_spinlock_unlock(&sc->lock, flags);
}

int kschedctx_apply_refills(struct KSchedContext *sc, uint64_t now) {
    if (!sc) return 0;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    int gained = 0;
    while (sc->refill_count > 0 && sc->refills[sc->refill_head].at <= now) {
        sc->remaining_budget += sc->refills[sc->refill_head].amount;
        sc->refill_head = (sc->refill_head + 1u) % sc->refill_max;
        sc->refill_count--;
        gained = 1;
    }
    /* Never above the configured budget: the invariant says the parts sum to
     * budget_ticks, and this is the belt-and-braces that keeps a bug in one of
     * the parts from turning into extra time. */
    if (sc->remaining_budget > sc->budget_ticks)
        sc->remaining_budget = sc->budget_ticks;
    irq_spinlock_unlock(&sc->lock, flags);
    return gained;
}
