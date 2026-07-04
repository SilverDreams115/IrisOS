/*
 * main.c — init service (ring-3 ELF, phase 22+).
 *
 * Receives a legacy bootstrap handle in %rdi (set by entry.S from %rbx).  That
 * handle is now vestigial — init closes it immediately.  The real spawn/bootstrap
 * capability arrives as a pre-start CPtr mint (IRIS_CPTR_SPAWN_CAP) in init's
 * CSpace, resolved via SYS_CSPACE_RESOLVE — no KChannel is involved.
 *
 * Fase 13/Track I: init no longer talks to kbd over a legacy KChannel — kbd is
 * endpoint-only and sh is the keystroke consumer (kbd.ep pull).  init validates
 * system health, runs the iris_test suite, then enters a quiet idle loop.
 *
 * Boot sequence validated:
 *   1. Resolve "vfs.ep" via the svcmgr discovery endpoint (Fase 7.2)
 *   2. VFS EP LIST + STAT/READ_AT of the boot file (stateless)
 *   3. Exception-delivery selftest (S8) + iris_test ring-3 suite
 *   4. Idle loop (init never exits)
 */

#include "init.h"
#include <iris/svcmgr_proto.h>
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>
#include <iris/kbd_proto.h>
#include <iris/vfs.h>
#include <iris/console_proto.h>
#include "../common/svc_loader.h"
#include "../common/console_client.h"

/* ── Utilities ──────────────────────────────────────────────────────────── */

/* Console KEndpoint master (Fase 7.3): init creates it, console serves it,
 * svcmgr publishes the send side as "console.ep".  Fase 13/Track I: the legacy
 * console KChannel write handle (g_init_console_h) is retired — init logs over
 * console.ep, with early-serial as the only pre-console.ep fallback. */
static handle_id_t g_init_console_ep_h = HANDLE_INVALID;
static uint8_t g_init_con_ep_buf[IRIS_IPC_BUF_SIZE];
static handle_id_t g_init_early_serial_h = HANDLE_INVALID;
static void init_early_serial_write(const char *s);

void init_log(const char *s) {
    /* Fase 13 (Track I): endpoint-first over console.ep (synchronous flush
     * barrier) once it exists; the only pre-console.ep fallback is the direct
     * UART (early-serial) — never the legacy console KChannel.  No silent
     * fallback after verification: a broken EP drops the gated markers and
     * fails smoke. */
    if (g_init_console_ep_h != HANDLE_INVALID) {
        (void)console_ep_write(g_init_console_ep_h, g_init_con_ep_buf, s);
        return;
    }
    init_early_serial_write(s);
}

static void init_early_serial_write(const char *s) {
    if (g_init_early_serial_h == HANDLE_INVALID || !s) return;
    while (*s) {
        long v;
        do {
            v = init_sys2(SYS_IOPORT_IN, (long)g_init_early_serial_h, 5);
        } while (v < 0 || !((uint8_t)v & 0x20u));
        (void)init_sys3(SYS_IOPORT_OUT, (long)g_init_early_serial_h, 0,
                        (long)(uint8_t)*s++);
    }
}

static void init_early_serial_start(handle_id_t spawn_cap_h) {
    long h;
    if (spawn_cap_h == HANDLE_INVALID || g_init_early_serial_h != HANDLE_INVALID) return;
    h = init_sys3(SYS_CAP_CREATE_IOPORT, (long)spawn_cap_h, 0x3F8, 8);
    if (h < 0) return;
    g_init_early_serial_h = (handle_id_t)h;
}

static const char init_stage_lookup[]    = "[USER][INIT][S1] service lookup\n";
static const char init_stage_vfs_list[]  = "[USER][INIT][S5] vfs ep list\n";
static const char init_stage_vfs_rw[]    = "[USER][INIT][S6] vfs ep rw\n";
/* init_stage_hello (S2) / init_stage_subscribe (S7) retired — Fase 13/Track I */
/* init_stage_exception (S8) lives in init_test.c — Fase 14/Inc 2 */
/* init_stage_seal/init_stage_rights (S9/S10) retired — Fase 13/Track F */
static const char init_stage_healthy[]   = "[USER][INIT][BOOT] healthy path OK\n";
static const char init_console_load_fail[] = "[INIT] console load FAILED\r\n";
static const char init_console_ioport_fail[] = "[INIT] console ioport FAILED\r\n";
static const char init_console_chan_fail[] = "[INIT] console ep FAILED\r\n";
/* Fase 13/Track I: readdup/writedup/boot_ioport/boot_service fail strings
 * retired with the legacy console KChannel bootstrap. */
static const char init_fb_load_fail[] = "[INIT] fb load FAILED\r\n";

#define INIT_RETRY_LIMIT 100
#define INIT_RETRY_SLEEP_TICKS 2
#define INIT_RUNTIME_ENDPOINT 0x8001u

static void init_exit(long code) {
    init_sys1(SYS_EXIT, code);
    /* unreachable */
    for (;;) {}
}

static void init_close(handle_id_t *h) {
    if (*h != HANDLE_INVALID && *h != 0) {
        init_sys1(SYS_HANDLE_CLOSE, (long)*h);
    }
    *h = HANDLE_INVALID;
}

static void init_early_serial_stop(void) {
    init_close(&g_init_early_serial_h);
}

/* init_msg_zero retired — Fase 13/Track I (no KChannel messages in init). */

/* Runtime probes + S8 exception selftest extracted to init_test.c — Fase 14/Inc 2. */

/* Fase 13 (Track F): init S9 (channel seal) and S10 (rights reduction)
 * KChannel selftests retired — the seal/close and rights-reduction
 * semantics are covered by the endpoint/cap-transfer runtime tests in
 * iris_test (T019 close-wakes-waiter, T052/T064 rights+transfer). */

/* ── iris_test spawn + wait ──────────────────────────────────────────────── */

/* Defined below; resolves a service name over svcmgr.ep via EP_LOOKUP_NAME.
 * init holds a supervisor badge so it receives the full granted rights
 * (WRITE|DUPLICATE|TRANSFER for svcmgr.ep, WRITE|DUPLICATE for vfs/kbd.ep). */
static handle_id_t init_ep_lookup_name(handle_id_t svcmgr_ep_h, const char *name);

/*
 * Spawns iris_test using spawn_cap_h (a dup of bootstrap_h kept before it is
 * closed).  Sends the cap via bootstrap channel, plus the svcmgr discovery
 * endpoint (looked up as "svcmgr.ep" over the legacy channel) so the suite
 * can exercise the EP-based service path (T026+).  Then waits up to 12
 * seconds for iris_test to exit and logs the final pass/fail result.
 */
static void init_spawn_iris_test(handle_id_t spawn_cap_h, handle_id_t sm_h) {
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

    {
        /* Fase 9: slots 1-4 carry IRIS_BADGE_IRIS_TEST so every server can
         * verify who is calling; slot 28 is a SECOND cap to the svcmgr
         * endpoint with a different badge (T053: two caps, same endpoint,
         * different identities). */
        struct svc_mint it_mints[10];
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
        r = svc_load_minted(spawn_cap_h, "iris_test", &proc_h, &boot_h,
                            it_mints, 10u);
    }
    init_close(&lk_svcmgr);
    init_close(&lk_vfs);
    init_close(&lk_kbd);
    if (fix_wrongtype != HANDLE_INVALID) init_close(&fix_wrongtype);
    if (r < 0) {
        init_log("[USER][INIT] iris_test load FAILED\n");
        goto out;
    }

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

static void init_retry_pause(void) {
    (void)init_sys1(SYS_SLEEP, INIT_RETRY_SLEEP_TICKS);
}

/* Fase 13 (Track I): init's spawn/bootstrap KBootstrapCap arrives as the
 * IRIS_CPTR_SPAWN_CAP (slot 6) pre-start mint from userboot — not over a
 * bootstrap KChannel.  Resolve the slot to a handle (init DUPs/restricts it for
 * its children, which needs a handle-table handle, not a bare CPtr). */
static handle_id_t init_recv_spawn_cap(handle_id_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;
    long h = init_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    return (h >= 0) ? (handle_id_t)h : HANDLE_INVALID;
}

/* ── Legacy channel send/recv helper (retired) ──────────────────────────── */

/* init_chan_send_recv retired — Fase 13/Track I (kbd HELLO/STATUS was its
 * only caller; kbd is endpoint-only now). */

/* ── fb spawn (Phase 30: ring-3 framebuffer painter) ────────────────────── */

static void init_spawn_fb(handle_id_t spawn_cap_h) {
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
static int init_spawn_console(handle_id_t spawn_cap_h) {
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
static handle_id_t init_spawn_svcmgr(handle_id_t spawn_cap_h) {
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

/* ── svcmgr lookup ──────────────────────────────────────────────────────── */
/*
 * Create a one-shot reply channel, dup the write end into svcmgr's table via
 * handle-transfer, send SVCMGR_MSG_LOOKUP, receive SVCMGR_MSG_LOOKUP_REPLY on
 * the read end.  Returns the attached handle on success, HANDLE_INVALID on error.
 */
/* Fase 13/Track I: init_lookup / init_lookup_wait / init_lookup_name (the
 * legacy KChannel SVCMGR_MSG_LOOKUP[_NAME] discovery) are fully retired —
 * init discovers services via EP_LOOKUP_NAME over svcmgr.ep
 * (init_ep_lookup_name).  No legacy LOOKUP, no fallback. */


/* ── VFS endpoint client (Fase 7.2) ─────────────────────────────────────── */

/* EP_CALL bulk buffer: the request path and the reply data share this buffer
 * (EP_CALL reuses buf_uptr in both directions). +1 for a guard NUL. */
static uint8_t g_init_ep_buf[VFS_EP_DATA_MAX + 1u];

static void init_imsg_zero(struct IrisMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

/*
 * Resolve a service name (e.g. "vfs.ep") through the svcmgr discovery endpoint:
 * EP_CALL(svcmgr_ep, IRIS_SVCMGR_EP_LOOKUP_NAME, name).  The reply carries the
 * endpoint cap via SYS_REPLY cap transfer.  Returns HANDLE_INVALID on any
 * failure (caller retries / fails fast).  Fase 13/Track I: this EP_LOOKUP_NAME
 * path replaces the retired legacy KChannel LOOKUP_NAME (init_lookup_name). */
static handle_id_t init_ep_lookup_name(handle_id_t svcmgr_ep_h, const char *name) {
    struct IrisMsg msg;
    uint32_t n = 0;

    if (svcmgr_ep_h == HANDLE_INVALID || !name) return HANDLE_INVALID;
    while (name[n] && n + 1u < (uint32_t)sizeof(g_init_ep_buf)) {
        g_init_ep_buf[n] = (uint8_t)name[n];
        n++;
    }
    g_init_ep_buf[n] = 0u;

    init_imsg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_init_ep_buf;
    msg.buf_len  = n + 1u;  /* includes NUL */

    if (init_sys2(SYS_EP_CALL, (long)svcmgr_ep_h, (long)&msg) != IRIS_OK)
        return HANDLE_INVALID;
    if (msg.label != IRIS_EP_REPLY_OK)
        return HANDLE_INVALID;
    if (msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP)
        return HANDLE_INVALID;
    return (handle_id_t)msg.attached_handle;
}

/*
 * One VFS endpoint round trip. The path (when non-NULL) is staged into
 * g_init_ep_buf; reply bulk data lands in the same buffer.
 */
static int init_vfs_ep_call(handle_id_t vfs_ep_h, struct IrisMsg *msg,
                            const char *path) {
    msg->buf_uptr = (uint64_t)(uintptr_t)g_init_ep_buf;
    if (path) {
        uint32_t plen = 0;
        while (path[plen]) plen++;
        if (plen + 1u > VFS_EP_PATH_MAX) return (int)IRIS_ERR_INVALID_ARG;
        for (uint32_t i = 0; i < plen; i++) g_init_ep_buf[i] = (uint8_t)path[i];
        g_init_ep_buf[plen] = 0u;
        msg->buf_len = plen + 1u;
    }
    return (int)init_sys2(SYS_EP_CALL, (long)vfs_ep_h, (long)msg);
}

/* ── VFS EP LIST check (S5) ─────────────────────────────────────────────── */

static int init_check_vfs_list_ep(handle_id_t vfs_ep_h) {
    struct IrisMsg msg;

    /* indices 0..2 — expect OK with a non-empty name */
    for (uint64_t idx = 0; idx < 3u; idx++) {
        init_imsg_zero(&msg);
        msg.label      = VFS_EP_OP_LIST;
        msg.words[0]   = idx;
        msg.word_count = 1u;
        if (init_vfs_ep_call(vfs_ep_h, &msg, 0) != IRIS_OK) return 0;
        if (msg.label != IRIS_EP_REPLY_OK) return 0;
        if (msg.words[2] == 0u) return 0;  /* name length */
    }

    /* index 100 — expect NOT_FOUND (end-of-listing semantics) */
    init_imsg_zero(&msg);
    msg.label      = VFS_EP_OP_LIST;
    msg.words[0]   = 100u;
    msg.word_count = 1u;
    if (init_vfs_ep_call(vfs_ep_h, &msg, 0) != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_ERR) return 0;
    if (msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) return 0;

    return 1;
}

static int init_wait_vfs_list_ep(handle_id_t vfs_ep_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_check_vfs_list_ep(vfs_ep_h)) return 1;
        init_retry_pause();
    }
    return 0;
}

/* ── VFS EP STAT / READ_AT check (S6) ───────────────────────────────────── */

static int init_check_vfs_rw_ep(handle_id_t vfs_ep_h) {
    struct IrisMsg msg;
    uint64_t size;

    /* STAT of the boot file — expect OK with a sane size */
    init_imsg_zero(&msg);
    msg.label = VFS_EP_OP_STAT;
    if (init_vfs_ep_call(vfs_ep_h, &msg, "iris.txt") != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_OK) return 0;
    size = msg.words[1];
    if (size == 0u || size > VFS_EP_DATA_MAX) return 0;

    /* READ_AT offset 0 — full content in one reply */
    init_imsg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0u;
    msg.words[1]   = VFS_EP_DATA_MAX;
    msg.word_count = 2u;
    if (init_vfs_ep_call(vfs_ep_h, &msg, "iris.txt") != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_OK) return 0;
    if (msg.words[1] != size || msg.words[2] != size) return 0;
    if (msg.buf_len != (uint32_t)size) return 0;

    /* READ_AT offset == size — EOF (0 bytes), not an error */
    init_imsg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = size;
    msg.words[1]   = VFS_EP_DATA_MAX;
    msg.word_count = 2u;
    if (init_vfs_ep_call(vfs_ep_h, &msg, "iris.txt") != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_OK) return 0;
    if (msg.words[1] != 0u || msg.words[2] != size) return 0;

    /* READ_AT of a missing export — NOT_FOUND */
    init_imsg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0u;
    msg.words[1]   = VFS_EP_DATA_MAX;
    msg.word_count = 2u;
    if (init_vfs_ep_call(vfs_ep_h, &msg, "no-such-file") != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_ERR) return 0;
    if (msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) return 0;

    return 1;
}

static int init_wait_vfs_rw_ep(handle_id_t vfs_ep_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_check_vfs_rw_ep(vfs_ep_h)) return 1;
        init_retry_pause();
    }
    return 0;
}

/* Fase 13 (Track I): the KBD HELLO/SUBSCRIBE legacy-KChannel helpers and the
 * PS/2 scancode→ASCII echo table are retired — kbd is endpoint-only and sh
 * is the keystroke consumer (kbd.ep pull). */


/* ── Echo loop ──────────────────────────────────────────────────────────── */

/* Fase 13 (Track I): init's interactive echo loop is retired — sh is the
 * keystroke consumer (kbd.ep pull).  init's final state is a quiet idle loop so
 * the process never exits (which would tear it down). */
static void init_idle_loop(void) {
    init_log("[USER] init idle loop start\n");
    for (;;) {
        init_sys1(SYS_SLEEP, 100);
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void init_main(handle_id_t bootstrap_ch_h) {
    handle_id_t bootstrap_h        = HANDLE_INVALID;
    handle_id_t sm_h               = HANDLE_INVALID;
    handle_id_t vfs_ep_h           = HANDLE_INVALID;
    handle_id_t iris_test_spawn_h  = HANDLE_INVALID;

    bootstrap_h = init_recv_spawn_cap(bootstrap_ch_h);
    init_close(&bootstrap_ch_h);
    if (bootstrap_h == HANDLE_INVALID)
        init_exit(1);
    init_early_serial_start(bootstrap_h);

    /* Spawn fb first (fire-and-forget): it claims the framebuffer and exits. */
    init_spawn_fb(bootstrap_h);

    /* Spawn console: endpoint-only, CPtr-provisioned (Fase 13/Track I). */
    if (!init_spawn_console(bootstrap_h)) {
        init_early_serial_write("[INIT] console spawn FAILED\r\n");
        init_exit(1);
    }

    /* Verify the console endpoint with the first gated write (Fase 7.3): the
     * EP_CALL blocks until console serves it, so this also synchronizes with
     * console boot.  Done BEFORE early-serial is stopped so a broken EP can
     * still report LOUDLY over the direct UART (the missing OK marker fails
     * smoke either way — no legacy console KChannel fallback). */
    if (g_init_console_ep_h != HANDLE_INVALID) {
        if (console_ep_write(g_init_console_ep_h, g_init_con_ep_buf,
                             "[USER] console ep OK\n") != 0) {
            init_close(&g_init_console_ep_h);
            init_early_serial_write("[USER] console ep FAILED\n");
        }
    } else {
        init_early_serial_write("[USER] console ep FAILED\n");
    }
    init_early_serial_stop();
    /* From here all init_log() calls go through console.ep. */

    init_log("[USER] init bootstrap start\n");

    sm_h = init_spawn_svcmgr(bootstrap_h);
    if (sm_h == HANDLE_INVALID) {
        init_log("[USER] svcmgr spawn FAILED\n");
        init_exit(1);
    }

    /* Dup spawn_cap for iris_test before releasing bootstrap_h */
    {
        long r = init_sys2(SYS_HANDLE_DUP, (long)bootstrap_h,
                           (long)(RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER));
        if (r >= 0) iris_test_spawn_h = (handle_id_t)r;
    }

    init_close(&bootstrap_h);

    /* ── Service discovery ── */
    init_log(init_stage_lookup);
    /* Fase 13 (Track I): init no longer probes kbd over the legacy service/reply
     * KChannel — kbd is endpoint/notification-only.  kbd liveness is covered by
     * sh's "[SH] kbd cptr OK" (a kbd.ep PING) and by T034/T035/T044/T058. */

    /* Fase 7.2/13: the VFS endpoint is the mandatory operational path.  sm_h is
     * svcmgr's discovery endpoint ("svcmgr.ep", owned by init) — look up "vfs.ep"
     * through it via EP_LOOKUP_NAME (retrying until VFS has bootstrapped).
     * Fail-fast: no legacy fallback. */
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        vfs_ep_h = init_ep_lookup_name(sm_h, VFS_EP_SVC_NAME);
        if (vfs_ep_h != HANDLE_INVALID) break;
        init_retry_pause();
    }
    if (vfs_ep_h == HANDLE_INVALID) {
        init_log("[USER] vfs.ep lookup FAILED\n");
        init_exit(5);
    }

    /* Fase 13 (Track E/F): the legacy KChannel diagnostics + dynamic-registry
     * self-tests were retired; their coverage now lives in the endpoint suite
     * (EP_DIAG → T067, cap-backed REGISTER/LOOKUP/UNREGISTER → T054/T063–T066). */

    /* ── VFS EP LIST ── */
    init_log(init_stage_vfs_list);
    if (!init_wait_vfs_list_ep(vfs_ep_h)) {
        init_log("[USER] vfs ep list FAILED\n");
        init_exit(9);
    }
    init_log("[USER] vfs ep list OK\n");

    /* ── VFS EP STAT / READ_AT ── */
    init_log(init_stage_vfs_rw);
    if (!init_wait_vfs_rw_ep(vfs_ep_h)) {
        init_log("[USER] vfs ep rw FAILED\n");
        init_exit(10);
    }
    init_log("[USER] vfs ep stat OK\n");
    init_log("[USER] vfs ep read OK\n");

    /* Fase 13 (Track I): KBD SUBSCRIBE / shared-reply probes retired — kbd is
     * endpoint/notification-only and sh consumes keystrokes via "kbd.ep" (pull).
     * init no longer subscribes to a push channel. */
    init_log(init_stage_healthy);

    /* Fase 13 (Track F): the ring-3 timed-IPC KChannel selftest (CHAN_RECV_TIMEOUT
     * → TIMED_OUT) is retired — covered by iris_test T010 (NOTIFY_WAIT_TIMEOUT). */

    init_runtime_probe_invalid_userptr();
    init_runtime_probe_timeout_overflow();
    init_selftest_exception();

    /* ── Block 8: iris_test ring-3 syscall test suite ── */
    if (iris_test_spawn_h != HANDLE_INVALID) {
        /* Drain our queued console output first: iris_test writes raw to
         * COM1, and a half-flushed backlog line would otherwise interleave
         * mid-line with test output under load. */
        if (g_init_console_ep_h != HANDLE_INVALID)
            (void)console_ep_sync(g_init_console_ep_h);
        init_spawn_iris_test(iris_test_spawn_h, sm_h);
        iris_test_spawn_h = HANDLE_INVALID;
    }

    /* ── Idle loop (init never exits) ── */
    init_idle_loop();

    /* unreachable */
    init_exit(0);
}
