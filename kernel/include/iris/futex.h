#ifndef IRIS_FUTEX_H
#define IRIS_FUTEX_H

#include <iris/nc/error.h>
#include <stdint.h>

struct task;

/*
 * futex_wait: block until woken, if *uaddr still equals expected.
 * futex_wake: wake up to count threads waiting on uaddr.
 * futex_cancel_waiter: remove any pending wait registered by task t.
 *   Called from task_cancel_blocked_waits during task teardown.
 */
/* futex_wait REMOVED (Stage 9-evt Step 1) — see futex_wait_step. */
/* Stage 9-evt Step 1 — restartable half; IRIS_ERR_BUSY means "parked, re-execute". */
iris_error_t futex_wait_step(uint64_t uaddr, uint32_t expected,
                             uint64_t deadline_ticks, int first);
uint32_t     futex_wake(uint64_t uaddr, uint32_t count);
void         futex_cancel_waiter(struct task *t);

#endif
