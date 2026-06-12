/*
 * sh/main.c — ring-3 interactive shell service (Phase 31).
 *
 * Bootstrap protocol (over bootstrap channel from svcmgr):
 *   recv SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP (6) → console_h  (RIGHT_WRITE)
 *   recv SVCMGR_ENDPOINT_SH        (7)        → own service_h (closed; unused)
 *   recv SVCMGR_ENDPOINT_SH_REPLY  (8)        → own reply_h  (closed; unused)
 *   recv SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP (0x20) → svcmgr discovery endpoint
 *
 * VFS access (Fase 7.2): endpoint-only. "vfs.ep" is resolved through the
 * svcmgr discovery endpoint; ls/cat use the stateless VFS EP protocol
 * (iris/vfs_ep_proto.h). There is no legacy KChannel fallback — if the
 * endpoint is missing, ls/cat report the error instead of masking it.
 *
 * Keyboard (Fase 7.4): endpoint-only. "kbd.ep" is resolved through the
 * svcmgr discovery endpoint; the REPL pulls one key event per
 * EP_CALL(KBD_EP_OP_READ) — kbd parks the reply until a key arrives, so the
 * call doubles as the blocking wait. No legacy KChannel subscribe fallback.
 *
 * Commands: help, ver, uptime, ls, cat <file>, clear
 */

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/svcmgr_proto.h>
#include <iris/kbd_ep_proto.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>
#include "../common/console_client.h"

/* ── Syscall helpers ─────────────────────────────────────────────── */

static inline long sh_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}
static inline long sh_sys2(long nr, long a0, long a1) { return sh_sys3(nr, a0, a1, 0); }
static inline long sh_sys1(long nr, long a0)           { return sh_sys3(nr, a0, 0, 0); }
static inline long sh_sys0(long nr)                    { return sh_sys3(nr, 0, 0, 0); }

static void sh_msg_zero(struct KChanMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

static void sh_imsg_zero(struct IrisMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

/* VFS endpoint handle (Fase 7.1; mandatory since Fase 7.2). Resolved once
 * after bootstrap via the svcmgr discovery endpoint; HANDLE_INVALID means
 * VFS is unavailable — ls/cat fail loudly, there is no legacy fallback. */
static handle_id_t g_sh_vfs_ep_h = HANDLE_INVALID;

/* IPC bulk buffer for EP_CALL round trips (request payload and reply data
 * share the buffer — EP_CALL reuses buf_uptr in both directions). */
static uint8_t g_sh_ep_buf[VFS_EP_DATA_MAX + 1u];

/* ── PS/2 Set-1 scancode tables ──────────────────────────────────── */

static const char sc_lower[0x80] = {
    0,    '\x1b','1',  '2',  '3',  '4',  '5',  '6',  /* 0x00-0x07 */
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t', /* 0x08-0x0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  /* 0x10-0x17 */
    'o',  'p',  '[',  ']',  '\r', 0,    'a',  's',  /* 0x18-0x1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  /* 0x20-0x27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',  /* 0x28-0x2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  /* 0x30-0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,    /* 0x38-0x3F */
    0,    0,    0,    0,    0,    0,    0,    '7',  /* 0x40-0x47 */
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  /* 0x48-0x4F */
    '2',  '3',  '0',  '.',  0,    0,    0,    0,    /* 0x50-0x57 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x58-0x5F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x60-0x67 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x68-0x6F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x70-0x77 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x78-0x7F */
};

static const char sc_upper[0x80] = {
    0,    '\x1b','!',  '@',  '#',  '$',  '%',  '^',  /* 0x00-0x07 */
    '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t', /* 0x08-0x0F */
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  /* 0x10-0x17 */
    'O',  'P',  '{',  '}',  '\r', 0,    'A',  'S',  /* 0x18-0x1F */
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  /* 0x20-0x27 */
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',  /* 0x28-0x2F */
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',  /* 0x30-0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,    /* 0x38-0x3F */
    0,    0,    0,    0,    0,    0,    0,    '7',  /* 0x40-0x47 */
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  /* 0x48-0x4F */
    '2',  '3',  '0',  '.',  0,    0,    0,    0,    /* 0x50-0x57 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x58-0x5F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x60-0x67 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x68-0x6F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x70-0x77 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 0x78-0x7F */
};

/* ── String helpers ──────────────────────────────────────────────── */

static int sh_word_eq(const char *line, const char *word) {
    while (*word) {
        if (*line != *word) return 0;
        line++; word++;
    }
    return *line == '\0' || *line == ' ';
}

static const char *sh_skip_word(const char *s) {
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return s;
}

static uint32_t sh_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ── Console output helpers ──────────────────────────────────────── */

static void sh_write_u32(handle_id_t con, uint32_t v) {
    char buf[11];
    uint32_t i = 0;
    if (v == 0) { console_write(con, "0"); return; }
    while (v && i < (uint32_t)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    char out[2] = {0, 0};
    while (i) { out[0] = buf[--i]; console_write(con, out); }
}

/* ── VFS endpoint path (Fase 7.1) ────────────────────────────────── */

/*
 * Resolve the VFS endpoint via the svcmgr discovery endpoint:
 * EP_CALL(svcmgr_ep, IRIS_SVCMGR_EP_LOOKUP_NAME, "vfs.ep"). The reply
 * carries the endpoint cap (RIGHT_WRITE) via SYS_REPLY cap transfer.
 */
static handle_id_t sh_svc_ep_lookup(handle_id_t svcmgr_ep_h,
                                    const char *ep_name) {
    struct IrisMsg msg;
    uint32_t len = 0;

    if (svcmgr_ep_h == HANDLE_INVALID) return HANDLE_INVALID;

    while (ep_name[len]) {
        g_sh_ep_buf[len] = (uint8_t)ep_name[len];
        len++;
    }
    g_sh_ep_buf[len++] = 0u;  /* include NUL */

    sh_imsg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_sh_ep_buf;
    msg.buf_len  = len;

    if (sh_sys2(SYS_EP_CALL, (long)svcmgr_ep_h, (long)&msg) != IRIS_OK)
        return HANDLE_INVALID;
    if (msg.label != IRIS_EP_REPLY_OK)
        return HANDLE_INVALID;
    if (msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP)
        return HANDLE_INVALID;
    return (handle_id_t)msg.attached_handle;
}

/*
 * One VFS endpoint call. The path (when non-NULL) is staged into g_sh_ep_buf;
 * reply bulk data lands in the same buffer. Returns IRIS_OK and fills *msg on
 * a served round trip (msg->label distinguishes OK from protocol error).
 */
static int sh_vfs_ep_call(struct IrisMsg *msg, const char *path) {
    msg->buf_uptr = (uint64_t)(uintptr_t)g_sh_ep_buf;
    if (path) {
        uint32_t plen = sh_strlen(path);
        if (plen + 1u > VFS_EP_PATH_MAX) return (int)IRIS_ERR_INVALID_ARG;
        for (uint32_t i = 0; i < plen; i++) g_sh_ep_buf[i] = (uint8_t)path[i];
        g_sh_ep_buf[plen] = 0u;
        msg->buf_len = plen + 1u;
    }
    return (int)sh_sys2(SYS_EP_CALL, (long)g_sh_vfs_ep_h, (long)msg);
}

static void sh_cmd_ls_ep(handle_id_t con) {
    for (uint32_t idx = 0; idx < 64u; idx++) {
        struct IrisMsg msg;
        sh_imsg_zero(&msg);
        msg.label      = VFS_EP_OP_LIST;
        msg.words[0]   = idx;
        msg.word_count = 1u;

        if (sh_vfs_ep_call(&msg, 0) != IRIS_OK) {
            console_write(con, "ls: vfs ep call failed\r\n");
            return;
        }
        if (msg.label != IRIS_EP_REPLY_OK)
            return;  /* NOT_FOUND past the last export — end of listing */

        uint32_t size     = (uint32_t)msg.words[1];
        uint32_t name_len = (uint32_t)msg.words[2];
        if (name_len >= VFS_EP_PATH_MAX) name_len = VFS_EP_PATH_MAX - 1u;
        g_sh_ep_buf[name_len] = 0u;

        console_write(con, "  ");
        console_write(con, (const char *)g_sh_ep_buf);
        console_write(con, "  (");
        sh_write_u32(con, size);
        console_write(con, " bytes)\r\n");
    }
}

static void sh_cmd_cat_ep(handle_id_t con, const char *path) {
    uint64_t offset = 0;

    for (;;) {
        struct IrisMsg msg;
        sh_imsg_zero(&msg);
        msg.label      = VFS_EP_OP_READ_AT;
        msg.words[0]   = offset;
        msg.words[1]   = VFS_EP_DATA_MAX;
        msg.word_count = 2u;

        if (sh_vfs_ep_call(&msg, path) != IRIS_OK) {
            console_write(con, "cat: vfs ep call failed\r\n");
            return;
        }
        if (msg.label != IRIS_EP_REPLY_OK) {
            if (offset == 0)
                console_write(con, "cat: not found\r\n");
            else
                console_write(con, "cat: read error\r\n");
            return;
        }

        uint32_t bytes = (uint32_t)msg.words[1];
        uint64_t total = msg.words[2];
        if (bytes == 0) return;  /* EOF */
        if (bytes > VFS_EP_DATA_MAX) bytes = VFS_EP_DATA_MAX;

        g_sh_ep_buf[bytes] = 0u;
        console_write(con, (const char *)g_sh_ep_buf);

        offset += bytes;
        if (offset >= total) return;
    }
}

/* ── Command dispatch ────────────────────────────────────────────── */

static void sh_dispatch(handle_id_t con, const char *line) {
    if (sh_word_eq(line, "help")) {
        console_write(con, "Commands:\r\n"
                           "  help          this message\r\n"
                           "  ver           version info\r\n"
                           "  uptime        seconds since boot\r\n"
                           "  ls            list VFS files\r\n"
                           "  cat <file>    read a file\r\n"
                           "  clear         clear screen\r\n");
        return;
    }
    if (sh_word_eq(line, "ver")) {
        console_write(con, "IRIS Phase 55 — pure microkernel shell\r\n"
                           "  kernel:   x86_64 ring-0/3, cooperative+preemptive\r\n"
                           "  services: init svcmgr kbd vfs console fb sh\r\n"
                           "  syscalls: SYS_KLOG_DRAIN(65) SYS_EXCEPTION_RESUME(66) SYS_VMO_SIZE(67)\r\n");
        return;
    }
    if (sh_word_eq(line, "uptime")) {
        long ns = sh_sys0(SYS_CLOCK_GET);
        if (ns < 0) {
            console_write(con, "uptime: clock unavailable\r\n");
        } else {
            uint64_t secs = (uint64_t)ns / 1000000000ULL;
            console_write(con, "uptime: ");
            sh_write_u32(con, (uint32_t)secs);
            console_write(con, " s\r\n");
        }
        return;
    }
    if (sh_word_eq(line, "ls")) {
        /* Endpoint-only path (Fase 7.2): no legacy KChannel fallback. */
        if (g_sh_vfs_ep_h == HANDLE_INVALID) {
            console_write(con, "ls: VFS endpoint unavailable\r\n");
            return;
        }
        sh_cmd_ls_ep(con);
        return;
    }
    if (sh_word_eq(line, "cat")) {
        const char *path = sh_skip_word(line);
        if (*path == '\0') {
            console_write(con, "usage: cat <filename>\r\n");
            return;
        }
        if (g_sh_vfs_ep_h == HANDLE_INVALID) {
            console_write(con, "cat: VFS endpoint unavailable\r\n");
            return;
        }
        sh_cmd_cat_ep(con, path);
        console_write(con, "\r\n");
        return;
    }
    if (sh_word_eq(line, "clear")) {
        console_write(con, "\033[2J\033[H");
        return;
    }
    console_write(con, "unknown command: ");
    console_write(con, line);
    console_write(con, "\r\n");
}

/* ── Main entry ──────────────────────────────────────────────────── */

void sh_main_c(handle_id_t bootstrap_h) {
    handle_id_t console_h     = HANDLE_INVALID;
    handle_id_t kbd_ep_h      = HANDLE_INVALID;
    handle_id_t svcmgr_ep_h   = HANDLE_INVALID;

    /* Bootstrap: receive channels from svcmgr. Timed recv so a missing
     * message degrades to a degraded-but-running shell instead of hanging
     * boot (a missing discovery EP then surfaces as "[SH] vfs ep FAILED"). */
    uint32_t recv_count;
    for (recv_count = 0; recv_count < 16u; recv_count++) {
        struct KChanMsg msg;
        sh_msg_zero(&msg);
        if (sh_sys3(SYS_CHAN_RECV_TIMEOUT, (long)bootstrap_h, (long)&msg,
                    500000000L) != IRIS_OK)
            break;

        if (msg.type != SVCMGR_MSG_BOOTSTRAP_HANDLE) {
            if (msg.attached_handle != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)msg.attached_handle);
            continue;
        }

        uint32_t kind = (uint32_t)msg.data[0] | ((uint32_t)msg.data[1] << 8) |
                        ((uint32_t)msg.data[2] << 16) | ((uint32_t)msg.data[3] << 24);
        handle_id_t h = msg.attached_handle;

        switch (kind) {
        case SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP:     /* 6 */
            if (h != HANDLE_INVALID && console_h == HANDLE_INVALID)
                console_h = h;
            else if (h != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)h);
            break;
        case SVCMGR_ENDPOINT_SH:                    /* 7 — own service channel (unused) */
        case SVCMGR_ENDPOINT_SH_REPLY:              /* 8 — own reply channel (unused) */
            if (h != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)h);
            break;
        case SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP:       /* 0x20 — discovery EP */
            if (h != HANDLE_INVALID && svcmgr_ep_h == HANDLE_INVALID)
                svcmgr_ep_h = h;
            else if (h != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)h);
            break;
        default:
            if (h != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)h);
            break;
        }

        if (console_h != HANDLE_INVALID && svcmgr_ep_h != HANDLE_INVALID)
            break;
    }
    (void)sh_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_h);

    if (console_h == HANDLE_INVALID) return;

    console_write(console_h, "[SH] boot\n");

    /* Fase 7.2: resolve the VFS endpoint — the only VFS path. A failure is
     * reported loudly (the smoke gate requires "[SH] vfs ep OK") and ls/cat
     * surface the error per command instead of masking it. */
    g_sh_vfs_ep_h = sh_svc_ep_lookup(svcmgr_ep_h, VFS_EP_SVC_NAME);
    if (g_sh_vfs_ep_h != HANDLE_INVALID)
        console_write(console_h, "[SH] vfs ep OK\n");
    else
        console_write(console_h, "[SH] vfs ep FAILED\n");

    /* Fase 7.4: resolve the kbd endpoint — the only key-event path. The
     * REPL pulls events with EP_CALL(KBD_EP_OP_READ); there is no legacy
     * KChannel subscribe fallback, a broken endpoint is reported loudly. */
    kbd_ep_h = sh_svc_ep_lookup(svcmgr_ep_h, KBD_EP_SVC_NAME);
    if (kbd_ep_h != HANDLE_INVALID)
        console_write(console_h, "[SH] kbd ep OK\n");
    else
        console_write(console_h, "[SH] kbd ep FAILED\n");

    if (svcmgr_ep_h != HANDLE_INVALID)
        (void)sh_sys1(SYS_HANDLE_CLOSE, (long)svcmgr_ep_h);

    /* Print banner */
    console_write(console_h,
        "\r\n"
        "IRIS shell (Phase 55) — 'help' for commands\r\n"
        "> ");

    /* REPL main loop */
    char line[128];
    uint32_t line_len = 0;
    uint8_t shift = 0;

    for (;;) {
        if (kbd_ep_h == HANDLE_INVALID) {
            (void)sh_sys0(SYS_YIELD);
            continue;
        }

        /* Blocking pull: kbd parks the reply until a key event arrives.
         * WOULD_BLOCK (park slot taken by a concurrent caller) and call
         * errors yield-and-retry; this never spins on an immediate reply. */
        struct IrisMsg msg;
        sh_imsg_zero(&msg);
        msg.label = KBD_EP_OP_READ;
        if (sh_sys2(SYS_EP_CALL, (long)kbd_ep_h, (long)&msg) != IRIS_OK) {
            (void)sh_sys0(SYS_YIELD);
            continue;
        }
        if (msg.label != IRIS_EP_REPLY_OK || msg.word_count < 2u) {
            (void)sh_sys0(SYS_YIELD);
            continue;
        }

        uint8_t sc      = (uint8_t)msg.words[1];
        uint8_t release = sc & 0x80u;
        uint8_t base    = sc & 0x7Fu;

        /* Track left/right shift state (0x2A, 0x36). */
        if (base == 0x2Au || base == 0x36u) {
            shift = release ? 0u : 1u;
            continue;
        }
        if (release) continue;

        /* Map make code to ASCII. */
        char c = shift ? sc_upper[base] : sc_lower[base];
        if (c == 0) continue;

        if (c == '\b') {
            if (line_len > 0) {
                line_len--;
                console_write(console_h, "\b \b");
            }
            continue;
        }

        if (c == '\r') {
            console_write(console_h, "\r\n");
            if (line_len > 0) {
                line[line_len] = '\0';
                sh_dispatch(console_h, line);
                line_len = 0;
            }
            console_write(console_h, "> ");
            continue;
        }

        if (line_len < 127u) {
            line[line_len++] = c;
            char echo[2] = {c, '\0'};
            console_write(console_h, echo);
        }
    }
}
