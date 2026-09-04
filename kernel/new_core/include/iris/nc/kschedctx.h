#ifndef IRIS_NC_KSCHEDCTX_H
#define IRIS_NC_KSCHEDCTX_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

#define KSCHEDCTX_DEFAULT_BUDGET  5u   /* 50ms at 100Hz */
#define KSCHEDCTX_DEFAULT_PERIOD  20u  /* 200ms at 100Hz */

struct task;

/*
 * Stage 8-mcs — SPORADIC REPLENISHMENT.
 *
 * The model before this was a leaky bucket that only refilled when empty:
 * `remaining_budget` was reset to the full budget in exactly one place, the
 * BUDGET_EXHAUSTED branch.  A thread that blocked before exhausting carried the
 * remainder forward for ever, so a server handling a request in 2 of its 5
 * ticks and then waiting on its endpoint kept 3, then 1, then stalled for a
 * whole period — its real bandwidth fell the more often it blocked, which is
 * the opposite of what a budget is supposed to guarantee.  That is neither
 * periodic nor sporadic.
 *
 * seL4's answer, and now this one: every tick consumed comes back exactly one
 * PERIOD after it was spent.  A replenishment queue records {when, how much},
 * and the invariant that makes it a guarantee is
 *
 *     remaining_budget + sum(pending refills) == budget_ticks
 *
 * so a thread can never hold more than its budget, and can never spend more
 * than `budget` in any window of `period` — which is the bandwidth isolation
 * the old model silently did not provide.
 */
#define KSCHEDCTX_REFILL_MAX  8u

struct KRefill {
    uint64_t at;      /* tick at which `amount` becomes available again */
    uint64_t amount;  /* ticks */
};

struct KSchedContext {
    struct KObject  base;              /* must be first */
    irq_spinlock_t  lock;
    uint64_t        budget_ticks;      /* configured ticks per period */
    uint64_t        period_ticks;      /* period length in ticks */
    uint64_t        remaining_budget;  /* ticks available RIGHT NOW */
    /* Phase S2: one-to-one binding (S2.9).  bound_task != NULL ⇒ this SC is
     * bound to exactly that task; cleared on unbind / task death. */
    struct task    *bound_task;
    uint8_t         configured;        /* Phase S2: 0 until SC_CONFIGURE (B2) */

    /* Pending replenishments, ordered oldest-first in a ring. */
    struct KRefill  refills[KSCHEDCTX_REFILL_MAX];
    uint32_t        refill_head;
    uint32_t        refill_count;
    /* The run being accounted: how much has been consumed and when that
     * consumption started.  Flushed into a refill when the thread stops
     * running, so one blocked-and-resumed thread costs one queue entry rather
     * than one per tick. */
    uint64_t        consumed_run;
    uint64_t        consume_start;
};

/* Charge one tick of execution at `now`.  Returns 1 if the budget is now
 * exhausted (caller suspends the thread), 0 otherwise. */
int  kschedctx_charge_tick(struct KSchedContext *sc, uint64_t now);
/* The thread stopped running: turn what it consumed into a replenishment due
 * one period after the consumption began.  Idempotent when nothing was
 * consumed, so it is safe to call on every context switch. */
void kschedctx_flush_run(struct KSchedContext *sc);
/* Apply every replenishment now due.  Returns 1 if budget became available
 * that was not available before (caller may wake an exhausted thread). */
int  kschedctx_apply_refills(struct KSchedContext *sc, uint64_t now);
/* Reset the queue to "the whole budget is available now" — configure time. */
void kschedctx_refill_reset(struct KSchedContext *sc);

/* Phase S2: Untyped retype is the ONLY creation path (kslab kschedctx_alloc
 * retired; SYS_SC_CREATE returns NOT_SUPPORTED). */
struct KSchedContext *kschedctx_alloc_at(void *mem); /* untyped-backed */
void                  kschedctx_close(struct KSchedContext *sc);
iris_error_t          kschedctx_configure(struct KSchedContext *sc,
                                           uint64_t budget, uint64_t period);
/* Phase S2: bind/unbind a task (one-to-one, atomic). */
iris_error_t          kschedctx_bind(struct KSchedContext *sc, struct task *t);
void                  kschedctx_unbind(struct KSchedContext *sc, struct task *t);
/* Phase 17/S2: live count + high-water/retype/destroy diagnostics. */
uint32_t              kschedctx_live_count(void);
void                  kschedctx_stats(uint32_t *live, uint32_t *hwm,
                                      uint32_t *retyped, uint32_t *destroyed);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KSCHEDCTX_H */
