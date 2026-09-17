/*
 * it_t079_t100.c — tests T079 through T100.
 *
 * The suite's numbering is chronological, not thematic: T079 was written
 * stages before T100, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T079 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"


#include "../common/iris_msg.h"
void test_t079(void) {
    /* init mints the self-proc cap post-load; retry briefly, then FAIL loud.
     * Stage 4: the cap is USED as a CPtr — SYS_PROC_CSPACE_MINT resolves its
     * process argument through CSpace — so waiting for it is a liveness probe
     * on the slot, not a materialisation into a handle. */
    long selfp = -1;
    for (int i = 0; i < 50 && selfp < 0; i++) {
        selfp = (it_invoke0((long)IRIS_CPTR_TEST_PROC, INV_CAP_IDENTIFY) >= 0)
                ? (long)IRIS_CPTR_TEST_PROC : -1;
        if (selfp < 0) it_settle(2);
    }
    if (selfp < 0) { it_fail("T079", "self proc cptr"); return; }

    long vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);
    if (vmo < 0) { it_fail("T079", "vmo create"); return; }
    iris_cptr_t vmo_h = (iris_cptr_t)vmo;

    int ok = 1;
    const char *why = "map by cptr";

    /* Mint the frame into our own CSpace: slot 16 rw, slot 17 read-only. */
    if (it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_SELF(T079_SLOT_RW), (long)(RIGHT_READ | RIGHT_WRITE)) != 0) { ok = 0; why = "mint rw"; }
    if (ok && it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_SELF(T079_SLOT_RO), (long)RIGHT_READ) != 0) { ok = 0; why = "mint ro"; }

    /* Map by CPtr (writable) and write through the mapping. */
    if (ok && it_invoke(T079_SLOT_RW, INV_FRAME_MAP, IT_VS, (long)T079_VA_CPTR, 1) != 0) { ok = 0; why = "map rw"; }
    if (ok) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)T079_VA_CPTR;
        *p = 0xA1C0FFEE00000079ULL;
        if (*p != 0xA1C0FFEE00000079ULL) { ok = 0; why = "write back"; }
    }

    /* Failure paths: empty slot, wrong type, insufficient rights. */
    if (ok && it_invoke(T079_SLOT_EMPTY, INV_FRAME_MAP, IT_VS, (long)T079_VA_HANDLE, 1) >= 0) { ok = 0; why = "empty slot mapped"; }
    if (ok && it_invoke((long)IRIS_CPTR_TEST_FIX_A, INV_FRAME_MAP, IT_VS, (long)T079_VA_HANDLE, 1) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "wrong type"; }
    if (ok && it_invoke(T079_SLOT_RO, INV_FRAME_MAP, IT_VS, (long)T079_VA_HANDLE, 1) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable"; }

    /* A second, independent capability to the same frame maps at a second VA
     * and reads back what the first mapping wrote — same physical page. */
    if (ok && it_invoke(vmo, INV_FRAME_MAP, IT_VS, (long)T079_VA_HANDLE, 1) != 0) { ok = 0; why = "second map"; }
    if (ok) {
        volatile uint64_t *q = (volatile uint64_t *)(uintptr_t)T079_VA_HANDLE;
        if (*q != 0xA1C0FFEE00000079ULL) { ok = 0; why = "not the same page"; }
    }

    (void)it_invoke2(T079_SLOT_RW, INV_FRAME_UNMAP, IT_VS, (long)T079_VA_CPTR);
    (void)it_invoke2(vmo, INV_FRAME_UNMAP, IT_VS, (long)T079_VA_HANDLE);
    it_close(&vmo_h);

    if (ok) it_pass("T079"); else it_fail("T079", why);
}

void test_t080(void) {
    /* Stage 4: the self-process cap is invoked as a CPtr; it never becomes
     * a handle.  SYS_PROC_CSPACE_MINT resolves it through CSpace. */
    const long selfp = (long)IRIS_CPTR_TEST_PROC;
    if (it_invoke0(selfp, INV_CAP_IDENTIFY) < 0) { it_fail("T080", "self proc cptr"); return; }

    long vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, T080_VMO_SIZE);
    if (vmo < 0) { it_fail("T080", "frame create"); return; }
    iris_cptr_t vmo_h = (iris_cptr_t)vmo;

    int ok = 1;
    const char *why = "frame family by cptr";

    /* Mint the VMO into our own CSpace: slot 19 rw+dup, slot 20 read-only. */
    if (it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_SELF(T080_SLOT_RWD), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0) { ok = 0; why = "mint rwd"; }
    if (ok && it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_SELF(T080_SLOT_RO), (long)RIGHT_READ) != 0) { ok = 0; why = "mint ro"; }

    /* ── SYS_FRAME_SIZE ── */
    if (ok && it_invoke0(T080_SLOT_RWD, INV_FRAME_SIZE) != (long)T080_VMO_SIZE) { ok = 0; why = "size by cptr"; }
    if (ok && it_invoke0(T079_SLOT_EMPTY, INV_FRAME_SIZE) >= 0) { ok = 0; why = "size of empty slot"; }
    if (ok && it_invoke0((long)IRIS_CPTR_TEST_FIX_A, INV_FRAME_SIZE) !=
              (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "size wrong type"; }
    if (ok && it_invoke0(vmo, INV_FRAME_SIZE) != (long)T080_VMO_SIZE) { ok = 0; why = "size by source"; }

    /* SHARE/MAP_INTO target: a lifecycle_probe child (blocks in EP_RECV). */
    long ep = it_ep_create();
    if (ep < 0) { it_close(&vmo_h);                  it_fail("T080", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;
    iris_cptr_t proc_h   = IRIS_CPTR_NULL;
    it_child_keep_vspace();   /* T080 maps into the child */
    if (lp_spawn_child_cn(1u, cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h); it_close(&vmo_h);        it_fail("T080", "spawn"); return;
    }

    /* ── Cross-process delegation (vmo by CPtr; dest process a handle) ──
     * SYS_VMO_SHARE is retired: it put the cap in the CHILD'S HANDLE TABLE,
     * where the child could not name it and the grantor could not revoke it.
     * SYS_PROC_CSPACE_MINT asserts the same three properties against the
     * child's CSpace: a rights-carrying source delegates, a source missing
     * RIGHT_DUPLICATE is denied, and a wrong-type source is rejected. */
    if (ok && it_invoke2(T080_SLOT_RWD, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T080_DST_SLOT), (long)(RIGHT_READ | RIGHT_WRITE)) != 0) { ok = 0; why = "delegate"; }
    if (ok && it_invoke2(T080_SLOT_RO, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T080_DST_SLOT2), (long)RIGHT_READ)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "delegate without DUP"; }
    if (ok && it_invoke2((long)IRIS_CPTR_TEST_FIX_A, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T080_DST_SLOT2), (long)RIGHT_READ) >= 0) { ok = 0; why = "delegate wrong type"; }

    /* ── SYS_FRAME_MAP (vmo by CPtr; Stage 7 Step 9: the TARGET is the
     * child's address space, named directly, not its process) ── */
    long t080_vs = it_child_vspace(proc_h);
    if (ok && t080_vs < 0) { ok = 0; why = "child vspace"; }
    if (ok && it_invoke(T080_SLOT_RWD, INV_FRAME_MAP, t080_vs, (long)LP_MAP_VA, 1) != 0) { ok = 0; why = "map into child"; }
    /* Same VA again → BUSY: the CPtr mapping really installed PTEs. */
    if (ok && it_invoke(T080_SLOT_RWD, INV_FRAME_MAP, t080_vs, (long)LP_MAP_VA, 1) != (long)IRIS_ERR_BUSY) { ok = 0; why = "remap not busy"; }
    if (ok && it_invoke(T080_SLOT_RO, INV_FRAME_MAP, t080_vs, (long)(LP_MAP_VA + 0x10000ULL), 1) !=
              (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro mapped writable"; }
    if (ok && it_invoke(T079_SLOT_EMPTY, INV_FRAME_MAP, t080_vs, (long)(LP_MAP_VA + 0x10000ULL), 1) >= 0) { ok = 0; why = "empty slot mapped"; }
    if (ok && it_invoke((long)IRIS_CPTR_TEST_FIX_A, INV_FRAME_MAP, t080_vs, (long)(LP_MAP_VA + 0x10000ULL), 1) !=
              (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "map wrong type"; }
    /* ...and a PROCESS capability is no longer accepted as the target: the
     * argument names an address space, and a process is not one. */
    if (ok && it_invoke(T080_SLOT_RWD, INV_FRAME_MAP, (long)proc_h, (long)(LP_MAP_VA + 0x20000ULL), 1) !=
              (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "process as vspace"; }

    /* Cleanup: kill the child (auto-unmaps, T076-proven), close everything —
     * including the child's address space, which this test asked the spawn to
     * keep and must therefore give back (Stage 7 Step 15). */
    (void)it_kill((long)proc_h);
    it_child_drop_vspace(proc_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);
    it_close(&vmo_h);

    if (ok) it_pass("T080"); else it_fail("T080", why);
}

void test_t081(void) {
    long ep = it_ep_create();
    if (ep < 0) { it_fail("T081", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;

    iris_cptr_t proc_h = IRIS_CPTR_NULL;
    if (lp_spawn_child_cn(1u, cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h);
        it_fail("T081", "spawn"); return;
    }

    int ok = 1;
    const char *why = "process by cptr";

    /* Mint the child's THREAD into our CSpace at two rights levels.  The kept
     * thread carries READ|WRITE|DUPLICATE, so the full slot is exactly that —
     * a mint cannot hand out authority its source does not hold, which is
     * itself part of what this test is for. */
    long ctcb = it_child_tcb(proc_h);
    if (ctcb == 0) { ok = 0; why = "no child thread"; }
    if (ok && it_invoke2(ctcb, INV_CSPACE_MINT, IT_MINT_SELF(T081_SLOT_PROC), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0)
        { ok = 0; why = "mint full"; }
    if (ok && it_invoke2(ctcb, INV_CSPACE_MINT, IT_MINT_SELF(T081_SLOT_RO), (long)(RIGHT_READ | RIGHT_DUPLICATE)) != 0)
        { ok = 0; why = "mint ro"; }
    /* ...and a mint cannot amplify.  Asking for rights the source does not
     * hold does not fail — seL4's rule is that rights only ever narrow, so the
     * request is REDUCED to what the source has — which means the proof is not
     * that the mint was refused but that the result still cannot do the thing
     * the missing right authorises. */
    long camp = ok ? it_cs_reduce(T081_SLOT_RO,
                                  RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE) : -1;
    if (ok && camp < 0) { ok = 0; why = "mint amplify"; }
    if (ok && it_invoke0(camp, INV_TCB_EXIT) != (long)IRIS_ERR_ACCESS_DENIED)
        { ok = 0; why = "mint amplified"; }

    /* Authority not relaxed: minting INTO the child through a READ-only
     * capability to its root CNode (no RIGHT_WRITE) must be denied. */
    long cro = it_cs_reduce(IT_CHILD_CN_CPTR(0), RIGHT_READ);
    if (ok && cro < 0) { ok = 0; why = "reduce child cnode"; }
    if (ok && it_invoke2((long)proc_h, INV_CSPACE_MINT, IT_MINT_INTO(cro, 60L), (long)RIGHT_READ) != (long)IRIS_ERR_ACCESS_DENIED)
        { ok = 0; why = "mint into ro cnode"; }

    /* Liveness by CPtr: alive via both slots; empty / wrong-type fail. */
    if (ok && it_tcb_alive(T081_SLOT_PROC) != 1) { ok = 0; why = "alive full"; }
    if (ok && it_tcb_alive(T081_SLOT_RO) != 1) { ok = 0; why = "alive ro"; }
    if (ok && it_tcb_alive(T079_SLOT_EMPTY) >= 0) { ok = 0; why = "alive empty"; }
    if (ok && it_tcb_alive((long)IRIS_CPTR_TEST_FIX_A) !=
              (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "alive wrong-type"; }

    /* EXIT_CODE by CPtr while alive → WOULD_BLOCK.  Ledger A-22: the
     * FAULT_INFO half of this probe is RETIRED — there is no syscall that
     * reads a thread's fault any more, because the fault is a message its
     * handler received.  What survives is the assertion that the number is
     * gone for everyone, whatever they hold. */
    if (ok && it_invoke0(it_child_tcb(proc_h), INV_TCB_EXIT_CODE) !=
              (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "exit code alive"; }
    {
        static uint8_t fault_buf[32];
        if (ok && it_sys2(SYS_TCB_FAULT_INFO, it_child_tcb(proc_h),
                          (long)(uintptr_t)fault_buf) !=
                  (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "fault info alive"; }
    }

    /* KILL via the READ-only slot → ACCESS_DENIED (needs RIGHT_WRITE).
     * Stage 7: the THREAD_START half of this check retired with the syscall —
     * a spawned process's first thread is composed from capabilities now, and
     * T297 is where that authority is checked. */
    if (ok && it_invoke0(T081_SLOT_RO, INV_TCB_EXIT) !=
              (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro killed"; }
    if (ok && it_sys4(SYS_THREAD_START, T081_SLOT_RO, 0x8000200000L,
                      0x8000300000L, 0) != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "thread_start ro"; }

    /* WATCH by CPtr, then KILL by CPtr; the watch must fire. */
    long n = it_notify_create();
    iris_cptr_t watch_h = (n >= 0) ? (iris_cptr_t)n : IRIS_CPTR_NULL;
    if (watch_h == IRIS_CPTR_NULL) { ok = 0; why = "notify"; }
    if (ok && it_invoke2(it_child_tcb(proc_h), INV_TCB_WATCH, (long)watch_h, 1) != 0)
        { ok = 0; why = "watch"; }
    if (ok && it_invoke0(T081_SLOT_PROC, INV_TCB_EXIT) != 0) { ok = 0; why = "kill"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_wait_timeout( (long)watch_h,
                    (long)(uintptr_t)&bits, 2000000000L) != 0 || !(bits & 1u))
            { ok = 0; why = "watch did not fire"; }
    }

    /* Dead child by CPtr: STATUS 0, EXIT_CODE readable, KILL idempotent,
     * and the retired THREAD_START answers NOT_SUPPORTED whatever it is given. */
    if (ok && it_tcb_alive(T081_SLOT_PROC) != 0) { ok = 0; why = "dead still alive"; }
    if (ok && it_invoke0(it_child_tcb(proc_h), INV_TCB_EXIT_CODE) < 0) { ok = 0; why = "exit code dead"; }
    if (ok && it_invoke0(T081_SLOT_PROC, INV_TCB_EXIT) != 0) { ok = 0; why = "kill not idempotent"; }
    if (ok && it_sys4(SYS_THREAD_START, T081_SLOT_PROC, 0x8000200000L,
                      0x8000300000L, 0) != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "thread_start dead"; }

    it_close(&watch_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);

    if (ok) it_pass("T081"); else it_fail("T081", why);
}

void test_t082(void) {
    const char *why = "frame ops proc by cptr";
    long vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);
    if (vmo < 0) { it_fail("T082", "frame create"); return; }
    iris_cptr_t vmo_h = (iris_cptr_t)vmo;

    long ep = it_ep_create();
    if (ep < 0) { it_close(&vmo_h); it_fail("T082", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;

    iris_cptr_t proc_h = IRIS_CPTR_NULL;
    /* Stage 7 Step 9: keep the child's ROOT CSPACE.  Delegating into a child
     * names the CSpace, so a spawner that intends to keep delegating keeps it
     * — there is no longer a way to reach it by naming the process instead.
     * Step 15: and its ADDRESS SPACE, for the same reason, one object over. */
    it_child_keep_vspace();
    if (lp_spawn_child_cn(1u, cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h); it_close(&vmo_h);
        it_fail("T082", "spawn"); return;
    }

    int ok = 1;

    /* Mint fixtures (target proc by CPtr 25): VMO → 23, child proc → 24. */
    if (it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_SELF(T082_SLOT_VMO), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0)
        ok = 0;
    if (ok && it_invoke2((long)proc_h, INV_CSPACE_MINT, IT_MINT_SELF(T082_SLOT_PROC), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE |
                             RIGHT_DUPLICATE)) != 0) ok = 0;

    /* MAP_INTO: VMO by CPtr + ADDRESS SPACE by CPtr; repeat → BUSY (PTEs
     * real).  Stage 7 Step 9: the target argument is the VSpace. */
    long t082_vs = it_child_vspace(proc_h);
    if (ok && t082_vs < 0) ok = 0;
    if (ok && it_invoke(T082_SLOT_VMO, INV_FRAME_MAP, t082_vs, (long)LP_MAP_VA, 1) != 0) ok = 0;
    if (ok && it_invoke(T082_SLOT_VMO, INV_FRAME_MAP, t082_vs, (long)LP_MAP_VA, 1) != (long)IRIS_ERR_BUSY) ok = 0;

    /* Cross-CSpace placement: the destination CNODE by CPtr, into the child's
     * CSpace.  SYS_VMO_SHARE and SYS_HANDLE_INSERT covered this by writing the
     * child's HANDLE TABLE and are retired; Stage 7 Step 9 retired naming the
     * child's PROCESS to reach a CSpace the caller did not hold.  The property
     * under test — a destination named by CPtr really is resolved — is now
     * asserted against the thing being written. */
    if (ok && it_invoke2(T082_SLOT_VMO, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T080_DST_SLOT), (long)(RIGHT_READ | RIGHT_WRITE)) != 0) ok = 0;

    /* Authority not relaxed.  Stage 7 Step 9 moved the target from the process
     * to the address space, so the denial moved with it: a VSpace capability
     * without RIGHT_WRITE cannot receive a mapping, and a PROCESS capability
     * is refused outright because it is not an address space at all. */
    {
        long vs_ro = (t082_vs >= 0) ? it_cs_reduce(t082_vs, RIGHT_READ) : -1;
        if (ok && vs_ro < 0) ok = 0;
        if (ok && it_invoke(T082_SLOT_VMO, INV_FRAME_MAP, vs_ro, (long)T082_MAP_VA2, 1) != (long)IRIS_ERR_ACCESS_DENIED)
            ok = 0;
        if (vs_ro >= 0) { iris_cptr_t h = (iris_cptr_t)vs_ro; it_close(&h); }
    }
    if (ok && it_invoke(T082_SLOT_VMO, INV_FRAME_MAP, (long)IRIS_CPTR_TEST_PROC, (long)T082_MAP_VA2, 1) != (long)IRIS_ERR_WRONG_TYPE)
        { ok = 0; why = "process as vspace"; }
    if (ok && it_invoke(T082_SLOT_VMO, INV_FRAME_MAP, (long)IRIS_CPTR_TEST_FIX_A, (long)T082_MAP_VA2, 1) != (long)IRIS_ERR_WRONG_TYPE)
        { ok = 0; why = "notification as vspace"; }
    if (ok && it_invoke2(T082_SLOT_VMO, INV_CSPACE_MINT, IT_MINT_INTO(T079_SLOT_EMPTY, T080_DST_SLOT2), (long)RIGHT_READ) >= 0) ok = 0;

    /* The same map through the address space named directly. */
    if (ok && it_invoke(vmo, INV_FRAME_MAP, t082_vs, (long)T082_MAP_VA2, 1) != 0) ok = 0;

    /* Cleanup: kill via the old handle path (still must work). */
    if (ok && it_kill((long)proc_h) != 0) ok = 0;
    if (ok && it_alive((long)proc_h) != 0) ok = 0;

    it_child_drop_vspace(proc_h);   /* Step 15: give the child's back */
    it_close(&proc_h);
    it_close(&cmd_ep_h);
    it_close(&vmo_h);

    if (ok) it_pass("T082"); else it_fail("T082", why);
}

static volatile uint64_t g_t083_count = 0;
static volatile int      g_t083_ready = 0;
static long              g_t083_tcb   = -1;
static uint8_t           g_t083_stack[8192];

static void t083_helper(uint64_t self_tcb) {
    /* Handed in by whoever created this thread, in the entry register.  It used
     * to be SYS_TCB_SELF: a capability to yourself, for the asking. */
    g_t083_tcb   = (long)self_tcb;
    g_t083_ready = 1;
    for (;;) {
        g_t083_count++;
        it_sys0(SYS_YIELD);
    }
}

void test_t083(void) {
    g_t083_count = 0; g_t083_ready = 0; g_t083_tcb = -1;

    uint64_t entry = (uint64_t)(uintptr_t)t083_helper;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t083_stack + sizeof(g_t083_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, IT_THREAD_ARG_SELF_TCB);
    if (tid < 0) { it_fail("T083", "thread create"); return; }

    IT_AWAIT(g_t083_ready, 200);
    if (!g_t083_ready || g_t083_tcb < 0) { it_fail("T083", "tcb self"); return; }
    iris_cptr_t tcb_h = (iris_cptr_t)g_t083_tcb;

    int ok = 1;

    /* Mint the helper's TCB: slot 32 rw+dup, slot 33 read-only. */
    if (it_invoke2((long)tcb_h, INV_CSPACE_MINT, IT_MINT_SELF(T083_SLOT_TCB), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0) ok = 0;
    if (ok && it_invoke2((long)tcb_h, INV_CSPACE_MINT, IT_MINT_SELF(T083_SLOT_TCB_RO), (long)RIGHT_READ) != 0)
        ok = 0;

    /* GET_INFO by CPtr (both slots — READ suffices).
     * Stage 5 Step 4: thread creation returns a capability, not a global
     * thread id, so the identity checked here is the one the helper's own TCB
     * capability reports — the two CPtrs must name the SAME object. */
    struct iris_tcb_info info, self_info;
    if (ok && it_invoke1((long)tcb_h, INV_TCB_GET_INFO, (long)(uintptr_t)&self_info) != 0) ok = 0;
    if (ok && it_invoke1(T083_SLOT_TCB, INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0) ok = 0;
    if (ok && info.task_id != self_info.task_id) ok = 0;
    if (ok && it_invoke1(T083_SLOT_TCB_RO, INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0) ok = 0;
    if (ok && it_invoke1((long)tcb_h, INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0) ok = 0;

    /* SET_PRIORITY by CPtr: change, verify via GET_INFO, restore. */
    uint8_t old_prio = info.priority;
    if (ok && it_invoke1(T083_SLOT_TCB, INV_TCB_SET_PRIORITY, (long)(old_prio + 1u)) != 0) ok = 0;
    if (ok && (it_invoke1(T083_SLOT_TCB, INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0 ||
               info.priority != (uint8_t)(old_prio + 1u))) ok = 0;
    if (ok && it_invoke1(T083_SLOT_TCB, INV_TCB_SET_PRIORITY, (long)old_prio) != 0)
        ok = 0;

    /* SUSPEND by CPtr: the helper's counter must freeze. */
    if (ok && it_invoke0(T083_SLOT_TCB, INV_TCB_SUSPEND) != 0) ok = 0;
    if (ok) {
        uint64_t before = g_t083_count;
        it_settle(5);
        if (g_t083_count != before) ok = 0;
    }

    /* RESUME by CPtr: the counter must advance again. */
    if (ok && it_invoke0(T083_SLOT_TCB, INV_TCB_RESUME) != 0) ok = 0;
    if (ok) {
        uint64_t before = g_t083_count;
        it_settle(5);
        if (g_t083_count == before) ok = 0;
    }

    /* Authority not relaxed + failure paths (TCB). */
    if (ok && it_invoke0(T083_SLOT_TCB_RO, INV_TCB_SUSPEND) !=
              (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_invoke1(T083_SLOT_TCB_RO, INV_TCB_SET_PRIORITY, 1) !=
              (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_invoke0(T079_SLOT_EMPTY, INV_TCB_SUSPEND) >= 0) ok = 0;
    if (ok && it_invoke0((long)IRIS_CPTR_TEST_FIX_A, INV_TCB_SUSPEND) !=
              (long)IRIS_ERR_WRONG_TYPE) ok = 0;

    /* ── SchedContext (Phase S2: SYS_SC_CREATE retired → RETYPE2) ── */
    if (ok && it_sys0(SYS_SC_CREATE) != (long)IRIS_ERR_NOT_SUPPORTED) ok = 0;
    long sc = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_SCHED_CONTEXT, 0);
    if (sc < 0) ok = 0;
    iris_cptr_t sc_h = (sc >= 0) ? (iris_cptr_t)sc : IRIS_CPTR_NULL;

    if (ok && it_invoke2((long)sc_h, INV_CSPACE_MINT, IT_MINT_SELF(T083_SLOT_SC), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0)
        ok = 0;
    if (ok && it_invoke2((long)sc_h, INV_CSPACE_MINT, IT_MINT_SELF(T083_SLOT_SC_RO), (long)RIGHT_READ) != 0)
        ok = 0;

    /* SC_CONFIGURE by CPtr (budget < period required); handle path too. */
    if (ok && it_invoke(T083_SLOT_SC, INV_SC_CONFIGURE, 50, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) ok = 0;
    if (ok && it_invoke((long)sc_h, INV_SC_CONFIGURE, 50, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) ok = 0;
    if (ok && it_invoke(T083_SLOT_SC_RO, INV_SC_CONFIGURE, 50, 100, (long)IRIS_CPTR_SCHED_CONTROL) !=
              (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_invoke(T079_SLOT_EMPTY, INV_SC_CONFIGURE, 50, 100, (long)IRIS_CPTR_SCHED_CONTROL) >= 0) ok = 0;
    /* A-30: an endpoint where a SchedContext belongs is named, not flattened. */
    if (ok && it_invoke((long)IRIS_CPTR_TEST_FIX_A, INV_SC_CONFIGURE, 50, 100, (long)IRIS_CPTR_SCHED_CONTROL) !=
              (long)IRIS_ERR_WRONG_TYPE) ok = 0;

    /* THREAD_SET_SC by CPtr: bind the calling thread, then unbind (0). */
    if (ok && it_invoke0(T083_SLOT_SC, INV_SC_SET_ON_CALLER) != 0) ok = 0;
    if (ok && it_invoke0(0, INV_SC_SET_ON_CALLER) != 0) ok = 0;
    if (ok && it_invoke0((long)IRIS_CPTR_TEST_FIX_A, INV_SC_SET_ON_CALLER) !=
              (long)IRIS_ERR_WRONG_TYPE) ok = 0;
    if (ok && it_invoke0(T079_SLOT_EMPTY, INV_SC_SET_ON_CALLER) >= 0) ok = 0;

    /* TCB_EXIT by CPtr on the helper (non-self): counter freezes for good. */
    if (ok && it_invoke0(T083_SLOT_TCB, INV_TCB_EXIT) != 0) ok = 0;
    if (ok) {
        it_settle(2);
        uint64_t before = g_t083_count;
        it_settle(5);
        if (g_t083_count != before) ok = 0;
    }

    it_close(&sc_h);
    { iris_cptr_t th = tcb_h; it_close(&th); }

    if (ok) it_pass("T083"); else it_fail("T083", "tcb/sc by cptr");
}

static iris_cptr_t  g_t084_cmd_ep = IRIS_CPTR_NULL;
static iris_cptr_t  g_t084_cap1   = IRIS_CPTR_NULL;
static iris_cptr_t  g_t084_cap2   = IRIS_CPTR_NULL;
static volatile int g_t084_s1 = 999, g_t084_s2 = 999, g_t084_done = 0;
static uint8_t      g_t084_stack[8192];

static void t084_sender(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label           = 0x84;
    m.cap = (uint32_t)g_t084_cap1;
    m.cap_rights = RIGHT_WRITE;
    g_t084_s1 = (int)iris_msg_send((long)g_t084_cmd_ep, &m);
    iris_msg_zero(&m);
    m.label           = 0x84;
    m.cap = (uint32_t)g_t084_cap2;
    m.cap_rights = RIGHT_WRITE;
    g_t084_s2 = (int)iris_msg_send((long)g_t084_cmd_ep, &m);
    g_t084_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t084(void) {
    g_t084_s1 = 999; g_t084_s2 = 999; g_t084_done = 0;

    long epx = it_ep_create();   /* the cap being transferred */
    long cmd = it_ep_create();   /* the transfer channel */
    if (epx < 0 || cmd < 0) { it_fail("T084", "ep create"); return; }
    iris_cptr_t epx_h = (iris_cptr_t)epx;
    g_t084_cmd_ep = (iris_cptr_t)cmd;

    /* Phase S4 (Step 2): transfer sources are CSpace SLOTS, not handles.
     * EP_SEND consumes the slot exactly as it used to consume the dup. */
    long c1 = it_xfer_slot(epx_h, IT_XFER_SLOT_A, RIGHT_WRITE);
    long c2 = it_xfer_slot(epx_h, IT_XFER_SLOT_B, RIGHT_WRITE);
    if (c1 < 0 || c2 < 0) {
        it_close(&epx_h); it_close(&g_t084_cmd_ep);
        it_fail("T084", "xfer slot"); return;
    }
    g_t084_cap1 = (iris_cptr_t)c1;
    g_t084_cap2 = (iris_cptr_t)c2;

    uint64_t entry = (uint64_t)(uintptr_t)t084_sender;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t084_stack + sizeof(g_t084_stack))) & ~0xFULL;
    if (it_thread_create(entry, rsp, 0) < 0) {
        it_close(&epx_h); it_close(&g_t084_cmd_ep);
        it_fail("T084", "thread create"); return;
    }
    it_settle(2);   /* let the sender queue its first send */

    int ok = 1;
    struct iris_msg r;

    /* recv #1: declare slot 36 → the cap must land there, as a CPtr. */
    iris_msg_zero(&r);
    r.recv_slot = T084_SLOT;
    if (iris_msg_recv((long)g_t084_cmd_ep, &r) != 0) ok = 0;
    if (ok && r.got_cap != T084_SLOT) ok = 0;
    if (ok) {
        struct iris_msg probe;
        iris_msg_zero(&probe);
        probe.label = 0x84;
        if (iris_msg_nb_send((long)T084_SLOT, &probe) !=
            (long)IRIS_ERR_WOULD_BLOCK) ok = 0;   /* resolves via CSpace */
    }

    /* recv #2: NO declaration.  Stage 4 retired handle materialization, so the
     * message arrives WITHOUT the capability — the same fail-closed shape a
     * raced or occupied slot has had since Step 2.  The sender is not left
     * hanging and nothing is half-transferred; a receiver that wants the cap
     * says where to put it. */
    iris_msg_zero(&r);
    if (ok && iris_msg_recv((long)g_t084_cmd_ep, &r) != 0) ok = 0;
    if (ok && r.got_cap != (uint32_t)IRIS_MSG_NO_CAP) ok = 0;

    IT_AWAIT(g_t084_done, 200);
    if (!g_t084_done || g_t084_s1 != 0 || g_t084_s2 != 0) ok = 0;

    it_close(&epx_h);
    it_close(&g_t084_cmd_ep);

    if (ok) it_pass("T084"); else it_fail("T084", "recv-slot basic");
}

static iris_cptr_t  g_t085_cmd_ep = IRIS_CPTR_NULL;
static iris_cptr_t  g_t085_cap    = IRIS_CPTR_NULL;
static volatile int g_t085_s1 = 999, g_t085_done = 0;
static uint8_t      g_t085_stack[8192];

static void t085_sender(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label           = 0x85;
    m.cap = (uint32_t)g_t085_cap;
    m.cap_rights = RIGHT_WRITE;            /* reduce: drop WAIT et al. */
    g_t085_s1 = (int)iris_msg_send((long)g_t085_cmd_ep, &m);
    g_t085_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t085(void) {
    g_t085_s1 = 999; g_t085_done = 0;

    long n   = it_notify_create();
    long cmd = it_ep_create();
    if (n < 0 || cmd < 0) { it_fail("T085", "create"); return; }
    iris_cptr_t n_h = (iris_cptr_t)n;
    g_t085_cmd_ep = (iris_cptr_t)cmd;

    long c = it_xfer_dup( n, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (c < 0) {
        it_close(&n_h); it_close(&g_t085_cmd_ep);
        it_fail("T085", "dup"); return;
    }
    g_t085_cap = (iris_cptr_t)c;

    uint64_t entry = (uint64_t)(uintptr_t)t085_sender;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t085_stack + sizeof(g_t085_stack))) & ~0xFULL;
    if (it_thread_create(entry, rsp, 0) < 0) {
        it_close(&n_h); it_close(&g_t085_cmd_ep);
        it_fail("T085", "thread create"); return;
    }
    it_settle(2);

    int ok = 1;
    struct iris_msg r;
    iris_msg_zero(&r);
    r.recv_slot = T085_SLOT;
    if (iris_msg_recv((long)g_t085_cmd_ep, &r) != 0) ok = 0;
    if (ok && r.got_cap != T085_SLOT) ok = 0;

    /* Permitted op by CPtr: SIGNAL (RIGHT_WRITE survived the reduce). */
    if (ok && it_invoke1((long)T085_SLOT, INV_NOTIFY_SIGNAL, 0x85) != 0) ok = 0;
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x85u) ok = 0;   /* original handle sees it: same object */
    }

    /* Denied op by CPtr: WAIT (RIGHT_WAIT was reduced away). */
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1((long)T085_SLOT, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) !=
            (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    }

    IT_AWAIT(g_t085_done, 200);
    if (!g_t085_done || g_t085_s1 != 0) ok = 0;

    it_xfer_release((long)g_t085_cap);   /* A-29: sender's copy survived */
    it_close(&n_h);
    it_close(&g_t085_cmd_ep);

    if (ok) it_pass("T085"); else it_fail("T085", "recv-slot rights");
}

static iris_cptr_t  g_t086_cmd_ep = IRIS_CPTR_NULL;
static iris_cptr_t  g_t086_cap    = IRIS_CPTR_NULL;
static volatile int g_t086_s1 = 999, g_t086_done = 0;
static uint8_t      g_t086_stack[8192];

static void t086_sender(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label           = 0x86;
    m.cap = (uint32_t)g_t086_cap;
    m.cap_rights = RIGHT_WRITE;
    g_t086_s1 = (int)iris_msg_send((long)g_t086_cmd_ep, &m);
    g_t086_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t086(void) {
    g_t086_s1 = 999; g_t086_done = 0;

    long n   = it_notify_create();
    long cmd = it_ep_create();
    if (n < 0 || cmd < 0) { it_fail("T086", "create"); return; }
    iris_cptr_t n_h = (iris_cptr_t)n;
    g_t086_cmd_ep = (iris_cptr_t)cmd;

    long c = it_xfer_dup( n, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (c < 0) {
        it_close(&n_h); it_close(&g_t086_cmd_ep);
        it_fail("T086", "dup"); return;
    }
    g_t086_cap = (iris_cptr_t)c;

    uint64_t entry = (uint64_t)(uintptr_t)t086_sender;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t086_stack + sizeof(g_t086_stack))) & ~0xFULL;
    if (it_thread_create(entry, rsp, 0) < 0) {
        it_close(&n_h); it_close(&g_t086_cmd_ep);
        it_fail("T086", "thread create"); return;
    }
    it_settle(2);   /* sender is now queued with its staged cap */

    int ok = 1;
    struct iris_msg r;

    /* Occupied slot → ALREADY_EXISTS, fail-fast. */
    iris_msg_zero(&r);
    r.recv_slot = T084_SLOT;                   /* occupied since T084 */
    if (iris_msg_recv((long)g_t086_cmd_ep, &r) !=
        (long)IRIS_ERR_ALREADY_EXISTS) ok = 0;

    /* A CPtr whose path cannot be walked → INVALID_ARG.  Stage 4 made the
     * declaration a full CPtr, so 300 is no longer "past the end of the root
     * CNode": it is root slot 44 (300 & 255) followed by index 1, and slot 44
     * holds no CNode to descend into.  Rejected either way, and deliberately
     * still rejected — a receive slot must name a real, empty destination. */
    iris_msg_zero(&r);
    r.recv_slot = 300u;
    if (ok && iris_msg_recv((long)g_t086_cmd_ep, &r) !=
              (long)IRIS_ERR_INVALID_ARG) ok = 0;

    /* EP_NB_RECV validates the declaration the same way. */
    iris_msg_zero(&r);
    r.recv_slot = T084_SLOT;
    if (ok && iris_msg_nb_recv((long)g_t086_cmd_ep, &r) !=
              (long)IRIS_ERR_ALREADY_EXISTS) ok = 0;

    /* Atomicity: the sender is still blocked — nothing was consumed. */
    if (ok && g_t086_s1 != 999) ok = 0;

    /* A good declaration now receives the SAME cap, intact. */
    iris_msg_zero(&r);
    r.recv_slot = T086_SLOT;
    if (ok && iris_msg_recv((long)g_t086_cmd_ep, &r) != 0) ok = 0;
    if (ok && r.got_cap != T086_SLOT) ok = 0;
    if (ok && it_invoke1((long)T086_SLOT, INV_NOTIFY_SIGNAL, 0x86) != 0) ok = 0;
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x86u) ok = 0;
    }

    IT_AWAIT(g_t086_done, 200);
    if (!g_t086_done || g_t086_s1 != 0) ok = 0;

    it_xfer_release((long)g_t086_cap);   /* A-29 */
    it_close(&n_h);
    it_close(&g_t086_cmd_ep);

    if (ok) it_pass("T086"); else it_fail("T086", "recv-slot atomicity");
}

static iris_cptr_t       g_t087_ep   = IRIS_CPTR_NULL;
static iris_cptr_t       g_t087_capB = IRIS_CPTR_NULL;
static volatile uint32_t g_t087_got_cap = 0, g_t087_reply_h = 0;
static volatile int      g_t087_sig = 999, g_t087_r1 = 999, g_t087_r2 = 999;
static volatile int      g_t087_done = 0;
static uint8_t           g_t087_stack[8192];

static void t087_server(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.recv_slot = T087_SRV_SLOT;              /* receive-slot declaration */
    /* Phase S1: explicit reply object (slot 94) staged via recv arg2. */
    long rr = (m.reply = 94, iris_msg_recv((long)g_t087_ep, &m));
    if (rr == 0) {
        /* A-33: a receive of a CALL is handed TWO capabilities and they no
         * longer share a field.  The caller's gift is wherever the receiver
         * DECLARED it should go — the receiver knows that without being told
         * — and `got_cap` is the reply object it is now owed. */
        g_t087_got_cap = (uint32_t)m.recv_slot;
        g_t087_reply_h = (uint32_t)m.got_cap;
        g_t087_sig = (int)it_invoke1(m.recv_slot, INV_NOTIFY_SIGNAL, 0x87);

        struct iris_msg rm;
        iris_msg_zero(&rm);
        rm.label           = 0x87;
        rm.cap = (uint32_t)g_t087_capB;   /* cap back to caller */
        rm.cap_rights = RIGHT_WRITE;
        g_t087_r1 = (int)iris_msg_reply((long)g_t087_reply_h, &rm);

        iris_msg_zero(&rm);
        rm.label  = 0x87;
        g_t087_r2 = (int)iris_msg_reply((long)g_t087_reply_h, &rm);
    }
    g_t087_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t087(void) {
    g_t087_got_cap = 0; g_t087_reply_h = 0;
    g_t087_sig = 999; g_t087_r1 = 999; g_t087_r2 = 999; g_t087_done = 0;

    long nA = it_notify_create();
    long nB = it_notify_create();
    long ep = it_ep_create();
    if (nA < 0 || nB < 0 || ep < 0) { it_fail("T087", "create"); return; }
    if (it_reply_create_at(94) < 0) { it_fail("T087", "reply create"); return; }
    iris_cptr_t nA_h = (iris_cptr_t)nA, nB_h = (iris_cptr_t)nB;
    g_t087_ep = (iris_cptr_t)ep;

    long cA = it_xfer_dup( nA, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
    long cB = it_xfer_dup( nB, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (cA < 0 || cB < 0) {
        it_close(&nA_h); it_close(&nB_h); it_close(&g_t087_ep);
        it_fail("T087", "dup"); return;
    }
    g_t087_capB = (iris_cptr_t)cB;

    uint64_t entry = (uint64_t)(uintptr_t)t087_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t087_stack + sizeof(g_t087_stack))) & ~0xFULL;
    if (it_thread_create(entry, rsp, 0) < 0) {
        it_close(&nA_h); it_close(&nB_h); it_close(&g_t087_ep);
        it_fail("T087", "thread create"); return;
    }
    it_settle(2);   /* server blocks with slot 38 declared */

    int ok = 1;
    struct iris_msg cm;
    iris_msg_zero(&cm);
    cm.label              = 0x87;
    cm.recv_slot    = T087_REPLY_SLOT;      /* reply receive-slot */
    cm.cap       = (uint32_t)cA;         /* cap to the server */
    cm.cap_rights = RIGHT_WRITE;
    if (iris_msg_call((long)g_t087_ep, &cm) != 0) ok = 0;

    /* Reply's transferred cap landed in OUR declared slot 39. */
    if (ok && cm.got_cap != T087_REPLY_SLOT) ok = 0;
    if (ok && it_invoke1((long)T087_REPLY_SLOT, INV_NOTIFY_SIGNAL, 0x99) != 0) ok = 0;
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(nB, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x99u) ok = 0;
    }
    /* Server signalled notifA through its slot-38 CPtr. */
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(nA, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x87u) ok = 0;
    }

    IT_AWAIT(g_t087_done, 200);
    if (!g_t087_done) ok = 0;
    if (ok && g_t087_got_cap != T087_SRV_SLOT) ok = 0;      /* landed as CPtr */
    /* Phase S1: the reply value is the server's OWN reply-object CPtr (echoed
     * from recv arg2) — explicit MCS-style authority, never a fabricated
     * handle. */
    if (ok && g_t087_reply_h != 94u) ok = 0;
    if (ok && g_t087_sig != 0) ok = 0;                      /* invocable by CPtr */
    if (ok && g_t087_r1 != 0) ok = 0;                       /* first reply ok */
    if (ok && g_t087_r2 != (int)IRIS_ERR_NOT_FOUND) ok = 0; /* one-shot (T074) */

    it_xfer_release(cA);                 /* A-29: both copies survived */
    it_xfer_release((long)g_t087_capB);
    it_close(&nA_h);
    it_close(&nB_h);
    it_close(&g_t087_ep);
    it_slot_delete(94);

    if (ok) it_pass("T087"); else it_fail("T087", "recv-slot ep_call");
}

static iris_cptr_t       g_t088_ep  = IRIS_CPTR_NULL;
static iris_cptr_t       g_t088_ep2 = IRIS_CPTR_NULL;
static long              g_t088_r1_tcb = -1;
static volatile int      g_t088_r1_ready = 0;
static volatile uint32_t g_t088_r2_got = 0;
static volatile int      g_t088_r2_sig = 999, g_t088_r2_done = 0;
static volatile long     g_t088_r3_rr = 999;
static volatile int      g_t088_r3_done = 0;
static uint8_t           g_t088_stack1[8192];
static uint8_t           g_t088_stack2[8192];
static uint8_t           g_t088_stack3[8192];

static void t088_recv1(uint64_t self_tcb) {   /* killed while blocked with slot declared */
    struct iris_msg m;
    g_t088_r1_tcb   = (long)self_tcb;
    g_t088_r1_ready = 1;
    iris_msg_zero(&m);
    m.recv_slot = T088_SLOT_A;
    (void)iris_msg_recv((long)g_t088_ep, &m);
    it_sys1(SYS_EXIT, 0);    /* not reached: killed while blocked */
    for (;;) {}
}

static void t088_recv2(void) {   /* real transfer into the same slot */
    struct iris_msg m;
    iris_msg_zero(&m);
    m.recv_slot = T088_SLOT_A;
    if (iris_msg_recv((long)g_t088_ep, &m) == 0) {
        g_t088_r2_got = m.got_cap;
        g_t088_r2_sig = (int)it_invoke1((long)m.got_cap, INV_NOTIFY_SIGNAL, 0x88);
    }
    g_t088_r2_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

static void t088_recv3(void) {   /* endpoint closed under a declared slot */
    struct iris_msg m;
    iris_msg_zero(&m);
    m.recv_slot = T088_SLOT_C;
    g_t088_r3_rr   = iris_msg_recv((long)g_t088_ep2, &m);
    g_t088_r3_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t088(void) {
    g_t088_r1_tcb = -1; g_t088_r1_ready = 0;
    g_t088_r2_got = 0; g_t088_r2_sig = 999; g_t088_r2_done = 0;
    g_t088_r3_rr = 999; g_t088_r3_done = 0;

    long n   = it_notify_create();
    long ep  = it_ep_create();
    long ep2 = it_ep_create();
    if (n < 0 || ep < 0 || ep2 < 0) { it_fail("T088", "create"); return; }
    iris_cptr_t n_h = (iris_cptr_t)n;
    g_t088_ep  = (iris_cptr_t)ep;
    g_t088_ep2 = (iris_cptr_t)ep2;

    int ok = 1;

    /* ── A: kill a receiver blocked with a declared slot ── */
    {
        uint64_t entry = (uint64_t)(uintptr_t)t088_recv1;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t088_stack1 + sizeof(g_t088_stack1))) & ~0xFULL;
        if (it_thread_create(entry, rsp, IT_THREAD_ARG_SELF_TCB) < 0) ok = 0;
        IT_AWAIT(g_t088_r1_ready, 200);
        it_settle(2);            /* let it block in EP_RECV */
        if (ok && (g_t088_r1_tcb < 0 ||
                   it_invoke0(g_t088_r1_tcb, INV_TCB_EXIT) != 0)) ok = 0;
        /* No ghost cap in the slot; no stale receiver on the endpoint. */
        if (ok && it_invoke0((long)T088_SLOT_A, INV_CAP_IDENTIFY) >= 0) ok = 0;
        if (ok) {
            struct iris_msg p;
            iris_msg_zero(&p);
            p.label = 0x88;
            if (iris_msg_nb_send((long)g_t088_ep, &p) !=
                (long)IRIS_ERR_WOULD_BLOCK) ok = 0;
        }
    }

    /* ── B: the same slot serves a real transfer (send-side delivery) ── */
    if (ok) {
        uint64_t entry = (uint64_t)(uintptr_t)t088_recv2;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t088_stack2 + sizeof(g_t088_stack2))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) ok = 0;
        it_settle(2);            /* receiver blocks FIRST */
        long c = it_xfer_dup( n, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (c < 0) ok = 0;
        if (ok) {
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label           = 0x88;
            m.cap = (uint32_t)c;
            m.cap_rights = RIGHT_WRITE;
            if (iris_msg_send((long)g_t088_ep, &m) != 0) ok = 0;
        }
        it_xfer_release(c);
        IT_AWAIT(g_t088_r2_done, 200);
        if (!g_t088_r2_done || g_t088_r2_got != T088_SLOT_A ||
            g_t088_r2_sig != 0) ok = 0;
        if (ok) {
            uint64_t bits = 0;
            if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
                bits != 0x88u) ok = 0;
        }
    }

    /* ── C: endpoint close under a declared slot ── */
    if (ok) {
        uint64_t entry = (uint64_t)(uintptr_t)t088_recv3;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t088_stack3 + sizeof(g_t088_stack3))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) ok = 0;
        it_settle(2);            /* let it block with slot 41 declared */
        it_close(&g_t088_ep2);            /* close wakes the blocked receiver */
        IT_AWAIT(g_t088_r3_done, 200);
        if (!g_t088_r3_done || g_t088_r3_rr != (long)IRIS_ERR_CLOSED) ok = 0;
        if (ok && it_invoke0((long)T088_SLOT_C, INV_CAP_IDENTIFY) >= 0) ok = 0;
    }

    it_close(&n_h);
    it_close(&g_t088_ep);
    it_close(&g_t088_ep2);

    if (ok) it_pass("T088"); else it_fail("T088", "recv-slot death cleanup");
}

/* ── A1.6: in-tree receive-slot service flows (T089–T092) ───────────────────
 * The receive-slot mechanism (T084–T088) as used by REAL services: svcmgr
 * stores REGISTER caps in its own CSpace (pool slots 64..255) and clients
 * receive LOOKUP caps into declared reply-slots.  Test slots here: 48..50. */

/* LOOKUP `name` over svcmgr.ep declaring `reply_slot` (0 = legacy). */
long it_lookup_name_slot(const char *name, uint32_t reply_slot,
                                struct iris_msg *msg) {
    uint32_t len = it_stage_path(name);
    iris_msg_zero(msg);
    msg->label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg->buf_len  = len;
    msg->recv_slot = (long)reply_slot;   /* where the reply's capability lands */
    return iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, msg);
}

/* UNREGISTER dynamic id; 0 on success, -(error) on reply ERR. */
long it_unregister_id(uint32_t id) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label      = IRIS_SVCMGR_EP_UNREGISTER;
    msg.words[0]   = id;
    msg.word_count = 1u;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    if (r != 0) return r;
    return (msg.label == IRIS_EP_REPLY_OK) ? 0 : -(long)(uint32_t)msg.words[0];
}

/* ── T089: svcmgr CSpace-backed registration lifecycle (A1.6) ───────────────
 * The REGISTER cap now lands in svcmgr's CSpace via its declared
 * receive-slot.  A legacy client (no reply-slot) must observe identical
 * behavior end to end: register → lookup returns a working handle (>= 1024)
 * duplicated from the CSpace-held master → unregister releases the pool slot
 * (lookup → NOT_FOUND) → the same name registers again (slot reuse). */
void test_t089(void) {
    long e = it_ep_create();
    if (e < 0) { it_fail("T089", "ep create"); return; }
    iris_cptr_t ep = (iris_cptr_t)e;
    int ok = 1;

    long id = it_register_ep("t89.svc", ep);
    if (id < 0) ok = 0;

    struct iris_msg msg;
    if (ok) {
        /* Lookup into a declared slot: the cap is minted from the CSpace
         * master svcmgr holds and lands in the client's CSpace. */
        it_slot_delete((uint32_t)IT_LOOKUP_TMP);
        if (it_lookup_name_slot("t89.svc", (uint32_t)IT_LOOKUP_TMP, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.got_cap != (uint32_t)IT_LOOKUP_TMP) ok = 0;
        if (ok) {
            /* Invocable: no receiver on the endpoint → WOULD_BLOCK proves
             * resolution + rights (T084 probe). */
            struct iris_msg p;
            iris_msg_zero(&p);
            p.label = 0x89;
            if (iris_msg_nb_send((long)IT_LOOKUP_TMP, &p) !=
                (long)IRIS_ERR_WOULD_BLOCK) ok = 0;
        }
        it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    }

    /* Unregister releases svcmgr's CSpace pool slot cleanly. */
    if (ok && it_unregister_id((uint32_t)id) != 0) ok = 0;
    if (ok) {
        if (it_lookup_name_slot("t89.svc", 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR ||
            msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) ok = 0;
    }

    /* Slot reuse: the same name registers again after the release. */
    if (ok) {
        long id2 = it_register_ep("t89.svc", ep);
        if (id2 < 0) ok = 0;
        else if (it_unregister_id((uint32_t)id2) != 0) ok = 0;
    }

    it_close(&ep);
    if (ok) it_pass("T089"); else it_fail("T089", "cspace-backed register");
}

void test_t090(void) {
    long e = it_ep_create();
    if (e < 0) { it_fail("T090", "ep create"); return; }
    iris_cptr_t ep = (iris_cptr_t)e;
    int ok = 1;

    long id = it_register_ep("t90.svc", ep);
    if (id < 0) ok = 0;

    struct iris_msg msg;
    if (ok) {
        /* Reply-slot lookup: the cap arrives as CPtr T090_SLOT, no handle. */
        if (it_lookup_name_slot("t90.svc", T090_SLOT, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.recv_slot != T090_SLOT) ok = 0;
        if (ok) {
            struct iris_msg p;
            iris_msg_zero(&p);
            p.label = 0x90;
            if (iris_msg_nb_send((long)T090_SLOT, &p) !=
                (long)IRIS_ERR_WOULD_BLOCK) ok = 0;   /* invocable by CPtr */
        }
    }

    /* Occupied reply-slot → EP_CALL fails fast, before any send. */
    if (ok && it_lookup_name_slot("t90.svc", T090_SLOT, &msg) !=
        (long)IRIS_ERR_ALREADY_EXISTS) ok = 0;
    /* The endpoint is untouched by the failed declaration: the same lookup
     * into a DIFFERENT, empty slot still delivers.  (It used to prove this
     * with a slotless lookup, which now delivers no cap at all — the property
     * being checked is that nothing was consumed, so it needs a destination.) */
    if (ok) {
        it_slot_delete((uint32_t)IT_LOOKUP_TMP);
        if (it_lookup_name_slot("t90.svc", (uint32_t)IT_LOOKUP_TMP, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.recv_slot != (uint32_t)IT_LOOKUP_TMP) ok = 0;
        it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    }

    /* NOT_FOUND with a declared slot: no cap, slot stays empty. */
    if (ok) {
        if (it_lookup_name_slot("t90.nope", T090_SLOT_B, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR ||
            msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) ok = 0;
        if (ok && it_invoke0((long)T090_SLOT_B, INV_CAP_IDENTIFY) >= 0) ok = 0;
    }

    if (id >= 0) (void)it_unregister_id((uint32_t)id);
    it_close(&ep);
    if (ok) it_pass("T090"); else it_fail("T090", "recv-slot lookup");
}

void test_t091(void) {
    static const char expect[] = "Hello from IrisOS VFS!\n";
    const uint32_t expect_len = (uint32_t)(sizeof(expect) - 1u);
    int ok = 1;

    struct iris_msg msg;
    if (it_lookup_name_slot(VFS_EP_SVC_NAME, T091_SLOT, &msg) != 0 ||
        msg.label != IRIS_EP_REPLY_OK ||
        msg.recv_slot != T091_SLOT) ok = 0;

    if (ok) {
        uint32_t len = it_stage_path("iris.txt");
        iris_msg_zero(&msg);
        msg.label      = VFS_EP_OP_READ_AT;
        msg.words[0]   = 0;
        msg.words[1]   = VFS_EP_DATA_MAX;
        msg.word_count = 2;
        msg.buf_len    = len;
        if (iris_msg_call((long)T091_SLOT, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.words[1] != (uint64_t)expect_len ||
            msg.buf_len != expect_len) ok = 0;
        if (ok) {
            for (uint32_t i = 0; i < expect_len; i++) {
                if (g_ep_io_buf[i] != (uint8_t)expect[i]) { ok = 0; break; }
            }
        }
    }
    if (ok) it_pass("T091"); else it_fail("T091", "vfs.ep by cptr slot");
}

/* ── T092: a client that declares no slot gets no capability ────────────────
 * This used to assert the opposite — that a slotless client still received the
 * vfs.ep cap, materialised as a handle.  Stage 4 retired that materialisation:
 * it was the last place a capability entered a process through the handle
 * namespace, and the receiver never asked for it there.
 *
 * The guarantee that replaces it is the one worth having, so it is asserted
 * here rather than deleted with the mechanism: the REQUEST still succeeds, the
 * reply still arrives, and it simply carries no capability.  Nothing is
 * half-delivered, the server is not left holding a staged cap, and a
 * subsequent lookup WITH a declared slot works — the failure is closed, not
 * sticky. */
void test_t092(void) {
    int ok = 1;
    struct iris_msg msg;

    /* Slotless: reply OK, no cap. */
    if (it_lookup_name_slot(VFS_EP_SVC_NAME, 0u, &msg) != 0 ||
        msg.label != IRIS_EP_REPLY_OK ||
        msg.got_cap != (uint32_t)IRIS_MSG_NO_CAP) ok = 0;

    /* The same lookup WITH a slot still delivers a working cap: the dropped
     * delivery left nothing wedged on either side. */
    if (ok) {
        it_slot_delete((uint32_t)IT_LOOKUP_TMP);
        if (it_lookup_name_slot(VFS_EP_SVC_NAME, (uint32_t)IT_LOOKUP_TMP, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.got_cap != (uint32_t)IT_LOOKUP_TMP) ok = 0;
    }
    if (ok) {
        iris_msg_zero(&msg);
        msg.label = VFS_EP_OP_STATUS;
        if (iris_msg_call((long)IT_LOOKUP_TMP, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK) ok = 0;
    }
    it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    if (ok) it_pass("T092"); else it_fail("T092", "slotless lookup not fail-closed");
}

/* ── A1.7: handle-table shrink/freeze evidence (T093–T096) ──────────────────
 * Stress the receive-slot pool, force the documented TOCTOU fallback, and
 * read the sys_sched_info extended diagnostics to prove the handle table is
 * a small, bounded working set.  Test slot: 51 (T094). */

/* Read the A1.7 extended sched_info words (offsets 40..87 as 12 uint32).
 * The KDEBUG authority comes from the spawn bootcap (slot 6), resolved for
 * the duration of the call — identical churn on every invocation, so
 * before/after comparisons of self_live are exact. */
int it_sched_ext(uint32_t w[14]) {
    uint8_t buf[96];
    /* Phase 16: request 96 bytes so the two lifecycle words (offsets 84/88)
     * land too; a pre-Phase-16 kernel clamps to 88 and leaves w[11..13] zero —
     * the extra words are additive, never required by legacy asserts. */
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 96);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 14u; i++) {
        uint32_t o = 40u + 4u * i;
        w[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
               ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/* Read the base-frame live TASK count (offset 32) — the scheduler's
 * sched_live_count, distinct from the handle-table live at IT_SI_LIVE. */
int it_task_live(uint32_t *out) {
    uint8_t buf[96];
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 96);
    if (r != 0) return 0;
    *out = (uint32_t)buf[32] | ((uint32_t)buf[33] << 8) |
           ((uint32_t)buf[34] << 16) | ((uint32_t)buf[35] << 24);
    return 1;
}
/* Stage 7-mem: the GLOBAL live-VMO count (diag offset 132).  It replaces the
 * per-process `vmos_usage` the retired SYS_RESOURCE_INFO reported, and it is
 * the stronger of the two: a leak by ANY principal shows here, where the
 * per-process form only ever caught the caller's own. */

int it_sched_ext3(uint32_t w3[6]) {
    uint8_t buf[136];
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 136);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t o = 112u + 4u * i;
        w3[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

int it_sched_ext4(uint32_t w4[5]) {
    uint8_t buf[160];
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 160);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 5u; i++) {
        uint32_t o = 136u + 4u * i;
        w4[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/*
 * The processor tier (SMP roadmap §9.3 step 4).
 *
 *   [0] cpus_online       how many processors are running IRIS code
 *   [1] cpus_dispatching  how many have ever dispatched a thread
 *   [2] tick_ipis         ticks broadcast to the other processors
 *   [3] reschedule_ipis   reschedules broadcast likewise
 *   [4] deaths_pending    deaths that have not finished yet
 *
 * This is what lets a test written once say the right thing on a machine with
 * one processor and on a machine with four, instead of encoding a core count
 * it cannot know.
 */
/*
 * The DMA containment tier (Stage 10-dma §10.2 step 3).
 *
 *   [0] units       remapping units the DMAR named
 *   [1] usable      ...that are what the kernel needs
 *   [2] translating ...that are ENFORCING right now
 *   [3] fault_status unit 0's fault status register
 *
 * `translating == units` is the claim: a device no capability names reaches
 * nothing.  Anything less means some devices reach all of memory.
 */
int it_sched_ext7(uint32_t w7[4]) {
    uint8_t buf[IRIS_SCHED_INFO_MAX_BYTES];
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO,
                        (long)(uintptr_t)buf, (long)IRIS_SCHED_INFO_MAX_BYTES);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t o = 208u + 4u * i;
        w7[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

int it_sched_ext6(uint32_t w6[5]) {
    uint8_t buf[208];
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 208);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 5u; i++) {
        uint32_t o = 184u + 4u * i;
        w6[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

int it_sched_ext5(uint32_t w5[5]) {
    uint8_t buf[184];
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 184);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 5u; i++) {
        uint32_t o = 160u + 4u * i;
        w5[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/* Phase 19: lazily mint a cap to iris_test's OWN VSpace into IRIS_CPTR_TEST_VSPACE
 * (via SYS_VSPACE_SELF + a self-mint through the self-proc cap, slot 25) so the
 * VM tests can pass it as the VSpace CPtr to SYS_FRAME_MAP/UNMAP.  Returns 1 on
 * success. */
static int g_it_vspace_ready = 0;
int it_setup_self_vspace(void) {
    if (g_it_vspace_ready) return 1;
    /* Stage 4: SYS_VSPACE_SELF publishes into a destination slot, so the
     * suite's own address space is a capability from the moment it exists —
     * it used to arrive as a handle that had to be minted onward and closed. */
    /* Derived from the address space the spawner delegated, not fabricated by
     * SYS_VSPACE_SELF — so IT_VS is a child of the loader's slot and a revoke
     * there reaches it, which is the whole of D-6. */
    long r = it_invoke2((long)IRIS_CPTR_OWN_VSPACE, INV_CSPACE_MINT, IT_MINT_SELF(IRIS_CPTR_TEST_VSPACE), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));
    if (r != 0) return 0;
    g_it_vspace_ready = 1;
    return 1;
}

/* A reserved self-map VA window (page-aligned, inside the user private window,
 * clear of the memory test VAs at 0x8050/0x8060/0x8061).  IT_VS itself is
 * defined at the top of the file: every map names its address space now. */

long it_map_fixup_inv(unsigned long label, long c, long a1, long a2, long a3) {
    if (!it_setup_self_vspace()) return (long)IRIS_ERR_MISSING_TABLE;
    return iris_vspace_fixup(label, c, a1, a2, a3,
                             IT_VS, (long)IRIS_CPTR_TEST_UNTYPED,
                             (long)(((uint64_t)252 << 32) | IT_OBJ_CNODE_SLOT),
                             (long)IT_PT_SCRATCH,
                             (long)(((uint64_t)253 << 32) | IT_OBJ_CNODE_SLOT),
                             (long)IT_PT_VS_SCRATCH);
}

/* The numbered wrappers still reach the fixup while the suite migrates
 * (ledger A-32).  One translation, and it goes with the last `it_sysN`. */
long it_map_fixup(long nr, long a0, long a1, long a2, long a3) {
    return it_map_fixup_inv((nr == SYS_FRAME_MAP) ? (unsigned long)INV_FRAME_MAP
                                                  : 0ul,
                            a0, a1, a2, a3);
}

long it_cspace_self(void) {
    /* Delegated, not fabricated (ledger D-6/A5).  `SYS_CSPACE_SELF` handed a
     * thread its own root CNode on request, asking for no capability at all —
     * ambient authority, and an MDB root nothing could revoke.  The loader
     * holds this CSpace: it made it, and it mints it at IRIS_CPTR_OWN_CSPACE
     * before the child's first instruction. */
    return (long)IRIS_CPTR_OWN_CSPACE;
}

/*
 * Threads live in their OWN CNode, not in the rotating object pool.
 *
 * `it_thread_create` retyped its TCB into a rotating leaf and returned the
 * CPtr — and 47 call sites later, most of them discard it (`if
 * (it_thread_create(...) < 0)`).  A slot nobody kept is a slot nobody can
 * release, so every thread the suite ever made left its capability in a pool
 * that recycles: T324 counted 22 of them, and the allocator wraps many times
 * per run, so each one was waiting to be deleted out from under a live thread
 * in somebody else's test.  That is the shape of four separate slot collisions
 * this convergence has already paid for.
 *
 * The contract could not be fixed at the call sites, so it moved into the
 * helper.  A leaf here is reused only when the thread it names is PROVABLY
 * gone — asked, not assumed, because "the test that made it has finished" is
 * not the same statement as "the thread has exited", and the rotating pool's
 * whole defect was treating them as one.
 */

static uint32_t g_it_tcb_next  = 0;
static int      g_it_tcb_ready = 0;

/* A leaf of the thread CNode that is free, or that holds a dead thread.
 * Returns the leaf index, or a negative error when every leaf is live. */
static long it_tcb_leaf_alloc(void) {
    if (!g_it_tcb_ready) {
        if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_CNODE | (1ULL << 32)), (long)((uint64_t)IT_TCB_CNODE_SLOT << 32), (long)IT_TCB_SLOTS) != 0)
            return (long)IRIS_ERR_NO_MEMORY;
        g_it_tcb_ready = 1;
    }
    /* Leaf 0 is IRIS_CPTR_NULL and refuses publication — start at 1. */
    uint32_t span  = IT_TCB_SLOTS - 1u;
    uint32_t start = __atomic_fetch_add(&g_it_tcb_next, 1u, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < span; i++) {
        uint32_t leaf = 1u + ((start + i) % span);
        long t = it_invoke0((long)IT_TCB_CPTR(leaf), INV_CAP_IDENTIFY);
        if (t < 0) return (long)leaf;                       /* empty */
        if (t == (long)IRIS_HANDLE_TYPE_TCB &&
            it_tcb_alive((long)IT_TCB_CPTR(leaf)) == 0) {   /* dead: reclaim */
            (void)it_invoke1((long)IT_TCB_CNODE_SLOT, INV_CNODE_DELETE, (long)leaf);
            return (long)leaf;
        }
    }
    return (long)IRIS_ERR_NO_MEMORY;   /* every leaf holds a LIVE thread */
}

long it_thread_create(uint64_t entry, uint64_t rsp, uint64_t arg) {
    if (!it_setup_self_vspace()) return (long)IRIS_ERR_NOT_FOUND;
    long cs = it_cspace_self();
    if (cs < 0) return cs;

    long leaf = it_tcb_leaf_alloc();
    if (leaf < 0) return leaf;
    if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_TCB | (1ULL << 32)), (long)(((uint64_t)leaf << 32) | (uint64_t)IT_TCB_CNODE_SLOT), 0) != 0)
        return (long)IRIS_ERR_NO_MEMORY;
    long tcb = (long)IT_TCB_CPTR((uint32_t)leaf);

    long r = it_invoke2(tcb, INV_TCB_CONFIGURE, cs, IT_VS);
    if (r != 0) return r;
    r = it_invoke(tcb, INV_TCB_WRITE_REGS, (long)entry, (long)rsp, (long)arg);
    if (r != 0) return r;
    /*
     * A-33: each thread marshals in the page IT registered, and it knows which
     * page because it was told.  There is no `buf_uptr` for the kernel to
     * report one in — that field was the message ABI's way of telling a
     * receiver where its own buffer was, which is a thing a thread that
     * registered one already knows.  The creator publishes it here, and the
     * thread it just made is the one that reads it.
     */
    g_it_thread_buf = (uint64_t)it_thread_ipc_buffer(tcb);
    if (arg == IT_THREAD_ARG_SELF_TCB) {
        /* The thread is handed its OWN TCB capability as its entry argument —
         * the only per-thread channel a freshly started thread has, and the
         * reason SYS_TCB_SELF can go (ledger A-18).  Whoever creates a thread
         * holds its TCB; telling it which one it is costs a register. */
        r = it_invoke(tcb, INV_TCB_WRITE_REGS, (long)entry, (long)rsp, tcb);
        if (r != 0) return r;
    }
    r = it_invoke0(tcb, INV_TCB_RESUME);
    if (r != 0) return r;
    return tcb;
}

/*
 * Every thread the suite makes gets an IPC BUFFER of its own (ledger D-4).
 *
 * Registration takes a TCB capability, so the CREATOR can do it — which is
 * what makes this one place instead of forty.  It happens between WRITE_REGS
 * and RESUME, while the thread is configured and not yet running: a thread
 * that started first would have a window in which it could send a bulk payload
 * with no buffer, and that window is exactly what the kernel is about to stop
 * having an answer for.
 *
 * One page per thread, at IRIS_IPC_BUFFER_VA + n*4096, out of the suite's own
 * Untyped.  The address space is shared with every other thread here, so the
 * windows have to be distinct; the counter never rewinds, which is what keeps
 * a reused TCB from inheriting a page a dead thread's buffer still names.
 */
static uint32_t g_it_ipcbuf_next;
static long     g_it_ipcbuf_ut = -1;   /* a pool of its own; see below */
_Static_assert(IT_IPCBUF_CNODE_SLOT == 66u,
               "the slot guard names 66 by value; keep the two together");

volatile uint64_t g_it_thread_buf;   /* A-33: see it_thread_create */

long it_thread_ipc_buffer(long tcb) {
    if (g_it_ipcbuf_ut < 0) {
        /* Carved into a rotating leaf only long enough to build the CNode it
         * pays for, then MOVED to a leaf of that CNode — because it is held
         * for the whole run, and the rotating pool recycles. */
        long ut = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                       IRIS_KOBJ_UNTYPED, 128u * 1024u);
        if (ut < 0) return ut;
        if (it_invoke(ut, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_CNODE | (1ULL << 32)), (long)((uint64_t)IT_IPCBUF_CNODE_SLOT << 32), 128) != 0)
            return (long)IRIS_ERR_TABLE_FULL;
        uint32_t home = IT_IPCBUF_MAX + 1u;      /* above every buffer leaf */
        if (it_invoke2(ut, INV_CSPACE_MINT, (long)(((uint64_t)home << 32) | (uint64_t)IT_IPCBUF_CNODE_SLOT), (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0)
            return (long)IRIS_ERR_TABLE_FULL;
        it_slot_delete((uint32_t)ut);
        g_it_ipcbuf_ut = (long)IT_IPCBUF_CPTR(home);
    }
    uint32_t n = g_it_ipcbuf_next++;
    if (n >= IT_IPCBUF_MAX) return (long)IRIS_ERR_NO_MEMORY;

    /* Leaf n+1: slot 0 of a CNode is the null slot and refuses publication,
     * which is a thing to remember rather than rediscover. */
    uint32_t leaf = n + 1u;
    uint64_t va   = IRIS_IPC_BUFFER_VA + (uint64_t)leaf * 4096u;
    if (it_invoke(g_it_ipcbuf_ut, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)), (long)(((uint64_t)leaf << 32) | (uint64_t)IT_IPCBUF_CNODE_SLOT), 4096) != 0)
        return (long)IRIS_ERR_NO_MEMORY;

    long fr = (long)IT_IPCBUF_CPTR(leaf);
    long r  = it_invoke(fr, INV_FRAME_MAP, IT_VS, (long)va, (long)IT_MAP_W);
    if (r != 0) return r;
    r = it_invoke2(tcb, INV_TCB_SET_IPC_BUFFER, fr, (long)va);
    if (r != 0) return r;
    return (long)va;          /* where the thread will find it */
}


int it_sched_ext2(uint32_t w2[4]) {
    uint8_t buf[112];
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 112);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t o = 96u + 4u * i;
        w2[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/* ── T093: svcmgr receive-slot pool stress (A1.7) ───────────────────────────
 * Three full cycles of 8 concurrent registrations: every REGISTER lands in
 * svcmgr's CSpace pool, lookups serve from it, UNREGISTER releases the pool
 * slot, and the next cycle re-registers the same names (clean reuse).  After
 * the last cycle nothing resolves (no ghost caps, no leaked slots). */
void test_t093(void) {
    long e = it_ep_create();
    if (e < 0) { it_fail("T093", "ep create"); return; }
    iris_cptr_t ep = (iris_cptr_t)e;
    int ok = 1;
    long ids[8];
    char name[6] = { 't', '9', '3', '.', 'a', '\0' };

    for (int cycle = 0; ok && cycle < 3; cycle++) {
        for (int i = 0; ok && i < 8; i++) {
            name[4] = (char)('a' + i);
            ids[i] = it_register_ep(name, ep);
            if (ids[i] < 0) ok = 0;
        }
        /* spot-check two: served from CSpace storage, invocable */
        for (int i = 0; ok && i < 8; i += 7) {
            struct iris_msg msg;
            name[4] = (char)('a' + i);
            it_slot_delete((uint32_t)IT_LOOKUP_TMP);
            if (it_lookup_name_slot(name, (uint32_t)IT_LOOKUP_TMP, &msg) != 0 ||
                msg.label != IRIS_EP_REPLY_OK ||
                msg.got_cap != (uint32_t)IT_LOOKUP_TMP) { ok = 0; break; }
            struct iris_msg p;
            iris_msg_zero(&p);
            p.label = 0x93;
            if (iris_msg_nb_send((long)IT_LOOKUP_TMP, &p) !=
                (long)IRIS_ERR_WOULD_BLOCK) ok = 0;
            it_slot_delete((uint32_t)IT_LOOKUP_TMP);
        }
        for (int i = 0; ok && i < 8; i++) {
            if (it_unregister_id((uint32_t)ids[i]) != 0) ok = 0;
        }
    }

    /* after the last unregister wave nothing resolves */
    if (ok) {
        struct iris_msg msg;
        name[4] = 'a';
        if (it_lookup_name_slot(name, 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR ||
            msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) ok = 0;
    }

    it_close(&ep);
    if (ok) it_pass("T093"); else it_fail("T093", "recv-slot pool stress");
}

static iris_cptr_t       g_t094_ep = IRIS_CPTR_NULL;
static volatile uint32_t g_t094_got = 0;
static volatile int      g_t094_ready = 0, g_t094_done = 0;
static uint8_t           g_t094_stack[8192];

static void t094_recv(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.recv_slot = (uint32_t)T094_SLOT;      /* receive-slot declaration */
    g_t094_ready = 1;
    if (iris_msg_recv((long)g_t094_ep, &m) == 0)
        g_t094_got = m.got_cap;        /* EP_SEND caps land here */
    g_t094_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t094(void) {
    g_t094_got = 0; g_t094_ready = 0; g_t094_done = 0;

    long nA = it_notify_create_slot();      /* the cap to transfer */
    long nB = it_notify_create_slot();      /* the slot-race winner */
    long ep = it_ep_create_slot();
    /* Stage 4: invoked as a CPtr; never materialised into a handle. */
    const long selfp = (it_invoke0((long)IRIS_CPTR_TEST_PROC, INV_CAP_IDENTIFY) >= 0)
                       ? (long)IRIS_CPTR_TEST_PROC : -1;
    if (nA < 0 || nB < 0 || ep < 0 || selfp < 0) { it_fail("T094", "create"); return; }
    iris_cptr_t nA_h = (iris_cptr_t)nA, nB_h = (iris_cptr_t)nB;
    g_t094_ep = (iris_cptr_t)ep;
    int ok = 1;
    const char *why = "toctou fallback";

    uint64_t entry = (uint64_t)(uintptr_t)t094_recv;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t094_stack + sizeof(g_t094_stack))) & ~0xFULL;
    if (it_thread_create(entry, rsp, 0) < 0) {
        ok = 0; why = "thread create";
    }
    IT_AWAIT(g_t094_ready, 200);
    it_settle(2);                     /* blocked with slot 51 declared */

    /* Fill the declared slot BEFORE delivery (the TOCTOU race). */
    if (ok && it_invoke2(nB, INV_CSPACE_MINT, IT_MINT_SELF(T094_SLOT), (long)RIGHT_WRITE) != 0) {
        ok = 0; why = "self mint";
    }

    long xsrc = -1;
    if (ok) {
        xsrc = it_xfer_dup( nA, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (xsrc < 0) { ok = 0; why = "xfer slot"; }
        else {
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label           = 0x94;
            m.cap = (uint32_t)xsrc;
            m.cap_rights = RIGHT_WRITE;
            if (iris_msg_send((long)g_t094_ep, &m) != 0) {
                ok = 0; why = "send";
            }
        }
    }
    IT_AWAIT(g_t094_done, 200);
    if (ok && !g_t094_done) { ok = 0; why = "recv incomplete"; }

    /* Step 2: FAIL CLOSED — no cap delivered, and above all NO handle. */
    if (ok && g_t094_got != (uint32_t)IRIS_MSG_NO_CAP) {
        ok = 0; why = "toctou degradation still alive";
    }
    /* Nothing delivered ⇒ the source slot was never consumed. */
    if (ok && it_invoke0(xsrc, INV_CAP_IDENTIFY) < 0) {
        ok = 0; why = "source consumed on failed delivery";
    }
    if (ok) it_slot_delete((uint32_t)xsrc);
    /* The slot keeps exactly the race winner: nB, a notification.  Stage 4:
     * both sides are slots, so identity is compared where it lives. */
    if (ok) {
        long rh = it_invoke0(T094_SLOT, INV_CAP_IDENTIFY);
        if (rh < 0) { ok = 0; why = "slot lost"; }
        else {
            if (it_invoke1(T094_SLOT, INV_CAP_SAME_OBJECT, nB) != 1) {
                ok = 0; why = "slot object changed";
            }
            iris_cptr_t r = (iris_cptr_t)rh;
            it_close(&r);
        }
    }

    it_close(&nA_h);
    it_close(&nB_h);
    it_close(&g_t094_ep);
    if (ok) it_pass("T094"); else it_fail("T094", why);
}

/* ── T095: handle high-water smoke (A1.7) ───────────────────────────────────
 * By this point the suite has exercised every creator, every transfer, every
 * spawn and every death.  Read the extended diagnostics, log the real numbers,
 * and assert what Stage 4 set out to make true: the handle namespace is empty.
 *
 * Three retirement witnesses live here.  Handle-live must be ZERO — no
 * capability the suite holds is addressed by a handle.  Handle-delivery must
 * be zero — a transferred capability has exactly one destination, the
 * receiver's declared slot.  TOCTOU must be zero — there is no degradation
 * path left.  Any of the three moving means the namespace came back. */
void test_t095(void) {
    uint32_t w[14];
    if (!it_sched_ext(w)) { it_fail("T095", "sched_info ext"); return; }

    it_serial_write("[IRIS][TEST] T095 hwm self=");
    it_log_num(w[IT_SI_HWM]);
    it_serial_write(" live=");
    it_log_num(w[IT_SI_LIVE]);
    it_serial_write(" global=");
    it_log_num(w[IT_SI_GHWM]);
    it_serial_write(" max=");
    it_log_num(w[IT_SI_MAX]);
    it_serial_write(" slot=");
    it_log_num(w[IT_SI_SLOTDEL]);
    it_serial_write(" hand=");
    it_log_num(w[IT_SI_HANDDEL]);
    it_serial_write(" toctou=");
    it_log_num(w[IT_SI_TOCTOU]);
    it_serial_write(" reply=");
    it_log_num(w[IT_SI_REPLY]);
    it_serial_write(" resolve=");
    it_log_num(w[IT_SI_RESOLVE]);
    it_serial_write("\n");

    int ok = 1;
    /* Stage 4: the handle namespace is DRAINED.  This test used to assert
     * `live > 0` — "we hold handles" — which was the honest reading while the
     * suite fabricated them.  It is now the opposite assertion, and it is the
     * one that matters: by this point the suite has exercised every creator,
     * every transfer and every spawn, and it holds NO handles at all. */
    if (w[IT_SI_LIVE] != 0u) ok = 0;
    if (w[IT_SI_HWM] < w[IT_SI_LIVE]) ok = 0;                /* hwm ≥ live */
    if (w[IT_SI_GHWM] < w[IT_SI_HWM]) ok = 0;                /* global ≥ self */
    if (w[IT_SI_INSERTS] < w[IT_SI_REMOVES]) ok = 0;         /* books balance */
    if (w[IT_SI_INSERTS] - w[IT_SI_REMOVES] != w[IT_SI_LIVE]) ok = 0;
    if (w[IT_SI_SLOTDEL] < 8u) ok = 0;      /* T084+ / svcmgr registrations */
    /* Stage 4: handle materialisation on delivery is RETIRED.  Every
     * transferred capability now lands in a declared receive slot, so this
     * counter is a STRUCTURAL zero and is the retirement witness — the
     * partition slot/handle/toctou has exactly one live member.  A non-zero
     * value means a capability entered a process through the handle namespace
     * again (charter I1). */
    if (w[IT_SI_HANDDEL] != 0u) ok = 0;
    /* Phase S4 (Step 2): the CPtr→handle TOCTOU degradation is RETIRED.  T094
     * still forces the race; the counter must now stay at a STRUCTURAL zero.
     * This is the roadmap's retirement criterion for the last permitted
     * degradation (charter §3.7) — if it ever moves, the fallback is back. */
    if (w[IT_SI_TOCTOU] != 0u) ok = 0;
    if (w[IT_SI_REPLY] < 50u) ok = 0;       /* hundreds of EP_CALLs by now */
    /* The bound: busiest table ever ≤ MAX/4 — real margin, not aesthetics. */
    if (w[IT_SI_GHWM] * 4u > w[IT_SI_MAX]) ok = 0;

    if (ok) it_pass("T095"); else it_fail("T095", "handle high-water");
}

/* ── T096: repeated lookup+release does not leak (A1.7) ─────────────────────
 * 32 lookup+release cycles through the SAME reply slot: every one delivers a
 * real capability into the slot and every one releases it.  self_live must
 * return exactly to its starting value.
 *
 * It used to run slotless, which made it a test of the handle-materialising
 * delivery — retired in Stage 4.  What it actually measures is that the
 * delivery path has no per-cycle residue, and reusing one slot 32 times is a
 * sharper version of that: a leaked reference would leave the slot occupied
 * and the second lookup would fail ALREADY_EXISTS. */
void test_t096(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T096", "sched_info ext"); return; }

    int ok = 1;
    it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    for (int i = 0; ok && i < 32; i++) {
        struct iris_msg msg;
        if (it_lookup_name_slot(VFS_EP_SVC_NAME, (uint32_t)IT_LOOKUP_TMP, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.got_cap != (uint32_t)IT_LOOKUP_TMP) { ok = 0; break; }
        if (i == 31) {          /* the last cap still actually works */
            struct iris_msg p;
            iris_msg_zero(&p);
            p.label = IRIS_EP_OP_PING;
            if (iris_msg_call((long)IT_LOOKUP_TMP, &p) != 0 ||
                p.label != IRIS_EP_REPLY_OK) ok = 0;
        }
        it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    }

    if (ok && !it_sched_ext(after)) ok = 0;
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) ok = 0;   /* zero leak */
    /* 32 deliveries, all of them into the slot — and not one into a handle. */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + 32u) ok = 0;
    if (ok && after[IT_SI_HANDDEL] != before[IT_SI_HANDDEL]) ok = 0;

    if (ok) it_pass("T096"); else it_fail("T096", "lookup+release residue");
}

/* ── T097: PROC_CSPACE_MINT replaces the legacy handle insert path ──────────
 * The canonical placement covers what SYS_HANDLE_TRANSFER used to do, with
 * CSpace-canonical delivery: mint lands in the child's root CNode (no handle
 * produced in the destination table), an occupied slot fails fast, authority
 * cannot escalate (empty effective rights → INVALID_ARG), a wrong-type
 * destination fails, a dead destination fails — and the retired
 * SYS_HANDLE_TRANSFER itself now returns NOT_SUPPORTED. */
void test_t097(void) {
    long ep = it_ep_create();
    if (ep < 0) { it_fail("T097", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;
    iris_cptr_t proc_h = IRIS_CPTR_NULL;
    if (lp_spawn_child_cn(1u, cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h);
        it_fail("T097", "spawn"); return;
    }
    long vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);
    if (vmo < 0) {
        (void)it_kill((long)proc_h);
        it_close(&proc_h); it_close(&cmd_ep_h);
        it_fail("T097", "vmo create"); return;
    }
    iris_cptr_t vmo_h = (iris_cptr_t)vmo;
    int ok = 1;
    const char *why = "canonical placement";

    /* Canonical placement: the cap lands in the child's CSpace — no handle
     * is created in the destination table. */
    if (it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T097_DST_SLOT), (long)(RIGHT_READ | RIGHT_WRITE)) != 0) {
        ok = 0; why = "mint";
    }
    /* Occupied destination slot → fail-fast, no overwrite. */
    if (ok && it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T097_DST_SLOT), (long)RIGHT_READ) !=
        (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "occupied"; }
    /* Authority cannot escalate: READ-only source + WRITE request →
     * empty effective rights → INVALID_ARG (never a widened grant). */
    if (ok) {
        long ro = it_cs_reduce(vmo, RIGHT_READ | RIGHT_DUPLICATE);
        if (ro < 0) { ok = 0; why = "dup"; }
        else {
            if (it_invoke2(ro, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T097_DST_SLOT2), (long)RIGHT_WRITE) !=
                (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "escalation"; }
            iris_cptr_t roh = (iris_cptr_t)ro;
            it_close(&roh);
        }
    }
    /* Wrong-type destination (the slot-30 KNotification fixture).  The
     * destination is a CNODE, and A-30 stopped the flattening: a non-CNode
     * named there is WRONG_TYPE, because the resolver identified it exactly
     * and "something about your argument is wrong" was a weaker answer than
     * the kernel already had. */
    if (ok && it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_INTO((long)IRIS_CPTR_TEST_FIX_A, T097_DST_SLOT2), (long)RIGHT_READ) !=
        (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "wrong type"; }
    /* The retired legacy producer is gone: NOT_SUPPORTED, nothing placed. */
    if (ok && it_sys3(SYS_HANDLE_TRANSFER, vmo, (long)proc_h,
                      (long)RIGHT_READ) !=
        (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "transfer not retired"; }
    /*
     * A dead destination, re-derived for Stage 7 Step 9.
     *
     * "Minting into a dead process fails" was a property of naming the
     * PROCESS: the kernel looked it up, found it torn down, and refused.  The
     * destination is a CNODE now, and a CNode outlives the process whose root
     * it was for exactly as long as somebody holds it — so the mint SUCCEEDS,
     * into a CSpace no thread resolves in.  That is not a leak of authority:
     * it is a capability to an object, doing what a capability to that object
     * does.
     *
     * What actually matters is asserted instead, and it is the stronger
     * claim: teardown EMPTIED the child's CSpace.  The supervisor holding the
     * root can see that the slots the child was spawned with are gone, which
     * is the guarantee "the process is dead" was standing in for.
     */
    if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok) { for (int w = 0; w < 200 &&
                   it_invoke0(it_child_tcb((long)proc_h), INV_TCB_EXIT_CODE) ==
                   (long)IRIS_ERR_WOULD_BLOCK; w++) it_settle(1); }
    if (ok) {
        /* The child's command-endpoint slot, addressed through the root the
         * suite kept: empty once teardown has run. */
        long child_ep = (long)((uint64_t)LP_CPTR_CMD_EP << 8) | IT_CHILD_CN_CPTR(0);
        if (it_invoke0(child_ep, INV_CAP_IDENTIFY) >= 0) { ok = 0; why = "cspace not emptied"; }
    }
    if (ok && it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T097_DST_SLOT3), (long)RIGHT_READ) != 0) {
        ok = 0; why = "dead dest";
    }

    if (!ok && proc_h != IRIS_CPTR_NULL)
        (void)it_kill((long)proc_h);
    it_close(&vmo_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);
    if (ok) it_pass("T097"); else it_fail("T097", why);
}

/* ── T098: sharing a VMO with another process is a CSpace mint ──────────────
 * The only way to give a VMO to another process is a mint into a destination
 * slot of its CSpace.  SYS_VMO_SHARE, which wrote the destination's HANDLE
 * TABLE, is retired (Stage 4): the receiver could not name what it was given
 * and the grantor could not revoke it.
 *
 * The three properties that test asserted are real and are re-asserted here
 * against the mint, which is where they now live: a source missing
 * RIGHT_DUPLICATE cannot delegate (ACCESS_DENIED), a request disjoint from
 * the source's rights is refused rather than widened, and a dead destination
 * fails clean. */
void test_t098(void) {
    long ep = it_ep_create();
    if (ep < 0) { it_fail("T098", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;
    iris_cptr_t proc_h = IRIS_CPTR_NULL;
    if (lp_spawn_child_cn(1u, cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h);
        it_fail("T098", "spawn"); return;
    }
    long vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);
    if (vmo < 0) {
        (void)it_kill((long)proc_h);
        it_close(&proc_h); it_close(&cmd_ep_h);
        it_fail("T098", "vmo create"); return;
    }
    iris_cptr_t vmo_h = (iris_cptr_t)vmo;
    int ok = 1;
    const char *why = "vmo share cspace";

    /* Canonical: the shared VMO lands in the destination CSpace. */
    if (it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T097_DST_SLOT), (long)RIGHT_READ) != 0) { ok = 0; why = "mint"; }

    /* A source without RIGHT_DUPLICATE cannot delegate. */
    if (ok) {
        long ro = it_cs_reduce(vmo, RIGHT_READ);
        if (ro < 0) { ok = 0; why = "dup ro"; }
        else {
            if (it_invoke2(ro, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T097_DST_SLOT2), (long)RIGHT_READ) != (long)IRIS_ERR_ACCESS_DENIED) {
                ok = 0; why = "mint no-dup";
            }
            iris_cptr_t roh = (iris_cptr_t)ro;
            it_close(&roh);
        }
    }
    /* A request disjoint from the source's rights is refused, never widened. */
    if (ok) {
        long rd = it_cs_reduce(vmo, RIGHT_READ | RIGHT_DUPLICATE);
        if (rd < 0) { ok = 0; why = "dup rd"; }
        else {
            if (it_invoke2(rd, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), T097_DST_SLOT2), (long)RIGHT_MANAGE) >= 0) {
                ok = 0; why = "mint disjoint";
            }
            iris_cptr_t rdh = (iris_cptr_t)rd;
            it_close(&rdh);
        }
    }
    /* A dead destination, Stage 7 Step 9: the CNode outlives the process whose
     * root it was, so the mint lands — in a CSpace no thread resolves in.
     * What teardown guarantees, and what the old "the process is dead" refusal
     * was standing in for, is that the child's own slots were EMPTIED. */
    if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok) { for (int w = 0; w < 200 &&
                   it_invoke0(it_child_tcb((long)proc_h), INV_TCB_EXIT_CODE) ==
                   (long)IRIS_ERR_WOULD_BLOCK; w++) it_settle(1); }
    if (ok) {
        long child_ep = (long)((uint64_t)LP_CPTR_CMD_EP << 8) | IT_CHILD_CN_CPTR(0);
        if (it_invoke0(child_ep, INV_CAP_IDENTIFY) >= 0) { ok = 0; why = "dead dest"; }
    }

    if (!ok && proc_h != IRIS_CPTR_NULL)
        (void)it_kill((long)proc_h);
    it_close(&vmo_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);
    if (ok) it_pass("T098"); else it_fail("T098", why);
}

/* Command a child into receive-slot mode (second recv declares `slot`). */
long it_lp_cmd_rslot(iris_cptr_t cmd_ep_h, uint32_t slot) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = LP_CMD_RSLOT_RECV;
    m.words[0]   = slot;
    m.word_count = 1u;
    return iris_msg_send((long)cmd_ep_h, &m);
}

/* Transfer a WRITE|TRANSFER dup of `notif` to the child (blocks until the
 * child's declared recv rendezvouses — natural synchronization). */
long it_lp_send_cap(iris_cptr_t cmd_ep_h, long notif) {
    long d = it_xfer_dup( notif,
                     (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (d < 0) return d;
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label           = 0x99;
    m.cap = (uint32_t)d;
    m.cap_rights = RIGHT_WRITE;
    long r = iris_msg_send((long)cmd_ep_h, &m);
    it_xfer_release(d);
    return r;
}
long it_lp_cmd(iris_cptr_t cmd_ep_h, uint32_t label) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label = label;
    return iris_msg_send((long)cmd_ep_h, &m);
}

/* Wait (≤ 2s) for a child to exit; returns its exit code or -1. */
/* Stage 7 Step 10: wait on the THREAD the child was started with, which the
 * child table kept.  A child the suite did not record has no thread to watch
 * and says so rather than falling back to the process form. */
long it_lp_wait_exit(iris_cptr_t proc_h) {
    long tcb = it_child_tcb(proc_h);
    if (!tcb) return -1;
    long n = it_notify_create();
    if (n < 0) return -1;
    iris_cptr_t n_h = (iris_cptr_t)n;
    long ec = -1;
    if (it_invoke2(tcb, INV_TCB_WATCH, n, 1) == 0) {
        uint64_t bits = 0;
        if (it_wait_timeout( n, (long)(uintptr_t)&bits,
                    2000000000LL) == 0)
            ec = it_invoke0(tcb, INV_TCB_EXIT_CODE);
    }
    it_close(&n_h);
    return ec;
}

/* ── T099: multi-child receive-slot endpoint transfer ───────────────────────
 * Three children each receive a transferred notification cap INTO THEIR OWN
 * CSpace (exit code == the declared CPtr, never a handle) and invoke it by
 * CPtr across the process boundary (signal bits 1 observed by the parent).
 * Failure paths: a child whose declared slot the parent pre-filled fails
 * fast (ALREADY_EXISTS, endpoint left clean); an out-of-range declaration
 * fails INVALID_ARG.  Parent handle books balance exactly. */
void test_t099(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T099", "sched ext"); return; }
    int ok = 1;
    const char *why = "multi-child rslot";

    for (int i = 0; ok && i < 3; i++) {
        long ep = it_ep_create_slot();
        long n  = it_notify_create_slot();
        iris_cptr_t ep_h = (iris_cptr_t)ep, n_h = (iris_cptr_t)n;
        iris_cptr_t proc_h = IRIS_CPTR_NULL;
        if (ep < 0 || n < 0 ||
            lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; }
        if (ok && it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) {
            ok = 0; why = "cmd";
        }
        if (ok && it_lp_send_cap(ep_h, n) != 0) { ok = 0; why = "send cap"; }
        if (ok) {
            uint64_t bits = 0;
            if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
                bits != 1u) { ok = 0; why = "cptr signal"; }
        }
        if (ok && it_lp_wait_exit(proc_h) != (long)T099_CHILD_SLOT) {
            ok = 0; why = "landing";
        }
        it_close(&proc_h);
        it_close(&n_h);
        it_close(&ep_h);
    }

    /* Occupied child slot → the child's declared recv fails fast and the
     * endpoint keeps no dead waiter. */
    if (ok) {
        long ep = it_ep_create_slot();
        long n2 = it_notify_create_slot();
        iris_cptr_t ep_h = (iris_cptr_t)ep, n2_h = (iris_cptr_t)n2;
        iris_cptr_t proc_h = IRIS_CPTR_NULL;
        if (ep < 0 || n2 < 0 ||
            lp_spawn_child_cn(1u, ep_h, &proc_h) < 0) { ok = 0; why = "spawn occ"; }
        if (ok && it_invoke2(n2, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), (long)T099_CHILD_SLOT), (long)RIGHT_WRITE) != 0) { ok = 0; why = "prefill"; }
        if (ok && it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) {
            ok = 0; why = "cmd occ";
        }
        if (ok && it_lp_wait_exit(proc_h) !=
            (LP_EXIT_RECV_ERR_BASE | (long)-IRIS_ERR_ALREADY_EXISTS)) {
            ok = 0; why = "occupied";
        }
        if (ok) {
            struct iris_msg pr;
            iris_msg_zero(&pr);
            pr.label = 0x99;
            if (iris_msg_nb_send((long)ep_h, &pr) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "dead waiter"; }
        }
        it_close(&proc_h);
        it_close(&n2_h);
        it_close(&ep_h);
    }

    /* Out-of-range declaration (slot 300, T086 fixture value) → INVALID_ARG. */
    if (ok) {
        long ep = it_ep_create_slot();
        iris_cptr_t ep_h = (iris_cptr_t)ep;
        iris_cptr_t proc_h = IRIS_CPTR_NULL;
        if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
            ok = 0; why = "spawn inv";
        }
        if (ok && it_lp_cmd_rslot(ep_h, 300u) != 0) { ok = 0; why = "cmd inv"; }
        if (ok && it_lp_wait_exit(proc_h) !=
            (LP_EXIT_RECV_ERR_BASE | (long)-IRIS_ERR_INVALID_ARG)) {
            ok = 0; why = "invalid slot";
        }
        it_close(&proc_h);
        it_close(&ep_h);
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* Parent books balance (dups consumed by staging; everything closed)
     * and the three cross-process deliveries were receive-slot installs. */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + 3u) {
        ok = 0; why = "slot count";
    }
    if (ok) it_pass("T099"); else it_fail("T099", why);
}

/* ── T100: svcmgr lookup receive-slot stress ────────────────────────────────
 * Four concurrent registrations served into distinct client reply-slots;
 * unregister under pressure; a post-unregister lookup with a declared slot
 * fails WITHOUT installing anything (the same slot then serves the
 * re-registered service — proof it stayed genuinely empty); legacy lookup
 * confirms the final NOT_FOUND. */
void test_t100(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T100", "sched ext"); return; }
    long e = it_ep_create();
    if (e < 0) { it_fail("T100", "ep create"); return; }
    iris_cptr_t ep = (iris_cptr_t)e;
    int ok = 1;
    const char *why = "lookup rslot stress";
    long ids[4];
    char name[7] = { 't', '1', '0', '0', '.', 'a', '\0' };
    struct iris_msg msg;

    for (int i = 0; ok && i < 4; i++) {
        name[5] = (char)('a' + i);
        ids[i] = it_register_ep(name, ep);
        if (ids[i] < 0) { ok = 0; why = "register"; }
    }
    for (int i = 0; ok && i < 4; i++) {
        uint32_t slot = 44u + (uint32_t)i;
        name[5] = (char)('a' + i);
        if (it_lookup_name_slot(name, slot, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.got_cap != slot) { ok = 0; why = "slot lookup"; }
        if (ok) {
            struct iris_msg p;
            iris_msg_zero(&p);
            p.label = 0xA0;
            if (iris_msg_nb_send((long)slot, &p) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "cptr dead"; }
        }
    }
    for (int i = 0; ok && i < 4; i++) {
        if (it_unregister_id((uint32_t)ids[i]) != 0) { ok = 0; why = "unreg"; }
    }

    /* Post-unregister lookup with a declared slot: ERR and nothing lands. */
    if (ok) {
        name[5] = 'a';
        if (it_lookup_name_slot(name, 43u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR ||
            msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) {
            ok = 0; why = "post-unreg lookup";
        }
        if (ok && it_invoke0(43L, INV_CAP_IDENTIFY) >= 0) {
            ok = 0; why = "ghost cap";
        }
    }
    /* The untouched slot then serves the re-registered service. */
    if (ok) {
        long id2 = it_register_ep("t100.a", ep);
        if (id2 < 0) { ok = 0; why = "re-register"; }
        else {
            if (it_lookup_name_slot("t100.a", 43u, &msg) != 0 ||
                msg.label != IRIS_EP_REPLY_OK ||
                msg.got_cap != 43u) { ok = 0; why = "slot reuse"; }
            (void)it_unregister_id((uint32_t)id2);
        }
    }
    if (ok) {
        if (it_lookup_name_slot("t100.a", 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR) { ok = 0; why = "final legacy"; }
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + 5u) {
        ok = 0; why = "slot count";
    }
    it_close(&ep);
    if (ok) it_pass("T100"); else it_fail("T100", why);
}
