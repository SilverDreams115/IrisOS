/*
 * init_test.c — init runtime probes + S8 exception selftest (Phase 14/Inc 2).
 *
 * Extracted verbatim from main.c: the boot supervisor calls these after the
 * healthy path is up.  No boot-order, launch, or grant logic lives here — only
 * test/probe logic and the smoke markers it emits.  Behaviour is byte-identical
 * to the pre-Phase-14 monolith.
 */

#include "init.h"
#include <iris/fault_proto.h>

static const char init_stage_exception[] = "[USER][INIT][S8] exception delivery OK\n";

/* Phase 13 (Track I): the invalid-userptr selftest now exercises the kernel's
 * user-pointer validation over a KNotification (SYS_NOTIFY_WAIT_TIMEOUT with a
 * bogus out_bits pointer → IRIS_ERR_INVALID_ARG) instead of a KChannel. */
void init_runtime_probe_invalid_userptr(void) {
    long n = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_NOTIFICATION,
                              INIT_SLOT_PROBE_NOTIF, 0);
    if (n < 0) return;
    n = (long)INIT_SLOT_PROBE_NOTIF;
    long r = init_sys3(SYS_NOTIFY_WAIT_TIMEOUT, n, 1 /* bogus user ptr */, 50000000L);
    if (r == (long)IRIS_ERR_INVALID_ARG)
        init_log("[USER][INIT][SELFTEST] invalid-userptr OK\n");
    else
        init_log("[USER][INIT][SELFTEST] invalid-userptr WARN\n");
}

void init_runtime_probe_timeout_overflow(void) {
    init_log("[USER][INIT][SELFTEST] timeout-overflow SKIP\n");
}

/* Stack for the ud2 fault thread — statically allocated, never actually used
 * (the thread immediately faults before touching the stack). */
static uint8_t s8_thread_stack[4096];

static void __attribute__((noinline)) s8_ud2_fn(void) {
    __asm__ volatile ("ud2");
    for (;;) {}
}

/* Phase 13 (Track I): exception delivery is a KNotification now — the kernel
 * records the fault and signals the handler's notification; init reads the
 * details with SYS_PROCESS_FAULT_INFO.  No KChannel. */
void init_selftest_exception(void) {
    uint8_t fbuf[FAULT_MSG_LEN];
    long n_raw, tid_raw, r;
    handle_id_t notif_h;
    uint32_t vec, task_id;
    uint64_t bits = 0;

    n_raw = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_NOTIFICATION,
                             INIT_SLOT_S8_NOTIF, 0);
    if (n_raw < 0) { init_log("[USER][INIT][S8] SKIP: notify create\n"); return; }
    /* Stage 4: INIT_SLOT_S8_NOTIF is a CSpace slot init retyped into, not a
     * handle.  The SYS_HANDLE_CLOSE calls that used to guard every early
     * return here were asking the handle table to close a CPtr — a failed
     * call that read as cleanup.  The slot is init's for the whole run. */
    notif_h = (handle_id_t)INIT_SLOT_S8_NOTIF;

    /* Spawn a thread that immediately executes ud2 (#UD, vector 6).
     *
     * Stage 5 Step 4: the thread is a TCB RETYPED from init's own Untyped and
     * configured with capabilities to the CSpace and VSpace it runs in — the
     * kernel's static task pool is not reachable from userland any more.  Each
     * step can fail on its own, and each failure is a SKIP rather than a
     * silent non-test. */
    uint64_t entry = (uint64_t)(uintptr_t)s8_ud2_fn;
    uint64_t rsp   = (uint64_t)(uintptr_t)(s8_thread_stack + sizeof(s8_thread_stack));
    if (init_sys1(SYS_CSPACE_SELF,
                  (long)((uint64_t)INIT_SLOT_OWN_CSPACE << 32)) != 0 ||
        init_sys1(SYS_VSPACE_SELF,
                  (long)((uint64_t)INIT_SLOT_OWN_VSPACE << 32)) != 0) {
        init_log("[USER][INIT][S8] SKIP: self caps\n"); return;
    }
    (void)init_sys2(SYS_CNODE_DELETE, 0, (long)INIT_SLOT_S8_TCB);
    tid_raw = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_TCB,
                               INIT_SLOT_S8_TCB, 0);
    if (tid_raw < 0) {
        init_log("[USER][INIT][S8] SKIP: tcb retype\n"); return;
    }
    /*
     * Stage 7 Step 12: arm the faults of the THREAD that is about to take one,
     * which is why this moved below the retype — there was no thread to name
     * before it.  Registration used to name init's PROCESS and catch whatever
     * of it faulted; it names the execution now, and the mailbox
     * (INIT_SLOT_S8_FAULT, in init's own root CNode) is where that thread's
     * capability lands so SYS_EXCEPTION_RESUME can answer.
     */
    if (init_sys4(SYS_TCB_SET_FAULT_HANDLER, (long)INIT_SLOT_S8_TCB,
                  (long)notif_h, 1,
                  (long)((uint64_t)INIT_SLOT_S8_FAULT << 32)) != 0) {
        init_log("[USER][INIT][S8] SKIP: handler reg\n"); return;
    }
    if (init_sys3(SYS_TCB_CONFIGURE, (long)INIT_SLOT_S8_TCB,
                  (long)INIT_SLOT_OWN_CSPACE, (long)INIT_SLOT_OWN_VSPACE) != 0 ||
        init_sys4(SYS_TCB_WRITE_REGS, (long)INIT_SLOT_S8_TCB,
                  (long)entry, (long)rsp, 0) != 0 ||
        init_sys1(SYS_TCB_RESUME, (long)INIT_SLOT_S8_TCB) != 0) {
        init_log("[USER][INIT][S8] SKIP: thread create\n"); return;
    }

    /* Wait up to 1 s for the fault notification, then read the fault details. */
    r = init_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)notif_h, (long)&bits, 1000000000L);
    if (r < 0) {
        init_log("[USER][INIT][S8] FAIL: no fault signal\n"); return;
    }

    for (uint32_t i = 0; i < (uint32_t)sizeof(fbuf); i++) fbuf[i] = 0;
    /* Stage 7 Step 8: the record comes off the thread whose capability the
     * fault delivered into INIT_SLOT_S8_FAULT. */
    r = init_sys2(SYS_TCB_FAULT_INFO, (long)INIT_SLOT_S8_FAULT, (long)fbuf);
    if (r < 0) {
        init_log("[USER][INIT][S8] FAIL: no fault info\n"); return;
    }
    vec = (uint32_t)fbuf[FAULT_OFF_VECTOR]
        | ((uint32_t)fbuf[FAULT_OFF_VECTOR + 1] << 8)
        | ((uint32_t)fbuf[FAULT_OFF_VECTOR + 2] << 16)
        | ((uint32_t)fbuf[FAULT_OFF_VECTOR + 3] << 24);
    task_id = (uint32_t)fbuf[FAULT_OFF_TASK_ID]
            | ((uint32_t)fbuf[FAULT_OFF_TASK_ID + 1] << 8)
            | ((uint32_t)fbuf[FAULT_OFF_TASK_ID + 2] << 16)
            | ((uint32_t)fbuf[FAULT_OFF_TASK_ID + 3] << 24);

    if (vec != 6u) {
        init_log("[USER][INIT][S8] FAIL: wrong fault\n"); return;
    }

    /* Kill the faulting thread — named by the capability the fault delivered,
     * not by the id the record still reports for diagnostics. */
    (void)init_sys2(SYS_EXCEPTION_RESUME, (long)INIT_SLOT_S8_FAULT, 1);
    (void)task_id;


    init_log(init_stage_exception);
}
