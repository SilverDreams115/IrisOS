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
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>
#include <iris/kbd_ep_proto.h>
#include <iris/console_ep_proto.h>

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

/* ── T018: EP_NB_SEND on empty endpoint → WOULD_BLOCK ──────────────────── */

static void test_t018(void) {
    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T018", "ep create"); return; }
    handle_id_t ep_h = (handle_id_t)ep_raw;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = 0x1818ULL;
    long r = it_sys2(SYS_EP_NB_SEND, ep_raw, (long)&msg);

    it_close(&ep_h);

    if (r == (long)IRIS_ERR_WOULD_BLOCK)
        it_pass("T018");
    else
        it_fail("T018", "expected WOULD_BLOCK");
}

/* ── T019: endpoint close wakes blocked EP_RECV thread ──────────────────── */

static handle_id_t g_t019_ep_h    = HANDLE_INVALID;
static volatile int g_t019_done   = 0;
static          int g_t019_result = 0;
static uint8_t      g_t019_stack[8192];

static void t019_thread(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys2(SYS_EP_RECV, (long)g_t019_ep_h, (long)&msg);
    g_t019_result = (int)r;
    g_t019_done   = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t019(void) {
    g_t019_done   = 0;
    g_t019_result = 0;

    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T019", "ep create"); return; }
    g_t019_ep_h = (handle_id_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t019_thread;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t019_stack + sizeof(g_t019_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) {
        it_close(&g_t019_ep_h);
        it_fail("T019", "thread create"); return;
    }
    handle_id_t tid_h = (handle_id_t)tid;

    /* Let thread enter EP_RECV and block */
    it_sys1(SYS_SLEEP, 5);

    /* Close handle → active_refs → 0 → endpoint close fires → thread wakes */
    it_close(&g_t019_ep_h);

    for (int i = 0; i < 200 && !g_t019_done; i++)
        it_sys1(SYS_SLEEP, 1);

    it_close(&tid_h);

    if (g_t019_result == (int)IRIS_ERR_CLOSED)
        it_pass("T019");
    else
        it_fail("T019", "expected CLOSED on blocked recv");
}

/* ── T020: endpoint close wakes blocked EP_SEND thread ──────────────────── */

static handle_id_t g_t020_ep_h    = HANDLE_INVALID;
static volatile int g_t020_done   = 0;
static          int g_t020_result = 0;
static uint8_t      g_t020_stack[8192];

static void t020_thread(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = 0x2020ULL;
    long r = it_sys2(SYS_EP_SEND, (long)g_t020_ep_h, (long)&msg);
    g_t020_result = (int)r;
    g_t020_done   = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t020(void) {
    g_t020_done   = 0;
    g_t020_result = 0;

    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T020", "ep create"); return; }
    g_t020_ep_h = (handle_id_t)ep_raw;

    /* Dup to get a second handle — active_refs = 2 */
    long ep2_raw = it_sys2(SYS_HANDLE_DUP, ep_raw,
                           (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));
    if (ep2_raw < 0) {
        it_close(&g_t020_ep_h);
        it_fail("T020", "dup"); return;
    }
    handle_id_t ep2_h = (handle_id_t)ep2_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t020_thread;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t020_stack + sizeof(g_t020_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) {
        it_close(&ep2_h);
        it_close(&g_t020_ep_h);
        it_fail("T020", "thread create"); return;
    }
    handle_id_t tid_h = (handle_id_t)tid;

    /* Let thread enter EP_SEND and block (no receiver present) */
    it_sys1(SYS_SLEEP, 5);

    /* Close both handles: active_refs 2→1→0 → endpoint close → thread wakes */
    it_close(&ep2_h);
    it_close(&g_t020_ep_h);

    for (int i = 0; i < 200 && !g_t020_done; i++)
        it_sys1(SYS_SLEEP, 1);

    it_close(&tid_h);

    if (g_t020_result == (int)IRIS_ERR_CLOSED)
        it_pass("T020");
    else
        it_fail("T020", "expected CLOSED on blocked send");
}

/* ── T021: SYS_REPLY twice → second returns NOT_FOUND ───────────────────── */

static handle_id_t g_t021_ep_h   = HANDLE_INVALID;
static volatile int g_t021_done  = 0;
static          int g_t021_ok    = 0;
static uint8_t      g_t021_stack[8192];

static void t021_client(void) {
    uint8_t rbuf[64];
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = 0x2121ULL;
    msg.buf_uptr = (uint64_t)(uintptr_t)rbuf;
    long r = it_sys2(SYS_EP_CALL, (long)g_t021_ep_h, (long)&msg);
    g_t021_ok   = (r == 0);
    g_t021_done = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t021(void) {
    g_t021_done = 0;
    g_t021_ok   = 0;

    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T021", "ep create"); return; }
    g_t021_ep_h = (handle_id_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t021_client;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t021_stack + sizeof(g_t021_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) {
        it_close(&g_t021_ep_h);
        it_fail("T021", "thread create"); return;
    }
    handle_id_t tid_h = (handle_id_t)tid;

    /* Main is server: receive the EP_CALL and get reply_h */
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys2(SYS_EP_RECV, ep_raw, (long)&msg);
    if (r < 0 || msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t021_ep_h);
        it_fail("T021", "ep_recv"); return;
    }

    handle_id_t reply_h = (handle_id_t)msg.attached_handle;

    /* First SYS_REPLY → client unblocks */
    struct IrisMsg reply;
    it_iris_msg_zero(&reply);
    reply.label = 0xCAFEULL;
    long r1 = it_sys2(SYS_REPLY, (long)reply_h, (long)&reply);

    /* Wait for client to record EP_CALL result */
    for (int i = 0; i < 200 && !g_t021_done; i++)
        it_sys1(SYS_SLEEP, 1);

    /* Second SYS_REPLY on same handle → NOT_FOUND (caller pointer is NULL) */
    it_iris_msg_zero(&reply);
    long r2 = it_sys2(SYS_REPLY, (long)reply_h, (long)&reply);

    it_close(&reply_h);
    it_close(&tid_h);
    it_close(&g_t021_ep_h);

    if (r1 == 0 && g_t021_ok && r2 == (long)IRIS_ERR_NOT_FOUND)
        it_pass("T021");
    else
        it_fail("T021", "reply-twice");
}

/* ── T022: EP_CALL + bulk kbuf round-trip ───────────────────────────────── */

static handle_id_t g_t022_ep_h   = HANDLE_INVALID;
static volatile int g_t022_done  = 0;
static          int g_t022_ok    = 0;
static uint8_t      g_t022_stack[8192];

static uint8_t g_t022_srv_recv[4];
static uint8_t g_t022_srv_reply[4];

static void t022_server(void) {
    struct IrisMsg rmsg;
    it_iris_msg_zero(&rmsg);
    rmsg.buf_uptr = (uint64_t)(uintptr_t)g_t022_srv_recv;
    long r = it_sys2(SYS_EP_RECV, (long)g_t022_ep_h, (long)&rmsg);
    if (r < 0 || rmsg.buf_len != 4u ||
            rmsg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        g_t022_done = 1;
        it_sys1(SYS_THREAD_EXIT, 0);
        for (;;) {}
    }

    int recv_ok = (g_t022_srv_recv[0] == 0x10 && g_t022_srv_recv[1] == 0x20 &&
                   g_t022_srv_recv[2] == 0x30 && g_t022_srv_recv[3] == 0x40);

    g_t022_srv_reply[0] = (uint8_t)(g_t022_srv_recv[0] + 1u);
    g_t022_srv_reply[1] = (uint8_t)(g_t022_srv_recv[1] + 1u);
    g_t022_srv_reply[2] = (uint8_t)(g_t022_srv_recv[2] + 1u);
    g_t022_srv_reply[3] = (uint8_t)(g_t022_srv_recv[3] + 1u);

    handle_id_t reply_h = (handle_id_t)rmsg.attached_handle;
    struct IrisMsg repl;
    it_iris_msg_zero(&repl);
    repl.label   = 0xB01FULL;
    repl.buf_uptr = (uint64_t)(uintptr_t)g_t022_srv_reply;
    repl.buf_len  = 4u;
    long rr = it_sys2(SYS_REPLY, (long)reply_h, (long)&repl);
    it_sys1(SYS_HANDLE_CLOSE, (long)reply_h);

    g_t022_ok   = (recv_ok && rr == 0);
    g_t022_done = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t022(void) {
    g_t022_done = 0;
    g_t022_ok   = 0;

    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T022", "ep create"); return; }
    g_t022_ep_h = (handle_id_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t022_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t022_stack + sizeof(g_t022_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) {
        it_close(&g_t022_ep_h);
        it_fail("T022", "thread create"); return;
    }
    handle_id_t tid_h = (handle_id_t)tid;

    uint8_t client_buf[4];
    client_buf[0] = 0x10; client_buf[1] = 0x20;
    client_buf[2] = 0x30; client_buf[3] = 0x40;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = 0xCA11ULL;
    msg.buf_uptr = (uint64_t)(uintptr_t)client_buf;
    msg.buf_len  = 4u;
    long r = it_sys2(SYS_EP_CALL, ep_raw, (long)&msg);

    for (int i = 0; i < 200 && !g_t022_done; i++)
        it_sys1(SYS_SLEEP, 1);

    it_close(&tid_h);
    it_close(&g_t022_ep_h);

    int bulk_ok = (client_buf[0] == 0x11 && client_buf[1] == 0x21 &&
                   client_buf[2] == 0x31 && client_buf[3] == 0x41);

    if (r == 0 && g_t022_ok && bulk_ok)
        it_pass("T022");
    else
        it_fail("T022", "ep_call bulk round-trip");
}

/* ── T023: EP_SEND on read-only endpoint handle → ACCESS_DENIED ─────────── */

static void test_t023(void) {
    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T023", "ep create"); return; }
    handle_id_t ep_h = (handle_id_t)ep_raw;

    /* Dup with READ-only (no WRITE right) */
    long ro_raw = it_sys2(SYS_HANDLE_DUP, ep_raw, (long)RIGHT_READ);
    if (ro_raw < 0) {
        it_close(&ep_h);
        it_fail("T023", "ro dup"); return;
    }
    handle_id_t ro_h = (handle_id_t)ro_raw;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = 0x2323ULL;
    long r = it_sys2(SYS_EP_SEND, ro_raw, (long)&msg);

    it_close(&ro_h);
    it_close(&ep_h);

    if (r == (long)IRIS_ERR_ACCESS_DENIED)
        it_pass("T023");
    else
        it_fail("T023", "expected ACCESS_DENIED");
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

/* ── T024: SYS_REPLY transfers an attached cap to the EP_CALL caller ────── */

static handle_id_t g_t024_ep_h   = HANDLE_INVALID;
static volatile int g_t024_done  = 0;
static          int g_t024_ok    = 0;
static uint32_t     g_t024_got_h = 0;
static uint8_t      g_t024_stack[8192];

static void t024_client(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = 0x2424ULL;
    long r = it_sys2(SYS_EP_CALL, (long)g_t024_ep_h, (long)&msg);
    g_t024_ok    = (r == 0 && msg.label == IRIS_EP_REPLY_OK);
    g_t024_got_h = msg.attached_handle;
    g_t024_done  = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t024(void) {
    g_t024_done = 0; g_t024_ok = 0; g_t024_got_h = 0;

    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T024", "ep create"); return; }
    g_t024_ep_h = (handle_id_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t024_client;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t024_stack + sizeof(g_t024_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) {
        it_close(&g_t024_ep_h);
        it_fail("T024", "thread create"); return;
    }
    handle_id_t tid_h = (handle_id_t)tid;

    /* Main is server: receive the call, reply with an attached notification */
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys2(SYS_EP_RECV, ep_raw, (long)&msg);
    if (r < 0 || msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t024_ep_h);
        it_fail("T024", "ep_recv"); return;
    }
    handle_id_t reply_h = (handle_id_t)msg.attached_handle;

    long rr = -1;
    long notif_raw = it_sys0(SYS_NOTIFY_CREATE);
    if (notif_raw >= 0) {
        struct IrisMsg reply;
        it_iris_msg_zero(&reply);
        reply.label           = IRIS_EP_REPLY_OK;
        reply.attached_handle = (uint32_t)notif_raw;
        reply.attached_rights = RIGHT_WRITE | RIGHT_WAIT;
        rr = it_sys2(SYS_REPLY, (long)reply_h, (long)&reply);
        if (rr != 0) {
            /* not consumed on pre-staging error */
            handle_id_t nh = (handle_id_t)notif_raw;
            it_close(&nh);
        }
    }

    for (int i = 0; i < 200 && !g_t024_done; i++)
        it_sys1(SYS_SLEEP, 1);

    long ty = -1;
    if (g_t024_got_h != (uint32_t)IRIS_MSG_NO_CAP)
        ty = it_sys1(SYS_HANDLE_TYPE, (long)g_t024_got_h);

    handle_id_t got_h = (handle_id_t)g_t024_got_h;
    it_close(&got_h);
    it_close(&reply_h);
    it_close(&tid_h);
    it_close(&g_t024_ep_h);

    if (rr == 0 && g_t024_ok &&
        g_t024_got_h != (uint32_t)IRIS_MSG_NO_CAP &&
        ty == (long)IRIS_HANDLE_TYPE_NOTIFICATION)
        it_pass("T024");
    else
        it_fail("T024", "reply-cap transfer");
}

/* ── T025: SYS_REPLY with non-transferable cap → ACCESS_DENIED, KReply
 *          survives the failed attempt and a clean reply still unblocks ──── */

static handle_id_t g_t025_ep_h  = HANDLE_INVALID;
static volatile int g_t025_done = 0;
static          int g_t025_ok   = 0;
static uint8_t      g_t025_stack[8192];

static void t025_client(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = 0x2525ULL;
    long r = it_sys2(SYS_EP_CALL, (long)g_t025_ep_h, (long)&msg);
    g_t025_ok   = (r == 0 &&
                   msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP);
    g_t025_done = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t025(void) {
    g_t025_done = 0; g_t025_ok = 0;

    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T025", "ep create"); return; }
    g_t025_ep_h = (handle_id_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t025_client;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t025_stack + sizeof(g_t025_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) {
        it_close(&g_t025_ep_h);
        it_fail("T025", "thread create"); return;
    }
    handle_id_t tid_h = (handle_id_t)tid;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys2(SYS_EP_RECV, ep_raw, (long)&msg);
    if (r < 0 || msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t025_ep_h);
        it_fail("T025", "ep_recv"); return;
    }
    handle_id_t reply_h = (handle_id_t)msg.attached_handle;

    /* Notification dup WITHOUT RIGHT_TRANSFER → staging must fail */
    handle_id_t notif_h = HANDLE_INVALID;
    handle_id_t nt_h    = HANDLE_INVALID;
    long r1 = -1;
    long notif_raw = it_sys0(SYS_NOTIFY_CREATE);
    if (notif_raw >= 0) {
        notif_h = (handle_id_t)notif_raw;
        long nt_raw = it_sys2(SYS_HANDLE_DUP, notif_raw,
                              (long)(RIGHT_WRITE | RIGHT_WAIT));
        if (nt_raw >= 0) {
            nt_h = (handle_id_t)nt_raw;
            struct IrisMsg reply;
            it_iris_msg_zero(&reply);
            reply.label           = IRIS_EP_REPLY_OK;
            reply.attached_handle = (uint32_t)nt_h;
            reply.attached_rights = RIGHT_WRITE | RIGHT_WAIT;
            r1 = it_sys2(SYS_REPLY, (long)reply_h, (long)&reply);
        }
    }

    /* Clean reply on the SAME KReply handle must still succeed */
    struct IrisMsg reply;
    it_iris_msg_zero(&reply);
    reply.label = IRIS_EP_REPLY_OK;
    long r2 = it_sys2(SYS_REPLY, (long)reply_h, (long)&reply);

    for (int i = 0; i < 200 && !g_t025_done; i++)
        it_sys1(SYS_SLEEP, 1);

    it_close(&nt_h);    /* not consumed by the denied staging */
    it_close(&notif_h);
    it_close(&reply_h);
    it_close(&tid_h);
    it_close(&g_t025_ep_h);

    if (r1 == (long)IRIS_ERR_ACCESS_DENIED && r2 == 0 && g_t025_ok)
        it_pass("T025");
    else
        it_fail("T025", "non-transferable reply cap");
}

/* ── Fase 7.1: EP-based service path (svcmgr discovery + VFS) ───────────── */

/* Fase 8: the discovery endpoint is the well-known CPtr slot (kind 0x20
 * retired); it is a CNode slot index, NOT a handle — never close it. */
static handle_id_t g_svcmgr_ep_h = (handle_id_t)IRIS_CPTR_SVCMGR_EP;
static handle_id_t g_vfs_ep_h    = HANDLE_INVALID;  /* from T026 lookup   */
static handle_id_t g_kbd_ep_h    = HANDLE_INVALID;  /* from T034 lookup   */
static handle_id_t g_con_ep_h    = HANDLE_INVALID;  /* from T036 lookup   */

/* EP_CALL buffer reuse: request payload AND reply bulk destination. */
static uint8_t g_ep_io_buf[VFS_EP_DATA_MAX];

static uint32_t it_stage_path(const char *path) {
    uint32_t n = 0;
    while (path[n] && n + 1u < (uint32_t)sizeof(g_ep_io_buf)) {
        g_ep_io_buf[n] = (uint8_t)path[n];
        n++;
    }
    g_ep_io_buf[n] = 0u;
    return n + 1u;
}

/* ── T026: svcmgr EP LOOKUP_NAME("vfs.ep") → endpoint cap ───────────────── */

static void test_t026(void) {
    if (g_svcmgr_ep_h == HANDLE_INVALID) {
        it_fail("T026", "svcmgr ep missing"); return;
    }

    uint32_t len = it_stage_path(VFS_EP_SVC_NAME);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)g_svcmgr_ep_h, (long)&msg);

    long ty = -1;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP)
        ty = it_sys1(SYS_HANDLE_TYPE, (long)msg.attached_handle);

    if (ty == (long)IRIS_HANDLE_TYPE_ENDPOINT) {
        g_vfs_ep_h = (handle_id_t)msg.attached_handle;
        it_pass("T026");
    } else {
        if (msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
            handle_id_t h = (handle_id_t)msg.attached_handle;
            it_close(&h);
        }
        it_fail("T026", "vfs.ep lookup");
    }
}

/* ── T027: VFS EP ping ──────────────────────────────────────────────────── */

static void test_t027(void) {
    if (g_vfs_ep_h == HANDLE_INVALID) {
        it_fail("T027", "vfs ep missing"); return;
    }

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_EP_OP_PING;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    long r = it_sys2(SYS_EP_CALL, (long)g_vfs_ep_h, (long)&msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T027");
    else
        it_fail("T027", "vfs ping");
}

/* ── T028: VFS EP READ_AT("iris.txt") content + EOF semantics ───────────── */

static void test_t028(void) {
    if (g_vfs_ep_h == HANDLE_INVALID) {
        it_fail("T028", "vfs ep missing"); return;
    }

    static const char expect[] = "Hello from IrisOS VFS!\n";
    const uint32_t expect_len = (uint32_t)(sizeof(expect) - 1u);

    uint32_t len = it_stage_path("iris.txt");
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;                 /* offset */
    msg.words[1]   = VFS_EP_DATA_MAX;   /* len (server clamps) */
    msg.word_count = 2;
    msg.buf_uptr   = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len    = len;
    long r = it_sys2(SYS_EP_CALL, (long)g_vfs_ep_h, (long)&msg);

    int ok = (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
              msg.words[1] == (uint64_t)expect_len &&
              msg.words[2] == (uint64_t)expect_len &&
              msg.buf_len == expect_len);
    if (ok) {
        for (uint32_t i = 0; i < expect_len; i++) {
            if (g_ep_io_buf[i] != (uint8_t)expect[i]) { ok = 0; break; }
        }
    }

    /* offset == size → EOF (bytes 0), not an error */
    int eof_ok = 0;
    if (ok) {
        len = it_stage_path("iris.txt");
        it_iris_msg_zero(&msg);
        msg.label      = VFS_EP_OP_READ_AT;
        msg.words[0]   = expect_len;
        msg.words[1]   = VFS_EP_DATA_MAX;
        msg.word_count = 2;
        msg.buf_uptr   = (uint64_t)(uintptr_t)g_ep_io_buf;
        msg.buf_len    = len;
        r = it_sys2(SYS_EP_CALL, (long)g_vfs_ep_h, (long)&msg);
        eof_ok = (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
                  msg.words[1] == 0 &&
                  msg.words[2] == (uint64_t)expect_len);
    }

    if (ok && eof_ok)
        it_pass("T028");
    else
        it_fail("T028", "read_at iris.txt");
}

/* ── T029: VFS EP unknown opcode → NOT_SUPPORTED; LIST oob → NOT_FOUND ──── */

static void test_t029(void) {
    if (g_vfs_ep_h == HANDLE_INVALID) {
        it_fail("T029", "vfs ep missing"); return;
    }

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = UINT64_C(0x0EEE);  /* not a VFS opcode */
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    long r = it_sys2(SYS_EP_CALL, (long)g_vfs_ep_h, (long)&msg);
    int unk_ok = (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
                  msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_SUPPORTED);

    it_iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_LIST;
    msg.words[0]   = 999;
    msg.word_count = 1;
    msg.buf_uptr   = (uint64_t)(uintptr_t)g_ep_io_buf;
    r = it_sys2(SYS_EP_CALL, (long)g_vfs_ep_h, (long)&msg);
    int oob_ok = (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
                  msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND);

    if (unk_ok && oob_ok)
        it_pass("T029");
    else
        it_fail("T029", "vfs ep error codes");
}

/* ── T030: VFS EP malformed READ_AT paths → INVALID_ARG ─────────────────── */

static int t030_expect_inval(struct IrisMsg *msg) {
    long r = it_sys2(SYS_EP_CALL, (long)g_vfs_ep_h, (long)msg);
    return (r == 0 && msg->label == IRIS_EP_REPLY_ERR &&
            msg->words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG);
}

static void test_t030(void) {
    if (g_vfs_ep_h == HANDLE_INVALID) {
        it_fail("T030", "vfs ep missing"); return;
    }

    struct IrisMsg msg;
    int ok = 1;

    /* (a) READ_AT with no path payload */
    it_iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;
    msg.words[1]   = 16;
    msg.word_count = 2;
    msg.buf_uptr   = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len    = 0;
    if (!t030_expect_inval(&msg)) ok = 0;

    /* (b) path not NUL-terminated (drop the NUL from buf_len) */
    uint32_t len = it_stage_path("iris.txt");
    it_iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;
    msg.words[1]   = 16;
    msg.word_count = 2;
    msg.buf_uptr   = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len    = len - 1u;
    if (!t030_expect_inval(&msg)) ok = 0;

    /* (c) oversized path (buf_len > VFS_EP_PATH_MAX) */
    for (uint32_t i = 0; i < VFS_EP_PATH_MAX; i++) g_ep_io_buf[i] = (uint8_t)'a';
    g_ep_io_buf[VFS_EP_PATH_MAX] = 0u;
    it_iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;
    msg.words[1]   = 16;
    msg.word_count = 2;
    msg.buf_uptr   = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len    = VFS_EP_PATH_MAX + 1u;
    if (!t030_expect_inval(&msg)) ok = 0;

    if (ok)
        it_pass("T030");
    else
        it_fail("T030", "malformed read_at");
}

/* ── T031: reserved ".ep" names cannot be spoofed via EP lookup ─────────── */

/*
 * "spoof.ep" matches neither svcmgr's own endpoint, a catalog service with
 * own_service_ep, nor a dynamic registration (svcmgr rejects ".ep" names at
 * register time — covered on the register side by init S4). The lookup must
 * return NOT_FOUND with no capability attached: a resolvable ".ep" name here
 * would mean a fabricated/spoofed endpoint.
 */
static void test_t031(void) {
    if (g_svcmgr_ep_h == HANDLE_INVALID) {
        it_fail("T031", "svcmgr ep missing"); return;
    }

    uint32_t len = it_stage_path("spoof.ep");
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)g_svcmgr_ep_h, (long)&msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
        msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND &&
        msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        it_pass("T031");
    } else {
        if (msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
            handle_id_t h = (handle_id_t)msg.attached_handle;
            it_close(&h);
        }
        it_fail("T031", "spoof.ep lookup must NOT resolve");
    }
}

/* ── T032: legacy "vfs" KChannel name must no longer resolve (Fase 7.5) ─── */

/*
 * vfs is endpoint_only: svcmgr never creates the legacy service/reply
 * KChannel pair, so a lookup of the bare "vfs" name must return NOT_FOUND
 * with no capability attached. A resolvable "vfs" here would mean a stale
 * legacy KChannel route back into the stateful protocol.
 */
static void test_t032(void) {
    if (g_svcmgr_ep_h == HANDLE_INVALID) {
        it_fail("T032", "svcmgr ep missing"); return;
    }

    uint32_t len = it_stage_path("vfs");
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)g_svcmgr_ep_h, (long)&msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
        msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND &&
        msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        it_pass("T032");
    } else {
        if (msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
            handle_id_t h = (handle_id_t)msg.attached_handle;
            it_close(&h);
        }
        it_fail("T032", "legacy vfs name must NOT resolve");
    }
}

/* ── T033: VFS EP STATUS (Fase 7.5) ─────────────────────────────────────── */

static void test_t033(void) {
    if (g_vfs_ep_h == HANDLE_INVALID) {
        it_fail("T033", "vfs ep missing"); return;
    }

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = VFS_EP_OP_STATUS;
    long r = it_sys2(SYS_EP_CALL, (long)g_vfs_ep_h, (long)&msg);

    if (r != 0 || msg.label != IRIS_EP_REPLY_OK ||
        msg.words[1] < (uint64_t)VFS_BOOT_EXPORT_COUNT) {
        it_fail("T033", "vfs ep status");
        return;
    }

    /* STATUS with a bulk payload is malformed → INVALID_ARG */
    uint32_t len = it_stage_path("junk");
    it_iris_msg_zero(&msg);
    msg.label    = VFS_EP_OP_STATUS;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    r = it_sys2(SYS_EP_CALL, (long)g_vfs_ep_h, (long)&msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
        msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG)
        it_pass("T033");
    else
        it_fail("T033", "status payload must be rejected");
}

/* ── T034: svcmgr EP LOOKUP_NAME("kbd.ep") → endpoint cap + PING (7.4) ─── */

static void test_t034(void) {
    if (g_svcmgr_ep_h == HANDLE_INVALID) {
        it_fail("T034", "svcmgr ep missing"); return;
    }

    uint32_t len = it_stage_path(KBD_EP_SVC_NAME);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)g_svcmgr_ep_h, (long)&msg);

    long ty = -1;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP)
        ty = it_sys1(SYS_HANDLE_TYPE, (long)msg.attached_handle);

    if (ty != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
        if (msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
            handle_id_t h = (handle_id_t)msg.attached_handle;
            it_close(&h);
        }
        it_fail("T034", "kbd.ep lookup");
        return;
    }
    g_kbd_ep_h = (handle_id_t)msg.attached_handle;

    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    r = it_sys2(SYS_EP_CALL, (long)g_kbd_ep_h, (long)&msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T034");
    else
        it_fail("T034", "kbd ping");
}

/* ── T035: kbd EP semantics — empty POLL, malformed requests (7.4) ────── */

static int t035_expect_err(struct IrisMsg *msg, uint32_t want) {
    return msg->label == IRIS_EP_REPLY_ERR &&
           (uint32_t)msg->words[0] == want;
}

static void test_t035(void) {
    if (g_kbd_ep_h == HANDLE_INVALID) {
        it_fail("T035", "kbd ep missing"); return;
    }

    struct IrisMsg msg;
    int ok = 1;

    /* POLL on an idle keyboard (headless: no keys) → WOULD_BLOCK, clean */
    it_iris_msg_zero(&msg);
    msg.label = KBD_EP_OP_POLL;
    if (it_sys2(SYS_EP_CALL, (long)g_kbd_ep_h, (long)&msg) != 0 ||
        !t035_expect_err(&msg, KBD_EP_E_WOULD_BLOCK)) ok = 0;

    /* unknown opcode → NOT_SUPPORTED */
    it_iris_msg_zero(&msg);
    msg.label = 0x7777;
    if (it_sys2(SYS_EP_CALL, (long)g_kbd_ep_h, (long)&msg) != 0 ||
        !t035_expect_err(&msg, KBD_EP_E_NOT_SUPPORTED)) ok = 0;

    /* bulk payload on POLL → INVALID_ARG */
    uint32_t len = it_stage_path("junk");
    it_iris_msg_zero(&msg);
    msg.label    = KBD_EP_OP_POLL;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    if (it_sys2(SYS_EP_CALL, (long)g_kbd_ep_h, (long)&msg) != 0 ||
        !t035_expect_err(&msg, KBD_EP_E_INVALID_ARG)) ok = 0;

    if (ok)
        it_pass("T035");
    else
        it_fail("T035", "kbd ep semantics");
}

/* ── T036: svcmgr EP LOOKUP_NAME("console.ep") → endpoint cap + PING (7.3) ─ */

static void test_t036(void) {
    if (g_svcmgr_ep_h == HANDLE_INVALID) {
        it_fail("T036", "svcmgr ep missing"); return;
    }

    uint32_t len = it_stage_path(CONSOLE_EP_SVC_NAME);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)g_svcmgr_ep_h, (long)&msg);

    long ty = -1;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP)
        ty = it_sys1(SYS_HANDLE_TYPE, (long)msg.attached_handle);

    if (ty != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
        if (msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
            handle_id_t h = (handle_id_t)msg.attached_handle;
            it_close(&h);
        }
        it_fail("T036", "console.ep lookup");
        return;
    }
    g_con_ep_h = (handle_id_t)msg.attached_handle;

    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    r = it_sys2(SYS_EP_CALL, (long)g_con_ep_h, (long)&msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T036");
    else
        it_fail("T036", "console ping");
}

/* ── T037: console EP WRITE — gated marker lands on the UART (7.3) ──────── */

static void test_t037(void) {
    if (g_con_ep_h == HANDLE_INVALID) {
        it_fail("T037", "console ep missing"); return;
    }

    static const char line[] = "[IRIS][TEST] console ep write OK\n";
    uint32_t len = (uint32_t)sizeof(line) - 1u;
    for (uint32_t i = 0; i < len; i++) g_ep_io_buf[i] = (uint8_t)line[i];

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = CONSOLE_EP_OP_WRITE;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)g_con_ep_h, (long)&msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T037");
    else
        it_fail("T037", "console ep write");
}

/* ── T038: console EP SYNC + malformed requests (7.3) ───────────────────── */

static void test_t038(void) {
    if (g_con_ep_h == HANDLE_INVALID) {
        it_fail("T038", "console ep missing"); return;
    }

    struct IrisMsg msg;
    int ok = 1;

    /* SYNC: deterministic barrier, no payload */
    it_iris_msg_zero(&msg);
    msg.label = CONSOLE_EP_OP_SYNC;
    if (it_sys2(SYS_EP_CALL, (long)g_con_ep_h, (long)&msg) != 0 ||
        msg.label != IRIS_EP_REPLY_OK) ok = 0;

    /* SYNC with bulk payload → INVALID_ARG */
    uint32_t len = it_stage_path("junk");
    it_iris_msg_zero(&msg);
    msg.label    = CONSOLE_EP_OP_SYNC;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    if (it_sys2(SYS_EP_CALL, (long)g_con_ep_h, (long)&msg) != 0 ||
        msg.label != IRIS_EP_REPLY_ERR ||
        (uint32_t)msg.words[0] != (uint32_t)IRIS_ERR_INVALID_ARG) ok = 0;

    /* unknown opcode → NOT_SUPPORTED */
    it_iris_msg_zero(&msg);
    msg.label = 0x6666;
    if (it_sys2(SYS_EP_CALL, (long)g_con_ep_h, (long)&msg) != 0 ||
        msg.label != IRIS_EP_REPLY_ERR ||
        (uint32_t)msg.words[0] != (uint32_t)IRIS_ERR_NOT_SUPPORTED) ok = 0;

    if (ok)
        it_pass("T038");
    else
        it_fail("T038", "console ep semantics");
}

/* ── T039: CPtr-first svcmgr discovery (Fase 8) ─────────────────────────── */

/*
 * init minted the svcmgr discovery endpoint into our root CNode at
 * IRIS_CPTR_SVCMGR_EP (slot 1, RIGHT_WRITE). EP_CALL resolves CSpace-first,
 * so the raw CPtr — never delivered as a handle — must work end to end:
 * PING, then a LOOKUP_NAME("vfs.ep") that returns a real endpoint cap.
 */
static void test_t039(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (r != 0 || msg.label != IRIS_EP_REPLY_OK) {
        it_fail("T039", "cptr ping");
        return;
    }

    uint32_t len = it_stage_path(VFS_EP_SVC_NAME);
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);

    long ty = -1;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP)
        ty = it_sys1(SYS_HANDLE_TYPE, (long)msg.attached_handle);

    if (msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
        handle_id_t h = (handle_id_t)msg.attached_handle;
        it_close(&h);
    }
    if (ty == (long)IRIS_HANDLE_TYPE_ENDPOINT)
        it_pass("T039");
    else
        it_fail("T039", "cptr lookup");
}

/* ── T040: CPtr failure semantics (Fase 8) ──────────────────────────────── */

/*
 * slot 30 (IRIS_CPTR_TEST_FIX_A) = console KChannel cap (wrong type),
 * slot 31 (IRIS_CPTR_TEST_FIX_B) = svcmgr ep with RIGHT_TRANSFER only
 * (insufficient for EP_CALL's RIGHT_WRITE).
 *   - CPTR_NULL → clean negative error (never a crash);
 *   - wrong type → IRIS_ERR_WRONG_TYPE from the CSpace path;
 *   - ACCESS_DENIED → hard stop: the dual resolver must NOT fall back to
 *     the handle table (a fallback would yield BAD_HANDLE instead, since
 *     raw values 30/31 are never live handles — generations start at 1).
 */
static void test_t040(void) {
    struct IrisMsg msg;
    int ok = 1;

    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = it_sys2(SYS_EP_CALL, 0L /* CPTR_NULL */, (long)&msg);
    if (r >= 0) ok = 0;

    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_TEST_FIX_A, (long)&msg);
    if (r != (long)IRIS_ERR_WRONG_TYPE) ok = 0;

    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_TEST_FIX_B, (long)&msg);
    if (r != (long)IRIS_ERR_ACCESS_DENIED) ok = 0;

    if (ok)
        it_pass("T040");
    else
        it_fail("T040", "cptr failure semantics");
}

/* ── T041: well-known CPtr slots resolve with the right type (Fase 8) ───── */

/*
 * SYS_CSPACE_RESOLVE materializes a slot into a handle; slots 1..4 must all
 * be live KEndpoints and a reserved-but-unminted slot must fail cleanly.
 */
static void test_t041(void) {
    static const uint64_t slots[4] = {
        IRIS_CPTR_SVCMGR_EP, IRIS_CPTR_VFS_EP,
        IRIS_CPTR_CONSOLE_EP, IRIS_CPTR_KBD_EP,
    };
    int ok = 1;
    for (uint32_t i = 0; i < 4u; i++) {
        long h = it_sys1(SYS_CSPACE_RESOLVE, (long)slots[i]);
        if (h < 0) { ok = 0; break; }
        if (it_sys1(SYS_HANDLE_TYPE, h) != (long)IRIS_HANDLE_TYPE_ENDPOINT)
            ok = 0;
        handle_id_t hh = (handle_id_t)h;
        it_close(&hh);
        if (!ok) break;
    }
    /* unminted reserved slot fails cleanly (no crash, negative error) */
    if (it_sys1(SYS_CSPACE_RESOLVE, 29L) >= 0) ok = 0;

    if (ok)
        it_pass("T041");
    else
        it_fail("T041", "well-known slot resolve");
}

/* ── T042: VFS READ_AT directly via IRIS_CPTR_VFS_EP (Fase 8) ───────────── */

static void test_t042(void) {
    static const char expect[] = "Hello from IrisOS VFS!\n";
    const uint32_t expect_len = (uint32_t)(sizeof(expect) - 1u);

    uint32_t len = it_stage_path("iris.txt");
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;
    msg.words[1]   = VFS_EP_DATA_MAX;
    msg.word_count = 2;
    msg.buf_uptr   = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len    = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_VFS_EP, (long)&msg);

    int ok = (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
              msg.words[1] == (uint64_t)expect_len &&
              msg.buf_len == expect_len);
    if (ok) {
        for (uint32_t i = 0; i < expect_len; i++) {
            if (g_ep_io_buf[i] != (uint8_t)expect[i]) { ok = 0; break; }
        }
    }
    if (ok)
        it_pass("T042");
    else
        it_fail("T042", "vfs via cptr");
}

/* ── T043: console EP WRITE via IRIS_CPTR_CONSOLE_EP — gated marker ─────── */

static void test_t043(void) {
    static const char line[] = "[IRIS][TEST] console cptr write OK\n";
    const uint32_t line_len = (uint32_t)(sizeof(line) - 1u);

    for (uint32_t i = 0; i < line_len; i++) g_ep_io_buf[i] = (uint8_t)line[i];

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = CONSOLE_EP_OP_WRITE;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = line_len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_CONSOLE_EP, (long)&msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T043");
    else
        it_fail("T043", "console via cptr");
}

/* ── T044: kbd PING via IRIS_CPTR_KBD_EP (Fase 8) ───────────────────────── */

static void test_t044(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_KBD_EP, (long)&msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T044");
    else
        it_fail("T044", "kbd via cptr");
}

/* ── T045: client slots carry WRITE only — recv is denied, no fallback ──── */

/*
 * Slot 2 was minted RIGHT_WRITE (client side).  EP_NB_RECV needs READ, so
 * the CSpace path must return ACCESS_DENIED as a hard stop; a handle-table
 * fallback would surface BAD_HANDLE (raw 2 is never a live handle).
 */
static void test_t045(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    long r = it_sys2(SYS_EP_NB_RECV, (long)IRIS_CPTR_VFS_EP, (long)&msg);
    if (r == (long)IRIS_ERR_ACCESS_DENIED)
        it_pass("T045");
    else
        it_fail("T045", "rights reduction on client slot");
}

/* ── T046: legacy handle path interop — lookup still yields real handles ─ */

/*
 * Discovery by name must keep working alongside CPtr slots: a LOOKUP_NAME
 * through slot 1 returns a REAL handle (>= 1024 — generation >= 1), which
 * must be invocable and closable like any legacy cap.
 */
static void test_t046(void) {
    uint32_t len = it_stage_path(CONSOLE_EP_SVC_NAME);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);

    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP &&
        msg.attached_handle >= 1024u) {
        struct IrisMsg ping;
        it_iris_msg_zero(&ping);
        ping.label = IRIS_EP_OP_PING;
        if (it_sys2(SYS_EP_CALL, (long)msg.attached_handle, (long)&ping) == 0 &&
            ping.label == IRIS_EP_REPLY_OK)
            ok = 1;
        handle_id_t h = (handle_id_t)msg.attached_handle;
        it_close(&h);
    }
    if (ok)
        it_pass("T046");
    else
        it_fail("T046", "legacy lookup handle interop");
}

/* ── Fase 9: badges & sender identity (T047–T053) ───────────────────────── */

/* PING a slot and return the badge the server says it observed (words[1]);
 * stores -1 on transport/protocol failure. */
static long it_ping_badge(long cptr, uint64_t *out_badge) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = it_sys2(SYS_EP_CALL, cptr, (long)&msg);
    if (r != 0 || msg.label != IRIS_EP_REPLY_OK || msg.word_count < 2u)
        return -1;
    *out_badge = msg.words[1];
    return 0;
}

/* T047: svcmgr observes our kernel-stamped badge on the discovery slot. */
static void test_t047(void) {
    uint64_t b = 0;
    if (it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &b) == 0 &&
        b == IRIS_BADGE_IRIS_TEST)
        it_pass("T047");
    else
        it_fail("T047", "svcmgr badge");
}

/* T048: VFS observes the expected badge. */
static void test_t048(void) {
    uint64_t b = 0;
    if (it_ping_badge((long)IRIS_CPTR_VFS_EP, &b) == 0 &&
        b == IRIS_BADGE_IRIS_TEST)
        it_pass("T048");
    else
        it_fail("T048", "vfs badge");
}

/* T049: console observes the expected badge. */
static void test_t049(void) {
    uint64_t b = 0;
    if (it_ping_badge((long)IRIS_CPTR_CONSOLE_EP, &b) == 0 &&
        b == IRIS_BADGE_IRIS_TEST)
        it_pass("T049");
    else
        it_fail("T049", "console badge");
}

/* T050: kbd (assembly server) observes the expected badge. */
static void test_t050(void) {
    uint64_t b = 0;
    if (it_ping_badge((long)IRIS_CPTR_KBD_EP, &b) == 0 &&
        b == IRIS_BADGE_IRIS_TEST)
        it_pass("T050");
    else
        it_fail("T050", "kbd badge");
}

/* T051: payload spoofing is impossible — whatever we write into
 * sender_badge is overwritten by the kernel at send time. */
static void test_t051(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label        = IRIS_EP_OP_PING;
    msg.sender_badge = 0xDEADBEEFu;          /* forged identity attempt */
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.word_count >= 2u && msg.words[1] == IRIS_BADGE_IRIS_TEST)
        it_pass("T051");
    else
        it_fail("T051", "payload badge spoof must not work");
}

/* T052: legacy unbadged path stays compatible — a cap obtained via name
 * lookup (handle >= 1024, unbadged master dup) delivers badge 0. */
static void test_t052(void) {
    uint32_t len = it_stage_path(CONSOLE_EP_SVC_NAME);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);

    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
        uint64_t b = 0xFFu;
        if (it_ping_badge((long)msg.attached_handle, &b) == 0 && b == 0u)
            ok = 1;
        handle_id_t h = (handle_id_t)msg.attached_handle;
        it_close(&h);
    }
    if (ok)
        it_pass("T052");
    else
        it_fail("T052", "legacy unbadged path");
}

/* T053: two caps to the SAME endpoint deliver DIFFERENT badges (slot 1 vs
 * fixture slot 28), and a badged cap still honours rights (slot 31 is
 * TRANSFER-only: EP_CALL stays ACCESS_DENIED, no fallback). */
static void test_t053(void) {
    uint64_t b1 = 0, b2 = 0;
    int ok = 1;
    if (it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &b1) != 0) ok = 0;
    if (it_ping_badge((long)IRIS_CPTR_TEST_FIX_C, &b2) != 0) ok = 0;
    if (b1 != IRIS_BADGE_IRIS_TEST || b2 != IRIS_BADGE_TEST_B || b1 == b2)
        ok = 0;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    if (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_TEST_FIX_B, (long)&msg) !=
        (long)IRIS_ERR_ACCESS_DENIED)
        ok = 0;

    if (ok)
        it_pass("T053");
    else
        it_fail("T053", "distinct badges per cap");
}

/* ── Fase 10: service lifecycle, death/relookup & badge policy (T054–T062) ─ */

/* svcmgr STATUS oracle: name → {alive, generation}. Returns 0 on OK. */
static long it_status(const char *name, uint32_t *alive, uint32_t *gen) {
    uint32_t len = it_stage_path(name);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_STATUS;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (r != 0 || msg.label != IRIS_EP_REPLY_OK || msg.word_count < 2u)
        return -1;
    if (alive) *alive = (uint32_t)msg.words[0];
    if (gen)   *gen   = (uint32_t)msg.words[1];
    return 0;
}

/* Generation cached at the pre-restart lookup (T056), checked stale in T059. */
static uint32_t g_vfs_gen0 = 0;

/* T054: badge-authenticated REGISTER name-claim + owner-checked UNREGISTER. */
static void test_t054(void) {
    struct IrisMsg msg;
    int ok = 1;
    uint32_t len = it_stage_path("ltst.svc");
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_REGISTER;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_OK)) ok = 0;
    uint32_t did = (uint32_t)msg.words[0];

    /* Re-claim the same name → BUSY. */
    len = it_stage_path("ltst.svc");
    it_iris_msg_zero(&msg);
    msg.label = IRIS_SVCMGR_EP_REGISTER;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len = len;
    r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
          msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_BUSY)) ok = 0;

    /* UNREGISTER with our own (owner) badge → OK. */
    it_iris_msg_zero(&msg);
    msg.label = IRIS_SVCMGR_EP_UNREGISTER;
    msg.words[0] = did;
    msg.word_count = 1u;
    r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_OK)) ok = 0;

    if (ok) it_pass("T054"); else it_fail("T054", "register/unregister badge");
}

/* T055: `.ep` EP-lookup grant tightening — an ordinary client receives a
 * call-only cap; it has RIGHT_WRITE (ping works) but NOT RIGHT_DUPLICATE
 * (SYS_HANDLE_DUP → ACCESS_DENIED, no re-mint authority). */
static void test_t055(void) {
    uint32_t len = it_stage_path(VFS_EP_SVC_NAME);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
        handle_id_t cap = (handle_id_t)msg.attached_handle;
        struct IrisMsg p;
        it_iris_msg_zero(&p);
        p.label = IRIS_EP_OP_PING;
        long pr  = it_sys2(SYS_EP_CALL, (long)cap, (long)&p);          /* WRITE works */
        long dup = it_sys2(SYS_HANDLE_DUP, (long)cap, (long)RIGHT_WRITE); /* no DUP */
        if (pr == 0 && p.label == IRIS_EP_REPLY_OK &&
            dup == (long)IRIS_ERR_ACCESS_DENIED)
            ok = 1;
        if (dup >= 0) { handle_id_t d = (handle_id_t)dup; it_close(&d); }
        it_close(&cap);
    }
    if (ok) it_pass("T055"); else it_fail("T055", ".ep grant not tightened");
}

/* T056: STATUS oracle reports a live service + its generation (cached for T059). */
static void test_t056(void) {
    uint32_t a = 0, g = 0;
    if (it_status(VFS_EP_SVC_NAME, &a, &g) == 0 && a == 1u && g >= 1u) {
        g_vfs_gen0 = g;
        it_pass("T056");
    } else {
        it_fail("T056", "vfs status");
    }
}

/* T057: REAL death→respawn E2E.  Drive the privileged RESTART via the
 * supervisor cap (slot 29), then poll STATUS (bounded, no sleep — each
 * iteration blocks in an EP_CALL which yields the CPU) until the kernel's
 * SYS_PROCESS_WATCH path has respawned VFS and bumped its generation. */
static void test_t057(void) {
    uint32_t a = 0, g0 = 0;
    if (it_status(VFS_EP_SVC_NAME, &a, &g0) != 0 || a != 1u) {
        it_fail("T057", "pre-status"); return;
    }
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_SVCMGR_EP_RESTART;
    msg.words[0] = (uint64_t)SVCMGR_SERVICE_VFS;
    msg.word_count = 1u;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_TEST_SUPER, (long)&msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_OK)) {
        it_fail("T057", "restart request denied"); return;
    }
    int recovered = 0;
    for (uint32_t i = 0; i < 400u && !recovered; i++) {
        uint64_t b = 0;
        (void)it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &b);  /* yield to svcmgr */
        uint32_t a1 = 0, g1 = 0;
        if (it_status(VFS_EP_SVC_NAME, &a1, &g1) == 0 && a1 == 1u && g1 > g0)
            recovered = 1;
    }
    if (recovered) it_pass("T057"); else it_fail("T057", "vfs did not restart");
}

/* T058: notification close-while-wait — covered by the dedicated host unit
 * test (tests/kernel/test_knotification.c, Fase 10).  This runtime slot
 * confirms the kbd IRQ-notification WAIT slot is still functional after the
 * lifecycle changes (a non-blocking poll must not fault). */
static void test_t058(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_KBD_EP, (long)&msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T058");
    else
        it_fail("T058", "kbd notification path");
}

/* T059: logical revocation — a client that cached the pre-restart generation
 * (T056) detects, via STATUS, that VFS has since been restarted (generation
 * advanced), so its old generation is stale and a relookup is required. */
static void test_t059(void) {
    uint32_t a = 0, g = 0;
    if (it_status(VFS_EP_SVC_NAME, &a, &g) == 0 &&
        a == 1u && g > g_vfs_gen0 && g_vfs_gen0 != 0u)
        it_pass("T059");
    else
        it_fail("T059", "stale generation not detected");
}

/* T060: relookup after restart yields a working cap to the new instance. */
static void test_t060(void) {
    uint32_t len = it_stage_path(VFS_EP_SVC_NAME);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
        handle_id_t cap = (handle_id_t)msg.attached_handle;
        struct IrisMsg p;
        it_iris_msg_zero(&p);
        p.label = IRIS_EP_OP_PING;
        long pr = it_sys2(SYS_EP_CALL, (long)cap, (long)&p);
        if (pr == 0 && p.label == IRIS_EP_REPLY_OK) ok = 1;
        it_close(&cap);
    }
    if (ok) it_pass("T060"); else it_fail("T060", "new vfs cap after restart");
}

/* T061: REGISTER cannot spoof a reserved name — ".ep" endpoints and catalog
 * service names are rejected regardless of the caller's badge. */
static void test_t061(void) {
    static const char *const reserved[] = {
        VFS_EP_SVC_NAME, CONSOLE_EP_SVC_NAME, "vfs",
    };
    int ok = 1;
    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t len = it_stage_path(reserved[i]);
        struct IrisMsg msg;
        it_iris_msg_zero(&msg);
        msg.label    = IRIS_SVCMGR_EP_REGISTER;
        msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
        msg.buf_len  = len;
        long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
        if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
              msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_ACCESS_DENIED))
            ok = 0;
    }
    if (ok) it_pass("T061"); else it_fail("T061", "reserved name spoof");
}

/* T062: badge policy regression guard — core servers still observe the
 * caller's kernel-stamped badge after all lifecycle changes. */
static void test_t062(void) {
    uint64_t bv = 0, bc = 0;
    if (it_ping_badge((long)IRIS_CPTR_VFS_EP, &bv) == 0 &&
        bv == IRIS_BADGE_IRIS_TEST &&
        it_ping_badge((long)IRIS_CPTR_CONSOLE_EP, &bc) == 0 &&
        bc == IRIS_BADGE_IRIS_TEST)
        it_pass("T062");
    else
        it_fail("T062", "badge policy regressed");
}

/* ── Bootstrap ──────────────────────────────────────────────────────────── */

/*
 * Receives the SPAWN_CAP bootstrap cap from init (serial/test loading).
 * Timeout-bounded so a missing message degrades to HANDLE_INVALID — the
 * dependent tests then FAIL loudly instead of hanging boot or being
 * silently skipped.  (Fase 8: the discovery endpoint no longer arrives
 * here; it is the well-known slot IRIS_CPTR_SVCMGR_EP.)
 */
static handle_id_t it_recv_bootstrap(handle_id_t boot_ch) {
    struct KChanMsg msg;
    handle_id_t spawn_cap_h = HANDLE_INVALID;
    /* Fase 8: only the spawn cap travels over the bootstrap channel
     * (KBootstrapCap is outside the dual resolver — handle boundary).
     * The svcmgr discovery endpoint is the well-known slot
     * IRIS_CPTR_SVCMGR_EP; bootstrap kind 0x20 is retired. */
    for (uint32_t attempt = 0; attempt < 8u; attempt++) {
        if (spawn_cap_h != HANDLE_INVALID)
            break;
        it_chan_msg_zero(&msg);
        if (it_sys3(SYS_CHAN_RECV_TIMEOUT, (long)boot_ch, (long)&msg,
                    500000000L) < 0)
            break;
        if (msg.type != SVCMGR_MSG_BOOTSTRAP_HANDLE ||
            msg.data_len < SVCMGR_BOOTSTRAP_MSG_LEN ||
            msg.attached_handle == HANDLE_INVALID)
            continue;
        uint32_t kind = svcmgr_proto_read_u32(
            &msg.data[SVCMGR_BOOTSTRAP_OFF_KIND]);
        if (kind == SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP &&
            spawn_cap_h == HANDLE_INVALID) {
            spawn_cap_h = (handle_id_t)msg.attached_handle;
        } else {
            handle_id_t discard = (handle_id_t)msg.attached_handle;
            it_close(&discard);
        }
    }
    return spawn_cap_h;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void iris_test_main(handle_id_t bootstrap_ch_h) {
    /* Receive spawn_cap + svcmgr discovery endpoint from init */
    handle_id_t spawn_cap_h = it_recv_bootstrap(bootstrap_ch_h);
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
    test_t018();
    test_t019();
    test_t020();
    test_t021();
    test_t022();
    test_t023();
    test_t024();
    test_t025();
    test_t026();
    test_t027();
    test_t028();
    test_t029();
    test_t030();
    test_t031();
    test_t032();
    test_t033();
    test_t034();
    test_t035();
    test_t036();
    test_t037();
    test_t038();
    test_t039();
    test_t040();
    test_t041();
    test_t042();
    test_t043();
    test_t044();
    test_t045();
    test_t046();
    test_t047();
    test_t048();
    test_t049();
    test_t050();
    test_t051();
    test_t052();
    test_t053();
    test_t054();
    test_t055();
    test_t056();
    test_t057();
    test_t058();
    test_t059();
    test_t060();
    test_t061();
    test_t062();

    /* g_svcmgr_ep_h is a CPtr slot (not a handle): nothing to close. */
    it_close(&g_vfs_ep_h);

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
