/*
 * init_launch.c — service launch for init (Fase 14 extraction).
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

void init_spawn_fb(handle_id_t spawn_cap_h) {
    handle_id_t fb_proc_h  = HANDLE_INVALID;
    handle_id_t fb_boot_h  = HANDLE_INVALID;
    handle_id_t fb_cap_h   = HANDLE_INVALID;
    long r;

    /* Fase 13 (Track I): fb gets its FRAMEBUFFER-restricted KBootstrapCap as a
     * pre-start CSpace mint (IRIS_CPTR_SPAWN_CAP) instead of a post-spawn
     * KChannel SPAWN_CAP send.  Build + restrict the cap BEFORE the load so it
     * can be minted into the child's root CNode. */
    r = init_sys2(SYS_HANDLE_DUP, (long)spawn_cap_h,
                  (long)(RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER));
    if (r < 0) {
        init_early_serial_write(init_fb_load_fail);
        goto out;
    }
    fb_cap_h = (handle_id_t)r;

    /* Restrict to FRAMEBUFFER only; original spawn_cap_h is unaffected. */
    r = init_sys2(SYS_BOOTCAP_RESTRICT, (long)fb_cap_h,
                  (long)IRIS_BOOTCAP_FRAMEBUFFER);
    if (r < 0) goto out;

    {
        struct svc_mint fb_mints[1];
        fb_mints[0].slot   = IRIS_CPTR_SPAWN_CAP;
        fb_mints[0].src_h  = fb_cap_h;
        fb_mints[0].rights = RIGHT_READ;
        fb_mints[0].badge  = 0;
        r = svc_load_minted(spawn_cap_h, "fb", &fb_proc_h, &fb_boot_h,
                            fb_mints, 1u);
    }
    if (r < 0) {
        init_early_serial_write(init_fb_load_fail);
        goto out;
    }

out:
    init_close(&fb_proc_h);
    init_close(&fb_boot_h);
    if (fb_cap_h != HANDLE_INVALID) init_close(&fb_cap_h);
}

/* ── console spawn (Phase 30: ring-3 serial console service) ────────────── */

/* Fase 13 (Track I): console is endpoint-only and CPtr-provisioned — its
 * endpoint recv side (IRIS_CPTR_OWN_EP) and its 0x3F8 UART KIoPort
 * (IRIS_CPTR_IOPORT) are pre-start mints; no legacy console KChannel pair, no
 * bootstrap sends.  Returns 1 on success, 0 on failure. */
int init_spawn_console(handle_id_t spawn_cap_h) {
    handle_id_t con_proc_h  = HANDLE_INVALID;
    handle_id_t con_boot_h  = HANDLE_INVALID;
    handle_id_t ioport_h    = HANDLE_INVALID;
    long r;

    /* Console KEndpoint master (init owns it); recv side minted to the child. */
    r = init_sys0(SYS_ENDPOINT_CREATE);
    if (r < 0) {
        init_early_serial_write(init_console_chan_fail);
        goto fail;
    }
    g_init_console_ep_h = (handle_id_t)r;

    /* KIoPort for the 8 UART registers at 0x3F8..0x3FF (IN poll LSR + OUT THR). */
    r = init_sys3(SYS_CAP_CREATE_IOPORT, (long)spawn_cap_h, 0x3F8, 8);
    if (r < 0) {
        init_early_serial_write(init_console_ioport_fail);
        goto fail;
    }
    ioport_h = (handle_id_t)r;

    {
        struct svc_mint con_mints[2];
        con_mints[0].slot   = IRIS_CPTR_OWN_EP;
        con_mints[0].src_h  = g_init_console_ep_h;
        con_mints[0].rights = RIGHT_READ;
        con_mints[0].badge  = 0;   /* server-side cap: unbadged */
        con_mints[1].slot   = IRIS_CPTR_IOPORT;
        con_mints[1].src_h  = ioport_h;
        con_mints[1].rights = RIGHT_READ | RIGHT_WRITE;
        con_mints[1].badge  = 0;
        r = svc_load_minted(spawn_cap_h, "console", &con_proc_h, &con_boot_h,
                            con_mints, 2u);
    }
    if (r < 0) {
        init_early_serial_write(init_console_load_fail);
        goto fail;
    }

    init_close(&ioport_h);    /* console holds the slot-10 mint now */
    init_close(&con_proc_h);
    init_close(&con_boot_h);
    return 1;

fail:
    init_close(&con_proc_h);
    init_close(&con_boot_h);
    if (ioport_h != HANDLE_INVALID) init_close(&ioport_h);
    return 0;
}

/* ── svcmgr spawn (Phase 29: ring-3 loader; Phase 30: also sends console) ── */

/* Fase 13 (Track I): init owns svcmgr's discovery endpoint ("svcmgr.ep").  It
 * creates the endpoint, mints the recv+mint side into svcmgr (IRIS_CPTR_OWN_EP)
 * and keeps the send side for its own EP_LOOKUP_NAME calls.  All of svcmgr's
 * bootstrap caps arrive as pre-start CSpace mints (no bootstrap KChannel):
 *   slot 3 (CONSOLE_EP)  — console.ep, WRITE|DUP|TRANSFER (re-mint to children);
 *   slot 5 (OWN_EP)      — svcmgr.ep recv side, READ|WRITE|DUP (recv + re-mint
 *                          IRIS_CPTR_SVCMGR_EP into catalog children);
 *   slot 6 (SPAWN_CAP)   — spawn/authority cap, READ|DUP|TRANSFER.
 * Returns the svcmgr.ep send side (init's discovery handle), or HANDLE_INVALID. */
handle_id_t init_spawn_svcmgr(handle_id_t spawn_cap_h) {
    handle_id_t svcmgr_proc_h  = HANDLE_INVALID;
    handle_id_t svcmgr_chan_h  = HANDLE_INVALID;
    handle_id_t svcmgr_ep_h    = HANDLE_INVALID;
    handle_id_t spawn_dup_h    = HANDLE_INVALID;
    long r;

    r = init_sys0(SYS_ENDPOINT_CREATE);
    if (r < 0) goto fail;
    svcmgr_ep_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)spawn_cap_h,
                  (long)(RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    spawn_dup_h = (handle_id_t)r;

    {
        struct svc_mint sm_mints[3];
        sm_mints[0].slot   = IRIS_CPTR_CONSOLE_EP;
        sm_mints[0].src_h  = g_init_console_ep_h;
        sm_mints[0].rights = RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[0].badge  = 0;   /* unbadged: svcmgr re-mints per child badge */
        sm_mints[1].slot   = IRIS_CPTR_OWN_EP;
        sm_mints[1].src_h  = svcmgr_ep_h;
        /* TRANSFER is required so svcmgr can hand out dup'd "svcmgr.ep" caps via
         * SYS_REPLY cap-transfer (EP_LOOKUP_NAME of "svcmgr.ep"). */
        sm_mints[1].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[1].badge  = 0;   /* unbadged: svcmgr re-mints per child badge */
        sm_mints[2].slot   = IRIS_CPTR_SPAWN_CAP;
        sm_mints[2].src_h  = spawn_dup_h;
        sm_mints[2].rights = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[2].badge  = 0;
        r = svc_load_minted(spawn_cap_h, "svcmgr", &svcmgr_proc_h,
                            &svcmgr_chan_h, sm_mints, 3u);
    }
    if (r < 0) goto fail;

    init_close(&spawn_dup_h);
    init_close(&svcmgr_chan_h);   /* bootstrap channel unused — svcmgr is CPtr-only */
    init_close(&svcmgr_proc_h);
    return svcmgr_ep_h;

fail:
    init_close(&svcmgr_proc_h);
    init_close(&svcmgr_chan_h);
    if (spawn_dup_h != HANDLE_INVALID) init_close(&spawn_dup_h);
    if (svcmgr_ep_h != HANDLE_INVALID) init_close(&svcmgr_ep_h);
    return HANDLE_INVALID;
}

/* ── iris_test spawn + wait ──────────────────────────────────────────────── */

/*
 * Spawns iris_test using spawn_cap_h (a dup of bootstrap_h kept before it is
 * closed; consumed here).  Every capability the suite needs — spawn cap,
 * svcmgr/vfs/console/kbd endpoints, test fixtures — is delivered as a
 * pre-start CSpace mint (table below); no bootstrap-channel sends remain
 * (Fase 13/Track I).  Then waits up to 12 seconds for iris_test to exit and
 * logs the final pass/fail result.
 */
void init_spawn_iris_test(handle_id_t spawn_cap_h, handle_id_t sm_h) {
    handle_id_t proc_h      = HANDLE_INVALID;
    handle_id_t boot_h      = HANDLE_INVALID;
    handle_id_t watch_base_h = HANDLE_INVALID; /* death notification (Track B) */
    long r;

    /* Fase 8: the full well-known slot set is pre-start-minted into
     * iris_test (the kind-0x20 bootstrap forward is retired — slot 1 is
     * the only discovery path):
     *   slot 1  — svcmgr discovery ep, RIGHT_WRITE   → T026+/T039/T041
     *   slot 2  — vfs.ep,     RIGHT_WRITE            → T042/T045
     *   slot 3  — console.ep, RIGHT_WRITE            → T043
     *   slot 4  — kbd.ep,     RIGHT_WRITE            → T044
     *   slot 30 — KNotification, RIGHT_WRITE (wrong type) → T040 WRONG_TYPE
     *   slot 31 — svcmgr ep, RIGHT_TRANSFER only     → T040 ACCESS_DENIED
     *             (the dual resolver must NOT fall back to handles).
     * Fase 13/Track I: svcmgr.ep/vfs.ep/kbd.ep come from EP_LOOKUP_NAME over
     * init's svcmgr.ep (init holds a supervisor badge → full granted rights,
     * including DUPLICATE for the mint).  Missing caps leave slots empty: the
     * tests FAIL loudly, never skip. */
    handle_id_t lk_svcmgr = init_ep_lookup_name(sm_h, "svcmgr.ep");
    handle_id_t lk_vfs    = init_ep_lookup_name(sm_h, "vfs.ep");
    handle_id_t lk_kbd    = init_ep_lookup_name(sm_h, "kbd.ep");
    /* Fase 13/Track I: a KNotification serves as the slot-30 wrong-type fixture
     * for T040 (replaces the retired console KChannel cap).  It carries
     * RIGHT_WRITE so EP_CALL passes the rights check and fails on TYPE
     * (WRONG_TYPE), not ACCESS_DENIED. */
    handle_id_t fix_wrongtype = HANDLE_INVALID;
    {
        long nr = init_sys0(SYS_NOTIFY_CREATE);
        if (nr >= 0) fix_wrongtype = (handle_id_t)nr;
    }
    if (lk_svcmgr == HANDLE_INVALID)
        init_log("[USER][INIT] svcmgr.ep lookup FAILED\n");

    /* Fase 18: forward the boot KUntyped (received from userboot at
     * IRIS_CPTR_INIT_UNTYPED) on to iris_test for the ring-3 authority suite.
     * Resolve init's CSpace slot into a mint-source handle; full rights so the
     * suite can retype (WRITE) and revoke.  Absent grant → slot stays empty and
     * T125–T131 FAIL loudly. */
    handle_id_t lk_untyped = HANDLE_INVALID;
    {
        long ur = init_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_INIT_UNTYPED);
        if (ur >= 0) lk_untyped = (handle_id_t)ur;
        else init_log("[USER][INIT] boot untyped resolve FAILED\n");
    }

    {
        /* Fase 9: slots 1-4 carry IRIS_BADGE_IRIS_TEST so every server can
         * verify who is calling; slot 28 is a SECOND cap to the svcmgr
         * endpoint with a different badge (T053: two caps, same endpoint,
         * different identities). */
        struct svc_mint it_mints[11];
        it_mints[0].slot = IRIS_CPTR_SVCMGR_EP;
        it_mints[0].src_h = lk_svcmgr;
        it_mints[0].rights = RIGHT_WRITE;
        it_mints[0].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[1].slot = IRIS_CPTR_VFS_EP;
        it_mints[1].src_h = lk_vfs;
        it_mints[1].rights = RIGHT_WRITE;
        it_mints[1].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[2].slot = IRIS_CPTR_CONSOLE_EP;
        it_mints[2].src_h = g_init_console_ep_h;
        it_mints[2].rights = RIGHT_WRITE;
        it_mints[2].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[3].slot = IRIS_CPTR_KBD_EP;
        it_mints[3].src_h = lk_kbd;
        it_mints[3].rights = RIGHT_WRITE;
        it_mints[3].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[4].slot = IRIS_CPTR_TEST_FIX_A;
        it_mints[4].src_h = fix_wrongtype;             /* wrong type (KNotification, not endpoint) */
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
        /* Fase 10: supervisor-badged svcmgr cap so iris_test can drive the
         * privileged RESTART path (real death→respawn E2E, T057/T060). */
        it_mints[7].slot = IRIS_CPTR_TEST_SUPER;
        it_mints[7].src_h = lk_svcmgr;
        it_mints[7].rights = RIGHT_WRITE;
        it_mints[7].badge = IRIS_BADGE_INIT;
        /* Fase 13: a device/authority cap (the spawn KBootstrapCap) in a CPtr
         * slot — iris_test invokes it by CPtr to prove device caps resolve via
         * CSpace (T069). */
        it_mints[8].slot = IRIS_CPTR_TEST_SPAWN;
        it_mints[8].src_h = spawn_cap_h;
        it_mints[8].rights = RIGHT_READ;
        it_mints[8].badge = 0;
        /* Fase 13 (Track I): the operational spawn cap (serial KIoPort +
         * INITRD access) is a pre-start mint too — no bootstrap KChannel send. */
        it_mints[9].slot = IRIS_CPTR_SPAWN_CAP;
        it_mints[9].src_h = spawn_cap_h;
        it_mints[9].rights = RIGHT_READ;
        it_mints[9].badge = 0;
        /* Fase 18: the boot KUntyped for the authority suite (T125–T131). */
        it_mints[10].slot = IRIS_CPTR_TEST_UNTYPED;
        it_mints[10].src_h = lk_untyped;   /* HANDLE_INVALID → skipped by svc_load */
        it_mints[10].rights = RIGHT_READ | RIGHT_WRITE |
                              RIGHT_DUPLICATE | RIGHT_TRANSFER;
        it_mints[10].badge = 0;
        r = svc_load_minted(spawn_cap_h, "iris_test", &proc_h, &boot_h,
                            it_mints, 11u);
    }
    init_close(&lk_svcmgr);
    init_close(&lk_vfs);
    init_close(&lk_kbd);
    init_close(&lk_untyped);
    if (fix_wrongtype != HANDLE_INVALID) init_close(&fix_wrongtype);
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

    /* Fase 13 (Track I): the iris_test spawn cap is delivered as the
     * IRIS_CPTR_SPAWN_CAP pre-start mint above — no KChannel SPAWN_CAP send. */
    init_close(&boot_h);

    /* Fase 13 (Track B): process-exit watch is delivered as a KNotification
     * signal.  One notification (full rights) serves both the watch arm
     * (RIGHT_WRITE) and our own wait (RIGHT_WAIT); bit 0 marks iris_test. */
    r = init_sys0(SYS_NOTIFY_CREATE);
    if (r < 0) goto out;
    watch_base_h = (handle_id_t)r;

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
    init_close(&watch_base_h);
    init_close(&spawn_cap_h);
}
