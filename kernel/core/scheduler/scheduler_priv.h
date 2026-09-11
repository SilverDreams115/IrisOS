#ifndef IRIS_SCHED_PRIV_H
#define IRIS_SCHED_PRIV_H

/*
 * scheduler_priv.h — internal declarations shared across the three scheduler
 * translation units: task_lifecycle.c and scheduler_core.c (scheduler.c).
 *
 * Nothing in this header is part of the public kernel API.  Include
 * <iris/task.h> or <iris/scheduler.h> from external callers instead.
 *
 * SMP readiness: irq_spinlock_t is used for PMM, futex, and klog.
 * cpu_local[cpu_id].current_task is kept in sync with current_task on every
 * context switch.  IA32_GS_BASE is wired to &cpu_local[0] by gdt_init() (Phase 2:
 * ring-0 always has GS_BASE = &cpu_local[cpu_id]; cpu_self() safe everywhere).
 * AP bringup and LAPIC timer are deferred; infrastructure is correct for BSP.
 */

#include <iris/task.h>
#include <iris/cpu_local.h>
#include <iris/nc/spinlock.h>
#include <iris/domain.h>
#include <stdint.h>
#include <stdatomic.h>

/*
 * CpuRunQueue — per-CPU O(1) priority run queue.
 * Each CPU owns one; tasks are assigned to a CPU at creation (home_cpu).
 * Cross-CPU wakeup enqueues into the target CPU's queue then sends an IPI.
 */
struct CpuRunQueue {
    irq_spinlock_t lock;
    /* Phase S2 Inc.2B: pointer-based FIFO per priority.  The parallel
     * index-keyed arrays (next[TASK_MAX]/queued[TASK_MAX]) are retired — the
     * per-task FIFO link and queued flag live inside struct task (rq_next /
     * rq_queued), so the run queue no longer derives identity from a static
     * array position.
     *
     * A queue PER DOMAIN, which is what makes the domain scheduler O(1): the
     * dispatcher searches the current domain's 256 priorities and never looks
     * at another domain's threads at all.  seL4 indexes one flat array by
     * `domain * NUM_PRIORITIES + prio` for the same reason.  Searching one set
     * of queues and skipping threads of the wrong domain would be the same
     * answer at O(threads), and it would make the cost of running a domain
     * depend on how many threads the OTHER domains have — which is exactly the
     * leak a time partition exists to close. */
    struct task   *head[IRIS_NUM_DOMAINS][256];
    struct task   *tail[IRIS_NUM_DOMAINS][256];
    uint64_t       mask[IRIS_NUM_DOMAINS][4];  /* bit p set ↔ prio-p non-empty */
};

/* ── the domain schedule (seL4's ksDomSchedule) ──────────────────────────── */

struct iris_dom_slot {
    uint8_t  domain;
    uint32_t ticks;      /* how long this domain owns the CPU */
};

/* The current domain, and how many ticks it has left.  Read by the dispatcher,
 * advanced by the tick. */
extern _Atomic uint8_t iris_cur_domain;  /* relaxed; see scheduler.c */

/* Advance the schedule by one tick; returns 1 if the DOMAIN changed, which the
 * tick turns into a reschedule. */
int  sched_domain_tick(void);
/* The live schedule, for diagnostics and for the tests that drive it. */
uint32_t sched_domain_current(void);
uint64_t sched_domain_switches(void);

/* ── Shared state (defined in task_lifecycle.c) ──────────────────────────── */

/*
 * Phase S2 Inc.2 (Step C) — KTCB registry.
 *
 * The registry is a table of REFERENCES (pointer + generation + flags), NOT of
 * TCB payload.  It is the sole iteration/allocation/lookup surface for the
 * scheduler; nothing derives a TCB's identity from an array position anymore.
 *
 * TRANSITIONAL: during Step C the `tcb` pointers still target the static
 * `tasks[]` backing (scaffolding kept only to keep boot green while consumers
 * migrate).  Step D re-points them at Untyped-carved KTCB objects and deletes
 * the static payload — a localized change, because every consumer already goes
 * through `ktcb_registry[i].tcb`.
 *
 * generation bumps on every slot reset so a stale index/token is detectable;
 * it never substitutes for capability authority.
 */
/*
 * The scheduler's list of live threads (ledger A-19).
 *
 * It was `ktcb_registry[TASK_MAX]`, an array of identity slots — and that made
 * TASK_MAX a ceiling on how many threads may exist, which the kernel had no
 * business deciding.  Everything that read it was walking it: three sweeps
 * looking for a thread whose sleep or replenishment is due.  A walk wants a
 * list, a list has no ceiling, and removal is O(1) because the links are in
 * the TCB.
 *
 * The `generation` field went with the array.  It existed so a stale index
 * could be detected, and there are no indices; a TCB is a capability, and a
 * capability's own lifetime already answers "is this still the thread you
 * meant".
 */
extern struct task        *sched_thread_list;   /* head; NULL before init */
/* Guards the list above (SMP roadmap §9.1, rank 5).  IRQ-off: the TICK walks
 * the list, so a plain spinlock would let an interrupt deadlock its own CPU
 * against the dispatcher's walk. */
extern irq_spinlock_t      sched_list_lock;
extern struct task         ktcb_backing[TASK_BOOTSTRAP_MAX]; /* idle + root task */
extern struct task        *current_task;
extern struct task        *task_list_head;
extern struct task        *task_list_tail;
extern _Atomic uint32_t    next_id;
/* Phase S2: task_rsp[TASK_MAX] retired — saved kernel RSP lives in
 * struct task.saved_krsp (scheduler indirection: no index-keyed parallel
 * array, no (t - tasks) pointer arithmetic to reach it). */
extern uint64_t            kernel_cr3;
extern uint8_t             initial_fpu_state[512];

/*
 * set_current_task — update both the global current_task and the per-CPU
 * cpu_local.current_task.  Must only be called from post-SWAPGS ring-0 context
 * (syscall entry or ISR entry) where GS_BASE = &cpu_local[cpu_id].
 *
 * Boot-time task_init() sets the idle task directly via cpu_local[0] array
 * access (pre-SWAPGS) and does NOT call this helper.
 */
static inline void set_current_task(struct task *t) {
    current_task = t;
    cpu_self()->current_task = t;
}

/* Scheduler tick counter (defined in scheduler.c) */
extern _Atomic uint64_t scheduler_ticks;  /* relaxed; see scheduler.c */

/* ── Architecture helpers ────────────────────────────────────────────────── */

extern void user_entry_trampoline(void);

extern void context_switch(struct cpu_context *old,
                            struct cpu_context *new,
                            uint64_t *old_rsp,
                            uint64_t  new_rsp,
                            uint8_t  *old_fpu,
                            uint8_t  *new_fpu);

/* kstack.c is GONE (ledger D-1, step 3).  A thread owned two pages of kernel
 * stack plus a guard page and every kernel entry landed on them; entries land
 * on the CORE's stack now, so there was a file, an allocator, a free, a fatal
 * reporter and a reserved virtual region left over with nothing calling any of
 * them. */

/* ── task_lifecycle.c (cross-file helpers) ───────────────────────────────── */

void task_init_fpu_state(struct task *t);
void task_reset_slot(struct task *t);
void unlink_task(struct task *t);
void reap_enqueue_dead(struct task *t);
void reap_pending_dead_task(void);

/* ── O(1) run queue (defined in task_lifecycle.c) ────────────────────────── */

extern _Atomic uint32_t  sched_live_count;  /* live (non-dead) task count */

void         rq_enqueue(struct task *t);
void         rq_remove(struct task *t);
struct task *rq_dequeue_best(void);
int          rq_top_priority(void);

#endif /* IRIS_SCHED_PRIV_H */
