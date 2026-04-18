/*
 * main.c — init service (ring-3 ELF, phase 22+).
 *
 * Receives the svcmgr bootstrap channel handle in %rdi (set by entry.S from %rbx).
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
#include <iris/svcmgr_proto.h>
#include <iris/vfs_proto.h>
#include <iris/kbd_proto.h>
#include <iris/vfs.h>

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

static void init_log(const char *s) {
    init_sys1(SYS_WRITE, (long)s);
}

static const char init_stage_lookup[]    = "[USER][INIT][S1] service lookup\n";
static const char init_stage_hello[]     = "[USER][INIT][S2] kbd hello\n";
static const char init_stage_diag[]      = "[USER][INIT][S3] global diag\n";
static const char init_stage_vfs_list[]  = "[USER][INIT][S4] vfs list\n";
static const char init_stage_vfs_rw[]    = "[USER][INIT][S5] vfs rw\n";
static const char init_stage_subscribe[] = "[USER][INIT][S6] kbd subscribe\n";
static const char init_stage_healthy[]   = "[USER][INIT][BOOT] healthy path OK\n";

#define INIT_RETRY_LIMIT 100
#define INIT_RETRY_SLEEP_TICKS 2

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

static void init_msg_zero(struct KChanMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

static void init_retry_pause(void) {
    (void)init_sys1(SYS_SLEEP, INIT_RETRY_SLEEP_TICKS);
}

/* ── Channel helper: send msg, recv reply (SYS_CHAN_SEND + SYS_CHAN_RECV) ── */

static long init_chan_send_recv(handle_id_t send_h, handle_id_t recv_h,
                                struct KChanMsg *msg) {
    long r = init_sys2(SYS_CHAN_SEND, (long)send_h, (long)msg);
    if (r < 0) return r;
    init_msg_zero(msg);
    return init_sys2(SYS_CHAN_RECV, (long)recv_h, (long)msg);
}

/* ── svcmgr spawn (E2: init launches svcmgr, not the kernel) ───────────── */

static handle_id_t init_spawn_svcmgr(handle_id_t spawn_cap_h) {
    handle_id_t svcmgr_entry_h = HANDLE_INVALID;
    handle_id_t svcmgr_proc_h  = HANDLE_INVALID;
    handle_id_t svcmgr_chan_h  = HANDLE_INVALID;
    handle_id_t dup_cap_h      = HANDLE_INVALID;
    struct KChanMsg msg;
    long r;

    r = init_sys2(SYS_INITRD_LOOKUP, (long)spawn_cap_h, (long)"svcmgr");
    if (r < 0) goto fail;
    svcmgr_entry_h = (handle_id_t)r;

    r = init_sys2(SYS_SPAWN_ELF, (long)svcmgr_entry_h, (long)&svcmgr_chan_h);
    if (r < 0) goto fail;
    svcmgr_proc_h = (handle_id_t)r;

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

    init_close(&svcmgr_entry_h);
    init_close(&svcmgr_proc_h);
    return svcmgr_chan_h;

fail:
    init_close(&svcmgr_entry_h);
    init_close(&svcmgr_proc_h);
    init_close(&svcmgr_chan_h);
    if (dup_cap_h != HANDLE_INVALID) init_close(&dup_cap_h);
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

    r = init_sys2(SYS_CHAN_SEND, (long)sm_h, (long)&msg);
    if (r < 0) goto done;
    xfer_h = HANDLE_INVALID;

    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV, (long)recv_h, (long)&msg);
    if (r < 0) goto done;
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
        if (manifest != 2u)                             goto done;
        if (ready    != 2u)                             goto done;
        if (slots    != 2u)                             goto done;
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
 * on every IRQ.  We read events from scan_recv_h.
 * Returns scan_recv_h (caller must close on exit), HANDLE_INVALID on error.
 */
static handle_id_t init_kbd_subscribe(handle_id_t kbd_h,
                                      handle_id_t kbd_reply_h) {
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

    init_msg_zero(&msg);
    r = init_sys2(SYS_CHAN_RECV, (long)kbd_reply_h, (long)&msg);
    if (r < 0) goto fail;
    if (msg.type != KBD_MSG_SUBSCRIBE_REPLY) goto fail;
    if ((int32_t)kbd_proto_read_u32(&msg.data[KBD_MSG_OFF_SUBSCRIBE_REPLY_ERR]) != 0) goto fail;

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
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        handle_id_t scan_recv_h = init_kbd_subscribe(kbd_h, kbd_reply_h);
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
    /* One-char NUL-terminated write buffer (3 bytes: char, optional \n, NUL) */
    char buf[3];

    init_log("[USER] init echo loop start\n");

    for (;;) {
        long r = init_sys2(SYS_CHAN_RECV_NB, (long)scan_recv_h, (long)&msg);
        if (r == 0) {
            if (msg.type == KBD_MSG_SCANCODE_EVENT &&
                msg.data_len >= KBD_MSG_SCANCODE_EVENT_LEN) {
                uint8_t sc = msg.data[KBD_MSG_OFF_SC_EVENT_CODE];
                /* Only process key-press events (bit 7 clear) */
                if ((sc & 0x80u) == 0u) {
                    char ch = g_sc_to_ascii[sc & 0x7Fu];
                    if (ch != 0) {
                        buf[0] = ch;
                        buf[1] = 0;
                        init_sys1(SYS_WRITE, (long)buf);
                    }
                }
            }
        }
        /* Brief yield to avoid busy-spinning the CPU */
        init_sys1(SYS_SLEEP, 10);
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void init_main(handle_id_t bootstrap_h) {
    handle_id_t sm_h        = HANDLE_INVALID;
    handle_id_t kbd_h       = HANDLE_INVALID;
    handle_id_t kbd_reply_h = HANDLE_INVALID;
    handle_id_t vfs_h       = HANDLE_INVALID;
    handle_id_t vfs_reply_h = HANDLE_INVALID;
    handle_id_t scan_recv_h = HANDLE_INVALID;

    init_log("[USER] init bootstrap start\n");

    sm_h = init_spawn_svcmgr(bootstrap_h);
    if (sm_h == HANDLE_INVALID) {
        init_log("[USER] svcmgr spawn FAILED\n");
        init_exit(1);
    }

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

    /* ── VFS LIST ── */
    init_log(init_stage_vfs_list);
    if (!init_wait_vfs_list(vfs_h, vfs_reply_h)) {
        init_log("[USER] vfs list FAILED\n");
        init_exit(8);
    }
    init_log("[USER] vfs list reply OK\n");

    /* ── VFS OPEN / READ / CLOSE ── */
    init_log(init_stage_vfs_rw);
    if (!init_wait_vfs_rw(vfs_h, vfs_reply_h)) {
        init_log("[USER] vfs rw FAILED\n");
        init_exit(9);
    }
    init_log("[USER] vfs open reply OK\n");
    init_log("[USER] vfs read reply OK\n");
    init_log("[USER] vfs close reply OK\n");

    /* ── KBD SUBSCRIBE ── */
    init_log(init_stage_subscribe);
    scan_recv_h = init_wait_kbd_subscribe(kbd_h, kbd_reply_h);
    if (scan_recv_h == HANDLE_INVALID) {
        init_log("[USER] kbd subscribe FAILED\n");
        init_exit(10);
    }
    init_log("[USER] kbd subscribe OK\n");
    init_log(init_stage_healthy);

    /* ── Interactive echo loop ── */
    init_echo_loop(scan_recv_h);

    /* unreachable */
    init_exit(0);
}
