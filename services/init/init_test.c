/*
 * init_test.c — init runtime probes + S8 exception selftest (Fase 14/Inc 2).
 *
 * Extracted verbatim from main.c: the boot supervisor calls these after the
 * healthy path is up.  No boot-order, launch, or grant logic lives here — only
 * test/probe logic and the smoke markers it emits.  Behaviour is byte-identical
 * to the pre-Fase-14 monolith.
 */

#include "init.h"
#include <iris/fault_proto.h>

static const char init_stage_exception[] = "[USER][INIT][S8] exception delivery OK\n";

/* Fase 13 (Track I): the invalid-userptr selftest now exercises the kernel's
 * user-pointer validation over a KNotification (SYS_NOTIFY_WAIT_TIMEOUT with a
 * bogus out_bits pointer → IRIS_ERR_INVALID_ARG) instead of a KChannel. */
void init_runtime_probe_invalid_userptr(void) {
    long n = init_sys0(SYS_NOTIFY_CREATE);
    if (n < 0) return;
    long r = init_sys3(SYS_NOTIFY_WAIT_TIMEOUT, n, 1 /* bogus user ptr */, 50000000L);
    if (r == (long)IRIS_ERR_INVALID_ARG)
        init_log("[USER][INIT][SELFTEST] invalid-userptr OK\n");
    else
        init_log("[USER][INIT][SELFTEST] invalid-userptr WARN\n");
    init_sys1(SYS_HANDLE_CLOSE, n);
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

/* Fase 13 (Track I): exception delivery is a KNotification now — the kernel
 * records the fault and signals the handler's notification; init reads the
 * details with SYS_PROCESS_FAULT_INFO.  No KChannel. */
void init_selftest_exception(void) {
    uint8_t fbuf[FAULT_MSG_LEN];
    long n_raw, tid_raw, r;
    handle_id_t notif_h;
    uint32_t vec, task_id;
    uint64_t bits = 0;

    n_raw = init_sys0(SYS_NOTIFY_CREATE);
    if (n_raw < 0) { init_log("[USER][INIT][S8] SKIP: notify create\n"); return; }
    notif_h = (handle_id_t)n_raw;

    /* Register exception handler for own process (HANDLE_INVALID = self),
     * signalling bit 0 on fault. */
    r = init_sys3(SYS_EXCEPTION_HANDLER, (long)HANDLE_INVALID, (long)notif_h, 1);
    if (r < 0) {
        init_sys1(SYS_HANDLE_CLOSE, (long)notif_h);
        init_log("[USER][INIT][S8] SKIP: handler reg\n"); return;
    }

    /* Spawn a thread that immediately executes ud2 (#UD, vector 6) */
    uint64_t entry = (uint64_t)(uintptr_t)s8_ud2_fn;
    uint64_t rsp   = (uint64_t)(uintptr_t)(s8_thread_stack + sizeof(s8_thread_stack));
    tid_raw = init_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid_raw < 0) {
        init_sys1(SYS_HANDLE_CLOSE, (long)notif_h);
        init_log("[USER][INIT][S8] SKIP: thread create\n"); return;
    }

    /* Wait up to 1 s for the fault notification, then read the fault details. */
    r = init_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)notif_h, (long)&bits, 1000000000L);
    if (r < 0) {
        init_sys1(SYS_HANDLE_CLOSE, (long)notif_h);
        init_log("[USER][INIT][S8] FAIL: no fault signal\n"); return;
    }

    for (uint32_t i = 0; i < (uint32_t)sizeof(fbuf); i++) fbuf[i] = 0;
    r = init_sys2(SYS_PROCESS_FAULT_INFO, (long)HANDLE_INVALID, (long)fbuf);
    if (r < 0) {
        init_sys1(SYS_HANDLE_CLOSE, (long)notif_h);
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
        init_sys1(SYS_HANDLE_CLOSE, (long)notif_h);
        init_log("[USER][INIT][S8] FAIL: wrong fault\n"); return;
    }

    /* Kill the faulting thread via exception resume */
    (void)init_sys3(SYS_EXCEPTION_RESUME, (long)HANDLE_INVALID, (long)task_id, 1);

    init_sys1(SYS_HANDLE_CLOSE, (long)notif_h);

    init_log(init_stage_exception);
}
