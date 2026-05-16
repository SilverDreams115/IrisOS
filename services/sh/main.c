/*
 * sh/main.c — ring-3 interactive shell service (Phase 31).
 *
 * Bootstrap protocol (over bootstrap channel from svcmgr):
 *   recv SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP (6) → console_h  (RIGHT_WRITE)
 *   recv SVCMGR_ENDPOINT_SH        (7)        → own service_h (closed; unused)
 *   recv SVCMGR_ENDPOINT_SH_REPLY  (8)        → own reply_h  (closed; unused)
 *   recv SVCMGR_BOOTSTRAP_KIND_KBD_CAP  (9)  → kbd_service_h (RIGHT_WRITE; for SUBSCRIBE)
 *   recv SVCMGR_BOOTSTRAP_KIND_VFS_CAP  (10) → vfs_h         (RIGHT_WRITE)
 *   recv SVCMGR_BOOTSTRAP_KIND_VFS_REPLY_CAP (11) → vfs_reply_h (RIGHT_READ)
 *
 * After bootstrap: subscribes to keyboard scancodes, enters REPL loop.
 * Commands: help, ver, ls, cat <file>, clear
 */

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/svcmgr_proto.h>
#include <iris/kbd_proto.h>
#include <iris/vfs_proto.h>
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

/* ── VFS helpers ─────────────────────────────────────────────────── */

static void sh_cmd_ls(handle_id_t con, handle_id_t vfs_h, handle_id_t vfs_reply_h) {
    struct KChanMsg req, reply;
    uint32_t idx;

    for (idx = 0; idx < 64u; idx++) {
        sh_msg_zero(&req);
        req.type = VFS_MSG_LIST;
        vfs_proto_write_u32(&req.data[VFS_MSG_OFF_LIST_INDEX], idx);
        req.data_len = VFS_MSG_LIST_LEN;
        req.attached_handle = HANDLE_INVALID;
        req.attached_rights = RIGHT_NONE;

        if (sh_sys2(SYS_CHAN_SEND, (long)vfs_h, (long)&req) != IRIS_OK) break;

        sh_msg_zero(&reply);
        if (sh_sys2(SYS_CHAN_RECV, (long)vfs_reply_h, (long)&reply) != IRIS_OK) break;
        if (reply.type != VFS_MSG_LIST_REPLY) break;

        int32_t err = (int32_t)vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_LIST_REPLY_ERR]);
        if (err != 0) break;

        uint32_t size     = vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_LIST_REPLY_SIZE]);
        uint32_t name_len = vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_LIST_REPLY_NAME_LEN]);
        if (name_len > VFS_MSG_LIST_REPLY_NAME_MAX - 1u)
            name_len = VFS_MSG_LIST_REPLY_NAME_MAX - 1u;

        char namebuf[VFS_MSG_LIST_REPLY_NAME_MAX];
        uint32_t ni;
        for (ni = 0; ni < name_len; ni++)
            namebuf[ni] = (char)reply.data[VFS_MSG_OFF_LIST_REPLY_NAME + ni];
        namebuf[name_len] = '\0';

        console_write(con, "  ");
        console_write(con, namebuf);
        console_write(con, "  (");
        sh_write_u32(con, size);
        console_write(con, " bytes)\r\n");
    }
}

static void sh_cmd_cat(handle_id_t con, handle_id_t vfs_h, handle_id_t vfs_reply_h,
                       const char *path) {
    struct KChanMsg req, reply;
    uint32_t file_id = 0;
    int opened = 0;

    /* Acquire own process handle for VFS_MSG_OPEN attachment. */
    long self_h = sh_sys0(SYS_PROCESS_SELF);
    if (self_h < 0) {
        console_write(con, "cat: SYS_PROCESS_SELF failed\r\n");
        return;
    }

    /* VFS_MSG_OPEN */
    sh_msg_zero(&req);
    req.type = VFS_MSG_OPEN;
    vfs_proto_write_u32(&req.data[VFS_MSG_OFF_OPEN_FLAGS], 0u);

    uint32_t plen = sh_strlen(path);
    if (plen >= VFS_MSG_OPEN_PATH_MAX) plen = VFS_MSG_OPEN_PATH_MAX - 1u;
    uint32_t pi;
    for (pi = 0; pi < plen; pi++)
        req.data[VFS_MSG_OFF_OPEN_PATH + pi] = (uint8_t)path[pi];
    req.data[VFS_MSG_OFF_OPEN_PATH + plen] = 0;
    req.data_len = VFS_MSG_OFF_OPEN_PATH + plen + 1u;
    req.attached_handle = (handle_id_t)self_h;
    req.attached_rights = RIGHT_READ;

    if (sh_sys2(SYS_CHAN_SEND, (long)vfs_h, (long)&req) != IRIS_OK) {
        console_write(con, "cat: open send failed\r\n");
        sh_sys1(SYS_HANDLE_CLOSE, self_h);
        return;
    }
    /* self_h was moved into the channel on send */

    sh_msg_zero(&reply);
    if (sh_sys2(SYS_CHAN_RECV, (long)vfs_reply_h, (long)&reply) != IRIS_OK ||
        reply.type != VFS_MSG_OPEN_REPLY) {
        console_write(con, "cat: open reply error\r\n");
        return;
    }
    int32_t err = (int32_t)vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_OPEN_REPLY_ERR]);
    if (err != 0) {
        console_write(con, "cat: not found\r\n");
        return;
    }
    file_id = vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_OPEN_REPLY_FILE_ID]);
    opened = 1;

    /* VFS_MSG_READ loop */
    for (;;) {
        sh_msg_zero(&req);
        req.type = VFS_MSG_READ;
        vfs_proto_write_u32(&req.data[VFS_MSG_OFF_READ_FILE_ID], file_id);
        vfs_proto_write_u32(&req.data[VFS_MSG_OFF_READ_LEN], VFS_MSG_READ_REPLY_DATA_MAX);
        req.data_len = VFS_MSG_READ_LEN;
        req.attached_handle = HANDLE_INVALID;
        req.attached_rights = RIGHT_NONE;

        if (sh_sys2(SYS_CHAN_SEND, (long)vfs_h, (long)&req) != IRIS_OK) break;

        sh_msg_zero(&reply);
        if (sh_sys2(SYS_CHAN_RECV, (long)vfs_reply_h, (long)&reply) != IRIS_OK) break;
        if (reply.type != VFS_MSG_READ_REPLY) break;

        err = (int32_t)vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_READ_REPLY_ERR]);
        if (err != 0) break;

        uint32_t rlen = vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_READ_REPLY_LEN]);
        if (rlen == 0) break;  /* EOF */

        /* Copy bytes into null-terminated buffer and write to console. */
        char buf[VFS_MSG_READ_REPLY_DATA_MAX + 1u];
        uint32_t bi;
        for (bi = 0; bi < rlen && bi < VFS_MSG_READ_REPLY_DATA_MAX; bi++)
            buf[bi] = (char)reply.data[VFS_MSG_OFF_READ_REPLY_DATA + bi];
        buf[bi] = '\0';
        console_write(con, buf);
    }

    /* VFS_MSG_CLOSE */
    if (opened) {
        sh_msg_zero(&req);
        req.type = VFS_MSG_CLOSE;
        vfs_proto_write_u32(&req.data[VFS_MSG_OFF_CLOSE_FILE_ID], file_id);
        req.data_len = VFS_MSG_CLOSE_LEN;
        req.attached_handle = HANDLE_INVALID;
        req.attached_rights = RIGHT_NONE;
        (void)sh_sys2(SYS_CHAN_SEND, (long)vfs_h, (long)&req);
        sh_msg_zero(&reply);
        (void)sh_sys2(SYS_CHAN_RECV, (long)vfs_reply_h, (long)&reply);
    }
}

/* ── Command dispatch ────────────────────────────────────────────── */

static void sh_dispatch(handle_id_t con, handle_id_t vfs_h, handle_id_t vfs_reply_h,
                        const char *line) {
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
        console_write(con, "IRIS Phase 45 — pure microkernel shell\r\n"
                           "  kernel:   x86_64 ring-0/3, cooperative+preemptive\r\n"
                           "  services: init svcmgr kbd vfs console fb sh\r\n"
                           "  syscalls: SYS_CHAN_RECV_TIMEOUT(63) SYS_NOTIFY_WAIT_TIMEOUT(64)\r\n");
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
        if (vfs_h == HANDLE_INVALID || vfs_reply_h == HANDLE_INVALID) {
            console_write(con, "ls: VFS unavailable\r\n");
            return;
        }
        sh_cmd_ls(con, vfs_h, vfs_reply_h);
        return;
    }
    if (sh_word_eq(line, "cat")) {
        const char *path = sh_skip_word(line);
        if (*path == '\0') {
            console_write(con, "usage: cat <filename>\r\n");
            return;
        }
        if (vfs_h == HANDLE_INVALID || vfs_reply_h == HANDLE_INVALID) {
            console_write(con, "cat: VFS unavailable\r\n");
            return;
        }
        sh_cmd_cat(con, vfs_h, vfs_reply_h, path);
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
    handle_id_t kbd_service_h = HANDLE_INVALID;
    handle_id_t vfs_h         = HANDLE_INVALID;
    handle_id_t vfs_reply_h   = HANDLE_INVALID;
    handle_id_t kbd_sub_h     = HANDLE_INVALID;

    /* Bootstrap: receive channels from svcmgr. */
    uint32_t recv_count;
    for (recv_count = 0; recv_count < 16u; recv_count++) {
        struct KChanMsg msg;
        sh_msg_zero(&msg);
        if (sh_sys2(SYS_CHAN_RECV, (long)bootstrap_h, (long)&msg) != IRIS_OK)
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
        case SVCMGR_BOOTSTRAP_KIND_KBD_CAP:         /* 9 */
            if (h != HANDLE_INVALID && kbd_service_h == HANDLE_INVALID)
                kbd_service_h = h;
            else if (h != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)h);
            break;
        case SVCMGR_BOOTSTRAP_KIND_VFS_CAP:         /* 10 */
            if (h != HANDLE_INVALID && vfs_h == HANDLE_INVALID)
                vfs_h = h;
            else if (h != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)h);
            break;
        case SVCMGR_BOOTSTRAP_KIND_VFS_REPLY_CAP:   /* 11 */
            if (h != HANDLE_INVALID && vfs_reply_h == HANDLE_INVALID)
                vfs_reply_h = h;
            else if (h != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)h);
            break;
        default:
            if (h != HANDLE_INVALID)
                (void)sh_sys1(SYS_HANDLE_CLOSE, (long)h);
            break;
        }

        if (console_h != HANDLE_INVALID && kbd_service_h != HANDLE_INVALID &&
            vfs_h != HANDLE_INVALID && vfs_reply_h != HANDLE_INVALID)
            break;
    }
    (void)sh_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_h);

    if (console_h == HANDLE_INVALID) return;

    console_write(console_h, "[SH] boot\n");

    /* Subscribe to keyboard scancode events. */
    if (kbd_service_h != HANDLE_INVALID) {
        long base_h = sh_sys0(SYS_CHAN_CREATE);
        if (base_h >= 0) {
            long sub_read  = sh_sys2(SYS_HANDLE_DUP, base_h, (long)(RIGHT_READ));
            long sub_write = sh_sys2(SYS_HANDLE_DUP, base_h,
                                     (long)(RIGHT_WRITE | RIGHT_TRANSFER));
            (void)sh_sys1(SYS_HANDLE_CLOSE, base_h);

            if (sub_read >= 0 && sub_write >= 0) {
                struct KChanMsg smsg;
                sh_msg_zero(&smsg);
                smsg.type = KBD_MSG_SUBSCRIBE;
                smsg.data_len = KBD_MSG_SUBSCRIBE_LEN;
                smsg.attached_handle = (handle_id_t)sub_write;
                smsg.attached_rights = RIGHT_WRITE;
                if (sh_sys2(SYS_CHAN_SEND, (long)kbd_service_h, (long)&smsg) == IRIS_OK) {
                    kbd_sub_h = (handle_id_t)sub_read;
                    sub_read = -1;  /* retained locally; don't close */
                }
            }
            if (sub_read >= 0)
                (void)sh_sys1(SYS_HANDLE_CLOSE, sub_read);
            /* sub_write was moved into the channel message; do not close it */
        }
        (void)sh_sys1(SYS_HANDLE_CLOSE, (long)kbd_service_h);
    }

    /* Print banner */
    console_write(console_h,
        "\r\n"
        "IRIS shell (Phase 45) — 'help' for commands\r\n"
        "> ");

    /* REPL main loop */
    char line[128];
    uint32_t line_len = 0;
    uint8_t shift = 0;

    for (;;) {
        if (kbd_sub_h == HANDLE_INVALID) {
            (void)sh_sys0(SYS_YIELD);
            continue;
        }

        struct KChanMsg msg;
        sh_msg_zero(&msg);
        if (sh_sys2(SYS_CHAN_RECV, (long)kbd_sub_h, (long)&msg) != IRIS_OK)
            continue;
        if (msg.type != KBD_MSG_SCANCODE_EVENT || msg.data_len < 1u)
            continue;

        uint8_t sc      = msg.data[KBD_MSG_OFF_SC_EVENT_CODE];
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
                sh_dispatch(console_h, vfs_h, vfs_reply_h, line);
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
