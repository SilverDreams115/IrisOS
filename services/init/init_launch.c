/*
 * init_launch.c — service launch for init (Phase 14 extraction).
 *
 * Everything here is MOVED VERBATIM from main.c (no functional change): the
 * fb / console / svcmgr / iris_test spawns — initrd loads via
 * svc_load_minted, the pre-start CSpace mint tables each child receives, and
 * the spawn error paths.  Boot order and every log string are unchanged;
 * main.c remains the orchestrator that calls these in sequence.
 */

#include "init.h"
#include <iris/endpoint_proto.h>
#include "../common/svc_loader.h"

static const char init_console_load_fail[] = "[INIT] console load FAILED\r\n";
static const char init_console_ioport_fail[] = "[INIT] console ioport FAILED\r\n";
static const char init_console_chan_fail[] = "[INIT] console ep FAILED\r\n";
static const char init_fb_load_fail[] = "[INIT] fb load FAILED\r\n";

/* ── fb spawn (Phase 30: ring-3 framebuffer painter) ────────────────────── */

/* Slot 42 held fb's FRAMEBUFFER-restricted clone between its construction and
 * the pre-start mint.  Stage 5 Step 2 deleted the construction: fb is minted
 * the framebuffer control capability from init's own slot. */

void init_spawn_fb(void) {
    handle_id_t fb_proc_h  = HANDLE_INVALID;
    handle_id_t fb_boot_h  = HANDLE_INVALID;
    long r;

    /* Stage 5 Step 2: fb receives the FRAMEBUFFER CONTROL capability — the
     * whole of what it is allowed to do — as a pre-start mint from init's own
     * slot, so the grant is an MDB child of init's and stays revocable.
     *
     * It used to be a SYS_BOOTCAP_RESTRICT of init's monolithic boot
     * capability into a scratch slot, minted on, then deleted: three steps
     * that existed only because the authority fb needed was one bit of an
     * object that also carried spawn and debug authority.  Delegating a
     * capability that means exactly one thing needs none of them. */
    {
        struct svc_mint fb_mints[1] = { 0 };
        fb_mints[0].slot     = IRIS_CPTR_FB_CONTROL;
        fb_mints[0].src_cptr = IRIS_CPTR_FB_CONTROL;
        fb_mints[0].rights   = RIGHT_READ;
        fb_mints[0].badge    = 0;
        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "fb", &fb_proc_h, &fb_boot_h, fb_mints, 1u,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20,
                               /* fb maps the framebuffer into a window nothing
                                * has touched, so it owes every level under it. */
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED);
    }
    if (r < 0)
        init_early_serial_write(init_fb_load_fail);

    init_close(&fb_proc_h);
    init_close(&fb_boot_h);
}

/* ── console spawn (Phase 30: ring-3 serial console service) ────────────── */

/* Phase 13 (Track I): console is endpoint-only and CPtr-provisioned — its
 * endpoint recv side (IRIS_CPTR_OWN_EP) and its 0x3F8 UART KIoPort
 * (IRIS_CPTR_IOPORT) are pre-start mints; no legacy console KChannel pair, no
 * bootstrap sends.  Returns 1 on success, 0 on failure. */
int init_spawn_console(void) {
    handle_id_t con_proc_h  = HANDLE_INVALID;
    handle_id_t con_boot_h  = HANDLE_INVALID;
#define INIT_CONSOLE_IOPORT_SLOT 41u
    uint32_t    ioport_c    = 0u;   /* Phase S4: CPtr slot, not a handle */
    long r;

    /* Console KEndpoint master (init owns it); recv side minted to the child.
     * Phase S1: retyped from init's untyped pool (SYS_ENDPOINT_CREATE retired). */
    r = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_CONSOLE_EP, 0);
    if (r < 0) {
        init_early_serial_write(init_console_chan_fail);
        goto fail;
    }
    g_init_console_ep_h = (handle_id_t)INIT_SLOT_CONSOLE_EP;

    /* KIoPort for the 8 UART registers at 0x3F8..0x3FF (IN poll LSR + OUT THR).
     * Phase S4: published into a CSpace slot as an MDB child of the authorising
     * slot, and forwarded to console by CSpace source — so the delegation is
     * revocable from init.  Slot 41 is free in init's root CNode.
     * Stage 5 Step 2: the authority is the ioport control capability. */
    if (init_sys4(SYS_CAP_CREATE_IOPORT, (long)IRIS_CPTR_IOPORT_CONTROL,
                  0x3F8, 8, (long)INIT_CONSOLE_IOPORT_SLOT) != 0) {
        init_early_serial_write(init_console_ioport_fail);
        goto fail;
    }
    ioport_c = INIT_CONSOLE_IOPORT_SLOT;

    /* Phase S1: console serves EP_CALLs, so it needs an explicit reply object.
     * Retype it from init's pool, mint it at IRIS_CPTR_OWN_REPLY, then DROP
     * init's handle — a retained reply cap would suppress the
     * close-wakes-caller path if console dies. */
    handle_id_t con_reply_h = HANDLE_INVALID;
    {
        long rr = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_REPLY,
                                   INIT_SLOT_CONSOLE_RPLY, 0);
        if (rr >= 0) con_reply_h = (handle_id_t)INIT_SLOT_CONSOLE_RPLY;
        else init_early_serial_write("[INIT] console reply retype FAILED\r\n");
    }

    {
        struct svc_mint con_mints[3] = { 0 };
        uint32_t n = 0;
        con_mints[n].slot   = IRIS_CPTR_OWN_EP;
        con_mints[n].src_cptr = g_init_console_ep_h;
        con_mints[n].rights = RIGHT_READ;
        con_mints[n].badge  = 0;   /* server-side cap: unbadged */
        n++;
        con_mints[n].slot   = IRIS_CPTR_IOPORT;
        con_mints[n].src_cptr = ioport_c;
        con_mints[n].rights = RIGHT_READ | RIGHT_WRITE;
        con_mints[n].badge  = 0;
        n++;
        if (con_reply_h != HANDLE_INVALID) {
            con_mints[n].slot   = IRIS_CPTR_OWN_REPLY;
            con_mints[n].src_cptr = con_reply_h;
            con_mints[n].rights = RIGHT_READ | RIGHT_WRITE;
            con_mints[n].badge  = 0;
            n++;
        }
        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "console", &con_proc_h, &con_boot_h,
                               con_mints, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20, /*own_budget_slot=*/0);
    }
    /* console's slot-13 mint is the only reply cap: drop ours. */
    (void)init_sys2(SYS_CNODE_DELETE, 0, (long)INIT_SLOT_CONSOLE_RPLY);
    if (r < 0) {
        init_early_serial_write(init_console_load_fail);
        goto fail;
    }

    /* console holds the slot-10 mint now; init keeps its own slot so it can
     * still revoke the delegation (Phase S4). */
    init_close(&con_proc_h);
    init_close(&con_boot_h);
    return 1;

fail:
    init_close(&con_proc_h);
    init_close(&con_boot_h);
    (void)ioport_c;
    return 0;
}

/* ── svcmgr spawn (Phase 29: ring-3 loader; Phase 30: also sends console) ── */

/* Phase 13 (Track I): init owns svcmgr's discovery endpoint ("svcmgr.ep").  It
 * creates the endpoint, mints the recv+mint side into svcmgr (IRIS_CPTR_OWN_EP)
 * and keeps the send side for its own EP_LOOKUP_NAME calls.  All of svcmgr's
 * bootstrap caps arrive as pre-start CSpace mints (no bootstrap KChannel):
 *   slot 3 (CONSOLE_EP)  — console.ep, WRITE|DUP|TRANSFER (re-mint to children);
 *   slot 5 (OWN_EP)      — svcmgr.ep recv side, READ|WRITE|DUP (recv + re-mint
 *                          IRIS_CPTR_SVCMGR_EP into catalog children);
 *   slot 6 (SPAWN_CAP)   — spawn/authority cap, READ|DUP|TRANSFER.
 * Returns the svcmgr.ep send side (init's discovery handle), or HANDLE_INVALID. */
handle_id_t init_spawn_svcmgr(void) {
    handle_id_t svcmgr_proc_h  = HANDLE_INVALID;
    handle_id_t svcmgr_chan_h  = HANDLE_INVALID;
    handle_id_t svcmgr_ep_h    = HANDLE_INVALID;
    long r;

    /* Phase S1: retyped from init's untyped pool (SYS_ENDPOINT_CREATE retired). */
    r = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_SVCMGR_EP, 0);
    if (r < 0) goto fail;
    svcmgr_ep_h = (handle_id_t)INIT_SLOT_SVCMGR_EP;

    /* Step 4: the SYS_HANDLE_DUP that used to sit here is gone.  It produced a
     * rights-reduced duplicate purely to have a HANDLE to pass as a mint
     * source — and the mint below already reduces to exactly the same rights,
     * so the duplicate never carried authority the mint did not compute
     * itself.  Minting straight from our spawn-cap SLOT drops a handle, drops
     * a syscall, and makes svcmgr's spawn cap an MDB child of that slot, so
     * the delegation is revocable by init instead of handed over outright. */

    /* Phase S1: carve svcmgr's untyped pool (a sub-untyped of init's boot
     * block) — svcmgr retypes every service endpoint / IRQ notification /
     * reply object from it.  Sized for the whole catalog plus per-service
     * reply sub-untypeds and restart churn. */
    handle_id_t sm_untyped_h = HANDLE_INVALID;
    {
        /* Stage 6: svcmgr's pool funds everything its subtree consumes, not
         * just its own endpoints and replies — each child's address space and
         * kernel state (Etapas 2-4), the loader's segment and stack VMOs per
         * spawn AND per restart, and vfs's copies of the initrd images
         * (Step 5).  A bump allocator does not rewind, so a restart costs its
         * images again until the pool is RESET; the budget is sized for that
         * rather than pretending the memory is free. */
        static const uint64_t s1_sm_ut_sizes[] =
            { 32u<<20, 16u<<20, 4u<<20, 1u<<20 };
        for (uint32_t szi = 0; szi < 4u && sm_untyped_h == HANDLE_INVALID; szi++) {
            long ur = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_UNTYPED,
                                       INIT_SLOT_SM_UNTYPED, s1_sm_ut_sizes[szi]);
            if (ur >= 0) sm_untyped_h = (handle_id_t)INIT_SLOT_SM_UNTYPED;
        }
        if (sm_untyped_h == HANDLE_INVALID)
            init_log("[USER][INIT] svcmgr untyped carve FAILED\n");
    }

    {
        struct svc_mint sm_mints[8] = { 0 };
        uint32_t n = 0;
        sm_mints[n].slot   = IRIS_CPTR_CONSOLE_EP;
        sm_mints[n].src_cptr = g_init_console_ep_h;
        sm_mints[n].rights = RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;   /* unbadged: svcmgr re-mints per child badge */
        n++;
        sm_mints[n].slot   = IRIS_CPTR_OWN_EP;
        sm_mints[n].src_cptr = svcmgr_ep_h;
        /* TRANSFER is required so svcmgr can hand out dup'd "svcmgr.ep" caps via
         * SYS_REPLY cap-transfer (EP_LOOKUP_NAME of "svcmgr.ep"). */
        sm_mints[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;   /* unbadged: svcmgr re-mints per child badge */
        n++;
        sm_mints[n].slot     = IRIS_CPTR_PROC_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_PROC_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        /* svcmgr loads service images and forwards image-reading authority to
         * vfs — which, unlike before, does NOT come with the authority to
         * create processes. */
        sm_mints[n].slot     = IRIS_CPTR_INITRD_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_INITRD_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        /* Stage 5 Step 2: svcmgr claims the machine's IRQs and port ranges on
         * behalf of the catalog, so it gets the two control capabilities that
         * authorise exactly that — and, once it has claimed everything, drops
         * them.  They used to be one bit on the capability above. */
        sm_mints[n].slot     = IRIS_CPTR_IRQ_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_IRQ_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        sm_mints[n].slot     = IRIS_CPTR_IOPORT_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_IOPORT_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        /* svcmgr drains the kernel log for its diagnostics endpoint and powers
         * the machine off on request: that is the debug control capability,
         * and nothing else it holds implies it. */
        sm_mints[n].slot     = IRIS_CPTR_DEBUG_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_DEBUG_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        if (sm_untyped_h != HANDLE_INVALID) {
            sm_mints[n].slot   = IRIS_CPTR_OWN_UNTYPED;
            sm_mints[n].src_cptr = sm_untyped_h;
            sm_mints[n].rights = RIGHT_READ | RIGHT_WRITE |
                                 RIGHT_DUPLICATE | RIGHT_TRANSFER;
            sm_mints[n].badge  = 0;
            n++;
        }
        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "svcmgr", &svcmgr_proc_h,
                            &svcmgr_chan_h, sm_mints, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               8u << 20, /*own_budget_slot=*/0);  /* has slot 12 already */
    }
    /* svcmgr's slot-12 mint keeps the pool alive: drop ours. */
    (void)init_sys2(SYS_CNODE_DELETE, 0, (long)INIT_SLOT_SM_UNTYPED);
    if (r < 0) goto fail;

    init_close(&svcmgr_chan_h);   /* bootstrap channel unused — svcmgr is CPtr-only */
    init_close(&svcmgr_proc_h);
    return svcmgr_ep_h;

fail:
    init_close(&svcmgr_proc_h);
    init_close(&svcmgr_chan_h);
    if (svcmgr_ep_h != HANDLE_INVALID) init_close(&svcmgr_ep_h);
    return HANDLE_INVALID;
}

/* ── iris_test spawn + wait ──────────────────────────────────────────────── */

/*
 * Spawns iris_test using spawn_cap_h (a dup of bootstrap_h kept before it is
 * closed; consumed here).  Every capability the suite needs — spawn cap,
 * svcmgr/vfs/console/kbd endpoints, test fixtures — is delivered as a
 * pre-start CSpace mint (table below); no bootstrap-channel sends remain
 * (Phase 13/Track I).  Then waits up to 12 seconds for iris_test to exit and
 * logs the final pass/fail result.
 */
void init_spawn_iris_test(handle_id_t sm_h) {
    handle_id_t proc_h      = HANDLE_INVALID;
    handle_id_t boot_h      = HANDLE_INVALID;
    handle_id_t watch_base_h = HANDLE_INVALID; /* death notification (Track B) */
    long r;

    /* Phase 8: the full well-known slot set is pre-start-minted into
     * iris_test (the kind-0x20 bootstrap forward is retired — slot 1 is
     * the only discovery path):
     *   slot 1  — svcmgr discovery ep, RIGHT_WRITE   → T026+/T039/T041
     *   slot 2  — vfs.ep,     RIGHT_WRITE            → T042/T045
     *   slot 3  — console.ep, RIGHT_WRITE            → T043
     *   slot 4  — kbd.ep,     RIGHT_WRITE            → T044
     *   slot 30 — KNotification, RIGHT_WRITE (wrong type) → T040 WRONG_TYPE
     *   slot 31 — svcmgr ep, RIGHT_TRANSFER only     → T040 ACCESS_DENIED
     *             (the dual resolver must NOT fall back to handles).
     * Phase 13/Track I: svcmgr.ep/vfs.ep/kbd.ep come from EP_LOOKUP_NAME over
     * init's svcmgr.ep (init holds a supervisor badge → full granted rights,
     * including DUPLICATE for the mint).  Missing caps leave slots empty: the
     * tests FAIL loudly, never skip. */
    /* Step 4: declare a receive slot for every lookup.  A recv that declares
     * none takes the delivery-by-handle path, which is the last IPC producer
     * of handles in the productive tree.  Slots 54..56 are free in init. */
    handle_id_t lk_svcmgr = init_ep_lookup_name_slot(sm_h, "svcmgr.ep",
                                                     INIT_RSLOT_LK_SVCMGR);
    handle_id_t lk_vfs    = init_ep_lookup_name_slot(sm_h, "vfs.ep",
                                                     INIT_RSLOT_LK_VFS);
    handle_id_t lk_kbd    = init_ep_lookup_name_slot(sm_h, "kbd.ep",
                                                     INIT_RSLOT_LK_KBD);
    /* Phase 13/Track I: a KNotification serves as the slot-30 wrong-type fixture
     * for T040 (replaces the retired console KChannel cap).  It carries
     * RIGHT_WRITE so EP_CALL passes the rights check and fails on TYPE
     * (WRONG_TYPE), not ACCESS_DENIED. */
    handle_id_t fix_wrongtype = HANDLE_INVALID;
    {
        long nr = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_NOTIFICATION,
                                   INIT_SLOT_FIX_WRONGTY, 0);
        if (nr >= 0) fix_wrongtype = (handle_id_t)INIT_SLOT_FIX_WRONGTY;
    }
    if (lk_svcmgr == HANDLE_INVALID)
        init_log("[USER][INIT] svcmgr.ep lookup FAILED\n");

    /* Phase 18: forward the boot KUntyped (received from userboot at
     * IRIS_CPTR_INIT_UNTYPED) on to iris_test for the ring-3 authority suite.
     * Resolve init's CSpace slot into a mint-source handle; full rights so the
     * suite can retype (WRITE) and revoke.  Absent grant → slot stays empty and
     * T125–T131 FAIL loudly. */
    handle_id_t lk_untyped = HANDLE_INVALID;
    {
        /* Phase S1: iris_test receives its OWN sub-untyped (carved from init's
         * pool) instead of a second cap to the shared boot block — the suite
         * can retype/reset it freely without touching init/svcmgr objects.
         * Sized generously for the object-churn suites; smaller fallbacks
         * keep the authority tests alive on small boot blocks. */
        /* Stage 6 Step 2: page tables are charged to the pool of whoever
         * spawns, so the suite — which spawns a hundred-odd children across
         * the lifecycle and pager tests — needs a budget sized for their
         * address spaces (about five tables each), not just for the objects
         * it fabricates.  The ladder keeps the authority tests alive on a
         * small boot block. */
        static const uint64_t s1_test_ut_sizes[] =
            { 96u<<20, 32u<<20, 8u<<20, 2u<<20 };
        /* Stage 6: carve the suite's budget from the SECOND boot block when
         * userboot handed one over, so the suite and svcmgr do not compete for
         * the same block now that every address space, process and VMO page
         * is charged to somebody's budget. */
        uint64_t test_ut_src = IRIS_CPTR_INIT_UNTYPED2;
        if (init_sys3(SYS_UNTYPED_INFO, (long)test_ut_src, 0, 0) != 0)
            test_ut_src = g_init_untyped_c;
        for (uint32_t szi = 0; szi < 4u && lk_untyped == HANDLE_INVALID; szi++) {
            long ur = init_retype_slot(test_ut_src, IRIS_KOBJ_UNTYPED,
                                       INIT_SLOT_TEST_UNTYPED, s1_test_ut_sizes[szi]);
            if (ur >= 0) lk_untyped = (handle_id_t)INIT_SLOT_TEST_UNTYPED;
        }
        if (lk_untyped == HANDLE_INVALID)
            init_log("[USER][INIT] test untyped carve FAILED\n");
    }

    {
        /* Phase 9: slots 1-4 carry IRIS_BADGE_IRIS_TEST so every server can
         * verify who is calling; slot 28 is a SECOND cap to the svcmgr
         * endpoint with a different badge (T053: two caps, same endpoint,
         * different identities). */
        struct svc_mint it_mints[17] = { 0 };
        it_mints[0].slot = IRIS_CPTR_SVCMGR_EP;
        it_mints[0].src_h = lk_svcmgr;
        it_mints[0].rights = RIGHT_WRITE;
        it_mints[0].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[1].slot = IRIS_CPTR_VFS_EP;
        it_mints[1].src_h = lk_vfs;
        it_mints[1].rights = RIGHT_WRITE;
        it_mints[1].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[2].slot = IRIS_CPTR_CONSOLE_EP;
        it_mints[2].src_cptr = g_init_console_ep_h;
        it_mints[2].rights = RIGHT_WRITE;
        it_mints[2].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[3].slot = IRIS_CPTR_KBD_EP;
        it_mints[3].src_h = lk_kbd;
        it_mints[3].rights = RIGHT_WRITE;
        it_mints[3].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[4].slot = IRIS_CPTR_TEST_FIX_A;
        it_mints[4].src_cptr = fix_wrongtype;          /* wrong type (KNotification, not endpoint) */
        it_mints[4].rights = RIGHT_WRITE;              /* WRITE so EP_CALL fails on TYPE, not rights */
        it_mints[4].badge = 0;
        it_mints[5].slot = IRIS_CPTR_TEST_FIX_B;
        it_mints[5].src_h = lk_svcmgr;                 /* TRANSFER only */
        it_mints[5].rights = RIGHT_TRANSFER;
        it_mints[5].badge = 0;
        it_mints[6].slot = IRIS_CPTR_TEST_FIX_C;
        it_mints[6].src_h = lk_svcmgr;                 /* badge B fixture */
        it_mints[6].rights = RIGHT_WRITE;
        it_mints[6].badge = IRIS_BADGE_TEST_B;
        /* Phase 10: supervisor-badged svcmgr cap so iris_test can drive the
         * privileged RESTART path (real death→respawn E2E, T057/T060). */
        it_mints[7].slot = IRIS_CPTR_TEST_SUPER;
        it_mints[7].src_h = lk_svcmgr;
        it_mints[7].rights = RIGHT_WRITE;
        it_mints[7].badge = IRIS_BADGE_INIT;
        /* Phase 13: an authority cap in a CPtr slot — iris_test invokes it by
         * CPtr to prove device authority resolves via CSpace (T069).
         * Stage 5 Step 2: that cap is the ioport CONTROL capability now, so
         * the test names something that authorises exactly one syscall. */
        it_mints[8].slot = IRIS_CPTR_IOPORT_CONTROL;
        it_mints[8].src_cptr = IRIS_CPTR_IOPORT_CONTROL;
        it_mints[8].rights = RIGHT_READ;
        it_mints[8].badge = 0;
        /* Phase 13 (Track I): the suite's operational authorities are pre-start
         * mints — no bootstrap KChannel send.  Stage 5 Step 2: they are three
         * capabilities, because the suite does three different things with
         * them (spawn children, read boot images, probe the framebuffer). */
        it_mints[9].slot = IRIS_CPTR_PROC_CONTROL;
        it_mints[9].src_cptr = IRIS_CPTR_PROC_CONTROL;
        it_mints[9].rights = RIGHT_READ;
        it_mints[9].badge = 0;
        /* Stage 5 Step 2: the suite creates IRQ capabilities in several
         * tests and asserts that each control capability authorises ONLY its
         * own syscall (T296).  The ioport half arrives at index 8 above. */
        it_mints[13].slot = IRIS_CPTR_IRQ_CONTROL;
        it_mints[13].src_cptr = IRIS_CPTR_IRQ_CONTROL;
        it_mints[13].rights = RIGHT_READ;
        it_mints[13].badge = 0;
        /* The suite reads scheduler and IPC statistics in ~20 tests; that is
         * debug authority and now says nothing about spawning. */
        it_mints[14].slot = IRIS_CPTR_DEBUG_CONTROL;
        it_mints[14].src_cptr = IRIS_CPTR_DEBUG_CONTROL;
        it_mints[14].rights = RIGHT_READ;
        it_mints[14].badge = 0;
        it_mints[15].slot = IRIS_CPTR_INITRD_CONTROL;
        it_mints[15].src_cptr = IRIS_CPTR_INITRD_CONTROL;
        it_mints[15].rights = RIGHT_READ;
        it_mints[15].badge = 0;
        /* T168 asserts the framebuffer VMO is one-shot: it needs the
         * capability that authorises the call, or it would be asserting a
         * denial instead. */
        it_mints[16].slot = IRIS_CPTR_FB_CONTROL;
        it_mints[16].src_cptr = IRIS_CPTR_FB_CONTROL;
        it_mints[16].rights = RIGHT_READ;
        it_mints[16].badge = 0;
        /* Phase 18: the boot KUntyped for the authority suite (T125–T131). */
        it_mints[10].slot = IRIS_CPTR_TEST_UNTYPED;
        it_mints[10].src_cptr = lk_untyped;   /* 0 → skipped by svc_load */
        it_mints[10].rights = RIGHT_READ | RIGHT_WRITE |
                              RIGHT_DUPLICATE | RIGHT_TRANSFER;
        it_mints[10].badge = 0;
        /* Phase 28.1: the supervisor-side file-grant caps for iris_test (the
         * pager supervisor in the runtime suite).  Two slots, because a badged
         * cap can never be re-badged:
         *   slot 58 — the grant ADMIN identity: call-only (WRITE) vfs.ep cap
         *             badged IRIS_BADGE_FILEGRANT_ADMIN.  Drives GRANT_OPEN /
         *             GRANT_REVOKE / GRANT_SESSION_RESET at the VFS.
         *   slot 59 — the session-cap MINT SOURCE: an UNBADGED vfs.ep cap with
         *             WRITE|DUPLICATE|TRANSFER.  Fresh session badges
         *             (IRIS_BADGE_FILEGRANT_S(s)) are minted from it into each
         *             pager instance; invoked directly it is an ordinary
         *             unbadged client (no grant authority).
         * lk_vfs came from init's own supervisor-badged lookup
         * (WRITE|DUPLICATE|TRANSFER); the ordinary client lookup strips
         * DUPLICATE, so this pre-mint is the only honest source.
         * HANDLE_INVALID (lookup miss) → svc_load skips it, and the
         * file-backed suite gates loudly. */
        it_mints[11].slot  = IRIS_CPTR_TEST_VFS_DUP;
        it_mints[11].src_h = lk_vfs;
        it_mints[11].rights = RIGHT_WRITE;
        it_mints[11].badge = IRIS_BADGE_FILEGRANT_ADMIN;
        it_mints[12].slot  = IRIS_CPTR_TEST_VFS_MINT;
        it_mints[12].src_h = lk_vfs;
        it_mints[12].rights = RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        it_mints[12].badge = 0;
        /* Step 4: the loader authority is our spawn-cap SLOT.  SYS_INITRD_VMO
         * and SYS_PROCESS_CREATE both resolve it either way, and the slot
         * outlives bootstrap_h by construction — which is the only reason the
         * retired duplicate had to exist. */
        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "iris_test",
                            &proc_h, &boot_h, it_mints, 17u,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               16u << 20, /*own_budget_slot=*/0); /* has TEST_UNTYPED */
    }
    init_close(&lk_svcmgr);
    init_close(&lk_vfs);
    init_close(&lk_kbd);
    (void)init_sys2(SYS_CNODE_DELETE, 0, (long)INIT_SLOT_TEST_UNTYPED);
    if (fix_wrongtype != 0u)
        (void)init_sys2(SYS_CNODE_DELETE, 0, (long)INIT_SLOT_FIX_WRONGTY);
    if (r < 0) {
        init_log("[USER][INIT] iris_test load FAILED\n");
        goto out;
    }

    /* A1 Increment 1: mint iris_test's OWN process cap (RIGHT_WRITE) into its
     * CSpace (slot 25) so the suite can SYS_PROC_CSPACE_MINT runtime-created
     * caps into its own slots — T079 mints a VMO and maps it by CPtr.  The
     * source handle only exists after the load, hence post-start; T079 runs
     * late in the suite and retries the resolve, then FAILS loudly. */
    if (init_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h,
                  (long)IRIS_CPTR_TEST_PROC, (long)proc_h,
                  (long)RIGHT_WRITE) != 0)
        init_log("[USER][INIT] iris_test self-proc mint FAILED\n");

    /* Phase 13 (Track I): the iris_test spawn cap is delivered as the
     * IRIS_CPTR_SPAWN_CAP pre-start mint above — no KChannel SPAWN_CAP send. */
    init_close(&boot_h);

    /* Phase 13 (Track B): process-exit watch is delivered as a KNotification
     * signal.  One notification (full rights) serves both the watch arm
     * (RIGHT_WRITE) and our own wait (RIGHT_WAIT); bit 0 marks iris_test. */
    /* Phase S1: retyped from init's untyped pool (SYS_NOTIFY_CREATE retired). */
    r = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_NOTIFICATION,
                         INIT_SLOT_WATCH_NOTIF, 0);
    if (r < 0) goto out;
    watch_base_h = (handle_id_t)INIT_SLOT_WATCH_NOTIF;

    r = init_sys3(SYS_PROCESS_WATCH, (long)proc_h, (long)watch_base_h, 1);
    if (r < 0) {
        init_log("[USER][INIT] iris_test watch FAILED\n");
        goto out;
    }

    /* Wait up to 12 seconds for iris_test to exit */
    {
        uint64_t bits = 0;
        r = init_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)watch_base_h,
                      (long)&bits, 12000000000LL);
    }
    if (r < 0) {
        init_log("[USER][INIT] iris_test wait TIMEOUT\n");
    } else {
        long ec = init_sys1(SYS_PROCESS_EXIT_CODE, (long)proc_h);
        if (ec == 0)
            init_log("[USER][INIT] iris_test PASS\n");
        else
            init_log("[USER][INIT] iris_test FAIL\n");
    }

out:
    init_close(&proc_h);
    init_close(&boot_h);
    (void)init_sys2(SYS_CNODE_DELETE, 0, (long)INIT_SLOT_WATCH_NOTIF);
    /* Step 4: nothing to close — the loader authority was our own CSpace slot,
     * not a duplicate this function owned and had to release. */
}
