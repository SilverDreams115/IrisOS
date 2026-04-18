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
iris_error_t futex_wait(uint64_t uaddr, uint32_t expected);
uint32_t     futex_wake(uint64_t uaddr, uint32_t count);
void         futex_cancel_waiter(struct task *t);

#endif
