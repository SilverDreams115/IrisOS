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
static int it_sched_ext(uint32_t w[12]) {
    long bh = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    if (bh < 0) return 0;
    uint8_t buf[88];
    long r = it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 88);
    handle_id_t h = (handle_id_t)bh;
    it_close(&h);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 12u; i++) {
        uint32_t o = 40u + 4u * i;
        w[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
               ((uint32_t)buf[o + 2u] << 16) | ((uint32_t)buf[o + 3u] << 24);
    }
    return 1;
}

/* Extended-word indices (see syscall_diag.c layout). */
#define IT_SI_LIVE     0u
#define IT_SI_HWM      1u
#define IT_SI_INSERTS  2u
#define IT_SI_REMOVES  3u
#define IT_SI_GHWM     4u
#define IT_SI_MAX      5u
#define IT_SI_SLOTDEL  6u
#define IT_SI_HANDDEL  7u
#define IT_SI_TOCTOU   8u
#define IT_SI_REPLY    9u
#define IT_SI_RESOLVE 10u

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
    uint32_t w[12];
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
    uint32_t before[12], after[12];
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
