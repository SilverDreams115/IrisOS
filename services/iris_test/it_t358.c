/* SPDX-License-Identifier: Apache-2.0 */
/*
 * it_t358.c — a thread pointer, and it is the THREAD's.
 *
 * Stage 10-run step 1.  Until this landed, ring 3 had no thread-local storage
 * of any kind: IA32_FS_BASE was never written by the kernel, never saved and
 * never restored; `struct iris_user_ctx` carries no segment base; `%gs` is the
 * kernel's under the SWAPGS ABI; and CR4.FSGSBASE is not enabled, so a thread
 * could not even set its own.  Two threads shared whatever the processor
 * happened to hold — which is to say, there was nothing that could be called a
 * thread pointer at all.  A C runtime cannot exist without one: musl's
 * `__pthread_self()` is `mov %fs:0` and `errno` is a field of what it returns.
 *
 * ── Why the test is shaped like this ───────────────────────
 *
 * One thread reading back what it just wrote proves almost nothing: the value
 * would survive exactly the same way if the base were a global.  The property
 * is that the base belongs to the THREAD and travels with it across a context
 * switch, so the discriminating check is two threads, each seeing only its
 * own, across thousands of switches, on a machine where they may be on
 * different processors and really do run at once.
 *
 * Each worker is handed its OWN TCB capability in its entry register
 * (`IT_THREAD_ARG_SELF_TCB`), which is the only per-thread channel a freshly
 * started thread has.  The first version of this test had the workers derive
 * `IRIS_CPTR_OWN_TCB` instead, and that slot lives in the CSpace these threads
 * SHARE: both workers set the MAIN thread's base, their own stayed zero, and
 * they page-faulted reading `%fs:0` at address zero.  That is not a defect to
 * fix — seL4 hands the initial thread `seL4_CapInitThreadTCB` and gives a
 * created thread nothing unless somebody grants it — it is the model being
 * right and the test being wrong.
 *
 * Four claims:
 *  1. a thread sets its own base and reads through it immediately, with no
 *     context switch in between — the path where the kernel installs the MSR
 *     at the syscall door rather than at the next resume;
 *  2. two threads with different bases never see each other's, across
 *     thousands of switches — the path where `sched_resume` restores it;
 *  3. a base at or above the user half is REFUSED.  This is the claim that
 *     matters for the KERNEL rather than for the runtime: `wrmsr` on
 *     IA32_FS_BASE with a non-canonical value raises #GP in ring 0, on the
 *     resume path, for a number ring 3 chose;
 *  4. and a REFUSED set leaves the target's base exactly as it was — which
 *     is the claim that says the bound is checked BEFORE anything is written,
 *     rather than written and then regretted.
 *
 * It takes nothing from the rotating object pool, deliberately.  Every
 * capability it needs is one it created: the workers' own TCBs.  A test that
 * borrows a pool slot and gives it back still advances the allocator's cursor,
 * which moves where the next wrap lands — and T324 counts wraps that evict
 * something live.  Two tests arguing over a shared allocator's phase is not
 * evidence about either of them.
 *
 * Invariants: A1, A5, S5.
 */
#include "it_priv.h"

#define T358_WORKERS  2u
#define T358_ROUNDS   2000u
/* The user half's ceiling.  A base at or above it is either non-canonical or
 * in the kernel half, and the kernel must refuse both. */
#define T358_USER_TOP 0x0000800000000000ULL

/* One "thread control block" per worker, in this task's own memory.  Offset 0
 * is what `%fs:0` reads, which is where musl keeps a thread's pointer to
 * itself — so this is the shape a real runtime will use. */
static volatile uint64_t g_t358_block[T358_WORKERS][8];
static volatile uint64_t g_t358_seen[T358_WORKERS];
static volatile uint32_t g_t358_bad[T358_WORKERS];
static volatile uint32_t g_t358_ready[T358_WORKERS];
static volatile int      g_t358_stop;
static uint8_t           g_t358_stack[T358_WORKERS][4096];

static inline uint64_t t358_fs0(void) {
    uint64_t v;
    __asm__ __volatile__ ("movq %%fs:0, %0" : "=r"(v));
    return v;
}

/* Distinct in every byte, so a worker reading the other's block is
 * unmistakable rather than merely wrong. */
static inline uint64_t t358_magic(uint32_t i) {
    return 0x7358000000000000ULL | (uint64_t)(i + 1u);
}

static void t358_body(uint64_t self_tcb, uint32_t i) {
    g_t358_block[i][0] = t358_magic(i);
    if (it_invoke2((long)self_tcb, INV_TCB_SET_TLS_BASE,
                   (long)(uintptr_t)&g_t358_block[i][0], 0) != 0) {
        g_t358_bad[i]++;
        g_t358_ready[i] = 1u;
        it_sys1(SYS_EXIT, 0);
        for (;;) {}
    }

    /* Claim 1, from inside: readable with no switch in between. */
    if (t358_fs0() != t358_magic(i)) g_t358_bad[i]++;
    g_t358_ready[i] = 1u;

    /* Claim 2: and it is still this thread's after every resume. */
    while (!g_t358_stop) {
        if (t358_fs0() != t358_magic(i)) g_t358_bad[i]++;
        g_t358_seen[i]++;
        it_sys1(SYS_YIELD, 0);
    }
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
static void t358_w0(uint64_t self_tcb) { t358_body(self_tcb, 0); }
static void t358_w1(uint64_t self_tcb) { t358_body(self_tcb, 1); }

void test_t358(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "the thread pointer is the thread's";

    static void (*const entry[T358_WORKERS])(uint64_t) = { t358_w0, t358_w1 };
    g_t358_stop = 0;
    for (uint32_t i = 0; i < T358_WORKERS; i++) {
        g_t358_seen[i] = 0; g_t358_bad[i] = 0; g_t358_ready[i] = 0;
        g_t358_block[i][0] = 0;
    }

    /* ── 1 and 2. two threads, each with its own ─────────────────────────*/
    long wtcb[T358_WORKERS];
    uint32_t started = 0;
    for (uint32_t i = 0; i < T358_WORKERS; i++) wtcb[i] = -1;
    if (ok) {
        for (uint32_t i = 0; i < T358_WORKERS; i++) {
            uint64_t rsp = ((uint64_t)(uintptr_t)(g_t358_stack[i] +
                              sizeof(g_t358_stack[i]))) & ~0xFULL;
            wtcb[i] = it_thread_create((uint64_t)(uintptr_t)entry[i], rsp,
                                       IT_THREAD_ARG_SELF_TCB);
            if (wtcb[i] < 0) { ok = 0; why = "thread"; break; }
            started++;
        }
    }

    if (ok) {
        /* Bounded by TIME: how long a thread takes to reach its first
         * instruction is a fact about the machine, not about this. */
        long t0 = it_sys0(SYS_CLOCK_GET);
        for (;;) {
            if (g_t358_ready[0] && g_t358_ready[1]) break;
            it_settle(1);
            long now = it_sys0(SYS_CLOCK_GET);
            if (t0 <= 0 || now <= 0) { for (int k = 0; k < 400; k++) it_settle(1); break; }
            if ((uint64_t)(now - t0) > 4000ull * 1000000ull) break;
        }
        if (!g_t358_ready[0] || !g_t358_ready[1]) {
            ok = 0; why = "a worker never got its own base";
        }
    }

    if (ok) {
        /* Enough switches that a base which did not travel with its thread
         * would have been caught long before the end. */
        long t0 = it_sys0(SYS_CLOCK_GET);
        for (;;) {
            if (g_t358_seen[0] > T358_ROUNDS && g_t358_seen[1] > T358_ROUNDS) break;
            it_settle(1);
            long now = it_sys0(SYS_CLOCK_GET);
            if (t0 <= 0 || now <= 0) { for (int k = 0; k < 600; k++) it_settle(1); break; }
            if ((uint64_t)(now - t0) > 8000ull * 1000000ull) break;
        }
        if (g_t358_seen[0] <= T358_ROUNDS || g_t358_seen[1] <= T358_ROUNDS) {
            ok = 0; why = "the workers did not run enough to prove anything";
            it_fz_note("T358", (uint32_t)g_t358_seen[0], (uint32_t)g_t358_seen[1], 0u);
        }
    }

    if (ok && (g_t358_bad[0] || g_t358_bad[1])) {
        ok = 0; why = "a thread read a base that was not its own";
        it_fz_note("T358", g_t358_bad[0], g_t358_bad[1], 0u);
    }

    /*
     * ── 3 and 4. the bound, against a real thread ────────────────────────
     *
     * Checked on a worker's TCB rather than on a made-up one, because that is
     * what says the refusal is about the BASE and not about the capability.
     * The kernel must refuse before it writes anything: `wrmsr` on
     * IA32_FS_BASE with a non-canonical value raises #GP in ring 0, on the
     * resume path, for a number ring 3 chose — so a base that was stored and
     * only rejected later would be a thread that brings the machine down the
     * next time it is scheduled.
     *
     * The worker keeps running through all of this, and claim 4 is that it
     * never sees anything but its own magic afterwards.
     */
    if (ok) {
        uint64_t bad_before = g_t358_bad[0];
        static const uint64_t refuse[3] = {
            T358_USER_TOP,            /* the first address of the kernel half */
            0xFFFF800000000000ULL,    /* canonical, but not the user's */
            0x8000000000000000ULL,    /* not canonical at all */
        };
        for (uint32_t k = 0; ok && k < 3u; k++) {
            if (it_invoke2(wtcb[0], INV_TCB_SET_TLS_BASE, (long)refuse[k], 0)
                != (long)IRIS_ERR_INVALID_ARG) {
                ok = 0; why = "a base outside the user half was accepted";
                it_fz_note("T358", k, 0u, 0u);
            }
        }
        /* And it kept running with the base it had. */
        uint64_t mark = g_t358_seen[0];
        long t0 = it_sys0(SYS_CLOCK_GET);
        for (;;) {
            if (g_t358_seen[0] > mark + 100u) break;
            it_settle(1);
            long now = it_sys0(SYS_CLOCK_GET);
            if (t0 <= 0 || now <= 0) { for (int k = 0; k < 400; k++) it_settle(1); break; }
            if ((uint64_t)(now - t0) > 4000ull * 1000000ull) break;
        }
        if (ok && g_t358_bad[0] != bad_before) {
            ok = 0; why = "a refused set disturbed the target's base";
        }
    }

    it_serial_write("[IRIS][TEST] T358 switches=");
    it_log_num((uint32_t)g_t358_seen[0]);
    it_serial_write("/");
    it_log_num((uint32_t)g_t358_seen[1]);
    it_serial_write("\n");

    g_t358_stop = 1;
    for (int k = 0; k < 200; k++) it_settle(1);
    for (uint32_t i = 0; i < started; i++)
        if (wtcb[i] >= 0) it_slot_delete((uint32_t)wtcb[i]);
    it_quiesce_reaper();

    if (ok) it_pass("T358"); else it_fail("T358", why);
}
