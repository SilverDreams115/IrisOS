/*
 * main.c — iris_test ring-3 syscall test suite (Block 8, Ph88-92).
 *
 * Spawned by init after the healthy path is established. Outputs per-test
 * results to COM1 serial. Exits 0 on full pass, 1 on any failure.
 * Final marker checked by smoke-runtime: [IRIS][TEST] SUITE PASS N/N
 */

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/error.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/ipc_msg.h>

/* ── Syscall helpers ────────────────────────────────────────────────────── */

static inline long it_sys0(long nr) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long it_sys1(long nr, long a0) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long it_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long it_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

/* ── Serial output ──────────────────────────────────────────────────────── */

static handle_id_t g_serial_h = HANDLE_INVALID;

static void it_serial_write(const char *s) {
    if (g_serial_h == HANDLE_INVALID || !s) return;
    while (*s) {
        long v;
        do {
            v = it_sys2(SYS_IOPORT_IN, (long)g_serial_h, 5);
        } while (v < 0 || !((uint8_t)v & 0x20u));
        (void)it_sys3(SYS_IOPORT_OUT, (long)g_serial_h, 0, (long)(uint8_t)*s++);
    }
}

static void it_log_num(uint32_t n) {
    char buf[12];
    uint32_t i = 11;
    buf[i] = '\0';
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n > 0) {
            buf[--i] = (char)('0' + (int)(n % 10u));
            n /= 10u;
        }
    }
    it_serial_write(buf + i);
}

/* ── Test framework ─────────────────────────────────────────────────────── */

static uint32_t g_pass  = 0;
static uint32_t g_total = 0;

static void it_pass(const char *id) {
    g_pass++;
    g_total++;
    it_serial_write("[IRIS][TEST] ");
    it_serial_write(id);
    it_serial_write(" ok\n");
}

static void it_fail(const char *id, const char *reason) {
    g_total++;
    it_serial_write("[IRIS][TEST] ");
    it_serial_write(id);
    it_serial_write(" FAIL: ");
    it_serial_write(reason);
    it_serial_write("\n");
}

/* ── Message helpers ────────────────────────────────────────────────────── */

static void it_chan_msg_zero(struct KChanMsg *m) {
    uint8_t *p = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) p[i] = 0;
}

static void it_iris_msg_zero(struct IrisMsg *m) {
    uint8_t *p = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) p[i] = 0;
}

static void it_close(handle_id_t *h) {
    if (*h != HANDLE_INVALID) {
        it_sys1(SYS_HANDLE_CLOSE, (long)*h);
        *h = HANDLE_INVALID;
    }
}

/* ── T001: SYS_GETPID ───────────────────────────────────────────────────── */

static void test_t001(void) {
    long r = it_sys0(SYS_GETPID);
    if (r >= 0)
        it_pass("T001");
    else
        it_fail("T001", "getpid negative");
}

/* ── T002: SYS_CLOCK_GET ────────────────────────────────────────────────── */

static void test_t002(void) {
    long r = it_sys0(SYS_CLOCK_GET);
    if (r >= 0)
        it_pass("T002");
    else
        it_fail("T002", "clock_get negative");
}

/* ── T003: SYS_YIELD ────────────────────────────────────────────────────── */

static void test_t003(void) {
    long r = it_sys0(SYS_YIELD);
    if (r == 0)
        it_pass("T003");
    else
        it_fail("T003", "yield non-zero");
}

/* ── T004: KChannel loopback ────────────────────────────────────────────── */

static void test_t004(void) {
    long ch_raw = it_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { it_fail("T004", "chan create"); return; }
    handle_id_t ch_h = (handle_id_t)ch_raw;

    long rd_raw = it_sys2(SYS_HANDLE_DUP, ch_raw, (long)RIGHT_READ);
    if (rd_raw < 0) {
        it_close(&ch_h);
        it_fail("T004", "rd dup"); return;
    }
    handle_id_t rd_h = (handle_id_t)rd_raw;

    struct KChanMsg msg;
    it_chan_msg_zero(&msg);
    msg.type     = 0xA5A5u;
    msg.data[0]  = 0x42u;
    msg.data_len = 1u;
    msg.attached_handle = HANDLE_INVALID;

    long r = it_sys2(SYS_CHAN_SEND, ch_raw, (long)&msg);
    if (r < 0) {
        it_close(&rd_h);
        it_close(&ch_h);
        it_fail("T004", "send"); return;
    }

    it_chan_msg_zero(&msg);
    r = it_sys2(SYS_CHAN_RECV, rd_raw, (long)&msg);

    it_close(&rd_h);
    it_close(&ch_h);

    if (r < 0 || msg.type != 0xA5A5u || msg.data[0] != 0x42u)
        it_fail("T004", "recv mismatch");
    else
        it_pass("T004");
}

/* ── T005: CHAN_RECV_NB on empty ────────────────────────────────────────── */

static void test_t005(void) {
    long ch_raw = it_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { it_fail("T005", "chan create"); return; }
    handle_id_t ch_h = (handle_id_t)ch_raw;

    long rd_raw = it_sys2(SYS_HANDLE_DUP, ch_raw, (long)RIGHT_READ);
    if (rd_raw < 0) {
        it_close(&ch_h);
        it_fail("T005", "rd dup"); return;
    }
    handle_id_t rd_h = (handle_id_t)rd_raw;

    struct KChanMsg msg;
    it_chan_msg_zero(&msg);
    long r = it_sys2(SYS_CHAN_RECV_NB, rd_raw, (long)&msg);

    it_close(&rd_h);
    it_close(&ch_h);

    if (r == (long)IRIS_ERR_WOULD_BLOCK)
        it_pass("T005");
    else
        it_fail("T005", "expected WOULD_BLOCK");
}

/* ── T006: CHAN_RECV_TIMEOUT → TIMED_OUT ────────────────────────────────── */

static void test_t006(void) {
    long ch_raw = it_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { it_fail("T006", "chan create"); return; }
    handle_id_t ch_h = (handle_id_t)ch_raw;

    long rd_raw = it_sys2(SYS_HANDLE_DUP, ch_raw, (long)RIGHT_READ);
    if (rd_raw < 0) {
        it_close(&ch_h);
        it_fail("T006", "rd dup"); return;
    }
    handle_id_t rd_h = (handle_id_t)rd_raw;

    struct KChanMsg msg;
    it_chan_msg_zero(&msg);
    /* 50 ms timeout on an empty channel */
    long r = it_sys3(SYS_CHAN_RECV_TIMEOUT, rd_raw, (long)&msg, 50000000L);

    it_close(&rd_h);
    it_close(&ch_h);

    if (r == (long)IRIS_ERR_TIMED_OUT)
        it_pass("T006");
    else
        it_fail("T006", "expected TIMED_OUT");
}

/* ── T007: CHAN_SEAL semantics ──────────────────────────────────────────── */

static void test_t007(void) {
    long ch_raw = it_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { it_fail("T007", "chan create"); return; }
    handle_id_t ch_h = (handle_id_t)ch_raw;

    long rd_raw = it_sys2(SYS_HANDLE_DUP, ch_raw, (long)RIGHT_READ);
    if (rd_raw < 0) {
        it_close(&ch_h);
        it_fail("T007", "rd dup"); return;
    }
    handle_id_t rd_h = (handle_id_t)rd_raw;

    /* Send one message then seal */
    struct KChanMsg msg;
    it_chan_msg_zero(&msg);
    msg.type     = 0x1234u;
    msg.data_len = 0u;
    msg.attached_handle = HANDLE_INVALID;
    long r = it_sys2(SYS_CHAN_SEND, ch_raw, (long)&msg);
    if (r < 0) {
        it_close(&rd_h);
        it_close(&ch_h);
        it_fail("T007", "send"); return;
    }

    (void)it_sys1(SYS_CHAN_SEAL, ch_raw);

    /* Second send must fail */
    it_chan_msg_zero(&msg);
    msg.type     = 0x5678u;
    msg.data_len = 0u;
    msg.attached_handle = HANDLE_INVALID;
    long r2 = it_sys2(SYS_CHAN_SEND, ch_raw, (long)&msg);
    if (r2 >= 0) {
        it_close(&rd_h);
        it_close(&ch_h);
        it_fail("T007", "send-after-seal"); return;
    }

    /* Drain the original message */
    it_chan_msg_zero(&msg);
    long r3 = it_sys2(SYS_CHAN_RECV, rd_raw, (long)&msg);
    if (r3 < 0 || msg.type != 0x1234u) {
        it_close(&rd_h);
        it_close(&ch_h);
        it_fail("T007", "drain recv"); return;
    }

    /* Recv on empty sealed channel must return CLOSED */
    it_chan_msg_zero(&msg);
    long r4 = it_sys2(SYS_CHAN_RECV_NB, rd_raw, (long)&msg);

    it_close(&rd_h);
    it_close(&ch_h);

    if (r4 == (long)IRIS_ERR_CLOSED)
        it_pass("T007");
    else
        it_fail("T007", "sealed empty not CLOSED");
}

/* ── T008: VMO create + map + rw + unmap ────────────────────────────────── */

#define T008_VMO_ADDR  0x8050000000ULL
#define T008_VMO_SIZE  4096U

static void test_t008(void) {
    long vmo_raw = it_sys1(SYS_VMO_CREATE, T008_VMO_SIZE);
    if (vmo_raw < 0) { it_fail("T008", "vmo create"); return; }
    handle_id_t vmo_h = (handle_id_t)vmo_raw;

    /* Map writable (flag=1) at T008_VMO_ADDR */
    long r = it_sys3(SYS_VMO_MAP, vmo_raw, (long)T008_VMO_ADDR, 1);
    if (r < 0) {
        it_close(&vmo_h);
        it_fail("T008", "vmo map"); return;
    }

    /* Write and read back via the mapped address */
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)T008_VMO_ADDR;
    *p = 0xDEADBEEFCAFEBABEULL;
    uint64_t readback = *p;

    /* Verify VMO size */
    long sz = it_sys1(SYS_VMO_SIZE, vmo_raw);

    /* Unmap */
    long ur = it_sys2(SYS_VMO_UNMAP, (long)T008_VMO_ADDR, T008_VMO_SIZE);

    it_close(&vmo_h);

    if (readback != 0xDEADBEEFCAFEBABEULL)
        it_fail("T008", "readback mismatch");
    else if (sz < 0 || (uint32_t)sz < T008_VMO_SIZE)
        it_fail("T008", "size wrong");
    else if (ur < 0)
        it_fail("T008", "unmap failed");
    else
        it_pass("T008");
}

/* ── T009: NOTIFY_SIGNAL then NOTIFY_WAIT (pre-signalled) ──────────────── */

static void test_t009(void) {
    long n_raw = it_sys0(SYS_NOTIFY_CREATE);
    if (n_raw < 0) { it_fail("T009", "notify create"); return; }
    handle_id_t n_h = (handle_id_t)n_raw;

    long r = it_sys2(SYS_NOTIFY_SIGNAL, n_raw, 0x3u);
    if (r < 0) {
        it_close(&n_h);
        it_fail("T009", "notify signal"); return;
    }

    uint64_t out_bits = 0;
    r = it_sys2(SYS_NOTIFY_WAIT, n_raw, (long)(uintptr_t)&out_bits);

    it_close(&n_h);

    if (r == 0 && out_bits == 0x3u)
        it_pass("T009");
    else
        it_fail("T009", "bits mismatch");
}

/* ── T010: NOTIFY_WAIT_TIMEOUT → TIMED_OUT ─────────────────────────────── */

static void test_t010(void) {
    long n_raw = it_sys0(SYS_NOTIFY_CREATE);
    if (n_raw < 0) { it_fail("T010", "notify create"); return; }
    handle_id_t n_h = (handle_id_t)n_raw;

    uint64_t out_bits = 0;
    long r = it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, n_raw,
                     (long)(uintptr_t)&out_bits, 50000000L);

    it_close(&n_h);

    if (r == (long)IRIS_ERR_TIMED_OUT)
        it_pass("T010");
    else
        it_fail("T010", "expected TIMED_OUT");
}

/* ── T011: HANDLE_TYPE on channel ───────────────────────────────────────── */

static void test_t011(void) {
    long ch_raw = it_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { it_fail("T011", "chan create"); return; }
    handle_id_t ch_h = (handle_id_t)ch_raw;

    long ty = it_sys1(SYS_HANDLE_TYPE, ch_raw);

    it_close(&ch_h);

    if (ty == (long)IRIS_HANDLE_TYPE_CHANNEL)
        it_pass("T011");
    else
        it_fail("T011", "wrong type");
}

/* ── T012: HANDLE_SAME_OBJECT ───────────────────────────────────────────── */

static void test_t012(void) {
    long ch_raw = it_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { it_fail("T012", "chan create"); return; }
    handle_id_t ch_h = (handle_id_t)ch_raw;

    long dup_raw = it_sys2(SYS_HANDLE_DUP, ch_raw,
                           (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));
    if (dup_raw < 0) {
        it_close(&ch_h);
        it_fail("T012", "dup"); return;
    }
    handle_id_t dup_h = (handle_id_t)dup_raw;

    long ch2_raw = it_sys0(SYS_CHAN_CREATE);
    if (ch2_raw < 0) {
        it_close(&dup_h);
        it_close(&ch_h);
        it_fail("T012", "ch2 create"); return;
    }
    handle_id_t ch2_h = (handle_id_t)ch2_raw;

    /* ch_h and dup_h → same object */
    long same = it_sys2(SYS_HANDLE_SAME_OBJECT, ch_raw, dup_raw);
    /* ch_h and ch2_h → different objects */
    long diff = it_sys2(SYS_HANDLE_SAME_OBJECT, ch_raw, ch2_raw);

    it_close(&ch2_h);
    it_close(&dup_h);
    it_close(&ch_h);

    if (same == 1 && diff == 0)
        it_pass("T012");
    else
        it_fail("T012", "same-obj wrong");
}

/* ── T013: Rights enforcement (no WRITE → send fails) ──────────────────── */

static void test_t013(void) {
    long ch_raw = it_sys0(SYS_CHAN_CREATE);
    if (ch_raw < 0) { it_fail("T013", "chan create"); return; }
    handle_id_t ch_h = (handle_id_t)ch_raw;

    /* Dup with READ-only (no WRITE right) */
    long ro_raw = it_sys2(SYS_HANDLE_DUP, ch_raw, (long)RIGHT_READ);
    if (ro_raw < 0) {
        it_close(&ch_h);
        it_fail("T013", "ro dup"); return;
    }
    handle_id_t ro_h = (handle_id_t)ro_raw;

    struct KChanMsg msg;
    it_chan_msg_zero(&msg);
    msg.type     = 0xFFFFu;
    msg.data_len = 0u;
    msg.attached_handle = HANDLE_INVALID;
    /* Send on read-only handle must fail */
    long r = it_sys2(SYS_CHAN_SEND, ro_raw, (long)&msg);

    it_close(&ro_h);
    it_close(&ch_h);

    if (r == (long)IRIS_ERR_ACCESS_DENIED)
        it_pass("T013");
    else
        it_fail("T013", "expected ACCESS_DENIED");
}

/* ── T014: EP_NB_RECV on empty endpoint ─────────────────────────────────── */

static void test_t014(void) {
    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T014", "ep create"); return; }
    handle_id_t ep_h = (handle_id_t)ep_raw;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys2(SYS_EP_NB_RECV, ep_raw, (long)&msg);

    it_close(&ep_h);

    if (r == (long)IRIS_ERR_WOULD_BLOCK)
        it_pass("T014");
    else
        it_fail("T014", "expected WOULD_BLOCK");
}

/* ── T015: EP_SEND / EP_RECV (two-thread rendezvous) ────────────────────── */

static handle_id_t g_t015_ep_h   = HANDLE_INVALID;
static volatile int g_t015_done  = 0;
static          int g_t015_ok    = 0;
static uint8_t      g_t015_stack[8192];

static void t015_server(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys2(SYS_EP_RECV, (long)g_t015_ep_h, (long)&msg);
    g_t015_ok   = (r == 0 && msg.label == 0xC0FFEEULL);
    g_t015_done = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t015(void) {
    g_t015_done = 0;
    g_t015_ok   = 0;

    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T015", "ep create"); return; }
    g_t015_ep_h = (handle_id_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t015_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t015_stack + sizeof(g_t015_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) {
        it_close(&g_t015_ep_h);
        it_fail("T015", "thread create"); return;
    }
    handle_id_t tid_h = (handle_id_t)tid;

    /* EP_SEND blocks until server is ready to rendezvous */
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label      = 0xC0FFEEULL;
    msg.word_count = 0;
    long r = it_sys2(SYS_EP_SEND, ep_raw, (long)&msg);

    /* Poll for server to set done flag (it runs after rendezvous returns) */
    for (int i = 0; i < 200 && !g_t015_done; i++)
        it_sys1(SYS_SLEEP, 1);

    it_close(&tid_h);
    it_close(&g_t015_ep_h);

    if (r == 0 && g_t015_ok)
        it_pass("T015");
    else
        it_fail("T015", "ep rendezvous");
}

/* ── T016: EP_CALL / SYS_REPLY (two-thread) ─────────────────────────────── */

static handle_id_t g_t016_ep_h   = HANDLE_INVALID;
static volatile int g_t016_done  = 0;
static          int g_t016_ok    = 0;
static uint8_t      g_t016_stack[8192];

static void t016_server(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys2(SYS_EP_RECV, (long)g_t016_ep_h, (long)&msg);
    if (r < 0 || msg.attached_handle == IRIS_MSG_NO_CAP) {
        g_t016_done = 1;
        it_sys1(SYS_THREAD_EXIT, 0);
        for (;;) {}
    }

    handle_id_t reply_h = (handle_id_t)msg.attached_handle;
    struct IrisMsg reply;
    it_iris_msg_zero(&reply);
    reply.label      = 0xFEEDBEEFULL;
    reply.word_count = 0;
    long rr = it_sys2(SYS_REPLY, (long)reply_h, (long)&reply);
    g_t016_ok   = (rr == 0);
    it_sys1(SYS_HANDLE_CLOSE, (long)reply_h);
    g_t016_done = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t016(void) {
    g_t016_done = 0;
    g_t016_ok   = 0;

    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T016", "ep create"); return; }
    g_t016_ep_h = (handle_id_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t016_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t016_stack + sizeof(g_t016_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) {
        it_close(&g_t016_ep_h);
        it_fail("T016", "thread create"); return;
    }
    handle_id_t tid_h = (handle_id_t)tid;

    /* Bulk reply buffer (unused but required by EP_CALL msg.buf_uptr path) */
    uint8_t reply_buf[64];
    for (uint32_t i = 0; i < 64; i++) reply_buf[i] = 0;

    /* EP_CALL blocks until server calls SYS_REPLY */
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label      = 0xABCDULL;
    msg.word_count = 0;
    msg.buf_uptr   = (uint64_t)(uintptr_t)reply_buf;
    long r = it_sys2(SYS_EP_CALL, ep_raw, (long)&msg);

    /* After EP_CALL returns the server has already replied */
    for (int i = 0; i < 200 && !g_t016_done; i++)
        it_sys1(SYS_SLEEP, 1);

    it_close(&tid_h);
    it_close(&g_t016_ep_h);

    if (r == 0 && g_t016_ok)
        it_pass("T016");
    else
        it_fail("T016", "ep_call/reply");
}

/* ── T017: FUTEX_WAIT timeout ───────────────────────────────────────────── */

static volatile uint32_t g_t017_futex = 99u;

static void test_t017(void) {
    g_t017_futex = 99u;
    /* FUTEX_WAIT(&g_t017_futex, expected=99, timeout=50ms) → TIMED_OUT */
    long r = it_sys3(SYS_FUTEX_WAIT,
                     (long)(uintptr_t)&g_t017_futex,
                     99,
                     50000000L);
    if (r == (long)IRIS_ERR_TIMED_OUT)
        it_pass("T017");
    else
        it_fail("T017", "expected TIMED_OUT");
}

/* ── Bootstrap ──────────────────────────────────────────────────────────── */

static handle_id_t it_recv_spawn_cap(handle_id_t boot_ch) {
    struct KChanMsg msg;
    for (uint32_t attempt = 0; attempt < 8u; attempt++) {
        it_chan_msg_zero(&msg);
        if (it_sys2(SYS_CHAN_RECV, (long)boot_ch, (long)&msg) < 0)
            return HANDLE_INVALID;
        if (msg.type == SVCMGR_MSG_BOOTSTRAP_HANDLE &&
            msg.data_len >= SVCMGR_BOOTSTRAP_MSG_LEN &&
            msg.attached_handle != HANDLE_INVALID) {
            uint32_t kind = svcmgr_proto_read_u32(
                &msg.data[SVCMGR_BOOTSTRAP_OFF_KIND]);
            if (kind == SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP)
                return (handle_id_t)msg.attached_handle;
            handle_id_t discard = (handle_id_t)msg.attached_handle;
            it_close(&discard);
        }
    }
    return HANDLE_INVALID;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void iris_test_main(handle_id_t bootstrap_ch_h) {
    /* Receive spawn_cap from init */
    handle_id_t spawn_cap_h = it_recv_spawn_cap(bootstrap_ch_h);
    it_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_ch_h);

    /* Open COM1 serial port for test output */
    if (spawn_cap_h != HANDLE_INVALID) {
        long h = it_sys3(SYS_CAP_CREATE_IOPORT, (long)spawn_cap_h, 0x3F8, 8);
        if (h >= 0) g_serial_h = (handle_id_t)h;
        it_sys1(SYS_HANDLE_CLOSE, (long)spawn_cap_h);
    }

    it_serial_write("[IRIS][TEST] start\n");

    /* Run all tests */
    test_t001();
    test_t002();
    test_t003();
    test_t004();
    test_t005();
    test_t006();
    test_t007();
    test_t008();
    test_t009();
    test_t010();
    test_t011();
    test_t012();
    test_t013();
    test_t014();
    test_t015();
    test_t016();
    test_t017();

    /* Final summary marker */
    if (g_pass == g_total) {
        it_serial_write("[IRIS][TEST] SUITE PASS ");
        it_log_num(g_pass);
        it_serial_write("/");
        it_log_num(g_total);
        it_serial_write("\n");
    } else {
        it_serial_write("[IRIS][TEST] SUITE FAIL ");
        it_log_num(g_pass);
        it_serial_write("/");
        it_log_num(g_total);
        it_serial_write("\n");
    }

    it_sys1(SYS_HANDLE_CLOSE, (long)g_serial_h);
    it_sys1(SYS_EXIT, (long)(g_pass != g_total ? 1 : 0));
    for (;;) {}
}
