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
/* Fase 16: high-water depth of the deferred-reap queue.  Monotonic; a value
 * approaching REAP_QUEUE_SIZE would mean the single-CPU "one death per yield"
 * assumption is being violated and dead task slots may leak. */
uint32_t sched_reap_queue_hwm(void);

/*
 * Fase 17 — scheduler hardening instrumentation (all additive, read-only).
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
/* Fase S2 Etapa C — KTCB registry gauges (references, not payload). */
void     task_registry_stats(uint32_t *active, uint32_t *hwm,
                             uint32_t *exhaustions, uint32_t *gen_mismatch);
uint32_t sched_yield_count(void);
uint64_t sched_current_ticks(void);
uint64_t sched_wall_ticks(void);
uint64_t sched_context_switches(void);
uint64_t sched_idle_ticks(void);

#endif
