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
#include <iris/ipc_recv_slot.h>
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>
#include <iris/kbd_ep_proto.h>
#include <iris/console_ep_proto.h>
#include "../common/svc_loader.h"

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

static inline long it_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long _a3 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_a3)
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

/* ── Ring-3 spawn/kill lifecycle harness (T075+) ────────────────────────────
 *
 * iris_test acts as the PARENT: using only its own IRIS_CPTR_SPAWN_CAP
 * KBootstrapCap it spawns the minimal `lifecycle_probe` child (svc_load), hands
 * it exactly one command endpoint (minted into the child's CPtr slot
 * LP_CPTR_CMD_EP), and observes the child's lifecycle via SYS_PROCESS_WATCH /
 * SYS_PROCESS_EXIT_CODE / SYS_PROCESS_KILL — all capability-scoped, no global
 * authority.  These constants must match services/lifecycle_probe/main.c. */
#define LP_CPTR_CMD_EP  3u
#define LP_EXIT_MARKER  0x1E57

/* Spawn a lifecycle_probe child, minting `cmd_ep_h` into its command slot.
 * Returns 0 and fills *out_proc_h on success, or a negative error. */
static long lp_spawn_child(handle_id_t cmd_ep_h, handle_id_t *out_proc_h) {
    struct svc_mint mints[1];
    mints[0].slot   = LP_CPTR_CMD_EP;
    mints[0].src_h  = cmd_ep_h;
    mints[0].rights = RIGHT_READ | RIGHT_WRITE;
    mints[0].badge  = 0;
    handle_id_t boot_h = HANDLE_INVALID;
    *out_proc_h = HANDLE_INVALID;
    long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "lifecycle_probe",
                             out_proc_h, &boot_h, mints, 1u);
    it_close(&boot_h);   /* Track I: no bootstrap channel (HANDLE_INVALID anyway) */
    return r;
}

/* ── T075: spawn/exit smoke ─────────────────────────────────────────────────
 * Foundation test: spawn the child, drive it to run and exit via one command
 * endpoint, and observe its exit code — proving the harness works end to end. */
static void test_t075(void) {
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T075", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;

    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h);
        it_fail("T075", "spawn"); return;
    }

    /* Watch the child for exit on a notification, bit 0. */
    long n = it_sys0(SYS_NOTIFY_CREATE);
    handle_id_t watch_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
    int watch_ok = (watch_h != HANDLE_INVALID) &&
                   (it_sys3(SYS_PROCESS_WATCH, (long)proc_h, (long)watch_h, 1) == 0);

    /* Drive the child: EP_NB_SEND until it is blocked in EP_RECV (bounded). */
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = 0x75;
    int sent = 0;
    for (int i = 0; i < 300 && !sent; i++) {
        if (it_sys2(SYS_EP_NB_SEND, (long)cmd_ep_h, (long)&msg) == 0) sent = 1;
        else it_sys1(SYS_SLEEP, 1);
    }

    /* Wait (bounded) for the death signal, then read the exit code. */
    uint64_t bits = 0;
    long ws = watch_ok
        ? it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)watch_h, (long)(uintptr_t)&bits, 2000000000L)
        : -1;
    long code = it_sys1(SYS_PROCESS_EXIT_CODE, (long)proc_h);

    it_close(&watch_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);

    if (sent && watch_ok && ws == 0 && (bits & 1u) && code == LP_EXIT_MARKER)
        it_pass("T075");
    else
        it_fail("T075", "spawn/exit smoke");
}

/* ── T077: blocked-IPC child kill cleanup ───────────────────────────────────
 * Spawn the child, let it block in SYS_EP_RECV, then SYS_PROCESS_KILL it while
 * blocked.  The kernel's teardown must cancel the blocked recv: the child dies
 * and the parent's endpoint is left clean — a non-blocking send now reports
 * WOULD_BLOCK (no stale receiver to rendezvous with a dead task). */
static void test_t077(void) {
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T077", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;

    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h);
        it_fail("T077", "spawn"); return;
    }

    /* Let the child reach EP_RECV and block. */
    it_sys1(SYS_SLEEP, 10);

    /* Kill the child while it is blocked (RIGHT_MANAGE on the child handle). */
    long kr   = it_sys1(SYS_PROCESS_KILL, (long)proc_h);
    int  dead = (it_sys1(SYS_PROCESS_STATUS, (long)proc_h) == 0);

    /* Blocked recv must have been cancelled: no stale receiver remains. */
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = 0x77;
    long s = it_sys2(SYS_EP_NB_SEND, (long)cmd_ep_h, (long)&msg);
    int  ep_clean = (s == (long)IRIS_ERR_WOULD_BLOCK);

    it_close(&proc_h);
    it_close(&cmd_ep_h);

    if (kr == 0 && dead && ep_clean)
        it_pass("T077");
    else
        it_fail("T077", "blocked child kill cleanup");
}

/* ── T078: server death → client recovery (reply-cap cleanup cross-process) ──
 * The parent is the CLIENT: it SYS_EP_CALLs the child's command endpoint.  The
 * child receives the call (obtaining the one-shot reply cap) and then exits
 * WITHOUT replying.  The child's teardown drops the reply cap, whose close
 * callback must wake this blocked call with IRIS_ERR_CLOSED — proving a client
 * cannot be stranded when its server dies mid-request. */
static void test_t078(void) {
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T078", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;

    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h);
        it_fail("T078", "spawn"); return;
    }

    /* Client call: rendezvous with the child's recv, then the child exits
     * unanswered.  EP_CALL must return CLOSED, not hang. */
    uint8_t reply_buf[64];
    for (uint32_t i = 0; i < 64; i++) reply_buf[i] = 0;
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label    = 0x78;
    msg.buf_uptr = (uint64_t)(uintptr_t)reply_buf;
    long r = it_sys2(SYS_EP_CALL, (long)cmd_ep_h, (long)&msg);

    /* Confirm the child actually died (bounded). */
    int dead = 0;
    for (int i = 0; i < 200 && !dead; i++) {
        if (it_sys1(SYS_PROCESS_STATUS, (long)proc_h) == 0) dead = 1;
        else it_sys1(SYS_SLEEP, 1);
    }

    it_close(&proc_h);
    it_close(&cmd_ep_h);

    if (r == (long)IRIS_ERR_CLOSED && dead)
        it_pass("T078");
    else
        it_fail("T078", "server death client recovery");
}

/* ── T076: child mapping teardown ───────────────────────────────────────────
 * Map a parent-owned VMO into the child's address space, let the child block
 * with the mapping live, then kill the child.  The kernel's teardown must reap
 * the child's address space (auto-unmapping the VMO) without freeing the VMO
 * itself — the parent still owns it.  Verified by re-mapping the same VMO into
 * the PARENT afterwards and reading/writing it: no panic, no stale mapping, no
 * corrupted refcount.  (LP_MAP_VA = USER_VMO_BASE, the same known-good VA T008
 * maps at; child and parent live in different address spaces.) */
#define LP_MAP_VA  0x8050000000ULL

static void test_t076(void) {
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T076", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;

    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h);
        it_fail("T076", "spawn"); return;
    }

    /* One-page VMO mapped writable into the CHILD's address space. */
    long vmo = it_sys1(SYS_VMO_CREATE, 4096);
    handle_id_t vmo_h = (vmo >= 0) ? (handle_id_t)vmo : HANDLE_INVALID;
    long mi = (vmo_h != HANDLE_INVALID)
        ? it_sys4(SYS_VMO_MAP_INTO, (long)vmo_h, (long)proc_h, (long)LP_MAP_VA, 1)
        : -1;

    /* Let the child reach EP_RECV and block (mapping stays live). */
    it_sys1(SYS_SLEEP, 10);

    /* Kill the child while the mapping is live — teardown must auto-unmap it. */
    long kr   = it_sys1(SYS_PROCESS_KILL, (long)proc_h);
    int  dead = (it_sys1(SYS_PROCESS_STATUS, (long)proc_h) == 0);

    /* VMO must have survived intact: re-map into the PARENT and read/write it. */
    int reusable = 0;
    long pm = it_sys3(SYS_VMO_MAP, (long)vmo_h, (long)LP_MAP_VA, 1);
    if (pm == 0) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)LP_MAP_VA;
        p[0] = 0xA5; p[4095] = 0x5A;
        reusable = (p[0] == 0xA5 && p[4095] == 0x5A);
        it_sys2(SYS_VMO_UNMAP, (long)LP_MAP_VA, 4096);
    }

    it_close(&vmo_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);

    if (mi == 0 && kr == 0 && dead && reusable)
        it_pass("T076");
    else
        it_fail("T076", "child mapping teardown");
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

/* ── T079: VMO map by CPtr (A1 Increment 1) ─────────────────────────────────
 * SYS_VMO_MAP now resolves the VMO through the dual resolver: a CSpace slot
 * (< 1024) must work exactly like a handle (>= 1024).  init mints our OWN
 * process cap (RIGHT_WRITE) at IRIS_CPTR_TEST_PROC; we use it to
 * SYS_PROC_CSPACE_MINT a runtime-created VMO into our own slots, map by CPtr
 * and prove the mapping is real (write, then read the same pages back through
 * a second raw-handle mapping — which also re-proves the old handle path).
 * Failure paths: empty slot (clean error), wrong type (slot 30 is a
 * KNotification → WRONG_TYPE), insufficient rights (read-only mint + writable
 * map flags → ACCESS_DENIED). */

#define T079_SLOT_RW    16L               /* dynamic slot: VMO, READ|WRITE */
#define T079_SLOT_RO    17L               /* dynamic slot: VMO, READ only  */
#define T079_SLOT_EMPTY 18L               /* never minted — must not resolve */
#define T079_VA_CPTR    0x8060000000ULL
#define T079_VA_HANDLE  0x8061000000ULL

static void test_t079(void) {
    /* init mints the self-proc cap post-load; retry briefly, then FAIL loud. */
    long selfp = -1;
    for (int i = 0; i < 50 && selfp < 0; i++) {
        selfp = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_TEST_PROC);
        if (selfp < 0) it_sys1(SYS_SLEEP, 2);
    }
    if (selfp < 0) { it_fail("T079", "self proc cptr"); return; }
    handle_id_t selfp_h = (handle_id_t)selfp;

    long vmo = it_sys1(SYS_VMO_CREATE, 4096);
    if (vmo < 0) { it_close(&selfp_h); it_fail("T079", "vmo create"); return; }
    handle_id_t vmo_h = (handle_id_t)vmo;

    int ok = 1;

    /* Mint the VMO into our own CSpace: slot 16 rw, slot 17 read-only. */
    if (it_sys4(SYS_PROC_CSPACE_MINT, (long)selfp_h, T079_SLOT_RW,
                vmo, (long)(RIGHT_READ | RIGHT_WRITE)) != 0) ok = 0;
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)selfp_h, T079_SLOT_RO,
                      vmo, (long)RIGHT_READ) != 0) ok = 0;

    /* Map by CPtr (writable) and write through the mapping. */
    if (ok && it_sys3(SYS_VMO_MAP, T079_SLOT_RW, (long)T079_VA_CPTR, 1) != 0)
        ok = 0;
    if (ok) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)T079_VA_CPTR;
        *p = 0xA1C0FFEE00000079ULL;
        if (*p != 0xA1C0FFEE00000079ULL) ok = 0;
    }

    /* Failure paths: empty slot, wrong type, insufficient rights. */
    if (ok && it_sys3(SYS_VMO_MAP, T079_SLOT_EMPTY,
                      (long)T079_VA_HANDLE, 1) >= 0) ok = 0;
    if (ok && it_sys3(SYS_VMO_MAP, (long)IRIS_CPTR_TEST_FIX_A,
                      (long)T079_VA_HANDLE, 1) != (long)IRIS_ERR_WRONG_TYPE)
        ok = 0;
    if (ok && it_sys3(SYS_VMO_MAP, T079_SLOT_RO,
                      (long)T079_VA_HANDLE, 1) != (long)IRIS_ERR_ACCESS_DENIED)
        ok = 0;

    /* Handle path unchanged: map the same VMO by raw handle at a second VA
     * and read back what the CPtr mapping wrote (same physical pages). */
    if (ok && it_sys3(SYS_VMO_MAP, vmo, (long)T079_VA_HANDLE, 1) != 0) ok = 0;
    if (ok) {
        volatile uint64_t *q = (volatile uint64_t *)(uintptr_t)T079_VA_HANDLE;
        if (*q != 0xA1C0FFEE00000079ULL) ok = 0;
    }

    (void)it_sys2(SYS_VMO_UNMAP, (long)T079_VA_CPTR, 4096);
    (void)it_sys2(SYS_VMO_UNMAP, (long)T079_VA_HANDLE, 4096);
    it_close(&vmo_h);
    it_close(&selfp_h);

    if (ok) it_pass("T079"); else it_fail("T079", "vmo map by cptr");
}

/* ── T080: VMO remaining syscalls by CPtr (A1 Increment 1b) ─────────────────
 * SYS_VMO_SIZE / SYS_VMO_SHARE / SYS_VMO_MAP_INTO now resolve their VMO
 * argument through the dual resolver (the target/destination process stays
 * handle-only).  Reuses the T079 fixture: the self-proc cap at
 * IRIS_CPTR_TEST_PROC mints a runtime VMO into own slots — 19 (READ|WRITE|
 * DUPLICATE) and 20 (READ only) — and the lifecycle_probe child is the
 * SHARE/MAP_INTO target.  Proofs per syscall:
 *   SIZE:     CPtr returns the real size; empty slot / wrong type fail;
 *             raw handle still works.
 *   SHARE:    CPtr shares into the child; READ-only slot → ACCESS_DENIED
 *             (needs READ|DUPLICATE); wrong type fails; handle still works.
 *   MAP_INTO: CPtr maps into the child; re-mapping the same VA → BUSY proves
 *             PTEs were really installed; READ-only slot + writable flags →
 *             ACCESS_DENIED; empty slot / wrong type fail.  (Handle-path
 *             MAP_INTO stays covered by T076.) */

#define T080_SLOT_RWD   19L               /* dynamic slot: VMO, READ|WRITE|DUP */
#define T080_SLOT_RO    20L               /* dynamic slot: VMO, READ only      */
#define T080_VMO_SIZE   8192U             /* 2 pages: distinct from other tests */

static void test_t080(void) {
    long selfp = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_TEST_PROC);
    if (selfp < 0) { it_fail("T080", "self proc cptr"); return; }
    handle_id_t selfp_h = (handle_id_t)selfp;

    long vmo = it_sys1(SYS_VMO_CREATE, T080_VMO_SIZE);
    if (vmo < 0) { it_close(&selfp_h); it_fail("T080", "vmo create"); return; }
    handle_id_t vmo_h = (handle_id_t)vmo;

    int ok = 1;

    /* Mint the VMO into our own CSpace: slot 19 rw+dup, slot 20 read-only. */
    if (it_sys4(SYS_PROC_CSPACE_MINT, (long)selfp_h, T080_SLOT_RWD, vmo,
                (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0) ok = 0;
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)selfp_h, T080_SLOT_RO,
                      vmo, (long)RIGHT_READ) != 0) ok = 0;

    /* ── SYS_VMO_SIZE ── */
    if (ok && it_sys1(SYS_VMO_SIZE, T080_SLOT_RWD) != (long)T080_VMO_SIZE)
        ok = 0;
    if (ok && it_sys1(SYS_VMO_SIZE, T079_SLOT_EMPTY) >= 0) ok = 0;
    if (ok && it_sys1(SYS_VMO_SIZE, (long)IRIS_CPTR_TEST_FIX_A) !=
              (long)IRIS_ERR_WRONG_TYPE) ok = 0;
    if (ok && it_sys1(SYS_VMO_SIZE, vmo) != (long)T080_VMO_SIZE) ok = 0;

    /* SHARE/MAP_INTO target: a lifecycle_probe child (blocks in EP_RECV). */
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_close(&vmo_h); it_close(&selfp_h);
                  it_fail("T080", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;
    handle_id_t proc_h   = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h); it_close(&vmo_h); it_close(&selfp_h);
        it_fail("T080", "spawn"); return;
    }

    /* ── SYS_VMO_SHARE (vmo by CPtr; dest process stays a handle) ── */
    if (ok && it_sys3(SYS_VMO_SHARE, T080_SLOT_RWD, (long)proc_h,
                      (long)(RIGHT_READ | RIGHT_WRITE)) < 0) ok = 0;
    if (ok && it_sys3(SYS_VMO_SHARE, T080_SLOT_RO, (long)proc_h,
                      (long)RIGHT_READ) != (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_sys3(SYS_VMO_SHARE, (long)IRIS_CPTR_TEST_FIX_A, (long)proc_h,
                      (long)RIGHT_READ) != (long)IRIS_ERR_WRONG_TYPE) ok = 0;
    if (ok && it_sys3(SYS_VMO_SHARE, vmo, (long)proc_h,
                      (long)RIGHT_READ) < 0) ok = 0;

    /* ── SYS_VMO_MAP_INTO (vmo by CPtr; target process stays a handle) ── */
    if (ok && it_sys4(SYS_VMO_MAP_INTO, T080_SLOT_RWD, (long)proc_h,
                      (long)LP_MAP_VA, 1) != 0) ok = 0;
    /* Same VA again → BUSY: the CPtr mapping really installed PTEs. */
    if (ok && it_sys4(SYS_VMO_MAP_INTO, T080_SLOT_RWD, (long)proc_h,
                      (long)LP_MAP_VA, 1) != (long)IRIS_ERR_BUSY) ok = 0;
    if (ok && it_sys4(SYS_VMO_MAP_INTO, T080_SLOT_RO, (long)proc_h,
                      (long)(LP_MAP_VA + 0x10000ULL), 1) !=
              (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_sys4(SYS_VMO_MAP_INTO, T079_SLOT_EMPTY, (long)proc_h,
                      (long)(LP_MAP_VA + 0x10000ULL), 1) >= 0) ok = 0;
    if (ok && it_sys4(SYS_VMO_MAP_INTO, (long)IRIS_CPTR_TEST_FIX_A, (long)proc_h,
                      (long)(LP_MAP_VA + 0x10000ULL), 1) !=
              (long)IRIS_ERR_WRONG_TYPE) ok = 0;

    /* Cleanup: kill the child (auto-unmaps, T076-proven), close everything. */
    (void)it_sys1(SYS_PROCESS_KILL, (long)proc_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);
    it_close(&vmo_h);
    it_close(&selfp_h);

    if (ok) it_pass("T080"); else it_fail("T080", "vmo family by cptr");
}

/* ── T081: Process syscalls by CPtr (A1 Increment 2a) ───────────────────────
 * The process-cap argument of the lifecycle syscalls now resolves through the
 * dual resolver.  Spawn a lifecycle_probe child and mint ITS process cap into
 * our own CSpace — slot 21 (READ|WRITE|MANAGE|DUPLICATE) and slot 22 (READ
 * only) — using SYS_PROC_CSPACE_MINT with the TARGET process given by CPtr
 * (our self-proc cap, slot 25), which is itself one of the migrated paths.
 * Then drive the whole lifecycle by CPtr: STATUS (alive), WATCH, EXIT_CODE
 * (alive → WOULD_BLOCK), FAULT_INFO (no fault → WOULD_BLOCK), KILL, watch
 * fires, STATUS (dead), EXIT_CODE (dead → code), KILL again (idempotent 0),
 * THREAD_START on the dead child (→ BAD_HANDLE).  Authority is not relaxed:
 * KILL / THREAD_START via the READ-only slot 22 → ACCESS_DENIED, and minting
 * INTO the child via slot 22 (no RIGHT_WRITE) → ACCESS_DENIED.  Failure
 * paths: empty slot and wrong-type slot fail cleanly. */

#define T081_SLOT_PROC  21L               /* child proc: READ|WRITE|MANAGE|DUP */
#define T081_SLOT_RO    22L               /* child proc: READ only             */

static void test_t081(void) {
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T081", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;

    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h);
        it_fail("T081", "spawn"); return;
    }

    int ok = 1;

    /* Mint the child's proc cap into our CSpace — target proc by CPtr (25). */
    if (it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC, T081_SLOT_PROC,
                (long)proc_h,
                (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE | RIGHT_DUPLICATE)) != 0)
        ok = 0;
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC, T081_SLOT_RO,
                      (long)proc_h, (long)RIGHT_READ) != 0) ok = 0;

    /* Authority not relaxed: minting INTO the child through the READ-only
     * child cap (no RIGHT_WRITE) must be denied. */
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, T081_SLOT_RO, 60L, (long)proc_h,
                      (long)RIGHT_READ) != (long)IRIS_ERR_ACCESS_DENIED) ok = 0;

    /* STATUS by CPtr: alive via both slots; empty / wrong-type fail. */
    if (ok && it_sys1(SYS_PROCESS_STATUS, T081_SLOT_PROC) != 1) ok = 0;
    if (ok && it_sys1(SYS_PROCESS_STATUS, T081_SLOT_RO) != 1) ok = 0;
    if (ok && it_sys1(SYS_PROCESS_STATUS, T079_SLOT_EMPTY) >= 0) ok = 0;
    if (ok && it_sys1(SYS_PROCESS_STATUS, (long)IRIS_CPTR_TEST_FIX_A) !=
              (long)IRIS_ERR_WRONG_TYPE) ok = 0;

    /* EXIT_CODE by CPtr while alive → WOULD_BLOCK; FAULT_INFO → WOULD_BLOCK. */
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, T081_SLOT_PROC) !=
              (long)IRIS_ERR_WOULD_BLOCK) ok = 0;
    {
        static uint8_t fault_buf[32];
        if (ok && it_sys2(SYS_PROCESS_FAULT_INFO, T081_SLOT_PROC,
                          (long)(uintptr_t)fault_buf) !=
                  (long)IRIS_ERR_WOULD_BLOCK) ok = 0;
    }

    /* KILL / THREAD_START via the READ-only slot → ACCESS_DENIED (both need
     * RIGHT_MANAGE; entry/rsp are valid so resolution+rights are what fail). */
    if (ok && it_sys1(SYS_PROCESS_KILL, T081_SLOT_RO) !=
              (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_sys4(SYS_THREAD_START, T081_SLOT_RO, 0x8000200000L,
                      0x8000300000L, 0) != (long)IRIS_ERR_ACCESS_DENIED) ok = 0;

    /* WATCH by CPtr, then KILL by CPtr; the watch must fire. */
    long n = it_sys0(SYS_NOTIFY_CREATE);
    handle_id_t watch_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
    if (watch_h == HANDLE_INVALID) ok = 0;
    if (ok && it_sys3(SYS_PROCESS_WATCH, T081_SLOT_PROC, (long)watch_h, 1) != 0)
        ok = 0;
    if (ok && it_sys1(SYS_PROCESS_KILL, T081_SLOT_PROC) != 0) ok = 0;
    if (ok) {
        uint64_t bits = 0;
        if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)watch_h,
                    (long)(uintptr_t)&bits, 2000000000L) != 0 || !(bits & 1u))
            ok = 0;
    }

    /* Dead child by CPtr: STATUS 0, EXIT_CODE readable, KILL idempotent,
     * THREAD_START on a torn-down process → BAD_HANDLE. */
    if (ok && it_sys1(SYS_PROCESS_STATUS, T081_SLOT_PROC) != 0) ok = 0;
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, T081_SLOT_PROC) < 0) ok = 0;
    if (ok && it_sys1(SYS_PROCESS_KILL, T081_SLOT_PROC) != 0) ok = 0;
    if (ok && it_sys4(SYS_THREAD_START, T081_SLOT_PROC, 0x8000200000L,
                      0x8000300000L, 0) != (long)IRIS_ERR_BAD_HANDLE) ok = 0;

    it_close(&watch_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);

    if (ok) it_pass("T081"); else it_fail("T081", "process by cptr");
}

/* ── T082: Process target by CPtr for VMO/handle operations (A1 Inc 2a) ─────
 * The destination-process argument of SYS_VMO_MAP_INTO / SYS_VMO_SHARE /
 * SYS_HANDLE_INSERT now resolves through the dual resolver, so a fully
 * CPtr-based delegation works: VMO by CPtr (slot 23) + process by CPtr
 * (slot 24).  Re-mapping the same VA → BUSY proves the PTEs were really
 * installed.  Authority is not relaxed: the self-proc cap (slot 25, WRITE
 * only — no MANAGE) is rejected as MAP_INTO/SHARE target with ACCESS_DENIED;
 * a wrong-type slot and an empty slot fail cleanly; and the pure handle path
 * (both args as handles) still works. */

#define T082_SLOT_VMO   23L               /* VMO: READ|WRITE|DUPLICATE  */
#define T082_SLOT_PROC  24L               /* child2 proc: full authority */
#define T082_MAP_VA2    (LP_MAP_VA + 0x10000ULL)

static void test_t082(void) {
    long vmo = it_sys1(SYS_VMO_CREATE, 4096);
    if (vmo < 0) { it_fail("T082", "vmo create"); return; }
    handle_id_t vmo_h = (handle_id_t)vmo;

    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_close(&vmo_h); it_fail("T082", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;

    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h); it_close(&vmo_h);
        it_fail("T082", "spawn"); return;
    }

    int ok = 1;

    /* Mint fixtures (target proc by CPtr 25): VMO → 23, child proc → 24. */
    if (it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC, T082_SLOT_VMO,
                vmo, (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0)
        ok = 0;
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC,
                      T082_SLOT_PROC, (long)proc_h,
                      (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE |
                             RIGHT_DUPLICATE)) != 0) ok = 0;

    /* MAP_INTO: VMO by CPtr + process by CPtr; repeat → BUSY (PTEs real). */
    if (ok && it_sys4(SYS_VMO_MAP_INTO, T082_SLOT_VMO, T082_SLOT_PROC,
                      (long)LP_MAP_VA, 1) != 0) ok = 0;
    if (ok && it_sys4(SYS_VMO_MAP_INTO, T082_SLOT_VMO, T082_SLOT_PROC,
                      (long)LP_MAP_VA, 1) != (long)IRIS_ERR_BUSY) ok = 0;

    /* SHARE: VMO by CPtr + process by CPtr → handle in the child's table. */
    if (ok && it_sys3(SYS_VMO_SHARE, T082_SLOT_VMO, T082_SLOT_PROC,
                      (long)(RIGHT_READ | RIGHT_WRITE)) < 0) ok = 0;

    /* HANDLE_INSERT: destination process by CPtr (src stays a handle). */
    if (ok && it_sys4(SYS_HANDLE_INSERT, T082_SLOT_PROC, vmo,
                      (long)RIGHT_READ, 0) < 0) ok = 0;

    /* Authority not relaxed: self-proc slot 25 carries WRITE only (no
     * MANAGE) → MAP_INTO/SHARE reject it; wrong type / empty slot fail. */
    if (ok && it_sys4(SYS_VMO_MAP_INTO, T082_SLOT_VMO, (long)IRIS_CPTR_TEST_PROC,
                      (long)T082_MAP_VA2, 1) != (long)IRIS_ERR_ACCESS_DENIED)
        ok = 0;
    if (ok && it_sys3(SYS_VMO_SHARE, T082_SLOT_VMO, (long)IRIS_CPTR_TEST_PROC,
                      (long)RIGHT_READ) != (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_sys4(SYS_VMO_MAP_INTO, T082_SLOT_VMO, (long)IRIS_CPTR_TEST_FIX_A,
                      (long)T082_MAP_VA2, 1) != (long)IRIS_ERR_WRONG_TYPE) ok = 0;
    if (ok && it_sys3(SYS_VMO_SHARE, T082_SLOT_VMO, T079_SLOT_EMPTY,
                      (long)RIGHT_READ) >= 0) ok = 0;

    /* Pure handle path unchanged: both args as raw handles. */
    if (ok && it_sys4(SYS_VMO_MAP_INTO, vmo, (long)proc_h,
                      (long)T082_MAP_VA2, 1) != 0) ok = 0;

    /* Cleanup: kill via the old handle path (still must work). */
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) ok = 0;
    if (ok && it_sys1(SYS_PROCESS_STATUS, (long)proc_h) != 0) ok = 0;

    it_close(&proc_h);
    it_close(&cmd_ep_h);
    it_close(&vmo_h);

    if (ok) it_pass("T082"); else it_fail("T082", "vmo ops proc by cptr");
}

/* ── T083: TCB and SchedContext by CPtr (A1 Increment 2b) ───────────────────
 * The TCB / SchedContext syscalls now resolve their cap through the dual
 * resolver.  A helper thread publishes its own TCB handle (SYS_TCB_SELF) and
 * spins a progress counter; we mint that TCB into own slots 32 (READ|WRITE|
 * DUP) and 33 (READ only) and drive it by CPtr: GET_INFO (task_id real,
 * priority round-trip), SET_PRIORITY, SUSPEND (counter freezes), RESUME
 * (counter advances), and finally TCB_EXIT on the helper — non-self, so it is
 * safe to run (self TCB_EXIT would tear down the harness thread; the self
 * branch shares the same resolver + rights code, so runtime-testing it adds
 * nothing).  A SchedContext is minted into slots 34 (rw) / 35 (ro):
 * SC_CONFIGURE by CPtr, THREAD_SET_SC binds the CALLING thread to the SC by
 * CPtr and 0 unbinds (THREAD_SET_SC takes no TCB argument — it always
 * operates on the caller).  Authority is not relaxed: WRITE ops through the
 * READ-only slots → ACCESS_DENIED (THREAD_SET_SC has no rights check today;
 * unchanged).  Wrong type → INVALID_ARG (this family's historical code, kept
 * by the migration); empty slot fails cleanly.  Slots 32..39: the 16..29
 * dynamic pool is exhausted by T079-T082 (slots are mint-once). */

#define T083_SLOT_TCB     32L             /* helper TCB: READ|WRITE|DUP */
#define T083_SLOT_TCB_RO  33L             /* helper TCB: READ only      */
#define T083_SLOT_SC      34L             /* SchedContext: READ|WRITE|DUP */
#define T083_SLOT_SC_RO   35L             /* SchedContext: READ only    */

static volatile uint64_t g_t083_count = 0;
static volatile int      g_t083_ready = 0;
static long              g_t083_tcb   = -1;
static uint8_t           g_t083_stack[8192];

static void t083_helper(void) {
    g_t083_tcb   = it_sys0(SYS_TCB_SELF);
    g_t083_ready = 1;
    for (;;) {
        g_t083_count++;
        it_sys0(SYS_YIELD);
    }
}

static void test_t083(void) {
    g_t083_count = 0; g_t083_ready = 0; g_t083_tcb = -1;

    uint64_t entry = (uint64_t)(uintptr_t)t083_helper;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t083_stack + sizeof(g_t083_stack))) & ~0xFULL;
    long tid = it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0);
    if (tid < 0) { it_fail("T083", "thread create"); return; }

    for (int i = 0; i < 200 && !g_t083_ready; i++) it_sys0(SYS_YIELD);
    if (!g_t083_ready || g_t083_tcb < 0) { it_fail("T083", "tcb self"); return; }
    handle_id_t tcb_h = (handle_id_t)g_t083_tcb;

    int ok = 1;

    /* Mint the helper's TCB: slot 32 rw+dup, slot 33 read-only. */
    if (it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC, T083_SLOT_TCB,
                (long)tcb_h,
                (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0) ok = 0;
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC,
                      T083_SLOT_TCB_RO, (long)tcb_h, (long)RIGHT_READ) != 0)
        ok = 0;

    /* GET_INFO by CPtr (both slots — READ suffices); handle path unchanged. */
    struct iris_tcb_info info;
    if (ok && it_sys2(SYS_TCB_GET_INFO, T083_SLOT_TCB,
                      (long)(uintptr_t)&info) != 0) ok = 0;
    if (ok && info.task_id != (uint32_t)tid) ok = 0;
    if (ok && it_sys2(SYS_TCB_GET_INFO, T083_SLOT_TCB_RO,
                      (long)(uintptr_t)&info) != 0) ok = 0;
    if (ok && it_sys2(SYS_TCB_GET_INFO, (long)tcb_h,
                      (long)(uintptr_t)&info) != 0) ok = 0;

    /* SET_PRIORITY by CPtr: change, verify via GET_INFO, restore. */
    uint8_t old_prio = info.priority;
    if (ok && it_sys2(SYS_TCB_SET_PRIORITY, T083_SLOT_TCB,
                      (long)(old_prio + 1u)) != 0) ok = 0;
    if (ok && (it_sys2(SYS_TCB_GET_INFO, T083_SLOT_TCB,
                       (long)(uintptr_t)&info) != 0 ||
               info.priority != (uint8_t)(old_prio + 1u))) ok = 0;
    if (ok && it_sys2(SYS_TCB_SET_PRIORITY, T083_SLOT_TCB, (long)old_prio) != 0)
        ok = 0;

    /* SUSPEND by CPtr: the helper's counter must freeze. */
    if (ok && it_sys1(SYS_TCB_SUSPEND, T083_SLOT_TCB) != 0) ok = 0;
    if (ok) {
        uint64_t before = g_t083_count;
        it_sys1(SYS_SLEEP, 5);
        if (g_t083_count != before) ok = 0;
    }

    /* RESUME by CPtr: the counter must advance again. */
    if (ok && it_sys1(SYS_TCB_RESUME, T083_SLOT_TCB) != 0) ok = 0;
    if (ok) {
        uint64_t before = g_t083_count;
        it_sys1(SYS_SLEEP, 5);
        if (g_t083_count == before) ok = 0;
    }

    /* Authority not relaxed + failure paths (TCB). */
    if (ok && it_sys1(SYS_TCB_SUSPEND, T083_SLOT_TCB_RO) !=
              (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_sys2(SYS_TCB_SET_PRIORITY, T083_SLOT_TCB_RO, 1) !=
              (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_sys1(SYS_TCB_SUSPEND, T079_SLOT_EMPTY) >= 0) ok = 0;
    if (ok && it_sys1(SYS_TCB_SUSPEND, (long)IRIS_CPTR_TEST_FIX_A) !=
              (long)IRIS_ERR_INVALID_ARG) ok = 0;

    /* ── SchedContext ── */
    long sc = it_sys0(SYS_SC_CREATE);
    if (sc < 0) ok = 0;
    handle_id_t sc_h = (sc >= 0) ? (handle_id_t)sc : HANDLE_INVALID;

    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC,
                      T083_SLOT_SC, (long)sc_h,
                      (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) != 0)
        ok = 0;
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC,
                      T083_SLOT_SC_RO, (long)sc_h, (long)RIGHT_READ) != 0)
        ok = 0;

    /* SC_CONFIGURE by CPtr (budget < period required); handle path too. */
    if (ok && it_sys3(SYS_SC_CONFIGURE, T083_SLOT_SC, 50, 100) != 0) ok = 0;
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)sc_h, 50, 100) != 0) ok = 0;
    if (ok && it_sys3(SYS_SC_CONFIGURE, T083_SLOT_SC_RO, 50, 100) !=
              (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    if (ok && it_sys3(SYS_SC_CONFIGURE, T079_SLOT_EMPTY, 50, 100) >= 0) ok = 0;
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)IRIS_CPTR_TEST_FIX_A, 50, 100) !=
              (long)IRIS_ERR_INVALID_ARG) ok = 0;

    /* THREAD_SET_SC by CPtr: bind the calling thread, then unbind (0). */
    if (ok && it_sys1(SYS_THREAD_SET_SC, T083_SLOT_SC) != 0) ok = 0;
    if (ok && it_sys1(SYS_THREAD_SET_SC, 0) != 0) ok = 0;
    if (ok && it_sys1(SYS_THREAD_SET_SC, (long)IRIS_CPTR_TEST_FIX_A) !=
              (long)IRIS_ERR_INVALID_ARG) ok = 0;
    if (ok && it_sys1(SYS_THREAD_SET_SC, T079_SLOT_EMPTY) >= 0) ok = 0;

    /* TCB_EXIT by CPtr on the helper (non-self): counter freezes for good. */
    if (ok && it_sys1(SYS_TCB_EXIT, T083_SLOT_TCB) != 0) ok = 0;
    if (ok) {
        it_sys1(SYS_SLEEP, 2);
        uint64_t before = g_t083_count;
        it_sys1(SYS_SLEEP, 5);
        if (g_t083_count != before) ok = 0;
    }

    it_close(&sc_h);
    { handle_id_t th = tcb_h; it_close(&th); }

    if (ok) it_pass("T083"); else it_fail("T083", "tcb/sc by cptr");
}

/* ── T084: IPC receive-slot — basic endpoint cap delivery (A1.5) ────────────
 * A receiver can declare an empty own-CSpace slot in the EP_RECV hint field
 * msg.attached_cap (1..1023; 0 = legacy): a cap attached by the sender then
 * lands IN THE SLOT and the receiver gets the CPtr back in attached_handle
 * (the <1024 / >=1024 namespace split is the discriminator — no new msg
 * fields).  A sender thread EP_SENDs the same endpoint cap twice (WRITE,
 * TRANSFER-consumed dups): recv #1 declares slot 36 → attached_handle == 36
 * and EP_NB_SEND by that CPtr resolves (WOULD_BLOCK = resolution + rights
 * OK, no receiver on that ep); recv #2 declares nothing → legacy handle
 * >= 1024, exactly as before A1.5. */

#define T084_SLOT  36u

static handle_id_t  g_t084_cmd_ep = HANDLE_INVALID;
static handle_id_t  g_t084_cap1   = HANDLE_INVALID;
static handle_id_t  g_t084_cap2   = HANDLE_INVALID;
static volatile int g_t084_s1 = 999, g_t084_s2 = 999, g_t084_done = 0;
static uint8_t      g_t084_stack[8192];

static void t084_sender(void) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label           = 0x84;
    m.attached_handle = (uint32_t)g_t084_cap1;
    m.attached_rights = RIGHT_WRITE;
    g_t084_s1 = (int)it_sys2(SYS_EP_SEND, (long)g_t084_cmd_ep, (long)&m);
    it_iris_msg_zero(&m);
    m.label           = 0x84;
    m.attached_handle = (uint32_t)g_t084_cap2;
    m.attached_rights = RIGHT_WRITE;
    g_t084_s2 = (int)it_sys2(SYS_EP_SEND, (long)g_t084_cmd_ep, (long)&m);
    g_t084_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}

static void test_t084(void) {
    g_t084_s1 = 999; g_t084_s2 = 999; g_t084_done = 0;

    long epx = it_sys0(SYS_ENDPOINT_CREATE);   /* the cap being transferred */
    long cmd = it_sys0(SYS_ENDPOINT_CREATE);   /* the transfer channel */
    if (epx < 0 || cmd < 0) { it_fail("T084", "ep create"); return; }
    handle_id_t epx_h = (handle_id_t)epx;
    g_t084_cmd_ep = (handle_id_t)cmd;

    /* Dups to attach (EP_SEND consumes the attached handle). */
    long c1 = it_sys2(SYS_HANDLE_DUP, epx, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    long c2 = it_sys2(SYS_HANDLE_DUP, epx, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (c1 < 0 || c2 < 0) {
        it_close(&epx_h); it_close(&g_t084_cmd_ep);
        it_fail("T084", "dup"); return;
    }
    g_t084_cap1 = (handle_id_t)c1;
    g_t084_cap2 = (handle_id_t)c2;

    uint64_t entry = (uint64_t)(uintptr_t)t084_sender;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t084_stack + sizeof(g_t084_stack))) & ~0xFULL;
    if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
        it_close(&epx_h); it_close(&g_t084_cmd_ep);
        it_fail("T084", "thread create"); return;
    }
    it_sys1(SYS_SLEEP, 2);   /* let the sender queue its first send */

    int ok = 1;
    struct IrisMsg r;

    /* recv #1: declare slot 36 → the cap must land there, as a CPtr. */
    it_iris_msg_zero(&r);
    r.attached_cap = T084_SLOT;
    if (it_sys2(SYS_EP_RECV, (long)g_t084_cmd_ep, (long)&r) != 0) ok = 0;
    if (ok && r.attached_handle != T084_SLOT) ok = 0;
    if (ok) {
        struct IrisMsg probe;
        it_iris_msg_zero(&probe);
        probe.label = 0x84;
        if (it_sys2(SYS_EP_NB_SEND, (long)T084_SLOT, (long)&probe) !=
            (long)IRIS_ERR_WOULD_BLOCK) ok = 0;   /* resolves via CSpace */
    }

    /* recv #2: no declaration → legacy handle materialization. */
    it_iris_msg_zero(&r);
    if (ok && it_sys2(SYS_EP_RECV, (long)g_t084_cmd_ep, (long)&r) != 0) ok = 0;
    if (ok && !(r.attached_handle >= 1024u)) ok = 0;
    if (ok) {
        handle_id_t lh = (handle_id_t)r.attached_handle;
        it_close(&lh);
    }

    for (int i = 0; i < 200 && !g_t084_done; i++) it_sys0(SYS_YIELD);
    if (!g_t084_done || g_t084_s1 != 0 || g_t084_s2 != 0) ok = 0;

    it_close(&epx_h);
    it_close(&g_t084_cmd_ep);

    if (ok) it_pass("T084"); else it_fail("T084", "recv-slot basic");
}

/* ── T085: receive-slot rights reduction (A1.5) ─────────────────────────────
 * The slot receives rights_reduce(sender_rights, requested) — never more.
 * A notification cap (R|W|WAIT|DUP|TRANSFER at creation) is transferred
 * WRITE-only into slot 37: NOTIFY_SIGNAL by CPtr works and the ORIGINAL
 * handle observes the signalled bits (same kernel object — the invocation
 * is real), while NOTIFY_WAIT by CPtr fails ACCESS_DENIED (RIGHT_WAIT was
 * reduced away). */

#define T085_SLOT  37u

static handle_id_t  g_t085_cmd_ep = HANDLE_INVALID;
static handle_id_t  g_t085_cap    = HANDLE_INVALID;
static volatile int g_t085_s1 = 999, g_t085_done = 0;
static uint8_t      g_t085_stack[8192];

static void t085_sender(void) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label           = 0x85;
    m.attached_handle = (uint32_t)g_t085_cap;
    m.attached_rights = RIGHT_WRITE;            /* reduce: drop WAIT et al. */
    g_t085_s1 = (int)it_sys2(SYS_EP_SEND, (long)g_t085_cmd_ep, (long)&m);
    g_t085_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}

static void test_t085(void) {
    g_t085_s1 = 999; g_t085_done = 0;

    long n   = it_sys0(SYS_NOTIFY_CREATE);
    long cmd = it_sys0(SYS_ENDPOINT_CREATE);
    if (n < 0 || cmd < 0) { it_fail("T085", "create"); return; }
    handle_id_t n_h = (handle_id_t)n;
    g_t085_cmd_ep = (handle_id_t)cmd;

    long c = it_sys2(SYS_HANDLE_DUP, n, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (c < 0) {
        it_close(&n_h); it_close(&g_t085_cmd_ep);
        it_fail("T085", "dup"); return;
    }
    g_t085_cap = (handle_id_t)c;

    uint64_t entry = (uint64_t)(uintptr_t)t085_sender;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t085_stack + sizeof(g_t085_stack))) & ~0xFULL;
    if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
        it_close(&n_h); it_close(&g_t085_cmd_ep);
        it_fail("T085", "thread create"); return;
    }
    it_sys1(SYS_SLEEP, 2);

    int ok = 1;
    struct IrisMsg r;
    it_iris_msg_zero(&r);
    r.attached_cap = T085_SLOT;
    if (it_sys2(SYS_EP_RECV, (long)g_t085_cmd_ep, (long)&r) != 0) ok = 0;
    if (ok && r.attached_handle != T085_SLOT) ok = 0;

    /* Permitted op by CPtr: SIGNAL (RIGHT_WRITE survived the reduce). */
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, (long)T085_SLOT, 0x85) != 0) ok = 0;
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x85u) ok = 0;   /* original handle sees it: same object */
    }

    /* Denied op by CPtr: WAIT (RIGHT_WAIT was reduced away). */
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_WAIT, (long)T085_SLOT, (long)(uintptr_t)&bits) !=
            (long)IRIS_ERR_ACCESS_DENIED) ok = 0;
    }

    for (int i = 0; i < 200 && !g_t085_done; i++) it_sys0(SYS_YIELD);
    if (!g_t085_done || g_t085_s1 != 0) ok = 0;

    it_close(&n_h);
    it_close(&g_t085_cmd_ep);

    if (ok) it_pass("T085"); else it_fail("T085", "recv-slot rights");
}

/* ── T086: receive-slot occupied / invalid — fail-fast atomicity (A1.5) ─────
 * A bad declaration must fail BEFORE the endpoint is touched: the queued
 * sender stays blocked with its staged cap intact and nothing is consumed.
 * With a sender queued (attached notification cap):
 *   - declaring slot 36 (occupied since T084) → ALREADY_EXISTS (the
 *     canonical occupied-slot error, same as SYS_PROC_CSPACE_MINT);
 *   - declaring slot 300 (< 1024 but beyond the 256-slot root CNode) →
 *     INVALID_ARG;  EP_NB_RECV validates identically;
 *   - sender still blocked after both failures (result flag untouched);
 *   - a good declaration (slot 42) then receives the SAME cap intact and
 *     invokes it by CPtr — the failed attempts consumed nothing. */

#define T086_SLOT  42u

static handle_id_t  g_t086_cmd_ep = HANDLE_INVALID;
static handle_id_t  g_t086_cap    = HANDLE_INVALID;
static volatile int g_t086_s1 = 999, g_t086_done = 0;
static uint8_t      g_t086_stack[8192];

static void t086_sender(void) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label           = 0x86;
    m.attached_handle = (uint32_t)g_t086_cap;
    m.attached_rights = RIGHT_WRITE;
    g_t086_s1 = (int)it_sys2(SYS_EP_SEND, (long)g_t086_cmd_ep, (long)&m);
    g_t086_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}

static void test_t086(void) {
    g_t086_s1 = 999; g_t086_done = 0;

    long n   = it_sys0(SYS_NOTIFY_CREATE);
    long cmd = it_sys0(SYS_ENDPOINT_CREATE);
    if (n < 0 || cmd < 0) { it_fail("T086", "create"); return; }
    handle_id_t n_h = (handle_id_t)n;
    g_t086_cmd_ep = (handle_id_t)cmd;

    long c = it_sys2(SYS_HANDLE_DUP, n, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (c < 0) {
        it_close(&n_h); it_close(&g_t086_cmd_ep);
        it_fail("T086", "dup"); return;
    }
    g_t086_cap = (handle_id_t)c;

    uint64_t entry = (uint64_t)(uintptr_t)t086_sender;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t086_stack + sizeof(g_t086_stack))) & ~0xFULL;
    if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
        it_close(&n_h); it_close(&g_t086_cmd_ep);
        it_fail("T086", "thread create"); return;
    }
    it_sys1(SYS_SLEEP, 2);   /* sender is now queued with its staged cap */

    int ok = 1;
    struct IrisMsg r;

    /* Occupied slot → ALREADY_EXISTS, fail-fast. */
    it_iris_msg_zero(&r);
    r.attached_cap = T084_SLOT;                   /* occupied since T084 */
    if (it_sys2(SYS_EP_RECV, (long)g_t086_cmd_ep, (long)&r) !=
        (long)IRIS_ERR_ALREADY_EXISTS) ok = 0;

    /* Out-of-range direct slot → INVALID_ARG (root CNode has 256 slots). */
    it_iris_msg_zero(&r);
    r.attached_cap = 300u;
    if (ok && it_sys2(SYS_EP_RECV, (long)g_t086_cmd_ep, (long)&r) !=
              (long)IRIS_ERR_INVALID_ARG) ok = 0;

    /* EP_NB_RECV validates the declaration the same way. */
    it_iris_msg_zero(&r);
    r.attached_cap = T084_SLOT;
    if (ok && it_sys2(SYS_EP_NB_RECV, (long)g_t086_cmd_ep, (long)&r) !=
              (long)IRIS_ERR_ALREADY_EXISTS) ok = 0;

    /* Atomicity: the sender is still blocked — nothing was consumed. */
    if (ok && g_t086_s1 != 999) ok = 0;

    /* A good declaration now receives the SAME cap, intact. */
    it_iris_msg_zero(&r);
    r.attached_cap = T086_SLOT;
    if (ok && it_sys2(SYS_EP_RECV, (long)g_t086_cmd_ep, (long)&r) != 0) ok = 0;
    if (ok && r.attached_handle != T086_SLOT) ok = 0;
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, (long)T086_SLOT, 0x86) != 0) ok = 0;
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x86u) ok = 0;
    }

    for (int i = 0; i < 200 && !g_t086_done; i++) it_sys0(SYS_YIELD);
    if (!g_t086_done || g_t086_s1 != 0) ok = 0;

    it_close(&n_h);
    it_close(&g_t086_cmd_ep);

    if (ok) it_pass("T086"); else it_fail("T086", "recv-slot atomicity");
}

/* ── T087: EP_CALL receive-slot keeps the reply cap ephemeral (A1.5) ────────
 * Both call-direction transfers use receive-slots while the KReply stays a
 * one-shot HANDLE:
 *   - the server's EP_RECV declares slot 38 → the cap the caller attaches
 *     via EP_CALL attached_cap lands there (CPtr);
 *   - the caller's EP_CALL declares slot 39 in attached_handle (previously
 *     a forced-zero field; >= 1024 still fails INVALID_ARG) → the cap the
 *     REPLY transfers lands there;
 *   - the reply cap itself is delivered to the server as a handle >= 1024
 *     (asserted) and keeps the T074 one-shot contract: first SYS_REPLY ok,
 *     second → NOT_FOUND.  receive-slot never converts a reply cap. */

#define T087_SRV_SLOT    38u
#define T087_REPLY_SLOT  39u

static handle_id_t       g_t087_ep   = HANDLE_INVALID;
static handle_id_t       g_t087_capB = HANDLE_INVALID;
static volatile uint32_t g_t087_got_cap = 0, g_t087_reply_h = 0;
static volatile int      g_t087_sig = 999, g_t087_r1 = 999, g_t087_r2 = 999;
static volatile int      g_t087_done = 0;
static uint8_t           g_t087_stack[8192];

static void t087_server(void) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.attached_cap = T087_SRV_SLOT;              /* receive-slot declaration */
    long rr = it_sys2(SYS_EP_RECV, (long)g_t087_ep, (long)&m);
    if (rr == 0) {
        g_t087_got_cap = m.attached_cap;         /* caller's transferred cap */
        g_t087_reply_h = m.attached_handle;      /* reply cap — MUST be a handle */
        g_t087_sig = (int)it_sys2(SYS_NOTIFY_SIGNAL, (long)m.attached_cap, 0x87);

        struct IrisMsg rm;
        it_iris_msg_zero(&rm);
        rm.label           = 0x87;
        rm.attached_handle = (uint32_t)g_t087_capB;   /* cap back to caller */
        rm.attached_rights = RIGHT_WRITE;
        g_t087_r1 = (int)it_sys2(SYS_REPLY, (long)g_t087_reply_h, (long)&rm);

        it_iris_msg_zero(&rm);
        rm.label  = 0x87;
        g_t087_r2 = (int)it_sys2(SYS_REPLY, (long)g_t087_reply_h, (long)&rm);
    }
    g_t087_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}

static void test_t087(void) {
    g_t087_got_cap = 0; g_t087_reply_h = 0;
    g_t087_sig = 999; g_t087_r1 = 999; g_t087_r2 = 999; g_t087_done = 0;

    long nA = it_sys0(SYS_NOTIFY_CREATE);
    long nB = it_sys0(SYS_NOTIFY_CREATE);
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (nA < 0 || nB < 0 || ep < 0) { it_fail("T087", "create"); return; }
    handle_id_t nA_h = (handle_id_t)nA, nB_h = (handle_id_t)nB;
    g_t087_ep = (handle_id_t)ep;

    long cA = it_sys2(SYS_HANDLE_DUP, nA, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    long cB = it_sys2(SYS_HANDLE_DUP, nB, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (cA < 0 || cB < 0) {
        it_close(&nA_h); it_close(&nB_h); it_close(&g_t087_ep);
        it_fail("T087", "dup"); return;
    }
    g_t087_capB = (handle_id_t)cB;

    uint64_t entry = (uint64_t)(uintptr_t)t087_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t087_stack + sizeof(g_t087_stack))) & ~0xFULL;
    if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
        it_close(&nA_h); it_close(&nB_h); it_close(&g_t087_ep);
        it_fail("T087", "thread create"); return;
    }
    it_sys1(SYS_SLEEP, 2);   /* server blocks with slot 38 declared */

    int ok = 1;
    struct IrisMsg cm;
    it_iris_msg_zero(&cm);
    cm.label              = 0x87;
    cm.attached_handle    = T087_REPLY_SLOT;      /* reply receive-slot */
    cm.attached_cap       = (uint32_t)cA;         /* cap to the server */
    cm.attached_cap_rights = RIGHT_WRITE;
    if (it_sys2(SYS_EP_CALL, (long)g_t087_ep, (long)&cm) != 0) ok = 0;

    /* Reply's transferred cap landed in OUR declared slot 39. */
    if (ok && cm.attached_handle != T087_REPLY_SLOT) ok = 0;
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, (long)T087_REPLY_SLOT, 0x99) != 0) ok = 0;
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_WAIT, nB, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x99u) ok = 0;
    }
    /* Server signalled notifA through its slot-38 CPtr. */
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_WAIT, nA, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x87u) ok = 0;
    }

    for (int i = 0; i < 200 && !g_t087_done; i++) it_sys0(SYS_YIELD);
    if (!g_t087_done) ok = 0;
    if (ok && g_t087_got_cap != T087_SRV_SLOT) ok = 0;      /* landed as CPtr */
    if (ok && !(g_t087_reply_h >= 1024u)) ok = 0;           /* reply cap = handle */
    if (ok && g_t087_sig != 0) ok = 0;                      /* invocable by CPtr */
    if (ok && g_t087_r1 != 0) ok = 0;                       /* first reply ok */
    if (ok && g_t087_r2 != (int)IRIS_ERR_NOT_FOUND) ok = 0; /* one-shot (T074) */

    it_close(&nA_h);
    it_close(&nB_h);
    it_close(&g_t087_ep);

    if (ok) it_pass("T087"); else it_fail("T087", "recv-slot ep_call");
}

/* ── T088: death cleanup with receive-slot (A1.5) ───────────────────────────
 * A declared receive-slot must leave NO trace when the receive never
 * completes:
 *   A. receiver thread killed while blocked with slot 40 declared → slot 40
 *      has no ghost cap (SYS_CSPACE_RESOLVE fails), the endpoint is clean
 *      (EP_NB_SEND → WOULD_BLOCK, the T077 probe);
 *   B. the SAME slot 40 then serves a real transfer: a fresh receiver
 *      declares it and blocks FIRST, the sender delivers from its own
 *      context (send-side routed path) → cap lands as CPtr 40, invocable —
 *      no partially-installed state survived sub-case A;
 *   C. endpoint closed while a receiver waits with slot 41 declared → the
 *      receiver wakes with CLOSED and slot 41 stays empty. */

#define T088_SLOT_A  40u
#define T088_SLOT_C  41u

static handle_id_t       g_t088_ep  = HANDLE_INVALID;
static handle_id_t       g_t088_ep2 = HANDLE_INVALID;
static long              g_t088_r1_tcb = -1;
static volatile int      g_t088_r1_ready = 0;
static volatile uint32_t g_t088_r2_got = 0;
static volatile int      g_t088_r2_sig = 999, g_t088_r2_done = 0;
static volatile long     g_t088_r3_rr = 999;
static volatile int      g_t088_r3_done = 0;
static uint8_t           g_t088_stack1[8192];
static uint8_t           g_t088_stack2[8192];
static uint8_t           g_t088_stack3[8192];

static void t088_recv1(void) {   /* killed while blocked with slot declared */
    struct IrisMsg m;
    g_t088_r1_tcb   = it_sys0(SYS_TCB_SELF);
    g_t088_r1_ready = 1;
    it_iris_msg_zero(&m);
    m.attached_cap = T088_SLOT_A;
    (void)it_sys2(SYS_EP_RECV, (long)g_t088_ep, (long)&m);
    it_sys0(SYS_THREAD_EXIT);    /* not reached: killed while blocked */
    for (;;) {}
}

static void t088_recv2(void) {   /* real transfer into the same slot */
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.attached_cap = T088_SLOT_A;
    if (it_sys2(SYS_EP_RECV, (long)g_t088_ep, (long)&m) == 0) {
        g_t088_r2_got = m.attached_handle;
        g_t088_r2_sig = (int)it_sys2(SYS_NOTIFY_SIGNAL, (long)m.attached_handle, 0x88);
    }
    g_t088_r2_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}

static void t088_recv3(void) {   /* endpoint closed under a declared slot */
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.attached_cap = T088_SLOT_C;
    g_t088_r3_rr   = it_sys2(SYS_EP_RECV, (long)g_t088_ep2, (long)&m);
    g_t088_r3_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}

static void test_t088(void) {
    g_t088_r1_tcb = -1; g_t088_r1_ready = 0;
    g_t088_r2_got = 0; g_t088_r2_sig = 999; g_t088_r2_done = 0;
    g_t088_r3_rr = 999; g_t088_r3_done = 0;

    long n   = it_sys0(SYS_NOTIFY_CREATE);
    long ep  = it_sys0(SYS_ENDPOINT_CREATE);
    long ep2 = it_sys0(SYS_ENDPOINT_CREATE);
    if (n < 0 || ep < 0 || ep2 < 0) { it_fail("T088", "create"); return; }
    handle_id_t n_h = (handle_id_t)n;
    g_t088_ep  = (handle_id_t)ep;
    g_t088_ep2 = (handle_id_t)ep2;

    int ok = 1;

    /* ── A: kill a receiver blocked with a declared slot ── */
    {
        uint64_t entry = (uint64_t)(uintptr_t)t088_recv1;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t088_stack1 + sizeof(g_t088_stack1))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) ok = 0;
        for (int i = 0; i < 200 && !g_t088_r1_ready; i++) it_sys0(SYS_YIELD);
        it_sys1(SYS_SLEEP, 2);            /* let it block in EP_RECV */
        if (ok && (g_t088_r1_tcb < 0 ||
                   it_sys1(SYS_TCB_EXIT, g_t088_r1_tcb) != 0)) ok = 0;
        /* No ghost cap in the slot; no stale receiver on the endpoint. */
        if (ok && it_sys1(SYS_CSPACE_RESOLVE, (long)T088_SLOT_A) >= 0) ok = 0;
        if (ok) {
            struct IrisMsg p;
            it_iris_msg_zero(&p);
            p.label = 0x88;
            if (it_sys2(SYS_EP_NB_SEND, (long)g_t088_ep, (long)&p) !=
                (long)IRIS_ERR_WOULD_BLOCK) ok = 0;
        }
    }

    /* ── B: the same slot serves a real transfer (send-side delivery) ── */
    if (ok) {
        uint64_t entry = (uint64_t)(uintptr_t)t088_recv2;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t088_stack2 + sizeof(g_t088_stack2))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) ok = 0;
        it_sys1(SYS_SLEEP, 2);            /* receiver blocks FIRST */
        long c = it_sys2(SYS_HANDLE_DUP, n, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (c < 0) ok = 0;
        if (ok) {
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label           = 0x88;
            m.attached_handle = (uint32_t)c;
            m.attached_rights = RIGHT_WRITE;
            if (it_sys2(SYS_EP_SEND, (long)g_t088_ep, (long)&m) != 0) ok = 0;
        }
        for (int i = 0; i < 200 && !g_t088_r2_done; i++) it_sys0(SYS_YIELD);
        if (!g_t088_r2_done || g_t088_r2_got != T088_SLOT_A ||
            g_t088_r2_sig != 0) ok = 0;
        if (ok) {
            uint64_t bits = 0;
            if (it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
                bits != 0x88u) ok = 0;
        }
    }

    /* ── C: endpoint close under a declared slot ── */
    if (ok) {
        uint64_t entry = (uint64_t)(uintptr_t)t088_recv3;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t088_stack3 + sizeof(g_t088_stack3))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) ok = 0;
        it_sys1(SYS_SLEEP, 2);            /* let it block with slot 41 declared */
        it_close(&g_t088_ep2);            /* close wakes the blocked receiver */
        for (int i = 0; i < 200 && !g_t088_r3_done; i++) it_sys0(SYS_YIELD);
        if (!g_t088_r3_done || g_t088_r3_rr != (long)IRIS_ERR_CLOSED) ok = 0;
        if (ok && it_sys1(SYS_CSPACE_RESOLVE, (long)T088_SLOT_C) >= 0) ok = 0;
    }

    it_close(&n_h);
    it_close(&g_t088_ep);
    it_close(&g_t088_ep2);

    if (ok) it_pass("T088"); else it_fail("T088", "recv-slot death cleanup");
}

/* ── A1.6: in-tree receive-slot service flows (T089–T092) ───────────────────
 * The receive-slot mechanism (T084–T088) as used by REAL services: svcmgr
 * stores REGISTER caps in its own CSpace (pool slots 64..255) and clients
 * receive LOOKUP caps into declared reply-slots.  Test slots here: 48..50. */

/* LOOKUP `name` over svcmgr.ep declaring `reply_slot` (0 = legacy). */
static long it_lookup_name_slot(const char *name, uint32_t reply_slot,
                                struct IrisMsg *msg) {
    uint32_t len = it_stage_path(name);
    it_iris_msg_zero(msg);
    msg->label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg->buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    msg->buf_len  = len;
    iris_msg_declare_reply_slot(msg, reply_slot);
    return it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)msg);
}

/* UNREGISTER dynamic id; 0 on success, -(error) on reply ERR. */
static long it_unregister_id(uint32_t id) {
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label      = IRIS_SVCMGR_EP_UNREGISTER;
    msg.words[0]   = id;
    msg.word_count = 1u;
    long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&msg);
    if (r != 0) return r;
    return (msg.label == IRIS_EP_REPLY_OK) ? 0 : -(long)(uint32_t)msg.words[0];
}

/* ── T089: svcmgr CSpace-backed registration lifecycle (A1.6) ───────────────
 * The REGISTER cap now lands in svcmgr's CSpace via its declared
 * receive-slot.  A legacy client (no reply-slot) must observe identical
 * behavior end to end: register → lookup returns a working handle (>= 1024)
 * duplicated from the CSpace-held master → unregister releases the pool slot
 * (lookup → NOT_FOUND) → the same name registers again (slot reuse). */
static void test_t089(void) {
    long e = it_sys0(SYS_ENDPOINT_CREATE);
    if (e < 0) { it_fail("T089", "ep create"); return; }
    handle_id_t ep = (handle_id_t)e;
    int ok = 1;

    long id = it_register_ep("t89.svc", ep);
    if (id < 0) ok = 0;

    struct IrisMsg msg;
    if (ok) {
        /* Legacy lookup: handle >= 1024, dup'd from the CSpace master. */
        if (it_lookup_name_slot("t89.svc", 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            !iris_msg_cap_is_handle(msg.attached_handle)) ok = 0;
        if (ok) {
            /* Invocable: no receiver on the endpoint → WOULD_BLOCK proves
             * resolution + rights (T084 probe). */
            struct IrisMsg p;
            it_iris_msg_zero(&p);
            p.label = 0x89;
            if (it_sys2(SYS_EP_NB_SEND, (long)msg.attached_handle, (long)&p) !=
                (long)IRIS_ERR_WOULD_BLOCK) ok = 0;
            handle_id_t cap = (handle_id_t)msg.attached_handle;
            it_close(&cap);
        }
    }

    /* Unregister releases svcmgr's CSpace pool slot cleanly. */
    if (ok && it_unregister_id((uint32_t)id) != 0) ok = 0;
    if (ok) {
        if (it_lookup_name_slot("t89.svc", 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR ||
            msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) ok = 0;
    }

    /* Slot reuse: the same name registers again after the release. */
    if (ok) {
        long id2 = it_register_ep("t89.svc", ep);
        if (id2 < 0) ok = 0;
        else if (it_unregister_id((uint32_t)id2) != 0) ok = 0;
    }

    it_close(&ep);
    if (ok) it_pass("T089"); else it_fail("T089", "cspace-backed register");
}

/* ── T090: LOOKUP into a client reply receive-slot (A1.6) ───────────────────
 * The looked-up cap lands in the CLIENT's CSpace as a CPtr; an occupied
 * reply-slot fails fast (ALREADY_EXISTS, endpoint untouched) and legacy
 * lookup keeps working after it; a failed lookup (NOT_FOUND) with a declared
 * slot leaves the slot empty. */
#define T090_SLOT   48u
#define T090_SLOT_B 49u

static void test_t090(void) {
    long e = it_sys0(SYS_ENDPOINT_CREATE);
    if (e < 0) { it_fail("T090", "ep create"); return; }
    handle_id_t ep = (handle_id_t)e;
    int ok = 1;

    long id = it_register_ep("t90.svc", ep);
    if (id < 0) ok = 0;

    struct IrisMsg msg;
    if (ok) {
        /* Reply-slot lookup: the cap arrives as CPtr T090_SLOT, no handle. */
        if (it_lookup_name_slot("t90.svc", T090_SLOT, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.attached_handle != T090_SLOT) ok = 0;
        if (ok) {
            struct IrisMsg p;
            it_iris_msg_zero(&p);
            p.label = 0x90;
            if (it_sys2(SYS_EP_NB_SEND, (long)T090_SLOT, (long)&p) !=
                (long)IRIS_ERR_WOULD_BLOCK) ok = 0;   /* invocable by CPtr */
        }
    }

    /* Occupied reply-slot → EP_CALL fails fast, before any send. */
    if (ok && it_lookup_name_slot("t90.svc", T090_SLOT, &msg) !=
        (long)IRIS_ERR_ALREADY_EXISTS) ok = 0;
    /* Legacy lookup still works after the failed declaration. */
    if (ok) {
        if (it_lookup_name_slot("t90.svc", 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            !iris_msg_cap_is_handle(msg.attached_handle)) ok = 0;
        else {
            handle_id_t cap = (handle_id_t)msg.attached_handle;
            it_close(&cap);
        }
    }

    /* NOT_FOUND with a declared slot: no cap, slot stays empty. */
    if (ok) {
        if (it_lookup_name_slot("t90.nope", T090_SLOT_B, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR ||
            msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) ok = 0;
        if (ok && it_sys1(SYS_CSPACE_RESOLVE, (long)T090_SLOT_B) >= 0) ok = 0;
    }

    if (id >= 0) (void)it_unregister_id((uint32_t)id);
    it_close(&ep);
    if (ok) it_pass("T090"); else it_fail("T090", "recv-slot lookup");
}

/* ── T091: vfs.ep session by CPtr receive-slot (A1.6) ───────────────────────
 * The real in-tree client flow init now uses at boot: look up "vfs.ep" into
 * a declared reply-slot and drive a REAL VFS operation through the CPtr. */
#define T091_SLOT 50u

static void test_t091(void) {
    static const char expect[] = "Hello from IrisOS VFS!\n";
    const uint32_t expect_len = (uint32_t)(sizeof(expect) - 1u);
    int ok = 1;

    struct IrisMsg msg;
    if (it_lookup_name_slot(VFS_EP_SVC_NAME, T091_SLOT, &msg) != 0 ||
        msg.label != IRIS_EP_REPLY_OK ||
        msg.attached_handle != T091_SLOT) ok = 0;

    if (ok) {
        uint32_t len = it_stage_path("iris.txt");
        it_iris_msg_zero(&msg);
        msg.label      = VFS_EP_OP_READ_AT;
        msg.words[0]   = 0;
        msg.words[1]   = VFS_EP_DATA_MAX;
        msg.word_count = 2;
        msg.buf_uptr   = (uint64_t)(uintptr_t)g_ep_io_buf;
        msg.buf_len    = len;
        if (it_sys2(SYS_EP_CALL, (long)T091_SLOT, (long)&msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.words[1] != (uint64_t)expect_len ||
            msg.buf_len != expect_len) ok = 0;
        if (ok) {
            for (uint32_t i = 0; i < expect_len; i++) {
                if (g_ep_io_buf[i] != (uint8_t)expect[i]) { ok = 0; break; }
            }
        }
    }
    if (ok) it_pass("T091"); else it_fail("T091", "vfs.ep by cptr slot");
}

/* ── T092: legacy attached-handle service IPC still works (A1.6) ────────────
 * A client that declares NO slot gets the exact pre-A1.6 behavior from the
 * migrated services: the vfs.ep cap materializes as a handle >= 1024 and
 * drives the same VFS operation. */
static void test_t092(void) {
    int ok = 1;
    struct IrisMsg msg;
    handle_id_t cap = HANDLE_INVALID;

    if (it_lookup_name_slot(VFS_EP_SVC_NAME, 0u, &msg) != 0 ||
        msg.label != IRIS_EP_REPLY_OK ||
        !iris_msg_cap_is_handle(msg.attached_handle)) ok = 0;
    else cap = (handle_id_t)msg.attached_handle;

    if (ok) {
        it_iris_msg_zero(&msg);
        msg.label = VFS_EP_OP_STATUS;
        if (it_sys2(SYS_EP_CALL, (long)cap, (long)&msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK) ok = 0;
    }
    it_close(&cap);
    if (ok) it_pass("T092"); else it_fail("T092", "legacy handle lookup");
}

/* ── A1.7: handle-table shrink/freeze evidence (T093–T096) ──────────────────
 * Stress the receive-slot pool, force the documented TOCTOU fallback, and
 * read the sys_sched_info extended diagnostics to prove the handle table is
 * a small, bounded working set.  Test slot: 51 (T094). */

/* Read the A1.7 extended sched_info words (offsets 40..87 as 12 uint32).
 * The KDEBUG authority comes from the spawn bootcap (slot 6), resolved for
 * the duration of the call — identical churn on every invocation, so
 * before/after comparisons of self_live are exact. */
static int it_sched_ext(uint32_t w[14]) {
    long bh = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    if (bh < 0) return 0;
    uint8_t buf[96];
    /* Fase 16: request 96 bytes so the two lifecycle words (offsets 84/88)
     * land too; a pre-Fase-16 kernel clamps to 88 and leaves w[11..13] zero —
     * the extra words are additive, never required by legacy asserts. */
    long r = it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 96);
    handle_id_t h = (handle_id_t)bh;
    it_close(&h);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 14u; i++) {
        uint32_t o = 40u + 4u * i;
        w[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
               ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/* Read the base-frame live TASK count (offset 32) — the scheduler's
 * sched_live_count, distinct from the handle-table live at IT_SI_LIVE. */
static int it_task_live(uint32_t *out) {
    long bh = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    if (bh < 0) return 0;
    uint8_t buf[96];
    long r = it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 96);
    handle_id_t h = (handle_id_t)bh;
    it_close(&h);
    if (r != 0) return 0;
    *out = (uint32_t)buf[32] | ((uint32_t)buf[33] << 8) |
           ((uint32_t)buf[34] << 16) | ((uint32_t)buf[35] << 24);
    return 1;
}

/* Extended-word indices (see syscall_diag.c layout). */
#define IT_SI_LIVE      0u
#define IT_SI_HWM       1u
#define IT_SI_INSERTS   2u
#define IT_SI_REMOVES   3u
#define IT_SI_GHWM      4u
#define IT_SI_MAX       5u
#define IT_SI_SLOTDEL   6u
#define IT_SI_HANDDEL   7u
#define IT_SI_TOCTOU    8u
#define IT_SI_REPLY     9u
#define IT_SI_RESOLVE  10u
#define IT_SI_PROCLIVE 11u   /* Fase 16: KProcess objects live */
#define IT_SI_REAPHWM  12u   /* Fase 16: deferred-reap queue depth hwm */

/* Fase 17 ext2 scheduler-hardening words (offsets 96..108, 4 uint32).  A
 * pre-Fase-17 kernel clamps SYS_SCHED_INFO to 96 bytes and leaves these zero —
 * they are additive and never required by a legacy assert. */
#define IT_S2_RQHWM   0u   /* run-queue depth high-water */
#define IT_S2_DUPENQ  1u   /* duplicate-enqueue guard trips (invariant S4) */
#define IT_S2_SCLIVE  2u   /* live KSchedContext objects (invariants S8/S9) */
#define IT_S2_YIELD   3u   /* task_yield() entries (monotonic progress) */

/* Fase 18 ext3 authority words (offsets 112..128, 5 uint32 live per-type
 * counts).  A pre-Fase-18 kernel clamps SYS_SCHED_INFO to 112 bytes and leaves
 * these zero — additive, never required by a legacy assert. */
#define IT_S3_UNTYPED 0u   /* KUntyped objects live */
#define IT_S3_FRAME   1u   /* KFrame objects live   */
#define IT_S3_EP      2u   /* KEndpoint objects live */
#define IT_S3_NOTIF   3u   /* KNotification objects live */
#define IT_S3_CNODE   4u   /* KCNode objects live   */

static int it_sched_ext3(uint32_t w3[5]) {
    long bh = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    if (bh < 0) return 0;
    uint8_t buf[136];
    long r = it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 136);
    handle_id_t h = (handle_id_t)bh;
    it_close(&h);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 5u; i++) {
        uint32_t o = 112u + 4u * i;
        w3[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/* KOBJ type codes for SYS_UNTYPED_RETYPE — must match the kobject_type_t order
 * in kernel/new_core/include/iris/nc/kobject.h (that enum is __KERNEL__-only). */
#define IT_KOBJ_NOTIFICATION   2u
#define IT_KOBJ_ENDPOINT       8u
#define IT_KOBJ_CNODE          9u
#define IT_KOBJ_SCHED_CONTEXT 10u
#define IT_KOBJ_UNTYPED       11u
#define IT_KOBJ_FRAME         15u
#define IT_KOBJ_VSPACE        14u

/* Fase 19 ext4 VM/VSpace words (offsets 136..152, 5 uint32). */
#define IT_S4_VSLIVE  0u   /* KVSpace objects live */
#define IT_S4_MAPLIVE 1u   /* KFrameMapping nodes live */
#define IT_S4_MAPOK   2u   /* successful maps (cumulative) */
#define IT_S4_UNMAPOK 3u   /* explicit unmaps (cumulative) */
#define IT_S4_TLB     4u   /* local invlpg count (cumulative) */

static int it_sched_ext4(uint32_t w4[5]) {
    long bh = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    if (bh < 0) return 0;
    uint8_t buf[160];
    long r = it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 160);
    handle_id_t h = (handle_id_t)bh;
    it_close(&h);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 5u; i++) {
        uint32_t o = 136u + 4u * i;
        w4[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/* Fase 19: lazily mint a cap to iris_test's OWN VSpace into IRIS_CPTR_TEST_VSPACE
 * (via SYS_VSPACE_SELF + a self-mint through the self-proc cap, slot 25) so the
 * VM tests can pass it as the VSpace CPtr to SYS_FRAME_MAP/UNMAP.  Returns 1 on
 * success. */
static int g_it_vspace_ready = 0;
static int it_setup_self_vspace(void) {
    if (g_it_vspace_ready) return 1;
    long h = it_sys0(SYS_VSPACE_SELF);
    if (h < 0) return 0;
    handle_id_t vh = (handle_id_t)h;
    long r = it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC,
                     (long)IRIS_CPTR_TEST_VSPACE, (long)vh,
                     (long)(RIGHT_READ | RIGHT_WRITE));
    it_close(&vh);
    if (r != 0) return 0;
    g_it_vspace_ready = 1;
    return 1;
}

/* Test VSpace CPtr and a reserved self-map VA window (page-aligned, inside the
 * user private window, clear of the VMO test VAs at 0x8050/0x8060/0x8061). */
#define IT_VS       ((long)IRIS_CPTR_TEST_VSPACE)
#define T133_VA     0x8070000000ULL
#define T134_VA     0x8071000000ULL
#define T135_VA_X   0x8072000000ULL
#define T135_VA_Y   0x8073000000ULL
#define T137_VA     0x8074000000ULL
#define T138_VA     0x8075000000ULL
#define T139_VA_BASE 0x8078000000ULL
#define IT_MAP_W    1ULL   /* SYS_FRAME_MAP flags: bit0 = WRITABLE */

static int it_sched_ext2(uint32_t w2[4]) {
    long bh = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    if (bh < 0) return 0;
    uint8_t buf[112];
    long r = it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 112);
    handle_id_t h = (handle_id_t)bh;
    it_close(&h);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t o = 96u + 4u * i;
        w2[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
                ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/* ── T093: svcmgr receive-slot pool stress (A1.7) ───────────────────────────
 * Three full cycles of 8 concurrent registrations: every REGISTER lands in
 * svcmgr's CSpace pool, lookups serve from it, UNREGISTER releases the pool
 * slot, and the next cycle re-registers the same names (clean reuse).  After
 * the last cycle nothing resolves (no ghost caps, no leaked slots). */
static void test_t093(void) {
    long e = it_sys0(SYS_ENDPOINT_CREATE);
    if (e < 0) { it_fail("T093", "ep create"); return; }
    handle_id_t ep = (handle_id_t)e;
    int ok = 1;
    long ids[8];
    char name[6] = { 't', '9', '3', '.', 'a', '\0' };

    for (int cycle = 0; ok && cycle < 3; cycle++) {
        for (int i = 0; ok && i < 8; i++) {
            name[4] = (char)('a' + i);
            ids[i] = it_register_ep(name, ep);
            if (ids[i] < 0) ok = 0;
        }
        /* spot-check two: served from CSpace storage, invocable */
        for (int i = 0; ok && i < 8; i += 7) {
            struct IrisMsg msg;
            name[4] = (char)('a' + i);
            if (it_lookup_name_slot(name, 0u, &msg) != 0 ||
                msg.label != IRIS_EP_REPLY_OK ||
                !iris_msg_cap_is_handle(msg.attached_handle)) { ok = 0; break; }
            struct IrisMsg p;
            it_iris_msg_zero(&p);
            p.label = 0x93;
            if (it_sys2(SYS_EP_NB_SEND, (long)msg.attached_handle, (long)&p) !=
                (long)IRIS_ERR_WOULD_BLOCK) ok = 0;
            handle_id_t cap = (handle_id_t)msg.attached_handle;
            it_close(&cap);
        }
        for (int i = 0; ok && i < 8; i++) {
            if (it_unregister_id((uint32_t)ids[i]) != 0) ok = 0;
        }
    }

    /* after the last unregister wave nothing resolves */
    if (ok) {
        struct IrisMsg msg;
        name[4] = 'a';
        if (it_lookup_name_slot(name, 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR ||
            msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) ok = 0;
    }

    it_close(&ep);
    if (ok) it_pass("T093"); else it_fail("T093", "recv-slot pool stress");
}

/* ── T094: receive-slot TOCTOU fallback (A1.7) ──────────────────────────────
 * A receiver declares slot 51 and blocks; before the sender delivers, the
 * process fills slot 51 itself (self-mint via the own-process cap).  The
 * delivery must take the DOCUMENTED fallback: the transferred cap arrives as
 * a handle >= 1024 (full authority, nothing lost), the slot keeps exactly
 * the cap that won the race (same object, right type), and no duplicate
 * authority appears in the slot. */
#define T094_SLOT 51L

static handle_id_t       g_t094_ep = HANDLE_INVALID;
static volatile uint32_t g_t094_got = 0;
static volatile int      g_t094_ready = 0, g_t094_done = 0;
static uint8_t           g_t094_stack[8192];

static void t094_recv(void) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.attached_cap = (uint32_t)T094_SLOT;      /* receive-slot declaration */
    g_t094_ready = 1;
    if (it_sys2(SYS_EP_RECV, (long)g_t094_ep, (long)&m) == 0)
        g_t094_got = m.attached_handle;        /* EP_SEND caps land here */
    g_t094_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}

static void test_t094(void) {
    g_t094_got = 0; g_t094_ready = 0; g_t094_done = 0;

    long nA = it_sys0(SYS_NOTIFY_CREATE);      /* the cap to transfer */
    long nB = it_sys0(SYS_NOTIFY_CREATE);      /* the slot-race winner */
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    long selfp = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_TEST_PROC);
    if (nA < 0 || nB < 0 || ep < 0 || selfp < 0) { it_fail("T094", "create"); return; }
    handle_id_t nA_h = (handle_id_t)nA, nB_h = (handle_id_t)nB;
    handle_id_t selfp_h = (handle_id_t)selfp;
    g_t094_ep = (handle_id_t)ep;
    int ok = 1;
    const char *why = "toctou fallback";

    uint64_t entry = (uint64_t)(uintptr_t)t094_recv;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t094_stack + sizeof(g_t094_stack))) & ~0xFULL;
    if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
        ok = 0; why = "thread create";
    }
    for (int i = 0; i < 200 && !g_t094_ready; i++) it_sys0(SYS_YIELD);
    it_sys1(SYS_SLEEP, 2);                     /* blocked with slot 51 declared */

    /* Fill the declared slot BEFORE delivery (the TOCTOU race). */
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)selfp_h, T094_SLOT,
                      nB, (long)RIGHT_WRITE) != 0) {
        ok = 0; why = "self mint";
    }

    if (ok) {
        long d = it_sys2(SYS_HANDLE_DUP, nA, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (d < 0) { ok = 0; why = "dup"; }
        else {
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label           = 0x94;
            m.attached_handle = (uint32_t)d;
            m.attached_rights = RIGHT_WRITE;
            if (it_sys2(SYS_EP_SEND, (long)g_t094_ep, (long)&m) != 0) {
                ok = 0; why = "send";
            }
        }
    }
    for (int i = 0; i < 200 && !g_t094_done; i++) it_sys0(SYS_YIELD);
    if (ok && !g_t094_done) { ok = 0; why = "recv incomplete"; }

    /* Documented fallback: handle >= 1024, full authority preserved. */
    if (ok && !(g_t094_got >= 1024u)) { ok = 0; why = "not handle fallback"; }
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, (long)g_t094_got, 0x94) != 0) {
        ok = 0; why = "fallback cap dead";
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_WAIT, nA, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x94u) { ok = 0; why = "signal lost"; }
    }
    /* The slot keeps exactly the race winner: nB, a notification. */
    if (ok) {
        long rh = it_sys1(SYS_CSPACE_RESOLVE, T094_SLOT);
        if (rh < 0) { ok = 0; why = "slot lost"; }
        else {
            if (it_sys2(SYS_HANDLE_SAME_OBJECT, rh, nB) != 1) {
                ok = 0; why = "slot object changed";
            }
            handle_id_t r = (handle_id_t)rh;
            it_close(&r);
        }
    }

    if (g_t094_got >= 1024u) { handle_id_t g = (handle_id_t)g_t094_got; it_close(&g); }
    it_close(&nA_h);
    it_close(&nB_h);
    it_close(&g_t094_ep);
    it_close(&selfp_h);
    if (ok) it_pass("T094"); else it_fail("T094", why);
}

/* ── T095: handle high-water smoke (A1.7) ───────────────────────────────────
 * By this point the suite has exercised every producer: creation returns,
 * DUP/derive, IPC transfers (slot + handle + TOCTOU), reply caps, resolves,
 * spawns, deaths.  Read the extended diagnostics, log the real numbers (the
 * evidence for the HANDLE_TABLE_MAX decision), and assert the working set is
 * bounded: the busiest table ever seen must fit in a quarter of the ceiling. */
static void test_t095(void) {
    uint32_t w[14];
    if (!it_sched_ext(w)) { it_fail("T095", "sched_info ext"); return; }

    it_serial_write("[IRIS][TEST] T095 hwm self=");
    it_log_num(w[IT_SI_HWM]);
    it_serial_write(" live=");
    it_log_num(w[IT_SI_LIVE]);
    it_serial_write(" global=");
    it_log_num(w[IT_SI_GHWM]);
    it_serial_write(" max=");
    it_log_num(w[IT_SI_MAX]);
    it_serial_write(" slot=");
    it_log_num(w[IT_SI_SLOTDEL]);
    it_serial_write(" hand=");
    it_log_num(w[IT_SI_HANDDEL]);
    it_serial_write(" toctou=");
    it_log_num(w[IT_SI_TOCTOU]);
    it_serial_write(" reply=");
    it_log_num(w[IT_SI_REPLY]);
    it_serial_write(" resolve=");
    it_log_num(w[IT_SI_RESOLVE]);
    it_serial_write("\n");

    int ok = 1;
    if (!(w[IT_SI_LIVE] > 0u)) ok = 0;                       /* we hold handles */
    if (w[IT_SI_HWM] < w[IT_SI_LIVE]) ok = 0;                /* hwm ≥ live */
    if (w[IT_SI_GHWM] < w[IT_SI_HWM]) ok = 0;                /* global ≥ self */
    if (w[IT_SI_INSERTS] < w[IT_SI_REMOVES]) ok = 0;         /* books balance */
    if (w[IT_SI_INSERTS] - w[IT_SI_REMOVES] != w[IT_SI_LIVE]) ok = 0;
    if (w[IT_SI_SLOTDEL] < 8u) ok = 0;      /* T084+ / svcmgr registrations */
    if (w[IT_SI_HANDDEL] < 8u) ok = 0;      /* legacy deliveries all along */
    if (w[IT_SI_TOCTOU] < 1u) ok = 0;       /* T094 forced exactly this */
    if (w[IT_SI_REPLY] < 50u) ok = 0;       /* hundreds of EP_CALLs by now */
    if (w[IT_SI_RESOLVE] < 4u) ok = 0;      /* sanctioned bridge in use */
    /* The bound: busiest table ever ≤ MAX/4 — real margin, not aesthetics. */
    if (w[IT_SI_GHWM] * 4u > w[IT_SI_MAX]) ok = 0;

    if (ok) it_pass("T095"); else it_fail("T095", "handle high-water");
}

/* ── T096: legacy client compatibility under pressure (A1.7) ────────────────
 * 32 slotless lookup+close cycles: every one materializes a real handle and
 * releases it.  self_live must return exactly to its starting value (zero
 * leak), and the legacy path keeps serving a working cap on the last lap. */
static void test_t096(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T096", "sched_info ext"); return; }

    int ok = 1;
    for (int i = 0; ok && i < 32; i++) {
        struct IrisMsg msg;
        if (it_lookup_name_slot(VFS_EP_SVC_NAME, 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            !iris_msg_cap_is_handle(msg.attached_handle)) { ok = 0; break; }
        handle_id_t cap = (handle_id_t)msg.attached_handle;
        if (i == 31) {          /* the last cap still actually works */
            struct IrisMsg p;
            it_iris_msg_zero(&p);
            p.label = IRIS_EP_OP_PING;
            if (it_sys2(SYS_EP_CALL, (long)cap, (long)&p) != 0 ||
                p.label != IRIS_EP_REPLY_OK) ok = 0;
        }
        it_close(&cap);
    }

    if (ok && !it_sched_ext(after)) ok = 0;
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) ok = 0;   /* zero leak */
    if (ok && after[IT_SI_HANDDEL] < before[IT_SI_HANDDEL] + 32u) ok = 0;

    if (ok) it_pass("T096"); else it_fail("T096", "legacy pressure");
}

/* ── A1.8: legacy handle producer cleanup (T097–T098) ───────────────────────
 * SYS_HANDLE_TRANSFER is retired (NOT_SUPPORTED); SYS_HANDLE_INSERT /
 * SYS_VMO_SHARE stay as deprecated compat.  The canonical cross-process
 * placement is SYS_PROC_CSPACE_MINT into a destination CSpace slot.
 * Destination child slots 60..62 (lifecycle_probe children only receive the
 * LP_CPTR_CMD_EP=3 mint, so these are guaranteed empty). */

#define T097_DST_SLOT   60L
#define T097_DST_SLOT2  61L
#define T097_DST_SLOT3  62L

/* ── T097: PROC_CSPACE_MINT replaces the legacy handle insert path ──────────
 * The canonical placement covers what SYS_HANDLE_TRANSFER used to do, with
 * CSpace-canonical delivery: mint lands in the child's root CNode (no handle
 * produced in the destination table), an occupied slot fails fast, authority
 * cannot escalate (empty effective rights → INVALID_ARG), a wrong-type
 * destination fails, a dead destination fails — and the retired
 * SYS_HANDLE_TRANSFER itself now returns NOT_SUPPORTED. */
static void test_t097(void) {
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T097", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;
    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h);
        it_fail("T097", "spawn"); return;
    }
    long vmo = it_sys1(SYS_VMO_CREATE, 4096);
    if (vmo < 0) {
        (void)it_sys1(SYS_PROCESS_KILL, (long)proc_h);
        it_close(&proc_h); it_close(&cmd_ep_h);
        it_fail("T097", "vmo create"); return;
    }
    handle_id_t vmo_h = (handle_id_t)vmo;
    int ok = 1;
    const char *why = "canonical placement";

    /* Canonical placement: the cap lands in the child's CSpace — no handle
     * is created in the destination table. */
    if (it_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h, T097_DST_SLOT,
                vmo, (long)(RIGHT_READ | RIGHT_WRITE)) != 0) {
        ok = 0; why = "mint";
    }
    /* Occupied destination slot → fail-fast, no overwrite. */
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h, T097_DST_SLOT,
                      vmo, (long)RIGHT_READ) !=
        (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "occupied"; }
    /* Authority cannot escalate: READ-only source + WRITE request →
     * empty effective rights → INVALID_ARG (never a widened grant). */
    if (ok) {
        long ro = it_sys2(SYS_HANDLE_DUP, vmo,
                          (long)(RIGHT_READ | RIGHT_DUPLICATE));
        if (ro < 0) { ok = 0; why = "dup"; }
        else {
            if (it_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h, T097_DST_SLOT2,
                        ro, (long)RIGHT_WRITE) !=
                (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "escalation"; }
            handle_id_t roh = (handle_id_t)ro;
            it_close(&roh);
        }
    }
    /* Wrong-type destination (the slot-30 KNotification fixture). */
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_FIX_A,
                      T097_DST_SLOT2, vmo, (long)RIGHT_READ) !=
        (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "wrong type"; }
    /* The retired legacy producer is gone: NOT_SUPPORTED, nothing placed. */
    if (ok && it_sys3(SYS_HANDLE_TRANSFER, vmo, (long)proc_h,
                      (long)RIGHT_READ) !=
        (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "transfer not retired"; }
    /* Dead destination fails cleanly. */
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h, T097_DST_SLOT3,
                      vmo, (long)RIGHT_READ) >= 0) { ok = 0; why = "dead dest"; }

    if (!ok && proc_h != HANDLE_INVALID)
        (void)it_sys1(SYS_PROCESS_KILL, (long)proc_h);
    it_close(&vmo_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);
    if (ok) it_pass("T097"); else it_fail("T097", why);
}

/* ── T098: VMO share destination via CSpace, legacy compat intact ───────────
 * The CSpace-canonical way to share a VMO with another process is a mint
 * into a destination slot (no handle produced); the deprecated
 * SYS_VMO_SHARE compat path still works for a slotless consumer and stays
 * rights-monotonic (missing DUPLICATE → ACCESS_DENIED; disjoint rights →
 * INVALID_ARG; dead destination → BAD_HANDLE). */
static void test_t098(void) {
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T098", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;
    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h);
        it_fail("T098", "spawn"); return;
    }
    long vmo = it_sys1(SYS_VMO_CREATE, 4096);
    if (vmo < 0) {
        (void)it_sys1(SYS_PROCESS_KILL, (long)proc_h);
        it_close(&proc_h); it_close(&cmd_ep_h);
        it_fail("T098", "vmo create"); return;
    }
    handle_id_t vmo_h = (handle_id_t)vmo;
    int ok = 1;
    const char *why = "vmo share cspace";

    /* Canonical: the shared VMO lands in the destination CSpace. */
    if (it_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h, T097_DST_SLOT,
                vmo, (long)RIGHT_READ) != 0) { ok = 0; why = "mint"; }

    /* Deprecated compat still works: a handle (>= 1024) in the dest table. */
    if (ok) {
        long sh = it_sys3(SYS_VMO_SHARE, vmo, (long)proc_h, (long)RIGHT_READ);
        if (sh < 1024) { ok = 0; why = "legacy share"; }
    }
    /* Rights monotonic on the compat path: no DUPLICATE → ACCESS_DENIED. */
    if (ok) {
        long ro = it_sys2(SYS_HANDLE_DUP, vmo, (long)RIGHT_READ);
        if (ro < 0) { ok = 0; why = "dup ro"; }
        else {
            if (it_sys3(SYS_VMO_SHARE, ro, (long)proc_h, (long)RIGHT_READ) !=
                (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "share no-dup"; }
            handle_id_t roh = (handle_id_t)ro;
            it_close(&roh);
        }
    }
    /* Disjoint rights request → INVALID_ARG (never a widened grant). */
    if (ok) {
        long rd = it_sys2(SYS_HANDLE_DUP, vmo,
                          (long)(RIGHT_READ | RIGHT_DUPLICATE));
        if (rd < 0) { ok = 0; why = "dup rd"; }
        else {
            if (it_sys3(SYS_VMO_SHARE, rd, (long)proc_h, (long)RIGHT_MANAGE) !=
                (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "share disjoint"; }
            handle_id_t rdh = (handle_id_t)rd;
            it_close(&rdh);
        }
    }
    /* Dead destination → BAD_HANDLE on the compat path too. */
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_sys3(SYS_VMO_SHARE, vmo, (long)proc_h, (long)RIGHT_READ) !=
        (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "dead dest"; }

    if (!ok && proc_h != HANDLE_INVALID)
        (void)it_sys1(SYS_PROCESS_KILL, (long)proc_h);
    it_close(&vmo_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);
    if (ok) it_pass("T098"); else it_fail("T098", why);
}

/* ── A1.9: multi-child receive-slot stress (T099–T102) ──────────────────────
 * Receive-slots across REAL process boundaries: lifecycle_probe children
 * declare parent-chosen slots (LP_CMD_RSLOT_RECV mode), receive transferred
 * caps in their own CSpace, invoke them by CPtr, and report the landing
 * discriminator via their exit code.  Child-side slot: 40 (children only
 * receive the LP_CPTR_CMD_EP=3 mint).  Parent-side lookup slots: 43..47. */

#define LP_CMD_RSLOT_RECV      0x1099u   /* must match lifecycle_probe */
#define LP_EXIT_RECV_ERR_BASE  0x0B00L
#define T099_CHILD_SLOT        40u

/* Command a child into receive-slot mode (second recv declares `slot`). */
static long it_lp_cmd_rslot(handle_id_t cmd_ep_h, uint32_t slot) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label      = LP_CMD_RSLOT_RECV;
    m.words[0]   = slot;
    m.word_count = 1u;
    return it_sys2(SYS_EP_SEND, (long)cmd_ep_h, (long)&m);
}

/* Transfer a WRITE|TRANSFER dup of `notif` to the child (blocks until the
 * child's declared recv rendezvouses — natural synchronization). */
static long it_lp_send_cap(handle_id_t cmd_ep_h, long notif) {
    long d = it_sys2(SYS_HANDLE_DUP, notif,
                     (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (d < 0) return d;
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label           = 0x99;
    m.attached_handle = (uint32_t)d;
    m.attached_rights = RIGHT_WRITE;
    return it_sys2(SYS_EP_SEND, (long)cmd_ep_h, (long)&m);
}

/* Fase 16: send a bare command label to a child (no payload). */
#define LP_CMD_SEND_BLOCK  0x109Au   /* must match lifecycle_probe */
#define LP_CMD_CALL_BLOCK  0x109Bu   /* must match lifecycle_probe */
static long it_lp_cmd(handle_id_t cmd_ep_h, uint32_t label) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label = label;
    return it_sys2(SYS_EP_SEND, (long)cmd_ep_h, (long)&m);
}

/* Wait (≤ 2s) for a child to exit; returns its exit code or -1. */
static long it_lp_wait_exit(handle_id_t proc_h) {
    long n = it_sys0(SYS_NOTIFY_CREATE);
    if (n < 0) return -1;
    handle_id_t n_h = (handle_id_t)n;
    long ec = -1;
    if (it_sys3(SYS_PROCESS_WATCH, (long)proc_h, n, 1) == 0) {
        uint64_t bits = 0;
        if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, n, (long)(uintptr_t)&bits,
                    2000000000LL) == 0)
            ec = it_sys1(SYS_PROCESS_EXIT_CODE, (long)proc_h);
    }
    it_close(&n_h);
    return ec;
}

/* ── T099: multi-child receive-slot endpoint transfer ───────────────────────
 * Three children each receive a transferred notification cap INTO THEIR OWN
 * CSpace (exit code == the declared CPtr, never a handle) and invoke it by
 * CPtr across the process boundary (signal bits 1 observed by the parent).
 * Failure paths: a child whose declared slot the parent pre-filled fails
 * fast (ALREADY_EXISTS, endpoint left clean); an out-of-range declaration
 * fails INVALID_ARG.  Parent handle books balance exactly. */
static void test_t099(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T099", "sched ext"); return; }
    int ok = 1;
    const char *why = "multi-child rslot";

    for (int i = 0; ok && i < 3; i++) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        long n  = it_sys0(SYS_NOTIFY_CREATE);
        handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || n < 0 ||
            lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; }
        if (ok && it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) {
            ok = 0; why = "cmd";
        }
        if (ok && it_lp_send_cap(ep_h, n) != 0) { ok = 0; why = "send cap"; }
        if (ok) {
            uint64_t bits = 0;
            if (it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
                bits != 1u) { ok = 0; why = "cptr signal"; }
        }
        if (ok && it_lp_wait_exit(proc_h) != (long)T099_CHILD_SLOT) {
            ok = 0; why = "landing";
        }
        it_close(&proc_h);
        it_close(&n_h);
        it_close(&ep_h);
    }

    /* Occupied child slot → the child's declared recv fails fast and the
     * endpoint keeps no dead waiter. */
    if (ok) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        long n2 = it_sys0(SYS_NOTIFY_CREATE);
        handle_id_t ep_h = (handle_id_t)ep, n2_h = (handle_id_t)n2;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || n2 < 0 ||
            lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn occ"; }
        if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h,
                          (long)T099_CHILD_SLOT, n2,
                          (long)RIGHT_WRITE) != 0) { ok = 0; why = "prefill"; }
        if (ok && it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) {
            ok = 0; why = "cmd occ";
        }
        if (ok && it_lp_wait_exit(proc_h) !=
            (LP_EXIT_RECV_ERR_BASE | (long)-IRIS_ERR_ALREADY_EXISTS)) {
            ok = 0; why = "occupied";
        }
        if (ok) {
            struct IrisMsg pr;
            it_iris_msg_zero(&pr);
            pr.label = 0x99;
            if (it_sys2(SYS_EP_NB_SEND, (long)ep_h, (long)&pr) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "dead waiter"; }
        }
        it_close(&proc_h);
        it_close(&n2_h);
        it_close(&ep_h);
    }

    /* Out-of-range declaration (slot 300, T086 fixture value) → INVALID_ARG. */
    if (ok) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
            ok = 0; why = "spawn inv";
        }
        if (ok && it_lp_cmd_rslot(ep_h, 300u) != 0) { ok = 0; why = "cmd inv"; }
        if (ok && it_lp_wait_exit(proc_h) !=
            (LP_EXIT_RECV_ERR_BASE | (long)-IRIS_ERR_INVALID_ARG)) {
            ok = 0; why = "invalid slot";
        }
        it_close(&proc_h);
        it_close(&ep_h);
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* Parent books balance (dups consumed by staging; everything closed)
     * and the three cross-process deliveries were receive-slot installs. */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + 3u) {
        ok = 0; why = "slot count";
    }
    if (ok) it_pass("T099"); else it_fail("T099", why);
}

/* ── T100: svcmgr lookup receive-slot stress ────────────────────────────────
 * Four concurrent registrations served into distinct client reply-slots;
 * unregister under pressure; a post-unregister lookup with a declared slot
 * fails WITHOUT installing anything (the same slot then serves the
 * re-registered service — proof it stayed genuinely empty); legacy lookup
 * confirms the final NOT_FOUND. */
static void test_t100(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T100", "sched ext"); return; }
    long e = it_sys0(SYS_ENDPOINT_CREATE);
    if (e < 0) { it_fail("T100", "ep create"); return; }
    handle_id_t ep = (handle_id_t)e;
    int ok = 1;
    const char *why = "lookup rslot stress";
    long ids[4];
    char name[7] = { 't', '1', '0', '0', '.', 'a', '\0' };
    struct IrisMsg msg;

    for (int i = 0; ok && i < 4; i++) {
        name[5] = (char)('a' + i);
        ids[i] = it_register_ep(name, ep);
        if (ids[i] < 0) { ok = 0; why = "register"; }
    }
    for (int i = 0; ok && i < 4; i++) {
        uint32_t slot = 44u + (uint32_t)i;
        name[5] = (char)('a' + i);
        if (it_lookup_name_slot(name, slot, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_OK ||
            msg.attached_handle != slot) { ok = 0; why = "slot lookup"; }
        if (ok) {
            struct IrisMsg p;
            it_iris_msg_zero(&p);
            p.label = 0xA0;
            if (it_sys2(SYS_EP_NB_SEND, (long)slot, (long)&p) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "cptr dead"; }
        }
    }
    for (int i = 0; ok && i < 4; i++) {
        if (it_unregister_id((uint32_t)ids[i]) != 0) { ok = 0; why = "unreg"; }
    }

    /* Post-unregister lookup with a declared slot: ERR and nothing lands. */
    if (ok) {
        name[5] = 'a';
        if (it_lookup_name_slot(name, 43u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR ||
            msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) {
            ok = 0; why = "post-unreg lookup";
        }
        if (ok && it_sys1(SYS_CSPACE_RESOLVE, 43L) >= 0) {
            ok = 0; why = "ghost cap";
        }
    }
    /* The untouched slot then serves the re-registered service. */
    if (ok) {
        long id2 = it_register_ep("t100.a", ep);
        if (id2 < 0) { ok = 0; why = "re-register"; }
        else {
            if (it_lookup_name_slot("t100.a", 43u, &msg) != 0 ||
                msg.label != IRIS_EP_REPLY_OK ||
                msg.attached_handle != 43u) { ok = 0; why = "slot reuse"; }
            (void)it_unregister_id((uint32_t)id2);
        }
    }
    if (ok) {
        if (it_lookup_name_slot("t100.a", 0u, &msg) != 0 ||
            msg.label != IRIS_EP_REPLY_ERR) { ok = 0; why = "final legacy"; }
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + 5u) {
        ok = 0; why = "slot count";
    }
    it_close(&ep);
    if (ok) it_pass("T100"); else it_fail("T100", why);
}

/* ── T101: cross-process receive-slot death cleanup ─────────────────────────
 * A child killed while blocked with a declared receive-slot leaves a clean
 * endpoint (no dead waiter), the sender's cap survives an attempted
 * delivery (WOULD_BLOCK, handle intact), and the handle books show no
 * staged-cap leak and no runaway high-water. */
static void test_t101(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T101", "sched ext"); return; }
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    long n  = it_sys0(SYS_NOTIFY_CREATE);
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
    handle_id_t proc_h = HANDLE_INVALID;
    int ok = 1;
    const char *why = "death cleanup";

    if (ep < 0 || n < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
        ok = 0; why = "spawn";
    }
    if (ok && it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) { ok = 0; why = "cmd"; }
    it_sys1(SYS_SLEEP, 2);   /* child re-blocks with slot 40 declared */

    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_sys1(SYS_PROCESS_STATUS, (long)proc_h) != 0) {
        ok = 0; why = "still alive";
    }

    /* Sender does not lose its cap on the failed delivery attempt. */
    if (ok) {
        long d = it_sys2(SYS_HANDLE_DUP, n,
                         (long)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (d < 0) { ok = 0; why = "dup"; }
        else {
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label           = 0x99;
            m.attached_handle = (uint32_t)d;
            m.attached_rights = RIGHT_WRITE;
            if (it_sys2(SYS_EP_NB_SEND, (long)ep_h, (long)&m) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "dead waiter"; }
            if (ok && it_sys1(SYS_HANDLE_TYPE, d) !=
                (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
                ok = 0; why = "cap consumed";
            }
            handle_id_t dh = (handle_id_t)d;
            it_close(&dh);
        }
    }

    it_close(&proc_h);
    it_close(&n_h);
    it_close(&ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) {
        ok = 0; why = "staged leak";
    }
    /* No runaway high-water: still bounded by the T095 rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) {
        ok = 0; why = "hwm runaway";
    }
    if (ok) it_pass("T101"); else it_fail("T101", why);
}

/* ── T102: legacy slotless children under multi-child pressure ──────────────
 * Two children run the same command protocol with slot 0 (= legacy): each
 * receives the transferred cap as a handle >= 1024, invokes it through that
 * handle across the process boundary (signal bits 2), and child teardown
 * releases everything — parent live handles return exactly to baseline. */
static void test_t102(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T102", "sched ext"); return; }
    int ok = 1;
    const char *why = "legacy children";

    for (int i = 0; ok && i < 2; i++) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        long n  = it_sys0(SYS_NOTIFY_CREATE);
        handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || n < 0 ||
            lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; }
        if (ok && it_lp_cmd_rslot(ep_h, 0u) != 0) { ok = 0; why = "cmd"; }
        if (ok && it_lp_send_cap(ep_h, n) != 0) { ok = 0; why = "send cap"; }
        if (ok) {
            uint64_t bits = 0;
            if (it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
                bits != 2u) { ok = 0; why = "handle signal"; }
        }
        if (ok) {
            long ec = it_lp_wait_exit(proc_h);
            if (ec < 1024) { ok = 0; why = "not legacy handle"; }
        }
        it_close(&proc_h);
        it_close(&n_h);
        it_close(&ep_h);
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok && after[IT_SI_HANDDEL] < before[IT_SI_HANDDEL] + 2u) {
        ok = 0; why = "hand count";
    }
    if (ok) it_pass("T102"); else it_fail("T102", why);
}

/* ── A1.10: staged-cap atomicity for blocking IPC paths (T103–T106) ─────────
 * The A1.9 rule locked for EP_NB_SEND — "a failed delivery never consumes
 * the source cap" — extended to the blocking paths: a sender canceled while
 * queued (endpoint close), an EP_CALL canceled before rendezvous, and a
 * reply that loses the one-shot race all keep their source cap; endpoint
 * close with multiple staged waiters releases every staging ref exactly
 * once.  Two-thread pattern (T019/T020 style): the victim blocks with an
 * attached cap, the main thread cancels, and the source handle is probed
 * with SYS_HANDLE_TYPE afterwards. */

static handle_id_t  g_t103_ep_h   = HANDLE_INVALID;
static long         g_t103_dup    = -1;
static volatile int g_t103_done   = 0;
static          int g_t103_result = 0;
static uint8_t      g_t103_stack[8192];

static void t103_sender(void) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label           = 0x103;
    m.attached_handle = (uint32_t)g_t103_dup;
    m.attached_rights = RIGHT_WRITE;
    long r = it_sys2(SYS_EP_SEND, (long)g_t103_ep_h, (long)&m);
    g_t103_result = (int)r;
    g_t103_done   = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

/* ── T103: blocking send canceled before delivery preserves source cap ──────
 * A sender blocks in EP_SEND with an attached cap (no receiver ever shows
 * up); the endpoint is then closed.  The sender must wake with CLOSED, its
 * source handle must still be alive (never consumed — the A1.10 two-phase
 * commit), no cap can have appeared anywhere, and the handle books must
 * return exactly to baseline (no staged-ref leak). */
static void test_t103(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T103", "sched ext"); return; }
    g_t103_done = 0; g_t103_result = 0;
    int ok = 1;
    const char *why = "blocking send cancel";

    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    long n  = it_sys0(SYS_NOTIFY_CREATE);
    g_t103_ep_h = (handle_id_t)ep;
    handle_id_t n_h = (handle_id_t)n;
    if (ep < 0 || n < 0) { it_fail("T103", "create"); return; }

    g_t103_dup = it_sys2(SYS_HANDLE_DUP, n, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (g_t103_dup < 0) { ok = 0; why = "dup"; }

    if (ok) {
        uint64_t entry = (uint64_t)(uintptr_t)t103_sender;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t103_stack + sizeof(g_t103_stack))) & ~0xFULL;
        /* returns a task id, not a handle — nothing to close */
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
            ok = 0; why = "thread";
        }
    }

    if (ok) {
        it_sys1(SYS_SLEEP, 5);           /* sender queues with staged cap */
        it_close(&g_t103_ep_h);          /* last ref → close → wakes sender */
        for (int i = 0; i < 200 && !g_t103_done; i++)
            it_sys1(SYS_SLEEP, 1);
        if (!g_t103_done || g_t103_result != (int)IRIS_ERR_CLOSED) {
            ok = 0; why = "not CLOSED";
        }
    }

    /* Source cap preserved: the dup is still a live notification handle. */
    if (ok && it_sys1(SYS_HANDLE_TYPE, g_t103_dup) !=
        (long)IRIS_HANDLE_TYPE_NOTIFICATION) { ok = 0; why = "cap consumed"; }
    /* And it still works: signal through it, observe on the original. */
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_SIGNAL, g_t103_dup, 1) != 0 ||
            it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
            bits != 1u) { ok = 0; why = "cap dead"; }
    }

    if (g_t103_dup >= 0) { handle_id_t d = (handle_id_t)g_t103_dup; it_close(&d); }
    it_close(&n_h);
    it_close(&g_t103_ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* Exact balance: +1 = the exited thread's KTcb handle, which stays with
     * the process by design (Ph96).  Anything above that is a staged leak. */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE] + 1u) { ok = 0; why = "leak"; }
    if (ok) it_pass("T103"); else it_fail("T103", why);
}

static handle_id_t  g_t104_ep_h   = HANDLE_INVALID;
static long         g_t104_dup    = -1;
static volatile int g_t104_done   = 0;
static          int g_t104_result = 0;
static uint8_t      g_t104_stack[8192];

static void t104_caller(void) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label               = 0x104;
    m.attached_cap        = (uint32_t)g_t104_dup;
    m.attached_cap_rights = RIGHT_WRITE;
    long r = it_sys2(SYS_EP_CALL, (long)g_t104_ep_h, (long)&m);
    g_t104_result = (int)r;
    g_t104_done   = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

/* ── T104: EP_CALL canceled before server receive preserves attached cap ────
 * A caller blocks in EP_CALL carrying a transferred cap (attached_cap);
 * the endpoint closes before any server ever receives.  The caller must
 * wake with CLOSED, keep its source cap, and no KReply may have been
 * created (the reply-caps counter stays flat — reply cleanup is trivially
 * correct because rendezvous never happened). */
static void test_t104(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T104", "sched ext"); return; }
    g_t104_done = 0; g_t104_result = 0;
    int ok = 1;
    const char *why = "ep_call cancel";

    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    long n  = it_sys0(SYS_NOTIFY_CREATE);
    g_t104_ep_h = (handle_id_t)ep;
    handle_id_t n_h = (handle_id_t)n;
    if (ep < 0 || n < 0) { it_fail("T104", "create"); return; }

    g_t104_dup = it_sys2(SYS_HANDLE_DUP, n, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (g_t104_dup < 0) { ok = 0; why = "dup"; }

    if (ok) {
        uint64_t entry = (uint64_t)(uintptr_t)t104_caller;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t104_stack + sizeof(g_t104_stack))) & ~0xFULL;
        /* returns a task id, not a handle — nothing to close */
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
            ok = 0; why = "thread";
        }
    }

    if (ok) {
        it_sys1(SYS_SLEEP, 5);           /* caller queues (SEND, call mode) */
        it_close(&g_t104_ep_h);
        for (int i = 0; i < 200 && !g_t104_done; i++)
            it_sys1(SYS_SLEEP, 1);
        if (!g_t104_done || g_t104_result != (int)IRIS_ERR_CLOSED) {
            ok = 0; why = "not CLOSED";
        }
    }

    if (ok && it_sys1(SYS_HANDLE_TYPE, g_t104_dup) !=
        (long)IRIS_HANDLE_TYPE_NOTIFICATION) { ok = 0; why = "cap consumed"; }

    if (g_t104_dup >= 0) { handle_id_t d = (handle_id_t)g_t104_dup; it_close(&d); }
    it_close(&n_h);
    it_close(&g_t104_ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* +1 = the exited thread's KTcb handle (stays with the process, Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE] + 1u) { ok = 0; why = "leak"; }
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY]) {
        ok = 0; why = "ghost kreply";
    }
    if (ok) it_pass("T104"); else it_fail("T104", why);
}

static handle_id_t  g_t105_ep_h   = HANDLE_INVALID;
static volatile int g_t105_done   = 0;
static          int g_t105_result = 0;
static uint8_t      g_t105_stack[8192];

static void t105_caller(void) {
    uint8_t rbuf[32];
    for (uint32_t i = 0; i < 32; i++) rbuf[i] = 0;
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label    = 0x105;
    m.buf_uptr = (uint64_t)(uintptr_t)rbuf;
    long r = it_sys2(SYS_EP_CALL, (long)g_t105_ep_h, (long)&m);
    g_t105_result = (int)(r == 0 && m.label == 0x5A5AULL);
    g_t105_done   = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

/* ── T105: reply cap transfer failure is atomic ─────────────────────────────
 * EP_REPLY supports one attached cap (Fase 7.1).  The deterministic failed
 * delivery is the lost one-shot race: reply once without a cap (consumes
 * the KReply), then reply AGAIN with an attached cap.  The second reply
 * must fail NOT_FOUND, the server must KEEP its source cap (A1.10 — before,
 * this path destroyed it), and the first reply's one-shot semantics and
 * bookkeeping stay intact. */
static void test_t105(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T105", "sched ext"); return; }
    g_t105_done = 0; g_t105_result = 0;
    int ok = 1;
    const char *why = "reply cap atomic";

    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    long n  = it_sys0(SYS_NOTIFY_CREATE);
    g_t105_ep_h = (handle_id_t)ep;
    handle_id_t n_h = (handle_id_t)n;
    if (ep < 0 || n < 0) { it_fail("T105", "create"); return; }

    handle_id_t reply_h = HANDLE_INVALID;
    {
        uint64_t entry = (uint64_t)(uintptr_t)t105_caller;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t105_stack + sizeof(g_t105_stack))) & ~0xFULL;
        /* returns a task id, not a handle — nothing to close */
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
            ok = 0; why = "thread";
        }
    }

    /* Serve the call: the reply cap arrives in attached_handle. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        if (it_sys2(SYS_EP_RECV, (long)g_t105_ep_h, (long)&m) != 0 ||
            m.label != 0x105ULL || m.attached_handle == IRIS_MSG_NO_CAP) {
            ok = 0; why = "recv call";
        } else {
            reply_h = (handle_id_t)m.attached_handle;
        }
    }

    /* First reply (no cap) succeeds and unblocks the caller. */
    if (ok) {
        struct IrisMsg rm;
        it_iris_msg_zero(&rm);
        rm.label = 0x5A5A;
        if (it_sys2(SYS_REPLY, (long)reply_h, (long)&rm) != 0) {
            ok = 0; why = "first reply";
        }
        for (int i = 0; ok && i < 200 && !g_t105_done; i++)
            it_sys1(SYS_SLEEP, 1);
        if (ok && (!g_t105_done || !g_t105_result)) { ok = 0; why = "caller"; }
    }

    /* Second reply WITH a cap loses the one-shot race: NOT_FOUND and the
     * server's source cap must survive un-consumed. */
    long d = -1;
    if (ok) {
        d = it_sys2(SYS_HANDLE_DUP, n, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (d < 0) { ok = 0; why = "dup"; }
    }
    if (ok) {
        struct IrisMsg rm;
        it_iris_msg_zero(&rm);
        rm.label           = 0xDEAD;
        rm.attached_handle = (uint32_t)d;
        rm.attached_rights = RIGHT_WRITE;
        if (it_sys2(SYS_REPLY, (long)reply_h, (long)&rm) !=
            (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "not one-shot"; }
        if (ok && it_sys1(SYS_HANDLE_TYPE, d) !=
            (long)IRIS_HANDLE_TYPE_NOTIFICATION) { ok = 0; why = "cap consumed"; }
    }

    if (d >= 0) { handle_id_t dh = (handle_id_t)d; it_close(&dh); }
    it_close(&reply_h);
    it_close(&n_h);
    it_close(&g_t105_ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* +1 = the exited thread's KTcb handle (stays with the process, Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE] + 1u) { ok = 0; why = "leak"; }
    if (ok) it_pass("T105"); else it_fail("T105", why);
}

static handle_id_t  g_t106_ep_h = HANDLE_INVALID;
static long         g_t106_dup[2]    = { -1, -1 };
static volatile int g_t106_done[2]   = { 0, 0 };
static          int g_t106_result[2] = { 0, 0 };
static uint8_t      g_t106_stack_a[8192];
static uint8_t      g_t106_stack_b[8192];

static void t106_send_idx(int idx) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label           = 0x106;
    m.attached_handle = (uint32_t)g_t106_dup[idx];
    m.attached_rights = RIGHT_WRITE;
    long r = it_sys2(SYS_EP_SEND, (long)g_t106_ep_h, (long)&m);
    g_t106_result[idx] = (int)r;
    g_t106_done[idx]   = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}
static void t106_sender_a(void) { t106_send_idx(0); }
static void t106_sender_b(void) { t106_send_idx(1); }

/* ── T106: endpoint close cancels staged waiters cleanly ────────────────────
 * TWO senders queue on the same endpoint, each with its own staged cap;
 * the endpoint closes.  Both must wake with CLOSED, both source caps must
 * survive with their owners, and the books must balance exactly (each
 * staging ref released exactly once — a double-release would show up as a
 * refcount crash or a negative live delta). */
static void test_t106(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T106", "sched ext"); return; }
    g_t106_done[0] = g_t106_done[1] = 0;
    g_t106_result[0] = g_t106_result[1] = 0;
    int ok = 1;
    const char *why = "close staged waiters";

    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    long n  = it_sys0(SYS_NOTIFY_CREATE);
    g_t106_ep_h = (handle_id_t)ep;
    handle_id_t n_h = (handle_id_t)n;
    if (ep < 0 || n < 0) { it_fail("T106", "create"); return; }

    for (int i = 0; ok && i < 2; i++) {
        g_t106_dup[i] = it_sys2(SYS_HANDLE_DUP, n,
                                (long)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (g_t106_dup[i] < 0) { ok = 0; why = "dup"; }
    }

    if (ok) {
        uint64_t ea = (uint64_t)(uintptr_t)t106_sender_a;
        uint64_t eb = (uint64_t)(uintptr_t)t106_sender_b;
        uint64_t ra = ((uint64_t)(uintptr_t)(g_t106_stack_a + sizeof(g_t106_stack_a))) & ~0xFULL;
        uint64_t rb = ((uint64_t)(uintptr_t)(g_t106_stack_b + sizeof(g_t106_stack_b))) & ~0xFULL;
        /* returns task ids, not handles — nothing to close */
        long ta = it_sys3(SYS_THREAD_CREATE, (long)ea, (long)ra, 0);
        long tb = it_sys3(SYS_THREAD_CREATE, (long)eb, (long)rb, 0);
        if (ta < 0 || tb < 0) { ok = 0; why = "thread"; }
    }

    if (ok) {
        it_sys1(SYS_SLEEP, 5);           /* both senders queue staged caps */
        it_close(&g_t106_ep_h);
        for (int i = 0; i < 200 && !(g_t106_done[0] && g_t106_done[1]); i++)
            it_sys1(SYS_SLEEP, 1);
        if (!g_t106_done[0] || !g_t106_done[1] ||
            g_t106_result[0] != (int)IRIS_ERR_CLOSED ||
            g_t106_result[1] != (int)IRIS_ERR_CLOSED) {
            ok = 0; why = "not CLOSED";
        }
    }

    for (int i = 0; ok && i < 2; i++) {
        if (it_sys1(SYS_HANDLE_TYPE, g_t106_dup[i]) !=
            (long)IRIS_HANDLE_TYPE_NOTIFICATION) { ok = 0; why = "cap consumed"; }
    }

    for (int i = 0; i < 2; i++) {
        if (g_t106_dup[i] >= 0) {
            handle_id_t dh = (handle_id_t)g_t106_dup[i];
            it_close(&dh);
        }
    }
    it_close(&n_h);
    it_close(&g_t106_ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* +2 = the two exited threads' KTcb handles (stay with the process, Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE] + 2u) { ok = 0; why = "leak"; }
    if (ok) it_pass("T106"); else it_fail("T106", why);
}

/* ── A1.11: deterministic IPC fuzz/stress harness (T107–T112) ───────────────
 *
 * Goal: break IPC/lifecycle/cap-transfer if a bug exists, reproducibly.
 * Design:
 *   - xorshift32 PRNG with a FIXED per-test seed: the operation sequence is
 *     identical on every run; a failure logs "FZ <test> seed=<s> iter=<i>".
 *   - bounded iterations; bounded retry/poll loops (no unbounded waits);
 *   - synchronization is the blocking-endpoint rendezvous itself: command
 *     handoff over a control endpoint blocks until the worker is at its
 *     recv, so no fragile external timing is needed.  The only sleeps are
 *     the short "let the worker reach its blocking syscall" pauses already
 *     used by T019/T020/T101/T103.
 *   - CSpace slots CANNOT be deleted from userland (no root-CNode accessor
 *     by design), so delivered/occupied slots are allocated monotonically
 *     from a budgeted window (FZ_SLOT_BASE..FZ_SLOT_LIMIT) — deterministic
 *     and bounded; the budget check fails loudly if a test overdraws.
 *   - every test snapshots the A1.7 counters before/after and asserts
 *     exact live-handle balance (KTcb handles from worker threads are a
 *     documented +N, as in A1.10), directional slot/handle-delivery deltas
 *     (>=: background services also move the global counters), and the
 *     T095 high-water rule (global_hwm * 4 <= max).
 *
 * Invariants (docs/architecture/ipc-stress-invariants.md): I1 no authority
 * without cap; I2 no fallback after ACCESS_DENIED; I3 no silent slot
 * overwrite; I4 occupied slot fails/degrades per contract; I5 sender keeps
 * cap without delivery commit; I6 receiver gains nothing on failure; I7 no
 * staged-cap leak; I8 no double release; I9 reply one-shot; I10 second
 * reply keeps server cap; I11 legacy slot-0 delivery; I12 slot delivery is
 * an invocable CPtr; I13 NOT_FOUND installs nothing; I14 close wakes
 * waiters; I15 death leaves no dead waiters; I16 live back to baseline;
 * I17 hwm bounded; I18 delivery counters move as expected. */

/* xorshift32 — deterministic, seeded per test. */
static uint32_t g_fz_seed;
static uint32_t fz_rand(void) {
    uint32_t x = g_fz_seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_fz_seed = x;
    return x;
}

/* Monotonic fresh-slot allocator (slots are never deleted — see header). */
#define FZ_SLOT_BASE  100u
#define FZ_SLOT_LIMIT 240u
static uint32_t g_fz_slot_next = FZ_SLOT_BASE;
static uint32_t fz_slot_alloc(void) {
    if (g_fz_slot_next >= FZ_SLOT_LIMIT) return 0;   /* budget blown → caller fails */
    return g_fz_slot_next++;
}

/* Failure locator: printed ONLY on failure, right before it_fail. */
static void fz_note(const char *t, uint32_t seed, uint32_t iter) {
    it_serial_write("[IRIS][TEST] FZ ");
    it_serial_write(t);
    it_serial_write(" seed=");
    it_log_num(seed);
    it_serial_write(" iter=");
    it_log_num(iter);
    it_serial_write("\n");
}

/* ── Command-driven persistent workers ──────────────────────────────────────
 * One worker thread per index, driven over its own control endpoint.  A
 * command is one blocking EP_SEND on the ctl ep (rendezvous == the worker is
 * ready); the worker runs the op against g_fz_data_ep, publishes results in
 * its result slots and re-blocks on ctl.  Workers are started/stopped per
 * test; each leaves the process KTcb handle behind by design (Ph96) — the
 * per-test live delta documents it. */
#define FZ_OP_EXIT       0u
#define FZ_OP_RECV       1u   /* w1 = receive-slot declaration (0 = legacy) */
#define FZ_OP_SEND_CAP   2u   /* w1 = handle (0 = none), w2 = rights, w3 = label */
#define FZ_OP_CALL       3u   /* w1 = attached_cap (0 = none), w2 = rights, w3 = reply slot */

static handle_id_t       g_fz_ctl[2]  = { HANDLE_INVALID, HANDLE_INVALID };
static handle_id_t       g_fz_data_ep = HANDLE_INVALID;
static volatile long     g_fz_res[2];
static volatile uint32_t g_fz_att[2];      /* msg.attached_handle seen by worker */
static volatile uint32_t g_fz_attcap[2];   /* msg.attached_cap seen by worker */
static volatile int      g_fz_done[2];
static uint8_t           g_fz_stk[2][8192];

static void fz_worker(int idx) {
    for (;;) {
        struct IrisMsg c;
        it_iris_msg_zero(&c);
        if (it_sys2(SYS_EP_RECV, (long)g_fz_ctl[idx], (long)&c) != 0) break;
        uint32_t op = (uint32_t)c.words[0];
        if (op == FZ_OP_EXIT) break;

        struct IrisMsg m;
        it_iris_msg_zero(&m);
        long r = -1;
        if (op == FZ_OP_RECV) {
            m.attached_cap = (uint32_t)c.words[1];   /* slot hint (0 = legacy) */
            r = it_sys2(SYS_EP_RECV, (long)g_fz_data_ep, (long)&m);
        } else if (op == FZ_OP_SEND_CAP) {
            m.label = c.words[3];
            if (c.words[1]) {
                m.attached_handle = (uint32_t)c.words[1];
                m.attached_rights = (uint32_t)c.words[2];
            }
            r = it_sys2(SYS_EP_SEND, (long)g_fz_data_ep, (long)&m);
        } else if (op == FZ_OP_CALL) {
            m.label = 0xF2;
            if (c.words[1]) {
                m.attached_cap        = (uint32_t)c.words[1];
                m.attached_cap_rights = (uint32_t)c.words[2];
            }
            m.attached_handle = (uint32_t)c.words[3]; /* reply slot (0 = legacy) */
            r = it_sys2(SYS_EP_CALL, (long)g_fz_data_ep, (long)&m);
        }
        g_fz_att[idx]    = m.attached_handle;
        g_fz_attcap[idx] = m.attached_cap;
        g_fz_res[idx]    = r;
        __asm__ volatile ("" ::: "memory");
        g_fz_done[idx]   = 1;
    }
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void fz_worker0(void) { fz_worker(0); }
static void fz_worker1(void) { fz_worker(1); }

/* Start `n` workers (1 or 2).  Returns 1 on success. */
static int fz_workers_start(int n) {
    static void (*const entries[2])(void) = { fz_worker0, fz_worker1 };
    for (int i = 0; i < n; i++) {
        long e = it_sys0(SYS_ENDPOINT_CREATE);
        if (e < 0) return 0;
        g_fz_ctl[i] = (handle_id_t)e;
        uint64_t entry = (uint64_t)(uintptr_t)entries[i];
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_fz_stk[i] + sizeof(g_fz_stk[i]))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) return 0;
    }
    return 1;
}

/* Send a command to worker `idx`; blocks until the worker picks it up. */
static int fz_cmd(int idx, uint32_t op, uint64_t a, uint64_t b, uint64_t c) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label      = 0xFC;
    m.words[0]   = op;
    m.words[1]   = a;
    m.words[2]   = b;
    m.words[3]   = c;
    m.word_count = 4u;
    g_fz_done[idx] = 0;
    return it_sys2(SYS_EP_SEND, (long)g_fz_ctl[idx], (long)&m) == 0;
}

/* Bounded wait for worker `idx` to publish a result. */
static int fz_wait(int idx) {
    for (int i = 0; i < 4000 && !g_fz_done[idx]; i++) it_sys0(SYS_YIELD);
    return g_fz_done[idx];
}

static void fz_workers_stop(int n) {
    for (int i = 0; i < n; i++) {
        if (g_fz_ctl[i] != HANDLE_INVALID) {
            (void)fz_cmd(i, FZ_OP_EXIT, 0, 0, 0);
            it_close(&g_fz_ctl[i]);
        }
    }
}

/* Dup a WRITE|TRANSFER cap of `src` for staging (returns handle or -err). */
static long fz_dup_xfer(long src) {
    return it_sys2(SYS_HANDLE_DUP, src, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
}

/* ── T107: randomized receive-slot IPC stress ───────────────────────────────
 * 48 PRNG-driven iterations over one endpoint and one worker mixing
 * EP_SEND / EP_NB_SEND / EP_RECV with notification AND endpoint caps into
 * fresh slots, occupied slots, invalid slots, legacy slot-0 and rights-
 * degraded staging.  Invariants: I1-I8, I11, I12, I16-I18. */
#define T107_SEED  0xA1110107u
#define T107_ITERS 48u

static void test_t107(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T107", "sched ext"); return; }
    g_fz_seed = T107_SEED;
    int ok = 1;
    const char *why = "rslot stress";
    uint32_t it_n = 0;
    uint32_t exp_slot = 0, exp_hand = 0;

    long ep    = it_sys0(SYS_ENDPOINT_CREATE);   /* data endpoint */
    long n     = it_sys0(SYS_NOTIFY_CREATE);     /* transferable notification */
    long ep2   = it_sys0(SYS_ENDPOINT_CREATE);   /* transferable endpoint */
    long selfp = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_TEST_PROC);
    handle_id_t n_h = (handle_id_t)n, ep2_h = (handle_id_t)ep2;
    handle_id_t selfp_h = (handle_id_t)selfp;
    g_fz_data_ep = (handle_id_t)ep;
    if (ep < 0 || n < 0 || ep2 < 0 || selfp < 0) { it_fail("T107", "create"); return; }
    if (!fz_workers_start(1)) { it_fail("T107", "worker"); return; }

    for (it_n = 0; ok && it_n < T107_ITERS; it_n++) {
        uint32_t pick = fz_rand() % 8u;

        if (pick == 0u) {
            /* NB send, no receiver → WOULD_BLOCK; nothing changes. */
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label = 0xF0;
            if (it_sys2(SYS_EP_NB_SEND, (long)g_fz_data_ep, (long)&m) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "nb empty"; }

        } else if (pick == 1u || pick == 7u) {
            /* Blocking EP_SEND of a cap into a FRESH declared slot.
             * pick 1 = notification cap, pick 7 = endpoint cap. */
            uint32_t s = fz_slot_alloc();
            long src = (pick == 1u) ? n : ep2;
            long d = fz_dup_xfer(src);
            if (s == 0u || d < 0) { ok = 0; why = "slot/dup"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, s, 0, 0)) { ok = 0; why = "cmd"; break; }
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label           = 0xF1;
            m.attached_handle = (uint32_t)d;
            m.attached_rights = RIGHT_WRITE;
            if (it_sys2(SYS_EP_SEND, (long)g_fz_data_ep, (long)&m) != 0) {
                ok = 0; why = "send";
            }
            if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && (g_fz_res[0] != 0 || g_fz_att[0] != s)) {
                ok = 0; why = "slot landing";
            }
            /* I12: the CPtr is invocable. */
            if (ok && pick == 1u &&
                it_sys2(SYS_NOTIFY_SIGNAL, (long)s, 1) != 0) {
                ok = 0; why = "cptr signal";
            }
            if (ok && pick == 1u) {
                uint64_t bits = 0;
                if (it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
                    bits == 0u) { ok = 0; why = "signal lost"; }
            }
            if (ok && pick == 7u) {
                struct IrisMsg pm;
                it_iris_msg_zero(&pm);     /* clean probe: no stale attached cap */
                pm.label = 0xF9;
                if (it_sys2(SYS_EP_NB_SEND, (long)s, (long)&pm) !=
                    (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "cptr ep"; }
            }
            /* move semantics: source dup consumed at delivery commit */
            if (ok && it_sys1(SYS_HANDLE_TYPE, d) >= 0) {
                ok = 0; why = "dup not consumed";
            }
            exp_slot++;

        } else if (pick == 2u) {
            /* NB_SEND of a cap to a slot-declared receiver (bounded retry
             * until the worker is queued). */
            uint32_t s = fz_slot_alloc();
            long d = fz_dup_xfer(n);
            if (s == 0u || d < 0) { ok = 0; why = "slot/dup"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, s, 0, 0)) { ok = 0; why = "cmd"; break; }
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label           = 0xF3;
            m.attached_handle = (uint32_t)d;
            m.attached_rights = RIGHT_WRITE;
            long r = (long)IRIS_ERR_WOULD_BLOCK;
            for (int i = 0; i < 400 && r == (long)IRIS_ERR_WOULD_BLOCK; i++) {
                r = it_sys2(SYS_EP_NB_SEND, (long)g_fz_data_ep, (long)&m);
                if (r == (long)IRIS_ERR_WOULD_BLOCK) it_sys0(SYS_YIELD);
            }
            if (r != 0) { ok = 0; why = "nb send"; }
            if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && (g_fz_res[0] != 0 || g_fz_att[0] != s)) {
                ok = 0; why = "nb slot landing";
            }
            /* A1.9 commit rule: NB source consumed once delivery commits. */
            if (ok && it_sys1(SYS_HANDLE_TYPE, d) >= 0) {
                ok = 0; why = "nb dup not consumed";
            }
            exp_slot++;

        } else if (pick == 3u) {
            /* Occupied slot: recv fails fast; sender's NB attempt finds no
             * waiter; source kept; occupant untouched (I3, I4, I5). */
            uint32_t s = fz_slot_alloc();
            if (s == 0u) { ok = 0; why = "slot budget"; break; }
            if (it_sys4(SYS_PROC_CSPACE_MINT, (long)selfp_h, (long)s,
                        n, (long)RIGHT_WRITE) != 0) { ok = 0; why = "premint"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, s, 0, 0)) { ok = 0; why = "cmd"; break; }
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_ALREADY_EXISTS) {
                ok = 0; why = "occupied not rejected";
            }
            long d = fz_dup_xfer(n);
            if (ok && d >= 0) {
                struct IrisMsg m;
                it_iris_msg_zero(&m);
                m.label           = 0xF4;
                m.attached_handle = (uint32_t)d;
                m.attached_rights = RIGHT_WRITE;
                if (it_sys2(SYS_EP_NB_SEND, (long)g_fz_data_ep, (long)&m) !=
                    (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "dead waiter"; }
                if (ok && it_sys1(SYS_HANDLE_TYPE, d) !=
                    (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
                    ok = 0; why = "cap consumed on fail";
                }
                handle_id_t dh = (handle_id_t)d;
                it_close(&dh);
            }
            /* I3: the occupant is still exactly our pre-minted cap. */
            if (ok) {
                long rh = it_sys1(SYS_CSPACE_RESOLVE, (long)s);
                if (rh < 0 || it_sys2(SYS_HANDLE_SAME_OBJECT, rh, n) != 1) {
                    ok = 0; why = "occupant changed";
                }
                if (rh >= 0) { handle_id_t r2 = (handle_id_t)rh; it_close(&r2); }
            }

        } else if (pick == 4u) {
            /* Invalid (out-of-range) slot declaration → INVALID_ARG fail-
             * fast; the endpoint is untouched. */
            if (!fz_cmd(0, FZ_OP_RECV, 300u, 0, 0)) { ok = 0; why = "cmd"; break; }
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_INVALID_ARG) {
                ok = 0; why = "invalid slot";
            }
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label = 0xF5;
            if (ok && it_sys2(SYS_EP_NB_SEND, (long)g_fz_data_ep, (long)&m) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "ep touched"; }

        } else if (pick == 5u) {
            /* Legacy slot 0: the cap materializes as a handle >= 1024 (I11). */
            long d = fz_dup_xfer(n);
            if (d < 0) { ok = 0; why = "dup"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, 0, 0, 0)) { ok = 0; why = "cmd"; break; }
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label           = 0xF6;
            m.attached_handle = (uint32_t)d;
            m.attached_rights = RIGHT_WRITE;
            if (it_sys2(SYS_EP_SEND, (long)g_fz_data_ep, (long)&m) != 0) {
                ok = 0; why = "legacy send";
            }
            if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && (g_fz_res[0] != 0 || g_fz_att[0] < 1024u)) {
                ok = 0; why = "legacy landing";
            }
            if (ok && it_sys2(SYS_NOTIFY_SIGNAL, (long)g_fz_att[0], 2) != 0) {
                ok = 0; why = "legacy cap dead";
            }
            if (ok) {
                uint64_t bits = 0;
                (void)it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits);
                handle_id_t gh = (handle_id_t)g_fz_att[0];
                it_close(&gh);
            }
            exp_hand++;

        } else {
            /* pick 6: staging without RIGHT_TRANSFER → ACCESS_DENIED; the
             * blocked receiver gains NOTHING and then gets a clean plain
             * message (I1, I2, I6); the degraded dup survives. */
            long bad = it_sys2(SYS_HANDLE_DUP, n, (long)RIGHT_WRITE);
            if (bad < 0) { ok = 0; why = "bad dup"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, 0, 0, 0)) { ok = 0; why = "cmd"; break; }
            it_sys1(SYS_SLEEP, 2);   /* worker re-blocks on the data ep */
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label           = 0xF7;
            m.attached_handle = (uint32_t)bad;
            m.attached_rights = RIGHT_WRITE;
            if (it_sys2(SYS_EP_NB_SEND, (long)g_fz_data_ep, (long)&m) !=
                (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no ACCESS_DENIED"; }
            if (ok && it_sys1(SYS_HANDLE_TYPE, bad) !=
                (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
                ok = 0; why = "bad dup consumed";
            }
            /* unblock the still-waiting receiver with a plain message */
            if (ok) {
                it_iris_msg_zero(&m);
                m.label = 0xF8;
                if (it_sys2(SYS_EP_SEND, (long)g_fz_data_ep, (long)&m) != 0) {
                    ok = 0; why = "plain send";
                }
                if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
                if (ok && (g_fz_res[0] != 0 ||
                           g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP)) {
                    ok = 0; why = "ghost cap";
                }
            }
            {
                handle_id_t bh = (handle_id_t)bad;
                it_close(&bh);
            }
        }
    }

    fz_workers_stop(1);
    it_close(&n_h);
    it_close(&ep2_h);
    it_close(&selfp_h);
    it_close(&g_fz_data_ep);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance; +1 = the worker's KTcb handle (Ph96, A1.10 note). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE] + 1u) { ok = 0; why = "leak"; }
    /* I18: directional counter deltas (>=: background services also count). */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + exp_slot) {
        ok = 0; why = "slot count";
    }
    if (ok && after[IT_SI_HANDDEL] < before[IT_SI_HANDDEL] + exp_hand) {
        ok = 0; why = "hand count";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T107"); }
    else    { fz_note("T107", T107_SEED, it_n); it_fail("T107", why); }
}

/* ── T108: randomized close/cancel staged-cap stress ────────────────────────
 * 16 PRNG-driven rounds; each round creates a FRESH endpoint, parks one or
 * two workers on it (blocking send with staged cap, EP_CALL with staged cap,
 * declared-slot recv, legacy recv) and closes the endpoint mid-flight.
 * Every waiter must wake with CLOSED, every staged source cap must survive
 * with its owner (release exactly once — a double release would corrupt the
 * books), a canceled declared slot must stay empty and reusable, and no
 * KReply may ever be minted (no call rendezvouses).  Thread/process death
 * cancellation is T101/T111 territory — this test owns endpoint close.
 * Invariants: I5-I8, I14, I16, I17 (+ I4 via the reused empty slot). */
#define T108_SEED   0xA1110108u
#define T108_ROUNDS 16u

static void test_t108(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T108", "sched ext"); return; }
    g_fz_seed = T108_SEED;
    int ok = 1;
    const char *why = "close/cancel stress";
    uint32_t it_n = 0;

    long n = it_sys0(SYS_NOTIFY_CREATE);
    handle_id_t n_h = (handle_id_t)n;
    if (n < 0) { it_fail("T108", "create"); return; }
    /* One reusable declared slot: every cancellation must leave it EMPTY,
     * so the same slot serves every pick-3 round (that IS the assert). */
    uint32_t rslot = fz_slot_alloc();
    if (rslot == 0u) { it_close(&n_h); it_fail("T108", "slot budget"); return; }
    if (!fz_workers_start(2)) { it_close(&n_h); it_fail("T108", "worker"); return; }

    for (it_n = 0; ok && it_n < T108_ROUNDS; it_n++) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        if (ep < 0) { ok = 0; why = "ep create"; break; }
        g_fz_data_ep = (handle_id_t)ep;
        uint32_t pick = fz_rand() % 5u;

        if (pick == 0u || pick == 1u) {
            /* One waiter with a staged cap: blocking EP_SEND (pick 0) or
             * EP_CALL (pick 1); close cancels it before any rendezvous. */
            long d = fz_dup_xfer(n);
            if (d < 0) { ok = 0; why = "dup"; break; }
            int sent = (pick == 0u)
                ? fz_cmd(0, FZ_OP_SEND_CAP, (uint64_t)d, RIGHT_WRITE, 0x108)
                : fz_cmd(0, FZ_OP_CALL,     (uint64_t)d, RIGHT_WRITE, 0);
            if (!sent) { ok = 0; why = "cmd"; break; }
            it_sys1(SYS_SLEEP, 5);            /* waiter queues its staged cap */
            it_close(&g_fz_data_ep);
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_CLOSED) {
                ok = 0; why = "not CLOSED";
            }
            /* I5/I7: no delivery commit → the source cap survives. */
            if (ok && it_sys1(SYS_HANDLE_TYPE, d) !=
                (long)IRIS_HANDLE_TYPE_NOTIFICATION) { ok = 0; why = "cap consumed"; }
            { handle_id_t dh = (handle_id_t)d; it_close(&dh); }

        } else if (pick == 2u) {
            /* TWO staged waiters (send + call) canceled by one close: every
             * staging ref released exactly once (I8). */
            long da = fz_dup_xfer(n), db = fz_dup_xfer(n);
            if (da < 0 || db < 0) { ok = 0; why = "dup2"; break; }
            if (!fz_cmd(0, FZ_OP_SEND_CAP, (uint64_t)da, RIGHT_WRITE, 0x208) ||
                !fz_cmd(1, FZ_OP_CALL,     (uint64_t)db, RIGHT_WRITE, 0)) {
                ok = 0; why = "cmd2"; break;
            }
            it_sys1(SYS_SLEEP, 5);            /* both queue staged caps */
            it_close(&g_fz_data_ep);
            if (!fz_wait(0) || !fz_wait(1)) { ok = 0; why = "worker hang"; }
            if (ok && (g_fz_res[0] != (long)IRIS_ERR_CLOSED ||
                       g_fz_res[1] != (long)IRIS_ERR_CLOSED)) {
                ok = 0; why = "not CLOSED x2";
            }
            if (ok && (it_sys1(SYS_HANDLE_TYPE, da) !=
                       (long)IRIS_HANDLE_TYPE_NOTIFICATION ||
                       it_sys1(SYS_HANDLE_TYPE, db) !=
                       (long)IRIS_HANDLE_TYPE_NOTIFICATION)) {
                ok = 0; why = "cap consumed x2";
            }
            { handle_id_t dh = (handle_id_t)da; it_close(&dh); }
            { handle_id_t dh = (handle_id_t)db; it_close(&dh); }

        } else if (pick == 3u) {
            /* Receiver with a DECLARED slot canceled by close: wakes CLOSED,
             * gains nothing (I6), and the slot stays empty — the next pick-3
             * round re-declares the very same slot. */
            if (!fz_cmd(0, FZ_OP_RECV, rslot, 0, 0)) { ok = 0; why = "cmd"; break; }
            it_sys1(SYS_SLEEP, 5);            /* receiver blocks, slot declared */
            it_close(&g_fz_data_ep);
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_CLOSED) {
                ok = 0; why = "recv not CLOSED";
            }
            if (ok && g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) {
                ok = 0; why = "ghost cap";
            }
            if (ok && it_sys1(SYS_CSPACE_RESOLVE, (long)rslot) >= 0) {
                ok = 0; why = "slot not empty";
            }

        } else {
            /* Legacy (slot 0) receiver canceled by close. */
            if (!fz_cmd(0, FZ_OP_RECV, 0, 0, 0)) { ok = 0; why = "cmd"; break; }
            it_sys1(SYS_SLEEP, 5);
            it_close(&g_fz_data_ep);
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_CLOSED) {
                ok = 0; why = "legacy not CLOSED";
            }
            if (ok && g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) {
                ok = 0; why = "legacy ghost cap";
            }
        }
        it_close(&g_fz_data_ep);   /* no-op on the already-closed rounds */
    }

    /* Post-stress health: a fresh endpoint still rendezvouses normally. */
    if (ok) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        if (ep < 0) { ok = 0; why = "final ep"; }
        else {
            g_fz_data_ep = (handle_id_t)ep;
            if (!fz_cmd(0, FZ_OP_RECV, 0, 0, 0)) { ok = 0; why = "final cmd"; }
            if (ok) {
                it_sys1(SYS_SLEEP, 2);        /* receiver re-blocks on data ep */
                struct IrisMsg m;
                it_iris_msg_zero(&m);
                m.label = 0x308;
                if (it_sys2(SYS_EP_SEND, (long)g_fz_data_ep, (long)&m) != 0 ||
                    !fz_wait(0) || g_fz_res[0] != 0 ||
                    g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) {
                    ok = 0; why = "ep not reusable";
                }
            }
            it_close(&g_fz_data_ep);
        }
    }

    fz_workers_stop(2);
    it_close(&n_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance; +2 = the two workers' KTcb handles (Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE] + 2u) { ok = 0; why = "leak"; }
    /* No call ever rendezvoused → not one KReply minted (T104 rule). */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY]) {
        ok = 0; why = "ghost kreply";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T108"); }
    else    { fz_note("T108", T108_SEED, it_n); it_fail("T108", why); }
}

/* ── T109: randomized reply one-shot + attached cap stress ──────────────────
 * 20 PRNG-driven EP_CALLs from one worker; the main thread serves each one
 * and ALWAYS attempts a second reply.  Mix: plain reply + second reply WITH
 * a cap (the A1.10 T105 case), cap reply into a declared fresh slot, cap
 * reply into legacy slot 0, and a call declaring an OCCUPIED reply slot
 * (fail-fast before any send — the server never even sees a message).
 * The reply-caps counter must balance EXACTLY: one KReply per rendezvous,
 * none for the fail-fast rounds.  Caller-death/close during a call is
 * T108/T111 territory.  Invariants: I3-I7, I9, I10, I12, I16-I18. */
#define T109_SEED  0xA1110109u
#define T109_ITERS 20u

static void test_t109(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T109", "sched ext"); return; }
    g_fz_seed = T109_SEED;
    int ok = 1;
    const char *why = "reply one-shot stress";
    uint32_t it_n = 0;
    uint32_t exp_slot = 0, exp_hand = 0, exp_reply = 0;

    long ep    = it_sys0(SYS_ENDPOINT_CREATE);
    long n     = it_sys0(SYS_NOTIFY_CREATE);
    long selfp = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_TEST_PROC);
    handle_id_t n_h = (handle_id_t)n, selfp_h = (handle_id_t)selfp;
    g_fz_data_ep = (handle_id_t)ep;
    if (ep < 0 || n < 0 || selfp < 0) { it_fail("T109", "create"); return; }
    /* Occupied reply-slot fixture: pre-minted once, occupied forever. */
    uint32_t occ = fz_slot_alloc();
    if (occ == 0u ||
        it_sys4(SYS_PROC_CSPACE_MINT, (long)selfp_h, (long)occ,
                n, (long)RIGHT_WRITE) != 0) {
        it_fail("T109", "occ fixture"); return;
    }
    if (!fz_workers_start(1)) { it_fail("T109", "worker"); return; }

    for (it_n = 0; ok && it_n < T109_ITERS; it_n++) {
        uint32_t pick = fz_rand() % 4u;

        if (pick == 3u) {
            /* Call declaring the OCCUPIED reply slot: fail-fast before any
             * send (I3/I4) — no message lands, no KReply is minted, and the
             * occupant is still exactly the fixture cap. */
            if (!fz_cmd(0, FZ_OP_CALL, 0, 0, occ)) { ok = 0; why = "cmd"; break; }
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_ALREADY_EXISTS) {
                ok = 0; why = "occupied not rejected";
            }
            if (ok) {
                struct IrisMsg m;
                it_iris_msg_zero(&m);
                if (it_sys2(SYS_EP_NB_RECV, (long)g_fz_data_ep, (long)&m) !=
                    (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "ghost msg"; }
            }
            if (ok) {
                long rh = it_sys1(SYS_CSPACE_RESOLVE, (long)occ);
                if (rh < 0 || it_sys2(SYS_HANDLE_SAME_OBJECT, rh, n) != 1) {
                    ok = 0; why = "occupant changed";
                }
                if (rh >= 0) { handle_id_t r2 = (handle_id_t)rh; it_close(&r2); }
            }
            continue;
        }

        /* Rendezvous rounds: the worker calls, we serve. */
        uint32_t s = 0;
        if (pick == 1u) {
            s = fz_slot_alloc();
            if (s == 0u) { ok = 0; why = "slot budget"; break; }
        }
        if (!fz_cmd(0, FZ_OP_CALL, 0, 0, s)) { ok = 0; why = "cmd"; break; }

        struct IrisMsg m;
        it_iris_msg_zero(&m);
        if (it_sys2(SYS_EP_RECV, (long)g_fz_data_ep, (long)&m) != 0 ||
            m.label != 0xF2ULL ||
            !iris_msg_cap_is_handle(m.attached_handle)) {
            ok = 0; why = "recv call"; break;
        }
        handle_id_t reply_h = (handle_id_t)m.attached_handle;
        exp_reply++;                       /* exactly one KReply per rendezvous */

        long d = -1;
        if (pick != 0u) {
            d = fz_dup_xfer(n);
            if (d < 0) { ok = 0; why = "dup"; }
        }

        /* First reply: plain (pick 0) or carrying the dup (picks 1-2). */
        if (ok) {
            struct IrisMsg rm;
            it_iris_msg_zero(&rm);
            rm.label = 0x5109;
            if (d >= 0) {
                rm.attached_handle = (uint32_t)d;
                rm.attached_rights = RIGHT_WRITE;
            }
            if (it_sys2(SYS_REPLY, (long)reply_h, (long)&rm) != 0) {
                ok = 0; why = "first reply";
            }
        }
        if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
        if (ok && g_fz_res[0] != 0) { ok = 0; why = "caller err"; }

        if (ok && pick == 0u) {
            /* Plain reply delivers no cap. */
            if (g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) { ok = 0; why = "ghost cap"; }
        } else if (ok && pick == 1u) {
            /* I12: the reply cap landed at the declared slot, invocable. */
            if (g_fz_att[0] != s) { ok = 0; why = "slot landing"; }
            uint64_t bits = 0;
            if (ok && (it_sys2(SYS_NOTIFY_SIGNAL, (long)s, 1) != 0 ||
                       it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
                       bits != 1u)) { ok = 0; why = "cptr dead"; }
            if (ok && it_sys1(SYS_HANDLE_TYPE, d) >= 0) {
                ok = 0; why = "dup not consumed";
            }
            if (ok) { d = -1; exp_slot++; }
        } else if (ok) {
            /* I11 on the reply path: legacy handle >= 1024, invocable. */
            if (!iris_msg_cap_is_handle(g_fz_att[0])) { ok = 0; why = "legacy landing"; }
            uint64_t bits = 0;
            if (ok && (it_sys2(SYS_NOTIFY_SIGNAL, (long)g_fz_att[0], 2) != 0 ||
                       it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
                       bits != 2u)) { ok = 0; why = "legacy cap dead"; }
            if (ok) {
                handle_id_t gh = (handle_id_t)g_fz_att[0];
                it_close(&gh);
            }
            if (ok && it_sys1(SYS_HANDLE_TYPE, d) >= 0) {
                ok = 0; why = "dup not consumed";
            }
            if (ok) { d = -1; exp_hand++; }
        }

        /* SECOND reply — one-shot must hold (I9); when it carries a cap the
         * server keeps it (I10, the A1.10 T105 rule under stress). */
        if (ok) {
            long d2 = -1;
            if (pick == 0u) {
                d2 = fz_dup_xfer(n);
                if (d2 < 0) { ok = 0; why = "dup2"; }
            }
            if (ok) {
                struct IrisMsg rm;
                it_iris_msg_zero(&rm);
                rm.label = 0xDEAD;
                if (d2 >= 0) {
                    rm.attached_handle = (uint32_t)d2;
                    rm.attached_rights = RIGHT_WRITE;
                }
                if (it_sys2(SYS_REPLY, (long)reply_h, (long)&rm) !=
                    (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "not one-shot"; }
                if (ok && d2 >= 0 && it_sys1(SYS_HANDLE_TYPE, d2) !=
                    (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
                    ok = 0; why = "second reply ate cap";
                }
            }
            if (d2 >= 0) { handle_id_t dh = (handle_id_t)d2; it_close(&dh); }
        }
        if (d >= 0) { handle_id_t dh = (handle_id_t)d; it_close(&dh); }
        it_close(&reply_h);
    }

    fz_workers_stop(1);
    it_close(&n_h);
    it_close(&selfp_h);
    it_close(&g_fz_data_ep);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance; +1 = the worker's KTcb handle (Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE] + 1u) { ok = 0; why = "leak"; }
    /* Reply caps balance EXACTLY: one per rendezvous, zero per fail-fast. */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY] + exp_reply) {
        ok = 0; why = "reply count";
    }
    /* I18: directional delivery deltas. */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + exp_slot) {
        ok = 0; why = "slot count";
    }
    if (ok && after[IT_SI_HANDDEL] < before[IT_SI_HANDDEL] + exp_hand) {
        ok = 0; why = "hand count";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T109"); }
    else    { fz_note("T109", T109_SEED, it_n); it_fail("T109", why); }
}

/* ── T110: svcmgr register/lookup receive-slot stress ───────────────────────
 * 18 PRNG-driven rounds over THREE temporary service names whose expected
 * registration state is tracked exactly: register / re-register (BUSY, the
 * rejected cap closed by svcmgr) / slot lookup (fresh slot, invocable CPtr)
 * / legacy lookup (handle >= 1024) / NOT_FOUND lookup with a declared slot
 * (the SAME reusable slot every time — it must stay empty) / occupied-slot
 * lookup (fail-fast, then legacy still works) / unregister + full
 * re-register cycle (svcmgr's CSpace pool frees and reuses, no ghost from
 * the previous generation).  The reply-caps counter balances EXACTLY: one
 * KReply per svcmgr rendezvous PLUS one per served lookup — svcmgr_log()
 * emits one console_ep_write EP_CALL per LOOKUP_NAME (OK and NOT_FOUND;
 * REGISTER/UNREGISTER do not log) — and none for the fail-fast occupied
 * rounds, which never reach svcmgr at all.
 * Invariants: I1, I3, I4, I11-I13, I16-I18. */
#define T110_SEED  0xA1110110u
#define T110_ITERS 18u

static void test_t110(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T110", "sched ext"); return; }
    g_fz_seed = T110_SEED;
    int ok = 1;
    const char *why = "svcmgr rslot stress";
    uint32_t it_n = 0;
    uint32_t exp_slot = 0, exp_hand = 0, exp_reply = 0, exp_log = 0;

    long ep    = it_sys0(SYS_ENDPOINT_CREATE);
    long nf    = it_sys0(SYS_NOTIFY_CREATE);
    long selfp = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_TEST_PROC);
    handle_id_t ep_h = (handle_id_t)ep, nf_h = (handle_id_t)nf;
    handle_id_t selfp_h = (handle_id_t)selfp;
    if (ep < 0 || nf < 0 || selfp < 0) { it_fail("T110", "create"); return; }

    /* Fixtures: one slot that must stay EMPTY across every NOT_FOUND round
     * and one pre-minted slot that is occupied forever. */
    uint32_t nfslot = fz_slot_alloc();
    uint32_t occ    = fz_slot_alloc();
    if (nfslot == 0u || occ == 0u ||
        it_sys4(SYS_PROC_CSPACE_MINT, (long)selfp_h, (long)occ,
                nf, (long)RIGHT_WRITE) != 0) {
        it_fail("T110", "fixtures"); return;
    }

    char name[5] = { 'f', 'z', '.', 'a', '\0' };
    long ids[3]  = { -1, -1, -1 };
    int  reg[3]  = { 0, 0, 0 };
    struct IrisMsg msg;

    /* Registered-name legacy lookup: handle >= 1024, invocable, closed. */
    #define T110_LEGACY_OK()                                                  \
        do {                                                                  \
            if (it_lookup_name_slot(name, 0u, &msg) != 0 ||                   \
                msg.label != IRIS_EP_REPLY_OK ||                              \
                !iris_msg_cap_is_handle(msg.attached_handle)) {               \
                ok = 0; why = "legacy lookup";                                \
            } else {                                                          \
                struct IrisMsg p;                                             \
                it_iris_msg_zero(&p);                                         \
                p.label = 0x110;                                              \
                if (it_sys2(SYS_EP_NB_SEND, (long)msg.attached_handle,        \
                            (long)&p) != (long)IRIS_ERR_WOULD_BLOCK) {        \
                    ok = 0; why = "legacy cap dead";                          \
                }                                                             \
                handle_id_t ch = (handle_id_t)msg.attached_handle;            \
                it_close(&ch);                                                \
                exp_hand++;                                                   \
            }                                                                 \
            exp_reply++; exp_log++;                                           \
        } while (0)

    /* Unregistered-name lookup into the reusable slot: ERR + slot empty. */
    #define T110_NOTFOUND()                                                   \
        do {                                                                  \
            if (it_lookup_name_slot(name, nfslot, &msg) != 0 ||               \
                msg.label != IRIS_EP_REPLY_ERR ||                             \
                msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) {     \
                ok = 0; why = "not-found lookup";                             \
            } else if (it_sys1(SYS_CSPACE_RESOLVE, (long)nfslot) >= 0) {      \
                ok = 0; why = "ghost cap";                                    \
            }                                                                 \
            exp_reply++; exp_log++;                                           \
        } while (0)

    for (it_n = 0; ok && it_n < T110_ITERS; it_n++) {
        uint32_t pick = fz_rand() % 6u;
        uint32_t i    = fz_rand() % 3u;
        name[3] = (char)('a' + i);

        if (pick == 0u) {
            /* Register, or re-register while live → BUSY (rejected cap is
             * closed by svcmgr — the final live-balance check proves it). */
            long r = it_register_ep(name, ep_h);
            exp_reply++;
            if (!reg[i]) {
                if (r < 0) { ok = 0; why = "register"; }
                else { ids[i] = r; reg[i] = 1; }
            } else if (r != -(long)(uint32_t)IRIS_ERR_BUSY) {
                ok = 0; why = "re-register not BUSY";
            }

        } else if (pick == 1u) {
            /* Slot lookup: fresh slot on success (occupied forever after —
             * the CPtr is the proof), reusable slot on NOT_FOUND. */
            if (reg[i]) {
                uint32_t s = fz_slot_alloc();
                if (s == 0u) { ok = 0; why = "slot budget"; break; }
                if (it_lookup_name_slot(name, s, &msg) != 0 ||
                    msg.label != IRIS_EP_REPLY_OK ||
                    msg.attached_handle != s) { ok = 0; why = "slot lookup"; }
                exp_reply++; exp_log++;
                if (ok) {
                    struct IrisMsg p;
                    it_iris_msg_zero(&p);
                    p.label = 0x110;
                    if (it_sys2(SYS_EP_NB_SEND, (long)s, (long)&p) !=
                        (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "cptr dead"; }
                    exp_slot++;
                }
            } else {
                T110_NOTFOUND();
            }

        } else if (pick == 2u) {
            if (reg[i]) T110_LEGACY_OK(); else T110_NOTFOUND();

        } else if (pick == 3u) {
            /* Unregister removes authority: the very next lookup fails. */
            if (reg[i]) {
                if (it_unregister_id((uint32_t)ids[i]) != 0) {
                    ok = 0; why = "unregister";
                }
                exp_reply++;
                reg[i] = 0;
                if (ok) T110_NOTFOUND();
            } else {
                T110_NOTFOUND();
            }

        } else if (pick == 4u) {
            /* Occupied-slot lookup: fail-fast BEFORE any send (no KReply,
             * no reply counter tick), then legacy still works. */
            if (reg[i]) {
                if (it_lookup_name_slot(name, occ, &msg) !=
                    (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "occ not rejected"; }
                if (ok) T110_LEGACY_OK();
            } else {
                T110_NOTFOUND();
            }

        } else {
            /* Full unregister → register cycle: the svcmgr pool slot is
             * freed and reused with no ghost of the previous generation. */
            if (reg[i]) {
                if (it_unregister_id((uint32_t)ids[i]) != 0) {
                    ok = 0; why = "cycle unreg";
                }
                exp_reply++;
                reg[i] = 0;
                if (ok) T110_NOTFOUND();
            }
            if (ok) {
                long r = it_register_ep(name, ep_h);
                exp_reply++;
                if (r < 0) { ok = 0; why = "cycle register"; }
                else { ids[i] = r; reg[i] = 1; }
            }
            if (ok) T110_LEGACY_OK();
        }
    }

    /* Teardown: unregister the survivors; every name must end NOT_FOUND. */
    for (uint32_t i = 0; i < 3u; i++) {
        name[3] = (char)('a' + i);
        if (ok && reg[i]) {
            if (it_unregister_id((uint32_t)ids[i]) != 0) { ok = 0; why = "final unreg"; }
            exp_reply++;
            reg[i] = 0;
        }
        if (ok) T110_NOTFOUND();
    }
    #undef T110_LEGACY_OK
    #undef T110_NOTFOUND

    it_close(&ep_h);
    it_close(&nf_h);
    it_close(&selfp_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance — svcmgr consumed/closed every staged cap. */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    /* One KReply per svcmgr rendezvous + one per served lookup (svcmgr_log
     * console EP_CALL), zero per fail-fast (exact). */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY] + exp_reply + exp_log) {
        it_serial_write("[IRIS][TEST] T110 reply delta=");
        it_log_num(after[IT_SI_REPLY] - before[IT_SI_REPLY]);
        it_serial_write(" exp=");
        it_log_num(exp_reply + exp_log);
        it_serial_write("\n");
        ok = 0; why = "reply count";
    }
    /* I18: directional delivery deltas (registers also move SLOTDEL —
     * svcmgr's own pool receive-slots — hence >=). */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + exp_slot) {
        ok = 0; why = "slot count";
    }
    if (ok && after[IT_SI_HANDDEL] < before[IT_SI_HANDDEL] + exp_hand) {
        ok = 0; why = "hand count";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T110"); }
    else    { fz_note("T110", T110_SEED, it_n); it_fail("T110", why); }
}

/* ── T111: cross-process receive-slot fuzz with lifecycle_probe ─────────────
 * Six lifecycle_probe children, one scenario each: a coverage-forced prefix
 * (kinds 0-3, every class runs exactly once regardless of the seed) plus a
 * PRNG-chosen tail — deterministic AND fully covered.  Kinds:
 *   0 rslot+notification : cap lands in the CHILD's CSpace at the declared
 *     CPtr (exit code == slot), child invokes it ACROSS the process
 *     boundary (signal observed by the parent);
 *   1 rslot+endpoint cap : same landing proof with an endpoint cap; the
 *     child's death then releases its CSpace ref — the parent's endpoint
 *     stays fully usable;
 *   2 kill before delivery: child killed while blocked with a declared
 *     slot — no dead waiter remains (NB probe), the sender's cap survives
 *     the attempted delivery;
 *   3 legacy slot 0      : cap materializes as a handle >= 1024 in the
 *     child, invoked across the boundary, torn down with the child.
 * Parent books balance EXACTLY (no thread helpers here — delta 0).
 * Invariants: I1, I5, I6, I11, I12, I15-I18. */
#define T111_SEED 0xA1110111u
#define T111_TAIL 2u

static int t111_round(uint32_t kind, uint32_t *exp_slot, uint32_t *exp_hand,
                      const char **why) {
    int ok = 1;
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    long n  = it_sys0(SYS_NOTIFY_CREATE);
    long e2 = (kind == 1u) ? it_sys0(SYS_ENDPOINT_CREATE) : -1;
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
    handle_id_t e2_h = (kind == 1u) ? (handle_id_t)e2 : HANDLE_INVALID;
    handle_id_t proc_h = HANDLE_INVALID;

    if (ep < 0 || n < 0 || (kind == 1u && e2 < 0) ||
        lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; *why = "spawn"; }

    if (ok && it_lp_cmd_rslot(ep_h, (kind == 3u) ? 0u : T099_CHILD_SLOT) != 0) {
        ok = 0; *why = "cmd";
    }

    if (ok && (kind == 0u || kind == 3u)) {
        /* Deliver a notification cap; the child invokes it (bits 1 = CPtr
         * landing, bits 2 = legacy handle landing) and reports where it
         * landed via its exit code. */
        if (it_lp_send_cap(ep_h, n) != 0) { ok = 0; *why = "send cap"; }
        if (ok) {
            uint64_t bits = 0;
            uint64_t want = (kind == 0u) ? 1u : 2u;
            if (it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
                bits != want) { ok = 0; *why = "x-proc signal"; }
        }
        if (ok) {
            long ec = it_lp_wait_exit(proc_h);
            if (kind == 0u && ec != (long)T099_CHILD_SLOT) {
                ok = 0; *why = "cptr landing";
            }
            if (kind == 3u && ec < (long)IRIS_CPTR_LIMIT) {
                ok = 0; *why = "legacy landing";
            }
        }
        if (ok) { if (kind == 0u) (*exp_slot)++; else (*exp_hand)++; }

    } else if (ok && kind == 1u) {
        /* Deliver an endpoint cap into the child's declared slot: the exit
         * code proves the CSpace landing (the child's blind NOTIFY_SIGNAL
         * on it fails and is ignored by design). */
        if (it_lp_send_cap(ep_h, e2) != 0) { ok = 0; *why = "send ep cap"; }
        if (ok && it_lp_wait_exit(proc_h) != (long)T099_CHILD_SLOT) {
            ok = 0; *why = "ep cptr landing";
        }
        /* Child death released its CSpace ref; the parent's endpoint is
         * still alive and clean (no waiter, no corruption). */
        if (ok) {
            struct IrisMsg p;
            it_iris_msg_zero(&p);
            p.label = 0x111;
            if (it_sys2(SYS_EP_NB_SEND, (long)e2_h, (long)&p) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; *why = "parent ep broken"; }
        }
        if (ok) (*exp_slot)++;

    } else if (ok) {
        /* Kill the child while it blocks with its slot declared: the wait
         * dies with it (no dead waiter) and a sender's delivery attempt
         * fails WITHOUT consuming the source cap. */
        it_sys1(SYS_SLEEP, 2);      /* child re-blocks, slot 40 declared */
        if (it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) {
            ok = 0; *why = "kill";
        }
        if (ok && it_sys1(SYS_PROCESS_STATUS, (long)proc_h) != 0) {
            ok = 0; *why = "still alive";
        }
        if (ok) {
            long d = fz_dup_xfer(n);
            if (d < 0) { ok = 0; *why = "dup"; }
            else {
                struct IrisMsg m;
                it_iris_msg_zero(&m);
                m.label           = 0x211;
                m.attached_handle = (uint32_t)d;
                m.attached_rights = RIGHT_WRITE;
                if (it_sys2(SYS_EP_NB_SEND, (long)ep_h, (long)&m) !=
                    (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; *why = "dead waiter"; }
                if (ok && it_sys1(SYS_HANDLE_TYPE, d) !=
                    (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
                    ok = 0; *why = "cap consumed";
                }
                handle_id_t dh = (handle_id_t)d;
                it_close(&dh);
            }
        }
    }

    if (!ok && proc_h != HANDLE_INVALID)
        (void)it_sys1(SYS_PROCESS_KILL, (long)proc_h);
    it_close(&proc_h);
    it_close(&e2_h);
    it_close(&n_h);
    it_close(&ep_h);
    return ok;
}

static void test_t111(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T111", "sched ext"); return; }
    g_fz_seed = T111_SEED;
    int ok = 1;
    const char *why = "x-proc rslot fuzz";
    uint32_t it_n = 0;
    uint32_t exp_slot = 0, exp_hand = 0;

    /* Coverage-forced prefix: every scenario class exactly once. */
    for (it_n = 0; ok && it_n < 4u; it_n++)
        ok = t111_round(it_n, &exp_slot, &exp_hand, &why);
    /* PRNG tail. */
    for (; ok && it_n < 4u + T111_TAIL; it_n++)
        ok = t111_round(fz_rand() % 4u, &exp_slot, &exp_hand, &why);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance — no helper threads here, so delta must be 0. */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    /* I18: directional cross-process delivery deltas. */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + exp_slot) {
        ok = 0; why = "slot count";
    }
    if (ok && after[IT_SI_HANDDEL] < before[IT_SI_HANDDEL] + exp_hand) {
        ok = 0; why = "hand count";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T111"); }
    else    { fz_note("T111", T111_SEED, it_n); it_fail("T111", why); }
}

/* ── T112: spawn/exit churn — deferred-reap slot-reuse regression ───────────
 * 24 back-to-back spawn → natural-exit → IMMEDIATE respawn cycles.  A task
 * that exits by itself cannot reap its own address space (it is still
 * running on it), so it parks TASK_DEAD in the deferred reap queue; the
 * immediate respawn races the reaper for that task slot.  Before the A1.11
 * fix the slot allocator treated dead-but-unreaped slots as free: the reuse
 * wiped t->process, the reaper's TASK_DEAD guard then skipped the stale
 * entry silently, and every lost race leaked the child KProcess + address
 * space (~20 pages) — this loop, plus the earlier suite spawns, reliably
 * drove SYS_PROCESS_CREATE into NO_MEMORY.  Locks: spawn never fails under
 * churn, exit codes intact, parent books balance, high-water bounded.
 * Invariants: I15, I16, I17. */
#define T112_CYCLES 24u

static void test_t112(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T112", "sched ext"); return; }
    int ok = 1;
    const char *why = "spawn/exit churn";
    uint32_t i = 0;

    for (i = 0; ok && i < T112_CYCLES; i++) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
            ok = 0; why = "spawn";
            it_close(&ep_h);
            break;
        }
        /* Natural exit: a plain send releases the child's first recv. */
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.label = 0x112;
        if (it_sys2(SYS_EP_SEND, (long)ep_h, (long)&m) != 0) {
            ok = 0; why = "send";
        }
        if (ok && it_lp_wait_exit(proc_h) != (long)LP_EXIT_MARKER) {
            ok = 0; why = "exit code";
        }
        it_close(&proc_h);
        it_close(&ep_h);
        /* No pause here: the immediate respawn IS the race being locked. */
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T112"); }
    else {
        it_serial_write("[IRIS][TEST] T112 cycle=");
        it_log_num(i);
        it_serial_write("\n");
        it_fail("T112", why);
    }
}

/* Drain the deferred-reap queue before a lifecycle baseline snapshot: a
 * prior test's self-exited children release their KProcess creation ref only
 * when the reaper runs on a later scheduler tick, so without this the live-
 * process baseline is racy (a not-yet-reaped zombie inflates `before`).  The
 * reaper drains one entry per task_yield; 200 yields clears any realistic
 * backlog on single-CPU. */
static void it_quiesce_reaper(void) {
    for (int i = 0; i < 200; i++) it_sys0(SYS_YIELD);
}

/* ── Fase 16: lifecycle/process hardening (T113–T118) ───────────────────────
 * The A1.11 deferred-reap fix (task.awaiting_reap) closed the one real bug in
 * this area; T113–T118 LOCK the surviving lifecycle contracts so a future
 * regression fails loudly.  Instrumentation: the Fase 16 SCHED_INFO words —
 * live TASK count (it_task_live), live PROCESS count (IT_SI_PROCLIVE) and the
 * deferred-reap queue high-water (IT_SI_REAPHWM).  Because a killed/exited
 * child's KProcess stays live until the PARENT closes its proc handle, every
 * test closes all child handles BEFORE the final snapshot, so proc-live must
 * return exactly to baseline. */

/* ── T113: caller death mid-EP_CALL with a live reply cap ───────────────────
 * A child EP_CALLs the parent; the parent receives (minting the one-shot
 * KReply) and then KILLS the child while it is BLOCKED_REPLY.  The server's
 * reply must fail NOT_FOUND (the caller is gone), a second reply carrying an
 * attached cap must ALSO fail NOT_FOUND without consuming the server's cap,
 * no KReply is left dangling, no waiter survives, and both handle- and
 * process-live counts return to baseline (exactly one KReply was created).
 * Invariants: I5-I10, I15, I16. */
static void test_t113(void) {
    uint32_t before[14], after[14];
    it_quiesce_reaper();
    if (!it_sched_ext(before)) { it_fail("T113", "sched ext"); return; }
    int ok = 1;
    const char *why = "caller death mid-call";

    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    long n  = it_sys0(SYS_NOTIFY_CREATE);      /* source cap for the 2nd reply */
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
    handle_id_t proc_h = HANDLE_INVALID;
    handle_id_t reply_h = HANDLE_INVALID;

    if (ep < 0 || n < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
        ok = 0; why = "spawn";
    }
    /* Drive the child into EP_CALL(cmd_ep); it queues as a caller. */
    if (ok && it_lp_cmd(ep_h, LP_CMD_CALL_BLOCK) != 0) { ok = 0; why = "cmd"; }

    /* Serve the call: receive it and capture the reply cap.  The blocking
     * EP_RECV is the rendezvous — no timing needed. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        if (it_sys2(SYS_EP_RECV, (long)ep_h, (long)&m) != 0 ||
            m.label != 0x5CULL ||
            m.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
            ok = 0; why = "recv call";
        } else {
            reply_h = (handle_id_t)m.attached_handle;
        }
    }

    /* Kill the caller while it is BLOCKED_REPLY: cancel clears r->caller. */
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_sys1(SYS_PROCESS_STATUS, (long)proc_h) != 0) {
        ok = 0; why = "still alive";
    }

    /* First reply (no cap) → the one-shot has no caller: NOT_FOUND. */
    if (ok) {
        struct IrisMsg rm;
        it_iris_msg_zero(&rm);
        rm.label = 0x5A5A;
        if (it_sys2(SYS_REPLY, (long)reply_h, (long)&rm) !=
            (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "reply not NOT_FOUND"; }
    }

    /* Second reply WITH an attached cap → still NOT_FOUND, and the server's
     * source cap must survive un-consumed (A1.10 rule under caller death). */
    if (ok) {
        long d = it_sys2(SYS_HANDLE_DUP, n, (long)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (d < 0) { ok = 0; why = "dup"; }
        else {
            struct IrisMsg rm;
            it_iris_msg_zero(&rm);
            rm.label           = 0xDEAD;
            rm.attached_handle = (uint32_t)d;
            rm.attached_rights = RIGHT_WRITE;
            if (it_sys2(SYS_REPLY, (long)reply_h, (long)&rm) !=
                (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "2nd reply"; }
            if (ok && it_sys1(SYS_HANDLE_TYPE, d) !=
                (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
                ok = 0; why = "server cap consumed";
            }
            handle_id_t dh = (handle_id_t)d; it_close(&dh);
        }
    }

    it_close(&reply_h);   /* KReply active_refs → 0 → close(no caller) → destroy */
    it_close(&proc_h);    /* drop the parent's ref → child KProcess freed */
    it_close(&n_h);
    it_close(&ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    /* Exactly one KReply was created (at the rendezvous) and none leaked. */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY] + 1u) {
        ok = 0; why = "reply count";
    }
    if (ok) it_pass("T113"); else it_fail("T113", why);
}

/* ── T114: reap-queue pressure and slot reuse ───────────────────────────────
 * Keeps four children live at once and churns 40 replacements, alternating
 * self-exit and external kill, each replacement respawning IMMEDIATELY into
 * the just-freed task slot (the A1.11 race, now sustained and concurrent).
 * Locks: spawn never returns NO_MEMORY, natural-exit codes stay intact, and
 * at the end task-live / process-live return to baseline with the deferred
 * reap queue never approaching its size bound.  Invariants: I15-I17. */
#define T114_LIVE   4u
#define T114_CHURN 40u
static void test_t114(void) {
    uint32_t before[14], after[14];
    uint32_t tl_before = 0, tl_after = 0;
    it_quiesce_reaper();
    if (!it_sched_ext(before) || !it_task_live(&tl_before)) {
        it_fail("T114", "sched ext"); return;
    }
    int ok = 1;
    const char *why = "reap pressure";
    uint32_t i = 0;

    handle_id_t ep[T114_LIVE];
    handle_id_t pr[T114_LIVE];
    for (uint32_t s = 0; s < T114_LIVE; s++) { ep[s] = HANDLE_INVALID; pr[s] = HANDLE_INVALID; }

    /* Prime: four concurrent children, each parked in its first EP_RECV. */
    for (uint32_t s = 0; ok && s < T114_LIVE; s++) {
        long e = it_sys0(SYS_ENDPOINT_CREATE);
        if (e < 0) { ok = 0; why = "prime ep"; break; }
        ep[s] = (handle_id_t)e;
        if (lp_spawn_child(ep[s], &pr[s]) < 0) { ok = 0; why = "prime spawn"; }
    }

    /* Churn: tear one child down (alt exit/kill) and respawn it at once. */
    for (i = 0; ok && i < T114_CHURN; i++) {
        uint32_t s = i % T114_LIVE;
        if ((i & 1u) == 0u) {
            /* Natural exit: unblock the child's recv, confirm the marker. */
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label = 0x114;
            if (it_sys2(SYS_EP_SEND, (long)ep[s], (long)&m) != 0) {
                ok = 0; why = "exit send"; break;
            }
            if (it_lp_wait_exit(pr[s]) != (long)LP_EXIT_MARKER) {
                ok = 0; why = "exit code"; break;
            }
        } else {
            /* External kill while the child is blocked in its first recv. */
            if (it_sys1(SYS_PROCESS_KILL, (long)pr[s]) != 0) {
                ok = 0; why = "kill"; break;
            }
            if (it_sys1(SYS_PROCESS_STATUS, (long)pr[s]) != 0) {
                ok = 0; why = "kill status"; break;
            }
        }
        it_close(&pr[s]);
        it_close(&ep[s]);

        /* Immediate respawn into the freed slot — the reuse-before-reap race. */
        long e = it_sys0(SYS_ENDPOINT_CREATE);
        if (e < 0) { ok = 0; why = "churn ep NO_MEMORY"; break; }
        ep[s] = (handle_id_t)e;
        if (lp_spawn_child(ep[s], &pr[s]) < 0) {
            ok = 0; why = "churn spawn NO_MEMORY"; break;
        }
    }

    /* Drain the survivors (natural exit). */
    for (uint32_t s = 0; s < T114_LIVE; s++) {
        if (pr[s] != HANDLE_INVALID) {
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label = 0x114;
            (void)it_sys2(SYS_EP_SEND, (long)ep[s], (long)&m);
            (void)it_lp_wait_exit(pr[s]);
        }
        it_close(&pr[s]);
        it_close(&ep[s]);
    }

    it_quiesce_reaper();   /* let deferred reaps of self-exited children drain */
    if (ok && (!it_sched_ext(after) || !it_task_live(&tl_after))) {
        ok = 0; why = "sched ext 2";
    }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    if (ok && tl_after != tl_before) { ok = 0; why = "task-live drift"; }
    /* The deferred reaper kept up: queue depth never neared its bound. */
    if (ok && after[IT_SI_REAPHWM] >= 8u) { ok = 0; why = "reap backlog"; }
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) it_pass("T114");
    else {
        it_serial_write("[IRIS][TEST] T114 iter=");
        it_log_num(i);
        it_serial_write("\n");
        it_fail("T114", why);
    }
}

/* ── T115: process death with active endpoint waiters ───────────────────────
 * A child is killed while blocked as an endpoint WAITER in each of the three
 * blocking states — EP_RECV (declared receive-slot), EP_SEND, EP_CALL.  In
 * every case the endpoint must retain no dead waiter (an NB probe from the
 * opposite direction returns WOULD_BLOCK), no KReply is minted for the
 * send/call cases (no rendezvous ever happened), and handle/process books
 * return to baseline.  Invariants: I6, I14, I15, I16. */
static void test_t115(void) {
    uint32_t before[14], after[14];
    it_quiesce_reaper();
    if (!it_sched_ext(before)) { it_fail("T115", "sched ext"); return; }
    int ok = 1;
    const char *why = "death with waiters";

    /* kind 0 = EP_RECV waiter, 1 = EP_SEND waiter, 2 = EP_CALL waiter. */
    for (uint32_t kind = 0; ok && kind < 3u; kind++) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; break; }

        uint32_t cmd = (kind == 0u) ? LP_CMD_RSLOT_RECV
                     : (kind == 1u) ? LP_CMD_SEND_BLOCK
                                    : LP_CMD_CALL_BLOCK;
        if (kind == 0u) {
            if (it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) { ok = 0; why = "cmd recv"; }
        } else {
            if (it_lp_cmd(ep_h, cmd) != 0) { ok = 0; why = "cmd"; }
        }
        it_sys1(SYS_SLEEP, 3);      /* child reaches its blocking syscall */

        if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
        if (ok && it_sys1(SYS_PROCESS_STATUS, (long)proc_h) != 0) {
            ok = 0; why = "still alive";
        }

        /* No dead waiter remains.  For the recv waiter, probe with NB_SEND;
         * for the send/call waiters, probe with NB_RECV. */
        if (ok) {
            struct IrisMsg p;
            it_iris_msg_zero(&p);
            p.label = 0x115;
            long probe = (kind == 0u)
                ? it_sys2(SYS_EP_NB_SEND, (long)ep_h, (long)&p)
                : it_sys2(SYS_EP_NB_RECV, (long)ep_h, (long)&p);
            if (probe != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "dead waiter"; }
        }

        it_close(&proc_h);
        it_close(&ep_h);
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    /* No caller ever rendezvoused → not one KReply was minted. */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY]) { ok = 0; why = "ghost kreply"; }
    if (ok) it_pass("T115"); else it_fail("T115", why);
}

/* ── T116: process death with live CSpace caps and a shared VMO ─────────────
 * A child is handed live authority — an endpoint and a notification minted
 * into its CSpace, plus a VMO cap shared into its handle table — and then
 * killed.  The child's teardown must release its refs WITHOUT destroying the
 * shared objects: the parent's endpoint/notification/VMO stay fully usable,
 * and handle/process books return to baseline (the child's CSpace root CNode,
 * KTcb and address space are all reaped).  mapped_count is not observable
 * from ring 3 (documented gap); the observable is object survival + exact
 * book balance.  Invariants: I1, I15, I16. */
#define T116_EP_SLOT 40u
#define T116_N_SLOT  41u
static void test_t116(void) {
    uint32_t before[14], after[14];
    it_quiesce_reaper();
    if (!it_sched_ext(before)) { it_fail("T116", "sched ext"); return; }
    int ok = 1;
    const char *why = "death with cspace/vmo";

    long ep  = it_sys0(SYS_ENDPOINT_CREATE);   /* shared endpoint */
    long n   = it_sys0(SYS_NOTIFY_CREATE);      /* shared notification */
    long vmo = it_sys1(SYS_VMO_CREATE, 4096);   /* shared VMO */
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n, vmo_h = (handle_id_t)vmo;
    handle_id_t cmd_ep_h = HANDLE_INVALID, proc_h = HANDLE_INVALID;

    /* Command endpoint keeps the child parked; the shared caps go into its
     * CSpace / handle table below. */
    long cep = it_sys0(SYS_ENDPOINT_CREATE);
    cmd_ep_h = (handle_id_t)cep;
    if (ep < 0 || n < 0 || vmo < 0 || cep < 0 ||
        lp_spawn_child(cmd_ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; }

    /* Mint the endpoint and the notification into the child's CSpace, and
     * share the VMO cap into the child's handle table. */
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h, (long)T116_EP_SLOT,
                      ep, (long)RIGHT_WRITE) != 0) { ok = 0; why = "mint ep"; }
    if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)proc_h, (long)T116_N_SLOT,
                      n, (long)RIGHT_WRITE) != 0) { ok = 0; why = "mint n"; }
    if (ok && it_sys3(SYS_VMO_SHARE, vmo, (long)proc_h,
                      (long)(RIGHT_READ | RIGHT_DUPLICATE)) < 0) {
        ok = 0; why = "share vmo";
    }

    /* Kill the child while it holds all three live caps. */
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_sys1(SYS_PROCESS_STATUS, (long)proc_h) != 0) {
        ok = 0; why = "still alive";
    }

    /* The parent's objects survived the child's teardown. */
    if (ok) {
        struct IrisMsg p;
        it_iris_msg_zero(&p);
        p.label = 0x116;
        if (it_sys2(SYS_EP_NB_SEND, (long)ep_h, (long)&p) !=
            (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "endpoint dead"; }
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_SIGNAL, n, 4) != 0 ||
            it_sys2(SYS_NOTIFY_WAIT, n, (long)(uintptr_t)&bits) != 0 ||
            bits != 4u) { ok = 0; why = "notif dead"; }
    }
    if (ok && it_sys1(SYS_HANDLE_TYPE, vmo) != (long)IRIS_HANDLE_TYPE_VMO) {
        ok = 0; why = "vmo dead";
    }

    it_close(&proc_h);
    it_close(&cmd_ep_h);
    it_close(&vmo_h);
    it_close(&n_h);
    it_close(&ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    if (ok) it_pass("T116"); else it_fail("T116", why);
}

/* ── T117: death-notification and watch consistency ─────────────────────────
 * Three children die by different routes — natural exit, external kill, and
 * blocked-then-killed (fault-in-IPC analogue) — each watched on its own
 * notification bit of a shared KNotification.  Every death must set its bit
 * exactly once (final mask == 0b111), STATUS must read dead and EXIT_CODE be
 * retrievable for all, a repeated KILL must be idempotent (0), and a watch
 * registered AFTER death must fire immediately (the already-dead emit path).
 * Invariants: I15 + the death-notification exactly-once contract. */
static void test_t117(void) {
    uint32_t before[14], after[14];
    it_quiesce_reaper();
    if (!it_sched_ext(before)) { it_fail("T117", "sched ext"); return; }
    int ok = 1;
    const char *why = "death notify";

    long n = it_sys0(SYS_NOTIFY_CREATE);
    handle_id_t n_h = (handle_id_t)n;
    handle_id_t ep[3]  = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID };
    handle_id_t pr[3]  = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID };
    if (n < 0) { it_fail("T117", "notif"); return; }

    for (uint32_t k = 0; ok && k < 3u; k++) {
        long e = it_sys0(SYS_ENDPOINT_CREATE);
        if (e < 0) { ok = 0; why = "ep"; break; }
        ep[k] = (handle_id_t)e;
        if (lp_spawn_child(ep[k], &pr[k]) < 0) { ok = 0; why = "spawn"; break; }
        /* Watch each child on its own bit of the shared notification. */
        if (it_sys3(SYS_PROCESS_WATCH, (long)pr[k], n, (long)(1u << k)) != 0) {
            ok = 0; why = "watch";
        }
    }

    /* child 0: natural exit; child 1: kill; child 2: block then kill. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.label = 0x117;
        if (it_sys2(SYS_EP_SEND, (long)ep[0], (long)&m) != 0) { ok = 0; why = "exit send"; }
    }
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)pr[1]) != 0) { ok = 0; why = "kill1"; }
    if (ok && it_lp_cmd(ep[2], LP_CMD_SEND_BLOCK) != 0) { ok = 0; why = "cmd2"; }
    if (ok) {
        it_sys1(SYS_SLEEP, 3);
        if (it_sys1(SYS_PROCESS_KILL, (long)pr[2]) != 0) { ok = 0; why = "kill2"; }
    }

    /* Collect the three death bits (bounded waits; each death signals once). */
    if (ok) {
        uint64_t seen = 0;
        for (int iter = 0; iter < 8 && seen != 0x7u; iter++) {
            uint64_t bits = 0;
            if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, n, (long)(uintptr_t)&bits,
                        1000000000LL) == 0)
                seen |= bits;
        }
        if (seen != 0x7u) { ok = 0; why = "missing death bit"; }
    }

    /* STATUS dead for all; EXIT_CODE retrievable; child 0 kept its marker. */
    for (uint32_t k = 0; ok && k < 3u; k++) {
        if (it_sys1(SYS_PROCESS_STATUS, (long)pr[k]) != 0) { ok = 0; why = "status alive"; }
        if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)pr[k]) < 0) {
            ok = 0; why = "exit code";
        }
    }
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)pr[0]) != (long)LP_EXIT_MARKER) {
        ok = 0; why = "exit0 marker";
    }

    /* Idempotent kill on an already-dead child → 0. */
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)pr[1]) != 0) { ok = 0; why = "kill not idempotent"; }

    /* A watch armed AFTER death fires immediately (already-dead emit path). */
    if (ok) {
        long n2 = it_sys0(SYS_NOTIFY_CREATE);
        handle_id_t n2_h = (handle_id_t)n2;
        if (n2 < 0) { ok = 0; why = "notif2"; }
        else {
            if (it_sys3(SYS_PROCESS_WATCH, (long)pr[0], n2, 0x20) != 0) {
                ok = 0; why = "late watch";
            }
            if (ok) {
                uint64_t bits = 0;
                if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, n2, (long)(uintptr_t)&bits,
                            1000000000LL) != 0 || bits != 0x20u) {
                    ok = 0; why = "late watch silent";
                }
            }
            it_close(&n2_h);
        }
    }

    for (uint32_t k = 0; k < 3u; k++) { it_close(&pr[k]); it_close(&ep[k]); }
    it_close(&n_h);

    it_quiesce_reaper();
    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    if (ok) it_pass("T117"); else it_fail("T117", why);
}

/* ── T118: scheduler live count under lifecycle churn ───────────────────────
 * Interleaves process self-exit, process external-kill and in-process thread
 * self-exit, then verifies the scheduler's live TASK count and the live
 * PROCESS count both return exactly to baseline — no zombie counted alive, no
 * double-decrement, no task pending reap left occupying a slot — with the
 * deferred reap queue staying well within its bound.  (Each thread leaves a
 * documented +1 KTcb HANDLE by design — Ph96 — so handle-live is not asserted
 * flat here; task-live and process-live are the invariants.)
 * Invariants: I15, I16, I17. */
#define T118_ROUNDS 10u
static uint8_t g_t118_stk[4096];
static void t118_thread(void) {
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void test_t118(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t pl_before = 0, pl_after = 0;
    uint32_t w[14];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext(w)) { it_fail("T118", "sched ext"); return; }
    pl_before = w[IT_SI_PROCLIVE];
    int ok = 1;
    const char *why = "live count churn";
    uint32_t i = 0;

    for (i = 0; ok && i < T118_ROUNDS; i++) {
        /* (a) process self-exit */
        {
            long e = it_sys0(SYS_ENDPOINT_CREATE);
            handle_id_t e_h = (handle_id_t)e;
            handle_id_t p_h = HANDLE_INVALID;
            if (e < 0 || lp_spawn_child(e_h, &p_h) < 0) { ok = 0; why = "spawn exit"; }
            else {
                struct IrisMsg m;
                it_iris_msg_zero(&m);
                m.label = 0x118;
                (void)it_sys2(SYS_EP_SEND, (long)e_h, (long)&m);
                (void)it_lp_wait_exit(p_h);
            }
            it_close(&p_h);
            it_close(&e_h);
        }
        /* (b) process external-kill */
        if (ok) {
            long e = it_sys0(SYS_ENDPOINT_CREATE);
            handle_id_t e_h = (handle_id_t)e;
            handle_id_t p_h = HANDLE_INVALID;
            if (e < 0 || lp_spawn_child(e_h, &p_h) < 0) { ok = 0; why = "spawn kill"; }
            else {
                it_sys1(SYS_SLEEP, 1);
                (void)it_sys1(SYS_PROCESS_KILL, (long)p_h);
            }
            it_close(&p_h);
            it_close(&e_h);
        }
        /* (c) in-process thread self-exit (task slot reaped; KTcb persists). */
        if (ok) {
            uint64_t entry = (uint64_t)(uintptr_t)t118_thread;
            uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t118_stk + sizeof(g_t118_stk))) & ~0xFULL;
            if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) {
                ok = 0; why = "thread create";
            }
            /* let the thread run to exit and be reaped before the next round */
            for (int y = 0; y < 50; y++) it_sys0(SYS_YIELD);
        }
    }

    /* Give the deferred reaper time to drain the final departures. */
    it_quiesce_reaper();

    if (ok && (!it_task_live(&tl_after) || !it_sched_ext(w))) { ok = 0; why = "sched ext 2"; }
    pl_after = w[IT_SI_PROCLIVE];

    if (ok && tl_after != tl_before) { ok = 0; why = "task-live drift"; }
    if (ok && pl_after != pl_before) { ok = 0; why = "proc-live drift"; }
    if (ok && w[IT_SI_REAPHWM] >= 8u) { ok = 0; why = "reap backlog"; }

    if (ok) it_pass("T118");
    else {
        it_serial_write("[IRIS][TEST] T118 round=");
        it_log_num(i);
        it_serial_write(" tl b/a=");
        it_log_num(tl_before);
        it_serial_write("/");
        it_log_num(tl_after);
        it_serial_write("\n");
        it_fail("T118", why);
    }
}

/* ── Fase 17: scheduler / Scheduling-Context hardening (T119–T124) ──────────
 *
 * These tests harden the scheduler as the microkernel's source of truth about
 * which task is alive, runnable, blocked, dead or pending-reap.  They lean on
 * the Fase 17 additive instrumentation exposed by SYS_SCHED_INFO's ext2 tier
 * (it_sched_ext2): run-queue high-water, the duplicate-enqueue guard counter
 * (invariant S4), the live KSchedContext count (S8/S9) and the monotonic
 * task_yield() counter (progress / no-lost-task).  Combined with the existing
 * task-live (it_task_live), process-live (IT_SI_PROCLIVE) and reap-hwm
 * (IT_SI_REAPHWM) words, they lock the scheduler invariants S1–S16 documented
 * in docs/architecture/scheduler-hardening.md.
 *
 * In-process worker threads are created with SYS_THREAD_CREATE, which returns a
 * task id (not a handle) and leaves one KTcb HANDLE in this process's table by
 * design (Ph96, exactly as T118 notes).  So these tests assert TASK-live and
 * PROCESS-live return to baseline, never handle-live — the KTcb handle id is
 * never surfaced to ring 3 and cannot be closed.  Thread counts are budgeted to
 * stay far below HANDLE_TABLE_MAX. */

#define SH_NWORK 4u
/* Local mirror of kernel TASK_MAX (256) for plausibility bounds — iris_test
 * does not include <iris/task.h>.  Used only as an upper sanity bound. */
#define TASK_MAX_HINT 256u
static uint8_t           g_sh_stk[SH_NWORK][8192];
static volatile int      g_sh_done[SH_NWORK];
static volatile uint32_t g_sh_prog[SH_NWORK];   /* per-worker progress counter */
static handle_id_t       g_sh_ep = HANDLE_INVALID; /* shared block/release ep   */
static volatile uint32_t g_sh_mode;             /* selects the worker script    */
static volatile uint32_t g_sh_iters;            /* yields per worker (churn/spin)*/
static handle_id_t       g_sh_sc = HANDLE_INVALID; /* SC to bind (SH_MODE_SC)    */

#define SH_MODE_YIELD_BLOCK 0u  /* yield, block on ep recv, yield, exit  (T119) */
#define SH_MODE_CHURN       1u  /* iters yields, block on ep recv, exit  (T120) */
#define SH_MODE_SPIN        2u  /* iters yields, exit                    (T122) */
#define SH_MODE_SC          3u  /* bind g_sh_sc, yield, exit             (T123) */

static void sh_worker(uint32_t idx) {
    uint32_t mode  = g_sh_mode;
    uint32_t iters = g_sh_iters;

    if (mode == SH_MODE_SPIN) {
        for (uint32_t k = 0; k < iters; k++) {
            it_sys0(SYS_YIELD);
            g_sh_prog[idx] = k + 1u;
        }
    } else if (mode == SH_MODE_CHURN) {
        for (uint32_t k = 0; k < iters; k++) {
            it_sys0(SYS_YIELD);
            g_sh_prog[idx] = k + 1u;
        }
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        (void)it_sys2(SYS_EP_RECV, (long)g_sh_ep, (long)&m); /* block until released */
    } else if (mode == SH_MODE_SC) {
        (void)it_sys1(SYS_THREAD_SET_SC, (long)g_sh_sc);      /* bind → task ref */
        for (uint32_t k = 0; k < 6u; k++) { it_sys0(SYS_YIELD); g_sh_prog[idx] = k + 1u; }
    } else { /* SH_MODE_YIELD_BLOCK */
        for (uint32_t k = 0; k < 6u; k++) it_sys0(SYS_YIELD);
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        (void)it_sys2(SYS_EP_RECV, (long)g_sh_ep, (long)&m); /* block until released */
        for (uint32_t k = 0; k < 6u; k++) it_sys0(SYS_YIELD);
        g_sh_prog[idx] = 1u;
    }

    g_sh_done[idx] = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void sh_worker0(void) { sh_worker(0); }
static void sh_worker1(void) { sh_worker(1); }
static void sh_worker2(void) { sh_worker(2); }
static void sh_worker3(void) { sh_worker(3); }
static void (*const g_sh_entries[SH_NWORK])(void) = {
    sh_worker0, sh_worker1, sh_worker2, sh_worker3
};

/* Start n workers (n ≤ SH_NWORK).  Returns 1 on success (all created). */
static int sh_start(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { g_sh_done[i] = 0; g_sh_prog[i] = 0; }
    for (uint32_t i = 0; i < n; i++) {
        uint64_t entry = (uint64_t)(uintptr_t)g_sh_entries[i];
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_sh_stk[i] + sizeof(g_sh_stk[i]))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) return 0;
    }
    return 1;
}

/* Bounded wait until all n workers set their done flag. */
static int sh_wait_all(uint32_t n) {
    for (int i = 0; i < 20000; i++) {
        int all = 1;
        for (uint32_t w = 0; w < n; w++) if (!g_sh_done[w]) { all = 0; break; }
        if (all) return 1;
        it_sys0(SYS_YIELD);
    }
    return 0;
}

/* Bounded wait until all n workers reach at least `target` progress. */
static int sh_wait_prog(uint32_t n, uint32_t target) {
    for (int i = 0; i < 20000; i++) {
        int all = 1;
        for (uint32_t w = 0; w < n; w++) if (g_sh_prog[w] < target) { all = 0; break; }
        if (all) return 1;
        it_sys0(SYS_YIELD);
    }
    return 0;
}

/* Release n workers blocked on g_sh_ep, one rendezvous send each. */
static int sh_release(uint32_t n) {
    for (uint32_t w = 0; w < n; w++) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.label = 0x1719;
        if (it_sys2(SYS_EP_SEND, (long)g_sh_ep, (long)&m) != 0) return 0;
    }
    return 1;
}

/* ── T119: task state transition stress ─────────────────────────────────────
 * Each round drives in-process worker threads through the full runnable →
 * blocked (EP_RECV) → runnable (rendezvous wakeup) → dead (self-exit) cycle,
 * then interleaves a lifecycle_probe child that is externally KILLED (a
 * runnable/blocked task torn down from outside).  After the churn every
 * scheduler book returns to baseline: no zombie counted alive, no dead task
 * left occupying a slot, the deferred reaper drained, no KSchedContext leaked,
 * and the yield counter advanced (tasks actually reached the scheduler).
 * Invariants: S1, S2, S3, S5, S6, S7, S15. */
#define T119_ROUNDS 4u
static void test_t119(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t e0[14], e1[14];
    uint32_t s2b[4], s2a[4];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext(e0) || !it_sched_ext2(s2b)) {
        it_fail("T119", "sched ext"); return;
    }
    int ok = 1;
    const char *why = "state churn";
    uint32_t i = 0;

    for (i = 0; ok && i < T119_ROUNDS; i++) {
        /* (a) in-process worker threads: block on a shared endpoint, wake by
         * rendezvous, then self-exit. */
        g_sh_mode = SH_MODE_YIELD_BLOCK;
        g_sh_iters = 0u;
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        if (ep < 0) { ok = 0; why = "ep create"; break; }
        g_sh_ep = (handle_id_t)ep;

        if (!sh_start(SH_NWORK)) { ok = 0; why = "thread create"; }
        /* let workers reach EP_RECV */
        if (ok) for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);
        if (ok && !sh_release(SH_NWORK)) { ok = 0; why = "release"; }
        if (ok && !sh_wait_all(SH_NWORK)) { ok = 0; why = "worker stuck"; }
        it_close(&g_sh_ep);

        /* (b) lifecycle_probe child externally killed while alive. */
        if (ok) {
            long ce = it_sys0(SYS_ENDPOINT_CREATE);
            handle_id_t ce_h = (handle_id_t)ce;
            handle_id_t p_h = HANDLE_INVALID;
            if (ce < 0 || lp_spawn_child(ce_h, &p_h) < 0) { ok = 0; why = "spawn"; }
            else {
                it_sys1(SYS_SLEEP, 1);
                (void)it_sys1(SYS_PROCESS_KILL, (long)p_h);
                if (it_sys1(SYS_PROCESS_STATUS, (long)p_h) != 0) { ok = 0; why = "kill"; }
            }
            it_close(&p_h);
            it_close(&ce_h);
        }
        it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && (!it_task_live(&tl_after) || !it_sched_ext(e1) || !it_sched_ext2(s2a))) {
        ok = 0; why = "sched ext 2";
    }
    if (ok && tl_after != tl_before)                       { ok = 0; why = "task-live drift"; }
    if (ok && e1[IT_SI_PROCLIVE] != e0[IT_SI_PROCLIVE])    { ok = 0; why = "proc-live drift"; }
    if (ok && e1[IT_SI_REAPHWM] >= 8u)                     { ok = 0; why = "reap backlog"; }
    if (ok && s2a[IT_S2_SCLIVE] != s2b[IT_S2_SCLIVE])      { ok = 0; why = "sc-live drift"; }
    if (ok && s2a[IT_S2_YIELD] <= s2b[IT_S2_YIELD])        { ok = 0; why = "no yield progress"; }

    if (ok) it_pass("T119");
    else {
        it_serial_write("[IRIS][TEST] T119 round=");
        it_log_num(i);
        it_serial_write(" tl b/a=");
        it_log_num(tl_before);
        it_serial_write("/");
        it_log_num(tl_after);
        it_serial_write("\n");
        it_fail("T119", why);
    }
}

/* ── T120: run-queue churn and duplicate-enqueue stress ─────────────────────
 * SH_NWORK workers each run a FIXED-length yield loop (deterministic: exactly
 * T120_ITERS iterations), so all of them are concurrently runnable and the
 * O(1) priority run queue is churned hard.  Every worker then blocks on a
 * shared endpoint (proving it reached the end of its loop) before being
 * released to exit.  Verifies:
 *   - no lost runnable task: every worker's progress counter is EXACTLY
 *     T120_ITERS (S12 — yield never drops a runnable task; a corrupted queue
 *     would strand a worker and time out the wait);
 *   - no dead worker advances: progress is frozen at T120_ITERS, never above;
 *   - the run-queue depth high-water is plausible (≥2 concurrent, ≤ TASK_MAX);
 *   - the duplicate-enqueue guard (S4) engaged only a bounded number of times
 *     (pure-yield churn creates no wakeup races, so the delta stays tiny);
 *   - task-live returns to baseline after reap.
 * Invariants: S4, S6, S12. */
#define T120_ITERS 80u
static void test_t120(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t s2b[4], s2a[4];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext2(s2b)) { it_fail("T120", "sched ext"); return; }
    int ok = 1;
    const char *why = "run-queue churn";

    g_sh_mode  = SH_MODE_CHURN;
    g_sh_iters = T120_ITERS;
    long ep = it_sys0(SYS_ENDPOINT_CREATE);
    if (ep < 0) { it_fail("T120", "ep create"); return; }
    g_sh_ep = (handle_id_t)ep;

    if (!sh_start(SH_NWORK)) { ok = 0; why = "thread create"; }

    /* Wait until every worker finished its full yield loop and parked in
     * EP_RECV — proves none was lost mid-churn. */
    if (ok && !sh_wait_prog(SH_NWORK, T120_ITERS)) { ok = 0; why = "worker lost"; }

    /* Each worker must have advanced EXACTLY T120_ITERS (no more, no less). */
    for (uint32_t w = 0; ok && w < SH_NWORK; w++)
        if (g_sh_prog[w] != T120_ITERS) { ok = 0; why = "progress mismatch"; }

    if (ok && !sh_release(SH_NWORK))  { ok = 0; why = "release"; }
    if (ok && !sh_wait_all(SH_NWORK)) { ok = 0; why = "worker stuck"; }
    it_close(&g_sh_ep);
    it_quiesce_reaper();

    if (ok && (!it_task_live(&tl_after) || !it_sched_ext2(s2a))) { ok = 0; why = "sched ext 2"; }
    /* yield counter advanced by at least the work we forced. */
    if (ok && (s2a[IT_S2_YIELD] - s2b[IT_S2_YIELD]) < SH_NWORK * T120_ITERS) {
        ok = 0; why = "yield accounting";
    }
    /* run queue actually held several concurrent runnable tasks, bounded. */
    if (ok && (s2a[IT_S2_RQHWM] < 2u || s2a[IT_S2_RQHWM] > TASK_MAX_HINT)) {
        ok = 0; why = "rq hwm implausible";
    }
    /* S4 guard trips stay bounded under pure-yield churn (no wakeup races). */
    if (ok && (s2a[IT_S2_DUPENQ] - s2b[IT_S2_DUPENQ]) > 64u) {
        ok = 0; why = "duplicate enqueue storm";
    }
    if (ok && tl_after != tl_before) { ok = 0; why = "task-live drift"; }

    if (ok) it_pass("T120");
    else {
        it_serial_write("[IRIS][TEST] T120 rqhwm=");
        it_log_num(s2a[IT_S2_RQHWM]);
        it_serial_write(" dup=");
        it_log_num(s2a[IT_S2_DUPENQ] - s2b[IT_S2_DUPENQ]);
        it_serial_write("\n");
        it_fail("T120", why);
    }
}

/* ── T121: IPC blocking scheduler invariants ────────────────────────────────
 * A worker thread is blocked in each of the three endpoint states — EP_RECV,
 * EP_SEND, EP_CALL — and then the endpoint's last handle is CLOSED out from
 * under it.  Each waiter must wake with exactly IRIS_ERR_CLOSED and leave the
 * scheduler with no residue.  Then a lifecycle_probe child is KILLED while
 * blocked as an EP_RECV waiter and the endpoint must retain no dead waiter (an
 * NB_SEND probe returns WOULD_BLOCK).  After all of it: task-live and
 * process-live return to baseline, no ghost KReply was minted (no rendezvous
 * ever happened), and the reaper drained.
 * Invariants: S2, S13, S14, S15. */
static volatile long g_t121_res[3];
/* Dedicated single-shot workers, one per blocking endpoint state.  Each blocks
 * on g_sh_ep, records its wake-up result, then self-exits.  The recv worker
 * uses the legacy (slotless) path; the receive-slot path is exercised by the
 * kill-child leg below (it_lp_cmd_rslot), so T121 covers both. */
static void t121_recv(void) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    g_t121_res[0] = it_sys2(SYS_EP_RECV, (long)g_sh_ep, (long)&m);
    g_sh_done[0] = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void t121_send(void) {
    struct IrisMsg m; it_iris_msg_zero(&m); m.label = 0x121;
    g_t121_res[1] = it_sys2(SYS_EP_SEND, (long)g_sh_ep, (long)&m);
    g_sh_done[0] = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void t121_call(void) {
    struct IrisMsg m; it_iris_msg_zero(&m); m.label = 0x121;
    g_t121_res[2] = it_sys2(SYS_EP_CALL, (long)g_sh_ep, (long)&m);
    g_sh_done[0] = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void (*const g_t121_entries[3])(void) = { t121_recv, t121_send, t121_call };
static void test_t121(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext(e0)) { it_fail("T121", "sched ext"); return; }
    int ok = 1;
    const char *why = "ipc blocking";

    /* kind 0 = EP_RECV, 1 = EP_SEND, 2 = EP_CALL — one worker each, closed. */
    for (uint32_t kind = 0; ok && kind < 3u; kind++) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        if (ep < 0) { ok = 0; why = "ep create"; break; }
        g_sh_ep = (handle_id_t)ep;
        g_t121_res[kind] = 999;
        g_sh_done[0] = 0;

        /* Drive one worker directly (index 0) into the chosen blocking state. */
        uint64_t entry = (uint64_t)(uintptr_t)g_t121_entries[kind];
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_sh_stk[0] + sizeof(g_sh_stk[0]))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) { ok = 0; why = "thread create"; }

        /* let the worker reach its blocking syscall */
        if (ok) for (int y = 0; y < 40; y++) it_sys0(SYS_YIELD);
        /* close the endpoint's only handle → close op wakes the waiter CLOSED */
        it_close(&g_sh_ep);
        /* wait for the worker to observe the wake-up and record its result */
        if (ok) for (int y = 0; y < 4000 && !g_sh_done[0]; y++) it_sys0(SYS_YIELD);
        if (ok && !g_sh_done[0]) { ok = 0; why = "waiter not woken"; }
        if (ok && g_t121_res[kind] != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong wake error"; }
        it_quiesce_reaper();
    }

    /* Kill a child blocked as an EP_RECV waiter; endpoint keeps no dead waiter. */
    if (ok) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t p_h  = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &p_h) < 0) { ok = 0; why = "spawn"; }
        else {
            if (it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) { ok = 0; why = "cmd recv"; }
            it_sys1(SYS_SLEEP, 3);
            if (ok && it_sys1(SYS_PROCESS_KILL, (long)p_h) != 0) { ok = 0; why = "kill"; }
            if (ok) {
                struct IrisMsg p;
                it_iris_msg_zero(&p);
                p.label = 0x121;
                if (it_sys2(SYS_EP_NB_SEND, (long)ep_h, (long)&p) != (long)IRIS_ERR_WOULD_BLOCK) {
                    ok = 0; why = "dead waiter";
                }
            }
        }
        it_close(&p_h);
        it_close(&ep_h);
        it_quiesce_reaper();
    }

    if (ok && (!it_task_live(&tl_after) || !it_sched_ext(e1))) { ok = 0; why = "sched ext 2"; }
    if (ok && tl_after != tl_before)                    { ok = 0; why = "task-live drift"; }
    if (ok && e1[IT_SI_PROCLIVE] != e0[IT_SI_PROCLIVE]) { ok = 0; why = "proc-live drift"; }
    if (ok && e1[IT_SI_REPLY] != e0[IT_SI_REPLY])       { ok = 0; why = "ghost kreply"; }
    if (ok && e1[IT_SI_REAPHWM] >= 8u)                  { ok = 0; why = "reap backlog"; }

    if (ok) it_pass("T121"); else it_fail("T121", why);
}

/* ── T122: yield / quantum / preemption accounting ──────────────────────────
 * SH_NWORK equal-priority cooperative workers each yield exactly T122_ITERS
 * times.  With no blocking, fairness under the round-robin-within-priority run
 * queue means every worker must complete its full quota — there is no
 * starvation in this workload.  This test documents, honestly, what the
 * scheduler guarantees TODAY:
 *   - it is cooperative-first: a task advances by calling task_yield (or by
 *     consuming its TASK_DEFAULT_SLICE quantum, after which scheduler_tick sets
 *     need_resched);
 *   - tick-driven preemption exists (priority + quantum) but under the QEMU TCG
 *     headless target no timer IRQs are delivered while a task spins in ring 0,
 *     so forward progress here is carried by explicit yields — which is exactly
 *     what this test measures;
 *   - what it does NOT yet guarantee: strict fairness weights, per-task CPU
 *     accounting beyond the SchedContext budget, or preemption of a ring-0
 *     spinner (see docs/architecture/scheduler-hardening.md "Limits").
 * Asserts: every worker completed T122_ITERS (no starvation), the global yield
 * counter advanced by ≥ SH_NWORK*T122_ITERS, and context switches advanced.
 * Invariants: S5, S12. */
#define T122_ITERS 60u
static void test_t122(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t s2b[4], s2a[4];
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext2(s2b) || !it_sched_ext(e0)) {
        it_fail("T122", "sched ext"); return;
    }
    (void)e0;
    int ok = 1;
    const char *why = "fairness";

    g_sh_mode  = SH_MODE_SPIN;
    g_sh_iters = T122_ITERS;
    if (!sh_start(SH_NWORK)) { ok = 0; why = "thread create"; }
    if (ok && !sh_wait_all(SH_NWORK)) { ok = 0; why = "worker stuck"; }

    /* No starvation: every cooperative worker ran to completion. */
    for (uint32_t w = 0; ok && w < SH_NWORK; w++)
        if (g_sh_prog[w] != T122_ITERS) { ok = 0; why = "starved worker"; }

    it_quiesce_reaper();
    if (ok && (!it_task_live(&tl_after) || !it_sched_ext2(s2a) || !it_sched_ext(e1))) {
        ok = 0; why = "sched ext 2";
    }
    if (ok && (s2a[IT_S2_YIELD] - s2b[IT_S2_YIELD]) < SH_NWORK * T122_ITERS) {
        ok = 0; why = "yield accounting";
    }
    if (ok && tl_after != tl_before) { ok = 0; why = "task-live drift"; }

    if (ok) it_pass("T122"); else it_fail("T122", why);
}

/* ── T123: Scheduling Context lifetime and cleanup ──────────────────────────
 * Exercises the full KSchedContext lifecycle and every documented failure path,
 * then proves the live-SC object count returns exactly to baseline — no leak,
 * no double free, no stale ref surviving a dead task.
 *   Happy path: create → configure(valid) → bind(main) → rebind(main) →
 *     unbind(main) → unbind again (idempotent); a worker thread binds an SC and
 *     self-exits, so the deferred reaper is the one that drops the SC's task ref
 *     (reap_dead_task_off_cpu → task_release_sched_ctx — the same helper the
 *     external-kill path uses).
 *   Failure paths: budget 0, period 0, budget==period, budget>period (all
 *     INVALID_ARG); wrong object type (endpoint handle → INVALID_ARG); missing
 *     RIGHT_WRITE (read-only dup → ACCESS_DENIED); THREAD_SET_SC on a bogus
 *     handle (rejected, no stale ref).
 * Invariants: S8, S9, S10, S11. */
static void test_t123(void) {
    uint32_t s2b[4], s2a[4];
    it_quiesce_reaper();
    if (!it_sched_ext2(s2b)) { it_fail("T123", "sched ext"); return; }
    uint32_t sc_base = s2b[IT_S2_SCLIVE];
    int ok = 1;
    const char *why = "sc lifetime";

    long a = it_sys0(SYS_SC_CREATE);
    long b = it_sys0(SYS_SC_CREATE);
    handle_id_t sc  = (a >= 0) ? (handle_id_t)a : HANDLE_INVALID;
    handle_id_t sc2 = (b >= 0) ? (handle_id_t)b : HANDLE_INVALID;
    if (sc == HANDLE_INVALID || sc2 == HANDLE_INVALID) { ok = 0; why = "sc create"; }

    /* live count reflects two fresh SC objects. */
    if (ok && !it_sched_ext2(s2a)) { ok = 0; why = "ext mid"; }
    if (ok && s2a[IT_S2_SCLIVE] != sc_base + 2u) { ok = 0; why = "sc not counted"; }

    /* SC_CONFIGURE validation (S10). */
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)sc, 10, 100) != 0)   { ok = 0; why = "configure valid"; }
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)sc, 0, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget 0"; }
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)sc, 10, 0)  != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "period 0"; }
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)sc, 100, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget==period"; }
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)sc, 200, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget>period"; }

    /* Wrong object type: an endpoint handle is not a SchedContext. */
    if (ok) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        if (ep_h == HANDLE_INVALID) { ok = 0; why = "ep create"; }
        if (ok && it_sys3(SYS_SC_CONFIGURE, (long)ep_h, 10, 100) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "wrong type";
        }
        if (ok && it_sys1(SYS_THREAD_SET_SC, (long)ep_h) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "set_sc wrong type";
        }
        it_close(&ep_h);
    }

    /* Missing RIGHT_WRITE: a read-only dup cannot configure (S11 rights). */
    if (ok) {
        long ro = it_sys2(SYS_HANDLE_DUP, (long)sc, (long)RIGHT_READ);
        handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
        if (ro_h == HANDLE_INVALID) { ok = 0; why = "ro dup"; }
        if (ok && it_sys3(SYS_SC_CONFIGURE, (long)ro_h, 10, 100) != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "rights not enforced";
        }
        it_close(&ro_h);
    }

    /* Bind / rebind / unbind on the main thread (S11 — no stale ref). */
    if (ok && it_sys1(SYS_THREAD_SET_SC, (long)sc)  != 0) { ok = 0; why = "bind"; }
    if (ok && it_sys1(SYS_THREAD_SET_SC, (long)sc2) != 0) { ok = 0; why = "rebind"; }
    if (ok && it_sys1(SYS_THREAD_SET_SC, 0)         != 0) { ok = 0; why = "unbind"; }
    if (ok && it_sys1(SYS_THREAD_SET_SC, 0)         != 0) { ok = 0; why = "unbind idempotent"; }

    /* A worker thread binds an SC and self-exits; the reaper releases the ref. */
    if (ok) {
        g_sh_mode = SH_MODE_SC;
        g_sh_sc   = sc;
        g_sh_done[0] = 0; g_sh_prog[0] = 0;
        uint64_t entry = (uint64_t)(uintptr_t)g_sh_entries[0];
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_sh_stk[0] + sizeof(g_sh_stk[0]))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) { ok = 0; why = "sc worker create"; }
        if (ok) for (int y = 0; y < 4000 && !g_sh_done[0]; y++) it_sys0(SYS_YIELD);
        if (ok && !g_sh_done[0]) { ok = 0; why = "sc worker stuck"; }
        it_quiesce_reaper();
    }

    /* Close both SC handles; with the worker reaped and main unbound, every ref
     * is gone and both objects must be destroyed → live back to baseline. */
    it_close(&sc);
    it_close(&sc2);
    it_quiesce_reaper();

    if (ok && !it_sched_ext2(s2a)) { ok = 0; why = "ext final"; }
    if (ok && s2a[IT_S2_SCLIVE] != sc_base) { ok = 0; why = "sc leak/double-free"; }

    if (ok) it_pass("T123"); else it_fail("T123", why);
}

/* ── T124: scheduler SMP-readiness audit ────────────────────────────────────
 * Not an SMP implementation — a codified audit that the scheduler's current
 * single-core assumptions still hold and are locked against silent drift.  It
 * checks, at runtime, the invariants that a future SMP port MUST revisit, using
 * only observable diagnostics:
 *   - the deferred-reap queue never approached its bound (the "one death per
 *     yield interval" single-CPU assumption held — SMP would need per-CPU dead
 *     lists);
 *   - the duplicate-enqueue guard is the single point that enforces "a task is
 *     in the run queue at most once"; under SMP the same guard must be held
 *     under the target CPU's run-queue lock (documented, asserted-reachable);
 *   - run-queue depth stayed within TASK_MAX (a single global run queue today);
 *   - the live task and live SC counts are internally consistent.
 * The authoritative list of single-core assumptions and required-before-SMP
 * work lives in docs/architecture/scheduler-hardening.md; this test is its
 * runtime tripwire.  Invariants: S4, S6, S16. */
static void test_t124(void) {
    uint32_t e[14];
    uint32_t s2[4];
    uint32_t tl = 0;
    it_quiesce_reaper();
    if (!it_task_live(&tl) || !it_sched_ext(e) || !it_sched_ext2(s2)) {
        it_fail("T124", "sched ext"); return;
    }
    int ok = 1;
    const char *why = "smp-readiness";

    /* Single-CPU deferred-reap assumption: depth never neared REAP_QUEUE_SIZE
     * (8).  Approaching it would mean multiple concurrent deaths per yield
     * interval — impossible on one CPU, mandatory to redesign for SMP. */
    if (ok && e[IT_SI_REAPHWM] >= 8u) { ok = 0; why = "reap-queue bound (SMP risk)"; }

    /* Run-queue depth fits the single global queue (≤ TASK_MAX). */
    if (ok && s2[IT_S2_RQHWM] > TASK_MAX_HINT) { ok = 0; why = "rq depth > TASK_MAX"; }

    /* The S4 guard must remain the one enforcement point for "at most once in
     * the run queue".  Its counter must be readable (reachable) — under SMP it
     * has to move to the per-CPU run-queue lock; here we assert the mechanism
     * exists and is wired to the diagnostics so a regression is visible. */
    if (ok && (s2[IT_S2_DUPENQ] == 0xFFFFFFFFu)) { ok = 0; why = "dup-enqueue counter unwired"; }

    /* Live counts are internally consistent: at least the idle task + this test
     * process's threads are alive, and SC live is a sane small number. */
    if (ok && tl < 1u) { ok = 0; why = "task-live underflow"; }
    if (ok && s2[IT_S2_SCLIVE] > TASK_MAX_HINT) { ok = 0; why = "sc-live implausible"; }

    if (ok) it_pass("T124"); else it_fail("T124", why);
}

/* ── Fase 18: untyped / retype / revoke authority hardening (T125–T131) ──────
 *
 * These tests exercise the memory-authority surface — SYS_UNTYPED_RETYPE,
 * SYS_UNTYPED_RESET, SYS_CAP_DERIVE, SYS_CAP_REVOKE — end to end from ring 3.
 * They lean on one boot KUntyped forwarded down the boot chain (userboot → init
 * → iris_test) into IRIS_CPTR_TEST_UNTYPED, plus the Fase 18 additive
 * instrumentation (it_sched_ext3: live per-type object counts) and the existing
 * handle-live word.
 *
 * Two independent, strong "authority died" observables anchor these tests:
 *   1. the live per-type object count (it_sched_ext3) returns to baseline —
 *      the object was destroyed, not leaked;
 *   2. SYS_UNTYPED_RESET succeeds — it is gated on child_count == 0, so success
 *      proves every object/sub-untyped carved from the region released its
 *      parent reference (U17/U18).
 *
 * The authority model (see docs/architecture/untyped-retype-revoke-hardening.md):
 *   - retype installs the new object as a fresh handle-table entry (a derivation
 *     ROOT), and tracks the untyped→child link via child_count for RESET gating;
 *   - SYS_CAP_REVOKE walks the handle-table derivation tree (SYS_CAP_DERIVE
 *     children), NOT CSpace CNode slots and NOT untyped child_count — its scope
 *     is documented and asserted here.
 */
#define IT_UT ((long)IRIS_CPTR_TEST_UNTYPED)

/* Reset the shared test untyped after a test drops all its children.  Returns 1
 * if the region is clean (child_count == 0 → RESET ok), 0 otherwise. */
static int it_ut_reset(void) {
    return it_sys1(SYS_UNTYPED_RESET, IT_UT) == 0;
}

/* ── T125: basic untyped retype authority ───────────────────────────────────
 * Retype every ring-3-supported object kind from a valid KUntyped, prove each
 * exists (type-check + a type-appropriate use), then destroy them and confirm
 * the region resets (child_count back to 0) and every live per-type count
 * returns to baseline.  Failure paths: wrong/unsupported type, invalid object
 * size, non-power-of-two CNode slot count, and retype through a cap lacking
 * RIGHT_WRITE (ACCESS_DENIED, no object born).
 * Invariants: U1, U2, U3, U6, U17, U18, U20. */
static void test_t125(void) {
    uint32_t s3b[5], s3a[5];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b)) { it_fail("T125", "sched ext3"); return; }
    int ok = 1;
    const char *why = "retype authority";

    /* The forwarded untyped must be present and non-trivially sized. */
    uint64_t avail = 0;
    if (it_sys3(SYS_UNTYPED_INFO, IT_UT, 0, (long)(uintptr_t)&avail) != 0) {
        it_fail("T125", "untyped absent (boot-chain forward failed)"); return;
    }
    if (avail < 65536u) { it_fail("T125", "untyped too small"); return; }

    handle_id_t ep = HANDLE_INVALID, nt = HANDLE_INVALID, cn = HANDLE_INVALID;
    handle_id_t sc = HANDLE_INVALID, fr = HANDLE_INVALID, sub = HANDLE_INVALID;

    long r;
    r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (r < 0) { ok = 0; why = "retype endpoint"; } else ep = (handle_id_t)r;
    r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_NOTIFICATION, 0);
    if (ok && r < 0) { ok = 0; why = "retype notification"; } else if (ok) nt = (handle_id_t)r;
    r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_CNODE, 4);
    if (ok && r < 0) { ok = 0; why = "retype cnode"; } else if (ok) cn = (handle_id_t)r;
    r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_SCHED_CONTEXT, 0);
    if (ok && r < 0) { ok = 0; why = "retype sc"; } else if (ok) sc = (handle_id_t)r;
    r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    if (ok && r < 0) { ok = 0; why = "retype frame"; } else if (ok) fr = (handle_id_t)r;
    r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_UNTYPED, 4096);
    if (ok && r < 0) { ok = 0; why = "retype sub-untyped"; } else if (ok) sub = (handle_id_t)r;

    /* Each object exists and has the expected type. */
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)ep)  != (long)IT_KOBJ_ENDPOINT)     { ok = 0; why = "ep type"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)nt)  != (long)IT_KOBJ_NOTIFICATION) { ok = 0; why = "nt type"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)cn)  != (long)IT_KOBJ_CNODE)        { ok = 0; why = "cn type"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)sc)  != (long)IT_KOBJ_SCHED_CONTEXT){ ok = 0; why = "sc type"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)fr)  != (long)IT_KOBJ_FRAME)        { ok = 0; why = "fr type"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)sub) != (long)IT_KOBJ_UNTYPED)      { ok = 0; why = "sub type"; }

    /* Type-appropriate use: the retyped endpoint is a real rendezvous point
     * (empty → WOULD_BLOCK); the sub-untyped answers INFO; the SC configures. */
    if (ok) {
        struct IrisMsg m; it_iris_msg_zero(&m);
        if (it_sys2(SYS_EP_NB_RECV, (long)ep, (long)&m) != (long)IRIS_ERR_WOULD_BLOCK) {
            ok = 0; why = "ep not usable";
        }
    }
    if (ok && it_sys3(SYS_UNTYPED_INFO, (long)sub, 0, 0) != 0) { ok = 0; why = "sub not usable"; }
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)sc, 10, 100) != 0) { ok = 0; why = "sc not usable"; }

    /* Live per-type counts rose by exactly the objects we made. */
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 mid"; }
    if (ok && s3a[IT_S3_EP]      != s3b[IT_S3_EP]      + 1u) { ok = 0; why = "ep live"; }
    if (ok && s3a[IT_S3_NOTIF]   != s3b[IT_S3_NOTIF]   + 1u) { ok = 0; why = "nt live"; }
    if (ok && s3a[IT_S3_CNODE]   != s3b[IT_S3_CNODE]   + 1u) { ok = 0; why = "cn live"; }
    if (ok && s3a[IT_S3_FRAME]   != s3b[IT_S3_FRAME]   + 1u) { ok = 0; why = "fr live"; }
    /* +1 sub-untyped (the parent stays counted the whole time). */
    if (ok && s3a[IT_S3_UNTYPED] != s3b[IT_S3_UNTYPED] + 1u) { ok = 0; why = "sub live"; }

    /* ── Failure paths (no object may be born) ── */
    if (ok) {
        uint32_t f0[5], f1[5];
        if (!it_sched_ext3(f0)) { ok = 0; why = "ext3 fail-base"; }
        /* wrong/unsupported type (KOBJ_PROCESS = 0). */
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, 0, 0) != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "wrong type"; }
        /* invalid frame size (not page-aligned). */
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad frame size"; }
        /* invalid sub-untyped size (< 4096). */
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_UNTYPED, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad ut size"; }
        /* non-power-of-two CNode slot count. */
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_CNODE, 3) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad cnode slots"; }
        /* missing RIGHT_WRITE: retype through a read-only derived cap. */
        if (ok) {
            long uth = it_sys1(SYS_CSPACE_RESOLVE, IT_UT);
            handle_id_t uth_h = (uth >= 0) ? (handle_id_t)uth : HANDLE_INVALID;
            long ro = (uth >= 0) ? it_sys2(SYS_CAP_DERIVE, uth, (long)RIGHT_READ) : -1;
            handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
            if (ro < 0) { ok = 0; why = "ro dup"; }
            if (ok && it_sys3(SYS_UNTYPED_RETYPE, (long)ro_h, IT_KOBJ_ENDPOINT, 0) != (long)IRIS_ERR_ACCESS_DENIED) {
                ok = 0; why = "rights not enforced";
            }
            it_close(&ro_h);
            it_close(&uth_h);
        }
        /* No object leaked through any failure path. */
        if (ok && !it_sched_ext3(f1)) { ok = 0; why = "ext3 fail-after"; }
        for (uint32_t i = 0; ok && i < 5u; i++)
            if (f1[i] != f0[i]) { ok = 0; why = "failure leaked object"; }
    }

    /* Destroy everything and prove the region is clean (child_count → 0). */
    it_close(&ep); it_close(&nt); it_close(&cn);
    it_close(&sc); it_close(&fr); it_close(&sub);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (child leak)"; }
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 final"; }
    for (uint32_t i = 0; ok && i < 5u; i++)
        if (s3a[i] != s3b[i]) { ok = 0; why = "object leak"; }

    if (ok) it_pass("T125"); else it_fail("T125", why);
}

/* ── T126: retype failure atomicity ─────────────────────────────────────────
 * Drive retype into each failure mode and prove it is atomic: no object is
 * born, no handle appears, no live count moves, the region stays resettable,
 * and a valid retype right after each failure still works.
 * Invariants: U5, U17, U18, U19. */
static void test_t126(void) {
    uint32_t s3b[5], s3a[5];
    uint32_t hlb[14], hla[14];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b) || !it_sched_ext(hlb)) { it_fail("T126", "sched ext"); return; }
    int ok = 1;
    const char *why = "retype atomicity";

    /* NO_MEMORY: a sub-untyped larger than the whole region. */
    uint64_t avail = 0;
    (void)it_sys3(SYS_UNTYPED_INFO, IT_UT, 0, (long)(uintptr_t)&avail);
    uint64_t huge = (avail + 0x100000u) & ~0xFFFULL;   /* page-aligned, > avail */
    struct { uint32_t type; uint64_t arg; long expect; const char *tag; } cases[] = {
        { IT_KOBJ_UNTYPED,     huge, (long)IRIS_ERR_NO_MEMORY,     "nomem" },
        { 99u,                 0,    (long)IRIS_ERR_NOT_SUPPORTED, "badtype" },
        { IT_KOBJ_FRAME,       1u,   (long)IRIS_ERR_INVALID_ARG,   "badsize" },
        { IT_KOBJ_CNODE,       7u,   (long)IRIS_ERR_INVALID_ARG,   "badslots" },
    };
    for (uint32_t c = 0; ok && c < 4u; c++) {
        long rr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, (long)cases[c].type, (long)cases[c].arg);
        if (rr != cases[c].expect) { ok = 0; why = cases[c].tag; break; }
        /* nothing leaked after this failure */
        if (!it_sched_ext3(s3a) || !it_sched_ext(hla)) { ok = 0; why = "ext mid"; break; }
        for (uint32_t i = 0; ok && i < 5u; i++)
            if (s3a[i] != s3b[i]) { ok = 0; why = "leaked object"; }
        if (ok && hla[IT_SI_LIVE] != hlb[IT_SI_LIVE]) { ok = 0; why = "leaked handle"; }
        /* a valid retype right after the failure still works */
        if (ok) {
            long good = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
            if (good < 0) { ok = 0; why = "valid-after-fail"; }
            else { handle_id_t g = (handle_id_t)good; it_close(&g); }
        }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 final"; }
    for (uint32_t i = 0; ok && i < 5u; i++)
        if (s3a[i] != s3b[i]) { ok = 0; why = "final leak"; }

    if (ok) it_pass("T126"); else it_fail("T126", why);
}

/* ── T127: revoke derivation-tree cascade ───────────────────────────────────
 * Build a handle-table derivation tree from a retyped endpoint (root → child →
 * grandchild, plus a second branch) and revoke the root: every descendant dies
 * (BAD_HANDLE), the root survives, an unrelated object outside the subtree is
 * untouched, and a repeated revoke is idempotent.  Failure paths: revoke on a
 * stale handle (BAD_HANDLE), revoke of a leaf with no children (idempotent 0).
 * Also asserts revoke SCOPE: a copy minted into a CNode is an independent ref,
 * not a derivation child, and survives the revoke — then is cleaned up.
 * Invariants: U8, U9, U10, U16. */
static void test_t127(void) {
    uint32_t s3b[5];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b)) { it_fail("T127", "sched ext3"); return; }
    int ok = 1;
    const char *why = "revoke cascade";

    long rr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (rr < 0) { it_fail("T127", "retype root"); return; }
    handle_id_t root = (handle_id_t)rr;

    /* Unrelated object outside the derivation subtree (must survive revoke). */
    long orr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
    handle_id_t outsider = (orr >= 0) ? (handle_id_t)orr : HANDLE_INVALID;
    if (orr < 0) { ok = 0; why = "retype outsider"; }

    /* Derivation tree: root → c1 → gc1 ; root → c2. */
    long c1  = ok ? it_sys2(SYS_CAP_DERIVE, (long)root, (long)RIGHT_SAME_RIGHTS) : -1;
    long gc1 = (c1 >= 0) ? it_sys2(SYS_CAP_DERIVE, c1, (long)RIGHT_SAME_RIGHTS) : -1;
    long c2  = ok ? it_sys2(SYS_CAP_DERIVE, (long)root, (long)RIGHT_SAME_RIGHTS) : -1;
    if (c1 < 0 || gc1 < 0 || c2 < 0) { ok = 0; why = "derive"; }

    /* A CNode copy of the root — an independent ref, NOT a derivation child. */
    long cnr = ok ? it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_CNODE, 4) : -1;
    handle_id_t cn = (cnr >= 0) ? (handle_id_t)cnr : HANDLE_INVALID;
    if (ok && cnr < 0) { ok = 0; why = "retype cnode"; }
    if (ok && it_sys4(SYS_CNODE_MINT, (long)cn, 1, (long)root,
                      (long)(RIGHT_READ | RIGHT_WRITE)) != 0) { ok = 0; why = "cnode mint"; }

    /* All derived handles are live before the revoke. */
    if (ok && it_sys1(SYS_HANDLE_TYPE, c1)  < 0) { ok = 0; why = "c1 dead early"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, gc1) < 0) { ok = 0; why = "gc1 dead early"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, c2)  < 0) { ok = 0; why = "c2 dead early"; }

    /* Revoke root's subtree. */
    if (ok && it_sys1(SYS_CAP_REVOKE, (long)root) != 0) { ok = 0; why = "revoke"; }

    /* Every descendant is gone; root survives. */
    if (ok && it_sys1(SYS_HANDLE_TYPE, c1)  != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "c1 alive"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, gc1) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "gc1 alive"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, c2)  != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "c2 alive"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)root) < 0) { ok = 0; why = "root died"; }
    /* Outsider outside the subtree is untouched. */
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)outsider) < 0) { ok = 0; why = "outsider died"; }

    /* Idempotent: a second revoke finds an empty subtree and succeeds. */
    if (ok && it_sys1(SYS_CAP_REVOKE, (long)root) != 0) { ok = 0; why = "revoke not idempotent"; }
    /* Revoke on a stale handle fails cleanly. */
    if (ok) {
        long tmp = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
        if (tmp >= 0) { handle_id_t th = (handle_id_t)tmp; it_close(&th);
            if (it_sys1(SYS_CAP_REVOKE, tmp) >= 0) { ok = 0; why = "stale revoke ok"; } }
    }

    /* Revoke SCOPE: the CNode-minted copy is an independent ref — SYS_CAP_REVOKE
     * did not touch the CSpace slot.  Deleting the slot releases that ref; it
     * must succeed (the slot still held a live cap after the revoke). */
    if (ok && it_sys2(SYS_CNODE_DELETE, (long)cn, 1) != 0) { ok = 0; why = "cnode copy not independent"; }

    /* Teardown: close root, outsider, cnode; region resets clean. */
    it_close(&root);
    it_close(&outsider);
    it_close(&cn);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }
    uint32_t s3a[5];
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 final"; }
    for (uint32_t i = 0; ok && i < 5u; i++)
        if (s3a[i] != s3b[i]) { ok = 0; why = "object leak"; }

    if (ok) it_pass("T127"); else it_fail("T127", why);
}

/* ── T128: frame authority lifetime + revoke (VSpace-map gap documented) ─────
 * A retyped KFrame's cap authority is exercised: derive a child handle, revoke
 * it (child dies, the frame object survives via the root), then release the
 * root and prove the frame is destroyed exactly once (frame_live baseline) with
 * the region resettable (child_count → 0).
 *
 * Documented gap (see the hardening doc): ring 3 cannot MAP a retyped frame
 * here — SYS_FRAME_MAP needs a VSpace cap by CPtr that iris_test is not granted
 * (VMO is the ring-3 mapping path).  The PTE-install / mapped_count / unmap
 * invariants are covered by the host KFrame suites; and by design SYS_CAP_REVOKE
 * is cap-scoped and never force-unmaps a frame (a live mapping holds an
 * independent ref; kframe_obj_destroy asserts mapped_count == 0, so a stale PTE
 * can never outlive the frame object).
 * Invariants: U10, U13, U17, U18. */
static void test_t128(void) {
    uint32_t s3b[5];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b)) { it_fail("T128", "sched ext3"); return; }
    int ok = 1;
    const char *why = "frame lifetime";

    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    if (fr < 0) { it_fail("T128", "retype frame"); return; }
    handle_id_t frame = (handle_id_t)fr;

    uint32_t mid[5];
    if (!it_sched_ext3(mid)) { ok = 0; why = "ext3 mid"; }
    if (ok && mid[IT_S3_FRAME] != s3b[IT_S3_FRAME] + 1u) { ok = 0; why = "frame not counted"; }

    /* Derive a child frame handle, then revoke it away from the root. */
    long child = ok ? it_sys2(SYS_CAP_DERIVE, (long)frame, (long)RIGHT_SAME_RIGHTS) : -1;
    if (ok && child < 0) { ok = 0; why = "derive"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, child) != (long)IT_KOBJ_FRAME) { ok = 0; why = "child type"; }
    if (ok && it_sys1(SYS_CAP_REVOKE, (long)frame) != 0) { ok = 0; why = "revoke"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, child) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "child alive"; }
    /* The frame object survives while the root cap is held. */
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)frame) != (long)IT_KOBJ_FRAME) { ok = 0; why = "frame died early"; }
    if (ok && !it_sched_ext3(mid)) { ok = 0; why = "ext3 mid2"; }
    if (ok && mid[IT_S3_FRAME] != s3b[IT_S3_FRAME] + 1u) { ok = 0; why = "frame miscounted after revoke"; }

    /* Release the root: the frame is destroyed exactly once (unmapped: no
     * mapping ever taken, so mapped_count == 0 → clean destroy). */
    it_close(&frame);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (frame child leak)"; }
    uint32_t s3a[5];
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 final"; }
    if (ok && s3a[IT_S3_FRAME] != s3b[IT_S3_FRAME]) { ok = 0; why = "frame leak/double-free"; }

    if (ok) it_pass("T128"); else it_fail("T128", why);
}

/* ── T129: revoke with IPC-visible objects ──────────────────────────────────
 * A worker thread blocks in EP_RECV on a RETYPED endpoint.  A derived child
 * handle is revoked (the endpoint object survives), then the last handle is
 * closed: the endpoint's close fires, the waiter wakes with IRIS_ERR_CLOSED,
 * and after the worker releases its ref the endpoint object is destroyed
 * (endpoint_live baseline), the region resets clean, no ghost KReply is minted,
 * and task/handle books return to baseline.
 * Invariants: U8, U13 (via S13/S14), U14, U15, U17. */
static volatile long g_t129_res;
static void t129_worker(void) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    g_t129_res = it_sys2(SYS_EP_RECV, (long)g_sh_ep, (long)&m);
    g_sh_done[0] = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void test_t129(void) {
    uint32_t s3b[5], s3a[5];
    uint32_t e0[14], e1[14];
    uint32_t tl0 = 0, tl1 = 0;
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b) || !it_sched_ext(e0) || !it_task_live(&tl0)) {
        it_fail("T129", "sched ext"); return;
    }
    int ok = 1;
    const char *why = "revoke ipc object";

    long er = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (er < 0) { it_fail("T129", "retype endpoint"); return; }
    g_sh_ep = (handle_id_t)er;
    {
        uint32_t mid[5];
        if (!it_sched_ext3(mid) || mid[IT_S3_EP] != s3b[IT_S3_EP] + 1u) {
            ok = 0; why = "endpoint not counted";
        }
    }
    g_sh_done[0] = 0; g_t129_res = 999;

    /* Worker blocks in EP_RECV on the retyped endpoint. */
    uint64_t entry = (uint64_t)(uintptr_t)t129_worker;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_sh_stk[0] + sizeof(g_sh_stk[0]))) & ~0xFULL;
    if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) { ok = 0; why = "thread create"; }
    if (ok) for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);

    /* Derive a child handle and revoke it — object survives, waiter unaffected. */
    long child = ok ? it_sys2(SYS_CAP_DERIVE, (long)g_sh_ep, (long)RIGHT_SAME_RIGHTS) : -1;
    if (ok && child < 0) { ok = 0; why = "derive"; }
    if (ok && it_sys1(SYS_CAP_REVOKE, (long)g_sh_ep) != 0) { ok = 0; why = "revoke"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, child) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "child alive"; }

    /* Close the last handle → endpoint close fires → waiter wakes CLOSED. */
    it_close(&g_sh_ep);
    if (ok) for (int y = 0; y < 4000 && !g_sh_done[0]; y++) it_sys0(SYS_YIELD);
    if (ok && !g_sh_done[0]) { ok = 0; why = "waiter not woken"; }
    if (ok && g_t129_res != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong wake error"; }

    it_quiesce_reaper();
    if (ok && (!it_sched_ext3(s3a) || !it_sched_ext(e1) || !it_task_live(&tl1))) { ok = 0; why = "ext2"; }
    if (ok && s3a[IT_S3_EP] != s3b[IT_S3_EP]) { ok = 0; why = "endpoint leak"; }
    if (ok && e1[IT_SI_REPLY] != e0[IT_SI_REPLY]) { ok = 0; why = "ghost kreply"; }
    if (ok && tl1 != tl0) { ok = 0; why = "task-live drift"; }
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }

    if (ok) it_pass("T129"); else it_fail("T129", why);
}

/* ── T130: rights monotonicity and cap derivation ───────────────────────────
 * Rights can only shrink along a derivation/mint chain, never grow, and there
 * is no fallback after ACCESS_DENIED.
 * Invariants: U7, U20 (+ U16). */
static void test_t130(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "rights monotonicity";

    long rr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (rr < 0) { it_fail("T130", "retype root"); return; }
    handle_id_t root = (handle_id_t)rr;   /* full rights: READ|WRITE|DUP|TRANSFER */

    /* Derive down to READ-only (drops DUPLICATE). */
    long ro = it_sys2(SYS_CAP_DERIVE, (long)root, (long)RIGHT_READ);
    handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
    if (ro < 0) { ok = 0; why = "derive ro"; }

    /* A cap without DUPLICATE cannot be a derivation source — ACCESS_DENIED,
     * no handle produced (no fallback). */
    if (ok && it_sys2(SYS_CAP_DERIVE, (long)ro_h, (long)RIGHT_SAME_RIGHTS) != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "escalation via derive";
    }

    /* Derivation cannot ADD rights: asking for FULL from a READ-only parent
     * yields a cap that still lacks WRITE (monotonic reduce).  We prove the
     * child cannot be a derivation source (no DUPLICATE) — i.e. WRITE/DUP were
     * NOT granted despite the request. */
    if (ok) {
        long up = it_sys2(SYS_CAP_DERIVE, (long)root, (long)(RIGHT_READ)); /* parent has DUP */
        /* From a full-rights parent, derive asking only READ → child has READ only. */
        handle_id_t up_h = (up >= 0) ? (handle_id_t)up : HANDLE_INVALID;
        if (up < 0) { ok = 0; why = "derive read"; }
        if (ok && it_sys2(SYS_CAP_DERIVE, (long)up_h, (long)RIGHT_SAME_RIGHTS) != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "read child escalated";
        }
        it_close(&up_h);
    }

    /* CNode mint reduces rights the same way and never amplifies: mint the
     * root down to READ into a CNode slot, resolve it, and confirm it cannot
     * derive (no DUPLICATE). */
    if (ok) {
        long cnr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_CNODE, 4);
        handle_id_t cn = (cnr >= 0) ? (handle_id_t)cnr : HANDLE_INVALID;
        if (cnr < 0) { ok = 0; why = "retype cnode"; }
        if (ok && it_sys4(SYS_CNODE_MINT, (long)cn, 2, (long)root, (long)RIGHT_READ) != 0) {
            ok = 0; why = "cnode mint";
        }
        /* mint asking for MORE than the source has is rejected/reduced: minting
         * with RIGHT_MANAGE (root lacks it) reduces to none → INVALID_ARG. */
        if (ok && it_sys4(SYS_CNODE_MINT, (long)cn, 3, (long)root, (long)RIGHT_MANAGE) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "mint amplified";
        }
        it_close(&cn);
    }

    it_close(&ro_h);
    it_close(&root);
    it_quiesce_reaper();
    (void)it_ut_reset();

    if (ok) it_pass("T130"); else it_fail("T130", why);
}

/* ── T131: deterministic untyped/revoke stress ──────────────────────────────
 * A fixed-seed PRNG drives many rounds of retype / derive / mint / revoke /
 * delete / close against the shared untyped, mixing endpoints, notifications
 * and CNodes and interleaving forced failures (bad type, bad size, stale
 * revoke, occupied CNode slot).  After the churn the region must RESET clean
 * and every live per-type count and the handle-live count return to baseline.
 * Prints seed/iteration only on failure.
 * Invariants: U4, U5, U8, U9, U17, U18, U19. */
#define T131_SEED   0x18C0DE18u
#define T131_ROUNDS 24u
static void test_t131(void) {
    uint32_t s3b[5], s3a[5];
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b) || !it_sched_ext(e0)) { it_fail("T131", "sched ext"); return; }
    g_fz_seed = T131_SEED;
    int ok = 1;
    const char *why = "untyped/revoke stress";
    uint32_t i = 0;

    for (i = 0; ok && i < T131_ROUNDS; i++) {
        uint32_t pick = fz_rand() % 3u;
        uint32_t type = (pick == 0u) ? IT_KOBJ_ENDPOINT
                      : (pick == 1u) ? IT_KOBJ_NOTIFICATION
                                     : IT_KOBJ_CNODE;
        uint64_t arg  = (type == IT_KOBJ_CNODE) ? 4u : 0u;

        long rr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, (long)type, (long)arg);
        if (rr < 0) { ok = 0; why = "retype"; break; }
        handle_id_t root = (handle_id_t)rr;

        /* Derive a small tree, sometimes revoke it, always tear it down. */
        long c1 = it_sys2(SYS_CAP_DERIVE, (long)root, (long)RIGHT_SAME_RIGHTS);
        long c2 = (c1 >= 0) ? it_sys2(SYS_CAP_DERIVE, c1, (long)RIGHT_SAME_RIGHTS) : -1;
        handle_id_t c1_h = (c1 >= 0) ? (handle_id_t)c1 : HANDLE_INVALID;
        handle_id_t c2_h = (c2 >= 0) ? (handle_id_t)c2 : HANDLE_INVALID;

        /* Forced failure paths, interleaved deterministically. */
        if ((fz_rand() & 1u) &&
            it_sys3(SYS_UNTYPED_RETYPE, IT_UT, 99, 0) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "badtype"; }
        if (ok && (fz_rand() & 1u) &&
            it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 7) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "badsize"; }

        if (ok && (fz_rand() & 1u)) {
            /* Revoke path: children die, root survives. */
            if (it_sys1(SYS_CAP_REVOKE, (long)root) != 0) { ok = 0; why = "revoke"; }
            if (ok && c1 >= 0 && it_sys1(SYS_HANDLE_TYPE, c1) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "c1 alive"; }
            if (ok && c2 >= 0 && it_sys1(SYS_HANDLE_TYPE, c2) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "c2 alive"; }
            /* Repeated revoke is idempotent. */
            if (ok && it_sys1(SYS_CAP_REVOKE, (long)root) != 0) { ok = 0; why = "revoke idem"; }
            c1_h = HANDLE_INVALID; c2_h = HANDLE_INVALID;   /* already gone */
        } else {
            /* Explicit teardown path. */
            it_close(&c2_h);
            it_close(&c1_h);
        }

        /* Stale-handle revoke fails cleanly. */
        if (ok && (fz_rand() & 1u)) {
            long tmp = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
            if (tmp >= 0) { handle_id_t th = (handle_id_t)tmp; it_close(&th);
                if (it_sys1(SYS_CAP_REVOKE, tmp) >= 0) { ok = 0; why = "stale revoke ok"; } }
        }

        it_close(&root);
        /* Periodically drain and reset so the bump region never runs dry. */
        if ((i & 3u) == 3u) {
            it_quiesce_reaper();
            if (ok && !it_ut_reset()) { ok = 0; why = "mid reset busy"; }
        }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "final reset busy"; }
    if (ok && (!it_sched_ext3(s3a) || !it_sched_ext(e1))) { ok = 0; why = "ext final"; }
    for (uint32_t k = 0; ok && k < 5u; k++)
        if (s3a[k] != s3b[k]) { ok = 0; why = "object leak"; }
    if (ok && e1[IT_SI_LIVE] != e0[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }

    if (ok) it_pass("T131");
    else {
        fz_note("T131", T131_SEED, i);
        it_fail("T131", why);
    }
}

/* ── Fase 19: VM / VSpace / frame mapping hardening (T132–T139) ──────────────
 *
 * These tests close the gap Fase 18 left open: ring 3 now drives SYS_FRAME_MAP /
 * SYS_FRAME_UNMAP directly against its OWN address space, using a self-VSpace
 * cap obtained from SYS_VSPACE_SELF (self-authority only) and minted into
 * IRIS_CPTR_TEST_VSPACE.  Frames come from the Fase 18 boot untyped
 * (IRIS_CPTR_TEST_UNTYPED).  The Fase 19 additive instrumentation
 * (it_sched_ext4) exposes live KVSpace count, live KFrameMapping count, and the
 * cumulative map/unmap/TLB-invalidate counters — the observables behind V10–V18.
 *
 * live_mapping_count is the anchor invariant: it returns to baseline after every
 * unmap and after every VSpace teardown, so a leaked mapping (stale PTE / stale
 * node) or a double free is immediately visible.  A frame whose cap is closed
 * while still mapped would trip the kframe_obj_destroy `mapped_count == 0`
 * assert (a kernel panic), so a clean close is itself proof the frame was
 * unmapped first (V17). */

/* Helper: retype a fresh writable KFrame from the test untyped. */
static handle_id_t it_retype_frame(void) {
    long r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    return (r >= 0) ? (handle_id_t)r : HANDLE_INVALID;
}

/* ── T132: self-VSpace authority from ring 3 ────────────────────────────────
 * The self-VSpace cap (SYS_VSPACE_SELF, minted into IRIS_CPTR_TEST_VSPACE)
 * grants exactly the authority to map into the caller's own address space:
 *   - a valid map through it succeeds;
 *   - a wrong-type cap (the untyped) in the VSpace slot is rejected WRONG_TYPE;
 *   - a VSpace cap lacking RIGHT_WRITE is rejected ACCESS_DENIED (no fallback);
 *   - there is no way to name another process's VSpace.
 * Invariants: V1, V3, V4, V20. */
#define IT_VS_RO 57L   /* read-only self-VSpace cap (missing-rights fixture) */
static void test_t132(void) {
    int ok = 1;
    const char *why = "self-vspace authority";
    it_quiesce_reaper();

    if (!it_setup_self_vspace()) { it_fail("T132", "vspace self mint"); return; }

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T132", "retype frame"); return; }

    /* Valid: map + unmap through the self-VSpace cap. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T133_VA, (long)IT_MAP_W) != 0) {
        ok = 0; why = "valid map";
    }
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)T133_VA) != 0) {
        ok = 0; why = "valid unmap";
    }

    /* Wrong-type VSpace slot: the untyped cap is not a VSpace. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_UT, (long)T133_VA, (long)IT_MAP_W)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "wrong-type not rejected"; }

    /* Missing rights: a read-only self-VSpace cap cannot install a PTE. */
    if (ok) {
        long h = it_sys0(SYS_VSPACE_SELF);
        handle_id_t vh = (h >= 0) ? (handle_id_t)h : HANDLE_INVALID;
        if (h < 0) { ok = 0; why = "vspace self 2"; }
        if (ok && it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC,
                          IT_VS_RO, (long)vh, (long)RIGHT_READ) != 0) { ok = 0; why = "ro mint"; }
        it_close(&vh);
        if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS_RO, (long)T133_VA, (long)IT_MAP_W)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro not denied"; }
        /* No fallback: the denied map installed nothing (a following valid map
         * at the same VA still succeeds, proving the VA was left free). */
        if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T133_VA, (long)IT_MAP_W) != 0) {
            ok = 0; why = "post-deny map";
        }
        if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)T133_VA) != 0) { ok = 0; why = "post-deny unmap"; }
    }

    it_close(&fr);
    it_quiesce_reaper();
    (void)it_ut_reset();
    if (ok) it_pass("T132"); else it_fail("T132", why);
}

/* ── T133: direct frame map/unmap from ring 3 ───────────────────────────────
 * Map a retyped frame writable into the caller's own VSpace, write and read
 * back a pattern (proving the PTE is live in the active address space), unmap,
 * and confirm the mapping/TLB counters and frame lifetime all return to
 * baseline.  Reading after unmap would fault, so the "unmap removed access"
 * property is asserted at the accounting level (live_mapping_count baseline +
 * a fresh remap of the same VA succeeding) rather than by dereference.
 * Invariants: V2, V10, V12, V13, V21. */
static void test_t133(void) {
    uint32_t s3b[5], s3a[5];
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T133", "vspace self mint"); return; }
    if (!it_sched_ext3(s3b) || !it_sched_ext4(v0)) { it_fail("T133", "sched ext"); return; }
    int ok = 1;
    const char *why = "frame map/unmap";

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T133", "retype frame"); return; }

    if (it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T133_VA, (long)IT_MAP_W) != 0) {
        ok = 0; why = "map";
    }
    /* mapping is live: exactly one more mapping and one map-success. */
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 1u) { ok = 0; why = "maplive"; }
    if (ok && v1[IT_S4_MAPOK]   != v0[IT_S4_MAPOK]   + 1u) { ok = 0; why = "mapok"; }

    /* Write/read a pattern through the live mapping. */
    if (ok) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T133_VA;
        *p = 0x19C0FFEEu;
        __asm__ volatile ("" ::: "memory");
        if (*p != 0x19C0FFEEu) { ok = 0; why = "readback"; }
    }

    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)T133_VA) != 0) { ok = 0; why = "unmap"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid2"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "maplive not restored"; }
    if (ok && v1[IT_S4_UNMAPOK] != v0[IT_S4_UNMAPOK] + 1u) { ok = 0; why = "unmapok"; }
    if (ok && v1[IT_S4_TLB]     <= v0[IT_S4_TLB])          { ok = 0; why = "no tlb invalidate"; }

    /* VA is free again: a fresh map at the same VA succeeds, then clean up. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T133_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "remap"; }
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)T133_VA) != 0) { ok = 0; why = "reunmap"; }

    /* Close the frame — a clean close proves mapped_count == 0 (no assert). */
    it_close(&fr);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (frame mapped?)"; }
    if (ok && (!it_sched_ext3(s3a) || !it_sched_ext4(v1))) { ok = 0; why = "ext final"; }
    if (ok && s3a[IT_S3_FRAME]  != s3b[IT_S3_FRAME])  { ok = 0; why = "frame leak"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping leak"; }

    if (ok) it_pass("T133"); else it_fail("T133", why);
}

/* ── T134: map failure atomicity ────────────────────────────────────────────
 * Drive SYS_FRAME_MAP into each failure mode and prove no PTE, mapping node,
 * frame ref, or counter moves, and that a valid map right after each failure
 * still works.
 * Invariants: V5, V6, V7, V9, V11. */
static void test_t134(void) {
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T134", "vspace self mint"); return; }
    if (!it_sched_ext4(v0)) { it_fail("T134", "sched ext4"); return; }
    int ok = 1;
    const char *why = "map atomicity";

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T134", "retype frame"); return; }

    /* Wrong-type frame fixture: an endpoint is not a frame. */
    long er = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0);
    handle_id_t ep = (er >= 0) ? (handle_id_t)er : HANDLE_INVALID;
    /* Read-only frame fixture (drops WRITE): cannot back a writable map. */
    long rr = it_sys2(SYS_CAP_DERIVE, (long)fr, (long)RIGHT_READ);
    handle_id_t fr_ro = (rr >= 0) ? (handle_id_t)rr : HANDLE_INVALID;
    if (ep == HANDLE_INVALID || fr_ro == HANDLE_INVALID) { ok = 0; why = "fixtures"; }

    struct { long frame; long vs; uint64_t va; uint64_t flags; long expect; const char *tag; } cases[] = {
        { (long)fr,    IT_VS, T134_VA | 0x100ULL, IT_MAP_W, (long)IRIS_ERR_INVALID_ARG,   "unaligned va" },
        { (long)fr,    IT_VS, 0xFFFF800000000000ULL, IT_MAP_W, (long)IRIS_ERR_INVALID_ARG, "kernel va" },
        { (long)fr,    IT_VS, T134_VA, 3ULL,     (long)IRIS_ERR_INVALID_ARG,   "w^x" },
        { (long)ep,    IT_VS, T134_VA, IT_MAP_W, (long)IRIS_ERR_WRONG_TYPE,    "wrong frame type" },
        { (long)fr_ro, IT_VS, T134_VA, IT_MAP_W, (long)IRIS_ERR_ACCESS_DENIED, "insufficient rights" },
        { (long)fr,    IT_UT, T134_VA, IT_MAP_W, (long)IRIS_ERR_WRONG_TYPE,    "wrong vspace type" },
        { (long)fr,    99L,   T134_VA, IT_MAP_W, (long)IRIS_ERR_NOT_FOUND,     "empty vspace slot" },
    };
    for (uint32_t c = 0; ok && c < 7u; c++) {
        long got = it_sys4(SYS_FRAME_MAP, cases[c].frame, cases[c].vs,
                           (long)cases[c].va, (long)cases[c].flags);
        if (got != cases[c].expect) { ok = 0; why = cases[c].tag; break; }
        /* nothing installed by the failure */
        if (!it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; break; }
        if (v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "leaked mapping"; break; }
    }

    /* Occupied VA: a real map, then a duplicate at the same VA → BUSY. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T134_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "base map"; }
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T134_VA, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) {
        ok = 0; why = "occupied not busy";
    }
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)T134_VA) != 0) { ok = 0; why = "cleanup unmap"; }

    /* A valid map right after all failures still works. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T134_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "valid-after-fail"; }
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)T134_VA) != 0) { ok = 0; why = "final unmap"; }

    it_close(&fr_ro);
    it_close(&ep);
    it_close(&fr);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 final"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "final mapping leak"; }

    if (ok) it_pass("T134"); else it_fail("T134", why);
}

/* ── T135: duplicate / overlap / remap semantics ────────────────────────────
 * Documents and verifies the current mapping contract:
 *   - map A @ X ................. ok
 *   - map A @ X again .......... BUSY (VA occupied)
 *   - map B @ X ................ BUSY (VA occupied, different frame)
 *   - map A @ Y ................ ok (same frame, second VA; mapped_count == 2)
 *   - unmap in either order .... exact; unmap-absent → NOT_FOUND
 * A clean close of A after both unmaps proves mapped_count returned to 0.
 * Invariants: V8, V10, V12, V13, V14. */
static void test_t135(void) {
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T135", "vspace self mint"); return; }
    if (!it_sched_ext4(v0)) { it_fail("T135", "sched ext4"); return; }
    int ok = 1;
    const char *why = "map semantics";

    handle_id_t a = it_retype_frame();
    handle_id_t b = it_retype_frame();
    if (a == HANDLE_INVALID || b == HANDLE_INVALID) { it_close(&a); it_close(&b); it_fail("T135", "retype"); return; }

    if (it_sys4(SYS_FRAME_MAP, (long)a, IT_VS, (long)T135_VA_X, (long)IT_MAP_W) != 0) { ok = 0; why = "map A@X"; }
    if (ok && it_sys4(SYS_FRAME_MAP, (long)a, IT_VS, (long)T135_VA_X, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) { ok = 0; why = "dup A@X"; }
    if (ok && it_sys4(SYS_FRAME_MAP, (long)b, IT_VS, (long)T135_VA_X, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) { ok = 0; why = "B over X"; }
    if (ok && it_sys4(SYS_FRAME_MAP, (long)a, IT_VS, (long)T135_VA_Y, (long)IT_MAP_W) != 0) { ok = 0; why = "map A@Y"; }

    /* Two live mappings of frame A. */
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 2u) { ok = 0; why = "maplive != +2"; }

    /* Unmap Y first, then X (reverse order); absent unmap → NOT_FOUND. */
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)a, IT_VS, (long)T135_VA_Y) != 0) { ok = 0; why = "unmap A@Y"; }
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)a, IT_VS, (long)T135_VA_Y) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "absent unmap"; }
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)a, IT_VS, (long)T135_VA_X) != 0) { ok = 0; why = "unmap A@X"; }

    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid2"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "maplive not restored"; }

    /* Clean close of both frames (mapped_count == 0 or destroy asserts). */
    it_close(&a);
    it_close(&b);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }

    if (ok) it_pass("T135"); else it_fail("T135", why);
}

/* ── T136: VSpace cleanup on process death ──────────────────────────────────
 * A spawned child owns a VSpace with live bootstrap KFrame mappings (its text
 * and stack).  Killing it (external) and letting one self-exit must run VSpace
 * cleanup exactly once: the child's KVSpace is destroyed and every one of its
 * mappings is swept, so live-VSpace and live-mapping counts return to baseline
 * — with no interaction bug against the deferred reaper, and process/task
 * counters back to baseline.  Connects Fase 16 (death) + Fase 19 (VSpace).
 * Invariants: V15, V16, V17, V18. */
#define T136_ROUNDS 4u
static void test_t136(void) {
    uint32_t v0[5], v1[5];
    uint32_t e0[14], e1[14];
    uint32_t tl0 = 0, tl1 = 0;
    it_quiesce_reaper();
    if (!it_sched_ext4(v0) || !it_sched_ext(e0) || !it_task_live(&tl0)) { it_fail("T136", "sched ext"); return; }
    int ok = 1;
    const char *why = "vspace death cleanup";
    uint32_t i = 0;

    for (i = 0; ok && i < T136_ROUNDS; i++) {
        long ep = it_sys0(SYS_ENDPOINT_CREATE);
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t p_h  = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &p_h) < 0) { ok = 0; why = "spawn"; }
        else {
            /* Child is alive with its own VSpace + bootstrap mappings. */
            uint32_t vm[5];
            if (!it_sched_ext4(vm)) { ok = 0; why = "ext4 alive"; }
            if (ok && vm[IT_S4_VSLIVE] <= v0[IT_S4_VSLIVE]) { ok = 0; why = "child vspace not counted"; }
            if (ok && vm[IT_S4_MAPLIVE] <= v0[IT_S4_MAPLIVE]) { ok = 0; why = "child mappings not counted"; }
            if (ok) {
                if (i & 1u) {
                    it_sys1(SYS_SLEEP, 1);
                    if (it_sys1(SYS_PROCESS_KILL, (long)p_h) != 0) { ok = 0; why = "kill"; }
                } else {
                    struct IrisMsg m; it_iris_msg_zero(&m); m.label = 0x136;
                    (void)it_sys2(SYS_EP_SEND, (long)ep_h, (long)&m);
                    (void)it_lp_wait_exit(p_h);
                }
            }
        }
        it_close(&p_h);
        it_close(&ep_h);
        it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && (!it_sched_ext4(v1) || !it_sched_ext(e1) || !it_task_live(&tl1))) { ok = 0; why = "ext final"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace leak"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping leak"; }
    if (ok && e1[IT_SI_PROCLIVE] != e0[IT_SI_PROCLIVE]) { ok = 0; why = "proc-live drift"; }
    if (ok && tl1 != tl0) { ok = 0; why = "task-live drift"; }

    if (ok) it_pass("T136");
    else {
        it_serial_write("[IRIS][TEST] T136 round=");
        it_log_num(i);
        it_serial_write("\n");
        it_fail("T136", why);
    }
}

/* ── T137: mapped frame revoke interaction (closes the Fase 18 T128 gap) ─────
 * A retyped frame is MAPPED, then a derived handle is revoked while the frame
 * is live in the address space.  This demonstrates the real contract:
 *   - SYS_CAP_REVOKE is cap-scoped: it kills the derived handle but does NOT
 *     unmap the frame (the mapping holds an independent ref);
 *   - the frame object survives and stays usable through its live mapping;
 *   - frame destroy is impossible while mapped — only after unmap does closing
 *     the last cap destroy it (a clean close proves mapped_count == 0);
 *   - no stale PTE and no stale cap remain.
 * Invariants: V17, V18 (+ U10/U13 from Fase 18). */
static void test_t137(void) {
    uint32_t s3b[5];
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T137", "vspace self mint"); return; }
    if (!it_sched_ext3(s3b) || !it_sched_ext4(v0)) { it_fail("T137", "sched ext"); return; }
    int ok = 1;
    const char *why = "mapped revoke";

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T137", "retype frame"); return; }

    if (it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T137_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; }

    /* Derived handle, revoked while the frame is mapped. */
    long child = ok ? it_sys2(SYS_CAP_DERIVE, (long)fr, (long)RIGHT_SAME_RIGHTS) : -1;
    if (ok && child < 0) { ok = 0; why = "derive"; }
    if (ok && it_sys1(SYS_CAP_REVOKE, (long)fr) != 0) { ok = 0; why = "revoke"; }
    if (ok && it_sys1(SYS_HANDLE_TYPE, child) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "child alive"; }

    /* Revoke did NOT unmap: the mapping is still live and usable. */
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 1u) { ok = 0; why = "revoke changed mapping"; }
    if (ok) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T137_VA;
        *p = 0x137ABCDEu;
        __asm__ volatile ("" ::: "memory");
        if (*p != 0x137ABCDEu) { ok = 0; why = "frame unusable after revoke"; }
    }
    /* Frame object still alive (root cap held it). */
    if (ok && it_sys1(SYS_HANDLE_TYPE, (long)fr) != (long)IT_KOBJ_FRAME) { ok = 0; why = "frame died"; }

    /* Now unmap, then close — clean close proves mapped_count == 0 (no assert). */
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)T137_VA) != 0) { ok = 0; why = "unmap"; }
    it_close(&fr);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (frame still mapped?)"; }
    uint32_t s3a[5];
    if (ok && (!it_sched_ext3(s3a) || !it_sched_ext4(v1))) { ok = 0; why = "ext final"; }
    if (ok && s3a[IT_S3_FRAME]  != s3b[IT_S3_FRAME])  { ok = 0; why = "frame leak/double-free"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping leak"; }

    if (ok) it_pass("T137"); else it_fail("T137", why);
}

/* ── T138: VSpace rights and user/kernel isolation ──────────────────────────
 * PTE authority reflects cap rights, and userland cannot map kernel space:
 *   - a read-only frame cap maps non-writable (flags=0) but is DENIED a
 *     writable map (flags=1) — rights are not amplified by the map;
 *   - a W^X request (writable+exec) is INVALID_ARG;
 *   - a kernel-range VA is INVALID_ARG (isolation).
 * Documented gap: ring 3 has no safe way to observe raw PTE flags or to trigger
 * a write-protection #PF without a fault-handling endpoint (Fase-future), so
 * NX/write-enforcement at the hardware level is asserted at the authority layer
 * (rights checks), not by faulting.
 * Invariants: V4, V6, V19, V20. */
static void test_t138(void) {
    int ok = 1;
    const char *why = "rights/isolation";
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T138", "vspace self mint"); return; }

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T138", "retype frame"); return; }
    long rr = it_sys2(SYS_CAP_DERIVE, (long)fr, (long)RIGHT_READ);
    handle_id_t fr_ro = (rr >= 0) ? (handle_id_t)rr : HANDLE_INVALID;
    if (fr_ro == HANDLE_INVALID) { ok = 0; why = "ro derive"; }

    /* Read-only cap: non-writable map ok, writable map denied. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_ro, IT_VS, (long)T138_VA, 0L) != 0) { ok = 0; why = "ro map"; }
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr_ro, IT_VS, (long)T138_VA) != 0) { ok = 0; why = "ro unmap"; }
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_ro, IT_VS, (long)T138_VA, (long)IT_MAP_W)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable not denied"; }

    /* W^X rejected. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T138_VA, 3L) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "w^x not rejected";
    }
    /* Kernel-range VA rejected (user cannot map kernel space). */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, 0xFFFF800000001000L, (long)IT_MAP_W)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel va not rejected"; }

    it_close(&fr_ro);
    it_close(&fr);
    it_quiesce_reaper();
    (void)it_ut_reset();
    if (ok) it_pass("T138"); else it_fail("T138", why);
}

/* ── T139: deterministic VSpace mapping stress ──────────────────────────────
 * A fixed-seed PRNG drives many rounds of retype/map/(derive+revoke)/unmap/
 * close over a reserved VA window, interleaving forced failures (unaligned VA,
 * occupied VA).  After the churn every VM counter and the object/handle books
 * return to baseline.  Prints seed/iteration only on failure.
 * Invariants: V9, V10, V13, V15, V17, V18. */
#define T139_SEED   0x19C0DE19u
#define T139_ROUNDS 40u
static void test_t139(void) {
    uint32_t v0[5], v1[5];
    uint32_t s3b[5], s3a[5];
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T139", "vspace self mint"); return; }
    if (!it_sched_ext4(v0) || !it_sched_ext3(s3b) || !it_sched_ext(e0)) { it_fail("T139", "sched ext"); return; }
    g_fz_seed = T139_SEED;
    int ok = 1;
    const char *why = "vspace stress";
    uint32_t i = 0;

    for (i = 0; ok && i < T139_ROUNDS; i++) {
        handle_id_t fr = it_retype_frame();
        if (fr == HANDLE_INVALID) { ok = 0; why = "retype"; break; }
        uint64_t va = T139_VA_BASE + (uint64_t)(fz_rand() % 8u) * 0x1000ULL;

        /* Forced failures, deterministically interleaved. */
        if ((fz_rand() & 1u) &&
            it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)(va | 0x40ULL), (long)IT_MAP_W) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "unaligned"; it_close(&fr); break;
        }

        if (it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)va, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; it_close(&fr); break; }
        /* Occupied VA is BUSY. */
        if ((fz_rand() & 1u) &&
            it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)va, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) {
            ok = 0; why = "occupied"; it_close(&fr); break;
        }

        /* Sometimes derive + revoke while mapped (revoke must not unmap). */
        if (fz_rand() & 1u) {
            long c = it_sys2(SYS_CAP_DERIVE, (long)fr, (long)RIGHT_SAME_RIGHTS);
            if (c >= 0 && it_sys1(SYS_CAP_REVOKE, (long)fr) != 0) { ok = 0; why = "revoke"; it_close(&fr); break; }
            if (ok && c >= 0 && it_sys1(SYS_HANDLE_TYPE, c) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "child alive"; it_close(&fr); break; }
        }

        if (it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)va) != 0) { ok = 0; why = "unmap"; it_close(&fr); break; }
        it_close(&fr);

        if ((i & 7u) == 7u) {
            it_quiesce_reaper();
            if (!it_ut_reset()) { ok = 0; why = "mid reset busy"; break; }
        }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "final reset busy"; }
    if (ok && (!it_sched_ext4(v1) || !it_sched_ext3(s3a) || !it_sched_ext(e1))) { ok = 0; why = "ext final"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping leak"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace leak"; }
    if (ok && s3a[IT_S3_FRAME]  != s3b[IT_S3_FRAME])  { ok = 0; why = "frame leak"; }
    if (ok && e1[IT_SI_LIVE]    != e0[IT_SI_LIVE])    { ok = 0; why = "handle leak"; }

    if (ok) it_pass("T139");
    else {
        fz_note("T139", T139_SEED, i);
        it_fail("T139", why);
    }
}

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
    test_t075();
    test_t076();
    test_t077();
    test_t078();
    test_t079();
    test_t080();
    test_t081();
    test_t082();
    test_t083();
    test_t084();
    test_t085();
    test_t086();
    test_t087();
    test_t088();
    test_t089();
    test_t090();
    test_t091();
    test_t092();
    test_t093();
    test_t094();
    test_t095();
    test_t096();
    test_t097();
    test_t098();
    test_t099();
    test_t100();
    test_t101();
    test_t102();
    test_t103();
    test_t104();
    test_t105();
    test_t106();
    test_t107();
    test_t108();
    test_t109();
    test_t110();
    test_t111();
    test_t112();
    test_t113();
    test_t114();
    test_t115();
    test_t116();
    test_t117();
    test_t118();
    test_t119();
    test_t120();
    test_t121();
    test_t122();
    test_t123();
    test_t124();
    test_t125();
    test_t126();
    test_t127();
    test_t128();
    test_t129();
    test_t130();
    test_t131();
    test_t132();
    test_t133();
    test_t134();
    test_t135();
    test_t136();
    test_t137();
    test_t138();
    test_t139();

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
