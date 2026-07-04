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

/* it_chan_msg_zero retired — Fase 13/Track I (no KChannel tests remain). */

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

/* ── T004-T007 retired (Fase 13/Track F) ───────────────────────────────
 * The KChannel-specific tests (loopback, NB-recv-empty, recv-timeout,
 * seal) are superseded by endpoint/notification equivalents:
 *   T004 → T015 (EP_SEND/RECV)      T005 → T014 (EP_NB_RECV empty)
 *   T006 → T010 (NOTIFY_WAIT_TIMEOUT) T007 → T019 (endpoint close).      */

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

/* ── T011: HANDLE_TYPE on endpoint (Fase 13/Track F: KChannel→KEndpoint) ── */

static void test_t011(void) {
    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T011", "ep create"); return; }
    handle_id_t ep_h = (handle_id_t)ep_raw;

    long ty = it_sys1(SYS_HANDLE_TYPE, ep_raw);

    it_close(&ep_h);

    if (ty == (long)IRIS_HANDLE_TYPE_ENDPOINT)
        it_pass("T011");
    else
        it_fail("T011", "wrong type");
}

/* ── T012: HANDLE_SAME_OBJECT on endpoints (Fase 13/Track F) ─────────────── */

static void test_t012(void) {
    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T012", "ep create"); return; }
    handle_id_t ep_h = (handle_id_t)ep_raw;

    long dup_raw = it_sys2(SYS_HANDLE_DUP, ep_raw,
                           (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));
    if (dup_raw < 0) {
        it_close(&ep_h);
        it_fail("T012", "dup"); return;
    }
    handle_id_t dup_h = (handle_id_t)dup_raw;

    long ep2_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep2_raw < 0) {
        it_close(&dup_h);
        it_close(&ep_h);
        it_fail("T012", "ep2 create"); return;
    }
    handle_id_t ep2_h = (handle_id_t)ep2_raw;

    /* ep_h and dup_h → same object */
    long same = it_sys2(SYS_HANDLE_SAME_OBJECT, ep_raw, dup_raw);
    /* ep_h and ep2_h → different objects */
    long diff = it_sys2(SYS_HANDLE_SAME_OBJECT, ep_raw, ep2_raw);

    it_close(&ep2_h);
    it_close(&dup_h);
    it_close(&ep_h);

    if (same == 1 && diff == 0)
        it_pass("T012");
    else
        it_fail("T012", "same-obj wrong");
}

/* ── T013: Rights enforcement on an endpoint (no WRITE → EP_SEND fails) ──── */
/* Fase 13/Track I: rewritten from KChannel to KEndpoint — EP_SEND requires
 * RIGHT_WRITE, so a READ-only cap is rejected with ACCESS_DENIED (same
 * rights-enforcement guarantee, no SYS_CHAN). */
static void test_t013(void) {
    long ep_raw = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep_raw < 0) { it_fail("T013", "ep create"); return; }
    handle_id_t ep_h = (handle_id_t)ep_raw;

    /* Dup with READ-only (no WRITE right) */
    long ro_raw = it_sys2(SYS_HANDLE_DUP, ep_raw, (long)RIGHT_READ);
    if (ro_raw < 0) {
        it_close(&ep_h);
        it_fail("T013", "ro dup"); return;
    }
    handle_id_t ro_h = (handle_id_t)ro_raw;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    /* EP_SEND on a read-only endpoint cap must fail with ACCESS_DENIED. */
    long r = it_sys2(SYS_EP_SEND, ro_raw, (long)&msg);

    it_close(&ro_h);
    it_close(&ep_h);

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

/* Fase 11: a dynamic test service registered by cap-transfer (T054), reused by
 * the lookup/unregister tests T063–T066. */
static handle_id_t g_ltst_ep = (handle_id_t)0;   /* HANDLE_INVALID */
static uint32_t    g_ltst_id = 0;

/* Register endpoint `ep` under `name` via EP_CALL cap-transfer (attached_cap).
 * The dup is consumed by staging; returns the dynamic id, or -(error code). */
static long it_register_ep(const char *name, handle_id_t ep) {
    /* The master svcmgr keeps must carry DUPLICATE so it can hand each client a
     * fresh WRITE cap on lookup (+TRANSFER so the cap is deliverable to it). */
    iris_rights_t mr = (iris_rights_t)(RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    long d = it_sys2(SYS_HANDLE_DUP, (long)ep, (long)mr);
    if (d < 0) return d;
    uint32_t len = it_stage_path(name);
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label               = IRIS_SVCMGR_EP_REGISTER;
    msg.buf_uptr            = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len             = len;
    msg.attached_cap        = (uint32_t)d;
    msg.attached_cap_rights = (uint32_t)mr;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (r != 0) return r;
    if (msg.label != IRIS_EP_REPLY_OK) return -(long)(uint32_t)msg.words[0];
    return (long)(uint32_t)msg.words[0];
}

/* T054: cap-backed REGISTER over EP — the caller transfers a REAL endpoint cap
 * (attached_cap) and still gets a working reply (KReply + transfer coexist). */
static void test_t054(void) {
    long e = it_sys0(SYS_ENDPOINT_CREATE);
    if (e < 0) { it_fail("T054", "endpoint create"); return; }
    g_ltst_ep = (handle_id_t)e;

    long id = it_register_ep("ltst.svc", g_ltst_ep);
    if (id < 0) { it_fail("T054", "cap register"); return; }
    g_ltst_id = (uint32_t)id;

    /* Re-register the same name (with another cap) → BUSY; the rejected cap is
     * closed by svcmgr (no leak). */
    long busy = it_register_ep("ltst.svc", g_ltst_ep);
    if (busy == -(long)(uint32_t)IRIS_ERR_BUSY)
        it_pass("T054");
    else
        it_fail("T054", "re-register not BUSY");
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

/* ── Fase 11: endpoint cap-transfer & cap-backed REGISTER (T063–T066) ────── */

/* T063: LOOKUP of the cap-registered name returns a REAL, usable endpoint cap —
 * SYS_HANDLE_SAME_OBJECT proves it is the very endpoint object iris_test
 * created and transferred (not a forged number, and not the reply cap). */
static void test_t063(void) {
    if (g_ltst_ep == (handle_id_t)0) { it_fail("T063", "no ltst ep"); return; }
    uint32_t len = it_stage_path("ltst.svc");
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
        handle_id_t got = (handle_id_t)msg.attached_handle;
        long ty   = it_sys1(SYS_HANDLE_TYPE, (long)got);
        long same = it_sys2(SYS_HANDLE_SAME_OBJECT, (long)got, (long)g_ltst_ep);
        if (ty == (long)IRIS_HANDLE_TYPE_ENDPOINT && same == 1) ok = 1;
        it_close(&got);
    }
    if (ok) it_pass("T063"); else it_fail("T063", "lookup real cap");
}

/* T064: REGISTER without a cap fails (INVALID_ARG); REGISTER of a wrong-type
 * cap (a notification) also fails — only endpoints are registrable. */
static void test_t064(void) {
    int ok = 1;
    /* no cap */
    uint32_t len = it_stage_path("nocap.svc");
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_REGISTER;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;                        /* attached_cap = NO_CAP */
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
          msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG)) ok = 0;

    /* wrong type: transfer a notification cap */
    long n = it_sys0(SYS_NOTIFY_CREATE);
    if (n < 0) { it_fail("T064", "notify create"); return; }
    handle_id_t notif = (handle_id_t)n;
    long d = it_sys2(SYS_HANDLE_DUP, (long)notif, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    len = it_stage_path("wrongtype.svc");
    it_iris_msg_zero(&msg);
    msg.label               = IRIS_SVCMGR_EP_REGISTER;
    msg.buf_uptr            = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len             = len;
    msg.attached_cap        = (uint32_t)d;
    msg.attached_cap_rights = (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER);
    r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
          msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG)) ok = 0;
    it_close(&notif);

    if (ok) it_pass("T064"); else it_fail("T064", "register reject paths");
}

/* T065: UNREGISTER by a non-owner, non-supervisor badge (TEST_B via fixture
 * slot 28) is denied; UNREGISTER by the owner succeeds. */
static void test_t065(void) {
    int ok = 1;
    struct IrisMsg msg;
    /* wrong badge (0xB2) via the second-identity fixture → ACCESS_DENIED */
    it_iris_msg_zero(&msg);
    msg.label      = IRIS_SVCMGR_EP_UNREGISTER;
    msg.words[0]   = g_ltst_id;
    msg.word_count = 1u;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_TEST_FIX_C, (long)&msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
          msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_ACCESS_DENIED)) ok = 0;

    /* owner badge (IRIS_TEST via slot 1) → OK */
    it_iris_msg_zero(&msg);
    msg.label      = IRIS_SVCMGR_EP_UNREGISTER;
    msg.words[0]   = g_ltst_id;
    msg.word_count = 1u;
    r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_OK)) ok = 0;

    if (ok) it_pass("T065"); else it_fail("T065", "unregister owner policy");
}

/* T066: after UNREGISTER the name no longer resolves (NOT_FOUND). */
static void test_t066(void) {
    uint32_t len = it_stage_path("ltst.svc");
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg.buf_len  = len;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    int ok = (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
              msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND);
    if (g_ltst_ep != (handle_id_t)0) it_close(&g_ltst_ep);
    if (ok) it_pass("T066"); else it_fail("T066", "lookup after unregister");
}

/* ── Fase 12: endpoint-first svcmgr — DIAG over EP + no legacy fallback ──── */

/* T067: svcmgr DIAG over the endpoint (replaces legacy KChannel SVCMGR_MSG_DIAG
 * as the productive path) returns the expected catalog snapshot. */
static void test_t067(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_SVCMGR_EP_DIAG;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK && msg.word_count >= 4u &&
        msg.words[0] == 3u &&                       /* catalog: kbd/vfs/sh */
        msg.words[1] >= 3u &&                       /* all core services ready */
        msg.words[3] == (uint64_t)IRIS_SERVICE_CATALOG_VERSION)
        it_pass("T067");
    else
        it_fail("T067", "ep diag snapshot");
}

/* T068: an unknown/malformed svcmgr EP opcode fails cleanly with INVALID_ARG —
 * there is NO silent fallback to a legacy path and no hang/crash. */
static void test_t068(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = UINT64_C(0xF0FE);                   /* not a real svcmgr opcode */
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
        msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG)
        it_pass("T068");
    else
        it_fail("T068", "unknown opcode no-fallback");
}

/* ── Fase 13: device-cap CPtr resolution (T069) ─────────────────────────── */

/* T069: a device/authority cap (the spawn KBootstrapCap) minted into a CPtr
 * slot is invocable BY CPtr — SYS_INITRD_COUNT resolves it through CSpace
 * (cspace_or_handle_resolve_obj).  A wrong-type CPtr slot (slot 1 = endpoint)
 * is rejected with ACCESS_DENIED: the namespace split holds and there is no
 * fallback.  This is the prerequisite that lets device caps stop travelling
 * over KChannel at bootstrap. */
static void test_t069(void) {
    long ok_cnt = it_sys1(SYS_INITRD_COUNT, (long)IRIS_CPTR_TEST_SPAWN);
    long wrong  = it_sys1(SYS_INITRD_COUNT, (long)IRIS_CPTR_SVCMGR_EP);
    if (ok_cnt >= 0 && wrong == (long)IRIS_ERR_ACCESS_DENIED)
        it_pass("T069");
    else
        it_fail("T069", "device cap via cptr");
}

/* ── Fase 13 / Track G: retired SYS_CHAN ABI (T070) ─────────────────────── */

/* T070: the ENTIRE SYS_CHAN_* ABI is retired in Track G — KChannel is no longer
 * a productive IPC mechanism.  Every channel syscall number falls through the
 * dispatch to IRIS_ERR_NOT_SUPPORTED.  Args are irrelevant: the dispatch rejects
 * the syscall number before touching them.  Locks the reservation. */
static void test_t070(void) {
    int ok = 1;
    static const long chan_syscalls[] = {
        SYS_CHAN_CREATE, SYS_CHAN_SEND, SYS_CHAN_RECV, SYS_CHAN_RECV_NB,
        SYS_CHAN_SEAL, SYS_CHAN_RECV_TIMEOUT, SYS_CHAN_CALL,
        SYS_WAIT_ANY, SYS_WAIT_ANY_TIMEOUT,
    };
    for (uint32_t i = 0; i < sizeof(chan_syscalls) / sizeof(chan_syscalls[0]); i++) {
        if (it_sys3(chan_syscalls[i], 0, 0, 0) != (long)IRIS_ERR_NOT_SUPPORTED)
            ok = 0;
    }
    if (ok) it_pass("T070"); else it_fail("T070", "retired SYS_CHAN ABI");
}

/* ── T071: cascade revoke — SYS_CAP_DERIVE tree torn down by SYS_CAP_REVOKE ──
 *
 * Runtime coverage for the derivation-tree revoke path (SYS_CAP_DERIVE(78) /
 * SYS_CAP_REVOKE(79)), which previously had zero ring-3 coverage.  Proves that
 * revoking a root cap transitively invalidates every handle derived from it
 * (child + grandchild) while the root itself survives, and that the revoked
 * handles fail cleanly with IRIS_ERR_BAD_HANDLE (slot generation bumped) rather
 * than resolving to phantom authority. */
static void test_t071(void) {
    long root = it_sys0(SYS_ENDPOINT_CREATE);
    if (root < 0) { it_fail("T071", "ep create"); return; }
    handle_id_t root_h = (handle_id_t)root;

    /* child derived from root; grandchild derived from child.  RIGHT_SAME_RIGHTS
     * keeps RIGHT_DUPLICATE so the child can itself be a derivation source. */
    long child  = it_sys2(SYS_CAP_DERIVE, root,  (long)RIGHT_SAME_RIGHTS);
    long gchild = (child >= 0)
                ? it_sys2(SYS_CAP_DERIVE, child, (long)RIGHT_SAME_RIGHTS)
                : -1;

    int before_ok = (child >= 0) && (gchild >= 0)
                 && (it_sys1(SYS_HANDLE_TYPE, child)  >= 0)
                 && (it_sys1(SYS_HANDLE_TYPE, gchild) >= 0)
                 && (it_sys2(SYS_HANDLE_SAME_OBJECT, root, child) == 1);

    /* Revoke transitively deletes child + grandchild; root is not a child of
     * itself and must remain valid. */
    long rv = it_sys1(SYS_CAP_REVOKE, root);

    int child_dead  = (it_sys1(SYS_HANDLE_TYPE, child)  == (long)IRIS_ERR_BAD_HANDLE);
    int gchild_dead = (it_sys1(SYS_HANDLE_TYPE, gchild) == (long)IRIS_ERR_BAD_HANDLE);
    int root_alive  = (it_sys1(SYS_HANDLE_TYPE, root)   >= 0);

    it_close(&root_h);

    if (before_ok && rv == 0 && child_dead && gchild_dead && root_alive)
        it_pass("T071");
    else
        it_fail("T071", "cascade revoke");
}

/* ── T072: derivation rights reduction + revoke failure paths ───────────────
 *
 * Proves (a) a cap derived with reduced rights cannot itself be a derivation
 * source once RIGHT_DUPLICATE is dropped (ACCESS_DENIED — no rights escalation),
 * (b) SYS_CAP_REVOKE on a stale handle fails cleanly (negative error, no panic),
 * and (c) a valid revoke tears down the one child that exists. */
static void test_t072(void) {
    long root = it_sys0(SYS_ENDPOINT_CREATE);
    if (root < 0) { it_fail("T072", "ep create"); return; }
    handle_id_t root_h = (handle_id_t)root;

    /* Read-only child (drops DUPLICATE/TRANSFER). */
    long ro = it_sys2(SYS_CAP_DERIVE, root, (long)RIGHT_READ);
    /* Deriving from a cap without RIGHT_DUPLICATE must be denied. */
    long escalate = (ro >= 0)
                  ? it_sys2(SYS_CAP_DERIVE, ro, (long)RIGHT_SAME_RIGHTS)
                  : 0;

    /* Stale handle → clean BAD_HANDLE (create+close to guarantee staleness). */
    long tmp = it_sys0(SYS_ENDPOINT_CREATE);
    if (tmp >= 0) it_sys1(SYS_HANDLE_CLOSE, tmp);
    long bad_revoke = (tmp >= 0) ? it_sys1(SYS_CAP_REVOKE, tmp) : -1;

    /* Valid revoke of root deletes its single child (ro). */
    long ok_revoke = it_sys1(SYS_CAP_REVOKE, root);
    int  ro_dead   = (it_sys1(SYS_HANDLE_TYPE, ro) == (long)IRIS_ERR_BAD_HANDLE);

    it_close(&root_h);

    if (ro >= 0 && escalate == (long)IRIS_ERR_ACCESS_DENIED &&
        bad_revoke < 0 && ok_revoke == 0 && ro_dead)
        it_pass("T072");
    else
        it_fail("T072", "derive rights / revoke error paths");
}

/* ── T073: IPC staged-cap cleanup on failure paths ──────────────────────────
 *
 * A capability attached to an outbound IPC message is "staged" (validated and
 * detached from the sender) before delivery.  This proves the staging FAILURE
 * paths leave no phantom authority and no half-transferred handle:
 *   (a) attaching a cap without RIGHT_TRANSFER → ACCESS_DENIED, and the source
 *       handle is NOT consumed (still resolvable afterwards);
 *   (b) attaching a stale handle → BAD_HANDLE, clean failure.
 * EP_NB_SEND is used so the call never blocks: the staging check runs and fails
 * before any rendezvous or enqueue. */
static void test_t073(void) {
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T073", "ep create"); return; }
    handle_id_t ep_h = (handle_id_t)ep;

    /* (a) A cap WITHOUT RIGHT_TRANSFER: dup the endpoint down to READ only. */
    long notrans = it_sys2(SYS_HANDLE_DUP, ep, (long)RIGHT_READ);
    if (notrans < 0) { it_close(&ep_h); it_fail("T073", "dup"); return; }
    handle_id_t notrans_h = (handle_id_t)notrans;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label           = 0x73;
    msg.attached_handle = (uint32_t)notrans;
    msg.attached_rights = (uint32_t)RIGHT_READ;
    long a = it_sys2(SYS_EP_NB_SEND, ep, (long)&msg);
    int  denied    = (a == (long)IRIS_ERR_ACCESS_DENIED);
    int  preserved = (it_sys1(SYS_HANDLE_TYPE, notrans) >= 0);  /* not consumed */

    /* (b) Stale attached handle → clean BAD_HANDLE.  Create+close to make the
     * id deterministically invalid. */
    long stale = it_sys0(SYS_ENDPOINT_CREATE);
    if (stale >= 0) it_sys1(SYS_HANDLE_CLOSE, stale);
    struct IrisMsg msg2;
    it_iris_msg_zero(&msg2);
    msg2.label           = 0x73;
    msg2.attached_handle = (uint32_t)stale;
    msg2.attached_rights = (uint32_t)RIGHT_TRANSFER;
    long b = it_sys2(SYS_EP_NB_SEND, ep, (long)&msg2);
    int  bogus_clean = (b == (long)IRIS_ERR_BAD_HANDLE);

    it_close(&notrans_h);
    it_close(&ep_h);

    if (denied && preserved && bogus_clean)
        it_pass("T073");
    else
        it_fail("T073", "staged cap failure cleanup");
}

/* ── T074: reply capability one-shot lifecycle ──────────────────────────────
 *
 * A KReply capability delivered by EP_CALL rendezvous is one-shot: it may unblock
 * its caller exactly once.  A server thread receives the call, replies once, then
 * invokes SYS_REPLY again on the same (now-consumed) reply cap.  The main thread's
 * EP_CALL must return 0 (first reply landed); the second SYS_REPLY must fail with
 * IRIS_ERR_NOT_FOUND — proving the reply cap cannot re-unblock a caller and leaves
 * no dangling reply authority.  (Mirrors the T016 EP_CALL/REPLY rendezvous.) */
static handle_id_t g_t074_ep_h  = HANDLE_INVALID;
static volatile int g_t074_done = 0;
static          int g_t074_r1   = 999;
static          int g_t074_r2   = 999;
static uint8_t      g_t074_stack[8192];

static void t074_server(void) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long rr = it_sys2(SYS_EP_RECV, (long)g_t074_ep_h, (long)&msg);
    if (rr == 0) {
        handle_id_t reply_h = (handle_id_t)msg.attached_handle;
        struct IrisMsg rmsg;
        it_iris_msg_zero(&rmsg);
        rmsg.label = 0x74;
        g_t074_r1 = (int)it_sys2(SYS_REPLY, (long)reply_h, (long)&rmsg);
        /* Second reply on the consumed one-shot cap must be rejected. */
        struct IrisMsg rmsg2;
        it_iris_msg_zero(&rmsg2);
        rmsg2.label = 0x74;
        g_t074_r2 = (int)it_sys2(SYS_REPLY, (long)reply_h, (long)&rmsg2);
    } else {
        g_t074_r1 = (int)rr;
    }
    g_t074_done = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t074(void) {
    g_t074_done = 0; g_t074_r1 = 999; g_t074_r2 = 999;

    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T074", "ep create"); return; }
    g_t074_ep_h = (handle_id_t)ep;

    uint64_t entry = (uint64_t)(uintptr_t)t074_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t074_stack + sizeof(g_t074_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) { it_close(&g_t074_ep_h); it_fail("T074", "thread create"); return; }
    handle_id_t tid_h = (handle_id_t)tid;

    uint8_t reply_buf[64];
    for (uint32_t i = 0; i < 64; i++) reply_buf[i] = 0;

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = 0x74;
    msg.buf_uptr = (uint64_t)(uintptr_t)reply_buf;
    long r = it_sys2(SYS_EP_CALL, ep, (long)&msg);

    for (int i = 0; i < 200 && !g_t074_done; i++)
        it_sys1(SYS_SLEEP, 1);

    it_close(&tid_h);
    it_close(&g_t074_ep_h);

    if (r == 0 && g_t074_r1 == 0 && g_t074_r2 == (int)IRIS_ERR_NOT_FOUND)
        it_pass("T074");
    else
        it_fail("T074", "reply one-shot");
}

/* ── Bootstrap ──────────────────────────────────────────────────────────── */

/*
 * Receives the SPAWN_CAP bootstrap cap from init (serial/test loading).
 * Timeout-bounded so a missing message degrades to HANDLE_INVALID — the
 * dependent tests then FAIL loudly instead of hanging boot or being
 * silently skipped.  (Fase 8: the discovery endpoint no longer arrives
 * here; it is the well-known slot IRIS_CPTR_SVCMGR_EP.)
 */
/* it_recv_bootstrap retired — Fase 13/Track I: the spawn cap is the
 * IRIS_CPTR_SPAWN_CAP pre-start mint, not a bootstrap KChannel message. */

/* ── Entry point ────────────────────────────────────────────────────────── */

void iris_test_main(handle_id_t bootstrap_ch_h) {
    /* Fase 13 (Track I): the spawn/authority cap arrives as the
     * IRIS_CPTR_SPAWN_CAP (slot 6) pre-start mint — no bootstrap KChannel.
     * SYS_CAP_CREATE_IOPORT resolves it by CPtr via the device-cap dual
     * resolver (the serial KIoPort for test output). */
    it_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_ch_h);

    {
        long h = it_sys3(SYS_CAP_CREATE_IOPORT,
                         (long)IRIS_CPTR_SPAWN_CAP, 0x3F8, 8);
        if (h >= 0) g_serial_h = (handle_id_t)h;
    }

    it_serial_write("[IRIS][TEST] start\n");

    /* Run all tests */
    test_t001();
    test_t002();
    test_t003();
    /* T004-T007 retired (Fase 13/Track F): the KChannel send/recv, NB-recv,
     * recv-timeout and seal/close semantics are covered by the endpoint /
     * notification equivalents — T015 (EP_SEND/RECV), T014 (EP_NB_RECV empty
     * → WOULD_BLOCK), T016 (EP_CALL/REPLY), T019 (endpoint close wakes a
     * blocked recv) and T010 (NOTIFY_WAIT_TIMEOUT → TIMED_OUT). */
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
    test_t063();
    test_t064();
    test_t065();
    test_t066();
    test_t067();
    test_t068();
    test_t069();
    test_t070();
    test_t071();
    test_t072();
    test_t073();
    test_t074();

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
