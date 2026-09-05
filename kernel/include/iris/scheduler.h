#ifndef IRIS_SCHEDULER_H
#define IRIS_SCHEDULER_H

#include <stdint.h>

void     scheduler_init(void);
void     scheduler_tick(void);
void     scheduler_add_task(void (*entry)(void));
void     scheduler_sleep_current(uint64_t ticks);

/*
 * Diagnostics accessors — cheap, read-only, safe to call from syscall context.
 *
 * sched_live_task_count: number of scheduler task slots in any non-DEAD state.
 *   Includes the idle task.  Useful as a coarse live-process indicator.
 *   Cost: O(TASK_MAX) scan.
 *
 * sched_current_ticks: current scheduler tick counter value.
 *   Incremented at TASK_DEFAULT_SLICE Hz; wraps at UINT64_MAX (>5000 years at 100Hz).
 *   Use the low 32 bits for short-lived deltas; use both halves for absolute timestamps.
 */
uint32_t sched_live_task_count(void);
/* Phase 16: high-water depth of the deferred-reap queue.  Monotonic; a value
 * approaching REAP_QUEUE_SIZE would mean the single-CPU "one death per yield"
 * assumption is being violated and dead task slots may leak. */
uint32_t sched_reap_queue_hwm(void);

/*
 * Phase 17 — scheduler hardening instrumentation (all additive, read-only).
 *
 * sched_run_queue_hwm:  high-water of tasks concurrently enqueued in the O(1)
 *   run queue.  Bounds proof for run-queue churn (T120).
 * sched_run_queue_live: tasks currently enqueued (instantaneous).
 * sched_duplicate_enqueue_count: times rq_enqueue's queued[] guard rejected a
 *   re-enqueue of an already-queued task — the counter behind invariant S4
 *   (no task twice in the run queue).  Benign/expected under wakeup races, so
 *   it is bounded, not necessarily zero.
 * sched_yield_count: monotonic count of task_yield() entries — a progress
 *   signal proving cooperative tasks reach the scheduler (T119/T122).
 */
uint32_t sched_run_queue_hwm(void);
uint32_t sched_run_queue_live(void);
uint32_t sched_duplicate_enqueue_count(void);
/* Phase S2 Step C — KTCB registry gauges (references, not payload). */
void     task_registry_stats(uint32_t *active, uint32_t *hwm,
                             uint32_t *exhaustions, uint32_t *gen_mismatch);
uint32_t sched_yield_count(void);
uint64_t sched_current_ticks(void);
uint64_t sched_wall_ticks(void);
uint64_t sched_context_switches(void);
uint64_t sched_idle_ticks(void);

struct task;

/*
 * Stage 9-evt step 3 — the per-core dispatcher and the stack it runs on.
 *
 * core_dispatch_init:   publish this core's stack top into its cpu_local.
 * core_stack_top_for:   that stack's top, for the boot path that has no
 *                       cpu_local to read yet.
 */
void     core_dispatch_init(void);
uint64_t core_stack_top_for(uint32_t cpu_id);
/* The loop itself; entered only through core_dispatch_enter. */
void     core_dispatch(struct task *outgoing);
/* Choose the next thread and COMMIT to it — current_task, TSS, CR3 and the
 * accounting are all done by the time it returns.  NULL means idle. */
struct task *sched_pick_for_dispatch(struct task *outgoing);
/* One tick's worth of "this core had nothing to do". */
void     sched_idle_account(void);

#endif
