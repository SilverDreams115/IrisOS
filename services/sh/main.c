/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sh/main.c — ring-3 interactive shell service.
 *
 * Bootstrap protocol (over bootstrap channel from svcmgr):
 *   recv SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP (6) → console_h  (RIGHT_WRITE)
 *   recv SVCMGR_ENDPOINT_SH        (7)        → own service_h (closed; unused)
 *   recv SVCMGR_ENDPOINT_SH_REPLY  (8)        → own reply_h  (closed; unused)
 *   recv SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP (0x20) → svcmgr discovery endpoint
 *
 * VFS access: endpoint-only. "vfs.ep" is resolved through the
 * svcmgr discovery endpoint; ls/cat use the stateless VFS EP protocol
 * (iris/vfs_ep_proto.h). There is no retired KChannel fallback — if the
 * endpoint is missing, ls/cat report the error instead of masking it.
 *
 * Keyboard: endpoint-only. "kbd.ep" is resolved through the
 * svcmgr discovery endpoint; the REPL pulls one key event per
 * EP_CALL(KBD_EP_OP_READ) — kbd parks the reply until a key arrives, so the
 * call doubles as the blocking wait. No retired KChannel subscribe fallback.
 *
 * Commands: help, ver, uptime, ls, cat <file>, run <prog>, clear
 */

#include <stdint.h>
#include "../common/iris_msg.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/svcmgr_proto.h>
#include <iris/kbd_ep_proto.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include "../timer/timer_proto.h"
#include <iris/vfs_ep_proto.h>
#include <iris/program_abi.h>
#include "../common/console_client.h"
#include "../common/iris_ipc_buffer.h"

/* ── Syscall helpers ─────────────────────────────────────────────── */

static inline long sh_sys3(long nr, long a0, long a1, long a2) {
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)0);
}
static inline long sh_sys0(long nr)                    { return sh_sys3(nr, 0, 0, 0); }

static void sh_imsg_zero(struct iris_msg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

/* VFS endpoint handle. Resolved once
 * after bootstrap via the svcmgr discovery endpoint; IRIS_CPTR_NULL means
 * VFS is unavailable — ls/cat fail loudly, there is no old fallback. */
static iris_cptr_t g_sh_vfs_ep_h = IRIS_CPTR_NULL;

/*
 * IPC bulk buffer for EP_CALL round trips (request payload and reply data
 * share the buffer — EP_CALL reuses buf_uptr in both directions).
 *
 * `g_sh_buf` starts at this static fallback and moves to a page sh
 * retypes from the Untyped it owns, registered as its IPC buffer.  Sharing one
 * page between the request and the reply is not a compromise here — it is what
 * an IPC buffer IS, and sh's call/reply pattern already worked that way.
 */
static uint8_t  g_sh_ep_buf[VFS_EP_DATA_MAX + 1u];
static uint8_t *g_sh_buf = g_sh_ep_buf;

/* Spare slots from the per-service range (22..29 are unassigned). */
#define SH_SLOT_IPCBUF_FRAME  22u
#define SH_SLOT_IPCBUF_PT     23u
/* ...and what `run` needs: the program spawner, and the THREAD of whatever it
 * last started.  A supervisor names the thread — there has been no process
 * object since Stage 7 — and the exit status is read off it. */
#define SH_SLOT_PROC_EP       24u
#define SH_SLOT_PROC_TCB      25u

/* Console endpoint path: sh is a pure CPtr-first client — ALL
 * console output goes through the well-known slot IRIS_CPTR_CONSOLE_EP.
 * There is no retired console cap anymore: if the slot is broken, sh stays
 * silent and every gated "[SH] ... OK" marker is missing, which fails the
 * smoke run. The `con` parameter is kept so call sites stay unchanged. */
static iris_cptr_t g_sh_con_ep_h = (iris_cptr_t)IRIS_CPTR_CONSOLE_EP;
/* The console client marshals into the buffer it is given, and a thread
 * with a registered IPC buffer must marshal into THAT — the kernel refuses a
 * send that names any other address.  So the log path shares the service's one
 * IPC buffer, which is what having one buffer means. */

static void sh_cout(iris_cptr_t con, const char *s) {
    (void)con;
    (void)console_ep_write(g_sh_con_ep_h, g_sh_buf, s);
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

static void sh_write_u32(iris_cptr_t con, uint32_t v) {
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

/* ── VFS endpoint path ────────────────────────────────── */

/* (sh_svc_ep_lookup removed — sh discovers nothing at runtime;
 * every core service cap is a well-known CSpace slot.) */

/*
 * One VFS endpoint call. The path (when non-NULL) is staged into g_sh_buf;
 * reply bulk data lands in the same buffer. Returns IRIS_OK and fills *msg on
 * a served round trip (msg->label distinguishes OK from protocol error).
 */
static int sh_vfs_ep_call(struct iris_msg *msg, const char *path) {
    if (path) {
        uint32_t plen = sh_strlen(path);
        if (plen + 1u > VFS_EP_PATH_MAX) return (int)IRIS_ERR_INVALID_ARG;
        for (uint32_t i = 0; i < plen; i++) g_sh_buf[i] = (uint8_t)path[i];
        g_sh_buf[plen] = 0u;
        msg->buf_len = plen + 1u;
    }
    return (int)iris_msg_call((long)g_sh_vfs_ep_h, msg);
}

static void sh_cmd_ls_ep(iris_cptr_t con) {
    for (uint32_t idx = 0; idx < 64u; idx++) {
        struct iris_msg msg;
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
        g_sh_buf[name_len] = 0u;

        sh_cout(con, "  ");
        sh_cout(con, (const char *)g_sh_buf);
        sh_cout(con, "  (");
        sh_write_u32(con, size);
        sh_cout(con, " bytes)\r\n");
    }
}

static void sh_cmd_cat_ep(iris_cptr_t con, const char *path) {
    uint64_t offset = 0;

    for (;;) {
        struct iris_msg msg;
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

        g_sh_buf[bytes] = 0u;
        sh_cout(con, (const char *)g_sh_buf);

        offset += bytes;
        if (offset >= total) return;
    }
}

/* ── run: start a program, and read what it exited with ──────────────────── */

/*
 * The program spawner, resolved once and lazily.
 *
 * `sh` discovers nothing else at runtime — every core service it uses is a
 * well-known slot minted before it ran — and `proc.ep` is the exception for a
 * reason: `proc` is started by `init` AFTER `sh` is already up, so there was
 * nothing to mint at the time.  It is looked up through the svcmgr discovery
 * endpoint, which is the mechanism this system already has for exactly that.
 */
static iris_cptr_t g_sh_proc_ep = IRIS_CPTR_NULL;

static int sh_proc_ep_try(void) {
    static const char name[] = "spawn";
    struct iris_msg m;

    if (g_sh_proc_ep != IRIS_CPTR_NULL) return 1;

    sh_imsg_zero(&m);
    m.label = IRIS_SVCMGR_EP_LOOKUP_NAME;
    for (uint32_t i = 0; i < (uint32_t)sizeof(name); i++)
        g_sh_buf[i] = (uint8_t)name[i];
    m.buf_len = (uint32_t)sizeof(name);
    /* The receive slot has to be EMPTY, and a failed lookup transfers nothing
     * — so it is cleared before every attempt rather than after. */
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)SH_SLOT_PROC_EP);
    m.recv_slot = (long)SH_SLOT_PROC_EP;

    if (iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m) != IRIS_OK ||
        m.label != IRIS_EP_REPLY_OK)
        return 0;
    g_sh_proc_ep = (iris_cptr_t)SH_SLOT_PROC_EP;
    return 1;
}

/* What time it is, from the service that holds the clock.  0 when this shell
 * was granted no timer capability, which is a thing a spawner may choose. */
static uint64_t sh_uptime_ns(void) {
    struct iris_msg m;
    sh_imsg_zero(&m);
    m.label = TMR_OP_UPTIME;
    if (iris_msg_call((long)IRIS_CPTR_TIMER_EP, &m) != 0) return 0u;
    return m.words[0];
}

/*
 * ...and the same, waiting for it.
 *
 * `proc` is started by `init` LONG after `sh` is already up — the shell comes
 * from svcmgr early in the boot, the spawner comes after the filesystem is
 * usable — so the first lookup legitimately finds nothing.
 *
 * The bound is REAL TIME rather than a retry count, and the difference matters:
 * an iteration count is a guess about how fast this machine is, and the machine
 * this runs on is a virtual one whose speed nobody controls.  Fifteen seconds
 * is "the spawner is not coming"; anything less is this shell being impatient
 * about a boot that is still happening.  Each attempt yields, so waiting here
 * is not spinning against the service being waited for.
 */
static int sh_proc_ep(iris_cptr_t con) {
    uint64_t t0 = sh_uptime_ns();

    for (;;) {
        uint64_t now, mark;
        if (sh_proc_ep_try()) return 1;
        now = sh_uptime_ns();
        /* No clock: fall back to a count, because a shell with no timer must
         * still give up rather than wait for ever. */
        if (now == 0u || t0 == 0u) { if (++t0 > 200000u) break; continue; }
        if (now - t0 > 15000000000ULL) break;
        /*
         * And WAIT between attempts, rather than asking as fast as the CPU
         * allows.  A failed lookup is a logged warning in svcmgr, so an
         * impatient shell writes tens of thousands of them into the boot log
         * of a machine that is working correctly — which is how a real failure
         * becomes unfindable.  Twenty milliseconds is imperceptible to a
         * person and is four hundred attempts across the whole bound.
         */
        for (mark = now; ; ) {
            uint64_t t = sh_uptime_ns();
            if (t == 0u || t - mark > 20000000ULL) break;
            (void)sh_sys0(SYS_YIELD);
        }
    }
    sh_cout(con, "run: no program spawner\r\n");
    return 0;
}

static void sh_cmd_run(iris_cptr_t con, const char *path) {
    struct iris_msg m;
    uint32_t n;
    long ec = -1;

    if (!sh_proc_ep(con)) return;

    n = sh_strlen(path);
    if (n == 0u || n + 1u > VFS_EP_PATH_MAX) {
        sh_cout(con, "run: bad path\r\n");
        return;
    }
    for (uint32_t i = 0; i < n; i++) g_sh_buf[i] = (uint8_t)path[i];
    g_sh_buf[n] = 0u;

    sh_imsg_zero(&m);
    m.label      = PROC_OP_SPAWN;
    m.words[0]   = 0u;            /* the default budget                      */
    m.words[1]   = 0u;            /* no object preloaded: the image says what
                                   * interpreter it wants, and `proc` resolves
                                   * that itself                             */
    m.word_count = 2u;
    m.buf_len    = n + 1u;        /* the argument vector: just argv[0]       */
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)SH_SLOT_PROC_TCB);
    m.recv_slot  = (long)SH_SLOT_PROC_TCB;

    if (iris_msg_call((long)g_sh_proc_ep, &m) != IRIS_OK) {
        sh_cout(con, "run: spawner did not answer\r\n");
        return;
    }
    if (m.label != PROC_REP_OK) {
        /* The step number IS the diagnosis — see PROC_STEP_* — so it is shown
         * rather than folded into "failed". */
        sh_cout(con, "run: did not start, step ");
        sh_write_u32(con, (uint32_t)m.words[0]);
        sh_cout(con, "\r\n");
        return;
    }

    /*
     * Wait for it, bounded, by ASKING the thread.
     *
     * A terminal thread answers its exit code and a live one answers an error,
     * so polling says the same thing a notification would with two fewer
     * objects — and each ask is a syscall, which is a scheduling point, so the
     * program it is waiting for gets the CPU.  The bound is here because a
     * shell that hangs on a program that hangs is a shell somebody has to
     * reboot to get back.
     */
    for (uint32_t spin = 0; spin < 2000000u; spin++) {
        ec = iris_invoke0((long)SH_SLOT_PROC_TCB, INV_TCB_EXIT_CODE);
        if (ec >= 0) break;
    }
    if (ec < 0) {
        sh_cout(con, "run: still running, gave up waiting\r\n");
    } else {
        sh_cout(con, "run: ");
        sh_cout(con, path);
        sh_cout(con, " exited ");
        sh_write_u32(con, (uint32_t)ec);
        sh_cout(con, "\r\n");
    }
    /* Its thread goes with it: while `sh` holds one, the budget the program was
     * carved from cannot be reset and the spawner's next child costs a fresh
     * region.  A supervisor holds nothing of a child it has finished with. */
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)SH_SLOT_PROC_TCB);
}

/* ── Command dispatch ────────────────────────────────────────────── */

static void sh_dispatch(iris_cptr_t con, const char *line) {
    if (sh_word_eq(line, "help")) {
        sh_cout(con, "Commands:\r\n"
                           "  help          this message\r\n"
                           "  ver           version info\r\n"
                           "  uptime        seconds since boot\r\n"
                           "  ls            list VFS files\r\n"
                           "  cat <file>    read a file\r\n"
                           "  run <prog>    start a program, print its status\r\n"
                           "  clear         clear screen\r\n");
        return;
    }
    if (sh_word_eq(line, "ver")) {
        sh_cout(con, "IRIS — pure microkernel shell\r\n"
                           "  kernel:   x86_64 ring-0/3, cooperative+preemptive\r\n"
                           "  services: init svcmgr kbd vfs console fb sh\r\n"
                           "  time:     a ring-3 service; the kernel cannot block on it\r\n");
        return;
    }
    if (sh_word_eq(line, "uptime")) {
        /*
         * The clock is a SERVICE.  `SYS_CLOCK_GET` handed any task
         * a timestamp for the asking; what time it is, is something you are
         * told by whoever holds the hardware, and a shell that was not granted
         * the timer capability says so rather than being told anyway.
         */
        struct iris_msg m;
        uint8_t *b = (uint8_t *)&m;
        for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) b[i] = 0;
        m.label = TMR_OP_UPTIME;
        long r = iris_msg_call((long)IRIS_CPTR_TIMER_EP, &m);
        if (r != 0) {
            sh_cout(con, "uptime: no clock granted\r\n");
        } else {
            uint64_t secs = m.words[0] / 1000000000ULL;
            sh_cout(con, "uptime: ");
            sh_write_u32(con, (uint32_t)secs);
            sh_cout(con, " s\r\n");
        }
        return;
    }
    if (sh_word_eq(line, "ls")) {
        /* Endpoint-only path: no retired KChannel fallback. */
        if (g_sh_vfs_ep_h == IRIS_CPTR_NULL) {
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
        if (g_sh_vfs_ep_h == IRIS_CPTR_NULL) {
            sh_cout(con, "cat: VFS endpoint unavailable\r\n");
            return;
        }
        sh_cmd_cat_ep(con, path);
        sh_cout(con, "\r\n");
        return;
    }
    if (sh_word_eq(line, "run")) {
        const char *path = sh_skip_word(line);
        if (*path == '\0') {
            sh_cout(con, "usage: run <program>\r\n");
            return;
        }
        sh_cmd_run(con, path);
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

void sh_main_c(iris_cptr_t rbx_unused) {
    iris_cptr_t console_h     = IRIS_CPTR_NULL;  /* unused: pure CPtr client */
    iris_cptr_t kbd_ep_h      = IRIS_CPTR_NULL;

    /* A page sh owns, registered as its IPC buffer.  Best-effort — a
     * failure leaves the kernel staging path, which still works. */
    {
        void *b = iris_ipc_buffer_init(SH_SLOT_IPCBUF_FRAME, SH_SLOT_IPCBUF_PT,
                                       IRIS_IPC_BUFFER_VA);
        if (b) g_sh_buf = (uint8_t *)b;
    }

    /* Sh is a pure CPtr-first client. The bootstrap bag is empty
     * (catalog: endpoint_only without an own endpoint) — everything sh
     * needs was minted into its root CNode before it ran:
     *   slot 1 svcmgr discovery, slot 2 vfs.ep, slot 3 console.ep,
     *   slot 4 kbd.ep.  Close the (empty) bootstrap channel and verify
     * each slot with a PING; every marker below is a smoke gate, so a
     * broken slot cannot hide.  No lookup, no handle fallback. */
    (void)rbx_unused;   /* RBX = 0 since the KChannel bootstrap retired */

    sh_cout(console_h, "[SH] boot\n");

    {
        struct iris_msg pmsg;
        sh_imsg_zero(&pmsg);
        pmsg.label = IRIS_EP_OP_PING;
        if (iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &pmsg) == IRIS_OK &&
            pmsg.label == IRIS_EP_REPLY_OK)
            sh_cout(console_h, "[SH] svcmgr cptr OK\n");
        else
            sh_cout(console_h, "[SH] svcmgr cptr FAILED\n");

        sh_imsg_zero(&pmsg);
        pmsg.label = IRIS_EP_OP_PING;
        if (iris_msg_call((long)IRIS_CPTR_VFS_EP, &pmsg) == IRIS_OK &&
            pmsg.label == IRIS_EP_REPLY_OK) {
            g_sh_vfs_ep_h = (iris_cptr_t)IRIS_CPTR_VFS_EP;
            sh_cout(console_h, "[SH] vfs cptr OK\n");
        } else {
            sh_cout(console_h, "[SH] vfs cptr FAILED\n");
        }

        sh_imsg_zero(&pmsg);
        pmsg.label = IRIS_EP_OP_PING;
        if (iris_msg_call((long)IRIS_CPTR_KBD_EP, &pmsg) == IRIS_OK &&
            pmsg.label == IRIS_EP_REPLY_OK) {
            kbd_ep_h = (iris_cptr_t)IRIS_CPTR_KBD_EP;
            sh_cout(console_h, "[SH] kbd cptr OK\n");
        } else {
            sh_cout(console_h, "[SH] kbd cptr FAILED\n");
        }

        /* console: every sh_cout above already exercised slot 3; PING for
         * the symmetric gated marker. */
        sh_imsg_zero(&pmsg);
        pmsg.label = IRIS_EP_OP_PING;
        if (iris_msg_call((long)IRIS_CPTR_CONSOLE_EP, &pmsg) == IRIS_OK &&
            pmsg.label == IRIS_EP_REPLY_OK)
            sh_cout(console_h, "[SH] console cptr OK\n");
        else
            sh_cout(console_h, "[SH] console cptr FAILED\n");
    }

    /*
     * ── run one program, before anybody types ──
     *
     * Stage 10-run step 6 closes on "a C program ... exits with a status the
     * SHELL reads — in the gate, on every commit", and a gate has no hands.
     * So the shell does once, at startup, exactly what `run cprog` does when a
     * person types it: the same lookup, the same spawn, the same read of the
     * same thread's exit status.  It is a demonstration that runs itself, not
     * a special path — delete these four lines and the command still works.
     */
    sh_cmd_run(console_h, "cprog");

    /* Print banner */
    sh_cout(console_h,
        "\r\n"
        "IRIS shell — 'help' for commands\r\n"
        "> ");

    /* REPL main loop */
    char line[128];
    uint32_t line_len = 0;
    uint8_t shift = 0;

    for (;;) {
        if (kbd_ep_h == IRIS_CPTR_NULL) {
            (void)sh_sys0(SYS_YIELD);
            continue;
        }

        /* Blocking pull: kbd parks the reply until a key event arrives.
         * WOULD_BLOCK (park slot taken by a concurrent caller) and call
         * errors yield-and-retry; this never spins on an immediate reply. */
        struct iris_msg msg;
        sh_imsg_zero(&msg);
        msg.label = KBD_EP_OP_READ;
        if (iris_msg_call((long)kbd_ep_h, &msg) != IRIS_OK) {
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
