/*
 * init_test.c — init runtime probes + S8 exception selftest (Phase 14/Inc 2).
 *
 * Extracted verbatim from main.c: the boot supervisor calls these after the
 * healthy path is up.  No boot-order, launch, or grant logic lives here — only
 * test/probe logic and the smoke markers it emits.  Behaviour is byte-identical
 * to the pre-Phase-14 monolith.
 */

#include <iris/endpoint_proto.h>
#include "../common/iris_msg.h"
#include "init.h"
#include <iris/ipc_msg.h>
#include <iris/fault_proto.h>

static const char init_stage_exception[] = "[USER][INIT][S8] exception delivery OK\n";

/* Phase 13 (Track I): the invalid-userptr selftest now exercises the kernel's
 * user-pointer validation over a KNotification (A-24: SYS_NOTIFY_POLL, with a
 * bogus out_bits pointer → IRIS_ERR_INVALID_ARG) instead of a KChannel. */
void init_runtime_probe_invalid_userptr(void) {
    long n = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_NOTIFICATION,
                              INIT_SLOT_PROBE_NOTIF, 0);
    if (n < 0) return;
    n = (long)INIT_SLOT_PROBE_NOTIF;
    long r = iris_invoke1(n, INV_NOTIFY_POLL, 1 /* bogus user ptr */);
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

/* Ledger A-22: exception delivery is IPC.  The faulting thread CALLS the
 * endpoint its supervisor armed; init receives the record as an ordinary
 * message and holds the reply capability that would resume it. */
void init_selftest_exception(void) {
    long ep_raw, rp_raw, tid_raw, r;
    uint32_t vec, task_id;
    struct iris_msg fm;

    ep_raw = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                              INIT_SLOT_S8_FAULT_EP, 0);
    if (ep_raw < 0) { init_log("[USER][INIT][S8] SKIP: ep create\n"); return; }
    /* A fault is a CALL, so answering one needs reply authority staged at the
     * receive.  The object is init's for the whole run, like the endpoint. */
    rp_raw = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_REPLY,
                              INIT_SLOT_S8_REPLY, 0);
    if (rp_raw < 0) { init_log("[USER][INIT][S8] SKIP: reply create\n"); return; }

    /* Spawn a thread that immediately executes ud2 (#UD, vector 6).
     *
     * Stage 5 Step 4: the thread is a TCB RETYPED from init's own Untyped and
     * configured with capabilities to the CSpace and VSpace it runs in — the
     * kernel's static task pool is not reachable from userland any more.  Each
     * step can fail on its own, and each failure is a SKIP rather than a
     * silent non-test. */
    uint64_t entry = (uint64_t)(uintptr_t)s8_ud2_fn;
    uint64_t rsp   = (uint64_t)(uintptr_t)(s8_thread_stack + sizeof(s8_thread_stack));
    /* D-6/A5: init's own CSpace and address space are DELEGATED by its
     * spawner (userboot, through svc_loader) at the well-known slots.  They
     * used to be fabricated by SYS_CSPACE_SELF / SYS_VSPACE_SELF — capabilities
     * handed over on request, with no capability asked for and no ancestor to
     * revoke them through. */
    if (iris_invoke2((long)IRIS_CPTR_OWN_CSPACE, INV_CSPACE_MINT, (long)((uint64_t)INIT_SLOT_OWN_CSPACE << 32), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0 ||
        iris_invoke2((long)IRIS_CPTR_OWN_VSPACE, INV_CSPACE_MINT, (long)((uint64_t)INIT_SLOT_OWN_VSPACE << 32), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0) {
        init_log("[USER][INIT][S8] SKIP: self caps\n"); return;
    }
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_S8_TCB);
    tid_raw = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_TCB,
                               INIT_SLOT_S8_TCB, 0);
    if (tid_raw < 0) {
        init_log("[USER][INIT][S8] SKIP: tcb retype\n"); return;
    }
    /*
     * Ledger A-22: point the thread's faults at the endpoint above.  It moved
     * below the retype in Stage 7 Step 12 for the reason it stays there: there
     * is no thread to name before it.
     */
    if (iris_invoke((long)INIT_SLOT_S8_TCB, INV_TCB_SET_FAULT_HANDLER, (long)INIT_SLOT_S8_FAULT_EP, 0, 0) != 0) {
        init_log("[USER][INIT][S8] SKIP: handler reg\n"); return;
    }
    if (iris_invoke2((long)INIT_SLOT_S8_TCB, INV_TCB_CONFIGURE, (long)INIT_SLOT_OWN_CSPACE, (long)INIT_SLOT_OWN_VSPACE) != 0 ||
        iris_invoke((long)INIT_SLOT_S8_TCB, INV_TCB_WRITE_REGS, (long)entry, (long)rsp, 0) != 0 ||
        iris_invoke0((long)INIT_SLOT_S8_TCB, INV_TCB_RESUME) != 0) {
        init_log("[USER][INIT][S8] SKIP: thread create\n"); return;
    }

    /* Receive the fault.  One call, where it used to be a timed notification
     * wait followed by a second syscall to fetch what the signal did not
     * carry. */
    for (uint32_t i = 0; i < (uint32_t)sizeof(fm); i++) ((uint8_t *)&fm)[i] = 0;
    r = (fm.reply = (long)INIT_SLOT_S8_REPLY, iris_msg_recv((long)INIT_SLOT_S8_FAULT_EP, &fm));
    if (r < 0) {
        init_log("[USER][INIT][S8] FAIL: no fault message\n"); return;
    }

    {
        const uint8_t *fbuf = (const uint8_t *)fm.words;
        vec = (uint32_t)fbuf[FAULT_OFF_VECTOR]
            | ((uint32_t)fbuf[FAULT_OFF_VECTOR + 1] << 8)
            | ((uint32_t)fbuf[FAULT_OFF_VECTOR + 2] << 16)
            | ((uint32_t)fbuf[FAULT_OFF_VECTOR + 3] << 24);
        task_id = (uint32_t)fbuf[FAULT_OFF_TASK_ID]
                | ((uint32_t)fbuf[FAULT_OFF_TASK_ID + 1] << 8)
                | ((uint32_t)fbuf[FAULT_OFF_TASK_ID + 2] << 16)
                | ((uint32_t)fbuf[FAULT_OFF_TASK_ID + 3] << 24);
    }

    if (vec != 6u) {
        init_log("[USER][INIT][S8] FAIL: wrong vector\n"); return;
    }

    /* Kill the faulting thread by REFUSING to answer it: dropping the reply
     * object is how a handler says "this one does not resume", and the kernel
     * destroys a thread whose fault nobody will ever answer. */
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_S8_REPLY);
    (void)task_id;


    init_log(init_stage_exception);
}
