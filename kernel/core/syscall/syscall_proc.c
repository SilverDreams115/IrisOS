/* SPDX-License-Identifier: Apache-2.0 */
#include "syscall_priv.h"
#include <iris/fault_proto.h>

uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    /* Stage 7 Step 10: the code belongs to the execution that produced it.
     * Step 15: and only to it — the process copy was kept while
     * SYS_PROCESS_EXIT_CODE could still be asked, and that syscall is
     * retired. */
    if (t) t->exit_code = (uint32_t)arg0;
    task_exit_current();
    return 0; /* unreachable */
}


/*
 * SYS_YIELD — restartable, like every other blocking syscall (Stage 9-evt).
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
 * sys_process_status(proc_handle) → 1 (alive), 0 (dead), or iris_error_t
 *
 * Non-blocking.  Returns immediately regardless of the target state.
 * Requires RIGHT_READ on proc_handle.
 *
 * Lifecycle contract:
 *   - Returns 1 while the process is running or blocked (main_thread alive).
 *   - Returns 0 once the process has called SYS_EXIT or been reaped;
 *     thread teardown has run and TASK_DEAD has been set.
 *   - The handle remains valid after death until the caller closes it;
 *     this allows the caller to detect and then clean up in one pass.
 *   - Closing the handle (SYS_HANDLE_CLOSE) is the caller's responsibility
 *     after observing death; the KProcess is released when refcount hits zero.
 */
/* ── Process termination ──────────────────────────────────────────── */

/*
 * sys_process_kill(proc_handle) → 0 or iris_error_t
 *
 * Requires RIGHT_MANAGE on proc_handle.
 * Cannot be used for self-termination — use SYS_EXIT for that (IRIS_ERR_INVALID_ARG).
 * Idempotent: if the target is already dead, returns 0 immediately.
 *
 * Internally calls task_kill_external which: runs thread teardown (fires exit
 * watches, closes the process's own handle table, unregisters IRQ routes),
 * frees user stack pages, reaps the address space (safe since the caller's CR3
 * is different from the target's), and releases the kernel's creation reference.
 *
 * The caller's handle to the proc remains valid until the caller closes it;
 * the KProcess object is freed when all handles to it are closed.
 */
/* ── Threading (D2) ──────────────────────────────────────────────── */

/*
 * sys_process_fault_info(proc_handle, out_uptr) → 0 or iris_error_t
 *
 * Phase 13 (Track I): reads the last fault recorded for proc_handle (or self when
 * proc_handle == IRIS_CPTR_NULL) into a 32-byte user buffer laid out per
 * iris/fault_proto.h (FAULT_OFF_VECTOR/TASK_ID/RIP/ERROR/CR2).  The exception
 * handler calls this after its KNotification fires.  Returns IRIS_ERR_WOULD_BLOCK
 * if no fault is pending.  Requires RIGHT_READ on a non-self proc_handle.
 */

