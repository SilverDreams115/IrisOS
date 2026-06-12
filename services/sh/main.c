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

/* Console endpoint path (Fase 8): sh is a pure CPtr-first client — ALL
 * console output goes through the well-known slot IRIS_CPTR_CONSOLE_EP.
 * There is no legacy console cap anymore: if the slot is broken, sh stays
 * silent and every gated "[SH] ... OK" marker is missing, which fails the
 * smoke run. The `con` parameter is kept so call sites stay unchanged. */
static handle_id_t g_sh_con_ep_h = (handle_id_t)IRIS_CPTR_CONSOLE_EP;
static uint8_t g_sh_con_ep_buf[IRIS_IPC_BUF_SIZE];

static void sh_cout(handle_id_t con, const char *s) {
    (void)con;
    (void)console_ep_write(g_sh_con_ep_h, g_sh_con_ep_buf, s);
}

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
    if (v == 0) { sh_cout(con, "0"); return; }
    while (v && i < (uint32_t)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    char out[2] = {0, 0};
    while (i) { out[0] = buf[--i]; sh_cout(con, out); }
}

/* ── VFS endpoint path (Fase 7.1) ────────────────────────────────── */

/* (Fase 8: sh_svc_ep_lookup removed — sh discovers nothing at runtime;
 * every core service cap is a well-known CSpace slot.) */

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
            sh_cout(con, "ls: vfs ep call failed\r\n");
            return;
        }
        if (msg.label != IRIS_EP_REPLY_OK)
            return;  /* NOT_FOUND past the last export — end of listing */

        uint32_t size     = (uint32_t)msg.words[1];
        uint32_t name_len = (uint32_t)msg.words[2];
        if (name_len >= VFS_EP_PATH_MAX) name_len = VFS_EP_PATH_MAX - 1u;
        g_sh_ep_buf[name_len] = 0u;

        sh_cout(con, "  ");
        sh_cout(con, (const char *)g_sh_ep_buf);
        sh_cout(con, "  (");
        sh_write_u32(con, size);
        sh_cout(con, " bytes)\r\n");
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
            sh_cout(con, "cat: vfs ep call failed\r\n");
            return;
        }
        if (msg.label != IRIS_EP_REPLY_OK) {
            if (offset == 0)
                sh_cout(con, "cat: not found\r\n");
            else
                sh_cout(con, "cat: read error\r\n");
            return;
        }

        uint32_t bytes = (uint32_t)msg.words[1];
        uint64_t total = msg.words[2];
        if (bytes == 0) return;  /* EOF */
        if (bytes > VFS_EP_DATA_MAX) bytes = VFS_EP_DATA_MAX;

        g_sh_ep_buf[bytes] = 0u;
        sh_cout(con, (const char *)g_sh_ep_buf);

        offset += bytes;
        if (offset >= total) return;
    }
}

/* ── Command dispatch ────────────────────────────────────────────── */

static void sh_dispatch(handle_id_t con, const char *line) {
    if (sh_word_eq(line, "help")) {
        sh_cout(con, "Commands:\r\n"
                           "  help          this message\r\n"
                           "  ver           version info\r\n"
                           "  uptime        seconds since boot\r\n"
                           "  ls            list VFS files\r\n"
                           "  cat <file>    read a file\r\n"
                           "  clear         clear screen\r\n");
        return;
    }
    if (sh_word_eq(line, "ver")) {
        sh_cout(con, "IRIS Phase 55 — pure microkernel shell\r\n"
                           "  kernel:   x86_64 ring-0/3, cooperative+preemptive\r\n"
                           "  services: init svcmgr kbd vfs console fb sh\r\n"
                           "  syscalls: SYS_KLOG_DRAIN(65) SYS_EXCEPTION_RESUME(66) SYS_VMO_SIZE(67)\r\n");
        return;
    }
    if (sh_word_eq(line, "uptime")) {
        long ns = sh_sys0(SYS_CLOCK_GET);
        if (ns < 0) {
            sh_cout(con, "uptime: clock unavailable\r\n");
        } else {
            uint64_t secs = (uint64_t)ns / 1000000000ULL;
            sh_cout(con, "uptime: ");
            sh_write_u32(con, (uint32_t)secs);
            sh_cout(con, " s\r\n");
        }
        return;
    }
    if (sh_word_eq(line, "ls")) {
        /* Endpoint-only path (Fase 7.2): no legacy KChannel fallback. */
        if (g_sh_vfs_ep_h == HANDLE_INVALID) {
            sh_cout(con, "ls: VFS endpoint unavailable\r\n");
            return;
        }
        sh_cmd_ls_ep(con);
        return;
    }
    if (sh_word_eq(line, "cat")) {
        const char *path = sh_skip_word(line);
        if (*path == '\0') {
            sh_cout(con, "usage: cat <filename>\r\n");
            return;
        }
        if (g_sh_vfs_ep_h == HANDLE_INVALID) {
            sh_cout(con, "cat: VFS endpoint unavailable\r\n");
            return;
        }
        sh_cmd_cat_ep(con, path);
        sh_cout(con, "\r\n");
        return;
    }
    if (sh_word_eq(line, "clear")) {
        sh_cout(con, "\033[2J\033[H");
        return;
    }
    sh_cout(con, "unknown command: ");
    sh_cout(con, line);
    sh_cout(con, "\r\n");
}

/* ── Main entry ──────────────────────────────────────────────────── */

void sh_main_c(handle_id_t bootstrap_h) {
    handle_id_t console_h     = HANDLE_INVALID;  /* unused: pure CPtr client */
    handle_id_t kbd_ep_h      = HANDLE_INVALID;

    /* Fase 8: sh is a pure CPtr-first client. The bootstrap bag is empty
     * (catalog: endpoint_only without an own endpoint) — everything sh
     * needs was minted into its root CNode before it ran:
     *   slot 1 svcmgr discovery, slot 2 vfs.ep, slot 3 console.ep,
     *   slot 4 kbd.ep.  Close the (empty) bootstrap channel and verify
     * each slot with a PING; every marker below is a smoke gate, so a
     * broken slot cannot hide.  No lookup, no handle fallback. */
    (void)sh_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_h);

    sh_cout(console_h, "[SH] boot\n");

    {
        struct IrisMsg pmsg;
        sh_imsg_zero(&pmsg);
        pmsg.label = IRIS_EP_OP_PING;
        if (sh_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&pmsg) == IRIS_OK &&
            pmsg.label == IRIS_EP_REPLY_OK)
            sh_cout(console_h, "[SH] svcmgr cptr OK\n");
        else
            sh_cout(console_h, "[SH] svcmgr cptr FAILED\n");

        sh_imsg_zero(&pmsg);
        pmsg.label = IRIS_EP_OP_PING;
        if (sh_sys2(SYS_EP_CALL, (long)IRIS_CPTR_VFS_EP, (long)&pmsg) == IRIS_OK &&
            pmsg.label == IRIS_EP_REPLY_OK) {
            g_sh_vfs_ep_h = (handle_id_t)IRIS_CPTR_VFS_EP;
            sh_cout(console_h, "[SH] vfs cptr OK\n");
        } else {
            sh_cout(console_h, "[SH] vfs cptr FAILED\n");
        }

        sh_imsg_zero(&pmsg);
        pmsg.label = IRIS_EP_OP_PING;
        if (sh_sys2(SYS_EP_CALL, (long)IRIS_CPTR_KBD_EP, (long)&pmsg) == IRIS_OK &&
            pmsg.label == IRIS_EP_REPLY_OK) {
            kbd_ep_h = (handle_id_t)IRIS_CPTR_KBD_EP;
            sh_cout(console_h, "[SH] kbd cptr OK\n");
        } else {
            sh_cout(console_h, "[SH] kbd cptr FAILED\n");
        }

        /* console: every sh_cout above already exercised slot 3; PING for
         * the symmetric gated marker. */
        sh_imsg_zero(&pmsg);
        pmsg.label = IRIS_EP_OP_PING;
        if (sh_sys2(SYS_EP_CALL, (long)IRIS_CPTR_CONSOLE_EP, (long)&pmsg) == IRIS_OK &&
            pmsg.label == IRIS_EP_REPLY_OK)
            sh_cout(console_h, "[SH] console cptr OK\n");
        else
            sh_cout(console_h, "[SH] console cptr FAILED\n");
    }

    /* Print banner */
    sh_cout(console_h,
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
                sh_cout(console_h, "\b \b");
            }
            continue;
        }

        if (c == '\r') {
            sh_cout(console_h, "\r\n");
            if (line_len > 0) {
                line[line_len] = '\0';
                sh_dispatch(console_h, line);
                line_len = 0;
            }
            sh_cout(console_h, "> ");
            continue;
        }

        if (line_len < 127u) {
            line[line_len++] = c;
            char echo[2] = {c, '\0'};
            sh_cout(console_h, echo);
        }
    }
}
