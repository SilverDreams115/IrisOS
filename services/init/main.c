/*
 * main.c — init service boot supervisor (ring-3 ELF, phase 22+).
 *
 * Phase 14: orchestrator ONLY.  Every helper lives in its module (see the
 * split contract in init.h): init_bootstrap.c (spawn-cap / early-serial /
 * discovery / S5-S6 VFS validation), init_launch.c (fb / console / svcmgr /
 * iris_test spawns), init_test.c (runtime probes + S8).  main.c owns the
 * boot sequence, the log sink and the tiny process utilities — nothing else.
 *
 * Receives a legacy bootstrap handle in %rdi (set by entry.S from %rbx).  That
 * handle is now vestigial — init closes it immediately.  The real spawn/bootstrap
 * capabilities arrive as pre-start CPtr mints (IRIS_CPTR_PROC_CONTROL and the
 * other five control slots) in init's
 * CSpace and is invoked as that slot — no KChannel, and no handle.
 *
 * Boot sequence validated:
 *   1. Resolve "vfs.ep" via the svcmgr discovery endpoint (Phase 7.2)
 *   2. VFS EP LIST + STAT/READ_AT of the boot file (stateless)
 *   3. Exception-delivery selftest (S8) + iris_test ring-3 suite
 *   4. Idle loop (init never exits)
 */

#include "init.h"
#include <iris/vfs_ep_proto.h>          /* VFS_EP_SVC_NAME (S5/S6 lookup) */
#include "../common/console_client.h"
#include "../common/svc_loader.h"   /* console.ep log sink */

/* ── Utilities ──────────────────────────────────────────────────────────── */

/* Console KEndpoint master (Phase 7.3): init creates it, console serves it,
 * svcmgr publishes the send side as "console.ep".  Phase 13/Track I: the legacy
 * console KChannel write handle (g_init_console_h) is retired — init logs over
 * console.ep, with early-serial as the only pre-console.ep fallback. */
handle_id_t g_init_console_ep_h = HANDLE_INVALID;
static uint8_t g_init_con_ep_buf[IRIS_IPC_BUF_SIZE];

/* Phase S1: init's untyped pool — the boot KUntyped userboot minted at slot 12.
 * Every kernel object init fabricates (console/svcmgr endpoints, reply
 * objects, test fixtures) is retyped from it; the sub-untypeds delegated to
 * svcmgr and iris_test are carved from it too. */
uint64_t g_init_untyped_c = 0;

void init_log(const char *s) {
    /* Phase 13 (Track I): endpoint-first over console.ep (synchronous flush
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

static const char init_stage_lookup[]    = "[USER][INIT][S1] service lookup\n";
static const char init_stage_vfs_list[]  = "[USER][INIT][S5] vfs ep list\n";
static const char init_stage_vfs_rw[]    = "[USER][INIT][S6] vfs ep rw\n";
/* init_stage_hello (S2) / init_stage_subscribe (S7) retired — Phase 13/Track I */
/* init_stage_exception (S8) lives in init_test.c — Phase 14/Inc 2 */
/* init_stage_seal/init_stage_rights (S9/S10) retired — Phase 13/Track F */
static const char init_stage_healthy[]   = "[USER][INIT][BOOT] healthy path OK\n";
/* Phase 13/Track I: readdup/writedup/boot_ioport/boot_service fail strings
 * retired with the legacy console KChannel bootstrap.  The console/fb spawn
 * fail strings moved to init_launch.c with their users — Phase 14. */

void init_exit(long code) {
    init_sys1(SYS_EXIT, code);
    /* unreachable */
    for (;;) {}
}

/* Release a capability, whichever namespace names it (Step 4).
 *
 * The loader hands back capabilities that live in CSpace now — including in a
 * second-level CNode — and closing one as a handle is a silent no-op that
 * leaves the slot occupied.  The next spawn then fails to publish into it. */
void init_close(handle_id_t *h) {
    uint32_t v = (uint32_t)*h;
    if (v != 0u) {
        if (v >= 256u)
            init_sys2(SYS_CNODE_DELETE, (long)(v & 0xFFu), (long)(v >> 8));
        else
            init_sys2(SYS_CNODE_DELETE, 0, (long)v);
    }
    *h = HANDLE_INVALID;
}

/* init_msg_zero retired — Phase 13/Track I (no KChannel messages in init). */

/* Runtime probes + S8 exception selftest extracted to init_test.c — Phase 14/Inc 2. */

/* Phase 13 (Track F): init S9 (channel seal) and S10 (rights reduction)
 * KChannel selftests retired — the seal/close and rights-reduction
 * semantics are covered by the endpoint/cap-transfer runtime tests in
 * iris_test (T019 close-wakes-waiter, T052/T064 rights+transfer). */

/* ── iris_test spawn + wait ──────────────────────────────────────────────── */

/* init_spawn_iris_test moved to init_launch.c — Phase 14. */

/* init_retry_pause / init_recv_spawn_cap moved to init_bootstrap.c — Phase 14. */

/* ── Legacy channel send/recv helper (retired) ──────────────────────────── */

/* init_chan_send_recv retired — Phase 13/Track I (kbd HELLO/STATUS was its
 * only caller; kbd is endpoint-only now). */

/* ── fb / console / svcmgr spawns moved to init_launch.c — Phase 14 ──────── */

/* ── svcmgr lookup ──────────────────────────────────────────────────────── */

/* Phase 13/Track I: init_lookup / init_lookup_wait / init_lookup_name (the
 * legacy KChannel SVCMGR_MSG_LOOKUP[_NAME] discovery) are fully retired —
 * init discovers services via EP_LOOKUP_NAME over svcmgr.ep
 * (init_ep_lookup_name, init_bootstrap.c).  No legacy LOOKUP, no fallback. */


/* ── VFS endpoint client + S5/S6 boot-health checks ─────────────────────── */

/* Moved to init_bootstrap.c — Phase 14: init_ep_lookup_name[_slot] (svcmgr.ep
 * discovery, incl. the A1.6 reply receive-slot), init_vfs_ep_call, and the
 * S5/S6 LIST/STAT/READ_AT validation with their retry waits. */

/* Phase 13 (Track I): the KBD HELLO/SUBSCRIBE legacy-KChannel helpers and the
 * PS/2 scancode→ASCII echo table are retired — kbd is endpoint-only and sh
 * is the keystroke consumer (kbd.ep pull). */


/* ── Echo loop ──────────────────────────────────────────────────────────── */

/* Phase 13 (Track I): init's interactive echo loop is retired — sh is the
 * keystroke consumer (kbd.ep pull).  init's final state is a quiet idle loop so
 * the process never exits (which would tear it down). */
static void init_idle_loop(void) {
    init_log("[USER] init idle loop start\n");
    for (;;) {
        init_sys1(SYS_SLEEP, 100);
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void init_main(handle_id_t rbx_unused) {
    handle_id_t sm_h               = HANDLE_INVALID;
    handle_id_t vfs_ep_h           = HANDLE_INVALID;

    /* Step 4: the spawn/authority capability is invoked as the CSpace slot
     * userboot minted it into.  init_recv_spawn_cap used to materialise it
     * into a handle here; every consumer left (early serial's ioport create,
     * and the three svc_load_minted spawns) resolves a CPtr directly, so the
     * bridge had nothing left to bridge.  An absent slot 6 now fails loudly at
     * the first spawn instead of exiting before a single log line. */
    (void)rbx_unused;   /* svc_loader passes RBX = 0 — not a handle */
    init_early_serial_start();

    /* D-4: a page init owns, registered as its IPC buffer.  Retyped from the
     * boot untyped at slot 12 — the same one every endpoint and reply object
     * below comes out of.  Best-effort; a failure leaves the staging path. */
    init_ipc_buffer_init();

    /* Phase S1: confirm the delegated boot untyped (slot 12) BEFORE any spawn —
     * console/svcmgr endpoints and reply objects are retyped from it.
     *
     * Stage 4: SYS_UNTYPED_INFO answers by CPtr and materializes nothing, so
     * the check costs no handle-table entry and the pool stays a CPtr all the
     * way into retype2 — which is what gives the fabricated objects a real MDB
     * ancestor instead of LEGACY_ROOT status (see init_retype_slot). */
    {
        long ur = init_sys3(SYS_UNTYPED_INFO, (long)IRIS_CPTR_INIT_UNTYPED, 0, 0);
        if (ur >= 0) g_init_untyped_c = IRIS_CPTR_INIT_UNTYPED;
        else init_early_serial_write("[INIT] boot untyped absent\r\n");
    }
    {
    }

    /* Spawn fb first (fire-and-forget): it claims the framebuffer and exits. */
    init_spawn_fb();

    /* Spawn console: endpoint-only, CPtr-provisioned (Phase 13/Track I). */
    if (!init_spawn_console()) {
        init_early_serial_write("[INIT] console spawn FAILED\r\n");
        init_exit(1);
    }

    /* Verify the console endpoint with the first gated write (Phase 7.3): the
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

    sm_h = init_spawn_svcmgr();
    if (sm_h == HANDLE_INVALID) {
        init_log("[USER] svcmgr spawn FAILED\n");
        init_exit(1);
    }

    /* Step 4: the SYS_HANDLE_DUP that reserved a spawn cap for iris_test here
     * is gone.  It existed because iris_test spawns long AFTER the close below,
     * so a materialised copy had to be kept alive to serve as both the loader
     * authority and the source of iris_test's two spawn-cap mints.  The
     * spawn-cap SLOT answers both roles and never needed keeping alive:
     * SYS_INITRD_VMO / SYS_PROCESS_CREATE resolve it directly, and the mints
     * name it as their source, which also makes iris_test's caps MDB children
     * of that slot instead of copies handed over outright. */

    /* ── Service discovery ── */
    init_log(init_stage_lookup);
    /* Phase 13 (Track I): init no longer probes kbd over the legacy service/reply
     * KChannel — kbd is endpoint/notification-only.  kbd liveness is covered by
     * sh's "[SH] kbd cptr OK" (a kbd.ep PING) and by T034/T035/T044/T058. */

    /* Phase 7.2/13: the VFS endpoint is the mandatory operational path.  sm_h is
     * svcmgr's discovery endpoint ("svcmgr.ep", owned by init) — look up "vfs.ep"
     * through it via EP_LOOKUP_NAME (retrying until VFS has bootstrapped).
     * Fail-fast: no legacy fallback.
     * A1.6: the session cap lands in init's CSpace (INIT_RSLOT_VFS_EP) and
     * every init VFS EP_CALL below invokes it by CPtr — no handle is created.
     * A failed attempt (EP_CALL error or reply ERR) transfers no cap and
     * leaves the slot empty, so the retry re-declares it safely. */
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        vfs_ep_h = init_ep_lookup_name_slot(sm_h, VFS_EP_SVC_NAME,
                                            INIT_RSLOT_VFS_EP);
        if (vfs_ep_h != HANDLE_INVALID) break;
        init_retry_pause();
    }
    if (vfs_ep_h == HANDLE_INVALID) {
        init_log("[USER] vfs.ep lookup FAILED\n");
        init_exit(5);
    }

    /* Phase 13 (Track E/F): the legacy KChannel diagnostics + dynamic-registry
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

    /* Phase 13 (Track I): KBD SUBSCRIBE / shared-reply probes retired — kbd is
     * endpoint/notification-only and sh consumes keystrokes via "kbd.ep" (pull).
     * init no longer subscribes to a push channel. */
    init_log(init_stage_healthy);

    /* Phase 13 (Track F): the ring-3 timed-IPC KChannel selftest (CHAN_RECV_TIMEOUT
     * → TIMED_OUT) is retired — covered by iris_test T010 (NOTIFY_WAIT_TIMEOUT). */

    init_runtime_probe_invalid_userptr();
    init_runtime_probe_timeout_overflow();
    init_selftest_exception();

    /* ── Block 8: iris_test ring-3 syscall test suite ── */
    {
        /* Drain our queued console output first: iris_test writes raw to
         * COM1, and a half-flushed backlog line would otherwise interleave
         * mid-line with test output under load. */
        if (g_init_console_ep_h != HANDLE_INVALID)
            (void)console_ep_sync(g_init_console_ep_h);
        /* Step 4: unconditional.  The old guard was "did the DUP succeed?";
         * with the slot named directly there is nothing to fail early, and an
         * empty slot 6 now fails loudly at the mint instead of silently
         * skipping the whole suite. */
        init_spawn_iris_test(sm_h);
    }

    /* ── Idle loop (init never exits) ── */
    init_idle_loop();

    /* unreachable */
    init_exit(0);
}
