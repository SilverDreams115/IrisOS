/* SPDX-License-Identifier: Apache-2.0 */
#include "syscall_priv.h"
#include <iris/fault_proto.h>

uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    /* The code belongs to the execution that produced it.
     * Step 15: and only to it — the process copy was kept while
     * SYS_PROCESS_EXIT_CODE could still be asked, and that syscall is
     * retired. */
    if (t) t->exit_code = (uint32_t)arg0;
    task_exit_current();
    return 0; /* unreachable */
}


/*
 * SYS_YIELD — restartable, like every other blocking syscall.
 *
 * It used to call task_yield, which switched from inside this frame and
 * returned here when the thread ran again: the continuation was "the rest of
 * this function", on the thread's kernel stack, for as long as the yield
 * lasted.  Once that stack belongs to the CORE there is no such place, so the
 * yield became a park and the continuation became a fact about the thread —
 * `sc_reentry`, which is the dispatcher saying "you already yielded; this is
 * you running again".
 *
 * A yield has nothing else to carry, which makes it the smallest possible
 * example of the shape: ask, leave, and on the way back, return.
 */
uint64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t) return 0;
    if (t->sc_reentry) return 0;      /* this IS the resume */

    t->need_resched = 1;
    syscall_request_restart(t);
    return 0;
}




/* ── Process lifecycle query ──────────────────────────────────────── */

/*
 * sys_process_status, sys_process_kill and sys_process_fault_info RETIRED.
 *
 * All three named a PROCESS by a handle, and there is neither any more: the
 * object was deleted and the handle table with it.  What they did is asked of
 * the THREAD now, by capability — SYS_TCB_WATCH, SYS_TCB_EXIT,
 * SYS_TCB_EXIT_CODE and SYS_TCB_FAULT_INFO — and their syscall numbers answer
 * IRIS_ERR_NOT_SUPPORTED for ever, because a retired number is never reused.
 */
