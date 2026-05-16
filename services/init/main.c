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
 *   2. Lookup vfs service (write end) and vfs reply channel (read end)
 *   3. Diagnostics check via svcmgr DIAG
 *   4. KBD HELLO liveness probe
 *   5. VFS LIST x3 (index 0, 1, 2-OOB)
 *   6. VFS OPEN / READ / CLOSE of the boot file
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
#include <iris/vfs_proto.h>
#include <iris/kbd_proto.h>
#include <iris/vfs.h>
#include <iris/console_proto.h>
#include "../common/svc_loader.h"
#include "../common/console_client.h"

/* ── Raw syscall helpers ────────────────────────────────────────────────── */

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
static handle_id_t g_init_early_serial_h = HANDLE_INVALID;

static void init_log(const char *s) {
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
static const char init_stage_diag[]      = "[USER][INIT][S3] global diag\n";
static const char init_stage_dynamic[]   = "[USER][INIT][S4] dynamic registry\n";
static const char init_stage_vfs_list[]  = "[USER][INIT][S5] vfs list\n";
static const char init_stage_vfs_rw[]    = "[USER][INIT][S6] vfs rw\n";
static const char init_stage_subscribe[] = "[USER][INIT][S7] kbd subscribe\n";
static const char init_stage_healthy[]   = "[USER][INIT][BOOT] healthy path OK\n";
static const char init_diag_req[]        = "[USER][INIT][DIAG] request\n";
static const char init_diag_sent[]       = "[USER][INIT][DIAG] sent\n";
static const char init_diag_reply[]      = "[USER][INIT][DIAG] reply\n";
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

    r = svc_load(spawn_cap_h, "console", &con_proc_h, &con_boot_h);
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

    r = svc_load(spawn_cap_h, "svcmgr", &svcmgr_proc_h, &svcmgr_chan_h);
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
                  (long)(RIGHT_READ | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    dup_cap_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP);
    msg.data_len        = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = dup_cap_h;
    msg.attached_rights = RIGHT_READ;

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

static int init_check_dynamic_registry(handle_id_t sm_h) {
    static const char runtime_name[] = "init.echo";
    struct KChanMsg msg;
    handle_id_t base_h = HANDLE_INVALID;
    handle_id_t publish_h = HANDLE_INVALID;
    handle_id_t proof_h = HANDLE_INVALID;
    handle_id_t proof2_h = HANDLE_INVALID;
    handle_id_t client_h = HANDLE_INVALID;
    handle_id_t base2_h = HANDLE_INVALID;
    handle_id_t publish2_h = HANDLE_INVALID;
    handle_id_t client2_h = HANDLE_INVALID;
    long r;

    r = init_sys0(SYS_CHAN_CREATE);
    if (r < 0) goto fail;
    base_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h,
                  (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    publish_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_REGISTER;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_REGISTER_OFF_ENDPOINT], INIT_RUNTIME_ENDPOINT);
    svcmgr_proto_write_u32(&msg.data[SVCMGR_REGISTER_OFF_RIGHTS], RIGHT_WRITE);
    for (uint32_t i = 0; i + 1u < SVCMGR_SERVICE_NAME_CAP && runtime_name[i]; i++)
        msg.data[SVCMGR_REGISTER_OFF_NAME + i] = (uint8_t)runtime_name[i];
    msg.data_len = SVCMGR_REGISTER_MSG_LEN;
    msg.attached_handle = publish_h;
    msg.attached_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;

    r = init_sys2(SYS_CHAN_SEND, (long)sm_h, (long)&msg);
    if (r < 0) goto fail;
    publish_h = HANDLE_INVALID;

    client_h = init_lookup_name(sm_h, runtime_name, RIGHT_WRITE);
    if (client_h == HANDLE_INVALID) goto fail;

    init_msg_zero(&msg);
    msg.type = 0xA551u;
    msg.data[0] = 'o';
    msg.data[1] = 'k';
    msg.data_len = 2u;
    msg.attached_handle = HANDLE_INVALID;
    r = init_sys2(SYS_CHAN_SEND, (long)client_h, (long)&msg);
    if (r < 0) goto fail;

    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV, (long)base_h, (long)&msg);
    if (r < 0) goto fail;
    if (msg.type != 0xA551u) goto fail;
    if (msg.data_len != 2u || msg.data[0] != 'o' || msg.data[1] != 'k') goto fail;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    proof_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_UNREGISTER;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_UNREGISTER_OFF_ENDPOINT], INIT_RUNTIME_ENDPOINT);
    msg.data_len = SVCMGR_UNREGISTER_MSG_LEN;
    msg.attached_handle = proof_h;
    msg.attached_rights = RIGHT_WRITE | RIGHT_TRANSFER;
    r = init_sys2(SYS_CHAN_SEND, (long)sm_h, (long)&msg);
    if (r < 0) goto fail;
    proof_h = HANDLE_INVALID;

    if (init_lookup_name(sm_h, runtime_name, RIGHT_WRITE) != HANDLE_INVALID) goto fail;
    init_msg_zero(&msg);
    msg.type = 0xA553u;
    msg.data_len = 0u;
    msg.attached_handle = HANDLE_INVALID;
    if (init_sys2(SYS_CHAN_SEND, (long)client_h, (long)&msg) >= 0) goto fail;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    proof2_h = (handle_id_t)r;
    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_UNREGISTER;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_UNREGISTER_OFF_ENDPOINT], INIT_RUNTIME_ENDPOINT);
    msg.data_len = SVCMGR_UNREGISTER_MSG_LEN;
    msg.attached_handle = proof2_h;
    msg.attached_rights = RIGHT_WRITE | RIGHT_TRANSFER;
    r = init_sys2(SYS_CHAN_SEND, (long)sm_h, (long)&msg);
    if (r < 0) goto fail;
    proof2_h = HANDLE_INVALID;

    r = init_sys0(SYS_CHAN_CREATE);
    if (r < 0) goto fail;
    base2_h = (handle_id_t)r;
    r = init_sys2(SYS_HANDLE_DUP, (long)base2_h,
                  (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER));
    if (r < 0) goto fail;
    publish2_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_REGISTER;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_REGISTER_OFF_ENDPOINT], INIT_RUNTIME_ENDPOINT);
    svcmgr_proto_write_u32(&msg.data[SVCMGR_REGISTER_OFF_RIGHTS], RIGHT_WRITE);
    for (uint32_t i = 0; i + 1u < SVCMGR_SERVICE_NAME_CAP && runtime_name[i]; i++)
        msg.data[SVCMGR_REGISTER_OFF_NAME + i] = (uint8_t)runtime_name[i];
    msg.data_len = SVCMGR_REGISTER_MSG_LEN;
    msg.attached_handle = publish2_h;
    msg.attached_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
    r = init_sys2(SYS_CHAN_SEND, (long)sm_h, (long)&msg);
    if (r < 0) goto fail;
    publish2_h = HANDLE_INVALID;

    client2_h = init_lookup_name(sm_h, runtime_name, RIGHT_WRITE);
    if (client2_h == HANDLE_INVALID) goto fail;
    init_msg_zero(&msg);
    msg.type = 0xA552u;
    msg.data[0] = 'r';
    msg.data[1] = 'e';
    msg.data_len = 2u;
    msg.attached_handle = HANDLE_INVALID;
    r = init_sys2(SYS_CHAN_SEND, (long)client2_h, (long)&msg);
    if (r < 0) goto fail;
    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV, (long)base2_h, (long)&msg);
    if (r < 0) goto fail;
    if (msg.type != 0xA552u) goto fail;
    if (msg.data_len != 2u || msg.data[0] != 'r' || msg.data[1] != 'e') goto fail;

    init_close(&client2_h);
    init_close(&base2_h);
    init_close(&client_h);
    init_close(&base_h);
    return 1;

fail:
    init_close(&client2_h);
    if (publish2_h != HANDLE_INVALID) init_close(&publish2_h);
    init_close(&base2_h);
    init_close(&client_h);
    if (proof_h != HANDLE_INVALID) init_close(&proof_h);
    if (proof2_h != HANDLE_INVALID) init_close(&proof2_h);
    if (publish_h != HANDLE_INVALID) init_close(&publish_h);
    init_close(&base_h);
    return 0;
}

static int init_wait_dynamic_registry(handle_id_t sm_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_check_dynamic_registry(sm_h)) return 1;
        init_retry_pause();
    }
    return 0;
}

/* ── Diagnostics check ──────────────────────────────────────────────────── */

static int init_check_diag(handle_id_t sm_h) {
    struct KChanMsg msg;
    handle_id_t base_h = HANDLE_INVALID;
    handle_id_t recv_h = HANDLE_INVALID;
    handle_id_t xfer_h = HANDLE_INVALID;
    int ok = 0;
    long r;

    r = init_sys0(SYS_CHAN_CREATE);
    if (r < 0) goto done;
    base_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h,
                  (long)RIGHT_READ);
    if (r < 0) goto done;
    recv_h = (handle_id_t)r;

    r = init_sys2(SYS_HANDLE_DUP, (long)base_h,
                  (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (r < 0) goto done;
    xfer_h = (handle_id_t)r;

    init_msg_zero(&msg);
    msg.type = SVCMGR_MSG_DIAG;
    msg.data_len = SVCMGR_DIAG_MSG_LEN;
    msg.attached_handle = xfer_h;
    msg.attached_rights = RIGHT_WRITE | RIGHT_TRANSFER;

    init_log(init_diag_req);
    r = init_sys2(SYS_CHAN_SEND, (long)sm_h, (long)&msg);
    if (r < 0) goto done;
    xfer_h = HANDLE_INVALID;

    init_log(init_diag_sent);
    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV, (long)recv_h, (long)&msg);
    if (r < 0) goto done;
    init_log(init_diag_reply);
    if (msg.type != SVCMGR_MSG_DIAG_REPLY) goto done;

    {
        int32_t  err      = (int32_t)svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_ERR]);
        uint32_t manifest = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_MANIFEST]);
        uint32_t ready    = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_READY]);
        uint32_t slots    = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_SLOTS]);
        uint32_t tasks    = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_TASKS]);
        uint32_t kproc    = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_KPROC]);
        uint32_t irq      = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_IRQ]);
        uint32_t cat      = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_CATALOG]);
        uint32_t vfs_exp  = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_VFS_EXPORTS]);
        uint32_t vfs_op   = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_VFS_OPENS]);
        uint32_t vfs_cap  = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_VFS_CAP]);
        uint32_t vfs_by   = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_VFS_BYTES]);
        uint32_t kbd_fl   = svcmgr_proto_read_u32(&msg.data[SVCMGR_DIAG_REPLY_OFF_KBD_FLAGS]);

        if (err != 0)                                   goto done;
        if (manifest != 3u)                             goto done;
        if (ready    != 3u)                             goto done;
        if (slots    != 3u)                             goto done;
        if (tasks    == 0u)                             goto done;
        if (kproc    == 0u)                             goto done;
        if (irq      == 0u)                             goto done;
        if (cat      != IRIS_SERVICE_CATALOG_VERSION)   goto done;
        if (vfs_exp  != VFS_BOOT_EXPORT_COUNT)          goto done;
        if (vfs_op   != 0u)                             goto done;
        if (vfs_cap  != VFS_SERVICE_OPEN_FILES)         goto done;
        if (vfs_by   != VFS_BOOT_EXPORT_TOTAL_BYTES)    goto done;
        if (kbd_fl   != KBD_STATUS_NORMAL)              goto done;
    }
    ok = 1;

done:
    init_close(&base_h);
    init_close(&recv_h);
    if (xfer_h != HANDLE_INVALID) init_close(&xfer_h);
    return ok;
}

static int init_wait_diag(handle_id_t sm_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_check_diag(sm_h)) return 1;
        init_retry_pause();
    }
    return 0;
}

/* ── VFS LIST check ─────────────────────────────────────────────────────── */

static int init_check_vfs_list(handle_id_t vfs_h, handle_id_t vfs_reply_h) {
    struct KChanMsg msg;
    long r;

    /* index 0 — expect OK */
    init_msg_zero(&msg);
    msg.type     = VFS_MSG_LIST;
    msg.data_len = VFS_MSG_LIST_LEN;
    vfs_proto_write_u32(&msg.data[VFS_MSG_OFF_LIST_INDEX], 0u);
    msg.attached_handle = HANDLE_INVALID;
    r = init_chan_send_recv(vfs_h, vfs_reply_h, &msg);
    if (r < 0) return 0;
    if (msg.type != VFS_MSG_LIST_REPLY) return 0;
    if ((int32_t)vfs_proto_read_u32(&msg.data[VFS_MSG_OFF_LIST_REPLY_ERR]) != 0) return 0;

    /* index 1 — expect OK */
    init_msg_zero(&msg);
    msg.type     = VFS_MSG_LIST;
    msg.data_len = VFS_MSG_LIST_LEN;
    vfs_proto_write_u32(&msg.data[VFS_MSG_OFF_LIST_INDEX], 1u);
    msg.attached_handle = HANDLE_INVALID;
    r = init_chan_send_recv(vfs_h, vfs_reply_h, &msg);
    if (r < 0) return 0;
    if (msg.type != VFS_MSG_LIST_REPLY) return 0;
    if ((int32_t)vfs_proto_read_u32(&msg.data[VFS_MSG_OFF_LIST_REPLY_ERR]) != 0) return 0;

    /* index 2 — expect out-of-bounds (err != 0) */
    init_msg_zero(&msg);
    msg.type     = VFS_MSG_LIST;
    msg.data_len = VFS_MSG_LIST_LEN;
    vfs_proto_write_u32(&msg.data[VFS_MSG_OFF_LIST_INDEX], 2u);
    msg.attached_handle = HANDLE_INVALID;
    r = init_chan_send_recv(vfs_h, vfs_reply_h, &msg);
    if (r < 0) return 0;
    if (msg.type != VFS_MSG_LIST_REPLY) return 0;
    if ((int32_t)vfs_proto_read_u32(&msg.data[VFS_MSG_OFF_LIST_REPLY_ERR]) == 0) return 0;

    return 1;
}

static int init_wait_vfs_list(handle_id_t vfs_h, handle_id_t vfs_reply_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_check_vfs_list(vfs_h, vfs_reply_h)) return 1;
        init_retry_pause();
    }
    return 0;
}

/* ── VFS OPEN / READ / CLOSE check ─────────────────────────────────────── */

static int init_check_vfs_rw(handle_id_t vfs_h, handle_id_t vfs_reply_h) {
    struct KChanMsg msg;
    handle_id_t proc_h  = HANDLE_INVALID;
    handle_id_t xfer_h  = HANDLE_INVALID;
    uint32_t    file_id = 0;
    long r;

    /* SYS_PROCESS_SELF → proc_h */
    r = init_sys0(SYS_PROCESS_SELF);
    if (r < 0) return 0;
    proc_h = (handle_id_t)r;

    /* Dup proc handle with RIGHT_READ for VFS open watch */
    r = init_sys2(SYS_HANDLE_DUP, (long)proc_h, (long)(RIGHT_READ | RIGHT_TRANSFER));
    if (r < 0) { init_close(&proc_h); return 0; }
    xfer_h = (handle_id_t)r;

    /* VFS_MSG_OPEN */
    init_msg_zero(&msg);
    msg.type = VFS_MSG_OPEN;
    vfs_proto_write_u32(&msg.data[VFS_MSG_OFF_OPEN_FLAGS], (uint32_t)VFS_O_READ);
    {
        const char path[] = "iris.txt";
        uint32_t plen = 0;
        while (path[plen]) plen++;
        plen++; /* include NUL */
        for (uint32_t i = 0; i < plen; i++)
            msg.data[VFS_MSG_OFF_OPEN_PATH + i] = (uint8_t)path[i];
        msg.data_len = VFS_MSG_OFF_OPEN_PATH + plen;
    }
    msg.attached_handle = xfer_h;
    msg.attached_rights = RIGHT_READ | RIGHT_TRANSFER;

    r = init_chan_send_recv(vfs_h, vfs_reply_h, &msg);
    xfer_h = HANDLE_INVALID; /* consumed */
    init_close(&proc_h);
    if (r < 0) return 0;
    if (msg.type != VFS_MSG_OPEN_REPLY) return 0;
    if ((int32_t)vfs_proto_read_u32(&msg.data[VFS_MSG_OFF_OPEN_REPLY_ERR]) != 0) return 0;
    file_id = vfs_proto_read_u32(&msg.data[VFS_MSG_OFF_OPEN_REPLY_FILE_ID]);

    /* VFS_MSG_READ */
    init_msg_zero(&msg);
    msg.type = VFS_MSG_READ;
    vfs_proto_write_u32(&msg.data[VFS_MSG_OFF_READ_FILE_ID], file_id);
    vfs_proto_write_u32(&msg.data[VFS_MSG_OFF_READ_LEN], VFS_MSG_READ_REPLY_DATA_MAX);
    msg.data_len        = VFS_MSG_READ_LEN;
    msg.attached_handle = HANDLE_INVALID;

    r = init_chan_send_recv(vfs_h, vfs_reply_h, &msg);
    if (r < 0) return 0;
    if (msg.type != VFS_MSG_READ_REPLY) return 0;
    if ((int32_t)vfs_proto_read_u32(&msg.data[VFS_MSG_OFF_READ_REPLY_ERR]) != 0) return 0;

    /* VFS_MSG_CLOSE */
    init_msg_zero(&msg);
    msg.type = VFS_MSG_CLOSE;
    vfs_proto_write_u32(&msg.data[VFS_MSG_OFF_CLOSE_FILE_ID], file_id);
    msg.data_len        = VFS_MSG_CLOSE_LEN;
    msg.attached_handle = HANDLE_INVALID;

    r = init_chan_send_recv(vfs_h, vfs_reply_h, &msg);
    if (r < 0) return 0;
    if (msg.type != VFS_MSG_CLOSE_REPLY) return 0;
    if ((int32_t)vfs_proto_read_u32(&msg.data[VFS_MSG_OFF_CLOSE_REPLY_ERR]) != 0) return 0;

    return 1;
}

static int init_wait_vfs_rw(handle_id_t vfs_h, handle_id_t vfs_reply_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_check_vfs_rw(vfs_h, vfs_reply_h)) return 1;
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
    handle_id_t bootstrap_h = HANDLE_INVALID;
    handle_id_t sm_h        = HANDLE_INVALID;
    handle_id_t kbd_h       = HANDLE_INVALID;
    handle_id_t kbd_reply_h = HANDLE_INVALID;
    handle_id_t vfs_h       = HANDLE_INVALID;
    handle_id_t vfs_reply_h = HANDLE_INVALID;
    handle_id_t scan_recv_h = HANDLE_INVALID;

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

    init_log("[USER] init bootstrap start\n");

    sm_h = init_spawn_svcmgr(bootstrap_h, g_init_console_h);
    if (sm_h == HANDLE_INVALID) {
        init_log("[USER] svcmgr spawn FAILED\n");
        init_exit(1);
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

    vfs_h = init_lookup_wait(sm_h, SVCMGR_ENDPOINT_VFS,
                             RIGHT_WRITE | RIGHT_DUPLICATE);
    if (vfs_h == HANDLE_INVALID) {
        init_log("[USER] vfs lookup FAILED\n");
        init_exit(4);
    }

    vfs_reply_h = init_lookup_wait(sm_h, SVCMGR_ENDPOINT_VFS_REPLY,
                                   RIGHT_READ | RIGHT_DUPLICATE);
    if (vfs_reply_h == HANDLE_INVALID) {
        init_log("[USER] vfs reply lookup FAILED\n");
        init_exit(5);
    }

    /* ── KBD HELLO ── */
    init_log(init_stage_hello);
    if (!init_wait_kbd_hello(kbd_h, kbd_reply_h)) {
        init_log("[USER] kbd hello FAILED\n");
        init_exit(6);
    }
    init_log("[USER] kbd hello reply OK\n");

    /* ── Diagnostics ── */
    init_log(init_stage_diag);
    if (!init_wait_diag(sm_h)) {
        init_log("[USER] diag FAILED\n");
        init_exit(7);
    }
    init_log("[USER] diag OK\n");

    init_log(init_stage_dynamic);
    if (!init_wait_dynamic_registry(sm_h)) {
        init_log("[USER] dynamic registry FAILED\n");
        init_exit(8);
    }
    init_log("[USER] dynamic registry OK\n");

    /* ── VFS LIST ── */
    init_log(init_stage_vfs_list);
    if (!init_wait_vfs_list(vfs_h, vfs_reply_h)) {
        init_log("[USER] vfs list FAILED\n");
        init_exit(9);
    }
    init_log("[USER] vfs list reply OK\n");

    /* ── VFS OPEN / READ / CLOSE ── */
    init_log(init_stage_vfs_rw);
    if (!init_wait_vfs_rw(vfs_h, vfs_reply_h)) {
        init_log("[USER] vfs rw FAILED\n");
        init_exit(10);
    }
    init_log("[USER] vfs open reply OK\n");
    init_log("[USER] vfs read reply OK\n");
    init_log("[USER] vfs close reply OK\n");

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

    /* ── Interactive echo loop ── */
    init_echo_loop(scan_recv_h);

    /* unreachable */
    init_exit(0);
}
