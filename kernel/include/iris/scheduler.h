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
uint64_t sched_current_ticks(void);

#endif
