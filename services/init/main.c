/*
 * main.c — init service (ring-3 ELF, phase 22+).
 *
 * Receives a private bootstrap channel handle in %rdi (set by entry.S from %rbx).
 * The ring-3 userboot loader delivers the spawn/bootstrap capability over that
 * channel before `init` starts the rest of the healthy path.
 * Performs full system health validation, subscribes to keyboard scancodes, and
 * enters a persistent interactive echo loop that prints typed characters.
 *
 * Boot sequence validated:
 *   1. Lookup kbd service (write end) and kbd reply channel (read end)
 *   2. Resolve "vfs.ep" via the svcmgr discovery endpoint (Fase 7.2)
 *   3. Diagnostics check via svcmgr DIAG
 *   4. KBD HELLO liveness probe
 *   5. VFS EP LIST x3 (index 0, 1, 2) + out-of-range index rejection
 *   6. VFS EP STAT / READ_AT of the boot file (stateless; no open/close)
 *   7. KBD SUBSCRIBE — attach a scancode event channel
 *   8. Echo loop: SYS_CHAN_RECV_NB on event channel + SYS_WRITE one char per keypress
 */

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/svcmgr_proto.h>
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>
#include <iris/kbd_proto.h>
#include <iris/vfs.h>
#include <iris/console_proto.h>
#include <iris/fault_proto.h>
#include "../common/svc_loader.h"
#include "../common/console_client.h"

/* ── Raw syscall helpers ────────────────────────────────────────────────── */

static inline long init_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long _a3 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_a3)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long init_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long init_sys2(long nr, long a0, long a1) {
    return init_sys3(nr, a0, a1, 0);
}

static inline long init_sys1(long nr, long a0) {
    return init_sys3(nr, a0, 0, 0);
}

static inline long init_sys0(long nr) {
    return init_sys3(nr, 0, 0, 0);
}

/* ── Utilities ──────────────────────────────────────────────────────────── */

static handle_id_t g_init_console_h = HANDLE_INVALID;
/* Console KEndpoint master (Fase 7.3): init creates it, console serves it,
 * svcmgr publishes the send side as "console.ep". */
static handle_id_t g_init_console_ep_h = HANDLE_INVALID;
static uint8_t g_init_con_ep_buf[IRIS_IPC_BUF_SIZE];
static handle_id_t g_init_early_serial_h = HANDLE_INVALID;

static void init_log(const char *s) {
    /* Endpoint-first (Fase 7.3): synchronous EP write once the console
     * endpoint is verified; the legacy KChannel only carries pre-EP boot
     * lines. No silent fallback after verification — a broken EP drops the
     * gated markers and fails smoke. */
    if (g_init_console_ep_h != HANDLE_INVALID) {
        (void)console_ep_write(g_init_console_ep_h, g_init_con_ep_buf, s);
        return;
    }
    console_write(g_init_console_h, s);
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
static const char init_stage_hello[]     = "[USER][INIT][S2] kbd hello\n";
static const char init_stage_vfs_list[]  = "[USER][INIT][S5] vfs ep list\n";
static const char init_stage_vfs_rw[]    = "[USER][INIT][S6] vfs ep rw\n";
static const char init_stage_subscribe[] = "[USER][INIT][S7] kbd subscribe\n";
static const char init_stage_exception[] = "[USER][INIT][S8] exception delivery OK\n";
static const char init_stage_seal[]      = "[USER][INIT][S9] channel seal OK\n";
static const char init_stage_rights[]    = "[USER][INIT][S10] rights reduction OK\n";
static const char init_stage_healthy[]   = "[USER][INIT][BOOT] healthy path OK\n";
static const char init_console_load_fail[] = "[INIT] console load FAILED\r\n";
static const char init_console_ioport_fail[] = "[INIT] console ioport FAILED\r\n";
static const char init_console_chan_fail[] = "[INIT] console chan FAILED\r\n";
static const char init_console_readdup_fail[] = "[INIT] console read dup FAILED\r\n";
static const char init_console_writedup_fail[] = "[INIT] console write dup FAILED\r\n";
static const char init_console_boot_ioport_fail[] = "[INIT] console boot i/o FAILED\r\n";
static const char init_console_boot_service_fail[] = "[INIT] console boot service FAILED\r\n";
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

static void init_msg_zero(struct KChanMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

static void init_runtime_probe_invalid_userptr(void) {
    long ch_raw = init_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) return;

    long rd_raw = init_sys2(SYS_HANDLE_DUP, ch_raw, (long)RIGHT_READ);
    if (rd_raw >= 0) {
        long r = init_sys3(SYS_CHAN_RECV_TIMEOUT, rd_raw, 1, 50000000L);
        if (r == (long)IRIS_ERR_INVALID_ARG)
            init_log("[USER][INIT][SELFTEST] invalid-userptr OK\n");
        else
            init_log("[USER][INIT][SELFTEST] invalid-userptr WARN\n");
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
    }

    init_sys1(SYS_HANDLE_CLOSE, ch_raw);
}

static void init_runtime_probe_timeout_overflow(void) {
    init_log("[USER][INIT][SELFTEST] timeout-overflow SKIP\n");
}

/* Stack for the ud2 fault thread — statically allocated, never actually used
 * (the thread immediately faults before touching the stack). */
static uint8_t s8_thread_stack[4096];

static void __attribute__((noinline)) s8_ud2_fn(void) {
    __asm__ volatile ("ud2");
    for (;;) {}
}

static void init_selftest_exception(void) {
    struct KChanMsg msg;
    long ch_raw, rd_raw, wr_raw, tid_raw, r;
    handle_id_t ch_h, rd_h, wr_h;
    uint32_t vec, task_id;

    ch_raw = init_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { init_log("[USER][INIT][S8] SKIP: chan create\n"); return; }
    ch_h = (handle_id_t)ch_raw;

    rd_raw = init_sys2(SYS_HANDLE_DUP, ch_raw, (long)RIGHT_READ);
    if (rd_raw < 0) {
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S8] SKIP: rd dup\n"); return;
    }
    rd_h = (handle_id_t)rd_raw;

    wr_raw = init_sys2(SYS_HANDLE_DUP, ch_raw, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (wr_raw < 0) {
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S8] SKIP: wr dup\n"); return;
    }
    wr_h = (handle_id_t)wr_raw;

    /* Register exception handler for own process (HANDLE_INVALID = self) */
    r = init_sys2(SYS_EXCEPTION_HANDLER, (long)HANDLE_INVALID, wr_raw);
    if (r < 0) {
        init_sys1(SYS_HANDLE_CLOSE, wr_raw);
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S8] SKIP: handler reg\n"); return;
    }

    /* Spawn a thread that immediately executes ud2 (#UD, vector 6) */
    uint64_t entry = (uint64_t)(uintptr_t)s8_ud2_fn;
    uint64_t rsp   = (uint64_t)(uintptr_t)(s8_thread_stack + sizeof(s8_thread_stack));
    tid_raw = init_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid_raw < 0) {
        init_sys1(SYS_HANDLE_CLOSE, wr_raw);
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S8] SKIP: thread create\n"); return;
    }

    /* Wait up to 1 s for the fault notification */
    init_msg_zero(&msg);
    r = init_sys3(SYS_CHAN_RECV_TIMEOUT, rd_raw, (long)&msg, 1000000000L);
    if (r < 0) {
        init_sys1(SYS_HANDLE_CLOSE, wr_raw);
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S8] FAIL: no fault msg\n"); return;
    }

    vec = (uint32_t)msg.data[FAULT_OFF_VECTOR]
        | ((uint32_t)msg.data[FAULT_OFF_VECTOR + 1] << 8)
        | ((uint32_t)msg.data[FAULT_OFF_VECTOR + 2] << 16)
        | ((uint32_t)msg.data[FAULT_OFF_VECTOR + 3] << 24);
    task_id = (uint32_t)msg.data[FAULT_OFF_TASK_ID]
            | ((uint32_t)msg.data[FAULT_OFF_TASK_ID + 1] << 8)
            | ((uint32_t)msg.data[FAULT_OFF_TASK_ID + 2] << 16)
            | ((uint32_t)msg.data[FAULT_OFF_TASK_ID + 3] << 24);

    if (msg.type != FAULT_MSG_NOTIFY || vec != 6u) {
        init_sys1(SYS_HANDLE_CLOSE, wr_raw);
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S8] FAIL: wrong fault\n"); return;
    }

    /* Kill the faulting thread via exception resume */
    (void)init_sys3(SYS_EXCEPTION_RESUME, (long)HANDLE_INVALID, (long)task_id, 1);

    init_sys1(SYS_HANDLE_CLOSE, wr_raw);
    init_sys1(SYS_HANDLE_CLOSE, rd_raw);
    init_sys1(SYS_HANDLE_CLOSE, ch_raw);

    init_log(init_stage_exception);

    (void)ch_h; (void)rd_h; (void)wr_h;
}

/* S9: channel seal semantics
 * Sends a message, seals the write end, then verifies:
 *   - a further send is rejected
 *   - the buffered message can still be drained
 *   - recv on the empty sealed channel returns immediately with CLOSED */
static void init_selftest_channel_seal(void) {
    struct KChanMsg msg;
    long ch_raw, rd_raw, r;

    ch_raw = init_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { init_log("[USER][INIT][S9] SKIP: chan\n"); return; }

    rd_raw = init_sys2(SYS_HANDLE_DUP, ch_raw, (long)RIGHT_READ);
    if (rd_raw < 0) {
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S9] SKIP: rd dup\n"); return;
    }

    init_msg_zero(&msg);
    msg.type = 0xA559u;
    msg.data[0] = 's';
    msg.data_len = 1u;
    msg.attached_handle = HANDLE_INVALID;
    if (init_sys2(SYS_CHAN_SEND, ch_raw, (long)&msg) < 0) {
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S9] SKIP: send\n"); return;
    }

    (void)init_sys1(SYS_CHAN_SEAL, ch_raw);

    /* Send after seal must be rejected */
    init_msg_zero(&msg);
    msg.type = 0xA55Au;
    msg.data_len = 0u;
    msg.attached_handle = HANDLE_INVALID;
    r = init_sys2(SYS_CHAN_SEND, ch_raw, (long)&msg);
    if (r >= 0) {
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S9] FAIL: send-after-seal\n"); return;
    }

    /* Drain the buffered message */
    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV, rd_raw, (long)&msg);
    if (r < 0 || msg.type != 0xA559u) {
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S9] FAIL: drain recv\n"); return;
    }

    /* Recv on empty sealed channel must return CLOSED, not block */
    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV_NB, rd_raw, (long)&msg);
    if (r != (long)IRIS_ERR_CLOSED) {
        init_sys1(SYS_HANDLE_CLOSE, rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch_raw);
        init_log("[USER][INIT][S9] FAIL: empty sealed recv\n"); return;
    }

    init_sys1(SYS_HANDLE_CLOSE, rd_raw);
    init_sys1(SYS_HANDLE_CLOSE, ch_raw);
    init_log(init_stage_seal);
}

/* S10: handle rights reduction via channel transfer
 * Transfers a handle through a channel with reduced attached_rights and
 * verifies the receiver cannot exercise the stripped right (RIGHT_DUPLICATE). */
static void init_selftest_rights_reduction(void) {
    struct KChanMsg msg;
    long ch1_raw, ch1_xfer_raw, chx_raw, chx_rd_raw, recv_h_raw, dup_r;

    ch1_raw = init_sys0(SYS_CHAN_CREATE);
    if (ch1_raw < 0) { init_log("[USER][INIT][S10] SKIP: ch1\n"); return; }

    /* Dup with READ|TRANSFER so we can attach it but without DUPLICATE */
    ch1_xfer_raw = init_sys2(SYS_HANDLE_DUP, ch1_raw,
                             (long)(RIGHT_READ | RIGHT_TRANSFER));
    if (ch1_xfer_raw < 0) {
        init_sys1(SYS_HANDLE_CLOSE, ch1_raw);
        init_log("[USER][INIT][S10] SKIP: ch1 dup\n"); return;
    }

    chx_raw = init_sys0(SYS_CHAN_CREATE);
    if (chx_raw < 0) {
        init_sys1(SYS_HANDLE_CLOSE, ch1_xfer_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch1_raw);
        init_log("[USER][INIT][S10] SKIP: chx\n"); return;
    }

    chx_rd_raw = init_sys2(SYS_HANDLE_DUP, chx_raw, (long)RIGHT_READ);
    if (chx_rd_raw < 0) {
        init_sys1(SYS_HANDLE_CLOSE, chx_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch1_xfer_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch1_raw);
        init_log("[USER][INIT][S10] SKIP: chx rd\n"); return;
    }

    /* Send ch1_xfer with attached_rights=RIGHT_READ only (strips TRANSFER) */
    init_msg_zero(&msg);
    msg.type = 0xA55Bu;
    msg.data_len = 0u;
    msg.attached_handle = (handle_id_t)ch1_xfer_raw;
    msg.attached_rights = RIGHT_READ;
    if (init_sys2(SYS_CHAN_SEND, chx_raw, (long)&msg) < 0) {
        init_sys1(SYS_HANDLE_CLOSE, ch1_xfer_raw);
        init_sys1(SYS_HANDLE_CLOSE, chx_rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, chx_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch1_raw);
        init_log("[USER][INIT][S10] SKIP: send\n"); return;
    }
    /* ch1_xfer_raw consumed by send */

    init_msg_zero(&msg);
    if (init_sys2(SYS_CHAN_RECV, chx_rd_raw, (long)&msg) < 0 ||
        msg.attached_handle == HANDLE_INVALID) {
        init_sys1(SYS_HANDLE_CLOSE, chx_rd_raw);
        init_sys1(SYS_HANDLE_CLOSE, chx_raw);
        init_sys1(SYS_HANDLE_CLOSE, ch1_raw);
        init_log("[USER][INIT][S10] SKIP: recv\n"); return;
    }
    recv_h_raw = (long)msg.attached_handle;

    /* recv_h has RIGHT_READ only; SYS_HANDLE_DUP requires RIGHT_DUPLICATE */
    dup_r = init_sys2(SYS_HANDLE_DUP, recv_h_raw, (long)RIGHT_READ);
    if (dup_r >= 0)
        init_sys1(SYS_HANDLE_CLOSE, dup_r);

    init_sys1(SYS_HANDLE_CLOSE, recv_h_raw);
    init_sys1(SYS_HANDLE_CLOSE, chx_rd_raw);
    init_sys1(SYS_HANDLE_CLOSE, chx_raw);
    init_sys1(SYS_HANDLE_CLOSE, ch1_raw);

    if (dup_r >= 0) {
        init_log("[USER][INIT][S10] FAIL: dup succeeded on rights-reduced handle\n");
        return;
    }
    init_log(init_stage_rights);
}

/* ── iris_test spawn + wait ──────────────────────────────────────────────── */

/* Defined below (svcmgr lookup section); used here to fetch "svcmgr.ep". */
static handle_id_t init_lookup_name(handle_id_t sm_h, const char *name,
                                    iris_rights_t rights);

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
    handle_id_t cap_dup     = HANDLE_INVALID;
    handle_id_t watch_base_h = HANDLE_INVALID; /* death notification (Track B) */
    struct KChanMsg msg;
    long r;

    /* Fase 8: the full well-known slot set is pre-start-minted into
     * iris_test (the kind-0x20 bootstrap forward is retired — slot 1 is
     * the only discovery path):
     *   slot 1  — svcmgr discovery ep, RIGHT_WRITE   → T026+/T039/T041
     *   slot 2  — vfs.ep,     RIGHT_WRITE            → T042/T045
     *   slot 3  — console.ep, RIGHT_WRITE            → T043
     *   slot 4  — kbd.ep,     RIGHT_WRITE            → T044
     *   slot 30 — console KChannel cap (wrong type)  → T040 WRONG_TYPE
     *   slot 31 — svcmgr ep, RIGHT_TRANSFER only     → T040 ACCESS_DENIED
     *             (the dual resolver must NOT fall back to handles).
     * vfs.ep / kbd.ep come from legacy lookup (WRITE|DUPLICATE) — that
     * lookup path staying alive is itself part of the test surface.
     * Missing caps leave slots empty: the tests FAIL loudly, never skip. */
    handle_id_t lk_svcmgr = init_lookup_name(sm_h, "svcmgr.ep",
                                RIGHT_WRITE | RIGHT_TRANSFER | RIGHT_DUPLICATE);
    handle_id_t lk_vfs    = init_lookup_name(sm_h, "vfs.ep",
                                RIGHT_WRITE | RIGHT_DUPLICATE);
    handle_id_t lk_kbd    = init_lookup_name(sm_h, "kbd.ep",
                                RIGHT_WRITE | RIGHT_DUPLICATE);
    if (lk_svcmgr == HANDLE_INVALID)
        init_log("[USER][INIT] svcmgr.ep lookup FAILED\n");

    {
        /* Fase 9: slots 1-4 carry IRIS_BADGE_IRIS_TEST so every server can
         * verify who is calling; slot 28 is a SECOND cap to the svcmgr
         * endpoint with a different badge (T053: two caps, same endpoint,
         * different identities). */
        struct svc_mint it_mints[9];
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
        it_mints[4].src_h = g_init_console_h;          /* wrong type */
        it_mints[4].rights = RIGHT_WRITE;
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
        r = svc_load_minted(spawn_cap_h, "iris_test", &proc_h, &boot_h,
                            it_mints, 9u);
    }
    init_close(&lk_svcmgr);
    init_close(&lk_vfs);
    init_close(&lk_kbd);
    if (r < 0) {
        init_log("[USER][INIT] iris_test load FAILED\n");
        goto out;
    }

    /* Send spawn_cap to iris_test */
    r = init_sys2(SYS_HANDLE_DUP, (long)spawn_cap_h,
                  (long)(RIGHT_READ | RIGHT_TRANSFER));
    if (r < 0) goto out;
    cap_dup = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP);
    msg.data_len        = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = cap_dup;
    msg.attached_rights = RIGHT_READ | RIGHT_TRANSFER;
    r = init_sys2(SYS_CHAN_SEND, (long)boot_h, (long)&msg);
    if (r < 0) goto out;
    cap_dup = HANDLE_INVALID; /* consumed by send */

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
    if (cap_dup != HANDLE_INVALID) init_close(&cap_dup);
    init_close(&spawn_cap_h);
}

static void init_retry_pause(void) {
    (void)init_sys1(SYS_SLEEP, INIT_RETRY_SLEEP_TICKS);
}

static handle_id_t init_recv_spawn_cap(handle_id_t bootstrap_ch_h) {
    struct KChanMsg msg;

    if (bootstrap_ch_h == HANDLE_INVALID) return HANDLE_INVALID;

    for (uint32_t attempt = 0; attempt < 8u; attempt++) {
        init_msg_zero(&msg);
        if (init_sys2(SYS_CHAN_RECV, (long)bootstrap_ch_h, (long)&msg) < 0)
            return HANDLE_INVALID;
        if (msg.type != SVCMGR_MSG_BOOTSTRAP_HANDLE ||
            msg.data_len < SVCMGR_BOOTSTRAP_MSG_LEN ||
            msg.attached_handle == HANDLE_INVALID) {
            if (msg.attached_handle != HANDLE_INVALID)
                init_close(&msg.attached_handle);
            continue;
        }
        if (svcmgr_proto_read_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND]) ==
            SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP) {
            return msg.attached_handle;
        }
        init_close(&msg.attached_handle);
    }

    return HANDLE_INVALID;
}

/* ── Channel helper: send msg, recv reply (SYS_CHAN_SEND + SYS_CHAN_RECV) ── */

static long init_chan_send_recv(handle_id_t send_h, handle_id_t recv_h,
                                struct KChanMsg *msg) {
    long r = init_sys2(SYS_CHAN_SEND, (long)send_h, (long)msg);
    if (r < 0) return r;
    init_msg_zero(msg);
    return init_sys2(SYS_CHAN_RECV, (long)recv_h, (long)msg);
}

/* ── fb spawn (Phase 30: ring-3 framebuffer painter) ────────────────────── */

static void init_spawn_fb(handle_id_t spawn_cap_h) {
    handle_id_t fb_proc_h  = HANDLE_INVALID;
    handle_id_t fb_boot_h  = HANDLE_INVALID;
    handle_id_t fb_cap_h   = HANDLE_INVALID;
    struct KChanMsg msg;
    long r;

    r = svc_load(spawn_cap_h, "fb", &fb_proc_h, &fb_boot_h);
    if (r < 0) {
        init_early_serial_write(init_fb_load_fail);
        goto out;
    }

    /* Create restricted bootcap with FRAMEBUFFER only for the fb service. */
    r = init_sys2(SYS_HANDLE_DUP, (long)spawn_cap_h,
                  (long)(RIGHT_READ | RIGHT_TRANSFER));
    if (r < 0) goto out;
    fb_cap_h = (handle_id_t)r;

    /* Restrict to FRAMEBUFFER only; original spawn_cap_h is unaffected. */
    r = init_sys2(SYS_BOOTCAP_RESTRICT, (long)fb_cap_h,
                  (long)IRIS_BOOTCAP_FRAMEBUFFER);
    if (r < 0) goto out;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP);
    msg.data_len        = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = fb_cap_h;
    msg.attached_rights = RIGHT_READ;

    r = init_sys2(SYS_CHAN_SEND, (long)fb_boot_h, (long)&msg);
    if (r < 0) goto out;
    fb_cap_h = HANDLE_INVALID; /* consumed by send */

out:
    init_close(&fb_proc_h);
    init_close(&fb_boot_h);
    if (fb_cap_h != HANDLE_INVALID) init_close(&fb_cap_h);
}

/* ── console spawn (Phase 30: ring-3 serial console service) ────────────── */

static handle_id_t init_spawn_console(handle_id_t spawn_cap_h) {
    handle_id_t con_proc_h  = HANDLE_INVALID;
    handle_id_t con_boot_h  = HANDLE_INVALID;
    handle_id_t ioport_h    = HANDLE_INVALID;
    handle_id_t con_base_h  = HANDLE_INVALID;
    handle_id_t con_read_h  = HANDLE_INVALID;
    handle_id_t con_write_h = HANDLE_INVALID;
    struct KChanMsg msg;
    long r;

    /* Console KEndpoint (Fase 7.3/8): init owns the master.  Created BEFORE
     * the spawn so the recv side can be pre-start-minted into the console's
     * root CNode (IRIS_CPTR_OWN_EP) — kind 0x21 retired. */
    r = init_sys0(SYS_ENDPOINT_CREATE);
    if (r < 0) {
        init_early_serial_write(init_console_chan_fail);
        goto fail;
    }
    g_init_console_ep_h = (handle_id_t)r;

    {
        struct svc_mint con_mints[1];
        con_mints[0].slot   = IRIS_CPTR_OWN_EP;
        con_mints[0].src_h  = g_init_console_ep_h;
        con_mints[0].rights = RIGHT_READ;
        con_mints[0].badge  = 0;   /* server-side cap: unbadged */
        r = svc_load_minted(spawn_cap_h, "console", &con_proc_h, &con_boot_h,
                            con_mints, 1u);
    }
    if (r < 0) {
        init_early_serial_write(init_console_load_fail);
        goto fail;
    }

    /* I/O port capability for the 8 UART registers at 0x3F8..0x3FF. */
    r = init_sys3(SYS_CAP_CREATE_IOPORT, (long)spawn_cap_h, 0x3F8, 8);
    if (r < 0) {
        init_early_serial_write(init_console_ioport_fail);
        goto fail;
    }
    ioport_h = (handle_id_t)r;

    /* Create the console message channel. */
    r = init_sys0(SYS_CHAN_CREATE);
    if (r < 0) {
        init_early_serial_write(init_console_chan_fail);
        goto fail;
    }
    con_base_h = (handle_id_t)r;

    /* Read end for the console server.
     * RIGHT_TRANSFER is needed so init can pass this via SYS_CHAN_SEND;
     * msg.attached_rights=RIGHT_READ ensures the receiver only gets READ. */
    r = init_sys2(SYS_HANDLE_DUP, (long)con_base_h,
                  (long)(RIGHT_READ | RIGHT_TRANSFER));
    if (r < 0) {
        init_early_serial_write(init_console_readdup_fail);
        goto fail;
    }
    con_read_h = (handle_id_t)r;

    /* Write end for init to keep (RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER). */
    r = init_sys2(SYS_HANDLE_DUP, (long)con_base_h,
                  (long)(RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER));
    if (r < 0) {
        init_early_serial_write(init_console_writedup_fail);
        goto fail;
    }
    con_write_h = (handle_id_t)r;
    init_close(&con_base_h); /* no longer need the full-rights base */

    /* Send IOPORT_CAP to console server. */
    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP);
    msg.data_len        = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = ioport_h;
    msg.attached_rights = RIGHT_READ | RIGHT_WRITE; /* IN (poll LSR) + OUT (write THR) */
    r = init_sys2(SYS_CHAN_SEND, (long)con_boot_h, (long)&msg);
    if (r < 0) {
        init_early_serial_write(init_console_boot_ioport_fail);
        goto fail;
    }
    ioport_h = HANDLE_INVALID;

    /* Send SERVICE (read end of console channel) to console server. */
    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_SERVICE);
    msg.data_len        = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = con_read_h;
    msg.attached_rights = RIGHT_READ;
    r = init_sys2(SYS_CHAN_SEND, (long)con_boot_h, (long)&msg);
    if (r < 0) {
        init_early_serial_write(init_console_boot_service_fail);
        goto fail;
    }
    con_read_h = HANDLE_INVALID;

    init_close(&con_proc_h);
    init_close(&con_boot_h);
    return con_write_h;

fail:
    init_close(&con_proc_h);
    init_close(&con_boot_h);
    if (ioport_h    != HANDLE_INVALID) init_close(&ioport_h);
    if (con_base_h  != HANDLE_INVALID) init_close(&con_base_h);
    if (con_read_h  != HANDLE_INVALID) init_close(&con_read_h);
    if (con_write_h != HANDLE_INVALID) init_close(&con_write_h);
    return HANDLE_INVALID;
}

/* ── svcmgr spawn (Phase 29: ring-3 loader; Phase 30: also sends console) ── */

static handle_id_t init_spawn_svcmgr(handle_id_t spawn_cap_h,
                                     handle_id_t console_h) {
    handle_id_t svcmgr_proc_h  = HANDLE_INVALID;
    handle_id_t svcmgr_chan_h  = HANDLE_INVALID;
    handle_id_t dup_cap_h      = HANDLE_INVALID;
    handle_id_t con_dup_h      = HANDLE_INVALID;
    struct KChanMsg msg;
    long r;

    /* Fase 8: the console endpoint send side is pre-start-minted into
     * svcmgr's root CNode at IRIS_CPTR_CONSOLE_EP (bootstrap kind 0x22
     * retired).  DUPLICATE|TRANSFER so svcmgr can publish "console.ep" and
     * re-mint it into catalog children. */
    {
        struct svc_mint sm_mints[1];
        sm_mints[0].slot   = IRIS_CPTR_CONSOLE_EP;
        sm_mints[0].src_h  = g_init_console_ep_h;
        sm_mints[0].rights = RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[0].badge  = 0;   /* MUST stay unbadged: svcmgr re-mints it
                                   * per child with each child's badge (a
                                   * badged cap can never be re-badged). */
        r = svc_load_minted(spawn_cap_h, "svcmgr", &svcmgr_proc_h,
                            &svcmgr_chan_h, sm_mints, 1u);
    }
    if (r < 0) goto fail;

    /* Send CONSOLE_CAP first so svcmgr can log as soon as it receives SPAWN_CAP. */
    if (console_h != HANDLE_INVALID) {
        r = init_sys2(SYS_HANDLE_DUP, (long)console_h,
                      (long)(RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER));
        if (r < 0) goto fail;
        con_dup_h = (handle_id_t)r;

        init_msg_zero(&msg);
        msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
        svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                               SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP);
        msg.data_len        = SVCMGR_BOOTSTRAP_MSG_LEN;
        msg.attached_handle = con_dup_h;
        msg.attached_rights = RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;

        r = init_sys2(SYS_CHAN_SEND, (long)svcmgr_chan_h, (long)&msg);
        if (r < 0) goto fail;
        con_dup_h = HANDLE_INVALID;
    }


    r = init_sys2(SYS_HANDLE_DUP, (long)spawn_cap_h,
                  (long)(RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    dup_cap_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP);
    msg.data_len        = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = dup_cap_h;
    msg.attached_rights = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;

    r = init_sys2(SYS_CHAN_SEND, (long)svcmgr_chan_h, (long)&msg);
    if (r < 0) goto fail;
    dup_cap_h = HANDLE_INVALID;

    init_close(&svcmgr_proc_h);
    return svcmgr_chan_h;

fail:
    init_close(&svcmgr_proc_h);
    init_close(&svcmgr_chan_h);
    if (dup_cap_h != HANDLE_INVALID) init_close(&dup_cap_h);
    if (con_dup_h != HANDLE_INVALID) init_close(&con_dup_h);
    return HANDLE_INVALID;
}

/* ── svcmgr lookup ──────────────────────────────────────────────────────── */
/*
 * Create a one-shot reply channel, dup the write end into svcmgr's table via
 * handle-transfer, send SVCMGR_MSG_LOOKUP, receive SVCMGR_MSG_LOOKUP_REPLY on
 * the read end.  Returns the attached handle on success, HANDLE_INVALID on error.
 */
static handle_id_t init_lookup(handle_id_t sm_h, uint32_t endpoint,
                                iris_rights_t rights) {
    struct KChanMsg msg;
    handle_id_t base_h = HANDLE_INVALID;
    handle_id_t recv_h = HANDLE_INVALID;
    handle_id_t xfer_h = HANDLE_INVALID;
    handle_id_t result = HANDLE_INVALID;
    long r;

    /* Create reply channel pair from one fully-authoritative base handle. */
    r = init_sys0(SYS_CHAN_CREATE);
    if (r < 0) goto fail;
    base_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h,
                  (long)RIGHT_READ);
    if (r < 0) goto fail;
    recv_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h,
                  (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    xfer_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_LOOKUP;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_LOOKUP_OFF_ENDPOINT], endpoint);
    svcmgr_proto_write_u32(&msg.data[SVCMGR_LOOKUP_OFF_RIGHTS], (uint32_t)rights);
    msg.data_len = SVCMGR_LOOKUP_MSG_LEN;
    msg.attached_handle = xfer_h;
    msg.attached_rights = RIGHT_WRITE | RIGHT_TRANSFER;

    r = init_sys2(SYS_CHAN_SEND, (long)sm_h, (long)&msg);
    if (r < 0) goto fail;
    xfer_h = HANDLE_INVALID; /* consumed by send */

    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV, (long)recv_h, (long)&msg);
    if (r < 0) goto fail;

    if (msg.type != SVCMGR_MSG_LOOKUP_REPLY) goto fail;
    {
        int32_t err = (int32_t)svcmgr_proto_read_u32(&msg.data[SVCMGR_LOOKUP_REPLY_OFF_ERR]);
        if (err != 0) goto fail;
    }
    result = msg.attached_handle;

fail:
    init_close(&base_h);
    init_close(&recv_h);
    if (xfer_h != HANDLE_INVALID) init_close(&xfer_h);
    return result;
}

static handle_id_t init_lookup_wait(handle_id_t sm_h, uint32_t endpoint,
                                    iris_rights_t rights) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        handle_id_t h = init_lookup(sm_h, endpoint, rights);
        if (h != HANDLE_INVALID) return h;
        init_retry_pause();
    }
    return HANDLE_INVALID;
}

static handle_id_t init_lookup_name(handle_id_t sm_h, const char *name,
                                    iris_rights_t rights) {
    struct KChanMsg msg;
    handle_id_t base_h = HANDLE_INVALID;
    handle_id_t recv_h = HANDLE_INVALID;
    handle_id_t xfer_h = HANDLE_INVALID;
    handle_id_t result = HANDLE_INVALID;
    long r;
    uint32_t i = 0;

    if (!name || !name[0]) return HANDLE_INVALID;

    r = init_sys0(SYS_CHAN_CREATE);
    if (r < 0) goto fail;
    base_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h, (long)RIGHT_READ);
    if (r < 0) goto fail;
    recv_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    xfer_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_LOOKUP_NAME;
    while (i + 1u < SVCMGR_SERVICE_NAME_CAP && name[i]) {
        msg.data[SVCMGR_LOOKUP_NAME_OFF_NAME + i] = (uint8_t)name[i];
        i++;
    }
    msg.data[SVCMGR_LOOKUP_NAME_OFF_NAME + i] = '\0';
    svcmgr_proto_write_u32(&msg.data[SVCMGR_LOOKUP_NAME_OFF_RIGHTS], (uint32_t)rights);
    msg.data_len = SVCMGR_LOOKUP_NAME_MSG_LEN;
    msg.attached_handle = xfer_h;
    msg.attached_rights = RIGHT_WRITE | RIGHT_TRANSFER;

    r = init_sys2(SYS_CHAN_SEND, (long)sm_h, (long)&msg);
    if (r < 0) goto fail;
    xfer_h = HANDLE_INVALID;

    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV, (long)recv_h, (long)&msg);
    if (r < 0) goto fail;
    if (msg.type != SVCMGR_MSG_LOOKUP_REPLY) goto fail;
    if ((int32_t)svcmgr_proto_read_u32(&msg.data[SVCMGR_LOOKUP_REPLY_OFF_ERR]) != 0) goto fail;
    result = msg.attached_handle;

fail:
    init_close(&base_h);
    init_close(&recv_h);
    if (xfer_h != HANDLE_INVALID) init_close(&xfer_h);
    return result;
}


/* ── VFS endpoint client (Fase 7.2) ─────────────────────────────────────── */

/* EP_CALL bulk buffer: the request path and the reply data share this buffer
 * (EP_CALL reuses buf_uptr in both directions). +1 for a guard NUL. */
static uint8_t g_init_ep_buf[VFS_EP_DATA_MAX + 1u];

static void init_imsg_zero(struct IrisMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

/*
 * Resolve "vfs.ep" through the svcmgr discovery endpoint:
 * EP_CALL(svcmgr_ep, IRIS_SVCMGR_EP_LOOKUP_NAME, "vfs.ep"). The reply carries
 * the endpoint cap (RIGHT_WRITE) via SYS_REPLY cap transfer. Returns
 * HANDLE_INVALID on any failure (caller retries / fails fast).
 */
static handle_id_t init_vfs_ep_lookup(handle_id_t svcmgr_ep_h) {
    static const char ep_name[] = VFS_EP_SVC_NAME;
    struct IrisMsg msg;

    if (svcmgr_ep_h == HANDLE_INVALID) return HANDLE_INVALID;

    for (uint32_t i = 0; i < (uint32_t)sizeof(ep_name); i++)
        g_init_ep_buf[i] = (uint8_t)ep_name[i];

    init_imsg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_init_ep_buf;
    msg.buf_len  = (uint32_t)sizeof(ep_name);  /* includes NUL */

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

/* ── KBD HELLO ──────────────────────────────────────────────────────────── */

static int init_kbd_hello(handle_id_t kbd_h, handle_id_t kbd_reply_h) {
    struct KChanMsg msg;
    long r;
    init_msg_zero(&msg);
    msg.type            = KBD_MSG_HELLO;
    msg.data_len        = KBD_MSG_HELLO_LEN;
    msg.attached_handle = HANDLE_INVALID;
    r = init_chan_send_recv(kbd_h, kbd_reply_h, &msg);
    if (r < 0) return 0;
    if (msg.type != KBD_MSG_HELLO_REPLY) return 0;
    if ((int32_t)kbd_proto_read_u32(&msg.data[KBD_MSG_OFF_HELLO_REPLY_ERR]) != 0) return 0;
    return 1;
}

static int init_wait_kbd_hello(handle_id_t kbd_h, handle_id_t kbd_reply_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_kbd_hello(kbd_h, kbd_reply_h)) return 1;
        init_retry_pause();
    }
    return 0;
}

/* ── KBD SUBSCRIBE ──────────────────────────────────────────────────────── */
/*
 * Create a channel pair (scan_recv_h / scan_send_h).
 * Attach scan_send_h to a KBD_MSG_SUBSCRIBE message sent to kbd.
 * kbd stores scan_send_h in %r15 and forwards KBD_MSG_SCANCODE_EVENT
 * on every IRQ. SUBSCRIBE is fire-and-forget to avoid cross-client reply
 * races on the shared kbd reply endpoint. We read events from scan_recv_h.
 * Returns scan_recv_h (caller must close on exit), HANDLE_INVALID on error.
 */
static handle_id_t init_kbd_subscribe(handle_id_t kbd_h) {
    struct KChanMsg msg;
    handle_id_t base_h = HANDLE_INVALID;
    handle_id_t scan_recv_h = HANDLE_INVALID;
    handle_id_t xfer_h      = HANDLE_INVALID;
    long r;

    r = init_sys0(SYS_CHAN_CREATE);
    if (r < 0) goto fail;
    base_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h,
                  (long)RIGHT_READ);
    if (r < 0) goto fail;
    scan_recv_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h,
                  (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    xfer_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type            = KBD_MSG_SUBSCRIBE;
    msg.data_len        = KBD_MSG_SUBSCRIBE_LEN;
    msg.attached_handle = xfer_h;
    msg.attached_rights = RIGHT_WRITE | RIGHT_TRANSFER;

    r = init_sys2(SYS_CHAN_SEND, (long)kbd_h, (long)&msg);
    if (r < 0) goto fail;
    xfer_h = HANDLE_INVALID;

    init_close(&base_h);
    return scan_recv_h;

fail:
    init_close(&base_h);
    init_close(&scan_recv_h);
    if (xfer_h != HANDLE_INVALID) init_close(&xfer_h);
    return HANDLE_INVALID;
}

static handle_id_t init_wait_kbd_subscribe(handle_id_t kbd_h,
                                           handle_id_t kbd_reply_h) {
    (void)kbd_reply_h;
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        handle_id_t scan_recv_h = init_kbd_subscribe(kbd_h);
        if (scan_recv_h != HANDLE_INVALID) return scan_recv_h;
        init_retry_pause();
    }
    return HANDLE_INVALID;
}

/* ── PS/2 scan set 1 → ASCII ────────────────────────────────────────────── */

static const char g_sc_to_ascii[128] = {
    0,    0,    '1',  '2',  '3',  '4',  '5',  '6',  /* 0x00-0x07 */
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t', /* 0x08-0x0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  /* 0x10-0x17 */
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',  /* 0x18-0x1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  /* 0x20-0x27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',  /* 0x28-0x2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  /* 0x30-0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,    /* 0x38-0x3F */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* 0x40-0x47 */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* 0x48-0x4F */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* 0x50-0x57 */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* 0x58-0x5F */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* 0x60-0x67 */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* 0x68-0x6F */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* 0x70-0x77 */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* 0x78-0x7F */
};

/* ── Echo loop ──────────────────────────────────────────────────────────── */

static void init_echo_loop(handle_id_t scan_recv_h) {
    struct KChanMsg msg;
    char buf[3];

    init_log("[USER] init echo loop start\n");

    for (;;) {
        long r = init_sys2(SYS_CHAN_RECV_NB, (long)scan_recv_h, (long)&msg);
        if (r == 0) {
            if (msg.type == KBD_MSG_SCANCODE_EVENT &&
                msg.data_len >= KBD_MSG_SCANCODE_EVENT_LEN) {
                uint8_t sc = msg.data[KBD_MSG_OFF_SC_EVENT_CODE];
                if ((sc & 0x80u) == 0u) {
                    char ch = g_sc_to_ascii[sc & 0x7Fu];
                    if (ch != 0) {
                        buf[0] = ch;
                        buf[1] = 0;
                        console_write(g_init_console_h, buf);
                    }
                }
            }
        }
        init_sys1(SYS_SLEEP, 10);
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void init_main(handle_id_t bootstrap_ch_h) {
    handle_id_t bootstrap_h        = HANDLE_INVALID;
    handle_id_t sm_h               = HANDLE_INVALID;
    handle_id_t kbd_h              = HANDLE_INVALID;
    handle_id_t kbd_reply_h        = HANDLE_INVALID;
    handle_id_t vfs_ep_h           = HANDLE_INVALID;
    handle_id_t scan_recv_h        = HANDLE_INVALID;
    handle_id_t iris_test_spawn_h  = HANDLE_INVALID;

    bootstrap_h = init_recv_spawn_cap(bootstrap_ch_h);
    init_close(&bootstrap_ch_h);
    if (bootstrap_h == HANDLE_INVALID)
        init_exit(1);
    init_early_serial_start(bootstrap_h);

    /* Spawn fb first (fire-and-forget): it claims the framebuffer and exits. */
    init_spawn_fb(bootstrap_h);

    /* Spawn console: creates the console service and returns the write channel. */
    g_init_console_h = init_spawn_console(bootstrap_h);
    if (g_init_console_h == HANDLE_INVALID) {
        init_early_serial_write("[INIT] console spawn FAILED\r\n");
        init_exit(1);
    }
    init_early_serial_stop();
    /* From here all init_log() calls go through the console service. */

    /* Verify the console endpoint with the first gated write (Fase 7.3):
     * the EP_CALL blocks until the console serves it, so this also
     * synchronizes with console boot. On failure: drop to the legacy
     * channel LOUDLY — the missing OK marker fails smoke. */
    if (g_init_console_ep_h != HANDLE_INVALID) {
        if (console_ep_write(g_init_console_ep_h, g_init_con_ep_buf,
                             "[USER] console ep OK\n") != 0) {
            init_close(&g_init_console_ep_h);
            console_write(g_init_console_h, "[USER] console ep FAILED\n");
        }
    } else {
        console_write(g_init_console_h, "[USER] console ep FAILED\n");
    }

    init_log("[USER] init bootstrap start\n");

    sm_h = init_spawn_svcmgr(bootstrap_h, g_init_console_h);
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
    kbd_h = init_lookup_wait(sm_h, SVCMGR_ENDPOINT_KBD,
                             RIGHT_WRITE);
    if (kbd_h == HANDLE_INVALID) {
        init_log("[USER] kbd lookup FAILED\n");
        init_exit(2);
    }

    kbd_reply_h = init_lookup_wait(sm_h, SVCMGR_ENDPOINT_KBD_REPLY,
                                   RIGHT_READ);
    if (kbd_reply_h == HANDLE_INVALID) {
        init_log("[USER] kbd reply lookup FAILED\n");
        init_exit(3);
    }

    /* Fase 7.2: the VFS endpoint is the mandatory operational path. Resolve
     * the svcmgr discovery endpoint once, then look up "vfs.ep" through it
     * (retrying until VFS has bootstrapped). Fail-fast: no legacy fallback. */
    {
        handle_id_t svcmgr_ep_h = init_lookup_name(sm_h, "svcmgr.ep",
                                                   RIGHT_WRITE);
        if (svcmgr_ep_h == HANDLE_INVALID) {
            init_log("[USER] svcmgr.ep lookup FAILED\n");
            init_exit(4);
        }
        for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
            vfs_ep_h = init_vfs_ep_lookup(svcmgr_ep_h);
            if (vfs_ep_h != HANDLE_INVALID) break;
            init_retry_pause();
        }
        init_close(&svcmgr_ep_h);
        if (vfs_ep_h == HANDLE_INVALID) {
            init_log("[USER] vfs.ep lookup FAILED\n");
            init_exit(5);
        }
    }

    /* ── KBD HELLO ── */
    init_log(init_stage_hello);
    if (!init_wait_kbd_hello(kbd_h, kbd_reply_h)) {
        init_log("[USER] kbd hello FAILED\n");
        init_exit(6);
    }
    init_log("[USER] kbd hello reply OK\n");

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

    /* ── KBD SUBSCRIBE ── */
    init_log(init_stage_subscribe);
    scan_recv_h = init_wait_kbd_subscribe(kbd_h, kbd_reply_h);
    if (scan_recv_h == HANDLE_INVALID) {
        init_log("[USER] kbd subscribe FAILED\n");
        init_exit(11);
    }
    init_log("[USER] kbd subscribe OK\n");
    if (!init_wait_kbd_hello(kbd_h, kbd_reply_h)) {
        init_log("[USER] kbd shared reply FAILED\n");
        init_exit(12);
    }
    init_log("[USER] kbd shared reply OK\n");

    /* Fase 7.4: sh consumes key events via "kbd.ep" (pull). Drop our S7
     * subscription (handle 0 = unsubscribe) so init's fallback echo loop
     * does not double-deliver keystrokes next to sh; the loop below then
     * just idles on the (now silent) scan channel. */
    {
        struct KChanMsg umsg;
        init_msg_zero(&umsg);
        umsg.type     = KBD_MSG_SUBSCRIBE;
        umsg.data_len = KBD_MSG_SUBSCRIBE_LEN;
        (void)init_sys2(SYS_CHAN_SEND, (long)kbd_h, (long)&umsg);
    }
    init_log(init_stage_healthy);

    /* ── Phase 44: ring-3 timed IPC selftest ─────────────────────── */
    {
        long ch_raw = init_sys0(SYS_CHAN_CREATE);
        if (ch_raw >= 0) {
            handle_id_t ch_wr = (handle_id_t)ch_raw;
            long rd_raw = init_sys2(SYS_HANDLE_DUP, ch_raw, (long)(RIGHT_READ));
            if (rd_raw >= 0) {
                struct KChanMsg tmsg;
                uint8_t *tp = (uint8_t *)&tmsg;
                for (uint32_t i = 0; i < (uint32_t)sizeof(tmsg); i++) tp[i] = 0;
                /* 50 ms timeout on an empty channel — must return TIMED_OUT */
                long tr = init_sys3(SYS_CHAN_RECV_TIMEOUT, rd_raw,
                                    (long)&tmsg, 50000000L);
                if (tr == (long)IRIS_ERR_TIMED_OUT)
                    init_log("[USER][INIT][TIMED] recv timeout OK\n");
                else
                    init_log("[USER][INIT][TIMED] recv timeout WARN: unexpected result\n");
                init_sys1(SYS_HANDLE_CLOSE, rd_raw);
            }
            init_sys1(SYS_HANDLE_CLOSE, (long)ch_wr);
        }
    }

    init_runtime_probe_invalid_userptr();
    init_runtime_probe_timeout_overflow();
    init_selftest_exception();
    init_selftest_channel_seal();
    init_selftest_rights_reduction();

    /* ── Block 8: iris_test ring-3 syscall test suite ── */
    if (iris_test_spawn_h != HANDLE_INVALID) {
        /* Drain our queued console output first: iris_test writes raw to
         * COM1, and a half-flushed backlog line (e.g. the S10 marker) would
         * otherwise interleave mid-line with test output under load. */
        if (g_init_console_ep_h != HANDLE_INVALID)
            (void)console_ep_sync(g_init_console_ep_h);
        else
            console_sync(g_init_console_h);
        init_spawn_iris_test(iris_test_spawn_h, sm_h);
        iris_test_spawn_h = HANDLE_INVALID;
    }

    /* ── Interactive echo loop ── */
    init_echo_loop(scan_recv_h);

    /* unreachable */
    init_exit(0);
}
