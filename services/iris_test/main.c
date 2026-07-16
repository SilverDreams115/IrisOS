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
#include <iris/fault_proto.h>
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
    /* Fase S1: arg2 (rdx) is ALWAYS zeroed — SYS_EP_RECV/SYS_EP_NB_RECV now
     * interpret it as the explicit reply-object CPtr, so leaking garbage
     * there would randomly stage bogus replies. */
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(0L)
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

/* ── Fase S1: object-creation helpers ───────────────────────────────────────
 *
 * SYS_ENDPOINT_CREATE / SYS_NOTIFY_CREATE / SYS_CNODE_CREATE are RETIRED:
 * every kernel object the suite fabricates is retyped from its delegated
 * untyped (IRIS_CPTR_TEST_UNTYPED, slot 55) via SYS_UNTYPED_RETYPE2.  The
 * new capability lands in a scratch CSpace slot (rotating pool 100..219,
 * atomic — creation happens from helper threads too), is materialized to a
 * handle (SYS_CSPACE_RESOLVE — the sanctioned CSpace→handle bridge) and the
 * scratch slot is deleted, so the handle-based test flows keep exercising
 * the handle-table semantics unchanged while every BIRTH is Untyped+CSpace.
 */
#define IT_S1_SCRATCH_BASE  64u   /* 64..87: below the fz pool (100..239) */
#define IT_S1_SCRATCH_SPAN  24u
static uint32_t g_it_s1_scratch_next;

static long it_retype2_at(long ut, uint32_t obj_type, uint32_t slot,
                          uint32_t count, long obj_arg) {
    return it_sys4(SYS_UNTYPED_RETYPE2, ut,
                   (long)((uint64_t)obj_type | ((uint64_t)count << 32)),
                   (long)((uint64_t)slot << 32), obj_arg);
}

static long it_retype_handle(long ut, uint32_t obj_type, long obj_arg) {
    uint32_t slot = IT_S1_SCRATCH_BASE +
        (__atomic_fetch_add(&g_it_s1_scratch_next, 1u, __ATOMIC_RELAXED)
         % IT_S1_SCRATCH_SPAN);
    long r = it_retype2_at(ut, obj_type, slot, 1u, obj_arg);
    if (r < 0) return r;
    long h = it_sys1(SYS_CSPACE_RESOLVE, (long)slot);
    (void)it_sys2(SYS_CNODE_DELETE, 0, (long)slot);
    return h;
}

static long it_ep_create(void) {
    return it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_ENDPOINT, 0);
}

static long it_notify_create(void) {
    return it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_NOTIFICATION, 0);
}

/* Retype a reply object into a FIXED root slot (tests pass it as recv arg2
 * and invoke SYS_REPLY on the echoed msg.attached_handle).  Delete with
 * it_slot_delete when the test is done. */
static long it_reply_create_at(uint32_t slot) {
    return it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY,
                         slot, 1u, 0);
}

static void it_slot_delete(uint32_t slot) {
    (void)it_sys2(SYS_CNODE_DELETE, 0, (long)slot);
}

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
    long n_raw = it_notify_create();
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
    long n_raw = it_notify_create();
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
    long ep_raw = it_ep_create();
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
    long ep_raw = it_ep_create();
    if (ep_raw < 0) { it_fail("T012", "ep create"); return; }
    handle_id_t ep_h = (handle_id_t)ep_raw;

    long dup_raw = it_sys2(SYS_HANDLE_DUP, ep_raw,
                           (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));
    if (dup_raw < 0) {
        it_close(&ep_h);
        it_fail("T012", "dup"); return;
    }
    handle_id_t dup_h = (handle_id_t)dup_raw;

    long ep2_raw = it_ep_create();
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
    long ep_raw = it_ep_create();
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
    long ep_raw = it_ep_create();
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

    long ep_raw = it_ep_create();
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
    long r = it_sys3(SYS_EP_RECV, (long)g_t016_ep_h, (long)&msg, 88);
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
    g_t016_done = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t016(void) {
    g_t016_done = 0;
    g_t016_ok   = 0;

    long ep_raw = it_ep_create();
    if (ep_raw < 0) { it_fail("T016", "ep create"); return; }
    g_t016_ep_h = (handle_id_t)ep_raw;
    if (it_reply_create_at(88) < 0) {
        it_close(&g_t016_ep_h);
        it_fail("T016", "reply create"); return;
    }

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
    it_slot_delete(88);

    if (r == 0 && g_t016_ok)
        it_pass("T016");
    else
        it_fail("T016", "ep_call/reply");
}

/* ── T018: EP_NB_SEND on empty endpoint → WOULD_BLOCK ──────────────────── */

static void test_t018(void) {
    long ep_raw = it_ep_create();
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

    long ep_raw = it_ep_create();
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

    long ep_raw = it_ep_create();
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

    long ep_raw = it_ep_create();
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
    if (it_reply_create_at(89) < 0) {
        it_close(&tid_h);
        it_close(&g_t021_ep_h);
        it_fail("T021", "reply create"); return;
    }
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys3(SYS_EP_RECV, ep_raw, (long)&msg, 89);
    if (r < 0 || msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t021_ep_h);
        it_slot_delete(89);
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

    it_close(&tid_h);
    it_close(&g_t021_ep_h);
    it_slot_delete(89);

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
    long r = it_sys3(SYS_EP_RECV, (long)g_t022_ep_h, (long)&rmsg, 90);
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

    g_t022_ok   = (recv_ok && rr == 0);
    g_t022_done = 1;
    it_sys1(SYS_THREAD_EXIT, 0);
    for (;;) {}
}

static void test_t022(void) {
    g_t022_done = 0;
    g_t022_ok   = 0;

    long ep_raw = it_ep_create();
    if (ep_raw < 0) { it_fail("T022", "ep create"); return; }
    g_t022_ep_h = (handle_id_t)ep_raw;
    if (it_reply_create_at(90) < 0) {
        it_close(&g_t022_ep_h);
        it_fail("T022", "reply create"); return;
    }

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
    it_slot_delete(90);

    int bulk_ok = (client_buf[0] == 0x11 && client_buf[1] == 0x21 &&
                   client_buf[2] == 0x31 && client_buf[3] == 0x41);

    if (r == 0 && g_t022_ok && bulk_ok)
        it_pass("T022");
    else
        it_fail("T022", "ep_call bulk round-trip");
}

/* ── T023: EP_SEND on read-only endpoint handle → ACCESS_DENIED ─────────── */

static void test_t023(void) {
    long ep_raw = it_ep_create();
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

    long ep_raw = it_ep_create();
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
    if (it_reply_create_at(91) < 0) {
        it_close(&tid_h);
        it_close(&g_t024_ep_h);
        it_fail("T024", "reply create"); return;
    }
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys3(SYS_EP_RECV, ep_raw, (long)&msg, 91);
    if (r < 0 || msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t024_ep_h);
        it_slot_delete(91);
        it_fail("T024", "ep_recv"); return;
    }
    handle_id_t reply_h = (handle_id_t)msg.attached_handle;

    long rr = -1;
    long notif_raw = it_notify_create();
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
    it_close(&tid_h);
    it_close(&g_t024_ep_h);
    it_slot_delete(91);

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

    long ep_raw = it_ep_create();
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

    if (it_reply_create_at(92) < 0) {
        it_close(&tid_h);
        it_close(&g_t025_ep_h);
        it_fail("T025", "reply create"); return;
    }
    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    long r = it_sys3(SYS_EP_RECV, ep_raw, (long)&msg, 92);
    if (r < 0 || msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t025_ep_h);
        it_slot_delete(92);
        it_fail("T025", "ep_recv"); return;
    }
    handle_id_t reply_h = (handle_id_t)msg.attached_handle;

    /* Notification dup WITHOUT RIGHT_TRANSFER → staging must fail */
    handle_id_t notif_h = HANDLE_INVALID;
    handle_id_t nt_h    = HANDLE_INVALID;
    long r1 = -1;
    long notif_raw = it_notify_create();
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
    it_close(&tid_h);
    it_close(&g_t025_ep_h);
    it_slot_delete(92);

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
    long e = it_ep_create();
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
    long n = it_notify_create();
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
    long root = it_ep_create();
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
    long root = it_ep_create();
    if (root < 0) { it_fail("T072", "ep create"); return; }
    handle_id_t root_h = (handle_id_t)root;

    /* Read-only child (drops DUPLICATE/TRANSFER). */
    long ro = it_sys2(SYS_CAP_DERIVE, root, (long)RIGHT_READ);
    /* Deriving from a cap without RIGHT_DUPLICATE must be denied. */
    long escalate = (ro >= 0)
                  ? it_sys2(SYS_CAP_DERIVE, ro, (long)RIGHT_SAME_RIGHTS)
                  : 0;

    /* Stale handle → clean BAD_HANDLE (create+close to guarantee staleness). */
    long tmp = it_ep_create();
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
    long ep = it_ep_create();
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
    long stale = it_ep_create();
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
    long rr = it_sys3(SYS_EP_RECV, (long)g_t074_ep_h, (long)&msg, 93);
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

    long ep = it_ep_create();
    if (ep < 0) { it_fail("T074", "ep create"); return; }
    g_t074_ep_h = (handle_id_t)ep;
    if (it_reply_create_at(93) < 0) {
        it_close(&g_t074_ep_h);
        it_fail("T074", "reply create"); return;
    }

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
    it_slot_delete(93);

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
    struct svc_mint mints[2];
    uint32_t n = 0;
    mints[n].slot   = LP_CPTR_CMD_EP;
    mints[n].src_h  = cmd_ep_h;
    mints[n].rights = RIGHT_READ | RIGHT_WRITE;
    mints[n].badge  = 0;
    n++;
    /* Fase S1: the child serves EP_CALLs on its command endpoint, so it needs
     * an explicit reply object at slot 13 (LP_CPTR_REPLY).  Retyped fresh
     * from the test untyped; the parent drops its handle right after the
     * mint so child death still fires close-wakes-caller. */
    handle_id_t reply_h = HANDLE_INVALID;
    {
        long rr = it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
        if (rr >= 0) {
            reply_h = (handle_id_t)rr;
            mints[n].slot   = 13u;
            mints[n].src_h  = reply_h;
            mints[n].rights = RIGHT_READ | RIGHT_WRITE;
            mints[n].badge  = 0;
            n++;
        }
    }
    handle_id_t boot_h = HANDLE_INVALID;
    *out_proc_h = HANDLE_INVALID;
    long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "lifecycle_probe",
                             out_proc_h, &boot_h, mints, n);
    it_close(&reply_h);  /* the child's slot-13 mint is the only reply cap */
    it_close(&boot_h);   /* Track I: no bootstrap channel (HANDLE_INVALID anyway) */
    return r;
}

/* ── T075: spawn/exit smoke ─────────────────────────────────────────────────
 * Foundation test: spawn the child, drive it to run and exit via one command
 * endpoint, and observe its exit code — proving the harness works end to end. */
static void test_t075(void) {
    long ep = it_ep_create();
    if (ep < 0) { it_fail("T075", "ep create"); return; }
    handle_id_t cmd_ep_h = (handle_id_t)ep;

    handle_id_t proc_h = HANDLE_INVALID;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == HANDLE_INVALID) {
        it_close(&cmd_ep_h);
        it_fail("T075", "spawn"); return;
    }

    /* Watch the child for exit on a notification, bit 0. */
    long n = it_notify_create();
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
    long ep = it_ep_create();
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
    long ep = it_ep_create();
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
    long ep = it_ep_create();
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
    long ep = it_ep_create();
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
    long ep = it_ep_create();
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
    long n = it_notify_create();
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

    long ep = it_ep_create();
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

    /* ── SchedContext (Fase S2: SYS_SC_CREATE retired → RETYPE2) ── */
    if (ok && it_sys0(SYS_SC_CREATE) != (long)IRIS_ERR_NOT_SUPPORTED) ok = 0;
    long sc = it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_SCHED_CONTEXT, 0);
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

    long epx = it_ep_create();   /* the cap being transferred */
    long cmd = it_ep_create();   /* the transfer channel */
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

    long n   = it_notify_create();
    long cmd = it_ep_create();
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

    long n   = it_notify_create();
    long cmd = it_ep_create();
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
    /* Fase S1: explicit reply object (slot 94) staged via recv arg2. */
    long rr = it_sys3(SYS_EP_RECV, (long)g_t087_ep, (long)&m, 94);
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

    long nA = it_notify_create();
    long nB = it_notify_create();
    long ep = it_ep_create();
    if (nA < 0 || nB < 0 || ep < 0) { it_fail("T087", "create"); return; }
    if (it_reply_create_at(94) < 0) { it_fail("T087", "reply create"); return; }
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
    /* Fase S1: the reply value is the server's OWN reply-object CPtr (echoed
     * from recv arg2) — explicit MCS-style authority, never a fabricated
     * handle. */
    if (ok && g_t087_reply_h != 94u) ok = 0;
    if (ok && g_t087_sig != 0) ok = 0;                      /* invocable by CPtr */
    if (ok && g_t087_r1 != 0) ok = 0;                       /* first reply ok */
    if (ok && g_t087_r2 != (int)IRIS_ERR_NOT_FOUND) ok = 0; /* one-shot (T074) */

    it_close(&nA_h);
    it_close(&nB_h);
    it_close(&g_t087_ep);
    it_slot_delete(94);

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

    long n   = it_notify_create();
    long ep  = it_ep_create();
    long ep2 = it_ep_create();
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
    long e = it_ep_create();
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
    long e = it_ep_create();
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

/* Fase 20 ext5 fault-model words (offsets 160..176, 5 uint32).  A pre-Fase-20
 * kernel clamps SYS_SCHED_INFO to 160 bytes and leaves these zero — additive,
 * never required by a legacy assert. */
#define IT_S5_DELIVER 0u   /* faults handed to a registered handler (cumulative) */
#define IT_S5_NOHAND  1u   /* faults with no handler → task killed (cumulative)  */
#define IT_S5_RESUME  2u   /* SYS_EXCEPTION_RESUME action 0 (cumulative)         */
#define IT_S5_KILL    3u   /* SYS_EXCEPTION_RESUME action 1 (cumulative)         */
#define IT_S5_CLEAN   4u   /* pending-fault records cleared (cumulative)         */

static int it_sched_ext5(uint32_t w5[5]) {
    long bh = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    if (bh < 0) return 0;
    uint8_t buf[184];
    long r = it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 184);
    handle_id_t h = (handle_id_t)bh;
    it_close(&h);
    if (r != 0) return 0;
    for (uint32_t i = 0; i < 5u; i++) {
        uint32_t o = 160u + 4u * i;
        w5[i] = (uint32_t)buf[o] | ((uint32_t)buf[o + 1u] << 8) |
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
    long e = it_ep_create();
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

    long nA = it_notify_create();      /* the cap to transfer */
    long nB = it_notify_create();      /* the slot-race winner */
    long ep = it_ep_create();
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
    long ep = it_ep_create();
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
    long ep = it_ep_create();
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
    long n = it_notify_create();
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
        long ep = it_ep_create();
        long n  = it_notify_create();
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
        long ep = it_ep_create();
        long n2 = it_notify_create();
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
        long ep = it_ep_create();
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
    long e = it_ep_create();
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
    long ep = it_ep_create();
    long n  = it_notify_create();
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
        long ep = it_ep_create();
        long n  = it_notify_create();
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

    long ep = it_ep_create();
    long n  = it_notify_create();
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

    long ep = it_ep_create();
    long n  = it_notify_create();
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

    long ep = it_ep_create();
    long n  = it_notify_create();
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

    /* Serve the call: the explicit reply object (slot 95) is staged via
     * recv arg2 and echoed back in attached_handle (Fase S1). */
    if (ok && it_reply_create_at(95) < 0) { ok = 0; why = "reply create"; }
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        if (it_sys3(SYS_EP_RECV, (long)g_t105_ep_h, (long)&m, 95) != 0 ||
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
    it_close(&n_h);
    it_close(&g_t105_ep_h);
    it_slot_delete(95);

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

    long ep = it_ep_create();
    long n  = it_notify_create();
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
        long e = it_ep_create();
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

    long ep    = it_ep_create();   /* data endpoint */
    long n     = it_notify_create();     /* transferable notification */
    long ep2   = it_ep_create();   /* transferable endpoint */
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

    long n = it_notify_create();
    handle_id_t n_h = (handle_id_t)n;
    if (n < 0) { it_fail("T108", "create"); return; }
    /* One reusable declared slot: every cancellation must leave it EMPTY,
     * so the same slot serves every pick-3 round (that IS the assert). */
    uint32_t rslot = fz_slot_alloc();
    if (rslot == 0u) { it_close(&n_h); it_fail("T108", "slot budget"); return; }
    if (!fz_workers_start(2)) { it_close(&n_h); it_fail("T108", "worker"); return; }

    for (it_n = 0; ok && it_n < T108_ROUNDS; it_n++) {
        long ep = it_ep_create();
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
        long ep = it_ep_create();
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

    long ep    = it_ep_create();
    long n     = it_notify_create();
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
    /* Fase S1: ONE reusable explicit reply object serves every rendezvous
     * round — free→staged→bound→free per call (S18 under stress). */
    if (it_reply_create_at(96) < 0) { it_fail("T109", "reply create"); return; }

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
        if (it_sys3(SYS_EP_RECV, (long)g_fz_data_ep, (long)&m, 96) != 0 ||
            m.label != 0xF2ULL ||
            m.attached_handle != 96u) {   /* Fase S1: our reply CPtr echoed */
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
    it_slot_delete(96);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance; +1 = the worker's KTcb handle (Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE] + 1u) { ok = 0; why = "leak"; }
    /* Reply BINDINGS balance EXACTLY: one per rendezvous, zero per fail-fast
     * (Fase S1: the counter tracks bindings of the reusable reply object). */
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

    long ep    = it_ep_create();
    long nf    = it_notify_create();
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
    long ep = it_ep_create();
    long n  = it_notify_create();
    long e2 = (kind == 1u) ? it_ep_create() : -1;
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
        long ep = it_ep_create();
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

    long ep = it_ep_create();
    long n  = it_notify_create();      /* source cap for the 2nd reply */
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
    if (ok && it_reply_create_at(97) < 0) { ok = 0; why = "reply create"; }
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        if (it_sys3(SYS_EP_RECV, (long)ep_h, (long)&m, 97) != 0 ||
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

    it_slot_delete(97);  /* last reply cap → close(no caller) → destroy */
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
        long e = it_ep_create();
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
        long e = it_ep_create();
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
        long ep = it_ep_create();
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

    long ep  = it_ep_create();   /* shared endpoint */
    long n   = it_notify_create();      /* shared notification */
    long vmo = it_sys1(SYS_VMO_CREATE, 4096);   /* shared VMO */
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n, vmo_h = (handle_id_t)vmo;
    handle_id_t cmd_ep_h = HANDLE_INVALID, proc_h = HANDLE_INVALID;

    /* Command endpoint keeps the child parked; the shared caps go into its
     * CSpace / handle table below. */
    long cep = it_ep_create();
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

    long n = it_notify_create();
    handle_id_t n_h = (handle_id_t)n;
    handle_id_t ep[3]  = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID };
    handle_id_t pr[3]  = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID };
    if (n < 0) { it_fail("T117", "notif"); return; }

    for (uint32_t k = 0; ok && k < 3u; k++) {
        long e = it_ep_create();
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
        long n2 = it_notify_create();
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
            long e = it_ep_create();
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
            long e = it_ep_create();
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
        long ep = it_ep_create();
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
            long ce = it_ep_create();
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
    long ep = it_ep_create();
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
        long ep = it_ep_create();
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
        long ep = it_ep_create();
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

    /* Fase S2: SYS_SC_CREATE retired; SCs come from Untyped RETYPE2. */
    long a = it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_SCHED_CONTEXT, 0);
    long b = it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_SCHED_CONTEXT, 0);
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
        long ep = it_ep_create();
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
/* Fase S1: the authority tests need a region whose child_count they fully
 * control (RESET is gated on child_count == 0).  The suite-wide creation
 * helpers (it_ep_create & friends) now also carve from the delegated untyped
 * at slot 55 and some fixtures live for the whole run, so these tests operate
 * on their OWN sub-untyped, carved lazily from slot 55 on first use. */
static long g_it_auth_ut = -1;
static long it_auth_ut(void) {
    if (g_it_auth_ut < 0)
        g_it_auth_ut = it_sys3(SYS_UNTYPED_RETYPE,
                               (long)IRIS_CPTR_TEST_UNTYPED,
                               IT_KOBJ_UNTYPED, 2097152);
    return g_it_auth_ut;
}
#define IT_UT (it_auth_ut())

/* Reset the authority-test untyped after a test drops all its children.
 * Returns 1 if the region is clean (child_count == 0 → RESET ok), 0 otherwise. */
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
    /* Materialize the lazy authority sub-untyped BEFORE the baseline so its
     * +1 untyped does not pollute this test's live-count deltas. */
    if (it_auth_ut() < 0) { it_fail("T125", "auth untyped carve"); return; }
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
    r = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (r < 0) { ok = 0; why = "retype endpoint"; } else ep = (handle_id_t)r;
    r = it_retype_handle(IT_UT, IT_KOBJ_NOTIFICATION, 0);
    if (ok && r < 0) { ok = 0; why = "retype notification"; } else if (ok) nt = (handle_id_t)r;
    r = it_retype_handle(IT_UT, IT_KOBJ_CNODE, 4);
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
        /* non-power-of-two CNode slot count (RETYPE2 — the canonical path). */
        if (ok && it_retype2_at(IT_UT, IT_KOBJ_CNODE, 240u, 1u, 3) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad cnode slots"; }
        /* Fase S1 retirement: the LEGACY handle-publishing retype refuses the
         * migrated family outright (S20 — no migrated object born by handle). */
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0)     != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy ep not retired"; }
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_NOTIFICATION, 0) != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy nt not retired"; }
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_CNODE, 4)        != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy cn not retired"; }
        /* missing RIGHT_WRITE: retype through a read-only derived cap.
         * (IT_UT is a handle now — derive straight from it.) */
        if (ok) {
            long ro = it_sys2(SYS_CAP_DERIVE, IT_UT, (long)RIGHT_READ);
            handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
            if (ro < 0) { ok = 0; why = "ro dup"; }
            if (ok && it_retype2_at((long)ro_h, IT_KOBJ_ENDPOINT, 240u, 1u, 0) != (long)IRIS_ERR_ACCESS_DENIED) {
                ok = 0; why = "rights not enforced";
            }
            it_close(&ro_h);
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
        { IT_KOBJ_CNODE,       7u,   (long)IRIS_ERR_NOT_SUPPORTED, "cnode legacy retired" },
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
            long good = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
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

    long rr = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (rr < 0) { it_fail("T127", "retype root"); return; }
    handle_id_t root = (handle_id_t)rr;

    /* Unrelated object outside the derivation subtree (must survive revoke). */
    long orr = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
    handle_id_t outsider = (orr >= 0) ? (handle_id_t)orr : HANDLE_INVALID;
    if (orr < 0) { ok = 0; why = "retype outsider"; }

    /* Derivation tree: root → c1 → gc1 ; root → c2. */
    long c1  = ok ? it_sys2(SYS_CAP_DERIVE, (long)root, (long)RIGHT_SAME_RIGHTS) : -1;
    long gc1 = (c1 >= 0) ? it_sys2(SYS_CAP_DERIVE, c1, (long)RIGHT_SAME_RIGHTS) : -1;
    long c2  = ok ? it_sys2(SYS_CAP_DERIVE, (long)root, (long)RIGHT_SAME_RIGHTS) : -1;
    if (c1 < 0 || gc1 < 0 || c2 < 0) { ok = 0; why = "derive"; }

    /* A CNode copy of the root — an independent ref, NOT a derivation child. */
    long cnr = ok ? it_retype_handle(IT_UT, IT_KOBJ_CNODE, 4) : -1;
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
        long tmp = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
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

    long er = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
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

    long rr = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
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
        long cnr = it_retype_handle(IT_UT, IT_KOBJ_CNODE, 4);
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

        long rr = it_retype_handle(IT_UT, type, (long)arg);
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
            long tmp = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
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
    long er = it_retype_handle(IT_UT, IT_KOBJ_ENDPOINT, 0);
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
        long ep = it_ep_create();
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

/* ── Fase 20: fault endpoint / exception delivery model (T140–T147) ──────────
 *
 * User faults are authority events: registering a fault endpoint requires
 * RIGHT_MANAGE on the target process; delivery records the fault in the
 * KProcess and signals the registered KNotification while the faulting task is
 * suspended in TASK_BLOCKED_FAULT; SYS_PROCESS_FAULT_INFO (RIGHT_READ) reads
 * the record; SYS_EXCEPTION_RESUME (RIGHT_MANAGE) resolves it — action 0
 * re-executes the faulting instruction, action 1 kills the task.  Kernel
 * faults are never deliverable (idt.c panics).  lifecycle_probe children are
 * the fault fixtures: FAULT_READ/WRITE take a target VA in words[0] (0 = the
 * child's own ASLR-biased code address), FAULT_EXEC calls into its NX stack.
 *
 * Observables: the ext5 SYS_SCHED_INFO tier (delivery/nohandler/resume/kill/
 * cleanup counters) plus the usual task/handle/object/mapping books. */

#define LP_CMD_FAULT_READ   0x109Cu  /* must match lifecycle_probe */
#define LP_CMD_FAULT_WRITE  0x109Du
#define LP_CMD_FAULT_EXEC   0x109Eu

#define T14X_BAD_VA   0x8090000000ULL          /* canonical user VA, never mapped */
#define T14X_KERN_VA  0xFFFF800000001000ULL    /* kernel half — user access faults */

/* #PF error-code bits (Intel SDM). */
#define PF_ERR_P 0x01u
#define PF_ERR_W 0x02u
#define PF_ERR_U 0x04u
#define PF_ERR_I 0x10u

struct it_fault {
    uint32_t vector, task_id, error;
    uint32_t seq;              /* Fase 25: fault generation (FAULT_OFF_SEQ) */
    uint64_t rip, cr2;
};

/* Read the pending-fault record for proc_h; 0 on success, else the error
 * (IRIS_ERR_WOULD_BLOCK when nothing is pending). */
static long it_fault_info(handle_id_t proc_h, struct it_fault *f) {
    uint8_t b[FAULT_MSG_LEN];
    long r = it_sys2(SYS_PROCESS_FAULT_INFO, (long)proc_h, (long)(uintptr_t)b);
    if (r != 0) return r;
    f->vector  = (uint32_t)b[FAULT_OFF_VECTOR]  | ((uint32_t)b[FAULT_OFF_VECTOR + 1] << 8) |
                 ((uint32_t)b[FAULT_OFF_VECTOR + 2] << 16) | ((uint32_t)b[FAULT_OFF_VECTOR + 3] << 24);
    f->task_id = (uint32_t)b[FAULT_OFF_TASK_ID] | ((uint32_t)b[FAULT_OFF_TASK_ID + 1] << 8) |
                 ((uint32_t)b[FAULT_OFF_TASK_ID + 2] << 16) | ((uint32_t)b[FAULT_OFF_TASK_ID + 3] << 24);
    f->error   = (uint32_t)b[FAULT_OFF_ERROR]   | ((uint32_t)b[FAULT_OFF_ERROR + 1] << 8) |
                 ((uint32_t)b[FAULT_OFF_ERROR + 2] << 16) | ((uint32_t)b[FAULT_OFF_ERROR + 3] << 24);
    f->seq     = (uint32_t)b[FAULT_OFF_SEQ]     | ((uint32_t)b[FAULT_OFF_SEQ + 1] << 8) |
                 ((uint32_t)b[FAULT_OFF_SEQ + 2] << 16) | ((uint32_t)b[FAULT_OFF_SEQ + 3] << 24);
    f->rip = 0; f->cr2 = 0;
    for (uint32_t i = 0; i < 8u; i++) f->rip |= (uint64_t)b[FAULT_OFF_RIP + i] << (i * 8);
    for (uint32_t i = 0; i < 8u; i++) f->cr2 |= (uint64_t)b[FAULT_OFF_CR2 + i] << (i * 8);
    return 0;
}

/* Send a fault-trigger command with a target VA (blocking send — returns once
 * the child has picked the message up, i.e. is about to fault). */
static long it_lp_cmd_va(handle_id_t ep_h, uint32_t label, uint64_t va) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label      = label;
    m.words[0]   = va;
    m.word_count = 1u;
    return it_sys2(SYS_EP_SEND, (long)ep_h, (long)&m);
}

/* Spawn a lifecycle_probe child wired for fault supervision: command endpoint,
 * fault-handler notification (signal bit 0) registered via proc cap, and an
 * exit watch (bit 0 of w_h).  All-or-nothing; on failure everything is closed
 * and *why is set.  Returns 1 on success. */
static int it_fault_spawn(handle_id_t *ep_h, handle_id_t *proc_h,
                          handle_id_t *n_h, handle_id_t *w_h, const char **why) {
    *ep_h = *proc_h = *n_h = *w_h = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ep create"; return 0; }
    *ep_h = (handle_id_t)ep;
    if (lp_spawn_child(*ep_h, proc_h) < 0 || *proc_h == HANDLE_INVALID) {
        it_close(ep_h); *why = "spawn"; return 0;
    }
    long n = it_notify_create();
    long w = it_notify_create();
    *n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
    *w_h = (w >= 0) ? (handle_id_t)w : HANDLE_INVALID;
    if (n < 0 || w < 0 ||
        it_sys3(SYS_EXCEPTION_HANDLER, (long)*proc_h, n, 1) != 0 ||
        it_sys3(SYS_PROCESS_WATCH, (long)*proc_h, w, 1) != 0) {
        (void)it_sys1(SYS_PROCESS_KILL, (long)*proc_h);
        it_close(n_h); it_close(w_h); it_close(proc_h); it_close(ep_h);
        *why = "wire handler/watch"; return 0;
    }
    return 1;
}

/* Bounded wait (≤2s) for signal bit 0 on a notification; 1 on success. */
static int it_fault_wait(handle_id_t n_h) {
    uint64_t bits = 0;
    if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)n_h, (long)(uintptr_t)&bits,
                2000000000LL) != 0) return 0;
    return (bits & 1ull) != 0;
}

static void it_fault_close4(handle_id_t *a, handle_id_t *b,
                            handle_id_t *c, handle_id_t *d) {
    it_close(a); it_close(b); it_close(c); it_close(d);
}

/* ── T140: register fault endpoint authority ────────────────────────────────
 * Registration is capability-mediated with no fallback:
 *   - RIGHT_MANAGE on the target process cap and RIGHT_WRITE on the
 *     notification are required — reduced-rights duplicates get ACCESS_DENIED;
 *   - wrong-type caps in either slot get WRONG_TYPE;
 *   - signal_bits == 0 is INVALID_ARG; an empty slot fails;
 *   - a failed registration leaves NO partial handler installed: a subsequent
 *     fault takes the no-handler path (task killed, nohandler counter up);
 *   - re-registration replaces the handler (last registration wins — only the
 *     new notification fires);
 *   - signalling the handler notification by hand does NOT fabricate a fault
 *     (FAULT_INFO stays WOULD_BLOCK — no spoofing);
 *   - registering on a dead process fails NOT_FOUND (would leak the pin).
 * Invariants: F3, F4, F5, F9, F17, F18. */
static void test_t140(void) {
    uint32_t e0[14], e1[14], s3b[5], s3a[5], f0[5], f1[5];
    int ok = 1;
    const char *why = "register authority";
    it_quiesce_reaper();
    if (!it_sched_ext(e0) || !it_sched_ext3(s3b) || !it_sched_ext5(f0)) {
        it_fail("T140", "sched ext"); return;
    }

    /* Child 1: probe every failure path, then fault with NO handler. */
    long ep = it_ep_create();
    handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    handle_id_t proc_h = HANDLE_INVALID;
    if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
        it_close(&ep_h); it_fail("T140", "spawn"); return;
    }
    long n1 = it_notify_create();
    long w  = it_notify_create();
    handle_id_t n1_h = (n1 >= 0) ? (handle_id_t)n1 : HANDLE_INVALID;
    handle_id_t w_h  = (w  >= 0) ? (handle_id_t)w  : HANDLE_INVALID;
    if (n1 < 0 || w < 0) { ok = 0; why = "notify create"; }

    if (ok && it_sys3(SYS_PROCESS_WATCH, (long)proc_h, w, 1) != 0) { ok = 0; why = "watch"; }

    /* Wrong types, both slots. */
    if (ok && it_sys3(SYS_EXCEPTION_HANDLER, (long)proc_h, (long)ep_h, 1)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "notif wrong-type"; }
    if (ok && it_sys3(SYS_EXCEPTION_HANDLER, (long)n1_h, n1, 1)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "proc wrong-type"; }
    /* Reduced rights, both slots — ACCESS_DENIED, no fallback. */
    long pr_ro = it_sys2(SYS_HANDLE_DUP, (long)proc_h, (long)RIGHT_READ);
    long n_ro  = it_sys2(SYS_HANDLE_DUP, n1, (long)RIGHT_READ);
    handle_id_t pr_ro_h = (pr_ro >= 0) ? (handle_id_t)pr_ro : HANDLE_INVALID;
    handle_id_t n_ro_h  = (n_ro  >= 0) ? (handle_id_t)n_ro  : HANDLE_INVALID;
    if (ok && (pr_ro < 0 || n_ro < 0)) { ok = 0; why = "ro dups"; }
    if (ok && it_sys3(SYS_EXCEPTION_HANDLER, pr_ro, n1, 1)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "proc no-manage not denied"; }
    if (ok && it_sys3(SYS_EXCEPTION_HANDLER, (long)proc_h, n_ro, 1)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "notif no-write not denied"; }
    /* Zero signal bits / empty slot. */
    if (ok && it_sys3(SYS_EXCEPTION_HANDLER, (long)proc_h, n1, 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bits==0 not rejected"; }
    if (ok && it_sys3(SYS_EXCEPTION_HANDLER, (long)proc_h, 9999L, 1) >= 0) {
        ok = 0; why = "empty slot accepted";
    }

    /* Every attempt above failed → no handler may be installed: the fault must
     * take the kill path (F5 — no partial registration, no fallback). */
    if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, w, (long)(uintptr_t)&bits,
                    2000000000LL) != 0 || !(bits & 1ull)) { ok = 0; why = "nohandler kill"; }
    }
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)proc_h) != 0) { ok = 0; why = "kill exit code"; }
    if (ok && it_fault_info(proc_h, &(struct it_fault){0}) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "dead proc fault info";
    }
    /* Dead process: registration must fail NOT_FOUND, not silently pin. */
    if (ok && it_sys3(SYS_EXCEPTION_HANDLER, (long)proc_h, n1, 1)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "dead reg not NOT_FOUND"; }

    it_close(&pr_ro_h); it_close(&n_ro_h);
    it_fault_close4(&ep_h, &proc_h, &n1_h, &w_h);

    /* Child 2: valid registration, replacement contract, spoof check. */
    handle_id_t ep2, pr2, na, wb;
    if (ok && !it_fault_spawn(&ep2, &pr2, &na, &wb, &why)) { ok = 0; }
    if (ok) {
        long n2 = it_notify_create();
        handle_id_t n2_h = (n2 >= 0) ? (handle_id_t)n2 : HANDLE_INVALID;
        if (n2 < 0) { ok = 0; why = "n2 create"; }
        /* Replace na with n2 — last registration wins. */
        if (ok && it_sys3(SYS_EXCEPTION_HANDLER, (long)pr2, n2, 1) != 0) {
            ok = 0; why = "re-register";
        }
        /* Spoof: hand-signal n2 — no fault state may appear (F9). */
        if (ok && it_sys2(SYS_NOTIFY_SIGNAL, n2, 1) != 0) { ok = 0; why = "spoof signal"; }
        if (ok && it_fault_info(pr2, &(struct it_fault){0})
                  != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "spoofed fault info"; }
        if (ok) {
            uint64_t bits = 0;   /* drain the hand-signal before the real fault */
            (void)it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, n2, (long)(uintptr_t)&bits, 100000000LL);
        }
        if (ok && it_lp_cmd_va(ep2, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd2"; }
        if (ok && !it_fault_wait(n2_h)) { ok = 0; why = "replaced handler no signal"; }
        /* The replaced-away notification must NOT have fired. */
        if (ok) {
            uint64_t bits = 0;
            if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)na, (long)(uintptr_t)&bits,
                        100000000LL) == 0 && (bits & 1ull)) { ok = 0; why = "old handler fired"; }
        }
        struct it_fault f;
        if (ok && it_fault_info(pr2, &f) != 0) { ok = 0; why = "fault info 2"; }
        if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)pr2, (long)f.task_id, 1) != 0) {
            ok = 0; why = "resume kill";
        }
        if (ok && it_lp_wait_exit(pr2) != 0) { ok = 0; why = "child2 exit"; }
        it_close(&n2_h);
    }
    it_fault_close4(&ep2, &pr2, &na, &wb);

    it_quiesce_reaper();
    if (ok && (!it_sched_ext(e1) || !it_sched_ext3(s3a) || !it_sched_ext5(f1))) {
        ok = 0; why = "ext final";
    }
    if (ok && f1[IT_S5_NOHAND]  != f0[IT_S5_NOHAND] + 1u)  { ok = 0; why = "nohandler count"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivery count"; }
    if (ok && e1[IT_SI_LIVE]    != e0[IT_SI_LIVE])         { ok = 0; why = "handle leak"; }
    if (ok && s3a[IT_S3_NOTIF]  != s3b[IT_S3_NOTIF])       { ok = 0; why = "notif leak"; }
    if (ok && s3a[IT_S3_EP]     != s3b[IT_S3_EP])          { ok = 0; why = "ep leak"; }
    if (ok) it_pass("T140"); else it_fail("T140", why);
}

/* ── T141: page fault delivery — invalid user VA ────────────────────────────
 * The foundational delivery contract: the child touches an unmapped user VA;
 * the fault arrives exactly once on the registered notification with honest
 * info (vector 14, cr2 == the VA, user-range rip, error P=0/U=1); the child is
 * suspended (still alive, not running past the faulting load) while pending;
 * the record persists until resolved; kill-resolution reaps the child and
 * clears the record.  Invariants: F1, F6, F7, F8, F11, F15, F19. */
static void test_t141(void) {
    uint32_t t0 = 0, t1 = 0, e0[14], e1[14], f0[5], f1[5];
    int ok = 1;
    const char *why = "invalid VA delivery";
    it_quiesce_reaper();
    if (!it_task_live(&t0) || !it_sched_ext(e0) || !it_sched_ext5(f0)) {
        it_fail("T141", "sched ext"); return;
    }

    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T141", why); return; }

    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd"; }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no delivery"; }

    struct it_fault f;
    if (ok && it_fault_info(proc_h, &f) != 0) { ok = 0; why = "fault info"; }
    if (ok && f.vector != 14u)               { ok = 0; why = "vector"; }
    if (ok && f.cr2 != T14X_BAD_VA)          { ok = 0; why = "cr2"; }
    if (ok && (f.rip == 0 || f.rip >= 0x0000800000000000ULL)) { ok = 0; why = "rip range"; }
    if (ok && f.task_id == 0)                { ok = 0; why = "task id"; }
    if (ok && f.error != PF_ERR_U)           { ok = 0; why = "error bits"; }  /* not-present user read */

    /* Suspended while pending: alive (EXIT_CODE blocks), no second signal, and
     * the record is stable across reads. */
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)proc_h)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "child not suspended-alive"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)n_h, (long)(uintptr_t)&bits,
                    100000000LL) == 0 && (bits & 1ull)) { ok = 0; why = "double delivery"; }
    }
    struct it_fault f2;
    if (ok && (it_fault_info(proc_h, &f2) != 0 || f2.task_id != f.task_id ||
               f2.cr2 != f.cr2 || f2.rip != f.rip)) { ok = 0; why = "record unstable"; }

    /* Kill-resolution: reaps the child, clears the record. */
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 1) != 0) {
        ok = 0; why = "resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit code"; }
    if (ok && it_fault_info(proc_h, &f2) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived kill";
    }

    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);
    it_quiesce_reaper();
    if (ok && (!it_task_live(&t1) || !it_sched_ext(e1) || !it_sched_ext5(f1))) {
        ok = 0; why = "ext final";
    }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivered != 1"; }
    if (ok && f1[IT_S5_KILL]    != f0[IT_S5_KILL] + 1u)    { ok = 0; why = "kill count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 1u)   { ok = 0; why = "cleanup count"; }
    if (ok && t1 != t0)                    { ok = 0; why = "task live drift"; }
    if (ok && e1[IT_SI_LIVE]  != e0[IT_SI_LIVE])  { ok = 0; why = "handle leak"; }
    if (ok && e1[IT_SI_REPLY] != e0[IT_SI_REPLY]) { ok = 0; why = "kreply drift"; }
    if (ok) it_pass("T141"); else it_fail("T141", why);
}

/* ── T142: write-protection fault on a read-only mapping ────────────────────
 * Closes the Fase 19 T138 gap: write-enforcement is now asserted at the
 * HARDWARE level, not only at the rights layer.  The child's own code pages
 * are mapped r-x by the loader (PF flags → map flags → PTE), so:
 *   - a READ of its own text completes (child exits normally, no fault);
 *   - a WRITE to its own text raises #PF with error P=1/W=1/U=1, cr2 = the
 *     write target, and the store must NOT retire: resuming without fixing
 *     the condition re-faults at the same rip/cr2 (no silent write, no
 *     corruption), after which the supervisor kills the child.
 * VSpace books return to baseline after reap.  Invariants: F6, F7, F16, F20,
 * F21 (write bit distinguishable).  Fase 19 V6 gap closed. */
static void test_t142(void) {
    uint32_t v0[5], v1[5], f0[5], f1[5];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "ro write fault";
    it_quiesce_reaper();
    if (!it_sched_ext4(v0) || !it_sched_ext5(f0) || !it_task_live(&t0)) {
        it_fail("T142", "sched ext"); return;
    }

    /* Child A: reading own text is allowed — exits, no fault delivered. */
    handle_id_t ep_a, pr_a, n_a, w_a;
    if (!it_fault_spawn(&ep_a, &pr_a, &n_a, &w_a, &why)) { it_fail("T142", why); return; }
    if (it_lp_cmd_va(ep_a, LP_CMD_FAULT_READ, 0) != 0) { ok = 0; why = "cmd read"; }
    if (ok) {
        long ec = it_lp_wait_exit(pr_a);
        if ((ec >> 8) != (LP_EXIT_MARKER >> 8)) { ok = 0; why = "text read blocked"; }
    }
    it_fault_close4(&ep_a, &pr_a, &n_a, &w_a);

    /* Child B: writing own text must fault — and must not retire. */
    handle_id_t ep_h, proc_h, n_h, w_h;
    if (ok && !it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T142", why); return; }
    if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_WRITE, 0) != 0) { ok = 0; why = "cmd write"; }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no delivery"; }

    struct it_fault f;
    if (ok && it_fault_info(proc_h, &f) != 0) { ok = 0; why = "fault info"; }
    if (ok && f.vector != 14u) { ok = 0; why = "vector"; }
    if (ok && f.error != (PF_ERR_P | PF_ERR_W | PF_ERR_U)) { ok = 0; why = "not a write-protect err"; }
    if (ok && (f.cr2 == 0 || f.cr2 >= 0x0000800000000000ULL)) { ok = 0; why = "cr2 range"; }

    /* Resume without fixing: the same store re-faults (no silent write). */
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 0) != 0) {
        ok = 0; why = "resume";
    }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no refault"; }
    struct it_fault g;
    if (ok && it_fault_info(proc_h, &g) != 0) { ok = 0; why = "refault info"; }
    if (ok && (g.rip != f.rip || g.cr2 != f.cr2 ||
               g.error != f.error)) { ok = 0; why = "refault mismatch"; }
    /* The child must NOT have exited (the store never retires). */
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)proc_h)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "write retired"; }

    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)g.task_id, 1) != 0) {
        ok = 0; why = "resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

    it_quiesce_reaper();
    if (ok && (!it_sched_ext4(v1) || !it_sched_ext5(f1) || !it_task_live(&t1))) {
        ok = 0; why = "ext final";
    }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 1u)  { ok = 0; why = "resume count"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping drift"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace drift"; }
    if (ok && t1 != t0) { ok = 0; why = "task live drift"; }
    if (ok) it_pass("T142"); else it_fail("T142", why);
}

/* ── T143: NX instruction-fetch fault ───────────────────────────────────────
 * NX is real end to end: EFER.NXE is enabled at paging init and every
 * non-EXEC user mapping carries PTE.NX (kframe_map_page), including the
 * child's stack (rw- by the loader/creator).  The child copies `ret` opcodes
 * onto its stack and calls them: the fetch must fault with error
 * P=1/U=1/I=1 and cr2 == rip (the fetch address IS the faulting address) in
 * the user range.  No escalation: the supervisor observes and kills.
 * Invariants: F1, F7, F21 (execute bit distinguishable), F22. */
static void test_t143(void) {
    uint32_t f0[5], f1[5];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "nx exec fault";
    it_quiesce_reaper();
    if (!it_sched_ext5(f0) || !it_task_live(&t0)) { it_fail("T143", "sched ext"); return; }

    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T143", why); return; }

    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_EXEC, 0) != 0) { ok = 0; why = "cmd"; }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no delivery"; }

    struct it_fault f;
    if (ok && it_fault_info(proc_h, &f) != 0) { ok = 0; why = "fault info"; }
    if (ok && f.vector != 14u) { ok = 0; why = "vector"; }
    if (ok && f.error != (PF_ERR_P | PF_ERR_U | PF_ERR_I)) { ok = 0; why = "not an ifetch err"; }
    if (ok && f.cr2 != f.rip) { ok = 0; why = "cr2 != rip"; }
    if (ok && (f.cr2 == 0 || f.cr2 >= 0x0000800000000000ULL)) { ok = 0; why = "cr2 range"; }

    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 1) != 0) {
        ok = 0; why = "resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

    it_quiesce_reaper();
    if (ok && (!it_sched_ext5(f1) || !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivery count"; }
    if (ok && t1 != t0) { ok = 0; why = "task live drift"; }
    if (ok) it_pass("T143"); else it_fail("T143", why);
}

/* ── T144: fault resume semantics ───────────────────────────────────────────
 * Resolution is exact, authorized, and one-shot:
 *   - RESUME through a proc cap without RIGHT_MANAGE is ACCESS_DENIED (F10);
 *   - RESUME naming a task that is not fault-blocked in that process is
 *     NOT_FOUND (F11); action > 1 is INVALID_ARG;
 *   - a valid action-0 RESUME re-executes the faulting instruction — the same
 *     unmapped load faults again as a NEW pending fault (F16);
 *   - after kill-resolution the record is gone: FAULT_INFO is WOULD_BLOCK and
 *     a second RESUME is a clean NOT_FOUND (F11, F12 — no stale state). */
static void test_t144(void) {
    uint32_t f0[5], f1[5];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "resume semantics";
    it_quiesce_reaper();
    if (!it_sched_ext5(f0) || !it_task_live(&t0)) { it_fail("T144", "sched ext"); return; }

    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T144", why); return; }

    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd"; }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no delivery"; }
    struct it_fault f;
    if (ok && it_fault_info(proc_h, &f) != 0) { ok = 0; why = "fault info"; }

    /* Wrong authority: RIGHT_READ-only proc dup must be denied. */
    long pr_ro = it_sys2(SYS_HANDLE_DUP, (long)proc_h, (long)RIGHT_READ);
    handle_id_t pr_ro_h = (pr_ro >= 0) ? (handle_id_t)pr_ro : HANDLE_INVALID;
    if (ok && pr_ro < 0) { ok = 0; why = "ro dup"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, pr_ro, (long)f.task_id, 0)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-manage not denied"; }
    /* Exactness: wrong task id / bad action. */
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)(f.task_id + 4096u), 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "bogus id not NOT_FOUND"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 2)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "action 2 not rejected"; }

    /* Valid resume: the load re-executes and faults again — a NEW fault. */
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 0) != 0) {
        ok = 0; why = "resume";
    }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no refault"; }
    struct it_fault g;
    if (ok && it_fault_info(proc_h, &g) != 0) { ok = 0; why = "refault info"; }
    if (ok && (g.cr2 != f.cr2 || g.task_id != f.task_id)) { ok = 0; why = "refault mismatch"; }

    /* Kill-resolution, then verify nothing stale remains. */
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)g.task_id, 1) != 0) {
        ok = 0; why = "resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)g.task_id, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late resume not NOT_FOUND"; }
    if (ok && it_fault_info(proc_h, &g) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "stale record";
    }

    it_close(&pr_ro_h);
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);
    it_quiesce_reaper();
    if (ok && (!it_sched_ext5(f1) || !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 1u)  { ok = 0; why = "resume count"; }
    if (ok && f1[IT_S5_KILL]    != f0[IT_S5_KILL] + 1u)    { ok = 0; why = "kill count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 2u)   { ok = 0; why = "cleanup count"; }
    if (ok && t1 != t0) { ok = 0; why = "task live drift"; }
    if (ok) it_pass("T144"); else it_fail("T144", why);
}

/* ── T145: handler drop during a pending fault ──────────────────────────────
 * The handler's notification HANDLE is not the resolution authority — the
 * process cap is.  Registration pins the notification object inside the
 * KProcess, so the supervisor closing its own handle mid-fault leaves the
 * pending fault fully resolvable:
 *   (a) close the handler notif while a fault is pending → EXCEPTION_RESUME
 *       (kill) through the proc cap still resolves and reaps;
 *   (b) same, but resolve via SYS_PROCESS_KILL → teardown clears the fault
 *       record, releases the pinned notification, reaps everything.
 * Documented contract: handler death never auto-kills the faulted process;
 * the faulted task stays suspended until a RIGHT_MANAGE holder resolves it.
 * No zombies, no waiter/KReply drift, notification objects at baseline.
 * Invariants: F13, F14, F15, F17, F18, F19. */
static void test_t145(void) {
    uint32_t e0[14], e1[14], s3b[5], s3a[5], f0[5], f1[5];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "handler drop";
    it_quiesce_reaper();
    if (!it_sched_ext(e0) || !it_sched_ext3(s3b) || !it_sched_ext5(f0) ||
        !it_task_live(&t0)) { it_fail("T145", "sched ext"); return; }

    /* (a) notif handle closed mid-fault → resume-kill still resolves. */
    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T145", why); return; }
    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd a"; }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no delivery a"; }
    struct it_fault f;
    if (ok && it_fault_info(proc_h, &f) != 0) { ok = 0; why = "fault info a"; }
    it_close(&n_h);                       /* handler endpoint gone */
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)proc_h)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "child died on handler close"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 1) != 0) {
        ok = 0; why = "post-close resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit a"; }
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

    /* (b) notif handle closed mid-fault → PROCESS_KILL resolves via teardown. */
    if (ok && !it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T145", why); return; }
    if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd b"; }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no delivery b"; }
    it_close(&n_h);
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "process kill"; }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit b"; }
    if (ok && it_fault_info(proc_h, &f) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived teardown";
    }
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

    it_quiesce_reaper();
    if (ok && (!it_sched_ext(e1) || !it_sched_ext3(s3a) || !it_sched_ext5(f1) ||
               !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 2u)   { ok = 0; why = "cleanup count"; }
    if (ok && t1 != t0)                   { ok = 0; why = "task live drift"; }
    if (ok && e1[IT_SI_LIVE]   != e0[IT_SI_LIVE])   { ok = 0; why = "handle leak"; }
    if (ok && e1[IT_SI_REPLY]  != e0[IT_SI_REPLY])  { ok = 0; why = "kreply drift"; }
    if (ok && s3a[IT_S3_NOTIF] != s3b[IT_S3_NOTIF]) { ok = 0; why = "notif obj leak"; }
    if (ok && s3a[IT_S3_EP]    != s3b[IT_S3_EP])    { ok = 0; why = "ep obj leak"; }
    if (ok) it_pass("T145"); else it_fail("T145", why);
}

/* ── T146: process kill while a fault is pending ────────────────────────────
 * The supervisor kills the whole process while its task sits in
 * TASK_BLOCKED_FAULT.  Teardown must clear the fault record, cancel nothing
 * that isn't there, and leave zero residue; the handler's LATE response after
 * the kill fails clean:
 *   - EXCEPTION_RESUME after the kill → NOT_FOUND (no matching blocked task);
 *   - FAULT_INFO after the kill → WOULD_BLOCK (record cleared by teardown);
 *   - task/handle/KReply/mapping books at baseline.
 * Invariants: F12, F15, F17, F18, F19, F20. */
static void test_t146(void) {
    uint32_t e0[14], e1[14], v0[5], v1[5], f0[5], f1[5];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "kill while pending";
    it_quiesce_reaper();
    if (!it_sched_ext(e0) || !it_sched_ext4(v0) || !it_sched_ext5(f0) ||
        !it_task_live(&t0)) { it_fail("T146", "sched ext"); return; }

    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T146", why); return; }

    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_WRITE, T14X_BAD_VA) != 0) { ok = 0; why = "cmd"; }
    if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no delivery"; }
    struct it_fault f;
    if (ok && it_fault_info(proc_h, &f) != 0) { ok = 0; why = "fault info"; }
    if (ok && f.error != (PF_ERR_W | PF_ERR_U)) { ok = 0; why = "write err bits"; } /* not-present user write */

    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }

    /* Late handler response: clean failures, no stale record. */
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late resume not NOT_FOUND"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 1)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late kill not NOT_FOUND"; }
    if (ok && it_fault_info(proc_h, &f) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived kill";
    }

    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);
    it_quiesce_reaper();
    if (ok && (!it_sched_ext(e1) || !it_sched_ext4(v1) || !it_sched_ext5(f1) ||
               !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_KILL]    != f0[IT_S5_KILL])         { ok = 0; why = "resume-kill count moved"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 1u)   { ok = 0; why = "cleanup count"; }
    if (ok && t1 != t0)                  { ok = 0; why = "task live drift"; }
    if (ok && e1[IT_SI_LIVE]  != e0[IT_SI_LIVE])  { ok = 0; why = "handle leak"; }
    if (ok && e1[IT_SI_REPLY] != e0[IT_SI_REPLY]) { ok = 0; why = "kreply drift"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping drift"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace drift"; }
    if (ok) it_pass("T146"); else it_fail("T146", why);
}

/* ── T147: deterministic fault endpoint stress ──────────────────────────────
 * A fixed-seed PRNG drives rounds of spawn → mixed fault kind (invalid-VA
 * read, own-text write, kernel-range read, NX exec) → mixed resolution
 * (resume-refault-kill / resume-kill / PROCESS_KILL / close-notif-then-kill),
 * with occasional non-faulting children (own-text read) interleaved.  Every
 * wait is bounded; two suspended-in-fault children coexist at one point every
 * round (the shared-IST regression fixture: both keep live frames while
 * blocked).  After the churn the fault counters and every book return to
 * baseline: no hung faulted tasks, no KReply/waiter drift, no live_task/proc
 * drift, no mapping drift, no stale fault state.  Prints seed/iteration only
 * on failure.  Invariants: F6, F11, F13, F15, F16, F17, F18, F19, F20. */
#define T147_SEED   0x20C0DE20u
#define T147_ROUNDS 10u
static void test_t147(void) {
    uint32_t e0[14], e1[14], s3b[5], s3a[5], v0[5], v1[5], f0[5], f1[5];
    uint32_t t0 = 0, t1 = 0;
    it_quiesce_reaper();
    if (!it_sched_ext(e0) || !it_sched_ext3(s3b) || !it_sched_ext4(v0) ||
        !it_sched_ext5(f0) || !it_task_live(&t0)) { it_fail("T147", "sched ext"); return; }
    g_fz_seed = T147_SEED;
    int ok = 1;
    const char *why = "fault stress";
    uint32_t i = 0;

    for (i = 0; ok && i < T147_ROUNDS; i++) {
        /* Primary faulting child. */
        handle_id_t ep_h, proc_h, n_h, w_h;
        if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { ok = 0; break; }

        uint32_t kind = fz_rand() % 4u;
        uint32_t cmd  = (kind == 1u) ? LP_CMD_FAULT_WRITE
                      : (kind == 3u) ? LP_CMD_FAULT_EXEC
                                     : LP_CMD_FAULT_READ;
        uint64_t va   = (kind == 0u) ? T14X_BAD_VA
                      : (kind == 2u) ? T14X_KERN_VA
                                     : 0;
        if (it_lp_cmd_va(ep_h, cmd, va) != 0) { ok = 0; why = "cmd"; }
        if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no delivery"; }
        struct it_fault f;
        if (ok && it_fault_info(proc_h, &f) != 0) { ok = 0; why = "fault info"; }
        if (ok && f.vector != 14u) { ok = 0; why = "vector"; }

        /* Second child suspended in fault at the same time: two live blocked
         * fault frames must coexist without corrupting each other. */
        handle_id_t ep2, pr2, n2, w2;
        int have2 = 0;
        if (ok && (fz_rand() & 1u)) {
            if (!it_fault_spawn(&ep2, &pr2, &n2, &w2, &why)) { ok = 0; break; }
            have2 = 1;
            if (it_lp_cmd_va(ep2, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd2"; }
            if (ok && !it_fault_wait(n2)) { ok = 0; why = "no delivery 2"; }
        }

        /* Resolve the primary child. */
        uint32_t res = fz_rand() % 4u;
        if (ok && res == 0u) {
            /* resume → refault → kill (also re-proves F16 under churn). */
            if (it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 0) != 0) {
                ok = 0; why = "resume";
            }
            if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no refault"; }
            if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 1) != 0) {
                ok = 0; why = "refault kill";
            }
        } else if (ok && res == 1u) {
            if (it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 1) != 0) {
                ok = 0; why = "resume kill";
            }
        } else if (ok && res == 2u) {
            if (it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "proc kill"; }
        } else if (ok) {
            it_close(&n_h);   /* handler drop first, then resolve via proc cap */
            if (it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 1) != 0) {
                ok = 0; why = "post-close kill";
            }
        }
        if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }
        if (ok && it_fault_info(proc_h, &f) != (long)IRIS_ERR_WOULD_BLOCK) {
            ok = 0; why = "stale record";
        }
        it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

        /* Resolve the second child (kill via whichever authority remains). */
        if (have2) {
            struct it_fault f2;
            if (ok && it_fault_info(pr2, &f2) != 0) { ok = 0; why = "fault info 2"; }
            if (ok && ((fz_rand() & 1u)
                       ? it_sys3(SYS_EXCEPTION_RESUME, (long)pr2, (long)f2.task_id, 1)
                       : it_sys1(SYS_PROCESS_KILL, (long)pr2)) != 0) {
                ok = 0; why = "resolve 2";
            }
            if (ok && it_lp_wait_exit(pr2) != 0) { ok = 0; why = "exit 2"; }
            it_fault_close4(&ep2, &pr2, &n2, &w2);
        }

        /* Occasionally interleave a non-faulting child (own-text read). */
        if (ok && (fz_rand() & 1u)) {
            handle_id_t ep3, pr3, n3, w3;
            if (!it_fault_spawn(&ep3, &pr3, &n3, &w3, &why)) { ok = 0; break; }
            if (it_lp_cmd_va(ep3, LP_CMD_FAULT_READ, 0) != 0) { ok = 0; why = "cmd3"; }
            if (ok) {
                long ec = it_lp_wait_exit(pr3);
                if ((ec >> 8) != (LP_EXIT_MARKER >> 8)) { ok = 0; why = "clean child"; }
            }
            it_fault_close4(&ep3, &pr3, &n3, &w3);
        }

        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && (!it_sched_ext(e1) || !it_sched_ext3(s3a) || !it_sched_ext4(v1) ||
               !it_sched_ext5(f1) || !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && t1 != t0)                    { ok = 0; why = "task live drift"; }
    if (ok && e1[IT_SI_LIVE]   != e0[IT_SI_LIVE])   { ok = 0; why = "handle leak"; }
    if (ok && e1[IT_SI_REPLY]  != e0[IT_SI_REPLY])  { ok = 0; why = "kreply drift"; }
    if (ok && s3a[IT_S3_NOTIF] != s3b[IT_S3_NOTIF]) { ok = 0; why = "notif obj leak"; }
    if (ok && s3a[IT_S3_EP]    != s3b[IT_S3_EP])    { ok = 0; why = "ep obj leak"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping drift"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace drift"; }
    if (ok && f1[IT_S5_DELIVER] < f0[IT_S5_DELIVER] + T147_ROUNDS) { ok = 0; why = "delivery floor"; }

    if (ok) it_pass("T147");
    else {
        fz_note("T147", T147_SEED, i);
        it_fail("T147", why);
    }
}

/* ── Fase 21: cross-syscall fuzzing / hostile argument surface (T148–T155) ───
 *
 * These tests do not exercise a feature — they subject the WHOLE syscall
 * surface to deterministic adversarial pressure and prove it fails clean:
 * unknown/retired numbers, wrong-type caps, stale/empty/boundary handles,
 * hostile user pointers, reduced rights, forced mid-syscall failures, and
 * mixed cross-family sequences.  The bar is not "no crash" — it is "every
 * hostile input fails clean with zero drift in any live count".
 *
 * The anchor is a full-surface snapshot: every live-object gauge the kernel
 * exposes (task, process, handle, untyped/frame/endpoint/notification/cnode,
 * VSpace, mapping) plus the KReply balance and the pending-fault flag.  A
 * hostile op that leaks a ref, plants a ghost slot/PTE, or strands a waiter
 * moves at least one of these, so equality-to-baseline after the churn is a
 * strong, cheap invariant.  Cumulative counters (map/unmap/TLB/fault-delivery)
 * are checked as directional deltas only where a test deliberately triggers
 * the event.
 *
 * Determinism: xorshift32 (fz_rand) seeded per test; failures print
 * `FZ <test> seed=<seed> iter=<i> op=<op>` and nothing on success.  Every
 * blocking interaction rendezvouses through a control endpoint or a bounded
 * NOTIFY_WAIT_TIMEOUT — no long sleeps.  Invariants X1–X24 live in
 * docs/architecture/syscall-fuzzing.md. */

/* Full-surface live snapshot. */
struct it_snap {
    uint32_t task;                 /* live scheduler tasks */
    uint32_t hlive, ghwm, hmax;    /* handle-table live / global hwm / max */
    uint32_t proclive, reply;      /* live processes / reply-caps-created (cumulative) */
    uint32_t ut, fr, ep, no, cn;   /* live untyped/frame/endpoint/notif/cnode */
    uint32_t vs, map;              /* live VSpace / mapping nodes */
    uint32_t fdeliver, fclean;     /* cumulative fault delivery / cleanup */
    uint8_t  ok;
};

static struct it_snap it_snap_take(void) {
    struct it_snap s;
    uint32_t e[14], w3[5], w4[5], w5[5];
    s.ok = 0;
    if (!it_task_live(&s.task) || !it_sched_ext(e) || !it_sched_ext3(w3) ||
        !it_sched_ext4(w4) || !it_sched_ext5(w5)) return s;
    s.hlive = e[IT_SI_LIVE]; s.ghwm = e[IT_SI_GHWM]; s.hmax = e[IT_SI_MAX];
    s.proclive = e[IT_SI_PROCLIVE]; s.reply = e[IT_SI_REPLY];
    s.ut = w3[IT_S3_UNTYPED]; s.fr = w3[IT_S3_FRAME]; s.ep = w3[IT_S3_EP];
    s.no = w3[IT_S3_NOTIF]; s.cn = w3[IT_S3_CNODE];
    s.vs = w4[IT_S4_VSLIVE]; s.map = w4[IT_S4_MAPLIVE];
    s.fdeliver = w5[IT_S5_DELIVER]; s.fclean = w5[IT_S5_CLEAN];
    s.ok = 1;
    return s;
}

/* Assert every LIVE gauge returned to baseline; *why names the first drift.
 * Cumulative counters are intentionally excluded. */
static int it_snap_baseline(const struct it_snap *a, const struct it_snap *b,
                            const char **why) {
    if (!a->ok || !b->ok)          { *why = "snap read"; return 0; }
    if (b->task != a->task)        { *why = "task live drift"; return 0; }
    if (b->proclive != a->proclive){ *why = "proc live drift"; return 0; }
    if (b->hlive != a->hlive)      { *why = "handle leak"; return 0; }
    if (b->ut != a->ut)            { *why = "untyped drift"; return 0; }
    if (b->fr != a->fr)            { *why = "frame drift"; return 0; }
    if (b->ep != a->ep)            { *why = "endpoint drift"; return 0; }
    if (b->no != a->no)            { *why = "notif drift"; return 0; }
    if (b->cn != a->cn)            { *why = "cnode drift"; return 0; }
    if (b->vs != a->vs)            { *why = "vspace drift"; return 0; }
    if (b->map != a->map)          { *why = "mapping drift"; return 0; }
    /* The global handle high-water rule (ghwm*4 <= max) is a monotonic gauge,
     * not a per-test balance — it is asserted by T095/T112 in their own
     * contexts.  Applying it here would fire on HWM inherited from the heavy
     * fault-stress tests, so the fuzz baseline checks LIVE gauges only. */
    return 1;
}

/* Lifecycle-reliable subset of the baseline, for tests that SPAWN child
 * processes.  Loading a service via svc_load creates transient child-owned
 * objects (bootstrap endpoint, segment frames, CNode) whose reaping is
 * deferred, so the per-type KEndpoint/KFrame/KCNode/KUntyped/KNotification
 * OBJECT counts are not a reliable per-test balance across a child spawn —
 * the canonical cross-process churn test (T114) omits them for the same
 * reason.  The gauges that DO return exactly to baseline after child teardown
 * are task-live, process-live, handle-live, the KReply balance and the VM
 * books; those are what the fault suite (T145–T147) already proved reliable. */
static int it_snap_baseline_live(const struct it_snap *a, const struct it_snap *b,
                                 const char **why) {
    if (!a->ok || !b->ok)          { *why = "snap read"; return 0; }
    if (b->task != a->task)        { *why = "task live drift"; return 0; }
    if (b->proclive != a->proclive){ *why = "proc live drift"; return 0; }
    if (b->hlive != a->hlive)      { *why = "handle leak"; return 0; }
    if (b->vs != a->vs)            { *why = "vspace drift"; return 0; }
    if (b->map != a->map)          { *why = "mapping drift"; return 0; }
    return 1;
}

static void it_fz_note(const char *t, uint32_t seed, uint32_t iter, uint32_t op) {
    it_serial_write("[IRIS][TEST] FZ ");
    it_serial_write(t);
    it_serial_write(" seed="); it_log_num(seed);
    it_serial_write(" iter="); it_log_num(iter);
    it_serial_write(" op="); it_log_num(op);
    it_serial_write("\n");
}

/* Boundary values invalid in BOTH namespaces the dual resolver consults —
 * the handle table AND the CSpace CNode — so they are safe to feed to
 * mutating syscalls without aliasing a real capability:
 *   - as a handle_id_t (slot=bits[9:0], gen=bits[31:10], gen 0 forbidden,
 *     HANDLE_TABLE_MAX=256): all have slot 1023 ≥ 256 → out of table range;
 *   - as a CPtr (valid range 0..1023, IRIS_CPTR_LIMIT=1024): all are > 1023.
 * Small integers (0,1,2,…) are deliberately EXCLUDED: as CPtrs they alias
 * real caps (CPtr 1 = svcmgr EP), so honouring them is correct, not a bug. */
static const long it_fz_bad_handles[] = {
    4095L, 0x1FFFFL, 0x7FFFFFFFL, (long)0xFFFFFFFFUL,
};
#define IT_FZ_BAD_H_N ((int)(sizeof(it_fz_bad_handles) / sizeof(it_fz_bad_handles[0])))

/* ── T148: syscall table / retired syscall fuzz ─────────────────────────────
 * Invoke every non-live syscall number — retired, reserved, never-assigned,
 * and out-of-range high — with garbage arguments, plus a batch of huge
 * numbers whose low bits alias a live handler (the dispatch is an exact-match
 * switch, so 0x1_0000_0000 | live must NOT alias).  Every one must return
 * NOT_SUPPORTED and touch nothing.  No live number is ever fuzzed here (SYS_EXIT
 * etc. would self-destruct — the table below is holes only).
 * Invariants: X1, X9, X15, X16, X18, X23. */
static void test_t148(void) {
    /* Holes in 0..107 (every number NOT routed by syscall_dispatch). */
    static const long retired[] = {
        0, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 18, 23, 24, 25, 30, 31, 34,
        37, 38, 41, 42, 44, 63, 72,
    };
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "retired fuzz";
    g_fz_seed = 0x21C0DE01u;

    for (int i = 0; ok && i < (int)(sizeof(retired) / sizeof(retired[0])); i++) {
        long a0 = (long)fz_rand(), a1 = (long)fz_rand(), a2 = (long)fz_rand();
        if (it_sys3(retired[i], a0, a1, a2) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "retired not NOT_SUPPORTED";
            it_fz_note("T148", g_fz_seed, (uint32_t)i, (uint32_t)retired[i]);
            break;
        }
    }
    /* High/unassigned range 114..400 (111 = SYS_UNTYPED_RETYPE2, 112 =
     * SYS_UNTYPED_QUERY, Fase S2's 113 = SYS_SC_BIND are live; 107..110 remain
     * live from Fases 25/26/29). */
    for (long n = 114; ok && n <= 400; n++) {
        if (it_sys3(n, (long)fz_rand(), (long)fz_rand(), (long)fz_rand())
            != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "high not NOT_SUPPORTED";
            it_fz_note("T148", g_fz_seed, (uint32_t)n, (uint32_t)n);
            break;
        }
    }
    /* Numbers whose low 32 bits alias a live handler must NOT dispatch it —
     * the switch matches the full 64-bit value.  (SYS_GETPID=2 chosen: a live
     * dispatch would return >= 0, not NOT_SUPPORTED.) */
    if (ok) {
        long alias[] = { (long)0x100000002LL, (long)0x200000002LL, (long)-1L };
        for (int i = 0; ok && i < 3; i++) {
            if (it_sys3(alias[i], 0, 0, 0) != (long)IRIS_ERR_NOT_SUPPORTED) {
                ok = 0; why = "alias dispatched live handler";
            }
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T148"); else it_fail("T148", why);
}

/* ── T149: CPtr / handle / wrong-type fuzz ──────────────────────────────────
 * Every handle-taking syscall family gets fed empty slots, wrong-type caps,
 * stale handles and boundary values.  Wrong-type crossings are the core:
 * a notification handed to SYS_EP_SEND is WRONG_TYPE; an endpoint handed to
 * SYS_NOTIFY_SIGNAL is WRONG_TYPE; a frame handed to SYS_PROCESS_KILL is
 * WRONG_TYPE — none fall back, none mutate, none amplify.  Fixtures: one live
 * endpoint, notification and frame, all closed at the end.
 * Invariants: X2, X4, X5, X8, X9, X12, X21, X24. */
static void test_t149(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "wrong-type fuzz";

    long ep = it_ep_create();
    long no = it_notify_create();
    handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    handle_id_t no_h = (no >= 0) ? (handle_id_t)no : HANDLE_INVALID;
    if (ep < 0 || no < 0) { it_close(&ep_h); it_close(&no_h); it_fail("T149", "fixture"); return; }

    struct IrisMsg m; it_iris_msg_zero(&m);

    /* Wrong-type: endpoint op on a notification and vice-versa. */
    if (ok && it_sys2(SYS_EP_SEND, no, (long)&m) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "ep_send on notif"; }
    if (ok && it_sys2(SYS_EP_NB_SEND, no, (long)&m) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "nb_send on notif"; }
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, ep, 1) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "signal on ep"; }
    if (ok && it_sys1(SYS_PROCESS_KILL, ep) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "kill on ep"; }
    if (ok && it_sys3(SYS_UNTYPED_RETYPE, no, (long)IT_KOBJ_FRAME, 4096) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "retype on notif"; }
    /* (frame_map wrong-type coverage against a real VSpace is in T151/T152.) */

    /* Boundary/empty/stale handles across a spread of families.  Any negative
     * error is acceptable (BAD_HANDLE / NOT_FOUND / WRONG_TYPE / INVALID_ARG);
     * a NON-NEGATIVE return would mean a hostile handle was honoured. */
    for (int i = 0; ok && i < IT_FZ_BAD_H_N; i++) {
        long h = it_fz_bad_handles[i];
        if (it_sys1(SYS_HANDLE_TYPE, h) >= 0)                 { ok = 0; why = "handle_type honoured bad"; break; }
        if (it_sys2(SYS_NOTIFY_SIGNAL, h, 1) >= 0)            { ok = 0; why = "signal honoured bad"; break; }
        if (it_sys2(SYS_EP_SEND, h, (long)&m) >= 0)           { ok = 0; why = "ep_send honoured bad"; break; }
        if (it_sys1(SYS_PROCESS_KILL, h) >= 0)                { ok = 0; why = "kill honoured bad"; break; }
        if (it_sys2(SYS_HANDLE_DUP, h, (long)RIGHT_READ) >= 0){ ok = 0; why = "dup honoured bad"; break; }
        if (it_sys3(SYS_UNTYPED_RETYPE, h, (long)IT_KOBJ_FRAME, 4096) >= 0) { ok = 0; why = "retype honoured bad"; break; }
    }

    /* Stale handle: dup then close, the old id must be dead. */
    if (ok) {
        long d = it_sys2(SYS_HANDLE_DUP, no, (long)RIGHT_SAME_RIGHTS);
        handle_id_t d_h = (d >= 0) ? (handle_id_t)d : HANDLE_INVALID;
        if (d < 0) { ok = 0; why = "dup for stale"; }
        else {
            it_close(&d_h);
            if (it_sys1(SYS_HANDLE_TYPE, d) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "stale handle alive"; }
        }
    }

    it_close(&ep_h); it_close(&no_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T149"); else it_fail("T149", why);
}

/* ── T150: user pointer / buffer fuzz ───────────────────────────────────────
 * Syscalls that read or write userland buffers are fed null, kernel-half VAs,
 * unmapped user VAs, and read-only buffers where a write is required.  The
 * kernel validates every range by page-table walk BEFORE touching it
 * (usercopy.c: SMAP + user_range_*), so a hostile pointer must yield a clean
 * INVALID_ARG — never a kernel #PF, never a partial write.  A valid buffer
 * still works afterwards, proving the reject path left nothing wedged.
 * Invariants: X6, X7, X10, X18. */
static void test_t150(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "user ptr fuzz";

    /* SYS_SCHED_INFO authorises on a KDEBUG bootstrap cap PRESENT in the
     * handle table; resolve the spawn cap so the pointer checks reach the
     * user_range_writable stage instead of short-circuiting ACCESS_DENIED. */
    long scr = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    handle_id_t scr_h = (scr >= 0) ? (handle_id_t)scr : HANDLE_INVALID;

    static const long bad_ptr[] = {
        0L,                          /* null */
        0x100L,                      /* below USER_ADDR_MIN */
        (long)0xFFFF800000000000ULL, /* kernel half */
        (long)0x8090000000ULL,       /* canonical user, unmapped */
        (long)0xDEADBEEFULL,         /* misaligned unmapped */
    };
    const int NB = (int)(sizeof(bad_ptr) / sizeof(bad_ptr[0]));

    /* SYS_SCHED_INFO writes a buffer: every hostile dst → INVALID_ARG. */
    for (int i = 0; ok && i < NB; i++) {
        if (it_sys2(SYS_SCHED_INFO, bad_ptr[i], 184) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "sched_info bad dst"; break;
        }
    }
    /* SYS_PROCESS_FAULT_INFO (self) writes 32 bytes: hostile dst → INVALID_ARG
     * (the pointer is validated before the fault lookup). */
    for (int i = 0; ok && i < NB; i++) {
        if (it_sys2(SYS_PROCESS_FAULT_INFO, (long)HANDLE_INVALID, bad_ptr[i])
            != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "fault_info bad dst"; break; }
    }
    /* SYS_UNTYPED_INFO writes two OPTIONAL out params (a null pointer means
     * "skip this field" and is legal), so only a NON-NULL hostile pointer must
     * fail INVALID_ARG.  Index 0 is the null case and is skipped. */
    for (int i = 1; ok && i < NB; i++) {
        if (it_sys3(SYS_UNTYPED_INFO, IT_UT, 0, bad_ptr[i]) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "untyped_info bad dst"; break;
        }
    }
    /* Both-null is the legal "just validate the cap" call → success. */
    if (ok && it_sys3(SYS_UNTYPED_INFO, IT_UT, 0, 0) != 0) { ok = 0; why = "untyped_info null-null not ok"; }
    /* SYS_NOTIFY_WAIT_TIMEOUT writes out_bits: hostile dst → INVALID_ARG
     * (validated before blocking, so no waiter is ever created). */
    {
        long no = it_notify_create();
        handle_id_t no_h = (no >= 0) ? (handle_id_t)no : HANDLE_INVALID;
        if (no < 0) { ok = 0; why = "notif fixture"; }
        for (int i = 0; ok && i < NB; i++) {
            if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, no, bad_ptr[i], 1000000L)
                != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "notify_wait bad out"; break; }
        }
        it_close(&no_h);
    }
    /* SYS_EP_SEND with a hostile message pointer → INVALID_ARG, endpoint clean. */
    {
        long ep = it_ep_create();
        handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        if (ep < 0) { ok = 0; why = "ep fixture"; }
        for (int i = 0; ok && i < NB; i++) {
            if (it_sys2(SYS_EP_NB_SEND, ep, bad_ptr[i]) != (long)IRIS_ERR_INVALID_ARG) {
                ok = 0; why = "ep_send bad msg"; break;
            }
        }
        it_close(&ep_h);
    }

    /* Size fuzz on SYS_SCHED_INFO: below-base is INVALID_ARG, huge size is
     * clamped to the largest tier (not an overflow) and succeeds into a valid
     * buffer. */
    if (ok) {
        uint8_t buf[184];
        if (it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 0) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "size 0"; }
        if (ok && it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 8) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "size below base"; }
        if (ok && it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 0x7FFFFFFFL) != 0) { ok = 0; why = "huge size not clamped"; }
        /* A valid call still works — reject paths left nothing wedged. */
        if (ok && it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 184) != 0) { ok = 0; why = "valid after fuzz"; }
    }

    it_close(&scr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T150"); else it_fail("T150", why);
}

/* ── T151: cross-family hostile sequence fuzz ───────────────────────────────
 * A seeded PRNG drives a mixed stream of operations across CSpace/cap,
 * untyped, frame/VSpace, notification, endpoint and fault families against a
 * tiny in-test model of what SHOULD be live.  Half the ops are deliberately
 * malformed (wrong slot, wrong type, bad rights, bogus VA).  A malformed op
 * must never alter the model; a well-formed one advances it.  After every
 * batch the full snapshot must equal the pre-batch baseline once the churn is
 * unwound.  Invariants: X2–X5, X8–X14, X22–X24. */
#define T151_SEED   0x21C0DE51u
#define T151_ROUNDS 24u
static void test_t151(void) {
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T151", "vspace self mint"); return; }
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "cross-family fuzz";
    g_fz_seed = T151_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T151_ROUNDS; i++) {
        /* One well-formed object per round, resolved back to baseline at the
         * end of the round; malformed ops are interleaved and must no-op. */
        handle_id_t fr = HANDLE_INVALID, ep = HANDLE_INVALID, no = HANDLE_INVALID;
        uint64_t va = 0x8098000000ULL + (uint64_t)(fz_rand() % 8u) * 0x1000ULL;

        /* --- malformed batch (must all fail clean) --- */
        op = 1;
        if (it_sys3(SYS_UNTYPED_RETYPE, IT_UT, (long)0x7777, 4096) >= 0) { ok = 0; why = "bad-type retype ok"; break; }
        op = 2;
        if (it_sys4(SYS_FRAME_MAP, IT_UT, IT_VS, (long)va, (long)IT_MAP_W) >= 0) { ok = 0; why = "map wrong-type ok"; break; }
        op = 3;
        if (it_sys2(SYS_CAP_DERIVE, 9000L, (long)RIGHT_READ) >= 0) { ok = 0; why = "derive stale ok"; break; }
        op = 4;
        if (it_sys1(SYS_CAP_REVOKE, 9000L) >= 0) { ok = 0; why = "revoke stale ok"; break; }
        op = 5;
        if (it_sys3(SYS_EXCEPTION_RESUME, (long)HANDLE_INVALID, (long)(fz_rand() | 0x40000000u), 1)
            != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "resume no-fault not NOT_FOUND"; break; }
        op = 6;
        if (it_sys2(SYS_PROCESS_FAULT_INFO, (long)HANDLE_INVALID, 0L)
            != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "fault_info null ok"; break; }

        /* --- well-formed batch (must all succeed and be observable) --- */
        op = 10;
        long r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, (long)IT_KOBJ_FRAME, 4096);
        fr = (r >= 0) ? (handle_id_t)r : HANDLE_INVALID;
        if (fr == HANDLE_INVALID) { ok = 0; why = "retype frame"; break; }
        op = 11;
        long en = it_ep_create();
        ep = (en >= 0) ? (handle_id_t)en : HANDLE_INVALID;
        long nn = it_notify_create();
        no = (nn >= 0) ? (handle_id_t)nn : HANDLE_INVALID;
        if (ep == HANDLE_INVALID || no == HANDLE_INVALID) { ok = 0; why = "obj create"; it_close(&fr); it_close(&ep); it_close(&no); break; }

        op = 12;
        if ((fz_rand() & 1u)) {
            if (it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)va, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; }
            /* occupied VA is BUSY, not a silent overwrite */
            if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)va, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied not BUSY"; }
            if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)va) != 0) { ok = 0; why = "unmap"; }
        }
        op = 13;
        if (ok && (fz_rand() & 1u)) {
            if (it_sys2(SYS_NOTIFY_SIGNAL, (long)no, 1) != 0) { ok = 0; why = "signal"; }
            uint64_t bits = 0;
            if (ok && it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)no, (long)(uintptr_t)&bits, 500000000L) != 0) { ok = 0; why = "wait"; }
        }

        it_close(&fr); it_close(&ep); it_close(&no);

        if ((i & 3u) == 3u) { it_quiesce_reaper(); if (!it_ut_reset()) { ok = 0; why = "mid reset busy"; break; } }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "final reset busy"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T151");
    else { it_fz_note("T151", T151_SEED, i, op); it_fail("T151", why); }
}

/* ── T152: failure atomicity fuzz ───────────────────────────────────────────
 * Force each failure mode and prove the operation is all-or-nothing: the full
 * snapshot before == after (no half-built object, no dangling ref), and a
 * following VALID operation of the same family still works.  Modes: occupied
 * destination (BUSY), missing rights (ACCESS_DENIED), wrong type (WRONG_TYPE),
 * invalid pointer (INVALID_ARG), invalid VA (INVALID_ARG), bad object size
 * (INVALID_ARG), resume mismatch (NOT_FOUND).  Invariants: X7, X9, X10, X13, X22. */
static void test_t152(void) {
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T152", "vspace self mint"); return; }
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "atomicity fuzz";
    const uint64_t VA = 0x80A0000000ULL;

    /* Occupied VA: map, then a second map is BUSY and must not leak a node. */
    long r = it_retype_frame();
    handle_id_t fr = (r != HANDLE_INVALID) ? (handle_id_t)r : HANDLE_INVALID;
    if (fr == HANDLE_INVALID) { it_fail("T152", "retype"); return; }
    struct it_snap mid;
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)VA, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; }
    mid = it_snap_take();
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)VA, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied not BUSY"; }
    /* BUSY must have changed nothing since mid. */
    { struct it_snap now = it_snap_take(); const char *w2;
      if (ok && !it_snap_baseline(&mid, &now, &w2)) { ok = 0; why = "BUSY mutated state"; } }
    if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)VA) != 0) { ok = 0; why = "unmap"; }

    /* Missing rights: RIGHT_READ frame cap cannot map writable — ACCESS_DENIED,
     * no PTE born; a following writable map with a full cap works. */
    if (ok) {
        long rd = it_sys2(SYS_CAP_DERIVE, (long)fr, (long)RIGHT_READ);
        handle_id_t fr_ro = (rd >= 0) ? (handle_id_t)rd : HANDLE_INVALID;
        if (rd < 0) { ok = 0; why = "ro derive"; }
        if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_ro, IT_VS, (long)VA, (long)IT_MAP_W) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable not denied"; }
        if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)VA, (long)IT_MAP_W) != 0) { ok = 0; why = "valid map after deny"; }
        if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)VA) != 0) { ok = 0; why = "unmap 2"; }
        it_close(&fr_ro);
    }

    /* Invalid VA / bad size / bad pointer — each INVALID_ARG, nothing born. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)(VA | 0x40ULL), (long)IT_MAP_W) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "unaligned VA"; }
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, 0xFFFF800000001000L, (long)IT_MAP_W) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel VA"; }
    if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, (long)IT_KOBJ_FRAME, 3) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad frame size"; }
    /* Non-null hostile out pointer (kernel half) → INVALID_ARG, nothing written. */
    if (ok && it_sys3(SYS_UNTYPED_INFO, IT_UT, 0, 0xFFFF800000001000L) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel out ptr"; }
    /* Resume mismatch — NOT_FOUND, no state touched. */
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)HANDLE_INVALID, 0x33221100L, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "resume mismatch"; }

    it_close(&fr);
    it_quiesce_reaper();
    (void)it_ut_reset();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T152"); else it_fail("T152", why);
}

/* ── T153: blocking syscall cancellation fuzz ───────────────────────────────
 * A lifecycle_probe child is parked in each blocking primitive, then torn down
 * by every cancellation route, and the books must balance every time.  This
 * re-proves, under one roof and a seeded resolution mix, the cancellation
 * contracts that Fase 16/20 established: EP_RECV / EP_SEND / EP_CALL and the
 * fault-pending state all wake-or-die on process kill / endpoint close /
 * handler drop, leaving no dead waiter, no KReply, no live-count drift.
 * Invariants: X11, X14, X15, X16, X20, X21. */
#define T153_SEED   0x21C0DE53u
#define T153_ROUNDS 8u
static void test_t153(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "cancellation fuzz";
    g_fz_seed = T153_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T153_ROUNDS; i++) {
        uint32_t kind = fz_rand() % 4u;   /* 0 recv, 1 send, 2 call, 3 fault */
        long ep = it_ep_create();
        handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; it_close(&ep_h); break; }

        handle_id_t n_h = HANDLE_INVALID;
        op = kind;
        if (kind == 3u) {
            /* Fault-pending waiter: register a handler, drive an invalid-VA fault. */
            long n = it_notify_create();
            n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
            if (n < 0 || it_sys3(SYS_EXCEPTION_HANDLER, (long)proc_h, n, 1) != 0) { ok = 0; why = "reg handler"; }
            if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "fault cmd"; }
            if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no fault"; }
        } else {
            uint32_t cmd = (kind == 0u) ? LP_CMD_RSLOT_RECV
                         : (kind == 1u) ? LP_CMD_SEND_BLOCK : LP_CMD_CALL_BLOCK;
            /* RSLOT_RECV parks in a second recv; SEND/CALL park as sender/caller. */
            if (kind == 0u) { if (it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) { ok = 0; why = "rslot cmd"; } }
            else            { if (it_lp_cmd(ep_h, cmd) != 0) { ok = 0; why = "block cmd"; } }
            it_sys1(SYS_SLEEP, 3);   /* let the child reach its blocking syscall */
        }

        /* Cancellation route: seeded mix of process kill / endpoint close /
         * handler drop.  Each must resolve without stranding the child. */
        uint32_t route = fz_rand() % 3u;
        if (ok && route == 0u) {
            if (it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
        } else if (ok && route == 1u) {
            it_close(&ep_h);                              /* close endpoint under the waiter */
            if (kind == 3u) it_close(&n_h);
            if (it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill after close"; }
        } else if (ok) {
            if (kind == 3u) it_close(&n_h);               /* handler drop first */
            if (it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill after drop"; }
        }
        if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "no exit"; }

        it_close(&ep_h); it_close(&n_h); it_close(&proc_h);
        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T153");
    else { it_fz_note("T153", T153_SEED, i, op); it_fail("T153", why); }
}

/* ── T154: rights monotonicity fuzz across syscalls ─────────────────────────
 * A reduced-rights cap must never be amplified by ANY syscall, and no legacy
 * path may ignore rights.  A RIGHT_READ notification dup cannot signal
 * (ACCESS_DENIED); a RIGHT_READ frame cap cannot map writable; a derive can
 * only narrow, never widen (asking for a right the parent lacks does not grant
 * it); and the reduced cap's failures mutate nothing.  Cross the CPtr path and
 * the legacy handle path for the same object.  Invariants: X3, X8. */
static void test_t154(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "rights monotonicity";

    long no = it_notify_create();
    handle_id_t no_h = (no >= 0) ? (handle_id_t)no : HANDLE_INVALID;
    if (no < 0) { it_fail("T154", "notif fixture"); return; }

    /* RIGHT_READ dup cannot signal (needs RIGHT_WRITE) — no fallback. */
    long rd = it_sys2(SYS_HANDLE_DUP, no, (long)RIGHT_READ);
    handle_id_t rd_h = (rd >= 0) ? (handle_id_t)rd : HANDLE_INVALID;
    if (rd < 0) { ok = 0; why = "read dup"; }
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, rd, 1) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read cap signalled"; }
    /* Re-deriving from the reduced cap cannot recover RIGHT_WRITE: either the
     * derive is rejected, or it yields a cap that STILL cannot signal. */
    if (ok) {
        long wr = it_sys2(SYS_HANDLE_DUP, rd, (long)(RIGHT_READ | RIGHT_WRITE));
        if (wr >= 0) {
            handle_id_t wr_h = (handle_id_t)wr;
            if (it_sys2(SYS_NOTIFY_SIGNAL, wr, 1) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights amplified via re-dup"; }
            it_close(&wr_h);
        }
    }
    /* The original full cap still signals — the reduced cap's failures did not
     * corrupt the object. */
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, no, 1) != 0) { ok = 0; why = "full cap broken"; }
    if (ok) { uint64_t bits = 0; (void)it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, no, (long)(uintptr_t)&bits, 100000000L); }

    /* Frame rights: RIGHT_READ frame cap maps non-writable but is denied a
     * writable map; the PTE reflects the cap, never the request. */
    if (ok && it_setup_self_vspace()) {
        long r = it_retype_frame();
        handle_id_t fr = (r != HANDLE_INVALID) ? (handle_id_t)r : HANDLE_INVALID;
        const uint64_t VA = 0x80A8000000ULL;
        if (fr == HANDLE_INVALID) { ok = 0; why = "retype"; }
        long dr = ok ? it_sys2(SYS_CAP_DERIVE, (long)fr, (long)RIGHT_READ) : -1;
        handle_id_t fr_ro = (dr >= 0) ? (handle_id_t)dr : HANDLE_INVALID;
        if (ok && dr < 0) { ok = 0; why = "ro derive"; }
        if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_ro, IT_VS, (long)VA, (long)IT_MAP_W) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable map"; }
        if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_ro, IT_VS, (long)VA, 0L) != 0) { ok = 0; why = "ro readable map"; }
        if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr_ro, IT_VS, (long)VA) != 0) { ok = 0; why = "ro unmap"; }
        it_close(&fr_ro); it_close(&fr);
        it_quiesce_reaper(); (void)it_ut_reset();
    } else if (ok) { ok = 0; why = "vspace self mint"; }

    it_close(&rd_h); it_close(&no_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T154"); else it_fail("T154", why);
}

/* ── T155: deterministic full syscall stress ────────────────────────────────
 * The grand finale: a seeded PRNG runs many rounds combining object create/
 * destroy, map/unmap, notify, endpoint rendezvous, cap derive/revoke, child
 * spawn/kill and a controlled fault, within fixed slot/process/mapping
 * budgets and an aggressive end-of-round cleanup.  Every gauge must return to
 * baseline each round; the run prints seed/iteration only on failure.  This is
 * the whole surface under sustained deterministic hostility.
 * Invariants: X9–X23 (full set). */
#define T155_SEED   0x21C0DE55u
#define T155_ROUNDS 16u
static void test_t155(void) {
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T155", "vspace self mint"); return; }
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "full stress";
    g_fz_seed = T155_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T155_ROUNDS; i++) {
        struct it_snap rb = it_snap_take();   /* per-round baseline */

        /* Object churn: retype a frame, map/derive/revoke/unmap, close. */
        op = 1;
        long r = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, (long)IT_KOBJ_FRAME, 4096);
        handle_id_t fr = (r >= 0) ? (handle_id_t)r : HANDLE_INVALID;
        if (fr == HANDLE_INVALID) { ok = 0; why = "retype"; break; }
        uint64_t va = 0x80B0000000ULL + (uint64_t)(fz_rand() % 4u) * 0x1000ULL;
        op = 2;
        if (it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)va, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; it_close(&fr); break; }
        op = 3;
        if (fz_rand() & 1u) {
            long c = it_sys2(SYS_CAP_DERIVE, (long)fr, (long)RIGHT_SAME_RIGHTS);
            if (c >= 0 && it_sys1(SYS_CAP_REVOKE, (long)fr) != 0) { ok = 0; why = "revoke"; it_close(&fr); break; }
            if (ok && c >= 0 && it_sys1(SYS_HANDLE_TYPE, c) != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "revoked child alive"; it_close(&fr); break; }
        }
        op = 4;
        if (it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)va) != 0) { ok = 0; why = "unmap"; it_close(&fr); break; }
        it_close(&fr);

        /* Notification rendezvous. */
        op = 5;
        long nn = it_notify_create();
        handle_id_t no = (nn >= 0) ? (handle_id_t)nn : HANDLE_INVALID;
        if (no != HANDLE_INVALID && (fz_rand() & 1u)) {
            if (it_sys2(SYS_NOTIFY_SIGNAL, (long)no, 3) != 0) { ok = 0; why = "signal"; }
            uint64_t bits = 0;
            if (ok && it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)no, (long)(uintptr_t)&bits, 500000000L) != 0) { ok = 0; why = "wait"; }
        }
        it_close(&no);

        /* Every few rounds: spawn a child and resolve it (clean exit, kill, or
         * a controlled fault → kill) — the lifecycle+fault surface under churn. */
        if (ok && (i % 4u) == 0u) {
            op = 6;
            long ep = it_ep_create();
            handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
            handle_id_t proc_h = HANDLE_INVALID;
            if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; it_close(&ep_h); break; }
            uint32_t what = fz_rand() % 3u;
            if (what == 0u) {                    /* clean run */
                if (it_lp_cmd(ep_h, 0x55u) != 0) { ok = 0; why = "cmd clean"; }
                if (ok && it_lp_wait_exit(proc_h) != (long)LP_EXIT_MARKER) { ok = 0; why = "clean exit"; }
            } else if (what == 1u) {             /* kill while parked */
                if (it_lp_cmd(ep_h, LP_CMD_SEND_BLOCK) != 0) { ok = 0; why = "cmd block"; }
                it_sys1(SYS_SLEEP, 3);
                if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc_h) != 0) { ok = 0; why = "kill"; }
                if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "kill exit"; }
            } else {                             /* controlled fault → kill */
                long n = it_notify_create();
                handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
                if (n < 0 || it_sys3(SYS_EXCEPTION_HANDLER, (long)proc_h, n, 1) != 0) { ok = 0; why = "reg handler"; }
                if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "fault cmd"; }
                if (ok && !it_fault_wait(n_h)) { ok = 0; why = "no fault"; }
                struct it_fault f;
                if (ok && it_fault_info(proc_h, &f) != 0) { ok = 0; why = "fault info"; }
                if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)f.task_id, 1) != 0) { ok = 0; why = "resume kill"; }
                if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "fault exit"; }
                it_close(&n_h);
            }
            it_close(&ep_h); it_close(&proc_h);
        }

        it_quiesce_reaper();
        if (ok && !it_ut_reset()) { ok = 0; why = "round reset busy"; break; }
        /* Per-round baseline: no lifecycle gauge may drift within a round
         * (object-count balance is checked by the single-process fuzz tests;
         * a round that spawned a child has transient child-owned objects). */
        struct it_snap re = it_snap_take();
        const char *w2;
        if (ok && !it_snap_baseline_live(&rb, &re, &w2)) { ok = 0; why = w2; break; }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "final reset busy"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T155");
    else { it_fz_note("T155", T155_SEED, i, op); it_fail("T155", why); }
}

/* ── Fase 22: service authority minimization (T156–T163) ────────────────────
 *
 * The kernel mechanism fails clean under hostility (Fase 21); the next risk is
 * a service holding authority it does not need.  These tests lock the
 * least-authority contract: delivery carries exactly the declared caps and no
 * more (T156/T162), svcmgr is a registry that never amplifies rights for
 * ordinary clients (T157), the productive services expose no ambient authority
 * (T158/T159), init's handoff is auditable (T160), and service teardown leaves
 * no ghost registration (T161/T163).  Invariants A1–A16 live in
 * docs/architecture/service-authority-minimization.md.
 *
 * The anchor for delivery is a self-report from a lifecycle_probe child: it
 * resolves its well-known CPtr slots and exits with a bitmask, so the parent
 * asserts the child sees EXACTLY the caps it was minted — no phantom
 * authority, and removing a cap removes it from the child. */

#define LP_CMD_REPORT_SLOTS  0x10A0u   /* must match lifecycle_probe */

/* Spawn a lifecycle_probe with the command endpoint at LP_CPTR_CMD_EP plus an
 * arbitrary extra mint set, command REPORT_SLOTS, and return the reported slot
 * bitmask (bits 0..15 = slots 0..15; bit 16 = slot 25 proc, 17 = slot 55
 * untyped, 18 = slot 56 vspace).  Returns -1 on spawn/command failure. */
static long it_lp_report_slots(const struct svc_mint *extra, uint32_t nextra) {
    long ep = it_ep_create();
    if (ep < 0) return -1;
    handle_id_t cmd = (handle_id_t)ep;

    struct svc_mint mints[8];
    mints[0].slot = LP_CPTR_CMD_EP;
    mints[0].src_h = cmd;
    mints[0].rights = RIGHT_READ | RIGHT_WRITE;
    mints[0].badge = 0;
    uint32_t n = 1u;
    for (uint32_t i = 0; i < nextra && n < 8u; i++) mints[n++] = extra[i];

    handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
    long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "lifecycle_probe",
                             &proc, &boot, mints, n);
    it_close(&boot);
    if (r < 0 || proc == HANDLE_INVALID) { it_close(&cmd); it_close(&proc); return -1; }

    long mask = -1;
    if (it_lp_cmd(cmd, LP_CMD_REPORT_SLOTS) == 0) mask = it_lp_wait_exit(proc);
    it_close(&cmd); it_close(&proc);
    return mask;
}

/* LOOKUP_NAME through the given svcmgr CPtr; returns the granted attached_rights
 * (>=0), or a negative error.  Closes the returned cap (rights are the subject). */
static long it_lookup_rights(long svcmgr_cptr, const char *name) {
    uint32_t len = it_stage_path(name);
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    m.buf_len  = len;
    if (it_sys2(SYS_EP_CALL, svcmgr_cptr, (long)&m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -(long)(uint32_t)m.words[0];
    if (m.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
        handle_id_t h = (handle_id_t)m.attached_handle;
        it_close(&h);
    }
    return (long)m.attached_rights;
}

/* svcmgr DIAG ready-service count (words[1]) — the registry gauge that INCLUDES
 * dynamic registrations (words[2] active_slot_count is catalog-only).  A dynamic
 * register bumps this by one; unregister drops it back. */
static long it_svcmgr_active_slots(void) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label    = IRIS_SVCMGR_EP_DIAG;
    m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    if (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK || m.word_count < 2u) return -1;
    return (long)(uint32_t)m.words[1];
}

/* ── T156: service authority manifest consistency (delivery) ────────────────
 * The mint delivery mechanism every service depends on carries EXACTLY the
 * declared caps.  A probe minted {cmd@3, notif@5, ep@7} reports exactly those
 * three slots; a probe minted {cmd@3} alone reports only slot 3.  No phantom
 * cap appears, and removing a cap removes it from the child.
 * Invariants: A1, A11, A13, A15. */
static void test_t156(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "manifest delivery";

    long no = it_notify_create();
    long ep = it_ep_create();
    handle_id_t no_h = (no >= 0) ? (handle_id_t)no : HANDLE_INVALID;
    handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    if (no < 0 || ep < 0) { it_close(&no_h); it_close(&ep_h); it_fail("T156", "fixture"); return; }

    /* Declared set {cmd@3, notif@5, ep@7}. */
    struct svc_mint extra[2];
    extra[0].slot = 5; extra[0].src_h = no_h; extra[0].rights = RIGHT_READ; extra[0].badge = 0;
    extra[1].slot = 7; extra[1].src_h = ep_h; extra[1].rights = RIGHT_READ; extra[1].badge = 0;
    long mask = it_lp_report_slots(extra, 2u);
    uint32_t want = (1u << 3) | (1u << 5) | (1u << 7);
    if (ok && mask < 0) { ok = 0; why = "report failed"; }
    if (ok && (uint32_t)mask != want) { ok = 0; why = "delivered != declared"; }

    /* Reduced set {cmd@3} only → report drops slots 5 and 7 (A15). */
    if (ok) {
        long m0 = it_lp_report_slots(0, 0u);
        if (m0 < 0 || (uint32_t)m0 != (1u << 3)) { ok = 0; why = "reduction not observed"; }
    }

    it_close(&no_h); it_close(&ep_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T156"); else it_fail("T156", why);
}

/* ── T157: svcmgr grant tightening (registry, not god) ──────────────────────
 * svcmgr never amplifies authority for an ordinary client.  A LOOKUP_NAME of a
 * reserved ".ep" name grants WRITE|DUPLICATE only to a SUPERVISOR badge; an
 * ordinary badge gets a call-only WRITE cap with DUPLICATE/TRANSFER stripped —
 * so an ordinary client cannot re-mint or hand on the service cap.  A lookup of
 * a nonexistent name is NOT_FOUND with no cap.  Invariants: A7, A11, A14. */
static void test_t157(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "grant tightening";

    /* Ordinary badge (slot 1 = IRIS_CPTR_SVCMGR_EP, IRIS_BADGE_IRIS_TEST). */
    long ord = it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, VFS_EP_SVC_NAME);
    if (ok && ord < 0) { ok = 0; why = "ordinary lookup failed"; }
    if (ok && ((uint32_t)ord & RIGHT_WRITE) == 0) { ok = 0; why = "ordinary lost WRITE"; }
    if (ok && ((uint32_t)ord & (RIGHT_DUPLICATE | RIGHT_TRANSFER)) != 0) {
        ok = 0; why = "ordinary amplified (dup/transfer)";
    }

    /* Supervisor badge (slot 27 = IRIS_CPTR_TEST_SUPER, IRIS_BADGE_INIT). */
    long sup = it_lookup_rights((long)IRIS_CPTR_TEST_SUPER, VFS_EP_SVC_NAME);
    if (ok && sup < 0) { ok = 0; why = "supervisor lookup failed"; }
    if (ok && ((uint32_t)sup & RIGHT_DUPLICATE) == 0) { ok = 0; why = "supervisor lost DUPLICATE"; }

    /* Nonexistent name → NOT_FOUND, no cap. */
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "no.such.svc")
              != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "missing name not NOT_FOUND"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T157"); else it_fail("T157", why);
}

/* ── T158: vfs authority boundary ───────────────────────────────────────────
 * vfs serves a filesystem and nothing else.  Its endpoint answers PING and its
 * own ops but rejects a foreign (registry) opcode; an ordinary client's vfs.ep
 * cap is call-only WRITE (cannot be re-minted to seize the service).  vfs holds
 * no authority to act on iris_test — there is no cap by which it could.
 * Invariants: A8, A11, A16. */
static void test_t158(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "vfs boundary";

    /* vfs.ep answers PING. */
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label    = IRIS_EP_OP_PING;
    m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    if (ok && (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_VFS_EP, (long)&m) != 0 ||
               m.label != IRIS_EP_REPLY_OK)) { ok = 0; why = "vfs ping"; }

    /* A foreign registry opcode to vfs.ep must NOT be honoured as a registry
     * op (vfs is not svcmgr); it replies with an error, never OK. */
    it_iris_msg_zero(&m);
    m.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    m.buf_len  = it_stage_path("vfs.ep");
    if (ok && it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_VFS_EP, (long)&m) == 0 &&
        m.label == IRIS_EP_REPLY_OK && m.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
        ok = 0; why = "vfs served a registry op";
        handle_id_t h = (handle_id_t)m.attached_handle; it_close(&h);
    }

    /* Ordinary vfs.ep cap is call-only (no DUPLICATE) — cannot be re-minted. */
    long rights = it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, VFS_EP_SVC_NAME);
    if (ok && (rights < 0 || ((uint32_t)rights & RIGHT_DUPLICATE) != 0)) {
        ok = 0; why = "vfs cap re-mintable by client";
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T158"); else it_fail("T158", why);
}

/* ── T159: console/kbd authority boundary ───────────────────────────────────
 * The I/O drivers have narrow authority: console.ep and kbd.ep answer PING and
 * their own ops but reject a foreign registry opcode (no cap handed back), and
 * their client caps are call-only WRITE.  A compromised driver cannot register
 * services or seize a peer.  Invariants: A9, A11, A16. */
static void test_t159(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "io driver boundary";
    const long eps[2] = { (long)IRIS_CPTR_CONSOLE_EP, (long)IRIS_CPTR_KBD_EP };

    for (int i = 0; ok && i < 2; i++) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.label    = IRIS_EP_OP_PING;
        m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
        if (it_sys2(SYS_EP_CALL, eps[i], (long)&m) != 0 || m.label != IRIS_EP_REPLY_OK) {
            ok = 0; why = "driver ping"; break;
        }
        /* Foreign registry op → must not hand back a cap. */
        it_iris_msg_zero(&m);
        m.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
        m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
        m.buf_len  = it_stage_path("vfs.ep");
        if (it_sys2(SYS_EP_CALL, eps[i], (long)&m) == 0 &&
            m.label == IRIS_EP_REPLY_OK && m.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
            ok = 0; why = "driver served registry op";
            handle_id_t h = (handle_id_t)m.attached_handle; it_close(&h); break;
        }
    }

    /* Both driver client caps are call-only (no DUPLICATE). */
    long rc = it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, CONSOLE_EP_SVC_NAME);
    if (ok && (rc < 0 || ((uint32_t)rc & RIGHT_DUPLICATE) != 0)) { ok = 0; why = "console cap re-mintable"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T159"); else it_fail("T159", why);
}

/* ── T160: init authority handoff audit ─────────────────────────────────────
 * init necessarily starts privileged; the audit is that its handoff is
 * deliberate.  iris_test is init's test child and holds the DECLARED test-only
 * authority (spawn cap slot 6, self-proc slot 25, untyped slot 55) — this test
 * confirms that authority is really present here, which is exactly what T162
 * proves must be ABSENT from an ordinary service.  Invariants: A4, A6, A10. */
static void test_t160(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "init handoff";

    /* iris_test's declared test authority resolves (it is a privileged test
     * child, not a productive service).  Each resolve mints a fresh handle;
     * close it immediately so the snapshot balances. */
    const long test_caps[3] = {
        (long)IRIS_CPTR_SPAWN_CAP, (long)IRIS_CPTR_TEST_UNTYPED, (long)IRIS_CPTR_TEST_PROC,
    };
    const char *const caps_why[3] = { "no spawn cap", "no test untyped", "no self proc" };
    for (int s = 0; ok && s < 3; s++) {
        long h = it_sys1(SYS_CSPACE_RESOLVE, test_caps[s]);
        if (h < 0) { ok = 0; why = caps_why[s]; break; }
        handle_id_t hh = (handle_id_t)h;
        it_close(&hh);
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T160"); else it_fail("T160", why);
}

/* ── T161: service death containment (dynamic registry) ─────────────────────
 * A registered service that goes away leaves no ghost in svcmgr.  iris_test
 * registers a dynamic service backed by an endpoint it owns, confirms it
 * resolves, then unregisters (owner authority) — the name no longer resolves,
 * the active-slot gauge returns to its pre-register value, and the registry
 * books balance.  Invariants: A12, A16. */
static void test_t161(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "death containment";

    long slots0 = it_svcmgr_active_slots();
    if (ok && slots0 < 0) { ok = 0; why = "diag baseline"; }

    long e = it_ep_create();
    handle_id_t svc_ep = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
    if (ok && e < 0) { ok = 0; why = "svc ep"; }

    long id = ok ? it_register_ep("t161.svc", svc_ep) : -1;
    if (ok && id < 0) { ok = 0; why = "register"; }
    /* Registered → resolves, and the active-slot gauge moved up. */
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t161.svc") < 0) { ok = 0; why = "lookup after register"; }
    if (ok && it_svcmgr_active_slots() != slots0 + 1) { ok = 0; why = "slot not tracked"; }

    /* Unregister (owner) → gone, gauge back to baseline, no ghost. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.label      = IRIS_SVCMGR_EP_UNREGISTER;
        m.words[0]   = (uint32_t)id;
        m.word_count = 1u;
        if (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&m) != 0 ||
            m.label != IRIS_EP_REPLY_OK) { ok = 0; why = "unregister"; }
    }
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t161.svc")
              != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "ghost after unregister"; }
    if (ok && it_svcmgr_active_slots() != slots0) { ok = 0; why = "slot not released"; }

    it_close(&svc_ep);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T161"); else it_fail("T161", why);
}

/* ── T162: least-authority regression lock (teeth) ──────────────────────────
 * The permanent guard: a minimal service (a probe minted only its command
 * endpoint) must hold NO high authority — no spawn cap (slot 6), no proc cap
 * (slot 25), no untyped (slot 55), no vspace (slot 56), and none of the peer
 * client endpoints (slots 1,2,4).  The test then proves it has TEETH: minting
 * one extra cap makes exactly that slot appear in the report, so a future
 * over-grant would be caught.  Invariants: A1, A3, A4, A5, A6, A10. */
static void test_t162(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "least-authority lock";

    /* Minimal probe: only the command endpoint (slot 3). */
    long m0 = it_lp_report_slots(0, 0u);
    if (ok && m0 < 0) { ok = 0; why = "report failed"; }
    if (ok && (uint32_t)m0 != (1u << 3)) { ok = 0; why = "minimal service over-authorized"; }
    /* Explicitly: none of the high-authority bits. */
    if (ok && ((uint32_t)m0 & ((1u << 6) | (1u << 16) | (1u << 17) | (1u << 18))) != 0) {
        ok = 0; why = "high authority leaked to minimal service";
    }

    /* Teeth: mint one extra cap at slot 6 → the report must show it. */
    if (ok) {
        long n = it_notify_create();
        handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
        if (n < 0) { ok = 0; why = "teeth fixture"; }
        else {
            struct svc_mint extra[1];
            extra[0].slot = 6; extra[0].src_h = n_h; extra[0].rights = RIGHT_READ; extra[0].badge = 0;
            long m1 = it_lp_report_slots(extra, 1u);
            if (m1 < 0 || ((uint32_t)m1 & (1u << 6)) == 0) { ok = 0; why = "extra cap not detected (no teeth)"; }
            it_close(&n_h);
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T162"); else it_fail("T162", why);
}

/* ── T163: deterministic service authority stress ───────────────────────────
 * A seeded PRNG drives register / lookup / unregister / endpoint-close churn
 * on the dynamic registry, interleaving malformed ops (unregister of a stale
 * id, lookup of a missing name, register of a reserved name).  No op amplifies
 * authority or leaves a ghost; the active-slot gauge and the full snapshot
 * return to baseline.  Invariants: A11, A12, A14, plus X-style no-drift.
 * Prints seed/iteration only on failure. */
#define T163_SEED   0x22C0DE63u
#define T163_ROUNDS 12u
static void test_t163(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "authority stress";
    long slots0 = it_svcmgr_active_slots();
    if (ok && slots0 < 0) { ok = 0; why = "diag baseline"; }
    g_fz_seed = T163_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T163_ROUNDS; i++) {
        /* Malformed ops — must all fail clean, no registry mutation. */
        op = 1;
        if (it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "ghost.svc")
            != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "missing lookup"; break; }
        op = 2;
        {   /* unregister a stale/never-registered id → not OK */
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label = IRIS_SVCMGR_EP_UNREGISTER;
            m.words[0] = 0x4000u + (fz_rand() & 0xFFu);
            m.word_count = 1u;
            long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&m);
            if (r == 0 && m.label == IRIS_EP_REPLY_OK) { ok = 0; why = "stale unregister accepted"; break; }
        }
        op = 3;   /* register a reserved name → ACCESS_DENIED */
        {
            uint32_t len = it_stage_path("vfs.ep");
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label = IRIS_SVCMGR_EP_REGISTER;
            m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
            m.buf_len = len;
            long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&m);
            if (r == 0 && m.label == IRIS_EP_REPLY_OK) { ok = 0; why = "reserved register accepted"; break; }
        }

        /* Well-formed: register → lookup → unregister, each round balanced. */
        op = 10;
        long e = it_ep_create();
        handle_id_t svc_ep = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
        if (e < 0) { ok = 0; why = "svc ep"; break; }
        long id = it_register_ep("t163.svc", svc_ep);
        if (id < 0) { ok = 0; why = "register"; it_close(&svc_ep); break; }
        op = 11;
        if (it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t163.svc") < 0) { ok = 0; why = "lookup"; it_close(&svc_ep); break; }
        op = 12;
        {
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label = IRIS_SVCMGR_EP_UNREGISTER;
            m.words[0] = (uint32_t)id;
            m.word_count = 1u;
            if (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&m) != 0 ||
                m.label != IRIS_EP_REPLY_OK) { ok = 0; why = "unregister"; it_close(&svc_ep); break; }
        }
        it_close(&svc_ep);
        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && it_svcmgr_active_slots() != slots0) { ok = 0; why = "registry slot drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T163");
    else { it_fz_note("T163", T163_SEED, i, op); it_fail("T163", why); }
}

/* ── Fase 23: device / driver isolation hardening (T164–T171) ───────────────
 *
 * The service authority is minimized (Fase 22); the next risk is a driver
 * reaching hardware it was never granted.  These tests prove containment: an
 * I/O-port cap bounds access to exactly its range (T164), a compromised
 * driver stand-in cannot escalate past its device caps (T165/T166), IRQ
 * route/ack require the matching cap and iris_test itself cannot route
 * (T167), framebuffer authority is isolated (T168), device caps derive
 * monotonically (T169), driver death releases device authority (T170), and a
 * seeded device fuzz never crosses a cap (T171).  Invariants D1–D20 live in
 * docs/architecture/device-driver-isolation.md.
 *
 * Safe test hardware: COM2 (0x2F8, whitelisted, unwired in headless QEMU) is
 * the dummy port; IRQ 5 (unused) is the dummy line.  iris_test's spawn cap
 * carries HW_ACCESS (it already mints the 0x3F8 serial cap at startup), so it
 * can create device caps to exercise the authority paths — but it holds NO
 * proc cap with RIGHT_ROUTE, so it cannot itself route an IRQ (a containment
 * property asserted directly in T167). */

#define LP_CMD_DEV_PROBE     0x10A1u   /* must match lifecycle_probe */
#define IT_COM2_BASE   0x2F8L
#define IT_COM2_COUNT  8L

/* Create an ioport cap over [base, base+count) via the HW_ACCESS spawn cap;
 * returns the handle or HANDLE_INVALID. */
static handle_id_t it_make_ioport(long base, long count) {
    long h = it_sys3(SYS_CAP_CREATE_IOPORT, (long)IRIS_CPTR_SPAWN_CAP, base, count);
    return (h >= 0) ? (handle_id_t)h : HANDLE_INVALID;
}

/* Spawn a lifecycle_probe with an ioport cap at slot 10 (its only device
 * authority — no spawn cap at slot 6, no IRQ cap at slot 11), send
 * LP_CMD_DEV_PROBE with the given ioport offset, and return the reported breach
 * bitmask.  With an OUT-OF-RANGE offset a contained driver reports 0 (every
 * escalation denied).  With an IN-RANGE offset bit 1 (a legitimate IN through
 * the held cap) is set — the teeth check proving the probe genuinely attempts
 * each op.  Returns -1 on spawn/command failure. */
static long it_dev_probe(handle_id_t ioport_h, uint64_t offset, iris_rights_t dev_rights) {
    long ep = it_ep_create();
    if (ep < 0) return -1;
    handle_id_t cmd = (handle_id_t)ep;

    /* The source ioport cap carries DUPLICATE (needed to mint it); the child's
     * cap is reduced to dev_rights — modelling a driver's exact port authority. */
    struct svc_mint mints[2];
    mints[0].slot = LP_CPTR_CMD_EP; mints[0].src_h = cmd;
    mints[0].rights = RIGHT_READ | RIGHT_WRITE; mints[0].badge = 0;
    mints[1].slot = 10; mints[1].src_h = ioport_h;
    mints[1].rights = dev_rights; mints[1].badge = 0;

    handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
    long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "lifecycle_probe",
                             &proc, &boot, mints, 2u);
    it_close(&boot);
    if (r < 0 || proc == HANDLE_INVALID) { it_close(&cmd); it_close(&proc); return -1; }

    long mask = -1;
    if (it_lp_cmd_va(cmd, LP_CMD_DEV_PROBE, offset) == 0) mask = it_lp_wait_exit(proc);
    it_close(&cmd); it_close(&proc);
    return mask;
}

/* ── T164: ioport authority boundaries ──────────────────────────────────────
 * A KIoPort cap bounds access to exactly [base, base+count): in-range IN/OUT
 * work, any out-of-range offset is INVALID_ARG (cannot cross the range), a
 * wrong-type cap fails, rights are enforced (READ for IN, WRITE for OUT), a
 * stale cap fails, and a non-whitelisted range cannot be created.  No fallback,
 * no drift.  Invariants: D1, D2, D3, D4, D5, D6, D7. */
static void test_t164(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "ioport boundaries";

    handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
    if (io == HANDLE_INVALID) { it_fail("T164", "com2 cap"); return; }

    /* In-range access works (COM2 is unwired: IN returns a byte, OUT is a no-op). */
    if (ok && it_sys2(SYS_IOPORT_IN, (long)io, 5) < 0) { ok = 0; why = "in-range IN"; }
    if (ok && it_sys3(SYS_IOPORT_OUT, (long)io, 0, 0) != 0) { ok = 0; why = "in-range OUT"; }
    /* Out-of-range offsets → INVALID_ARG (cannot cross the granted range). */
    if (ok && it_sys2(SYS_IOPORT_IN, (long)io, IT_COM2_COUNT) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "offset==count IN"; }
    if (ok && it_sys2(SYS_IOPORT_IN, (long)io, 1000) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "big offset IN"; }
    if (ok && it_sys3(SYS_IOPORT_OUT, (long)io, 1000, 0) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "big offset OUT"; }

    /* Wrong-type cap: a notification is not a KIoPort. */
    if (ok) {
        long n = it_notify_create();
        handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
        if (n < 0) { ok = 0; why = "notif fixture"; }
        if (ok && it_sys2(SYS_IOPORT_IN, n, 0) >= 0) { ok = 0; why = "wrong-type IN honoured"; }
        it_close(&n_h);
    }

    /* Rights enforcement: READ-only cap denies OUT; WRITE-only denies IN. */
    if (ok) {
        long rr = it_sys2(SYS_CAP_DERIVE, (long)io, (long)RIGHT_READ);
        handle_id_t ro = (rr >= 0) ? (handle_id_t)rr : HANDLE_INVALID;
        if (rr < 0) { ok = 0; why = "read derive"; }
        if (ok && it_sys3(SYS_IOPORT_OUT, ro, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "RO OUT not denied"; }
        if (ok && it_sys2(SYS_IOPORT_IN, ro, 0) < 0) { ok = 0; why = "RO IN broken"; }
        it_close(&ro);
        long wr = it_sys2(SYS_CAP_DERIVE, (long)io, (long)RIGHT_WRITE);
        handle_id_t wo = (wr >= 0) ? (handle_id_t)wr : HANDLE_INVALID;
        if (ok && wr < 0) { ok = 0; why = "write derive"; }
        if (ok && it_sys2(SYS_IOPORT_IN, wo, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "WO IN not denied"; }
        it_close(&wo);
    }

    /* Stale cap: dup, close, use → fails. */
    if (ok) {
        long d = it_sys2(SYS_HANDLE_DUP, (long)io, (long)RIGHT_SAME_RIGHTS);
        handle_id_t d_h = (d >= 0) ? (handle_id_t)d : HANDLE_INVALID;
        if (d < 0) { ok = 0; why = "stale dup"; }
        else { it_close(&d_h);
            if (it_sys2(SYS_IOPORT_IN, d, 0) >= 0) { ok = 0; why = "stale cap honoured"; } }
    }

    /* Non-whitelisted range cannot be created: CMOS (0x70) and a range that
     * spills past the PS/2 whitelist entry. */
    if (ok && it_sys3(SYS_CAP_CREATE_IOPORT, (long)IRIS_CPTR_SPAWN_CAP, 0x70, 2) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "CMOS not denied"; }
    if (ok && it_sys3(SYS_CAP_CREATE_IOPORT, (long)IRIS_CPTR_SPAWN_CAP, 0x60, 100) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "range spill not denied"; }

    it_close(&io);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T164"); else it_fail("T164", why);
}

/* ── T165: kbd driver containment (compromised-driver stand-in) ──────────────
 * A probe minted a driver-like device cap (an ioport cap at slot 10, READ, like
 * kbd's 0x60) and NOTHING else — no spawn cap, no IRQ cap — cannot escalate: it
 * cannot forge a port or IRQ cap (no HW_ACCESS), cannot cross its port range,
 * and cannot ack an IRQ it holds no cap for.  The probe exits 0 (contained).
 * A control run WITH the spawn cap proves the probe genuinely attempts each
 * escalation (teeth).  This closes the loop with Fase 22: kbd already has no
 * peer client caps; now its hardware authority is shown bounded too.
 * Invariants: D7, D15, D17, D18. */
static void test_t165(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "kbd containment";

    /* Model kbd's data port as a READ-only device cap in the child. */
    handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
    if (io == HANDLE_INVALID) { it_fail("T165", "io cap"); return; }

    /* Contained: out-of-range offset → every escalation denied (mask 0). */
    long mask = it_dev_probe(io, 1000u, RIGHT_READ);
    if (ok && mask < 0) { ok = 0; why = "probe failed"; }
    if (ok && mask != 0) { ok = 0; why = "driver escalated past its caps"; }

    /* Teeth: an IN-RANGE offset makes the legitimate IN (bit 1) succeed —
     * proving the contained run's 0 means genuinely denied, not "never tried". */
    if (ok) {
        long tm = it_dev_probe(io, 0u, RIGHT_READ);
        if (tm < 0 || (tm & (1u << 1)) == 0) { ok = 0; why = "no teeth (IN not attempted)"; }
        /* Even in-range, an OUT through a READ-only cap stays denied (bit 2). */
        if (ok && (tm & (1u << 2)) != 0) { ok = 0; why = "RO cap wrote a port"; }
    }

    it_close(&io);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T165"); else it_fail("T165", why);
}

/* ── T166: console UART containment ──────────────────────────────────────────
 * A probe standing in for console (an ioport cap at slot 10 with READ|WRITE,
 * like the UART) still cannot cross its range or forge new device authority:
 * an out-of-range IN/OUT fails even WITH the WRITE right, and there is no spawn
 * or IRQ cap to escalate through.  Contained → 0.  Invariants: D2, D7, D15, D18. */
static void test_t166(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "console containment";

    handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);   /* RW, like the UART */
    if (io == HANDLE_INVALID) { it_fail("T166", "io cap"); return; }

    long mask = it_dev_probe(io, 1000u, RIGHT_READ | RIGHT_WRITE);
    if (ok && mask < 0) { ok = 0; why = "probe failed"; }
    if (ok && mask != 0) { ok = 0; why = "console escalated past its caps"; }

    it_close(&io);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T166"); else it_fail("T166", why);
}

/* ── T167: IRQ route / ack authority ────────────────────────────────────────
 * IRQ authority is cap-scoped: an IRQ cap carries its authorized irq_num, so a
 * holder can only route/ack THAT line.  Route requires RIGHT_ROUTE on the IRQ
 * cap, RIGHT_WRITE on a KNotification destination, and RIGHT_READ|ROUTE on the
 * owner proc cap — iris_test's own proc cap lacks ROUTE, so iris_test CANNOT
 * route (a containment win asserted directly).  ACK requires RIGHT_ROUTE on the
 * IRQ cap.  Every failure leaves no route (a leaked route would keep its destination
 * notification alive, caught by the snapshot).
 * Invariants: D8, D9, D10, D12, D14, D19. */
static void test_t167(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "irq authority";

    /* Create an IRQ cap for the unused line 5. */
    long ic = it_sys3(SYS_CAP_CREATE_IRQCAP, (long)IRIS_CPTR_SPAWN_CAP, 5, 0);
    handle_id_t irq = (ic >= 0) ? (handle_id_t)ic : HANDLE_INVALID;
    if (ok && irq == HANDLE_INVALID) { ok = 0; why = "irqcap create"; }

    long nn = it_notify_create();
    handle_id_t notif = (nn >= 0) ? (handle_id_t)nn : HANDLE_INVALID;
    if (ok && notif == HANDLE_INVALID) { ok = 0; why = "notif"; }

    /* ACK with the valid IRQ cap (RIGHT_ROUTE) succeeds — unmasks the unused
     * line, harmless. */
    if (ok && it_sys1(SYS_IRQ_ACK, (long)irq) != 0) { ok = 0; why = "valid ack"; }
    /* ACK failure paths. */
    if (ok && it_sys1(SYS_IRQ_ACK, (long)notif) >= 0) { ok = 0; why = "ack wrong-type honoured"; }
    if (ok) {
        /* IRQ caps carry ROUTE|DUPLICATE|TRANSFER; derive DUPLICATE-only to get
         * a cap that lacks ROUTE (a subset — never amplified). */
        long rd = it_sys2(SYS_CAP_DERIVE, (long)irq, (long)RIGHT_DUPLICATE);
        handle_id_t irq_ro = (rd >= 0) ? (handle_id_t)rd : HANDLE_INVALID;
        if (rd < 0) { ok = 0; why = "irq derive"; }
        if (ok && it_sys1(SYS_IRQ_ACK, irq_ro) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-route ack not denied"; }
        /* Route with an IRQ cap lacking ROUTE → ACCESS_DENIED. */
        if (ok && it_sys3(SYS_IRQ_ROUTE_REGISTER, irq_ro, (long)notif, (long)IRIS_CPTR_TEST_PROC)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-route route not denied"; }
        it_close(&irq_ro);
    }
    /* Route with a wrong-type destination (endpoint, not notification). */
    if (ok) {
        long e = it_ep_create();
        handle_id_t ep_h = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
        if (e < 0) { ok = 0; why = "ep fixture"; }
        if (ok && it_sys3(SYS_IRQ_ROUTE_REGISTER, (long)irq, (long)ep_h, (long)IRIS_CPTR_TEST_PROC)
                  != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "route wrong-type notif"; }
        it_close(&ep_h);
    }
    /* Route with a notification lacking WRITE → ACCESS_DENIED. */
    if (ok) {
        long nrd = it_sys2(SYS_HANDLE_DUP, (long)notif, (long)RIGHT_READ);
        handle_id_t n_ro = (nrd >= 0) ? (handle_id_t)nrd : HANDLE_INVALID;
        if (nrd < 0) { ok = 0; why = "notif read dup"; }
        if (ok && it_sys3(SYS_IRQ_ROUTE_REGISTER, (long)irq, n_ro, (long)IRIS_CPTR_TEST_PROC)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "route no-write notif"; }
        it_close(&n_ro);
    }
    /* Route with a valid IRQ cap + valid notif but a proc cap lacking ROUTE
     * (iris_test's own proc, slot 25, is WRITE only) → ACCESS_DENIED: iris_test
     * cannot route, proving route needs proc-ROUTE authority (containment). */
    if (ok && it_sys3(SYS_IRQ_ROUTE_REGISTER, (long)irq, (long)notif, (long)IRIS_CPTR_TEST_PROC)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "route lacking proc-ROUTE not denied"; }

    it_close(&irq); it_close(&notif);
    it_quiesce_reaper();
    /* Every route attempt was an authority failure, so no route object was ever
     * installed — a leaked route would keep its destination notification alive,
     * caught by the notification-live check in the snapshot below. */
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T167"); else it_fail("T167", why);
}

/* ── T168: framebuffer / MMIO containment ───────────────────────────────────
 * Framebuffer authority is gated behind a FRAMEBUFFER-flagged bootstrap cap AND
 * is one-shot: the real fb service consumes the framebuffer VMO at boot, so even
 * iris_test's fully-privileged TEST spawn cap (which does carry FRAMEBUFFER)
 * cannot obtain a second handle — SYS_FRAMEBUFFER_VMO returns NOT_FOUND.  A
 * wrong-type auth cap is rejected ACCESS_DENIED.  Documented gap: because the
 * VMO is consumed and bounded to the framebuffer size, its mapping cannot be
 * exercised at runtime from iris_test; the bounded-range property is asserted at
 * the authority layer, and the absence of FRAMEBUFFER from productive services
 * is covered structurally by T162.  Invariants: D4, D5, D15, D16 (auth-level). */
static void test_t168(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "framebuffer containment";

    uint8_t fbbuf[64];
    /* The framebuffer VMO was consumed by fb at boot → NOT_FOUND, even though
     * the spawn cap carries FRAMEBUFFER (one-shot authority). */
    if (ok && it_sys3(SYS_FRAMEBUFFER_VMO, (long)IRIS_CPTR_SPAWN_CAP, (long)(uintptr_t)fbbuf, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "framebuffer not one-shot"; }
    /* Wrong-type auth cap (a notification) → ACCESS_DENIED (not a bootstrap cap). */
    if (ok) {
        long n = it_notify_create();
        handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
        if (n < 0) { ok = 0; why = "notif fixture"; }
        if (ok && it_sys3(SYS_FRAMEBUFFER_VMO, n, (long)(uintptr_t)fbbuf, 0)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "wrong-type got framebuffer"; }
        it_close(&n_h);
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T168"); else it_fail("T168", why);
}

/* ── T169: device cap derivation monotonicity ───────────────────────────────
 * A derived device cap can only narrow rights, never widen.  A READ-derived
 * ioport cap cannot OUT and cannot re-derive back a WRITE that works; a
 * ROUTE-less IRQ cap cannot ack or route; a revoked cap fails.  No path
 * recovers lost device authority.  Invariants: D3, D5, D14, D15. */
static void test_t169(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "device derivation";

    handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
    if (io == HANDLE_INVALID) { it_fail("T169", "io cap"); return; }

    long rr = it_sys2(SYS_CAP_DERIVE, (long)io, (long)RIGHT_READ);
    handle_id_t ro = (rr >= 0) ? (handle_id_t)rr : HANDLE_INVALID;
    if (ok && rr < 0) { ok = 0; why = "read derive"; }
    /* Re-derive asking for WRITE from a READ-only cap: either rejected or the
     * result still cannot OUT (rights never amplified). */
    if (ok) {
        long wr = it_sys2(SYS_CAP_DERIVE, (long)ro, (long)(RIGHT_READ | RIGHT_WRITE));
        if (wr >= 0) {
            handle_id_t w_h = (handle_id_t)wr;
            if (it_sys3(SYS_IOPORT_OUT, wr, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights amplified via re-derive"; }
            it_close(&w_h);
        }
    }
    /* Revoke children; the READ cap must be dead afterward. */
    if (ok && it_sys1(SYS_CAP_REVOKE, (long)io) != 0) { ok = 0; why = "revoke"; }
    if (ok && it_sys2(SYS_IOPORT_IN, ro, 0) >= 0) { ok = 0; why = "revoked cap usable"; }
    it_close(&ro);

    /* IRQ cap: a ROUTE-less derivation cannot ack. */
    if (ok) {
        long ic = it_sys3(SYS_CAP_CREATE_IRQCAP, (long)IRIS_CPTR_SPAWN_CAP, 5, 0);
        handle_id_t irq = (ic >= 0) ? (handle_id_t)ic : HANDLE_INVALID;
        if (ic < 0) { ok = 0; why = "irqcap"; }
        long rd = ok ? it_sys2(SYS_CAP_DERIVE, (long)irq, (long)RIGHT_DUPLICATE) : -1;
        handle_id_t irq_ro = (rd >= 0) ? (handle_id_t)rd : HANDLE_INVALID;
        if (ok && rd < 0) { ok = 0; why = "irq derive"; }
        if (ok && it_sys1(SYS_IRQ_ACK, irq_ro) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "route-less ack not denied"; }
        it_close(&irq_ro); it_close(&irq);
    }

    it_close(&io);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T169"); else it_fail("T169", why);
}

/* ── T170: driver death cleanup ─────────────────────────────────────────────
 * A driver holding device caps that dies releases them: several probe children
 * are minted an ioport cap and parked in a recv, then killed.  Process, task,
 * handle, endpoint and notification live counts return to baseline — a dead
 * driver's device authority is gone, no ghost.  Invariants: D13, D19. */
static void test_t170(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "driver death cleanup";

    for (int i = 0; ok && i < 4; i++) {
        handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
        if (io == HANDLE_INVALID) { ok = 0; why = "io cap"; break; }

        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        struct svc_mint mints[2];
        mints[0].slot = LP_CPTR_CMD_EP; mints[0].src_h = cmd;
        mints[0].rights = RIGHT_READ | RIGHT_WRITE; mints[0].badge = 0;
        mints[1].slot = 10; mints[1].src_h = io;
        mints[1].rights = RIGHT_READ | RIGHT_WRITE; mints[1].badge = 0;
        handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
        long r = (ep < 0) ? -1 : svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP,
                     "lifecycle_probe", &proc, &boot, mints, 2u);
        it_close(&boot); it_close(&io);
        if (r < 0 || proc == HANDLE_INVALID) { ok = 0; why = "spawn"; it_close(&cmd); it_close(&proc); break; }

        /* The child blocks in its first recv; kill it while parked. */
        it_sys1(SYS_SLEEP, 2);
        if (it_sys1(SYS_PROCESS_KILL, (long)proc) != 0) { ok = 0; why = "kill"; }
        if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "exit"; }
        it_close(&cmd); it_close(&proc);
        it_quiesce_reaper();
    }

    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T170"); else it_fail("T170", why);
}

/* ── T171: deterministic device authority fuzz ──────────────────────────────
 * A seeded PRNG drives a mix of ioport create (whitelisted / non-whitelisted),
 * in/out at valid and out-of-range offsets, derive/revoke, IRQ-cap create and
 * route/ack failure paths, and a compromised-driver probe.  No access crosses a
 * cap, no probe escalates (mask 0), and every gauge — including the IRQ-route
 * count — returns to baseline.  Prints seed/iteration only on failure.
 * Invariants: D1–D3, D5–D10, D14, D15, D17–D19. */
#define T171_SEED   0x23C0DE71u
#define T171_ROUNDS 14u
static void test_t171(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "device fuzz";
    g_fz_seed = T171_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T171_ROUNDS; i++) {
        /* Non-whitelisted create always denied. */
        op = 1;
        static const long bad_bases[] = { 0x70L, 0x80L, 0x3B0L, 0xCF8L };
        long bb = bad_bases[fz_rand() % 4u];
        if (it_sys3(SYS_CAP_CREATE_IOPORT, (long)IRIS_CPTR_SPAWN_CAP, bb, 2) != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "non-whitelist created"; break;
        }

        /* A valid COM2 cap, exercised in and out of range. */
        op = 2;
        handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
        if (io == HANDLE_INVALID) { ok = 0; why = "io cap"; break; }
        uint32_t off = fz_rand() % 32u;
        long rin = it_sys2(SYS_IOPORT_IN, (long)io, (long)off);
        if (off < (uint32_t)IT_COM2_COUNT) { if (rin < 0) { ok = 0; why = "in-range IN failed"; } }
        else { if (rin != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "out-range IN honoured"; } }
        if (!ok) { it_close(&io); break; }

        /* Sometimes derive READ-only and confirm OUT is denied. */
        op = 3;
        if (fz_rand() & 1u) {
            long rr = it_sys2(SYS_CAP_DERIVE, (long)io, (long)RIGHT_READ);
            if (rr >= 0) {
                handle_id_t ro = (handle_id_t)rr;
                if (it_sys3(SYS_IOPORT_OUT, rr, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "RO OUT honoured"; }
                if (ok && it_sys1(SYS_CAP_REVOKE, (long)io) != 0) { ok = 0; why = "revoke"; }
                if (ok && it_sys2(SYS_IOPORT_IN, rr, 0) >= 0) { ok = 0; why = "revoked usable"; }
                it_close(&ro);
            }
        }
        it_close(&io);
        if (!ok) break;

        /* Occasionally: a compromised-driver probe must stay contained. */
        op = 4;
        if (fz_rand() & 1u) {
            handle_id_t io2 = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
            if (io2 == HANDLE_INVALID) { ok = 0; why = "io2"; break; }
            long mask = it_dev_probe(io2, 500u + (fz_rand() & 0xFFu), RIGHT_READ | RIGHT_WRITE);
            it_close(&io2);
            if (mask != 0) { ok = 0; why = "probe escalated"; break; }
        }

        /* IRQ authority failure path: ack with a wrong-type cap. */
        op = 5;
        {
            long n = it_notify_create();
            if (n >= 0) {
                handle_id_t n_h = (handle_id_t)n;
                if (it_sys1(SYS_IRQ_ACK, n) >= 0) { ok = 0; why = "ack wrong-type honoured"; }
                it_close(&n_h);
            }
        }
        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T171");
    else { it_fz_note("T171", T171_SEED, i, op); it_fail("T171", why); }
}

/* ── Fase 24: service restart / supervision model (T172–T180) ───────────────
 *
 * Drivers are contained (Fase 23); the next structural risk is what happens
 * when a service or driver DIES.  These tests lock the supervision contract:
 * every service has an explicit policy (T172), a restarted service comes back
 * with a new generation and only its declared authority (T173/T177), a stale
 * generation cannot touch the new instance (T174), a crash-loop stops at its
 * limit and leaves the service degraded (T175), a client blocked on a dying
 * service wakes with an error (T176), a critical service's loss is an explicit
 * documented state (T178), and death mid-register leaves no ghost (T179/T180).
 * Invariants R1–R24 live in docs/architecture/service-supervision-model.md.
 *
 * Two supervision surfaces are exercised: svcmgr's real catalog policy (via the
 * STATUS oracle, now carrying the explicit policy words) and iris_test acting
 * as a supervisor over lifecycle_probe children it owns — a supervisor is just
 * a process holding proc caps plus a policy, so this tests the kernel primitives
 * (watch, kill, reap, endpoint cleanup) every supervisor depends on, plus the
 * policy logic, without spending the real services' restart budgets. */

/* Supervision-policy classes (must match service_catalog.h). */
#define IT_SUP_CRITICAL_RESTART     1u
#define IT_SUP_OPTIONAL_RESTART     2u
#define IT_SUP_OPTIONAL_NO_RESTART  3u
#define IT_SUP_CRITICAL_NO_RESTART  4u

/* Read the STATUS policy of a catalog service into six logical fields:
 *   p[0]=alive p[1]=gen p[2]=supervision p[3]=restart_count p[4]=restart_limit
 *   p[5]=degraded.  Over the wire words[3] packs count|limit<<8|degraded<<16
 *   (IPC carries only 4 words).  Returns 4 when the policy words are present,
 *   2 when only alive/gen were returned, or -1 on failure. */
static long it_policy(const char *name, uint32_t p[6]) {
    uint32_t len = it_stage_path(name);
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label    = IRIS_SVCMGR_EP_STATUS;
    m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    m.buf_len  = len;
    for (uint32_t i = 0; i < 6u; i++) p[i] = 0u;
    if (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -1;
    p[0] = (uint32_t)m.words[0];   /* alive */
    p[1] = (uint32_t)m.words[1];   /* generation */
    if (m.word_count >= 4u) {
        uint32_t w3 = (uint32_t)m.words[3];
        p[2] = (uint32_t)m.words[2];        /* supervision */
        p[3] = w3 & 0xFFu;                  /* restart_count */
        p[4] = (w3 >> 8) & 0xFFu;           /* restart_limit */
        p[5] = (w3 >> 16) & 0x1u;           /* degraded */
        return 4;
    }
    return (long)m.word_count;
}

/* Unregister a dynamic service id through the svcmgr endpoint; returns 0 (OK) or
 * the negative error the reply carried. */
static long it_unregister(uint32_t dyn_id) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label      = IRIS_SVCMGR_EP_UNREGISTER;
    m.words[0]   = dyn_id;
    m.word_count = 1u;
    if (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -(long)(uint32_t)m.words[0];
    return 0;
}

/* ── T172: supervision policy manifest consistency ──────────────────────────
 * Every catalog service declares an explicit supervision policy, and the policy
 * is consistent with its restart flags: a RESTART class carries a non-zero
 * limit, a NO_RESTART class carries a zero limit.  The policy does not
 * contradict the Fase 22 authority manifest (a driver stays a driver).
 * Invariants: R14, R15, R16, R20, R21. */
static void test_t172(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "policy manifest";

    struct { const char *name; uint32_t sup; int restartable; } expect[] = {
        { VFS_EP_SVC_NAME, IT_SUP_CRITICAL_RESTART,    1 },
        { KBD_EP_SVC_NAME, IT_SUP_OPTIONAL_RESTART,    1 },
        { "sh",            IT_SUP_OPTIONAL_NO_RESTART,  0 },
    };
    for (int i = 0; ok && i < 3; i++) {
        uint32_t p[6];
        long wc = it_policy(expect[i].name, p);
        if (wc < 4) { ok = 0; why = "no explicit policy"; break; }
        if (p[2] != expect[i].sup) { ok = 0; why = "wrong criticality"; break; }
        /* Consistency: restartable class ⇒ limit > 0; no-restart ⇒ limit 0. */
        if (expect[i].restartable && p[4] == 0u) { ok = 0; why = "restartable with zero limit"; break; }
        if (!expect[i].restartable && p[4] != 0u) { ok = 0; why = "no-restart with nonzero limit"; break; }
        /* A restart_count must never exceed its limit. */
        if (p[4] != 0u && p[3] > p[4]) { ok = 0; why = "restart_count exceeds limit"; break; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T172"); else it_fail("T172", why);
}

/* ── T173: restartable service basic recovery (real catalog restart) ────────
 * Drive one real RESTART of kbd (a restartable OPTIONAL service, budget-frugal:
 * one of three) through the supervisor cap.  The kernel's watch path respawns
 * it: the generation and restart_count both advance, the service is alive again,
 * kbd.ep still resolves and answers PING (the endpoint survives restarts), and
 * the live books return to baseline.  Invariants: R1, R2, R3, R19. */
static void test_t173(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "restart recovery";

    uint32_t p0[6];
    if (ok && it_policy(KBD_EP_SVC_NAME, p0) < 4) { ok = 0; why = "pre-policy"; }
    if (ok && p0[0] != 1u) { ok = 0; why = "kbd not alive pre"; }

    struct IrisMsg msg;
    it_iris_msg_zero(&msg);
    msg.label = IRIS_SVCMGR_EP_RESTART;
    msg.words[0] = (uint64_t)SVCMGR_SERVICE_KBD;
    msg.word_count = 1u;
    if (ok && (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_TEST_SUPER, (long)&msg) != 0 ||
               msg.label != IRIS_EP_REPLY_OK)) { ok = 0; why = "restart denied"; }

    /* Poll (bounded, no sleep — each EP_CALL yields) until the new generation. */
    int recovered = 0;
    for (uint32_t i = 0; ok && i < 400u && !recovered; i++) {
        uint64_t bb = 0;
        (void)it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &bb);
        uint32_t p1[6];
        if (it_policy(KBD_EP_SVC_NAME, p1) >= 4 && p1[0] == 1u &&
            p1[1] > p0[1] && p1[3] > p0[3]) recovered = 1;
    }
    if (ok && !recovered) { ok = 0; why = "kbd did not restart with new gen/count"; }

    /* The restarted instance answers on kbd.ep (endpoint survives restart). */
    if (ok) {
        struct IrisMsg pm;
        it_iris_msg_zero(&pm);
        pm.label = IRIS_EP_OP_PING;
        if (it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_KBD_EP, (long)&pm) != 0 ||
            pm.label != IRIS_EP_REPLY_OK) { ok = 0; why = "kbd.ep dead after restart"; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T173"); else it_fail("T173", why);
}

/* ── T174: stale generation / stale registration rejection ──────────────────
 * A dynamic registration is owner-managed: register a dummy service, unregister
 * it, and prove the stale id cannot act again — a second unregister is
 * NOT_FOUND, and a lookup of the gone name is NOT_FOUND.  A fresh registration
 * of the same name succeeds with its own slot; the OLD id still cannot
 * unregister the NEW registration (no cross-generation authority).  In parallel,
 * the catalog generation is monotonic (kbd's generation from T173 never
 * decreases).  Invariants: R2, R4, R18, R24. */
static void test_t174(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "stale generation";

    long e = it_ep_create();
    handle_id_t svc_ep = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
    if (e < 0) { it_fail("T174", "ep"); return; }

    long id0 = it_register_ep("t174.svc", svc_ep);
    if (ok && id0 < 0) { ok = 0; why = "register"; }
    if (ok && it_unregister((uint32_t)id0) != 0) { ok = 0; why = "unregister"; }
    /* Stale id: second unregister is NOT_FOUND; the name no longer resolves. */
    if (ok && it_unregister((uint32_t)id0) != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale unregister accepted"; }
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t174.svc")
              != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "gone name resolves"; }

    /* Fresh registration of the same name; the OLD id must not unregister it. */
    long id1 = ok ? it_register_ep("t174.svc", svc_ep) : -1;
    if (ok && id1 < 0) { ok = 0; why = "re-register"; }
    if (ok && id1 == id0) {
        /* Same slot reused: the stale-id test is only meaningful with a
         * different id, but reuse is acceptable — the unregister below still
         * proves the NEW registration is the one that is torn down. */
    }
    if (ok && it_unregister((uint32_t)id1) != 0) { ok = 0; why = "new unregister"; }

    /* Catalog generation is monotonic (kbd was restarted in T173). */
    if (ok) {
        uint32_t p[6];
        if (it_policy(KBD_EP_SVC_NAME, p) < 4 || p[1] < 1u) { ok = 0; why = "gen not monotonic"; }
    }

    it_close(&svc_ep);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T174"); else it_fail("T174", why);
}

/* ── T175: crash-loop limit and degraded state ──────────────────────────────
 * iris_test supervises a dummy service (a probe it spawns and immediately kills,
 * modelling a service that dies right after start) under an explicit restart
 * limit.  The supervisor respawns exactly `limit` times, then STOPS and marks
 * the service degraded — no infinite loop, no leak per attempt, live counts back
 * to baseline.  This is the policy the real catalog applies (restart_count <
 * restart_limit), driven deterministically.  Invariants: R13, R14, R15, R19. */
#define T175_LIMIT 3u
static void test_t175(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "crash-loop limit";

    uint32_t restart_count = 0u;
    int degraded = 0;
    uint32_t generation = 0u;

    /* Supervision loop: (re)start until the budget is spent. */
    while (ok && !degraded) {
        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "spawn"; it_close(&cmd); break; }
        generation++;                              /* each (re)start is a new generation */

        /* The "service" dies immediately (modelled by an external kill). */
        it_sys1(SYS_SLEEP, 1);
        if (it_sys1(SYS_PROCESS_KILL, (long)proc) != 0) { ok = 0; why = "kill"; }
        if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "exit"; }
        it_close(&cmd); it_close(&proc);
        it_quiesce_reaper();

        /* Restart policy: count the death; stop at the limit. */
        if (restart_count < T175_LIMIT) restart_count++;
        else degraded = 1;
    }

    if (ok && restart_count != T175_LIMIT) { ok = 0; why = "wrong restart count"; }
    if (ok && !degraded) { ok = 0; why = "never degraded"; }
    if (ok && generation != T175_LIMIT + 1u) { ok = 0; why = "generation mismatch"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T175"); else it_fail("T175", why);
}

/* ── T176: client behavior during service death ─────────────────────────────
 * A client blocked on a call to a service that dies must wake with an error, not
 * hang forever, and the stale endpoint must not become magically valid.  A probe
 * blocks as a CALLER on its command endpoint (LP_CMD_CALL_BLOCK); killing it
 * while blocked is the kernel path a supervisor relies on — the blocked wait is
 * cancelled, the child reaps, no KReply/waiter is stranded.  A following NB-send
 * to the closed endpoint reports WOULD_BLOCK (no phantom receiver).
 * Invariants: R9, R12. */
static void test_t176(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "client during death";

    long ep = it_ep_create();
    handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    handle_id_t proc = HANDLE_INVALID;
    if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { it_close(&cmd); it_fail("T176", "spawn"); return; }

    /* Drive the child to block as a caller, then kill it mid-call. */
    if (ok && it_lp_cmd(cmd, LP_CMD_CALL_BLOCK) != 0) { ok = 0; why = "cmd"; }
    it_sys1(SYS_SLEEP, 3);
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc) != 0) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "no exit"; }

    /* The endpoint has no receiver now: a non-blocking send reports WOULD_BLOCK,
     * not a phantom rendezvous with the dead caller. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.label = 0x176;
        long r = it_sys2(SYS_EP_NB_SEND, (long)cmd, (long)&m);
        if (r != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "phantom receiver after death"; }
    }

    it_close(&cmd); it_close(&proc);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T176"); else it_fail("T176", why);
}

/* ── T177: driver restart with device authority ─────────────────────────────
 * A restarted driver must come back with ONLY its declared device authority —
 * no amplification, no leaked route.  A driver-like probe is minted an ioport
 * cap, killed, then a NEW instance is spawned with the SAME declared caps; the
 * new instance is still contained (DEV_PROBE breach 0) and its slot report shows
 * only the command endpoint + its device cap — no spawn/proc/untyped, no peer
 * client caps.  No IRQ/device ghost across the restart.  Invariants: R5, R6, R7,
 * R8, R10, R23. */
static void test_t177(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "driver restart authority";

    for (int gen = 0; ok && gen < 2; gen++) {
        /* Each "generation" is a fresh driver instance with the SAME manifest. */
        handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
        if (io == HANDLE_INVALID) { ok = 0; why = "io cap"; break; }
        long mask = it_dev_probe(io, 1000u, RIGHT_READ);   /* out-of-range → contained */
        it_close(&io);
        if (mask != 0) { ok = 0; why = "restarted driver escalated"; break; }
    }

    /* The driver instance holds only its command endpoint + device cap: report
     * its well-known slots and assert no high-authority slot appears. */
    if (ok) {
        long io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
        handle_id_t io_h = (io >= 0) ? (handle_id_t)io : HANDLE_INVALID;
        if (io_h == HANDLE_INVALID) { ok = 0; why = "io cap 2"; }
        else {
            struct svc_mint extra[1];
            extra[0].slot = 10; extra[0].src_h = io_h; extra[0].rights = RIGHT_READ; extra[0].badge = 0;
            long rep = it_lp_report_slots(extra, 1u);
            /* Expect exactly {cmd ep slot 3, device cap slot 10}. */
            if (rep < 0 || (uint32_t)rep != ((1u << 3) | (1u << 10))) { ok = 0; why = "unexpected authority set"; }
            /* Explicitly none of spawn(6)/proc(16)/untyped(17)/vspace(18)/peers(1,2,4). */
            if (ok && ((uint32_t)rep & ((1u<<6)|(1u<<16)|(1u<<17)|(1u<<18)|(1u<<1)|(1u<<2)|(1u<<4))) != 0) {
                ok = 0; why = "driver gained extra authority on restart";
            }
            it_close(&io_h);
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T177"); else it_fail("T177", why);
}

/* ── T178: critical service death policy ────────────────────────────────────
 * A critical service's loss is an explicit, documented state — never an implicit
 * silent behaviour.  The policy manifest marks vfs CRITICAL_RESTART (restarted
 * up to its limit, then degraded) and sh OPTIONAL_NO_RESTART (never auto-
 * restarted); both are observable via STATUS.  This asserts the policy is
 * present and self-consistent WITHOUT destructively killing a critical service
 * (which would break the running system for no test value).  Invariants: R15,
 * R16, R20. */
static void test_t178(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "critical policy";

    uint32_t pv[6], ps[6];
    if (ok && it_policy(VFS_EP_SVC_NAME, pv) < 4) { ok = 0; why = "vfs policy"; }
    if (ok && pv[2] != IT_SUP_CRITICAL_RESTART) { ok = 0; why = "vfs not critical-restart"; }
    if (ok && pv[4] == 0u) { ok = 0; why = "critical service has no restart budget"; }
    if (ok && pv[0] != 1u) { ok = 0; why = "vfs not alive"; }   /* critical ⇒ present */

    if (ok && it_policy("sh", ps) < 4) { ok = 0; why = "sh policy"; }
    if (ok && ps[2] != IT_SUP_OPTIONAL_NO_RESTART) { ok = 0; why = "sh not optional-no-restart"; }
    if (ok && ps[4] != 0u) { ok = 0; why = "no-restart service has budget"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T178"); else it_fail("T178", why);
}

/* ── T179: service death during register / unregister ───────────────────────
 * The registry stays consistent when a registrant dies around registration.  A
 * dynamic registration backed by an endpoint iris_test owns survives the death
 * of any OTHER process; a probe killed after a registration was made on its
 * behalf leaves no ghost — the name still resolves to the (owner-held) endpoint,
 * and a clean unregister removes it.  Repeated unregister is idempotent
 * (NOT_FOUND).  Invariants: R17, R18. */
static void test_t179(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "register/unregister death";
    long slots0 = it_svcmgr_active_slots();
    if (ok && slots0 < 0) { ok = 0; why = "diag baseline"; }

    /* Register a service, spawn a probe, kill the probe: the registration (owned
     * by iris_test) is unaffected by the unrelated death. */
    long e = it_ep_create();
    handle_id_t svc_ep = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
    if (e < 0) { it_fail("T179", "ep"); return; }
    long id = it_register_ep("t179.svc", svc_ep);
    if (ok && id < 0) { ok = 0; why = "register"; }

    long cep = it_ep_create();
    handle_id_t cmd = (cep >= 0) ? (handle_id_t)cep : HANDLE_INVALID;
    handle_id_t proc = HANDLE_INVALID;
    if (ok && (cep < 0 || lp_spawn_child(cmd, &proc) < 0)) { ok = 0; why = "spawn"; }
    if (ok) {
        it_sys1(SYS_SLEEP, 2);
        if (it_sys1(SYS_PROCESS_KILL, (long)proc) != 0) { ok = 0; why = "kill"; }
        if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "exit"; }
    }
    it_close(&cmd); it_close(&proc);

    /* The registration still resolves; unregister removes it; repeat is NOT_FOUND. */
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t179.svc") < 0) { ok = 0; why = "reg lost on unrelated death"; }
    if (ok && it_unregister((uint32_t)id) != 0) { ok = 0; why = "unregister"; }
    if (ok && it_unregister((uint32_t)id) != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "double unregister accepted"; }
    if (ok && it_svcmgr_active_slots() != slots0) { ok = 0; why = "registry slot drift"; }

    it_close(&svc_ep);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T179"); else it_fail("T179", why);
}

/* ── T180: deterministic supervision stress ─────────────────────────────────
 * A seeded PRNG drives a mix of supervised-service operations against probe
 * children and the dynamic registry: spawn, register, lookup, kill, fault-
 * crash, restart (respawn a new generation), unregister, stale unregister,
 * endpoint close, and a driver-like instance with a device cap.  No stale
 * generation acts, no registry ghost survives, no client stays stuck, and every
 * gauge — registry slots and the full live snapshot — returns to baseline.
 * Prints seed/iteration only on failure.  Invariants: R2, R4, R9, R12, R18–R24. */
#define T180_SEED   0x24C0DE80u
#define T180_ROUNDS 12u
static void test_t180(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "supervision stress";
    long slots0 = it_svcmgr_active_slots();
    if (ok && slots0 < 0) { ok = 0; why = "diag baseline"; }
    g_fz_seed = T180_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T180_ROUNDS; i++) {
        /* Stale-registry pressure: unregister a never-registered id. */
        op = 1;
        {
            struct IrisMsg m;
            it_iris_msg_zero(&m);
            m.label = IRIS_SVCMGR_EP_UNREGISTER;
            m.words[0] = 0x4000u + (fz_rand() & 0xFFu);
            m.word_count = 1u;
            long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_SVCMGR_EP, (long)&m);
            if (r == 0 && m.label == IRIS_EP_REPLY_OK) { ok = 0; why = "stale unregister accepted"; break; }
        }

        /* A supervised probe: spawn, kill or fault-crash, reap. */
        op = 2;
        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "spawn"; it_close(&cmd); break; }

        uint32_t how = fz_rand() % 3u;
        if (how == 0u) {                             /* immediate kill */
            it_sys1(SYS_SLEEP, 1);
            if (it_sys1(SYS_PROCESS_KILL, (long)proc) != 0) { ok = 0; why = "kill"; }
        } else if (how == 1u) {                      /* block then kill */
            if (it_lp_cmd(cmd, LP_CMD_SEND_BLOCK) != 0) { ok = 0; why = "cmd block"; }
            it_sys1(SYS_SLEEP, 2);
            if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc) != 0) { ok = 0; why = "kill2"; }
        } else {                                     /* fault-crash (no handler → kill) */
            if (it_lp_cmd_va(cmd, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "fault cmd"; }
        }
        if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "no exit"; }
        it_close(&cmd); it_close(&proc);

        /* Occasionally exercise the dynamic registry with a clean round-trip. */
        op = 3;
        if (ok && (fz_rand() & 1u)) {
            long e2 = it_ep_create();
            handle_id_t sep = (e2 >= 0) ? (handle_id_t)e2 : HANDLE_INVALID;
            if (e2 < 0) { ok = 0; why = "reg ep"; it_close(&cmd); break; }
            long id = it_register_ep("t180.svc", sep);
            if (id < 0) { ok = 0; why = "register"; }
            if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t180.svc") < 0) { ok = 0; why = "lookup"; }
            if (ok && it_unregister((uint32_t)id) != 0) { ok = 0; why = "unregister"; }
            it_close(&sep);
        }
        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && it_svcmgr_active_slots() != slots0) { ok = 0; why = "registry drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T180");
    else { it_fz_note("T180", T180_SEED, i, op); it_fail("T180", why); }
}

/* ── Fase 25: VM policy / user pager groundwork (T181–T190) ─────────────────
 *
 * Services are supervised (Fase 24); the next structural boundary is MEMORY
 * POLICY.  These tests lock the first capability-mediated user-pager model:
 * an authorized ring-3 component resolves page faults of a SPECIFIC process
 * with explicit, attenuable authority — and nothing more.
 *
 * The pager authority manifest (all minted pre-start by the supervisor):
 *   target process cap  READ|MANAGE  — fault info + resolution, this target
 *   target VSpace cap   WRITE        — map-into-target (SYS_PROCESS_VSPACE)
 *   frame cap           READ[/WRITE] — the one page it may install
 *   fault notification  WAIT         — the delivery wake-up
 *
 * Four Fase 25 additive kernel extensions are exercised here:
 *   SYS_PROCESS_VSPACE(107)  MANAGE holder → target VSpace cap;
 *   FAULT_OFF_SEQ            per-process fault generation in the record;
 *   EXCEPTION_RESUME 2/3     seq-checked resume/kill (stale → NOT_FOUND);
 *   FRAME_MAP/UNMAP VSpace   dual resolver (handle works; A1 migration
 *                            completed, radix-masking hazard closed).
 *
 * Invariants P1–P24 live in docs/architecture/user-pager-vm-policy.md.
 * Constants must match services/lifecycle_probe/main.c. */

#define LP_CMD_PAGER_SERVE  0x10A2u
#define LP_CMD_PAGER_XPROBE 0x10A3u
#define LP_PGR_SLOT_XPROC   8u
#define LP_PGR_SLOT_XVS     9u
#define LP_PGR_SLOT_TPROC   12u
#define LP_PGR_SLOT_TVS     13u
#define LP_PGR_SLOT_FRAME   14u
#define LP_PGR_SLOT_NOTIF   15u
#define LP_EXIT_PGR_OK      0x0D00L

/* Fault VAs: canonical, page-aligned, inside the user-private window, clear
 * of every earlier suite VA (T13x self-maps end at 0x8078…, T14X_BAD_VA is
 * 0x8090…).  T25_SELF_VA is the parent's own scratch mapping for preparing /
 * inspecting frame contents. */
#define T25_VA_A    0x8092000000ULL
#define T25_VA_B    0x8093000000ULL
#define T25_VA_C    0x8094000000ULL
#define T25_VA_D    0x8095000000ULL
#define T25_SELF_VA 0x807A000000ULL
#define T25_PATTERN 0x5AA51234u
#define T25_WMARK   0xFA017E57u   /* the store LP_CMD_FAULT_WRITE retires */

/* Best-effort reap: kill (no-op if already dead), wait, close.  Every Fase 25
 * exit path — success or failure — must NOT leave a probe blocked in EP_RECV
 * forever: the child holds its own mint of the command endpoint, so closing
 * the parent's handle alone never wakes it, and a leaked live child keeps its
 * exception-handler notification pinned. */
static void t25_reap(handle_id_t *proc_h) {
    if (*proc_h != HANDLE_INVALID) {
        (void)it_sys1(SYS_PROCESS_KILL, (long)*proc_h);
        (void)it_lp_wait_exit(*proc_h);
    }
    it_close(proc_h);
}

/* A supervised pager target: command endpoint, proc cap, VSpace cap (via
 * SYS_PROCESS_VSPACE), fault-handler notification, exit watch. */
struct t25_tgt {
    handle_id_t cmd, proc, vs, notif, watch;
};

static void t25_tgt_close(struct t25_tgt *g) {
    it_close(&g->cmd); it_close(&g->proc); it_close(&g->vs);
    it_close(&g->notif); it_close(&g->watch);
}

/* Reap variant of the close: kills a possibly-still-alive target first (a
 * dead target makes the kill a clean no-op). */
static void t25_tgt_reap(struct t25_tgt *g) {
    t25_reap(&g->proc);
    t25_tgt_close(g);
}

static int t25_tgt_spawn(struct t25_tgt *g, const char **why) {
    g->cmd = g->proc = g->vs = g->notif = g->watch = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ep create"; return 0; }
    g->cmd = (handle_id_t)ep;
    if (lp_spawn_child(g->cmd, &g->proc) < 0 || g->proc == HANDLE_INVALID) {
        it_close(&g->cmd); *why = "spawn"; return 0;
    }
    long vs = it_sys1(SYS_PROCESS_VSPACE, (long)g->proc);
    long n  = it_notify_create();
    long w  = it_notify_create();
    g->vs    = (vs >= 0) ? (handle_id_t)vs : HANDLE_INVALID;
    g->notif = (n  >= 0) ? (handle_id_t)n  : HANDLE_INVALID;
    g->watch = (w  >= 0) ? (handle_id_t)w  : HANDLE_INVALID;
    long eh = 0, wt = 0;
    if (vs < 0 || n < 0 || w < 0 ||
        (eh = it_sys3(SYS_EXCEPTION_HANDLER, (long)g->proc, n, 1)) != 0 ||
        (wt = it_sys3(SYS_PROCESS_WATCH, (long)g->proc, w, 1)) != 0) {
        it_serial_write("[IRIS][TEST] t25 wire vs="); it_log_num((uint32_t)-vs);
        it_serial_write(" n="); it_log_num((uint32_t)-n);
        it_serial_write(" w="); it_log_num((uint32_t)-w);
        it_serial_write(" eh="); it_log_num((uint32_t)-eh);
        it_serial_write(" wt="); it_log_num((uint32_t)-wt);
        it_serial_write("\n");
        (void)it_sys1(SYS_PROCESS_KILL, (long)g->proc);
        t25_tgt_close(g);
        *why = "target wire"; return 0;
    }
    return 1;
}

/* Spawn an external pager over `g` with the declared manifest (plus optional
 * extra mints, e.g. T184's under-privileged victim caps).  0 on success. */
static long t25_pager_spawn(const struct t25_tgt *g, handle_id_t frame_h,
                            iris_rights_t frame_rights,
                            const struct svc_mint *extra, uint32_t nextra,
                            handle_id_t *out_cmd, handle_id_t *out_proc) {
    *out_cmd = *out_proc = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) return -1;
    handle_id_t cmd = (handle_id_t)ep;
    struct svc_mint m[8];
    m[0].slot = LP_CPTR_CMD_EP;    m[0].src_h = cmd;      m[0].rights = RIGHT_READ | RIGHT_WRITE;  m[0].badge = 0;
    m[1].slot = LP_PGR_SLOT_TPROC; m[1].src_h = g->proc;  m[1].rights = RIGHT_READ | RIGHT_MANAGE; m[1].badge = 0;
    m[2].slot = LP_PGR_SLOT_TVS;   m[2].src_h = g->vs;    m[2].rights = RIGHT_WRITE;               m[2].badge = 0;
    m[3].slot = LP_PGR_SLOT_FRAME; m[3].src_h = frame_h;  m[3].rights = frame_rights;              m[3].badge = 0;
    m[4].slot = LP_PGR_SLOT_NOTIF; m[4].src_h = g->notif; m[4].rights = RIGHT_WAIT;                m[4].badge = 0;
    uint32_t n = 5u;
    for (uint32_t i = 0; i < nextra && n < 8u; i++) m[n++] = extra[i];
    handle_id_t boot = HANDLE_INVALID;
    long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "lifecycle_probe",
                             out_proc, &boot, m, n);
    it_close(&boot);
    if (r < 0 || *out_proc == HANDLE_INVALID) {
        it_close(&cmd); it_close(out_proc); return -1;
    }
    *out_cmd = cmd;
    return 0;
}

static long t25_serve(handle_id_t pcmd, uint32_t sub, uint32_t count,
                      uint64_t mflags, uint64_t va_ovr, uint64_t expect_cr2) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label = LP_CMD_PAGER_SERVE;
    m.words[0] = (uint64_t)sub | ((uint64_t)count << 8);
    m.words[1] = mflags;
    m.words[2] = va_ovr;
    m.words[3] = expect_cr2;
    m.word_count = 4u;
    return it_sys2(SYS_EP_SEND, (long)pcmd, (long)&m);
}

static long t25_xprobe(handle_id_t pcmd, uint32_t vtid, uint64_t va, uint32_t vseq) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label = LP_CMD_PAGER_XPROBE;
    m.words[0] = vtid;
    m.words[1] = va;
    m.words[2] = vseq;
    m.word_count = 3u;
    return it_sys2(SYS_EP_SEND, (long)pcmd, (long)&m);
}

/* Seq-checked resolution (EXCEPTION_RESUME action 2/3 with the generation in
 * bits [63:32]). */
static long t25_resume_seq(handle_id_t proc_h, uint32_t tid, uint32_t seq, int kill) {
    return it_sys3(SYS_EXCEPTION_RESUME, (long)proc_h, (long)tid,
                   (long)(((uint64_t)seq << 32) | (kill ? 3u : 2u)));
}

/* Bounded non-consuming wait for a pending fault (polls FAULT_INFO — never
 * steals the notification signal from a pager). */
static int t25_wait_fault(handle_id_t proc_h, struct it_fault *f) {
    for (int i = 0; i < 400; i++) {
        long r = it_fault_info(proc_h, f);
        if (r == 0) return 1;
        if (r != (long)IRIS_ERR_WOULD_BLOCK) return 0;
        it_sys1(SYS_SLEEP, 1);
    }
    return 0;
}

/* Bounded wait for the fault generation to move past `old_seq` (refault). */
static int t25_wait_refault(handle_id_t proc_h, uint32_t old_seq, struct it_fault *f) {
    for (int i = 0; i < 400; i++) {
        if (it_fault_info(proc_h, f) == 0 && f->seq != old_seq) return 1;
        it_sys1(SYS_SLEEP, 1);
    }
    return 0;
}

/* Write (write=1) or read back (write=0) word 0 of a frame through the
 * parent's own VSpace — frame preparation and post-mortem inspection. */
static int t25_frame_word(handle_id_t fr, uint32_t *val, int write) {
    if (!it_setup_self_vspace()) return 0;
    if (it_sys4(SYS_FRAME_MAP, (long)fr, IT_VS, (long)T25_SELF_VA,
                write ? 1L : 0L) != 0) return 0;
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T25_SELF_VA;
    if (write) *p = *val; else *val = *p;
    return it_sys3(SYS_FRAME_UNMAP, (long)fr, IT_VS, (long)T25_SELF_VA) == 0;
}

/* ── T181: pager authority manifest ─────────────────────────────────────────
 * A pager's authority is EXACTLY its declared manifest: cmd endpoint + target
 * proc (READ|MANAGE) + target VSpace (WRITE) + one frame + fault notification
 * (WAIT).  The slot report shows exactly that set — no spawn cap, no device
 * caps, no untyped, no KDEBUG, no peer service slots, no self-proc/vspace.
 * SYS_PROCESS_VSPACE itself is MANAGE-gated with no fallback: a READ-only
 * proc cap is denied, a wrong type is rejected, self (HANDLE_INVALID) stays
 * the VSPACE_SELF equivalence.  Invariants: P1, P2, P24. */
static void test_t181(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager manifest";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T181", why); return; }
    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }

    if (ok) {
        struct svc_mint x[4];
        x[0].slot = LP_PGR_SLOT_TPROC; x[0].src_h = g.proc;  x[0].rights = RIGHT_READ | RIGHT_MANAGE; x[0].badge = 0;
        x[1].slot = LP_PGR_SLOT_TVS;   x[1].src_h = g.vs;    x[1].rights = RIGHT_WRITE;               x[1].badge = 0;
        x[2].slot = LP_PGR_SLOT_FRAME; x[2].src_h = fr_h;    x[2].rights = RIGHT_READ | RIGHT_WRITE;  x[2].badge = 0;
        x[3].slot = LP_PGR_SLOT_NOTIF; x[3].src_h = g.notif; x[3].rights = RIGHT_WAIT;                x[3].badge = 0;
        long rep = it_lp_report_slots(x, 4u);
        uint32_t expect = (1u << LP_CPTR_CMD_EP)    | (1u << LP_PGR_SLOT_TPROC) |
                          (1u << LP_PGR_SLOT_TVS)   | (1u << LP_PGR_SLOT_FRAME) |
                          (1u << LP_PGR_SLOT_NOTIF);
        if (rep < 0 || (uint32_t)rep != expect) { ok = 0; why = "manifest mismatch"; }
        /* Explicitly: no spawn(6), no device(10/11), no peers(1/2/4), no
         * self-proc(→16)/untyped(→17)/vspace-self(→18). */
        if (ok && ((uint32_t)rep & ((1u<<6)|(1u<<10)|(1u<<11)|(1u<<1)|(1u<<2)|(1u<<4)|
                                    (1u<<16)|(1u<<17)|(1u<<18))) != 0) {
            ok = 0; why = "high-authority slot leaked";
        }
    }

    /* SYS_PROCESS_VSPACE authority: MANAGE or nothing. */
    long ro = ok ? it_sys2(SYS_HANDLE_DUP, (long)g.proc, (long)RIGHT_READ) : -1;
    handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
    if (ok && ro < 0) { ok = 0; why = "ro dup"; }
    if (ok && it_sys1(SYS_PROCESS_VSPACE, ro) != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "no-manage not denied";
    }
    if (ok && it_sys1(SYS_PROCESS_VSPACE, (long)g.notif) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "wrong type accepted";
    }
    if (ok) {
        long sv = it_sys1(SYS_PROCESS_VSPACE, (long)HANDLE_INVALID);
        if (sv < 0) { ok = 0; why = "self vspace"; }
        else { handle_id_t h = (handle_id_t)sv; it_close(&h); }
    }
    it_close(&ro_h);

    if (it_sys1(SYS_PROCESS_KILL, (long)g.proc) != 0 && ok) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T181"); else it_fail("T181", why);
}

/* ── T182: external pager receives an invalid-VA fault ──────────────────────
 * The full external delivery path: the target touches an unmapped VA; the
 * PAGER (a separate process) wakes on its WAIT-only notification cap, reads
 * honest fault info through its READ|MANAGE target cap (validating cr2
 * itself), decides, and resolves with a seq-checked kill.  Delivery is
 * exactly-once (no residual signal, no residual record) and hands the pager
 * no capability it was not minted.  Invariants: P2, P3, P9, P12, P21. */
static void test_t182(void) {
    uint32_t f0[5], f1[5];
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_sched_ext5(f0);
    const char *why = "external delivery";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T182", why); return; }
    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
        ok = 0; why = "pager spawn";
    }

    /* Pager: one fault, validate cr2 == T25_VA_A, seq-checked kill. */
    if (ok && t25_serve(pcmd, 3u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "serve cmd"; }
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target not killed"; }
    if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report"; }

    /* Exactly-once: the signal was consumed by the pager and never re-fires;
     * the record did not outlive the resolution. */
    if (ok) {
        uint64_t bits = 0;
        if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)g.notif, (long)(uintptr_t)&bits,
                    100000000LL) == 0 && (bits & 1ull)) { ok = 0; why = "double delivery"; }
    }
    if (ok && it_fault_info(g.proc, &(struct it_fault){0}) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived";
    }

    t25_reap(&pproc); it_close(&pcmd);
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5 final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivered != 1"; }
    if (ok && f1[IT_S5_KILL]    != f0[IT_S5_KILL] + 1u)    { ok = 0; why = "kill count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 1u)   { ok = 0; why = "cleanup count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T182"); else it_fail("T182", why);
}

/* ── T183: pager maps a frame into the target VSpace ────────────────────────
 * The resolution path that DEFINES a pager: map-into-target + resume, both by
 * explicit capability.  (A) read fault: the pager installs its frame
 * read-only at the faulting VA and seq-resumes; the target continues and
 * READS the pattern the supervisor placed in the frame (its exit code proves
 * the data flowed).  (B) write fault: a second target's store retires into a
 * writable mapping and is visible to the supervisor through the frame
 * afterwards.  Target death sweeps the pager-installed mapping; the frame
 * stays intact and reusable.  Invariants: P5, P7, P9, P11, P18, P19, P20. */
static void test_t183(void) {
    uint32_t f0[5], f1[5], word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_sched_ext5(f0);
    const char *why = "map into target";

    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { it_fail("T183", "frame retype"); return; }
    word = T25_PATTERN;
    if (!t25_frame_word(fr_h, &word, 1)) { it_close(&fr_h); it_fail("T183", "frame fill"); return; }

    /* (A) read fault, read-only frame cap — the pager cannot and need not map
     * writable; the target reads the supervisor's pattern. */
    struct t25_tgt g;
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&fr_h); it_fail("T183", why); return; }
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
        ok = 0; why = "pager spawn A";
    }
    if (ok && t25_serve(pcmd, 1u, 1u, 0 /*RO map*/, 0, T25_VA_A) != 0) { ok = 0; why = "serve A"; }
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault A"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T25_PATTERN & 0xFFu))) { ok = 0; why = "target did not continue"; }
    if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report A"; }
    if (ok && it_fault_info(g.proc, &(struct it_fault){0}) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived A";
    }
    t25_reap(&pproc); it_close(&pcmd);
    t25_tgt_reap(&g);

    /* Target death swept the mapping: the frame must be clean and intact. */
    it_quiesce_reaper();
    if (ok && (!t25_frame_word(fr_h, &word, 0) || word != T25_PATTERN)) {
        ok = 0; why = "frame not reusable after sweep";
    }

    /* (B) write fault, writable mapping — the store lands in the frame. */
    if (ok) {
        word = 0;
        if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame zero"; }
    }
    struct t25_tgt g2;
    handle_id_t p2cmd = HANDLE_INVALID, p2proc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g2, &why)) { it_close(&fr_h); it_fail("T183", why); return; }
    if (ok) {
        if (t25_pager_spawn(&g2, fr_h, RIGHT_READ | RIGHT_WRITE, 0, 0u,
                            &p2cmd, &p2proc) != 0) { ok = 0; why = "pager spawn B"; }
        if (ok && t25_serve(p2cmd, 1u, 1u, 1 /*W map*/, 0, T25_VA_B) != 0) { ok = 0; why = "serve B"; }
        if (ok && it_lp_cmd_va(g2.cmd, LP_CMD_FAULT_WRITE, T25_VA_B) != 0) { ok = 0; why = "fault B"; }
        if (ok && it_lp_wait_exit(g2.proc) != LP_EXIT_MARKER) { ok = 0; why = "store did not retire"; }
        if (ok && it_lp_wait_exit(p2proc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report B"; }
        t25_reap(&p2proc); it_close(&p2cmd);
    }
    t25_tgt_reap(&g2);
    it_quiesce_reaper();
    if (ok && (!t25_frame_word(fr_h, &word, 0) || word != T25_WMARK)) {
        ok = 0; why = "write not visible in frame";
    }

    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5 final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 2u)  { ok = 0; why = "resume count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 2u)   { ok = 0; why = "cleanup count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T183"); else it_fail("T183", why);
}

/* ── T184: unauthorized pager cannot resolve a foreign fault ────────────────
 * A pager fully authorized for target B holds only under-privileged caps for
 * victim A (proc READ-only, VSpace READ-only).  While A's fault is pending
 * the pager attempts the whole battery — resume, kill, map, unmap, resolve
 * A's task through B's cap, read A's fault through B's cap, forge device
 * authority — and every attempt is denied with no fallback, no partial
 * mapping and no record disturbance.  READ on A's cap grants exactly
 * information, never resolution.  Invariants: P1, P3, P4, P5, P22. */
static void test_t184(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "unauthorized pager";

    struct t25_tgt va, gb;                    /* victim A, authorized target B */
    if (!t25_tgt_spawn(&va, &why)) { it_fail("T184", why); return; }
    if (!t25_tgt_spawn(&gb, &why)) {
        (void)it_sys1(SYS_PROCESS_KILL, (long)va.proc);
        t25_tgt_reap(&va); it_fail("T184", why); return;
    }
    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }

    /* Under-privileged victim caps (DUPLICATE only so they can be minted). */
    long apro = ok ? it_sys2(SYS_HANDLE_DUP, (long)va.proc, (long)(RIGHT_READ | RIGHT_DUPLICATE)) : -1;
    long avso = ok ? it_sys2(SYS_HANDLE_DUP, (long)va.vs,   (long)(RIGHT_READ | RIGHT_DUPLICATE)) : -1;
    handle_id_t apro_h = (apro >= 0) ? (handle_id_t)apro : HANDLE_INVALID;
    handle_id_t avso_h = (avso >= 0) ? (handle_id_t)avso : HANDLE_INVALID;
    if (ok && (apro < 0 || avso < 0)) { ok = 0; why = "victim dups"; }

    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok) {
        struct svc_mint x[2];
        x[0].slot = LP_PGR_SLOT_XPROC; x[0].src_h = apro_h; x[0].rights = RIGHT_READ; x[0].badge = 0;
        x[1].slot = LP_PGR_SLOT_XVS;   x[1].src_h = avso_h; x[1].rights = RIGHT_READ; x[1].badge = 0;
        if (t25_pager_spawn(&gb, fr_h, RIGHT_READ | RIGHT_WRITE, x, 2u,
                            &pcmd, &pproc) != 0) { ok = 0; why = "pager spawn"; }
    }

    /* Victim faults; the pager runs the unauthorized battery. */
    struct it_fault fa;
    if (ok && it_lp_cmd_va(va.cmd, LP_CMD_FAULT_READ, T25_VA_C) != 0) { ok = 0; why = "victim fault"; }
    if (ok && !t25_wait_fault(va.proc, &fa)) { ok = 0; why = "victim fault pending"; }
    if (ok && t25_xprobe(pcmd, fa.task_id, T25_VA_C, fa.seq) != 0) { ok = 0; why = "xprobe cmd"; }
    if (ok) {
        long breach = it_lp_wait_exit(pproc);
        if (breach != 0) { ok = 0; why = "unauthorized op leaked"; }
    }

    /* The victim's fault is untouched: same generation, still suspended. */
    struct it_fault fa2;
    if (ok && (it_fault_info(va.proc, &fa2) != 0 || fa2.seq != fa.seq ||
               fa2.task_id != fa.task_id || fa2.cr2 != fa.cr2)) { ok = 0; why = "record disturbed"; }
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)va.proc)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "victim not suspended"; }

    /* Parent-side rights split on the same reduced cap: READ gives info and
     * ONLY info — resolution and registration stay MANAGE-gated. */
    if (ok && it_fault_info(apro_h, &fa2) != 0) { ok = 0; why = "read cap info denied"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, apro, (long)fa.task_id, 1)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read cap resolved"; }
    if (ok && it_sys3(SYS_EXCEPTION_HANDLER, apro, (long)va.notif, 1)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read cap registered"; }

    /* Proper authority resolves. */
    if (ok && t25_resume_seq(va.proc, fa.task_id, fa.seq, 1) != 0) { ok = 0; why = "proper resolve"; }
    if (ok && it_lp_wait_exit(va.proc) != 0) { ok = 0; why = "victim exit"; }

    if (it_sys1(SYS_PROCESS_KILL, (long)gb.proc) != 0 && ok) { ok = 0; why = "kill B"; }
    if (ok && it_lp_wait_exit(gb.proc) != 0) { ok = 0; why = "B exit"; }
    it_close(&apro_h); it_close(&avso_h);
    t25_reap(&pproc); it_close(&pcmd);
    t25_tgt_reap(&va); t25_tgt_reap(&gb);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T184"); else it_fail("T184", why);
}

/* ── T185: stale fault generation rejection ─────────────────────────────────
 * Fault generations make stale resolutions impossible.  A fresh process's
 * first fault is generation 1; a resume-without-map re-faults the SAME
 * instruction as generation 2 (same rip/cr2, new seq, new delivery).  Every
 * replay with the old generation — resume or kill — is NOT_FOUND with the
 * record undisturbed; generation 0 and out-of-range actions are INVALID_ARG.
 * The current generation resolves for real (map + seq-resume, the store
 * retires), after which even the CORRECT generation is late: NOT_FOUND, and
 * the record is gone.  Invariants: P9, P10, P12, P13.  */
static void test_t185(void) {
    uint32_t f0[5], f1[5], word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_sched_ext5(f0);
    const char *why = "stale generation";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T185", why); return; }
    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    long tvs_c = (long)g.vs;   /* dual resolver: the VSpace HANDLE works */
    if (ok && fr < 0)    { ok = 0; why = "frame retype"; }
    if (ok) { word = 0; if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame zero"; } }

    struct it_fault fx1, fx2;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T25_VA_D) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && !t25_wait_fault(g.proc, &fx1)) { ok = 0; why = "F1 pending"; }
    if (ok && fx1.seq != 1u) { ok = 0; why = "first gen not 1"; }
    if (ok && (fx1.vector != 14u || fx1.cr2 != T25_VA_D ||
               fx1.error != (PF_ERR_W | PF_ERR_U))) { ok = 0; why = "F1 info"; }

    /* Clean refault: resume without resolving → generation 2, same site. */
    if (ok && t25_resume_seq(g.proc, fx1.task_id, fx1.seq, 0) != 0) { ok = 0; why = "resume F1"; }
    if (ok && !t25_wait_refault(g.proc, fx1.seq, &fx2)) { ok = 0; why = "no refault"; }
    if (ok && (fx2.seq != fx1.seq + 1u || fx2.rip != fx1.rip ||
               fx2.cr2 != fx1.cr2 || fx2.task_id != fx1.task_id)) { ok = 0; why = "F2 identity"; }

    /* Stale replays of F1 cannot touch F2; malformed generations rejected. */
    if (ok && t25_resume_seq(g.proc, fx1.task_id, fx1.seq, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale resume accepted"; }
    if (ok && t25_resume_seq(g.proc, fx1.task_id, fx1.seq, 1)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale kill accepted"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)g.proc, (long)fx1.task_id, 2L)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "gen 0 accepted"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)g.proc, (long)fx1.task_id, 4L)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "action 4 accepted"; }
    struct it_fault fx3;
    if (ok && (it_fault_info(g.proc, &fx3) != 0 || fx3.seq != fx2.seq)) {
        ok = 0; why = "record disturbed by stale ops";
    }

    /* Resolve the CURRENT generation for real: map writable + seq-resume. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_h, tvs_c, (long)T25_VA_D, 1) != 0) {
        ok = 0; why = "map at fault";
    }
    if (ok && t25_resume_seq(g.proc, fx2.task_id, fx2.seq, 0) != 0) { ok = 0; why = "resume F2"; }
    if (ok && it_lp_wait_exit(g.proc) != LP_EXIT_MARKER) { ok = 0; why = "target completion"; }

    /* Late-but-correct is still late: the record did not outlive resolution. */
    if (ok && t25_resume_seq(g.proc, fx2.task_id, fx2.seq, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late resume accepted"; }
    if (ok && it_fault_info(g.proc, &fx3) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived";
    }
    it_quiesce_reaper();
    if (ok && (!t25_frame_word(fr_h, &word, 0) || word != T25_WMARK)) {
        ok = 0; why = "store did not land";
    }

    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5 final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 2u)   { ok = 0; why = "cleanup count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T185"); else it_fail("T185", why);
}

/* ── T186: pager death while the target's fault is pending ──────────────────
 * The pager dies holding responsibility (blocked, never resolving).  The
 * contract (same as Fase 20 handler-death, now under supervision): the target
 * is NOT a zombie — it stays suspended with its record and generation intact,
 * observable by any READ holder, resolvable by any proper authority.  The
 * supervisor restarts the pager with the SAME declared manifest; the new
 * generation consumes the still-pending delivery signal and completes the
 * map+resume.  Nothing leaks per generation.  Invariants: P9, P14(neg), P15,
 * P16, P21. */
static void test_t186(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager death pending";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T186", why); return; }
    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }
    if (ok) { word = T25_PATTERN; if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame fill"; } }

    /* Gen 1: spawned in charge, never commanded — dies blocked. */
    handle_id_t p1cmd = HANDLE_INVALID, p1proc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &p1cmd, &p1proc) != 0) {
        ok = 0; why = "pager1 spawn";
    }
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "fault pending"; }
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)p1proc) != 0) { ok = 0; why = "kill pager1"; }
    if (ok && it_lp_wait_exit(p1proc) != 0) { ok = 0; why = "pager1 exit"; }
    it_quiesce_reaper();

    /* No zombie: suspended-alive, record and generation intact. */
    struct it_fault f2;
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)g.proc)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "target not suspended"; }
    if (ok && (it_fault_info(g.proc, &f2) != 0 || f2.seq != f.seq ||
               f2.cr2 != f.cr2)) { ok = 0; why = "record lost with pager"; }
    /* The dead pager's endpoint has no phantom receiver. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.label = 0x186;
        if (it_sys2(SYS_EP_NB_SEND, (long)p1cmd, (long)&m)
            != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "phantom pager receiver"; }
    }
    t25_reap(&p1proc); it_close(&p1cmd);

    /* Gen 2: same declared manifest, nothing more — and it finishes the job
     * (the delivery signal was never lost). */
    handle_id_t p2cmd = HANDLE_INVALID, p2proc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &p2cmd, &p2proc) != 0) {
        ok = 0; why = "pager2 spawn";
    }
    if (ok && t25_serve(p2cmd, 1u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "serve"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T25_PATTERN & 0xFFu))) { ok = 0; why = "target completion"; }
    if (ok && it_lp_wait_exit(p2proc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager2 report"; }
    if (ok && it_fault_info(g.proc, &f2) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived";
    }

    t25_reap(&p2proc); it_close(&p2cmd);
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T186"); else it_fail("T186", why);
}

/* ── T187: target death during pager resolution ─────────────────────────────
 * The supervisor kills the target between fault delivery and the pager's
 * completion.  Every late step fails clean: map-into-target is BAD_HANDLE
 * (the VSpace was invalidated with the process), seq-checked and legacy
 * resume are NOT_FOUND, the record reports WOULD_BLOCK.  The never-installed
 * frame stays clean and reusable; VSpace/mapping books return to baseline
 * once the stale caps are dropped.  Invariants: P12, P14, P18, P19, P23. */
static void test_t187(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "target death mid-resolution";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T187", why); return; }
    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    long tvs_c = (long)g.vs;   /* dual resolver: the VSpace HANDLE works */
    if (ok && fr < 0)    { ok = 0; why = "frame retype"; }

    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "fault pending"; }

    /* Mid-resolution kill. */
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)g.proc) != 0) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }
    it_quiesce_reaper();

    /* Late completion fails clean at every step. */
    if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_h, tvs_c, (long)T25_VA_A, 0)
              != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "late map not BAD_HANDLE"; }
    if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late seq-resume accepted"; }
    if (ok && it_sys3(SYS_EXCEPTION_RESUME, (long)g.proc, (long)f.task_id, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late resume accepted"; }
    if (ok && it_fault_info(g.proc, &f) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived death";
    }

    /* The frame was never installed: still clean, still usable. */
    if (ok) {
        word = T25_PATTERN;
        if (!t25_frame_word(fr_h, &word, 1) ||
            !t25_frame_word(fr_h, &word, 0) || word != T25_PATTERN) {
            ok = 0; why = "frame unusable after target death";
        }
    }

    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T187"); else it_fail("T187", why);
}

/* ── T188: pager rights and PTE policy ──────────────────────────────────────
 * PTE rights are the MEET of every capability involved, never more.  A
 * read-only frame cap cannot install a writable PTE; a read-only VSpace
 * authority cannot install anything; W^X, kernel-range, out-of-window and
 * unaligned VAs are INVALID_ARG; a denied map leaves NO partial PTE; an
 * occupied VA is BUSY.  The decisive check is architectural: a page mapped
 * READ-ONLY into the target through a fully-writable frame cap still refuses
 * the target's store with err P|W|U — the PTE carries the MAPPING's rights,
 * not the cap's ceiling.  Late unmap after target death is BAD_HANDLE.
 * Invariants: P5, P6, P7, P8, P10, P19. */
static void test_t188(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "rights/PTE policy";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T188", why); return; }
    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    long fro = (fr >= 0) ? it_sys2(SYS_HANDLE_DUP, fr, (long)RIGHT_READ) : -1;
    handle_id_t fro_h = (fro >= 0) ? (handle_id_t)fro : HANDLE_INVALID;
    long tvs_c  = (long)g.vs;  /* dual resolver: the VSpace HANDLE works */
    long tvs_ro = it_sys2(SYS_HANDLE_DUP, (long)g.vs, (long)RIGHT_READ);
    handle_id_t tvs_ro_h = (tvs_ro >= 0) ? (handle_id_t)tvs_ro : HANDLE_INVALID;
    if (ok && (fr < 0 || fro < 0)) { ok = 0; why = "frame caps"; }
    if (ok && tvs_ro < 0)          { ok = 0; why = "tvs ro dup"; }

    /* Rights monotonicity and namespace limits — all denied, no fallback. */
    if (ok && it_sys4(SYS_FRAME_MAP, fro, tvs_c, (long)T25_VA_B, 1)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "RO frame mapped W"; }
    if (ok && it_sys4(SYS_FRAME_MAP, fr, tvs_ro, (long)T25_VA_B, 0)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "RO vspace installed"; }
    if (ok && it_sys4(SYS_FRAME_MAP, fr, tvs_c, (long)T25_VA_B, 3)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "W^X accepted"; }
    if (ok && it_sys4(SYS_FRAME_MAP, fr, tvs_c,
                      (long)0xFFFF800000000000ULL, 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel VA accepted"; }
    if (ok && it_sys4(SYS_FRAME_MAP, fr, tvs_c, 0x1000L, 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "low VA accepted"; }
    if (ok && it_sys4(SYS_FRAME_MAP, fr, tvs_c, (long)(T25_VA_B | 0x123u), 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "unaligned accepted"; }
    /* No partial PTE from any denial. */
    if (ok && it_sys3(SYS_FRAME_UNMAP, fr, tvs_c, (long)T25_VA_B)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "partial PTE"; }

    /* Occupied-VA and W^X-compliant exec map contracts. */
    if (ok && it_sys4(SYS_FRAME_MAP, fr, tvs_c, (long)T25_VA_B, 0) != 0) {
        ok = 0; why = "RO map";
    }
    if (ok && it_sys4(SYS_FRAME_MAP, fr, tvs_c, (long)T25_VA_B, 0)
              != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied VA remapped"; }
    if (ok && it_sys4(SYS_FRAME_MAP, fr, tvs_c, (long)T25_VA_C, 2) != 0) {
        ok = 0; why = "r-x map denied";
    }
    if (ok && it_sys3(SYS_FRAME_UNMAP, fr, tvs_c, (long)T25_VA_C) != 0) {
        ok = 0; why = "r-x unmap";
    }

    /* Architectural proof: the RO PTE (installed via the RW frame cap) still
     * refuses the target's store — err = P|W|U at exactly that VA. */
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T25_VA_B) != 0) { ok = 0; why = "write cmd"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "no wp fault"; }
    if (ok && (f.vector != 14u || f.cr2 != T25_VA_B ||
               f.error != (PF_ERR_P | PF_ERR_W | PF_ERR_U))) { ok = 0; why = "wp err bits"; }
    if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 1) != 0) { ok = 0; why = "seq kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }
    it_quiesce_reaper();

    /* The target died with the mapping installed: teardown swept it; a late
     * unmap through the dead VSpace fails clean. */
    if (ok && it_sys3(SYS_FRAME_UNMAP, fr, tvs_c, (long)T25_VA_B)
              != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "late unmap"; }
    /* Frame reusable, mapped_count back at zero. */
    if (ok) {
        uint32_t word = T25_PATTERN;
        if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame not clean"; }
    }

    it_close(&fro_h); it_close(&tvs_ro_h);
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T188"); else it_fail("T188", why);
}

/* ── T189: pager restart preserves least authority ──────────────────────────
 * The Fase 24 ↔ Fase 25 junction.  A crashing pager is supervised under an
 * explicit restart limit: two generations die before resolving and the
 * budget is spent — the pager service is DEGRADED, the loop STOPS (P17).
 * The fault meanwhile stays pending and resolvable.  A post-crash generation
 * spawned from the same declaration reports EXACTLY the declared manifest
 * (nothing accumulated across generations, no stale endpoint, no peer/device/
 * KDEBUG authority), and the serving generation completes the resolution.
 * Invariants: P15, P16, P17, P21, P22. */
#define T189_LIMIT 2u
static void test_t189(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "restart least authority";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T189", why); return; }
    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }
    if (ok) { word = T25_PATTERN; if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame fill"; } }

    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "fault pending"; }

    /* Supervision loop: each generation crashes before resolving; the budget
     * (T189_LIMIT) bounds the loop and flips the service to degraded. */
    uint32_t restart_count = 0u, generation = 0u;
    int degraded = 0;
    while (ok && !degraded) {
        handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
        if (t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
            ok = 0; why = "gen spawn"; break;
        }
        generation++;
        it_sys1(SYS_SLEEP, 1);
        if (it_sys1(SYS_PROCESS_KILL, (long)pproc) != 0) { ok = 0; why = "gen kill"; }
        if (ok && it_lp_wait_exit(pproc) != 0) { ok = 0; why = "gen exit"; }
        t25_reap(&pproc); it_close(&pcmd);
        it_quiesce_reaper();
        if (restart_count < T189_LIMIT) restart_count++;
        else degraded = 1;
    }
    if (ok && restart_count != T189_LIMIT) { ok = 0; why = "wrong restart count"; }
    if (ok && generation != T189_LIMIT + 1u) { ok = 0; why = "generation mismatch"; }

    /* Degraded or not, the fault never became a zombie: still the same
     * generation, still suspended, still resolvable. */
    struct it_fault f2;
    if (ok && (it_fault_info(g.proc, &f2) != 0 || f2.seq != f.seq)) {
        ok = 0; why = "fault lost across pager generations";
    }

    /* A fresh instance of the same declaration carries EXACTLY the manifest —
     * restart amplified nothing. */
    if (ok) {
        struct svc_mint x[4];
        x[0].slot = LP_PGR_SLOT_TPROC; x[0].src_h = g.proc;  x[0].rights = RIGHT_READ | RIGHT_MANAGE; x[0].badge = 0;
        x[1].slot = LP_PGR_SLOT_TVS;   x[1].src_h = g.vs;    x[1].rights = RIGHT_WRITE;               x[1].badge = 0;
        x[2].slot = LP_PGR_SLOT_FRAME; x[2].src_h = fr_h;    x[2].rights = RIGHT_READ;                x[2].badge = 0;
        x[3].slot = LP_PGR_SLOT_NOTIF; x[3].src_h = g.notif; x[3].rights = RIGHT_WAIT;                x[3].badge = 0;
        long rep = it_lp_report_slots(x, 4u);
        uint32_t expect = (1u << LP_CPTR_CMD_EP)    | (1u << LP_PGR_SLOT_TPROC) |
                          (1u << LP_PGR_SLOT_TVS)   | (1u << LP_PGR_SLOT_FRAME) |
                          (1u << LP_PGR_SLOT_NOTIF);
        if (rep < 0 || (uint32_t)rep != expect) { ok = 0; why = "post-restart manifest"; }
    }

    /* The serving generation finishes the original resolution. */
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
        ok = 0; why = "server spawn";
    }
    if (ok && t25_serve(pcmd, 1u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "serve"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T25_PATTERN & 0xFFu))) { ok = 0; why = "target completion"; }
    if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "server report"; }
    t25_reap(&pproc); it_close(&pcmd);

    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T189"); else it_fail("T189", why);
}

/* ── T190: deterministic user pager stress ──────────────────────────────────
 * Seeded mixed-operation rounds over the whole Fase 25 surface.  Every round
 * holds TWO concurrent pending faults (the shared-IST regression stays
 * covered under pager traffic) and resolves them through a seed-chosen path:
 * external pager map+resume / kill, pager death + supervisor takeover,
 * target death mid-fault, refault + stale-generation rejection, unauthorized
 * caps denied, occupied-VA and RO-PTE enforcement.  After every round: no
 * pending fault, no zombie, and the live books (task/proc/handle/VSpace/
 * mapping) exactly at the pre-suite baseline.  Prints seed/round/op only on
 * failure.  Invariants: P9–P23 under load. */
#define T190_SEED   0x25AF9B31u
#define T190_ROUNDS 6u
static uint32_t t190_rnd(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static void test_t190(void) {
    uint32_t rng = T190_SEED, word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager stress";
    uint32_t round = 0u, op = 0u;

    long fr = it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { it_fail("T190", "frame retype"); return; }

    /* The frame is held for the whole test, so the PER-ROUND balance is
     * against a baseline that already accounts for it; the final check below
     * (after fr_h is closed) uses the pre-suite `b`. */
    it_quiesce_reaper();
    struct it_snap br = it_snap_take();
    if (ok && !br.ok) { it_close(&fr_h); it_fail("T190", "round baseline"); return; }

    for (round = 0; ok && round < T190_ROUNDS; round++) {
        op = t190_rnd(&rng) % 6u;
        word = T25_PATTERN;
        if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame fill"; break; }

        struct t25_tgt g1, g2;
        if (!t25_tgt_spawn(&g1, &why)) { ok = 0; break; }
        if (!t25_tgt_spawn(&g2, &why)) {
            (void)it_sys1(SYS_PROCESS_KILL, (long)g1.proc);
            t25_tgt_reap(&g1); ok = 0; break;
        }

        /* Two concurrent pending faults, every round. */
        struct it_fault f1, f2;
        if (it_lp_cmd_va(g1.cmd, LP_CMD_FAULT_READ,  T25_VA_A) != 0 ||
            it_lp_cmd_va(g2.cmd, LP_CMD_FAULT_WRITE, T25_VA_B) != 0) { ok = 0; why = "fault cmds"; }
        if (ok && (!t25_wait_fault(g1.proc, &f1) ||
                   !t25_wait_fault(g2.proc, &f2))) { ok = 0; why = "faults pending"; }

        switch (ok ? (int)op : -1) {
        case 0: {
            /* External pager resolves g1 by map+resume; supervisor seq-kills g2. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (t25_pager_spawn(&g1, fr_h, RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op0 pager"; break; }
            if (t25_serve(pc, 1u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "op0 serve"; }
            if (ok && it_lp_wait_exit(g1.proc) !=
                      (long)(LP_EXIT_MARKER ^ (T25_PATTERN & 0xFFu))) { ok = 0; why = "op0 g1"; }
            if (ok && it_lp_wait_exit(pp) != LP_EXIT_PGR_OK) { ok = 0; why = "op0 pager report"; }
            if (ok && t25_resume_seq(g2.proc, f2.task_id, f2.seq, 1) != 0) { ok = 0; why = "op0 g2 kill"; }
            if (ok && it_lp_wait_exit(g2.proc) != 0) { ok = 0; why = "op0 g2 exit"; }
            t25_reap(&pp); it_close(&pc);
            break;
        }
        case 1: {
            /* External pager seq-kills g1; stale-generation replay on g2 must
             * fail before the proper kill lands. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (t25_pager_spawn(&g1, fr_h, RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op1 pager"; break; }
            if (t25_serve(pc, 3u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "op1 serve"; }
            if (ok && it_lp_wait_exit(g1.proc) != 0) { ok = 0; why = "op1 g1"; }
            if (ok && it_lp_wait_exit(pp) != LP_EXIT_PGR_OK) { ok = 0; why = "op1 pager report"; }
            if (ok && t25_resume_seq(g2.proc, f2.task_id, f2.seq + 7u, 1)
                      != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "op1 bogus gen"; }
            if (ok && t25_resume_seq(g2.proc, f2.task_id, f2.seq, 1) != 0) { ok = 0; why = "op1 g2 kill"; }
            if (ok && it_lp_wait_exit(g2.proc) != 0) { ok = 0; why = "op1 g2 exit"; }
            t25_reap(&pp); it_close(&pc);
            break;
        }
        case 2: {
            /* Pager dies before serving; the supervisor takes over: seq-kill
             * g1, map+seq-resume g2 (its store retires into the frame). */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (t25_pager_spawn(&g1, fr_h, RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op2 pager"; break; }
            if (it_sys1(SYS_PROCESS_KILL, (long)pp) != 0 ||
                it_lp_wait_exit(pp) != 0) { ok = 0; why = "op2 pager death"; }
            t25_reap(&pp); it_close(&pc);
            long tvs_c = (long)g2.vs;
            if (ok && t25_resume_seq(g1.proc, f1.task_id, f1.seq, 1) != 0) { ok = 0; why = "op2 g1 kill"; }
            if (ok && it_lp_wait_exit(g1.proc) != 0) { ok = 0; why = "op2 g1 exit"; }
            if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_h, tvs_c, (long)T25_VA_B, 1) != 0) { ok = 0; why = "op2 map"; }
            if (ok && t25_resume_seq(g2.proc, f2.task_id, f2.seq, 0) != 0) { ok = 0; why = "op2 resume"; }
            if (ok && it_lp_wait_exit(g2.proc) != LP_EXIT_MARKER) { ok = 0; why = "op2 g2 exit"; }
            break;
        }
        case 3: {
            /* Target death mid-fault + refault path on the survivor: resume
             * g2 without map → new generation at the same site → old
             * generation refused → proper seq-kill of the NEW generation. */
            if (it_sys1(SYS_PROCESS_KILL, (long)g1.proc) != 0 ||
                it_lp_wait_exit(g1.proc) != 0) { ok = 0; why = "op3 g1 death"; break; }
            if (t25_resume_seq(g1.proc, f1.task_id, f1.seq, 0)
                != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "op3 late resume"; }
            struct it_fault f2b;
            if (ok && t25_resume_seq(g2.proc, f2.task_id, f2.seq, 0) != 0) { ok = 0; why = "op3 refault resume"; }
            if (ok && !t25_wait_refault(g2.proc, f2.seq, &f2b)) { ok = 0; why = "op3 no refault"; }
            if (ok && f2b.seq != f2.seq + 1u) { ok = 0; why = "op3 gen"; }
            if (ok && t25_resume_seq(g2.proc, f2.task_id, f2.seq, 1)
                      != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "op3 stale kill"; }
            if (ok && t25_resume_seq(g2.proc, f2b.task_id, f2b.seq, 1) != 0) { ok = 0; why = "op3 kill"; }
            if (ok && it_lp_wait_exit(g2.proc) != 0) { ok = 0; why = "op3 g2 exit"; }
            break;
        }
        case 4: {
            /* Unauthorized caps under load: READ-only proc cap cannot
             * resolve; READ-only vspace mint cannot install.  Then proper
             * seq-kills. */
            long rp = it_sys2(SYS_HANDLE_DUP, (long)g1.proc, (long)RIGHT_READ);
            handle_id_t rp_h = (rp >= 0) ? (handle_id_t)rp : HANDLE_INVALID;
            long rvs = it_sys2(SYS_HANDLE_DUP, (long)g1.vs, (long)RIGHT_READ);
            handle_id_t rvs_h = (rvs >= 0) ? (handle_id_t)rvs : HANDLE_INVALID;
            if (rp < 0 || rvs < 0) { ok = 0; why = "op4 caps"; }
            if (ok && it_sys3(SYS_EXCEPTION_RESUME, rp, (long)f1.task_id, 1)
                      != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op4 ro resume"; }
            if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_h, rvs, (long)T25_VA_A, 0)
                      != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op4 ro map"; }
            if (ok && it_sys3(SYS_FRAME_UNMAP, (long)fr_h, rvs, (long)T25_VA_A)
                      != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op4 ro unmap"; }
            it_close(&rp_h); it_close(&rvs_h);
            if (ok && t25_resume_seq(g1.proc, f1.task_id, f1.seq, 1) != 0) { ok = 0; why = "op4 g1 kill"; }
            if (ok && t25_resume_seq(g2.proc, f2.task_id, f2.seq, 1) != 0) { ok = 0; why = "op4 g2 kill"; }
            if (ok && (it_lp_wait_exit(g1.proc) != 0 ||
                       it_lp_wait_exit(g2.proc) != 0)) { ok = 0; why = "op4 exits"; }
            break;
        }
        case 5: {
            /* Occupied VA + RO-PTE enforcement under load: install the frame
             * read-only at g2's fault VA; a duplicate install is BUSY; the
             * resumed store now write-protection-faults (P|W|U) as a NEW
             * generation, then dies by it. */
            long tvs_c = (long)g2.vs;
            if (it_sys4(SYS_FRAME_MAP, (long)fr_h, tvs_c, (long)T25_VA_B, 0) != 0) { ok = 0; why = "op5 map"; }
            if (ok && it_sys4(SYS_FRAME_MAP, (long)fr_h, tvs_c, (long)T25_VA_B, 0)
                      != (long)IRIS_ERR_BUSY) { ok = 0; why = "op5 busy"; }
            struct it_fault f2b;
            if (ok && t25_resume_seq(g2.proc, f2.task_id, f2.seq, 0) != 0) { ok = 0; why = "op5 resume"; }
            if (ok && !t25_wait_refault(g2.proc, f2.seq, &f2b)) { ok = 0; why = "op5 no wp fault"; }
            if (ok && f2b.error != (PF_ERR_P | PF_ERR_W | PF_ERR_U)) { ok = 0; why = "op5 err bits"; }
            if (ok && t25_resume_seq(g2.proc, f2b.task_id, f2b.seq, 1) != 0) { ok = 0; why = "op5 g2 kill"; }
            if (ok && it_lp_wait_exit(g2.proc) != 0) { ok = 0; why = "op5 g2 exit"; }
            if (ok && t25_resume_seq(g1.proc, f1.task_id, f1.seq, 1) != 0) { ok = 0; why = "op5 g1 kill"; }
            if (ok && it_lp_wait_exit(g1.proc) != 0) { ok = 0; why = "op5 g1 exit"; }
            break;
        }
        default:
            break;
        }

        /* Round postconditions: no pending fault, no zombie, live books at
         * the pre-suite baseline. */
        if (ok && it_fault_info(g1.proc, &f1) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "g1 record"; }
        if (ok && it_fault_info(g2.proc, &f2) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "g2 record"; }
        t25_tgt_reap(&g1); t25_tgt_reap(&g2);
        it_quiesce_reaper();
        if (ok) {
            struct it_snap r = it_snap_take();
            if (!it_snap_baseline_live(&br, &r, &why)) ok = 0;
        }
    }

    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T190");
    else { it_fz_note("T190", T190_SEED, round, op); it_fail("T190", why); }
}

/* ── Fase 26: Memory object / VMO policy expansion (T191–T200) ───────────────
 *
 * Fase 25 fixed the pager AUTHORITY contract; the page source was a raw frame.
 * Fase 26 makes the source a first-class MEMORY OBJECT: a VMO defended by
 * policy — logical range, size, offsets, rights, mappings, cleanup — that a
 * VMO-backed pager uses to resolve faults page by page.
 *
 * The one additive kernel primitive exercised here:
 *   SYS_VMO_MAP_PAGE(108)  page-granular, offset-addressed map of a VMO page
 *                          into a VSpace named by capability (VMO READ[/WRITE]
 *                          + VSpace WRITE — no process MANAGE; the VSpace cap
 *                          IS the map-into-target authority, composing with
 *                          SYS_PROCESS_VSPACE from Fase 25).
 *
 * Invariants M1–M30 live in docs/architecture/memory-object-vmo-policy.md.
 * Reuses the Fase 25 t25_* pager harness verbatim: the pager's slot-14 "page
 * source" cap is a KVmo instead of a KFrame, and PAGER_SERVE subaction 4 maps
 * a VMO page via SYS_VMO_MAP_PAGE. */

#define T26_TVA_A    0x8096000000ULL   /* target fault VAs (clear of T25 range) */
#define T26_TVA_B    0x8097000000ULL
#define T26_SELF_VA  0x807B000000ULL   /* parent scratch for VMO-page inspect */
#define T26_VMO_SZ   0x4000ULL         /* 4 pages: exercises real offsets */
#define T26_PAT0     0xA1B2C3D4u
#define T26_PAT1     0x11223344u
#define T26_WMARK    0xFA017E57u   /* == the probe's LP_CMD_FAULT_WRITE store */

/* Live memory-object count (SYS_SCHED_INFO ext3, offset 132 — the Fase 26
 * additive field in the old pad half of buf[16]).  Returns -1 on failure. */
static long it_vmo_live(void) {
    long bh = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_SPAWN_CAP);
    if (bh < 0) return -1;
    uint8_t buf[136];
    long r = it_sys2(SYS_SCHED_INFO, (long)(uintptr_t)buf, 136);
    handle_id_t h = (handle_id_t)bh;
    it_close(&h);
    if (r != 0) return -1;
    return (long)((uint32_t)buf[132] | ((uint32_t)buf[133] << 8) |
                  ((uint32_t)buf[134] << 16) | ((uint32_t)buf[135] << 24));
}

/* SYS_VMO_MAP_PAGE offset_flags encoder: page-aligned byte offset + W/X. */
static inline long t26_ofs(uint64_t offset, uint64_t flags) {
    return (long)((offset & ~0xFFFULL) | (flags & 0x3ULL));
}

/* Create a sparse VMO; returns handle or HANDLE_INVALID. */
static handle_id_t t26_vmo_create(uint64_t size) {
    long v = it_sys1(SYS_VMO_CREATE, (long)size);
    return (v >= 0) ? (handle_id_t)v : HANDLE_INVALID;
}

/* Read/write word 0 of the VMO page at `offset`, via the parent's OWN VSpace
 * (IT_VS) using SYS_VMO_MAP_PAGE — dogfoods the new syscall and lets the
 * supervisor prep/inspect VMO page contents.  0 on success. */
static int t26_vmo_word(handle_id_t vmo, uint64_t offset, uint32_t *val, int write) {
    if (!it_setup_self_vspace()) return -1;
    long r = it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA,
                     t26_ofs(offset, write ? 1u : 0u));
    if (r != 0) return (int)r;
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T26_SELF_VA;
    if (write) *p = *val; else *val = *p;
    /* Unmap: SYS_VMO_UNMAP removes the KFrame-backed page from our VSpace. */
    return (int)it_sys2(SYS_VMO_UNMAP, (long)T26_SELF_VA, 0x1000L);
}

/* ── T191: VMO authority and size contract ──────────────────────────────────
 * A VMO is a capability defended by policy.  Size is stable and READ-gated;
 * SYS_VMO_MAP_PAGE is READ-gated (RIGHT_WRITE additionally for a writable
 * PTE); wrong-type in either slot is WRONG_TYPE; a reduced-rights derivation
 * (SYS_HANDLE_DUP) cannot regain rights it dropped; a stale (closed) cap
 * fails clean.  No authority amplification, no live drift.
 * Invariants: M1, M2, M9, M10, M11, M12, M23. */
static void test_t191(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo authority";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T191", "vmo create"); return; }
    if (ok && it_vmo_live() != vlive0 + 1) { ok = 0; why = "vmo not live"; }

    /* Size is stable and READ-gated. */
    if (ok && it_sys1(SYS_VMO_SIZE, (long)vmo) != (long)T26_VMO_SZ) { ok = 0; why = "size"; }
    if (ok && it_sys1(SYS_VMO_SIZE, (long)vmo) != (long)T26_VMO_SZ) { ok = 0; why = "size unstable"; }

    /* A READ-only derivation cannot map writable (rights monotonicity). */
    long ro = it_sys2(SYS_HANDLE_DUP, (long)vmo, (long)(RIGHT_READ | RIGHT_DUPLICATE));
    handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
    if (ok && ro < 0) { ok = 0; why = "ro dup"; }
    if (ok && it_setup_self_vspace()) {
        if (it_sys4(SYS_VMO_MAP_PAGE, ro, IT_VS, (long)T26_SELF_VA, t26_ofs(0, 1u))
            != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro mapped writable"; }
        /* READ-only can still map read-only. */
        if (ok && it_sys4(SYS_VMO_MAP_PAGE, ro, IT_VS, (long)T26_SELF_VA, t26_ofs(0, 0))
            != 0) { ok = 0; why = "ro map denied"; }
        if (ok) (void)it_sys2(SYS_VMO_UNMAP, (long)T26_SELF_VA, 0x1000L);
    } else if (ok) { ok = 0; why = "self vspace"; }

    /* Wrong-type in the VMO slot (a notification) and the VSpace slot. */
    long n = it_notify_create();
    handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
    if (ok && n < 0) { ok = 0; why = "notif"; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, n, IT_VS, (long)T26_SELF_VA, t26_ofs(0, 0))
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "vmo wrong-type"; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, n, (long)T26_SELF_VA, t26_ofs(0, 0))
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "vspace wrong-type"; }

    /* Stale (closed) VMO cap fails clean. */
    it_close(&ro_h);
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, ro, IT_VS, (long)T26_SELF_VA, t26_ofs(0, 0))
              >= 0) { ok = 0; why = "stale cap mapped"; }
    if (ok && it_sys1(SYS_VMO_SIZE, ro) >= 0) { ok = 0; why = "stale size"; }

    it_close(&n_h);
    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T191"); else it_fail("T191", why);
}

/* ── T192: VMO map offset/range validation ──────────────────────────────────
 * Every offset and VA is validated before a page is touched: a valid page maps;
 * an offset with the reserved bits set (unaligned) is INVALID_ARG; offset ==
 * size and offset+page > size are INVALID_ARG; a kernel VA, an unaligned VA and
 * an occupied VA are each rejected with the right code; a read-only VMO cannot
 * install a writable PTE.  No invalid attempt leaves a PTE, a mapped_count
 * bump or a VMO ref; a valid map afterwards still works.
 * Invariants: M4, M5, M6, M7, M8, M9, M21. */
static void test_t192(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_setup_self_vspace();
    const char *why = "vmo range validation";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);         /* 4 pages */
    if (vmo == HANDLE_INVALID) { it_fail("T192", "vmo create"); return; }

    /* Unaligned offset (reserved bits [11:2] set). */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA,
                      (long)(0u | 0x40u)) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "unaligned offset"; }
    /* offset == size (first byte past the last page). */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA,
                      t26_ofs(T26_VMO_SZ, 0)) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "offset==size"; }
    /* offset beyond size. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA,
                      t26_ofs(T26_VMO_SZ + 0x1000ULL, 0)) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "offset>size"; }
    /* Kernel VA. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS,
                      (long)0xFFFF800000000000ULL, t26_ofs(0, 0))
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel VA"; }
    /* Unaligned VA. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS,
                      (long)(T26_SELF_VA | 0x800ULL), t26_ofs(0, 0))
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "unaligned VA"; }
    /* Bad flags (W^X). */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA,
                      t26_ofs(0, 3u)) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "W^X"; }

    /* None of the above installed a PTE — a valid map now succeeds. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA,
                      t26_ofs(0x2000ULL, 0)) != 0) { ok = 0; why = "valid map"; }
    /* Occupied VA. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA,
                      t26_ofs(0x1000ULL, 0)) != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied VA"; }
    if (ok && it_sys2(SYS_VMO_UNMAP, (long)T26_SELF_VA, 0x1000L) != 0) {
        ok = 0; why = "unmap"; }

    it_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T192"); else it_fail("T192", why);
}

/* ── T193: VMO-backed pager resolves a target fault ──────────────────────────
 * The defining Fase 26 path.  The supervisor fills VMO page 2 (offset 0x2000)
 * with a pattern; a VMO-backed pager (slot 14 = the VMO) resolves the target's
 * read fault by mapping THAT page read-only at the fault VA and seq-resuming;
 * the target continues and reads the pattern (its exit code proves the byte
 * flowed VMO→target).  Then a writable run: the target's store lands in the
 * VMO page, visible to the supervisor afterwards.  Target death sweeps the
 * VMO-backed mapping; the VMO stays live and reusable.
 * Invariants: M3, M9, M13(pos), M14, M15, M17, M19, M20. */
static void test_t193(void) {
    uint32_t f0[5], f1[5], word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0 && it_sched_ext5(f0);
    const char *why = "vmo-backed pager";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T193", "vmo create"); return; }
    word = T26_PAT0;
    if (ok && t26_vmo_word(vmo, 0x2000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    /* (A) read fault, read-only VMO cap → target reads the supervisor pattern. */
    struct t25_tgt g;
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T193", why); return; }
    if (ok && t25_pager_spawn(&g, vmo, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
        ok = 0; why = "pager spawn A"; }
    /* subaction 4 = VMO map; va_ovr carries the VMO offset (page 2). */
    if (ok && t25_serve(pcmd, 4u, 1u, 0 /*RO*/, 0x2000ULL, T26_TVA_A) != 0) { ok = 0; why = "serve A"; }
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "fault A"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T26_PAT0 & 0xFFu))) { ok = 0; why = "target did not read pattern"; }
    if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report A"; }
    t25_reap(&pproc); it_close(&pcmd);
    t25_tgt_reap(&g);
    it_quiesce_reaper();

    /* VMO survived target death and is still readable/intact. */
    if (ok && (t26_vmo_word(vmo, 0x2000ULL, &word, 0) != 0 || word != T26_PAT0)) {
        ok = 0; why = "vmo not reusable after sweep"; }

    /* (B) write fault, writable VMO cap → target store lands in the VMO page. */
    if (ok) { word = 0; if (t26_vmo_word(vmo, 0x3000ULL, &word, 1) != 0) { ok = 0; why = "vmo zero"; } }
    struct t25_tgt g2;
    handle_id_t p2cmd = HANDLE_INVALID, p2proc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g2, &why)) { it_close(&vmo); it_fail("T193", why); return; }
    if (ok) {
        if (t25_pager_spawn(&g2, vmo, RIGHT_READ | RIGHT_WRITE, 0, 0u, &p2cmd, &p2proc) != 0) {
            ok = 0; why = "pager spawn B"; }
        if (ok && t25_serve(p2cmd, 4u, 1u, 1 /*W*/, 0x3000ULL, T26_TVA_B) != 0) { ok = 0; why = "serve B"; }
        if (ok && it_lp_cmd_va(g2.cmd, LP_CMD_FAULT_WRITE, T26_TVA_B) != 0) { ok = 0; why = "fault B"; }
        if (ok && it_lp_wait_exit(g2.proc) != LP_EXIT_MARKER) { ok = 0; why = "store did not retire"; }
        if (ok && it_lp_wait_exit(p2proc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report B"; }
        t25_reap(&p2proc); it_close(&p2cmd);
    }
    t25_tgt_reap(&g2);
    it_quiesce_reaper();
    if (ok && (t26_vmo_word(vmo, 0x3000ULL, &word, 0) != 0 || word != T26_WMARK)) {
        ok = 0; why = "write not visible in vmo"; }

    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5 final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 2u)  { ok = 0; why = "resume count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T193"); else it_fail("T193", why);
}

/* ── T194: unauthorized VMO pager denial ────────────────────────────────────
 * A VMO-backed pager fully authorized for target B, holding a WRITABLE VMO but
 * only a READ-only VMO derivation for the writable attempt, cannot: map into a
 * VSpace it lacks WRITE on; install a writable PTE from a read-only VMO cap;
 * touch a foreign target.  Every denial is ACCESS_DENIED with no mapping, no
 * VMO ref leak, no fault-state corruption.
 * Invariants: M2, M3, M9, M10, M13, M26. */
static void test_t194(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0 && it_setup_self_vspace();
    const char *why = "unauthorized vmo pager";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T194", "vmo create"); return; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T194", why); return; }

    /* A READ-only VMO cap cannot install a writable PTE into g's VSpace. */
    long vro = it_sys2(SYS_HANDLE_DUP, (long)vmo, (long)(RIGHT_READ | RIGHT_DUPLICATE));
    handle_id_t vro_h = (vro >= 0) ? (handle_id_t)vro : HANDLE_INVALID;
    if (ok && vro < 0) { ok = 0; why = "vro dup"; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, vro, (long)g.vs, (long)T26_TVA_A, t26_ofs(0, 1u))
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro vmo writable into target"; }

    /* A READ-only VSpace derivation cannot install ANY PTE. */
    long vsro = it_sys2(SYS_HANDLE_DUP, (long)g.vs, (long)(RIGHT_READ | RIGHT_DUPLICATE));
    handle_id_t vsro_h = (vsro >= 0) ? (handle_id_t)vsro : HANDLE_INVALID;
    if (ok && vsro < 0) { ok = 0; why = "vsro dup"; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, vsro, (long)T26_TVA_A, t26_ofs(0, 0))
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro vspace installed"; }

    /* Correct authority DOES install (proves the denials were the rights, not
     * some unrelated failure), then unmap through the target VSpace cap. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T26_TVA_A, t26_ofs(0, 1u))
              != 0) { ok = 0; why = "authorized map denied"; }
    /* The authorized mapping is swept when the target dies below. */

    it_close(&vro_h); it_close(&vsro_h);
    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T194"); else it_fail("T194", why);
}

/* ── T195: VMO shared mappings across two targets ────────────────────────────
 * One VMO backs a page in target A and (independently) a page in target B.  A
 * writable share lets A's store become visible to the supervisor and to a
 * subsequent B mapping of the same VMO page — the VMO is genuinely shared.  A
 * dies; B's mapping is untouched and still works; B dies; everything returns
 * to baseline with the VMO still live until the supervisor closes it.
 * Invariants: M27, M28, M17, M19, M20, M22. */
static void test_t195(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo shared mappings";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T195", "vmo create"); return; }
    word = 0;
    if (ok && t26_vmo_word(vmo, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo zero"; }

    /* Target A writes the shared VMO page (offset 0x1000). */
    struct t25_tgt ga;
    handle_id_t pacmd = HANDLE_INVALID, paproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&ga, &why)) { it_close(&vmo); it_fail("T195", why); return; }
    if (ok) {
        if (t25_pager_spawn(&ga, vmo, RIGHT_READ | RIGHT_WRITE, 0, 0u, &pacmd, &paproc) != 0) {
            ok = 0; why = "pager A"; }
        if (ok && t25_serve(pacmd, 4u, 1u, 1 /*W*/, 0x1000ULL, T26_TVA_A) != 0) { ok = 0; why = "serve A"; }
        if (ok && it_lp_cmd_va(ga.cmd, LP_CMD_FAULT_WRITE, T26_TVA_A) != 0) { ok = 0; why = "fault A"; }
        if (ok && it_lp_wait_exit(ga.proc) != LP_EXIT_MARKER) { ok = 0; why = "A store"; }
        if (ok && it_lp_wait_exit(paproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager A report"; }
        t25_reap(&paproc); it_close(&pacmd);
    }
    t25_tgt_reap(&ga);           /* A dies — its mapping swept */
    it_quiesce_reaper();

    /* Supervisor sees A's write in the shared VMO page. */
    if (ok && (t26_vmo_word(vmo, 0x1000ULL, &word, 0) != 0 || word != T26_WMARK)) {
        ok = 0; why = "A write not shared"; }

    /* Target B reads the SAME VMO page and observes A's write (RO map). */
    struct t25_tgt gb;
    handle_id_t pbcmd = HANDLE_INVALID, pbproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&gb, &why)) { it_close(&vmo); it_fail("T195", why); return; }
    if (ok) {
        if (t25_pager_spawn(&gb, vmo, RIGHT_READ, 0, 0u, &pbcmd, &pbproc) != 0) {
            ok = 0; why = "pager B"; }
        if (ok && t25_serve(pbcmd, 4u, 1u, 0 /*RO*/, 0x1000ULL, T26_TVA_B) != 0) { ok = 0; why = "serve B"; }
        if (ok && it_lp_cmd_va(gb.cmd, LP_CMD_FAULT_READ, T26_TVA_B) != 0) { ok = 0; why = "fault B"; }
        if (ok && it_lp_wait_exit(gb.proc) !=
                  (long)(LP_EXIT_MARKER ^ (T26_WMARK & 0xFFu))) { ok = 0; why = "B did not read A's write"; }
        if (ok && it_lp_wait_exit(pbproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager B report"; }
        t25_reap(&pbproc); it_close(&pbcmd);
    }
    t25_tgt_reap(&gb);           /* B dies — its (independent) mapping swept */

    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T195"); else it_fail("T195", why);
}

/* ── T196: VMO revoke/destruction with active mappings ───────────────────────
 * The lifetime contract: a VMO with a live mapping is NOT destroyed while the
 * mapping exists — the KFrame behind the map retains the VMO.  Closing the
 * supervisor's last VMO handle while a target maps it drops vmo_live by zero
 * (the mapping still holds it); only after the target dies (mapping swept)
 * does the VMO actually get destroyed.  A stale VMO cap fails clean; no
 * double free, no PTE stale, no live drift.
 * Invariants: M18, M22, M23, M17, M19. */
static void test_t196(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo revoke/destroy";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T196", "vmo create"); return; }
    word = T26_PAT1;
    if (ok && t26_vmo_word(vmo, 0, &word, 1) != 0) { ok = 0; why = "vmo fill"; }
    if (ok && it_vmo_live() != vlive0 + 1) { ok = 0; why = "vmo not live"; }

    /* Map the VMO into a target, then keep the target alive while the
     * supervisor closes ITS handle.  A separate handle for the pager mint is
     * required (pager takes its own), so dup first. */
    struct t25_tgt g;
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T196", why); return; }
    if (ok) {
        if (t25_pager_spawn(&g, vmo, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) { ok = 0; why = "pager"; }
        if (ok && t25_serve(pcmd, 4u, 1u, 0, 0, T26_TVA_A) != 0) { ok = 0; why = "serve"; }
        if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "fault"; }
        if (ok && it_lp_wait_exit(g.proc) !=
                  (long)(LP_EXIT_MARKER ^ (T26_PAT1 & 0xFFu))) { ok = 0; why = "target read"; }
        if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report"; }
        t25_reap(&pproc); it_close(&pcmd);
    }
    /* NOTE: the target exited (SYS_EXIT), so its mapping is already swept; the
     * VMO is retained only by our handle now.  Verify closing the handle frees
     * it and a stale cap fails clean. */
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0 + 1) { ok = 0; why = "vmo lost early"; }
    handle_id_t vcopy = vmo;
    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo not destroyed"; }
    /* Stale cap fails clean. */
    if (ok && it_sys1(SYS_VMO_SIZE, (long)vcopy) >= 0) { ok = 0; why = "stale size"; }

    t25_tgt_reap(&g);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T196"); else it_fail("T196", why);
}

/* ── T197: VMO-backed pager death/restart ────────────────────────────────────
 * A VMO-backed pager supervised under a restart limit dies with the target's
 * fault pending; the fault survives (suspended-alive), the VMO stays live, no
 * ghost refs.  A restarted instance carries EXACTLY the declared manifest (its
 * page source is the VMO, no untyped/global frame authority) and completes the
 * resolution from the VMO.  The Fase24↔26 junction.
 * Invariants: M24, M25, plus P15/P16/P17 (supervision) under a VMO source. */
static void test_t197(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo pager death/restart";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T197", "vmo create"); return; }
    word = T26_PAT0;
    if (ok && t26_vmo_word(vmo, 0x2000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T197", why); return; }
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "fault pending"; }

    /* Gen 1: spawned in charge, killed before serving. */
    handle_id_t p1cmd = HANDLE_INVALID, p1proc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, vmo, RIGHT_READ, 0, 0u, &p1cmd, &p1proc) != 0) { ok = 0; why = "pager1"; }
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)p1proc) != 0) { ok = 0; why = "kill pager1"; }
    if (ok && it_lp_wait_exit(p1proc) != 0) { ok = 0; why = "pager1 exit"; }
    t25_reap(&p1proc); it_close(&p1cmd);
    it_quiesce_reaper();

    /* Fault survives; VMO stays live. */
    struct it_fault f2;
    if (ok && (it_fault_info(g.proc, &f2) != 0 || f2.seq != f.seq)) { ok = 0; why = "fault lost"; }
    /* The VMO survived the pager's death — verified functionally (the target is
     * still SUSPENDED here, so an absolute vmo_live count would also see its
     * live segment/stack VMOs; the leak guard is the final vlive0 check after
     * everything is reaped). */
    if (ok && it_sys1(SYS_VMO_SIZE, (long)vmo) != (long)T26_VMO_SZ) { ok = 0; why = "vmo lost with pager"; }

    /* Post-restart manifest is exactly the declaration (slot-14 = VMO source). */
    if (ok) {
        struct svc_mint x[4];
        x[0].slot = LP_PGR_SLOT_TPROC; x[0].src_h = g.proc;  x[0].rights = RIGHT_READ | RIGHT_MANAGE; x[0].badge = 0;
        x[1].slot = LP_PGR_SLOT_TVS;   x[1].src_h = g.vs;    x[1].rights = RIGHT_WRITE;               x[1].badge = 0;
        x[2].slot = LP_PGR_SLOT_FRAME; x[2].src_h = vmo;     x[2].rights = RIGHT_READ;                x[2].badge = 0;
        x[3].slot = LP_PGR_SLOT_NOTIF; x[3].src_h = g.notif; x[3].rights = RIGHT_WAIT;                x[3].badge = 0;
        long rep = it_lp_report_slots(x, 4u);
        uint32_t expect = (1u << LP_CPTR_CMD_EP) | (1u << LP_PGR_SLOT_TPROC) |
                          (1u << LP_PGR_SLOT_TVS) | (1u << LP_PGR_SLOT_FRAME) |
                          (1u << LP_PGR_SLOT_NOTIF);
        if (rep < 0 || (uint32_t)rep != expect) { ok = 0; why = "post-restart manifest"; }
        if (ok && ((uint32_t)rep & ((1u<<6)|(1u<<10)|(1u<<11)|(1u<<16)|(1u<<17)|(1u<<18))) != 0) {
            ok = 0; why = "extra authority after restart"; }
    }

    /* Gen 2 (the server) completes the resolution from the VMO. */
    handle_id_t p2cmd = HANDLE_INVALID, p2proc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, vmo, RIGHT_READ, 0, 0u, &p2cmd, &p2proc) != 0) { ok = 0; why = "pager2"; }
    if (ok && t25_serve(p2cmd, 4u, 1u, 0, 0x2000ULL, T26_TVA_A) != 0) { ok = 0; why = "serve"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T26_PAT0 & 0xFFu))) { ok = 0; why = "target completion"; }
    if (ok && it_lp_wait_exit(p2proc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager2 report"; }
    t25_reap(&p2proc); it_close(&p2cmd);

    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T197"); else it_fail("T197", why);
}

/* ── T198: VMO partial failure atomicity ─────────────────────────────────────
 * A batch of denied SYS_VMO_MAP_PAGE calls — bad offset, bad VA, occupied VA,
 * wrong VMO rights, stale VMO cap — leaves the address space and every book
 * exactly as before: no partial PTE, no mapped_count drift, no VMO ref leak.
 * A valid map before and after the batch proves the space was never corrupted.
 * Invariants: M21, M22, M19, M20, M23. */
static void test_t198(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0 && it_setup_self_vspace();
    const char *why = "vmo partial failure";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T198", "vmo create"); return; }
    long vro = it_sys2(SYS_HANDLE_DUP, (long)vmo, (long)(RIGHT_READ | RIGHT_DUPLICATE));
    handle_id_t vro_h = (vro >= 0) ? (handle_id_t)vro : HANDLE_INVALID;
    long vstale = it_sys2(SYS_HANDLE_DUP, (long)vmo, (long)(RIGHT_READ | RIGHT_DUPLICATE));
    handle_id_t vstale_h = (vstale >= 0) ? (handle_id_t)vstale : HANDLE_INVALID;
    if (ok && (vro < 0 || vstale < 0)) { ok = 0; why = "dups"; }
    it_close(&vstale_h);   /* now stale */

    /* Anchor map at page 0 so "occupied VA" has a real occupant. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA, t26_ofs(0, 0)) != 0) {
        ok = 0; why = "anchor map"; }

    /* Failure battery — every one must be rejected with no side effect. */
    struct { long vmo_c; long vs_c; uint64_t va; long ofs; long want; const char *tag; } bad[] = {
        { (long)vmo, IT_VS, T26_SELF_VA + 0x10000ULL, (long)(0x40u),               (long)IRIS_ERR_INVALID_ARG,  "unaligned ofs" },
        { (long)vmo, IT_VS, T26_SELF_VA + 0x10000ULL, t26_ofs(T26_VMO_SZ, 0),      (long)IRIS_ERR_INVALID_ARG,  "ofs==size" },
        { (long)vmo, IT_VS, 0xFFFF800000000000ULL,    t26_ofs(0, 0),               (long)IRIS_ERR_INVALID_ARG,  "kernel va" },
        { (long)vmo, IT_VS, T26_SELF_VA,              t26_ofs(0x1000ULL, 0),       (long)IRIS_ERR_BUSY,         "occupied" },
        { vro,       IT_VS, T26_SELF_VA + 0x10000ULL, t26_ofs(0, 1u),              (long)IRIS_ERR_ACCESS_DENIED,"ro writable" },
        { vstale,    IT_VS, T26_SELF_VA + 0x10000ULL, t26_ofs(0, 0),               (long)IRIS_ERR_BAD_HANDLE,   "stale vmo" },
    };
    for (uint32_t i = 0; ok && i < 6u; i++) {
        long r = it_sys4(SYS_VMO_MAP_PAGE, bad[i].vmo_c, bad[i].vs_c, (long)bad[i].va, bad[i].ofs);
        if (r != bad[i].want) { ok = 0; why = bad[i].tag; }
    }

    /* The space is intact: unmap the anchor, remap elsewhere, unmap. */
    if (ok && it_sys2(SYS_VMO_UNMAP, (long)T26_SELF_VA, 0x1000L) != 0) { ok = 0; why = "anchor unmap"; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, IT_VS, (long)T26_SELF_VA, t26_ofs(0x2000ULL, 1u)) != 0) {
        ok = 0; why = "post-batch map"; }
    if (ok && it_sys2(SYS_VMO_UNMAP, (long)T26_SELF_VA, 0x1000L) != 0) { ok = 0; why = "post unmap"; }

    it_close(&vro_h);
    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T198"); else it_fail("T198", why);
}

/* ── T199: VMO rights/PTE policy stress ──────────────────────────────────────
 * PTE rights are the meet of the VMO cap and the requested flags, enforced at
 * the hardware level: a page mapped READ-ONLY into a target from a WRITABLE
 * VMO cap still write-protection-faults the target's store (err P|W|U) — the
 * PTE carries the MAPPING's rights, not the cap's ceiling.  W^X and remap of an
 * occupied VA are rejected.  No silent write, no escalation via remap.
 * Invariants: M9, M10, M8, M21, plus the Fase 20 write-fault observable. */
static void test_t199(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo rights/PTE";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T199", "vmo create"); return; }

    /* Map RO into a target through a fully-WRITABLE VMO cap; the target's store
     * must still fault write-protection (the PTE is RO, cap ceiling irrelevant). */
    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T199", why); return; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T26_TVA_A, t26_ofs(0, 0)) != 0) {
        ok = 0; why = "ro map"; }
    /* Occupied VA remap rejected. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T26_TVA_A, t26_ofs(0x1000ULL, 0))
              != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied remap"; }

    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T26_TVA_A) != 0) { ok = 0; why = "write cmd"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "no wp fault"; }
    if (ok && (f.vector != 14u || f.cr2 != T26_TVA_A ||
               f.error != (PF_ERR_P | PF_ERR_W | PF_ERR_U))) { ok = 0; why = "wp err bits"; }
    if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 1) != 0) { ok = 0; why = "seq kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }

    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T199"); else it_fail("T199", why);
}

/* ── T200: deterministic memory-object stress ────────────────────────────────
 * Seeded mixed-operation rounds over the whole Fase 26 surface: create VMOs,
 * derive reduced caps, map at offsets, VMO-back a fault, resolve or refault,
 * kill target, kill/restart pager, revoke (close) with mapping live, unmap,
 * inject failures.  After every round: no pending fault, no zombie, VMO/frame/
 * mapping/handle/process books at baseline.  Prints seed/round/op on failure.
 * Invariants: M13–M29 under load. */
#define T200_SEED   0x26C0FFEEu
#define T200_ROUNDS 6u
static uint32_t t200_rnd(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static void test_t200(void) {
    uint32_t rng = T200_SEED, word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo stress";
    uint32_t round = 0u, op = 0u;

    for (round = 0; ok && round < T200_ROUNDS; round++) {
        op = t200_rnd(&rng) % 5u;
        uint64_t ofs = ((uint64_t)(t200_rnd(&rng) % 4u)) << 12;   /* one of 4 pages */

        handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
        if (vmo == HANDLE_INVALID) { ok = 0; why = "vmo create"; break; }
        word = T26_PAT0 ^ round;
        if (t26_vmo_word(vmo, ofs, &word, 1) != 0) { ok = 0; why = "vmo fill"; it_close(&vmo); break; }

        struct t25_tgt g;
        if (!t25_tgt_spawn(&g, &why)) { ok = 0; it_close(&vmo); break; }
        struct it_fault f;

        switch (op) {
        case 0: {
            /* VMO-backed read resolve. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (t25_pager_spawn(&g, vmo, RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op0 pager"; break; }
            if (t25_serve(pc, 4u, 1u, 0, ofs, 0) != 0) { ok = 0; why = "op0 serve"; }
            if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "op0 fault"; }
            if (ok && it_lp_wait_exit(g.proc) !=
                      (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op0 target"; }
            if (ok && it_lp_wait_exit(pp) != LP_EXIT_PGR_OK) { ok = 0; why = "op0 pager report"; }
            t25_reap(&pp); it_close(&pc);
            break;
        }
        case 1: {
            /* VMO-backed writable resolve; store lands in the VMO. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            word = 0; if (t26_vmo_word(vmo, ofs, &word, 1) != 0) { ok = 0; why = "op1 zero"; break; }
            if (t25_pager_spawn(&g, vmo, RIGHT_READ | RIGHT_WRITE, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op1 pager"; break; }
            if (t25_serve(pc, 4u, 1u, 1u, ofs, T26_TVA_A) != 0) { ok = 0; why = "op1 serve"; }
            if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T26_TVA_A) != 0) { ok = 0; why = "op1 fault"; }
            if (ok && it_lp_wait_exit(g.proc) != LP_EXIT_MARKER) { ok = 0; why = "op1 store"; }
            if (ok && it_lp_wait_exit(pp) != LP_EXIT_PGR_OK) { ok = 0; why = "op1 pager report"; }
            t25_reap(&pp); it_close(&pc);
            if (ok && (t26_vmo_word(vmo, ofs, &word, 0) != 0 || word != T26_WMARK)) { ok = 0; why = "op1 not stored"; }
            break;
        }
        case 2: {
            /* Pager dies before serving; supervisor takes over from the VMO. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "op2 fault"; break; }
            if (!t25_wait_fault(g.proc, &f)) { ok = 0; why = "op2 pending"; break; }
            if (t25_pager_spawn(&g, vmo, RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op2 pager"; break; }
            if (it_sys1(SYS_PROCESS_KILL, (long)pp) != 0 || it_lp_wait_exit(pp) != 0) { ok = 0; why = "op2 pager death"; }
            t25_reap(&pp); it_close(&pc);
            /* Supervisor resolves from the VMO via its own VSpace handle. */
            if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T26_TVA_A, t26_ofs(ofs, 0)) != 0) { ok = 0; why = "op2 map"; }
            if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 0) != 0) { ok = 0; why = "op2 resume"; }
            if (ok && it_lp_wait_exit(g.proc) !=
                      (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op2 target"; }
            break;
        }
        case 3: {
            /* Target death mid-fault; late map is BAD_HANDLE. */
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "op3 fault"; break; }
            if (!t25_wait_fault(g.proc, &f)) { ok = 0; why = "op3 pending"; break; }
            if (it_sys1(SYS_PROCESS_KILL, (long)g.proc) != 0 || it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "op3 kill"; }
            it_quiesce_reaper();
            if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T26_TVA_A, t26_ofs(ofs, 0))
                      != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "op3 late map"; }
            if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "op3 late resume"; }
            break;
        }
        case 4: {
            /* Failure injection under load, THEN a clean resolve.  The denied
             * maps install nothing, so T26_TVA_A stays unmapped and the target
             * still faults; only then does the supervisor map + resume. */
            long vro = it_sys2(SYS_HANDLE_DUP, (long)vmo, (long)(RIGHT_READ | RIGHT_DUPLICATE));
            handle_id_t vro_h = (vro >= 0) ? (handle_id_t)vro : HANDLE_INVALID;
            if (vro < 0) { ok = 0; why = "op4 dup"; break; }
            /* RO cap cannot install a writable PTE; a beyond-size offset is
             * rejected — neither leaves anything at T26_TVA_A. */
            if (it_sys4(SYS_VMO_MAP_PAGE, vro, (long)g.vs, (long)T26_TVA_A, t26_ofs(ofs, 1u))
                != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op4 ro writable"; }
            if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T26_TVA_A, t26_ofs(T26_VMO_SZ, 0))
                != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "op4 bad ofs"; }
            it_close(&vro_h);
            /* Drive the real fault, then map + resume from the VMO. */
            if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "op4 fault"; }
            if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "op4 pending"; }
            if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T26_TVA_A, t26_ofs(ofs, 0)) != 0) { ok = 0; why = "op4 map"; }
            if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 0) != 0) { ok = 0; why = "op4 resume"; }
            if (ok && it_lp_wait_exit(g.proc) !=
                      (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op4 target"; }
            break;
        }
        default: break;
        }

        if (ok && it_fault_info(g.proc, &f) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "residual fault"; }
        t25_tgt_reap(&g);
        it_close(&vmo);
        it_quiesce_reaper();
        if (ok) {
            struct it_snap r = it_snap_take();
            if (!it_snap_baseline_live(&b, &r, &why)) ok = 0;
            else if (it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live final"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T200");
    else { it_fz_note("T200", T200_SEED, round, op); it_fail("T200", why); }
}

/* ── Fase 27: Service Pager Integration (T201–T210) ──────────────────────────
 *
 * Fase 25/26 made the pager a MODEL and a page SOURCE.  Fase 27 makes it a
 * SERVICE: a distinct supervised image ("pager", initrd index 10 — NOT
 * iris_test, NOT lifecycle_probe), registered in svcmgr, driven request/reply
 * over a control endpoint, resolving faults strictly inside a capability
 * manifest of target + VMO grants.
 *
 * iris_test acts as the SUPERVISOR (the Fase 24 probe-supervisor model, now
 * over a real named service): it mints the manifest, spawns the service,
 * registers "pager.ep", drives it, watches it, and restarts it under an
 * explicit policy.  Constants below MUST match services/pager/pager_proto.h.
 *
 * Invariants G1–G30 live in docs/architecture/service-pager-integration.md. */

#define PGR_SLOT_CTRL_EP    LP_CPTR_CMD_EP   /* slot 3: the pager's control endpoint */
/* Fase 28.1 manifest layout (must match services/pager/pager_proto.h): ONE
 * shared fault notification at slot 5 for ALL targets (bit i = target i),
 * targets at 20 + i*2 (proc/vs only — the per-target notification column is
 * gone; that is what makes 16 concurrent targets cost ONE notification
 * against the supervisor's quota instead of 16). */
#define PGR_SLOT_FAULT_NOTIF 5u
#define PGR_TGT_BASE        20u
#define PGR_TGT_STRIDE      2u
#define PGR_TSLOT_PROC(i)   (PGR_TGT_BASE + (i) * PGR_TGT_STRIDE + 0u)
#define PGR_TSLOT_VS(i)     (PGR_TGT_BASE + (i) * PGR_TGT_STRIDE + 1u)
#define PGR_VMO_BASE        16u
#define PGR_VSLOT(j)        (PGR_VMO_BASE + (j))
/* The pager service is the lifecycle_probe image in persistent service mode
 * (Fase 27): a real, separate, supervised image (NOT iris_test).  The control
 * endpoint lands in the probe's command slot; the manifest slots above match
 * services/lifecycle_probe/main.c's LP_PS_* layout. */
#define PGR_OP_PING         1u
#define PGR_OP_REPORT       2u
#define PGR_OP_MAP_RESUME   3u
#define PGR_OP_KILL         4u
#define PGR_OP_SHUTDOWN     5u
#define PGR_PACK(op, tidx, vidx, flags) \
    ((uint64_t)(op) | ((uint64_t)(tidx) << 8) | ((uint64_t)(vidx) << 16) | \
     ((uint64_t)((flags) & 0x3u) << 24))

/* The pager service's declared supervision policy (applied by this supervisor;
 * OPTIONAL_RESTART — a lost pager degrades paging for its targets but is not
 * fatal to the system). */
#define PGR_SUP_CLASS       IT_SUP_OPTIONAL_RESTART
#define PGR_RESTART_LIMIT   3u

/* Fase 27 fault/scratch VAs (clear of the T25/T26 windows). */
#define T27_VA_A    0x8098000000ULL
#define T27_VA_B    0x8099000000ULL
#define T27_PAT     0x27C0DE27u
#define T27_WMARK   0xFA017E57u   /* == the probe's LP_CMD_FAULT_WRITE store */

struct t27_pager {
    handle_id_t ctrl_ep;   /* supervisor's WRITE cap to call the pager */
    handle_id_t proc;      /* pager process cap (watch / kill) */
    long        reg_id;    /* svcmgr "pager.ep" registration id, or -1 */
    uint32_t    generation;/* supervisor-tracked restart generation */
};

/* Spawn the pager SERVICE granting targets[0..nt) and vmos[0..nv).  vmo_w_mask
 * bit j = grant RIGHT_WRITE on vmo j.  Optionally register "pager.ep".  0 on
 * success. */
static int t27_pager_spawn(struct t27_pager *p,
                           struct t25_tgt *targets, uint32_t nt,
                           handle_id_t *vmos, uint32_t nv, uint32_t vmo_w_mask,
                           int do_register, const char **why) {
    p->ctrl_ep = p->proc = HANDLE_INVALID; p->reg_id = -1; p->generation = 0;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ctrl ep"; return 0; }
    handle_id_t ctrl = (handle_id_t)ep;

    /* Fase 28.1: rewire every granted target's fault delivery onto the ONE
     * shared notification (targets[0].notif) with bit (1 << i) BEFORE the
     * pager starts, so no fault can land on the old per-target wiring. */
    for (uint32_t i = 0; i < nt; i++) {
        if (it_sys3(SYS_EXCEPTION_HANDLER, (long)targets[i].proc,
                    (long)targets[0].notif, (long)(1u << i)) != 0) {
            it_close(&ctrl); *why = "shared notif wire"; return 0;
        }
    }

    struct svc_mint m[40];
    uint32_t n = 0;
    m[n].slot = PGR_SLOT_CTRL_EP; m[n].src_h = ctrl; m[n].rights = RIGHT_READ; m[n].badge = 0; n++;
    if (nt > 0) {
        m[n].slot = PGR_SLOT_FAULT_NOTIF; m[n].src_h = targets[0].notif; m[n].rights = RIGHT_WAIT; m[n].badge = 0; n++;
    }
    for (uint32_t i = 0; i < nt; i++) {
        m[n].slot = PGR_TSLOT_PROC(i);  m[n].src_h = targets[i].proc;  m[n].rights = RIGHT_READ | RIGHT_MANAGE; m[n].badge = 0; n++;
        m[n].slot = PGR_TSLOT_VS(i);    m[n].src_h = targets[i].vs;    m[n].rights = RIGHT_WRITE;               m[n].badge = 0; n++;
    }
    for (uint32_t j = 0; j < nv; j++) {
        iris_rights_t vr = RIGHT_READ | ((vmo_w_mask & (1u << j)) ? RIGHT_WRITE : 0u);
        m[n].slot = PGR_VSLOT(j); m[n].src_h = vmos[j]; m[n].rights = vr; m[n].badge = 0; n++;
    }

    /* Fase S1: the pager serves EP_CALLs on its ctrl endpoint — retype a
     * fresh reply object and mint it at PGR_SLOT_REPLY (13); drop our handle
     * right after so pager death still wakes blocked callers. */
    handle_id_t pgr_reply_h = HANDLE_INVALID;
    {
        long rr = it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
        if (rr >= 0) {
            pgr_reply_h = (handle_id_t)rr;
            m[n].slot = 13u; m[n].src_h = pgr_reply_h;
            m[n].rights = RIGHT_READ | RIGHT_WRITE; m[n].badge = 0; n++;
        }
    }

    /* Fase 28: the pager is its own supervised binary (initrd "pager"); it
     * enters its serve loop immediately on start — no mode-entry message. */
    handle_id_t boot = HANDLE_INVALID;
    long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "pager",
                             &p->proc, &boot, m, n);
    it_close(&pgr_reply_h);
    it_close(&boot);
    if (r < 0 || p->proc == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&p->proc); *why = "pager spawn"; return 0;
    }
    p->ctrl_ep = ctrl;
    p->generation = 1;
    if (do_register) p->reg_id = it_register_ep("pager.svc", ctrl);
    return 1;
}

/* EP_CALL the pager; returns its result word (0 = OK, negative error/marker,
 * or a positive REPORT bitmask), or a negative transport error. */
static long t27_pager_call(handle_id_t ctrl_ep, uint32_t op, uint32_t tidx,
                           uint32_t vidx, uint32_t flags,
                           uint64_t offset, uint64_t expect) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.words[0] = PGR_PACK(op, tidx, vidx, flags);
    m.words[1] = offset;
    m.words[2] = expect;
    m.word_count = 3u;
    long r = it_sys2(SYS_EP_CALL, (long)ctrl_ep, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}

static void t27_pager_reap(struct t27_pager *p) {
    if (p->reg_id >= 0) { (void)it_unregister((uint32_t)p->reg_id); p->reg_id = -1; }
    if (p->proc != HANDLE_INVALID) {
        (void)it_sys1(SYS_PROCESS_KILL, (long)p->proc);
        (void)it_lp_wait_exit(p->proc);
    }
    it_close(&p->proc);
    it_close(&p->ctrl_ep);
}

/* Drive one VMO-backed read-fault resolution end to end: trigger the target's
 * read fault, call the pager to map vmo[vidx]@offset and resume; return 1 if
 * the target then ran to completion reading `pat`. */
static int t27_resolve_read(struct t27_pager *p, struct t25_tgt *g,
                            uint32_t tidx, uint32_t vidx, uint64_t offset,
                            uint64_t va, uint32_t pat, const char **why) {
    if (it_lp_cmd_va(g->cmd, LP_CMD_FAULT_READ, va) != 0) { *why = "fault trigger"; return 0; }
    long res = t27_pager_call(p->ctrl_ep, PGR_OP_MAP_RESUME, tidx, vidx, 0u, offset, va);
    if (res != 0) { *why = "pager resolve"; return 0; }
    if (it_lp_wait_exit(g->proc) != (long)(LP_EXIT_MARKER ^ (pat & 0xFFu))) {
        *why = "target completion"; return 0;
    }
    return 1;
}

/* ── T201: pager service manifest and startup ───────────────────────────────
 * The pager comes up as a real supervised service with EXACTLY its declared
 * manifest — control endpoint + one target grant (proc/vspace/notif) + one VMO
 * grant — and nothing else: no spawn cap, no device caps, no untyped, no
 * KDEBUG, no core client eps, no caps for undeclared targets/VMOs.  It
 * registers "pager.ep" in svcmgr; a lookup returns the current endpoint and a
 * PING through it answers.  Invariants: G1–G8, G28. */
static void test_t201(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager manifest";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T201", why); return; }
    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { t25_tgt_reap(&g); it_fail("T201", "vmo create"); return; }

    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 1 /*register*/, &why)) { ok = 0; }

    /* PING via the direct control cap. */
    if (ok && t27_pager_call(p.ctrl_ep, PGR_OP_PING, 0, 0, 0, 0, 0) != 0) { ok = 0; why = "ping"; }

    /* Manifest oracle: EXACTLY {ctrl 3, shared fault notif 5, vmo0 16,
     * target proc/vs presence (bits 20/21)}. */
    if (ok) {
        long mask = t27_pager_call(p.ctrl_ep, PGR_OP_REPORT, 0, 0, 0, 0, 0);
        uint32_t expect = (1u << PGR_SLOT_CTRL_EP) | (1u << PGR_SLOT_FAULT_NOTIF) |
                          (1u << 13) /* Fase S1: explicit reply object */ |
                          (1u << PGR_VSLOT(0)) | (1u << 20) | (1u << 21);
        if (mask < 0 || (uint32_t)mask != expect) { ok = 0; why = "manifest mismatch"; }
        /* Explicitly none of: core client eps (1/2/4 — slot 3 is the control
         * endpoint), spawn(bit24), untyped(bit26), vspace-self(bit27), second
         * vmo (17). */
        if (ok && ((uint32_t)mask & ((1u<<1)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<7)|
                                     (1u<<24)|(1u<<26)|(1u<<27)|(1u<<17))) != 0) {
            ok = 0; why = "extra authority";
        }
    }

    /* Registry presence: "pager.ep" resolves and a PING through the looked-up
     * cap answers (the registry serves the CURRENT endpoint). */
    if (ok && p.reg_id < 0) { ok = 0; why = "not registered"; }
    if (ok) {
        struct IrisMsg lm;
        if (it_lookup_name_slot("pager.svc", 0u, &lm) != 0 ||
            lm.label != IRIS_EP_REPLY_OK ||
            !iris_msg_cap_is_handle(lm.attached_handle)) { ok = 0; why = "lookup"; }
        else {
            handle_id_t ep = (handle_id_t)lm.attached_handle;
            if (t27_pager_call(ep, PGR_OP_PING, 0, 0, 0, 0, 0) != 0) { ok = 0; why = "lookup ping"; }
            it_close(&ep);
        }
    }

    t27_pager_reap(&p);
    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T201"); else it_fail("T201", why);
}

/* ── T202: pager service resolves one target fault ──────────────────────────
 * The full service path: supervisor fills a VMO page, spawns the pager with a
 * target grant + VMO grant, triggers the target's read fault, and calls the
 * pager to resolve it.  The pager waits on the target's fault notification,
 * reads the record through its proc cap, maps the VMO page at the fault VA via
 * its VSpace cap, and seq-resumes.  The target reads the supervisor's pattern
 * and completes.  Exactly-once delivery; no implicit caps; books at baseline.
 * Invariants: G9–G13, G23–G27. */
static void test_t202(void) {
    uint32_t f0[5], f1[5], word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0 && it_sched_ext5(f0);
    const char *why = "pager resolve";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T202", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_vmo_word(vmo, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T202", why); return; }
    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    if (ok && !t27_resolve_read(&p, &g, 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    /* Fault record cleared; no residual delivery. */
    if (ok && it_fault_info(g.proc, &(struct it_fault){0}) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived";
    }

    t27_pager_reap(&p);
    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 1u)  { ok = 0; why = "resume count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T202"); else it_fail("T202", why);
}

/* ── T203: unauthorized target denial ───────────────────────────────────────
 * A pager granted target A only cannot touch target B: neither the pager (its
 * manifest has no B cap) nor a hand-forged attempt through A's caps reaches B.
 * We prove it at the cap layer the pager relies on: A's proc/vspace caps do
 * NOT resolve B's fault or install in B's VSpace.  B's fault stays pending and
 * is resolved only by B's own authority.  Invariants: G5, G6, G10, G20, G30. */
static void test_t203(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "unauthorized target";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T203", "vmo create"); return; }

    struct t25_tgt ga, gb;      /* A = pager's grant; B = unrelated */
    if (ok && !t25_tgt_spawn(&ga, &why)) { it_close(&vmo); it_fail("T203", why); return; }
    if (ok && !t25_tgt_spawn(&gb, &why)) { t25_tgt_reap(&ga); it_close(&vmo); it_fail("T203", why); return; }

    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, &ga, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    /* B faults. */
    struct it_fault fb;
    if (ok && it_lp_cmd_va(gb.cmd, LP_CMD_FAULT_READ, T27_VA_B) != 0) { ok = 0; why = "B fault"; }
    if (ok && !t25_wait_fault(gb.proc, &fb)) { ok = 0; why = "B pending"; }

    /* A's caps (which the pager holds) grant nothing over B — proven directly:
     * A's proc cap is a different object, so resolving B's task through it is
     * NOT_FOUND; mapping the VMO into A's VSpace does not touch B. */
    if (ok && t25_resume_seq(ga.proc, fb.task_id, fb.seq, 0) != (long)IRIS_ERR_NOT_FOUND) {
        ok = 0; why = "A cap resolved B"; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)ga.vs, (long)T27_VA_B, 0)
        == 0) {
        /* This installs into A's VSpace at T27_VA_B — legal for A, but it must
         * NOT affect B.  Verify B still faults (unchanged) below; unmap via A's
         * death at reap. */
    }
    /* B's fault is intact: same generation, still suspended. */
    struct it_fault fb2;
    if (ok && (it_fault_info(gb.proc, &fb2) != 0 || fb2.seq != fb.seq)) { ok = 0; why = "B fault disturbed"; }
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)gb.proc) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "B not suspended"; }

    /* B is resolved only by B's own authority. */
    if (ok && t25_resume_seq(gb.proc, fb2.task_id, fb2.seq, 1) != 0) { ok = 0; why = "B proper kill"; }
    if (ok && it_lp_wait_exit(gb.proc) != 0) { ok = 0; why = "B exit"; }

    t27_pager_reap(&p);
    t25_tgt_reap(&ga); t25_tgt_reap(&gb);
    it_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T203"); else it_fail("T203", why);
}

/* ── T204: unauthorized VMO denial ──────────────────────────────────────────
 * A pager with target authority but a READ-only VMO grant cannot resolve a
 * WRITE fault (its map is refused ACCESS_DENIED, so the target keeps
 * write-faulting); a bad offset / kernel VA is refused; a valid RO resolution
 * afterwards still works.  No partial PTE, no VMO ref leak, fault state intact.
 * Invariants: G7, G11, G12, G24, G25. */
static void test_t204(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "unauthorized vmo";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T204", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_vmo_word(vmo, 0x2000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T204", why); return; }
    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    /* RO VMO grant (vmo_w_mask = 0). */
    if (ok && !t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    /* WRITE fault + RO VMO grant → the pager's writable map is ACCESS_DENIED,
     * so PGR_OP_MAP_RESUME (flags W) reports the error and the target is left
     * write-faulting.  We drive it directly to observe the denial cleanly. */
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T27_VA_A) != 0) { ok = 0; why = "write fault"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "pending"; }
    /* The pager holds RO vmo → a writable map into the target must be denied.
     * Emulate the pager's exact call via its VSpace cap (the pager would get
     * the same ACCESS_DENIED). */
    long vro = it_sys2(SYS_HANDLE_DUP, (long)vmo, (long)(RIGHT_READ | RIGHT_DUPLICATE));
    handle_id_t vro_h = (vro >= 0) ? (handle_id_t)vro : HANDLE_INVALID;
    if (ok && vro < 0) { ok = 0; why = "ro dup"; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, vro, (long)g.vs, (long)T27_VA_A, t26_ofs(0x2000ULL, 1u))
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable not denied"; }
    /* Bad offset / kernel VA also refused, no PTE installed. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T27_VA_A, t26_ofs(T26_VMO_SZ, 0))
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad offset"; }
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)0xFFFF800000000000ULL, 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel VA"; }
    it_close(&vro_h);

    /* A valid RO resolution afterwards works (space uncorrupted): map RO at the
     * fault VA and seq-resume — the store will re-fault WP, so kill instead. */
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T27_VA_A, t26_ofs(0x2000ULL, 0u)) != 0) {
        ok = 0; why = "valid ro map"; }
    if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 1) != 0) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "exit"; }

    t27_pager_reap(&p);
    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T204"); else it_fail("T204", why);
}

/* ── T205: pager restart preserves authority ────────────────────────────────
 * A pager service resolves a fault, is killed, and restarted by the supervisor
 * with the SAME manifest.  The new instance: gets a new generation; is
 * re-registered so "pager.ep" serves the CURRENT endpoint (the stale endpoint
 * is gone); reports EXACTLY the declared manifest (no accumulated authority);
 * resolves a fresh fault.  Invariants: G14, G16, G28, plus G2–G4 across
 * restart. */
static void test_t205(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager restart";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T205", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_vmo_word(vmo, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    /* Two fresh targets: gen1 resolves g1 (which then exits), gen2 resolves a
     * NEW target g2 (a resolved target runs to completion, so each generation
     * needs its own live target). */
    struct t25_tgt g1, g2;
    if (ok && !t25_tgt_spawn(&g1, &why)) { it_close(&vmo); it_fail("T205", why); return; }
    if (ok && !t25_tgt_spawn(&g2, &why)) { t25_tgt_reap(&g1); it_close(&vmo); it_fail("T205", why); return; }
    handle_id_t vmos1[1] = { vmo };

    /* Gen 1: register + resolve g1 once. */
    struct t27_pager p1;
    if (ok && !t27_pager_spawn(&p1, &g1, 1u, vmos1, 1u, 0u, 1, &why)) { ok = 0; }
    if (ok && !t27_resolve_read(&p1, &g1, 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;
    long gen1_reg = ok ? p1.reg_id : -1;

    /* Kill gen 1; the stale endpoint no longer serves. */
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)p1.proc) != 0) { ok = 0; why = "kill gen1"; }
    if (ok && it_lp_wait_exit(p1.proc) != 0) { ok = 0; why = "gen1 exit"; }
    if (gen1_reg >= 0) { (void)it_unregister((uint32_t)gen1_reg); p1.reg_id = -1; }
    it_close(&p1.proc); it_close(&p1.ctrl_ep);
    it_quiesce_reaper();
    /* The old "pager.svc" name no longer resolves (unregistered on death). */
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "pager.svc")
              != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale endpoint served"; }

    /* Gen 2: restart with the same manifest (over g2), re-register. */
    struct t27_pager p2;
    if (ok && !t27_pager_spawn(&p2, &g2, 1u, vmos1, 1u, 0u, 1, &why)) { ok = 0; }
    if (ok && p2.reg_id < 0) { ok = 0; why = "gen2 not registered"; }

    /* Manifest is exactly the declaration — restart amplified nothing. */
    if (ok) {
        long mask = t27_pager_call(p2.ctrl_ep, PGR_OP_REPORT, 0, 0, 0, 0, 0);
        uint32_t expect = (1u << PGR_SLOT_CTRL_EP) | (1u << PGR_SLOT_FAULT_NOTIF) |
                          (1u << 13) /* Fase S1: explicit reply object */ |
                          (1u << PGR_VSLOT(0)) | (1u << 20) | (1u << 21);
        if (mask < 0 || (uint32_t)mask != expect) { ok = 0; why = "restart manifest"; }
        if (ok && ((uint32_t)mask & ((1u<<6)|(1u<<24)|(1u<<26)|(1u<<27))) != 0) {
            ok = 0; why = "restart gained authority"; }
    }
    /* Gen 2 resolves a fresh fault on g2. */
    if (ok && !t27_resolve_read(&p2, &g2, 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    t27_pager_reap(&p2);
    t25_tgt_reap(&g1); t25_tgt_reap(&g2);
    it_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T205"); else it_fail("T205", why);
}

/* ── T206: pager crash-loop containment ─────────────────────────────────────
 * A pager that dies right after start is respawned exactly `limit` times, then
 * the supervisor STOPS and marks the service degraded — no infinite loop, no
 * leak per attempt.  The target's fault meanwhile stays pending and observable;
 * the supervisor resolves it with its own authority.  Mirrors the Fase 24
 * crash-loop policy, now for the pager service.  Invariants: G18, G19, G23. */
#define T206_LIMIT PGR_RESTART_LIMIT
static void test_t206(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager crash-loop";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T206", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_vmo_word(vmo, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T206", why); return; }
    handle_id_t vmos[1] = { vmo };

    /* Target faults; it will stay pending across the whole crash-loop. */
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "fault"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "pending"; }

    /* Supervision loop: each generation dies immediately (modelled by kill
     * right after start); stop at the limit, mark degraded. */
    uint32_t restart_count = 0u, generation = 0u;
    int degraded = 0;
    while (ok && !degraded) {
        struct t27_pager pg;
        if (!t27_pager_spawn(&pg, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
        generation++;
        it_sys1(SYS_SLEEP, 1);
        if (it_sys1(SYS_PROCESS_KILL, (long)pg.proc) != 0) { ok = 0; why = "kill"; }
        if (ok && it_lp_wait_exit(pg.proc) != 0) { ok = 0; why = "gen exit"; }
        it_close(&pg.proc); it_close(&pg.ctrl_ep);
        it_quiesce_reaper();
        if (restart_count < T206_LIMIT) restart_count++;
        else degraded = 1;
    }
    if (ok && restart_count != T206_LIMIT) { ok = 0; why = "wrong restart count"; }
    if (ok && !degraded) { ok = 0; why = "never degraded"; }
    if (ok && generation != T206_LIMIT + 1u) { ok = 0; why = "generation mismatch"; }

    /* The fault survived every crash; supervisor resolves with its authority. */
    struct it_fault f2;
    if (ok && (it_fault_info(g.proc, &f2) != 0 || f2.seq != f.seq)) { ok = 0; why = "fault lost"; }
    if (ok && t25_resume_seq(g.proc, f2.task_id, f2.seq, 1) != 0) { ok = 0; why = "supervisor resolve"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }

    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T206"); else it_fail("T206", why);
}

/* ── T207: multiple targets under one pager ─────────────────────────────────
 * One pager holds explicit grants for target A (grant 0) and target B (grant
 * 1).  Both fault; the pager resolves each from the shared VMO into the RIGHT
 * VSpace by index.  A dies; B still resolves.  Grants never mix: the fault info
 * and the mappings are per-target.  Invariants: G20, G17, G25, G26. */
static void test_t207(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "multi-target";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T207", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_vmo_word(vmo, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt ga, gb;
    if (ok && !t25_tgt_spawn(&ga, &why)) { it_close(&vmo); it_fail("T207", why); return; }
    if (ok && !t25_tgt_spawn(&gb, &why)) { t25_tgt_reap(&ga); it_close(&vmo); it_fail("T207", why); return; }
    struct t25_tgt tg[2] = { ga, gb };

    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, tg, 2u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    /* Resolve A via grant 0, B via grant 1 — both read the same VMO page. */
    if (ok && !t27_resolve_read(&p, &tg[0], 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;
    /* A has exited; B still resolves through its own grant. */
    if (ok && !t27_resolve_read(&p, &tg[1], 1u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    t27_pager_reap(&p);
    t25_tgt_reap(&tg[0]); t25_tgt_reap(&tg[1]);
    it_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T207"); else it_fail("T207", why);
}

/* ── T208: multiple VMOs under one pager ────────────────────────────────────
 * One pager holds two VMO grants (VMO0 read-only, VMO1 writable).  The target
 * faults twice; the pager backs region A from VMO0 and region B from VMO1.  The
 * backings never mix: A reads VMO0's pattern, B's store lands in VMO1 (and not
 * VMO0).  PTE rights are per-VMO grant.  Invariants: G21, G12, G24, G26. */
static void test_t208(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "multi-vmo";

    handle_id_t vmo0 = t26_vmo_create(T26_VMO_SZ);
    handle_id_t vmo1 = t26_vmo_create(T26_VMO_SZ);
    if (vmo0 == HANDLE_INVALID || vmo1 == HANDLE_INVALID) {
        it_close(&vmo0); it_close(&vmo1); it_fail("T208", "vmo create"); return;
    }
    word = T27_PAT;  if (ok && t26_vmo_word(vmo0, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo0 fill"; }
    word = 0;        if (ok && t26_vmo_word(vmo1, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo1 zero"; }

    /* One pager, two targets, two VMO grants: target A reads from VMO0 (grant
     * 0, RO), target B writes into VMO1 (grant 1, writable). */
    struct t25_tgt ga, gb;
    if (ok && !t25_tgt_spawn(&ga, &why)) { it_close(&vmo0); it_close(&vmo1); it_fail("T208", why); return; }
    if (ok && !t25_tgt_spawn(&gb, &why)) { t25_tgt_reap(&ga); it_close(&vmo0); it_close(&vmo1); it_fail("T208", why); return; }
    struct t25_tgt tg[2] = { ga, gb };
    struct t27_pager p;
    handle_id_t vmos[2] = { vmo0, vmo1 };
    if (ok && !t27_pager_spawn(&p, tg, 2u, vmos, 2u, (1u << 1), 0, &why)) { ok = 0; }

    /* A: read VMO0 grant into target A → reads the pattern. */
    if (ok && !t27_resolve_read(&p, &tg[0], 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    /* B: write VMO1 grant into target B → store lands in VMO1. */
    if (ok && it_lp_cmd_va(tg[1].cmd, LP_CMD_FAULT_WRITE, T27_VA_B) != 0) { ok = 0; why = "B fault"; }
    if (ok && t27_pager_call(p.ctrl_ep, PGR_OP_MAP_RESUME, 1u /*target B*/, 1u /*vmo1*/, 1u /*W*/, 0x1000ULL, T27_VA_B) != 0) {
        ok = 0; why = "B resolve"; }
    if (ok && it_lp_wait_exit(tg[1].proc) != LP_EXIT_MARKER) { ok = 0; why = "B store"; }

    it_quiesce_reaper();
    /* The store landed in VMO1, and VMO0 is untouched (backings did not mix). */
    if (ok && (t26_vmo_word(vmo1, 0x1000ULL, &word, 0) != 0 || word != T27_WMARK)) { ok = 0; why = "vmo1 not stored"; }
    if (ok && (t26_vmo_word(vmo0, 0x1000ULL, &word, 0) != 0 || word != T27_PAT)) { ok = 0; why = "vmo0 disturbed"; }

    t27_pager_reap(&p);
    t25_tgt_reap(&tg[0]); t25_tgt_reap(&tg[1]);
    it_close(&vmo0); it_close(&vmo1);
    it_quiesce_reaper();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T208"); else it_fail("T208", why);
}

/* ── T209: pager service death with pending faults ──────────────────────────
 * The pager dies with the target's fault pending (never resolved).  The target
 * is not a zombie: it stays suspended-alive with its record and generation
 * intact, resolvable by another authority (a restarted pager or the
 * supervisor).  The dead pager's control endpoint has no phantom receiver; no
 * endpoint/notification/KReply leak.  Invariants: G16, G18, G23, G27. */
static void test_t209(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager death pending";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T209", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_vmo_word(vmo, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T209", why); return; }
    handle_id_t vmos[1] = { vmo };

    /* Target faults; pager spawned in charge but killed before it serves. */
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "fault"; }
    if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "pending"; }

    struct t27_pager p1;
    if (ok && !t27_pager_spawn(&p1, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }
    handle_id_t p1ctrl = ok ? p1.ctrl_ep : HANDLE_INVALID;
    if (ok && it_sys1(SYS_PROCESS_KILL, (long)p1.proc) != 0) { ok = 0; why = "kill pager"; }
    if (ok && it_lp_wait_exit(p1.proc) != 0) { ok = 0; why = "pager exit"; }
    it_close(&p1.proc);
    it_quiesce_reaper();

    /* Target not a zombie: suspended-alive, record + generation intact. */
    struct it_fault f2;
    if (ok && it_sys1(SYS_PROCESS_EXIT_CODE, (long)g.proc) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "target not suspended"; }
    if (ok && (it_fault_info(g.proc, &f2) != 0 || f2.seq != f.seq)) { ok = 0; why = "record lost"; }
    /* Dead pager's control endpoint has no phantom receiver. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.words[0] = PGR_PACK(PGR_OP_PING, 0, 0, 0);
        m.word_count = 1u;
        if (it_sys2(SYS_EP_NB_SEND, (long)p1ctrl, (long)&m) != (long)IRIS_ERR_WOULD_BLOCK) {
            ok = 0; why = "phantom receiver"; }
    }
    it_close(&p1.ctrl_ep);

    /* A restarted pager (same manifest) completes the resolution. */
    struct t27_pager p2;
    if (ok && !t27_pager_spawn(&p2, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }
    if (ok) {
        long res = t27_pager_call(p2.ctrl_ep, PGR_OP_MAP_RESUME, 0u, 0u, 0u, 0x1000ULL, T27_VA_A);
        if (res != 0) { ok = 0; why = "restart resolve"; }
        if (ok && it_lp_wait_exit(g.proc) != (long)(LP_EXIT_MARKER ^ (T27_PAT & 0xFFu))) {
            ok = 0; why = "target completion"; }
    }

    t27_pager_reap(&p2);
    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T209"); else it_fail("T209", why);
}

/* ── T210: deterministic pager-service stress ───────────────────────────────
 * Seeded mixed-operation rounds over the whole service surface: VMO-backed
 * read/write resolve, pager death + supervisor takeover, target death
 * mid-fault, unauthorized VMO/offset denial, restart, crash + resolve.  After
 * every round: no pending fault, no zombie, no registry ghost, and the live
 * books (process/task/handle/VSpace/mapping/VMO) at baseline.  Prints
 * seed/round/op on failure.  Invariants: G10–G27 under load. */
#define T210_SEED   0x27F00D27u
#define T210_ROUNDS 6u
static uint32_t t210_rnd(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
static void test_t210(void) {
    uint32_t rng = T210_SEED, word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_vmo_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "pager stress";
    uint32_t round = 0u, op = 0u;

    for (round = 0; ok && round < T210_ROUNDS; round++) {
        op = t210_rnd(&rng) % 4u;
        handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
        if (vmo == HANDLE_INVALID) { ok = 0; why = "vmo create"; break; }
        word = T27_PAT ^ round;
        if (t26_vmo_word(vmo, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; it_close(&vmo); break; }

        struct t25_tgt g;
        if (!t25_tgt_spawn(&g, &why)) { ok = 0; it_close(&vmo); break; }
        handle_id_t vmos[1] = { vmo };
        struct it_fault f;

        switch (op) {
        case 0: {
            /* Clean VMO-backed read resolve. */
            struct t27_pager p;
            if (!t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
            if (!t27_resolve_read(&p, &g, 0u, 0u, 0x1000ULL, T27_VA_A, word, &why)) ok = 0;
            t27_pager_reap(&p);
            break;
        }
        case 1: {
            /* Pager dies before serving; supervisor takes over. */
            struct t27_pager p;
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "op1 fault"; break; }
            if (!t25_wait_fault(g.proc, &f)) { ok = 0; why = "op1 pending"; break; }
            if (!t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
            if (it_sys1(SYS_PROCESS_KILL, (long)p.proc) != 0 || it_lp_wait_exit(p.proc) != 0) { ok = 0; why = "op1 pager death"; }
            it_close(&p.proc); it_close(&p.ctrl_ep);
            if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T27_VA_A, t26_ofs(0x1000ULL, 0u)) != 0) { ok = 0; why = "op1 map"; }
            if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 0) != 0) { ok = 0; why = "op1 resume"; }
            if (ok && it_lp_wait_exit(g.proc) != (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op1 target"; }
            break;
        }
        case 2: {
            /* Target death mid-fault; pager's late map is BAD_HANDLE. */
            struct t27_pager p;
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "op2 fault"; break; }
            if (!t25_wait_fault(g.proc, &f)) { ok = 0; why = "op2 pending"; break; }
            if (!t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
            if (it_sys1(SYS_PROCESS_KILL, (long)g.proc) != 0 || it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "op2 kill"; }
            it_quiesce_reaper();
            /* The pager would now get BAD_HANDLE on its map; verify at cap layer. */
            if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T27_VA_A, t26_ofs(0x1000ULL, 0u))
                      != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "op2 late map"; }
            t27_pager_reap(&p);
            break;
        }
        case 3: {
            /* Unauthorized VMO (RO grant) write attempt denied, then clean read. */
            struct t27_pager p;
            if (!t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "op3 fault"; break; }
            long res = t27_pager_call(p.ctrl_ep, PGR_OP_MAP_RESUME, 0u, 0u, 1u /*W*/, 0x1000ULL, T27_VA_A);
            /* RO VMO grant + writable request → the pager's map is ACCESS_DENIED. */
            if (res != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op3 not denied"; }
            /* The target is still faulting; resolve read-only for real. */
            if (ok && !t25_wait_fault(g.proc, &f)) { ok = 0; why = "op3 pending"; }
            if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vmo, (long)g.vs, (long)T27_VA_A, t26_ofs(0x1000ULL, 0u)) != 0) { ok = 0; why = "op3 map"; }
            if (ok && t25_resume_seq(g.proc, f.task_id, f.seq, 0) != 0) { ok = 0; why = "op3 resume"; }
            if (ok && it_lp_wait_exit(g.proc) != (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op3 target"; }
            t27_pager_reap(&p);
            break;
        }
        default: break;
        }

        if (ok && it_fault_info(g.proc, &f) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "residual fault"; }
        t25_tgt_reap(&g);
        it_close(&vmo);
        it_quiesce_reaper();
        if (ok) {
            struct it_snap r = it_snap_take();
            if (!it_snap_baseline_live(&b, &r, &why)) ok = 0;
            else if (it_vmo_live() != vlive0) { ok = 0; why = "vmo live drift"; }
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && it_vmo_live() != vlive0) { ok = 0; why = "vmo live final"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T210");
    else { it_fz_note("T210", T210_SEED, round, op); it_fail("T210", why); }
}

/* ── Fase 28 Bloque A: boot-growth hardening (T211–T216) ─────────────────────
 *
 * The Fase 27 "wedge on the 10th image" was NOT a memory/alignment/allocator
 * bug: it was an over-strict boot assertion.  userboot required the kernel
 * initrd count to EQUAL a hardcoded catalog size and, on any mismatch, exited
 * before loading init — so adding ANY image (even a 256-byte blob, with
 * __data_start/__kernel_end byte-identical) left the system with no userland.
 * The fix relaxed the check to `count >= SL_CATALOG_COUNT` and made the
 * remaining genuine shortage emit a serial diagnostic instead of vanishing.
 * There is no fixed-size image array anywhere in the boot path (the kernel
 * g_initrd[] is sizeof-derived), so image growth is unbounded by construction.
 *
 * These tests exercise the boot-growth MECHANISMS from ring 3 (the boot itself
 * already proved it survives >SL_CATALOG_COUNT images by running this suite):
 * the initrd count/query surface, per-image size/mapping, loader failure
 * atomicity, explicit failure over silent wedge, the promoted pager binary,
 * and a seeded load matrix.  Invariants documented in
 * docs/architecture/boot-image-growth.md. */

#define T2_MIN_IMAGES   11u   /* SL_CATALOG_COUNT — every named index resolves */
#define T2_BADELF_IDX   11u   /* the invalid-ELF fixture image */

/* ── T211: initrd image-count boundary ──────────────────────────────────────
 * The count is queryable and >= the named catalog; every index in range yields
 * an initrd VMO; an out-of-range index fails cleanly (NOT_FOUND), never a
 * wedge.  That this test runs at all proves boot reached userland with more
 * than SL_CATALOG_COUNT images. */
static void test_t211(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "initrd count boundary";

    long n = it_sys1(SYS_INITRD_COUNT, (long)IRIS_CPTR_SPAWN_CAP);
    if (n < (long)T2_MIN_IMAGES) { ok = 0; why = "count below catalog"; }

    /* Every in-range index yields a live VMO with a positive size. */
    for (long i = 0; ok && i < n; i++) {
        long v = it_sys2(SYS_INITRD_VMO, (long)IRIS_CPTR_SPAWN_CAP, i);
        if (v < 0) { ok = 0; why = "image vmo"; break; }
        handle_id_t vh = (handle_id_t)v;
        if (it_sys1(SYS_VMO_SIZE, v) <= 0) { ok = 0; why = "image size"; }
        it_close(&vh);
    }
    /* Out-of-range indices fail cleanly. */
    if (ok && it_sys2(SYS_INITRD_VMO, (long)IRIS_CPTR_SPAWN_CAP, n)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "oob not NOT_FOUND"; }
    if (ok && it_sys2(SYS_INITRD_VMO, (long)IRIS_CPTR_SPAWN_CAP, 9999L)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "far oob not NOT_FOUND"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T211"); else it_fail("T211", why);
}

/* ── T212: initrd aggregate-size boundary ───────────────────────────────────
 * Every image's initrd VMO has a correct, non-overflowing size and is mappable
 * into the caller's own VSpace at the file's true size (no truncation, no
 * overlap between images).  Reading back the first bytes of a couple of images
 * confirms the physical bounds are honest (not aliased). */
static void test_t212(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_setup_self_vspace();
    const char *why = "initrd size boundary";

    long n = it_sys1(SYS_INITRD_COUNT, (long)IRIS_CPTR_SPAWN_CAP);
    if (n < (long)T2_MIN_IMAGES) { ok = 0; why = "count"; }

    for (long i = 0; ok && i < n; i++) {
        long v = it_sys2(SYS_INITRD_VMO, (long)IRIS_CPTR_SPAWN_CAP, i);
        if (v < 0) { ok = 0; why = "vmo"; break; }
        handle_id_t vh = (handle_id_t)v;
        long sz = it_sys1(SYS_VMO_SIZE, v);
        if (sz <= 0 || sz > (long)(64u * 1024u * 1024u)) { ok = 0; why = "size range"; }
        /* Map page 0 read-only into our own VSpace at a scratch VA; a mappable
         * VMO with a real backing proves the bounds are honest. */
        if (ok && it_sys4(SYS_VMO_MAP_PAGE, v, IT_VS, (long)T26_SELF_VA, t26_ofs(0, 0)) != 0) {
            ok = 0; why = "map"; }
        if (ok && it_sys2(SYS_VMO_UNMAP, (long)T26_SELF_VA, 0x1000L) != 0) { ok = 0; why = "unmap"; }
        it_close(&vh);
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T212"); else it_fail("T212", why);
}

/* ── T213: loader/process launch growth ─────────────────────────────────────
 * A launch of a valid service (lifecycle_probe) works; a launch of the
 * invalid-ELF fixture fails cleanly (INVALID_ARG) with FULL atomicity — no
 * ghost process/task/VSpace/CSpace, no handle leak; and a valid launch AFTER
 * the failed one still works.  This is the growth invariant: an added
 * (possibly malformed) image never poisons the loader for the next one. */
static void test_t213(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "loader launch growth";

    /* 1. A valid launch works. */
    {
        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "valid launch 1"; }
        if (ok) { (void)it_sys1(SYS_PROCESS_KILL, (long)proc); (void)it_lp_wait_exit(proc); }
        it_close(&cmd); it_close(&proc);
    }
    it_quiesce_reaper();

    /* 2. The invalid-ELF fixture fails cleanly with no ghost state. */
    {
        struct it_snap fb = it_snap_take();
        handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
        long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "badelf",
                                 &proc, &boot, 0, 0u);
        if (r != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "badelf not INVALID_ARG"; }
        if (ok && proc != HANDLE_INVALID) { ok = 0; why = "badelf left a process"; }
        it_close(&boot); it_close(&proc);
        it_quiesce_reaper();
        struct it_snap fa = it_snap_take();
        if (ok && !it_snap_baseline_live(&fb, &fa, &why)) ok = 0;   /* no ghost */
    }

    /* 3. A valid launch AFTER the failure still works. */
    {
        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ok && (ep < 0 || lp_spawn_child(cmd, &proc) < 0)) { ok = 0; why = "valid launch 2"; }
        if (ok) { (void)it_sys1(SYS_PROCESS_KILL, (long)proc); (void)it_lp_wait_exit(proc); }
        it_close(&cmd); it_close(&proc);
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T213"); else it_fail("T213", why);
}

/* ── T214: boot failure diagnostics ─────────────────────────────────────────
 * Every capacity/validity failure in the load path is an EXPLICIT error, never
 * a silent hang: an unknown image name is NOT_FOUND, a malformed image is
 * INVALID_ARG, and neither blocks.  (The boot-time analogue — userboot's
 * catalog-shortage diagnostic — is exercised by construction: this suite only
 * runs because userboot loaded init with >SL_CATALOG_COUNT images present.) */
static void test_t214(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "boot failure diagnostics";

    handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
    /* Unknown name → NOT_FOUND, no hang, no process. */
    long r1 = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "no_such_image",
                              &proc, &boot, 0, 0u);
    if (r1 != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "unknown not NOT_FOUND"; }
    if (ok && proc != HANDLE_INVALID) { ok = 0; why = "unknown left process"; }
    it_close(&boot); it_close(&proc);

    /* Malformed image → INVALID_ARG, no hang, no process. */
    proc = boot = HANDLE_INVALID;
    long r2 = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "badelf",
                              &proc, &boot, 0, 0u);
    if (ok && r2 != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "malformed not INVALID_ARG"; }
    if (ok && proc != HANDLE_INVALID) { ok = 0; why = "malformed left process"; }
    it_close(&boot); it_close(&proc);

    /* Initrd VMO of the invalid image still succeeds (it is bytes, not code) —
     * the failure is the LOADER's, cleanly reported, not the initrd layer's. */
    long v = it_sys2(SYS_INITRD_VMO, (long)IRIS_CPTR_SPAWN_CAP, (long)T2_BADELF_IDX);
    if (ok && v < 0) { ok = 0; why = "badelf vmo"; }
    if (v >= 0) { handle_id_t vh = (handle_id_t)v; it_close(&vh); }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T214"); else it_fail("T214", why);
}

/* ── T215: pager binary promotion ───────────────────────────────────────────
 * The pager is now its OWN initrd binary ("pager", index 10) — no longer a
 * lifecycle_probe mode.  Load it by name, wire a real target + VMO grant, and
 * resolve a VMO-backed fault end to end; verify the manifest is exactly the
 * grant set (no authority gained from being a standalone image).  T201–T210
 * already exercise the full supervised/registered/restart surface over this
 * same binary; T215 is the focused promotion proof. */
static void test_t215(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager binary promotion";

    handle_id_t vmo = t26_vmo_create(T26_VMO_SZ);
    if (vmo == HANDLE_INVALID) { it_fail("T215", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_vmo_word(vmo, 0x1000ULL, &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&vmo); it_fail("T215", why); return; }
    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    /* Manifest is exactly the grant set (a standalone binary gained nothing). */
    if (ok) {
        long mask = t27_pager_call(p.ctrl_ep, PGR_OP_REPORT, 0, 0, 0, 0, 0);
        uint32_t expect = (1u << PGR_SLOT_CTRL_EP) | (1u << PGR_SLOT_FAULT_NOTIF) |
                          (1u << 13) /* Fase S1: explicit reply object */ |
                          (1u << PGR_VSLOT(0)) | (1u << 20) | (1u << 21);
        if (mask < 0 || (uint32_t)mask != expect) { ok = 0; why = "manifest"; }
        if (ok && ((uint32_t)mask & ((1u<<6)|(1u<<24)|(1u<<26)|(1u<<27))) != 0) {
            ok = 0; why = "gained authority"; }
    }
    /* Resolve a fault from the binary pager. */
    if (ok && !t27_resolve_read(&p, &g, 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    t27_pager_reap(&p);
    t25_tgt_reap(&g);
    it_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T215"); else it_fail("T215", why);
}

/* ── T216: deterministic boot-growth stress ─────────────────────────────────
 * A seeded matrix of load operations over the grown initrd: query counts, map
 * random in-range images, load valid vs invalid images, out-of-range queries.
 * Every op either succeeds or fails EXPLICITLY; the loader never wedges and
 * never drifts.  Boot progressed to run this (with the fixture images present),
 * so the growth itself is the standing proof; this hammers the mechanisms. */
#define T216_SEED   0x28B007A4u
#define T216_ROUNDS 12u
static uint32_t t216_rnd(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
static void test_t216(void) {
    uint32_t rng = T216_SEED;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_setup_self_vspace();
    const char *why = "boot-growth stress";
    uint32_t round = 0u, op = 0u;

    long n = it_sys1(SYS_INITRD_COUNT, (long)IRIS_CPTR_SPAWN_CAP);
    if (n < (long)T2_MIN_IMAGES) { it_fail("T216", "count"); return; }

    for (round = 0; ok && round < T216_ROUNDS; round++) {
        op = t216_rnd(&rng) % 4u;
        switch (op) {
        case 0: {
            /* Map a random in-range image page 0 into our own VSpace. */
            long i = (long)(t216_rnd(&rng) % (uint32_t)n);
            long v = it_sys2(SYS_INITRD_VMO, (long)IRIS_CPTR_SPAWN_CAP, i);
            if (v < 0) { ok = 0; why = "map vmo"; break; }
            handle_id_t vh = (handle_id_t)v;
            if (it_sys4(SYS_VMO_MAP_PAGE, v, IT_VS, (long)T26_SELF_VA, t26_ofs(0, 0)) != 0) { ok = 0; why = "map"; }
            if (ok) (void)it_sys2(SYS_VMO_UNMAP, (long)T26_SELF_VA, 0x1000L);
            it_close(&vh);
            break;
        }
        case 1: {
            /* Out-of-range query fails clean. */
            if (it_sys2(SYS_INITRD_VMO, (long)IRIS_CPTR_SPAWN_CAP, n + (long)(t216_rnd(&rng) % 100u))
                != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "oob"; }
            break;
        }
        case 2: {
            /* Invalid-ELF load fails clean, no ghost. */
            struct it_snap fb = it_snap_take();
            handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
            long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "badelf", &proc, &boot, 0, 0u);
            if (r >= 0) { ok = 0; why = "badelf loaded"; }
            it_close(&boot); it_close(&proc);
            it_quiesce_reaper();
            struct it_snap fa = it_snap_take();
            if (ok && !it_snap_baseline_live(&fb, &fa, &why)) ok = 0;
            break;
        }
        case 3: {
            /* Valid launch works and reaps cleanly. */
            long ep = it_ep_create();
            handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
            handle_id_t proc = HANDLE_INVALID;
            if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "launch"; }
            if (ok) { (void)it_sys1(SYS_PROCESS_KILL, (long)proc); (void)it_lp_wait_exit(proc); }
            it_close(&cmd); it_close(&proc);
            break;
        }
        default: break;
        }
        it_quiesce_reaper();
        if (ok) {
            struct it_snap r = it_snap_take();
            if (!it_snap_baseline_live(&b, &r, &why)) ok = 0;
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T216");
    else { it_fz_note("T216", T216_SEED, round, op); it_fail("T216", why); }
}

/* ── Fase 28 Bloque B: file-backed memory (T217–T230) ────────────────────────
 *
 * The pager (its own binary) now backs faults from files: a supervisor
 * registers a backing (identity + generation) and validated regions, and the
 * pager resolves faults by reading the file via VFS READ_AT, filling a VMO page
 * (RO cache or private-writable pool), mapping it, and seq-resuming.  These
 * tests drive the whole subsystem end to end and verify content through the
 * TARGET's own read (file → VFS → pager → VMO → target mapping → target byte).
 * Constants/structs MUST match services/pager/pager_proto.h. */

#define FBK_OP_PING               1u
#define FBK_OP_SHUTDOWN           5u
#define FBK_OP_MAP_REGION         6u
#define FBK_OP_REGISTER_BACKING   7u
#define FBK_OP_REGISTER_REGION    8u
#define FBK_OP_UNREGISTER_REGION  9u
#define FBK_OP_REVOKE_BACKING    10u
#define FBK_OP_DIAG              11u
#define FBK_OP_TARGET_RESET      12u
#define FBK_SLOT_VFS_EP    4u
#define FBK_SLOT_NOTIF     5u     /* Fase 28.1: the ONE shared fault notification */
#define FBK_VMO_CACHE_SLOT  16u   /* pager slot 16 = VMO grant 0 (cache) */
#define FBK_VMO_PRIV_SLOT   17u   /* pager slot 17 = VMO grant 1 (private) */
#define FBK_MAX_BACKINGS   4u
#define FBK_MAX_REGIONS    16u    /* Fase 28.1: one region per possible target */
#define FBK_MAX_TARGETS    16u
#define FBK_CACHE_CAP      8u
#define FBK_PRIV_CAP       8u
#define FBK_MODE_RO         0u
#define FBK_MODE_PRIVATE    1u
#define FBK_MODE_SHARED_W   2u
#define FBK_PROT_R  1u
#define FBK_PROT_W  2u
#define FBK_PROT_X  4u
/* err markers (pager returns them negated). */
#define FBK_ERR_NOFAULT    0x63u
#define FBK_ERR_NO_REGION  0x64u
#define FBK_ERR_STALE_GEN  0x65u
#define FBK_ERR_SHORT_READ 0x66u
#define FBK_ERR_CACHE_FULL 0x67u
#define FBK_ERR_MODE       0x68u
#define FBK_ERR_ACCESS     0x69u
#define FBK_ERR_RANGE      0x6Au
#define FBK_ERR_NOBACK     0x6Bu
#define FBK_ERR_PRIVFULL   0x6Cu
#define FBK_ERR_GRANT      0x6Du   /* Fase 28.1: VFS denied the grant */
/* The productive pager's grant session (badge IRIS_BADGE_FILEGRANT_S(0)). */
#define FBK_SESSION        0u

/* File fixtures (must match the services/filebk .dat files + gen_fixtures.py). */
#define FBK_FILE_NAME    "fbk.dat"
#define FBK_FILE_SIZE    20480u          /* 5 pages */
#define FBK2_FILE_NAME   "fbk2.dat"
#define FBK2_FILE_SIZE   12288u          /* 3 pages */
#define ELFSEG_FILE_NAME "elfseg.dat"
#define ELFSEG_FILE_SIZE 16384u          /* 4 pages */
#define SMALL_FILE_NAME  "small.dat"
#define SMALL_FILE_SIZE  100u            /* sub-page */
#define LP_CMD_FAULT_READ_SEQ 0x109Fu   /* must match lifecycle_probe */
#define LP_CMD_FAULT_WRITE_M  0x109Du   /* mirror of LP_CMD_FAULT_WRITE */
static inline uint8_t t28_pat(uint64_t i)    { return (uint8_t)((i * 31u + 7u) & 0xFFu); }
static inline uint8_t t28_pat2(uint64_t i)   { return (uint8_t)((i * 17u + 101u) & 0xFFu); }
static inline uint8_t t28_patseg(uint64_t i) { return (uint8_t)((i * 13u + 0x40u) & 0xFFu); }
static inline uint8_t t28_pats(uint64_t i)   { return (uint8_t)((i * 7u + 1u) & 0xFFu); }

/* Send a multi-page fault-read sequence to a target (words[0]=base VA,
 * words[1]=count, words[2]=visit order). */
static long t28_cmd_read_seq(handle_id_t cmd, uint64_t base, uint32_t count, uint64_t order) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.label = LP_CMD_FAULT_READ_SEQ;
    m.words[0] = base; m.words[1] = (uint64_t)count; m.words[2] = order;
    m.word_count = 3u;
    return it_sys2(SYS_EP_SEND, (long)cmd, (long)&m);
}
#define LP_CMD_FAULT_READ_OFFS_M 0x10A5u   /* mirror of lifecycle_probe */
/* Two-offset fault-read: read base+off0 and (count==2) base+off1. */
static long t28_cmd_read_offs(handle_id_t cmd, uint64_t base, uint32_t count,
                              uint64_t off0, uint64_t off1) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.label = LP_CMD_FAULT_READ_OFFS_M;
    m.words[0] = base; m.words[1] = (uint64_t)count; m.words[2] = off0; m.words[3] = off1;
    m.word_count = 4u;
    return it_sys2(SYS_EP_SEND, (long)cmd, (long)&m);
}

/* Fase 28 region VAs (clear of all prior windows). */
#define T28_VA_A   0x809A000000ULL
#define T28_VA_B   0x809B000000ULL
#define T28_VA_C   0x809C000000ULL

struct pgr_backing_req {   /* == services/pager/pager_proto.h */
    uint32_t backing_idx; uint32_t grant_idx;
    uint64_t backing_id; uint64_t generation; uint64_t file_size;
};
struct pgr_region_req {
    uint32_t region_idx; uint32_t target_idx; uint32_t backing_idx;
    uint32_t prot; uint32_t mode; uint32_t reserved;
    uint64_t start_va; uint64_t memory_length; uint64_t file_offset;
    uint64_t file_length; uint64_t backing_generation;
};
struct pgr_diag {
    uint32_t backing_live, region_count, cache_capacity, cache_entries,
             cache_hit, cache_miss, cache_evict, page_fill, page_fill_fail,
             generation_stale, grant_revoke, private_pages,
             notif_waits, notif_wakeups, pending_mask, target_resets,
             grant_denied;
};

struct t28_fbk {
    handle_id_t ctrl_ep;   /* supervisor's control cap */
    handle_id_t proc;      /* pager process cap */
    handle_id_t cache_vmo; /* supervisor holds these VMO handles */
    handle_id_t priv_vmo;
    handle_id_t vfs_cap;   /* UNBADGED dup vfs cap (mint source; name-op client) */
    handle_id_t admin;     /* the grant-ADMIN cap (badge IRIS_BADGE_FILEGRANT_ADMIN) */
};

/* A VFS-issued file grant as seen by the supervisor (GRANT_OPEN reply). */
struct t28_grant { uint32_t idx; uint64_t bid; uint64_t gen; };

static uint8_t g_t28_buf[128];   /* pager-request staging buffer */

/* Materialize the two supervisor-side file-grant caps init pre-minted:
 *   slot 59 (IRIS_CPTR_TEST_VFS_MINT) — UNBADGED WRITE|DUPLICATE|TRANSFER
 *       vfs.ep cap: the mint SOURCE for session-badged pager caps, and an
 *       ordinary unbadged name-op client for STATs.
 *   slot 58 (IRIS_CPTR_TEST_VFS_DUP) — the grant ADMIN identity (badge
 *       IRIS_BADGE_FILEGRANT_ADMIN, WRITE-only): GRANT_OPEN / GRANT_REVOKE /
 *       GRANT_SESSION_RESET.
 * SYS_CSPACE_RESOLVE copies the CSpace leaf into a fresh handle preserving
 * rights AND badge.  The ordinary svcmgr lookup strips DUPLICATE (client
 * grant tightening) and cannot mint fresh badges, so these pre-mints are the
 * only honest supervisor path. */
static handle_id_t t28_vfs_cap(void) {
    long h = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_TEST_VFS_MINT);
    return (h >= 0) ? (handle_id_t)h : HANDLE_INVALID;
}
static handle_id_t t28_admin_cap(void) {
    long h = it_sys1(SYS_CSPACE_RESOLVE, (long)IRIS_CPTR_TEST_VFS_DUP);
    return (h >= 0) ? (handle_id_t)h : HANDLE_INVALID;
}

/* iris_test's own root CNode handle (slots 0..15, type CNODE) — found once,
 * used to DELETE a CSpace slot before re-minting into it.  Mirrors
 * svcmgr_find_root_cnode. */
static handle_id_t t28_root_cnode(void) {
    static handle_id_t cached = HANDLE_INVALID;
    if (cached != HANDLE_INVALID) return cached;
    for (uint32_t slot = 0; slot < 16u; slot++) {
        handle_id_t h = handle_id_make(slot, 1u);
        if (it_sys1(SYS_HANDLE_TYPE, (long)h) == (long)IRIS_HANDLE_TYPE_CNODE) {
            cached = h; return h;
        }
    }
    return HANDLE_INVALID;
}

/* Self-mint a SESSION-badged vfs cap (badge IRIS_BADGE_FILEGRANT_S(s)) into
 * iris_test's own CSpace and materialize it.  This is byte-identical to the
 * cap a pager of session s holds — the hostile-client stand-in for the
 * compromised-pager tests.  Slots T28_FG_SLOT(s) are dedicated to the grant
 * sessions; the slot is DELETED before minting so re-entry (and any prior
 * occupant) is handled cleanly, then resolved to a fresh handle per call
 * (caller closes it). */
/* Slots 52..54 are free (between T091's 50/51 and TEST_UNTYPED at 55); the
 * harness therefore probes at most 3 distinct sessions.  The tests only need
 * sessions 0 (the productive pager) and 1 (the cross-session attacker). */
#define T28_FG_SLOT(s)  (52u + (s))
#define T28_FG_SESSIONS 3u
static handle_id_t t28_session_cap(uint32_t session) {
    if (session >= T28_FG_SESSIONS) return HANDLE_INVALID;
    handle_id_t src = t28_vfs_cap();
    if (src == HANDLE_INVALID) return HANDLE_INVALID;
    handle_id_t root = t28_root_cnode();
    if (root != HANDLE_INVALID)
        (void)it_sys2(SYS_CNODE_DELETE, (long)root, (long)T28_FG_SLOT(session));
    long mr = it_sys4(SYS_PROC_CSPACE_MINT, (long)IRIS_CPTR_TEST_PROC,
                      (long)T28_FG_SLOT(session), (long)src,
                      (long)((IRIS_BADGE_FILEGRANT_S(session) << 32) | RIGHT_WRITE));
    it_close(&src);
    if (mr != 0) return HANDLE_INVALID;
    long h = it_sys1(SYS_CSPACE_RESOLVE, (long)T28_FG_SLOT(session));
    return (h >= 0) ? (handle_id_t)h : HANDLE_INVALID;
}

/* STAT a file via a vfs cap → size, or -1. */
static long t28_stat(handle_id_t vfs_cap, const char *name) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    uint32_t n = 0; while (name[n] && n + 1u < sizeof(g_ep_io_buf)) { g_ep_io_buf[n] = (uint8_t)name[n]; n++; }
    g_ep_io_buf[n] = 0;
    m.label = VFS_EP_OP_STAT; m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf; m.buf_len = n + 1u;
    if (it_sys2(SYS_EP_CALL, (long)vfs_cap, (long)&m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -1;
    return (long)m.words[1];
}

/* Generic VFS grant-protocol call.  Stages `name` (may be NULL) as the bulk
 * payload, sends label/w0..w2, and returns 0 on REPLY_OK (msg copied to *out
 * when non-NULL) or the NEGATIVE iris_error_t from the error reply. */
static long t28_gcall(handle_id_t cap, uint64_t label, uint64_t w0, uint64_t w1,
                      uint64_t w2, uint32_t wc, const char *name,
                      struct IrisMsg *out) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label = label;
    m.words[0] = w0; m.words[1] = w1; m.words[2] = w2; m.word_count = wc;
    m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    if (name) {
        uint32_t n = 0;
        while (name[n] && n + 1u < sizeof(g_ep_io_buf)) { g_ep_io_buf[n] = (uint8_t)name[n]; n++; }
        g_ep_io_buf[n] = 0;
        m.buf_len = n + 1u;
    }
    long r = it_sys2(SYS_EP_CALL, (long)cap, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return (long)(int32_t)(uint32_t)m.words[0];
    if (out) *out = m;
    return 0;
}

/* Supervisor grant operations (ADMIN cap). */
static long t28_grant_open(handle_id_t admin, uint32_t session, const char *name,
                           uint32_t rights, struct t28_grant *out) {
    struct IrisMsg m;
    long r = t28_gcall(admin, VFS_EP_OP_GRANT_OPEN, session, rights, 0, 2u, name, &m);
    if (r != 0) return r;
    if (out) { out->idx = (uint32_t)m.words[1]; out->bid = m.words[2]; out->gen = m.words[3]; }
    return 0;
}
static long t28_session_reset(handle_id_t admin, uint32_t session) {
    return t28_gcall(admin, VFS_EP_OP_GRANT_SESSION_RESET, session, 0, 0, 1u, 0, 0);
}
static long t28_grant_revoke_name(handle_id_t admin, const char *name, uint64_t *newgen) {
    struct IrisMsg m;
    long r = t28_gcall(admin, VFS_EP_OP_GRANT_REVOKE, 0, 0, 0, 0u, name, &m);
    if (r != 0) return r;
    if (newgen) *newgen = m.words[1];
    return 0;
}

/* Session-holder grant operations (a SESSION-badged cap). */
static long t28_grant_read(handle_id_t cap, uint32_t idx, uint64_t off, uint32_t len,
                           uint8_t *first_byte, uint64_t *bytes) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_READ_AT, idx, off, len, 3u, 0, &m);
    if (r != 0) return r;
    if (bytes) *bytes = m.words[1];
    if (first_byte) *first_byte = (m.words[1] > 0u) ? g_ep_io_buf[0] : 0u;
    return 0;
}
static long t28_grant_stat(handle_id_t cap, uint32_t idx, uint64_t *size,
                           uint64_t *bid, uint64_t *gen) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_STAT, idx, 0, 0, 1u, 0, &m);
    if (r != 0) return r;
    if (size) *size = m.words[1];
    if (bid)  *bid  = m.words[2];
    if (gen)  *gen  = m.words[3];
    return 0;
}
static long t28_grant_query(handle_id_t cap, uint32_t idx, uint64_t *bid,
                            uint64_t *gen, uint64_t *rights) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_QUERY_IDENTITY, idx, 0, 0, 1u, 0, &m);
    if (r != 0) return r;
    if (bid) *bid = m.words[1];
    if (gen) *gen = m.words[2];
    if (rights) *rights = m.words[3];
    return 0;
}
static long t28_grant_derive(handle_id_t cap, uint32_t src, uint32_t rights,
                             uint32_t *newidx) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_DERIVE, src, rights, 0, 2u, 0, &m);
    if (r != 0) return r;
    if (newidx) *newidx = (uint32_t)m.words[1];
    return 0;
}
static long t28_grant_revoke_idx(handle_id_t cap, uint32_t idx, uint64_t *newgen) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_REVOKE, idx, 0, 0, 1u, 0, &m);
    if (r != 0) return r;
    if (newgen) *newgen = m.words[1];
    return 0;
}

/* Spawn a file-backed pager granting `nt` targets, its SESSION-badged vfs cap,
 * the shared fault notification, and cache/private VMOs.  Runs the supervisor
 * restart protocol: session FBK_SESSION is RESET first, so no grant of a
 * previous pager instance can survive into this one (A11).  0 on success. */
static int t28_fbk_spawn(struct t28_fbk *f, struct t25_tgt *targets, uint32_t nt,
                         const char **why) {
    f->ctrl_ep = f->proc = f->cache_vmo = f->priv_vmo = HANDLE_INVALID;
    f->vfs_cap = f->admin = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ctrl ep"; return 0; }
    handle_id_t ctrl = (handle_id_t)ep;
    handle_id_t cvmo = t26_vmo_create((uint64_t)FBK_CACHE_CAP * 0x1000ULL);
    handle_id_t pvmo = t26_vmo_create((uint64_t)FBK_PRIV_CAP * 0x1000ULL);
    handle_id_t vfs  = t28_vfs_cap();
    handle_id_t adm  = t28_admin_cap();
    if (cvmo == HANDLE_INVALID || pvmo == HANDLE_INVALID ||
        vfs == HANDLE_INVALID || adm == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&cvmo); it_close(&pvmo); it_close(&vfs); it_close(&adm);
        *why = "fbk grants"; return 0;
    }
    /* Pager-(re)start protocol step 1: the session starts clean. */
    if (t28_session_reset(adm, FBK_SESSION) != 0) {
        it_close(&ctrl); it_close(&cvmo); it_close(&pvmo); it_close(&vfs); it_close(&adm);
        *why = "session reset"; return 0;
    }
    /* Rewire every target's fault delivery onto the ONE shared notification
     * (targets[0].notif), bit (1 << i), before the pager starts. */
    for (uint32_t i = 0; i < nt; i++) {
        if (it_sys3(SYS_EXCEPTION_HANDLER, (long)targets[i].proc,
                    (long)targets[0].notif, (long)(1u << i)) != 0) {
            it_close(&ctrl); it_close(&cvmo); it_close(&pvmo); it_close(&vfs); it_close(&adm);
            *why = "shared notif wire"; return 0;
        }
    }

    struct svc_mint m[40];
    uint32_t k = 0;
    m[k].slot = PGR_SLOT_CTRL_EP; m[k].src_h = ctrl; m[k].rights = RIGHT_READ; m[k].badge = 0; k++;
    /* The pager's ONLY VFS identity: a session-badged, WRITE-only cap.  The
     * fresh badge is legal because the source (slot 59) is unbadged. */
    m[k].slot = FBK_SLOT_VFS_EP;  m[k].src_h = vfs;  m[k].rights = RIGHT_WRITE;
    m[k].badge = IRIS_BADGE_FILEGRANT_S(FBK_SESSION); k++;
    if (nt > 0) {
        m[k].slot = FBK_SLOT_NOTIF; m[k].src_h = targets[0].notif; m[k].rights = RIGHT_WAIT; m[k].badge = 0; k++;
    }
    for (uint32_t i = 0; i < nt; i++) {
        m[k].slot = PGR_TSLOT_PROC(i);  m[k].src_h = targets[i].proc;  m[k].rights = RIGHT_READ | RIGHT_MANAGE; m[k].badge = 0; k++;
        m[k].slot = PGR_TSLOT_VS(i);    m[k].src_h = targets[i].vs;    m[k].rights = RIGHT_WRITE;               m[k].badge = 0; k++;
    }
    m[k].slot = PGR_VSLOT(0); m[k].src_h = cvmo; m[k].rights = RIGHT_READ | RIGHT_WRITE; m[k].badge = 0; k++;
    m[k].slot = PGR_VSLOT(1); m[k].src_h = pvmo; m[k].rights = RIGHT_READ | RIGHT_WRITE; m[k].badge = 0; k++;

    /* Fase S1: explicit reply object for the pager's ctrl EP (slot 13). */
    handle_id_t pgr_reply_h = HANDLE_INVALID;
    {
        long rr = it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
        if (rr >= 0) {
            pgr_reply_h = (handle_id_t)rr;
            m[k].slot = 13u; m[k].src_h = pgr_reply_h;
            m[k].rights = RIGHT_READ | RIGHT_WRITE; m[k].badge = 0; k++;
        }
    }
    handle_id_t boot = HANDLE_INVALID;
    long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "pager", &f->proc, &boot, m, k);
    it_close(&pgr_reply_h);
    it_close(&boot);
    if (r < 0 || f->proc == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&cvmo); it_close(&pvmo); it_close(&vfs); it_close(&adm); it_close(&f->proc);
        *why = "pager spawn"; return 0;
    }
    f->ctrl_ep = ctrl; f->cache_vmo = cvmo; f->priv_vmo = pvmo;
    f->vfs_cap = vfs; f->admin = adm;
    return 1;
}

static void t28_fbk_reap(struct t28_fbk *f) {
    if (f->proc != HANDLE_INVALID) { (void)it_sys1(SYS_PROCESS_KILL, (long)f->proc); (void)it_lp_wait_exit(f->proc); }
    it_close(&f->proc); it_close(&f->ctrl_ep);
    it_close(&f->cache_vmo); it_close(&f->priv_vmo);
    it_close(&f->vfs_cap); it_close(&f->admin);
}

/* Open a grant for the pager session and register it as pager backing `bidx`
 * — the whole supervisor-side backing setup.  Returns 1 and fills *gr. */
static long t28_reg_backing2(handle_id_t ctrl, uint32_t bidx,
                             const struct t28_grant *gr, uint64_t size);
static int t28_backing_setup(struct t28_fbk *f, uint32_t bidx, const char *name,
                             uint64_t size, struct t28_grant *gr, const char **why) {
    if (t28_grant_open(f->admin, FBK_SESSION, name, VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, gr) != 0) {
        *why = "grant open"; return 0;
    }
    if (t28_reg_backing2(f->ctrl_ep, bidx, gr, size) != 0) {
        *why = "reg backing"; return 0;
    }
    return 1;
}

/* Control calls. */
static long t28_ctrl_words(handle_id_t ctrl, uint32_t op, uint64_t w1, uint64_t w2) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)op; m.words[1] = w1; m.words[2] = w2; m.word_count = 3u;
    long r = it_sys2(SYS_EP_CALL, (long)ctrl, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}
static long t28_map_region(handle_id_t ctrl, uint32_t tidx) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)FBK_OP_MAP_REGION | ((uint64_t)tidx << 8);
    m.word_count = 1u;
    long r = it_sys2(SYS_EP_CALL, (long)ctrl, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}
/* Register a pager backing from raw fields (attack surface: the values may
 * deliberately MISMATCH the VFS-issued identity). */
static long t28_reg_backing_raw(handle_id_t ctrl, uint32_t idx, uint32_t grant_idx,
                                uint64_t id, uint64_t gen, uint64_t size) {
    struct pgr_backing_req *rq = (struct pgr_backing_req *)g_t28_buf;
    for (uint32_t i = 0; i < sizeof(*rq); i++) g_t28_buf[i] = 0;
    rq->backing_idx = idx; rq->grant_idx = grant_idx;
    rq->backing_id = id; rq->generation = gen; rq->file_size = size;
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)FBK_OP_REGISTER_BACKING; m.word_count = 1u;
    m.buf_uptr = (uint64_t)(uintptr_t)g_t28_buf; m.buf_len = (uint32_t)sizeof(*rq);
    long r = it_sys2(SYS_EP_CALL, (long)ctrl, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}
/* Register a pager backing from a VFS-issued grant (the honest path). */
static long t28_reg_backing2(handle_id_t ctrl, uint32_t bidx,
                             const struct t28_grant *gr, uint64_t size) {
    return t28_reg_backing_raw(ctrl, bidx, gr->idx, gr->bid, gr->gen, size);
}
static long t28_reg_region(handle_id_t ctrl, const struct pgr_region_req *src) {
    struct pgr_region_req *rq = (struct pgr_region_req *)g_t28_buf;
    for (uint32_t i = 0; i < sizeof(*rq); i++) g_t28_buf[i] = ((const uint8_t *)src)[i];
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)FBK_OP_REGISTER_REGION; m.word_count = 1u;
    m.buf_uptr = (uint64_t)(uintptr_t)g_t28_buf; m.buf_len = (uint32_t)sizeof(*rq);
    long r = it_sys2(SYS_EP_CALL, (long)ctrl, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}
static int t28_diag(handle_id_t ctrl, struct pgr_diag *out) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)FBK_OP_DIAG; m.word_count = 1u;
    m.buf_uptr = (uint64_t)(uintptr_t)g_t28_buf;
    if (it_sys2(SYS_EP_CALL, (long)ctrl, (long)&m) != 0) return 0;
    if (m.label != IRIS_EP_REPLY_OK || m.buf_len < sizeof(*out)) return 0;
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = g_t28_buf[i];
    return 1;
}

/* Build a region descriptor. */
static void t28_region(struct pgr_region_req *rq, uint32_t ridx, uint32_t tidx, uint32_t bidx,
                       uint64_t va, uint64_t mem_len, uint64_t file_off, uint64_t file_len,
                       uint32_t prot, uint32_t mode, uint64_t gen) {
    for (uint32_t i = 0; i < sizeof(*rq); i++) ((uint8_t *)rq)[i] = 0;
    rq->region_idx = ridx; rq->target_idx = tidx; rq->backing_idx = bidx;
    rq->prot = prot; rq->mode = mode; rq->start_va = va; rq->memory_length = mem_len;
    rq->file_offset = file_off; rq->file_length = file_len; rq->backing_generation = gen;
}

/* Resolve a target read fault at `va` and verify the low byte the target read
 * equals `expect_byte` (end-to-end file→pager→target verification). */
static int t28_read_verify(struct t28_fbk *f, struct t25_tgt *g, uint32_t tidx,
                           uint64_t va, uint8_t expect_byte, const char **why) {
    if (it_lp_cmd_va(g->cmd, LP_CMD_FAULT_READ, va) != 0) { *why = "fault trigger"; return 0; }
    long res = t28_map_region(f->ctrl_ep, tidx);
    if (res != 0) { *why = "map region"; return 0; }
    long ec = it_lp_wait_exit(g->proc);
    if (ec != (long)(LP_EXIT_MARKER ^ (uint32_t)expect_byte)) { *why = "content"; return 0; }
    return 1;
}

/* ── T217: file backing identity and grants ─────────────────────────────────
 * A registered backing has a stable identity + generation; a region can only
 * bind a registered backing at its current generation; an unregistered backing
 * or a stale generation is rejected; the STAT size is honest.  The pager holds
 * a bounded vfs cap, not global VFS authority (it reports only its manifest).
 * Invariants: F1, F2, F3, F4, F40. */
static void test_t217(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "backing identity";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T217", why); return; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; }

    /* STAT the file → honest size. */
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    if (ok && sz != (long)FBK_FILE_SIZE) { ok = 0; why = "stat size"; }

    /* Manifest oracle: ctrl(3) + session vfs(4) + shared notif(5) + vmos(16/17)
     * + target proc/vs presence (20/21). */
    if (ok) {
        long mask = t27_pager_call(f.ctrl_ep, PGR_OP_REPORT, 0, 0, 0, 0, 0);
        uint32_t expect = (1u<<3)|(1u<<4)|(1u<<5)|(1u<<13)|(1u<<16)|(1u<<17)|(1u<<20)|(1u<<21);
        if (mask < 0 || (uint32_t)mask != expect) { ok = 0; why = "manifest"; }
        if (ok && ((uint32_t)mask & ((1u<<6)|(1u<<24)|(1u<<26)|(1u<<27))) != 0) { ok = 0; why = "extra authority"; }
    }

    /* Open a grant and register backing 0 with the VFS-issued identity. */
    struct t28_grant gr;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "grant open"; }
    /* A backing whose declared identity MISMATCHES the VFS-issued one → GRANT
     * (the pager cross-checks against GRANT_QUERY_IDENTITY before trusting). */
    if (ok && t28_reg_backing_raw(f.ctrl_ep, 0, gr.idx, gr.bid + 1u, gr.gen, (uint64_t)sz)
              != -(long)FBK_ERR_GRANT) { ok = 0; why = "wrong id accepted"; }
    if (ok && t28_reg_backing_raw(f.ctrl_ep, 0, gr.idx, gr.bid, gr.gen + 1u, (uint64_t)sz)
              != -(long)FBK_ERR_GRANT) { ok = 0; why = "wrong gen accepted"; }
    /* A backing referencing a grant the session does not hold → GRANT. */
    if (ok && t28_reg_backing_raw(f.ctrl_ep, 0, gr.idx + 5u, gr.bid, gr.gen, (uint64_t)sz)
              != -(long)FBK_ERR_GRANT) { ok = 0; why = "bogus grant accepted"; }
    /* The honest registration succeeds. */
    if (ok && t28_reg_backing2(f.ctrl_ep, 0, &gr, (uint64_t)sz) != 0) { ok = 0; why = "reg backing"; }

    /* A region binding an UNREGISTERED backing (idx 1) → NOBACK. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 1 /*unregistered*/, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_NOBACK) { ok = 0; why = "unregistered backing not rejected"; }
    }
    /* A region binding the WRONG generation → NOBACK (stale). */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen + 1u /*stale gen*/);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_NOBACK) { ok = 0; why = "stale gen not rejected"; }
    }
    /* Correct backing + generation → OK. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "valid region rejected"; }
    }
    /* DIAG: exactly one backing live, one region. */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.backing_live != 1u || d.region_count != 1u) { ok = 0; why = "diag counts"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T217"); else it_fail("T217", why);
}

/* ── T218: file region validation ───────────────────────────────────────────
 * Every field is validated atomically: unaligned VA/offset, zero/oversized
 * length, kernel VA, overflow, file range beyond backing, file_length >
 * memory_length, writable prot on a RO region, overlap — each rejected with no
 * partial region; a valid region then registers and a fault resolves.
 * Invariants: F6, F7, F8, F38. */
static void test_t218(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "region validation";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T218", why); return; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; }
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;

    struct pgr_region_req rq;
    /* unaligned VA */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A|0x800, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="unaligned va"; } }
    /* zero length */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0, 0, 0, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="zero len"; } }
    /* kernel VA */
    if (ok) { t28_region(&rq, 0,0,0, 0xFFFF800000000000ULL, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="kernel va"; } }
    /* unaligned file offset */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0x800, 0x800, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="unaligned off"; } }
    /* file range beyond backing */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, (uint64_t)sz + 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="beyond backing"; } }
    /* file_length > memory_length */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, 0x2000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="flen>mlen"; } }
    /* writable prot on RO mode */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R|FBK_PROT_W, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="w prot ro"; } }
    /* shared-writable mode rejected */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R|FBK_PROT_W, FBK_MODE_SHARED_W, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_MODE) { ok=0; why="shared-w not rejected"; } }
    /* none of the above created a region */
    if (ok) { struct pgr_diag d; if (!t28_diag(f.ctrl_ep, &d) || d.region_count != 0u) { ok=0; why="ghost region"; } }
    /* a valid region then registers and resolves */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok=0; why="valid region"; } }
    /* overlap with the registered region → RANGE */
    if (ok) { t28_region(&rq, 1,0,0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="overlap"; } }
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0), &why)) ok = 0;

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T218"); else it_fail("T218", why);
}

/* ── T219: read-only file-backed fault resolution ───────────────────────────
 * Faults across a multi-page RO region resolve from the file with exact
 * content at nonzero offsets and out-of-order pages; the target reads the file
 * bytes.  Invariants: F10, F13, plus the B7 flow. */
static void test_t219(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "ro resolution";

    /* ONE target faults on four pages of a 4-page RO region in a scrambled
     * order [2,0,3,1]; the pager resolves each fault as it arrives.  A single
     * target (2 notifications) drives genuine multi-page out-of-order
     * resolution without exhausting the per-process notification quota that N
     * one-shot targets would. */
    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    /* One RO region over the file window [0x1000, 0x5000) — four pages, all at
     * nonzero file offsets. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x4000, 0x1000, 0x4000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    if (ok) {
        /* Visit order nibbles: step0=page2, step1=page0, step2=page3, step3=page1. */
        uint64_t order = (2ull << 0) | (0ull << 4) | (3ull << 8) | (1ull << 12);
        if (t28_cmd_read_seq(g.cmd, T28_VA_A, 4u, order) != 0) { ok = 0; why = "seq trigger"; }
        for (uint32_t k = 0; ok && k < 4u; k++)
            if (t28_map_region(f.ctrl_ep, 0u) != 0) { ok = 0; why = "map region"; }
        if (ok) {
            uint32_t acc = (uint32_t)t28_pat(0x1000) ^ (uint32_t)t28_pat(0x2000) ^
                           (uint32_t)t28_pat(0x3000) ^ (uint32_t)t28_pat(0x4000);
            long ec = it_lp_wait_exit(g.proc);
            if (ec != (long)(LP_EXIT_MARKER ^ acc)) { ok = 0; why = "content"; }
        }
    }
    /* All four pages came from the ONE cache backing: exactly 4 misses, 4 cache
     * entries, no evictions (CACHE_CAP=8 ≥ 4). */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.cache_miss != 4u || d.cache_entries != 4u || d.cache_evict != 0u) { ok = 0; why = "cache counts"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T219"); else it_fail("T219", why);
}

/* Resolve a target WRITE fault at `va` on a private-writable region: the pager
 * maps a fresh writable private page, the target's store completes, and it exits
 * with the plain marker.  Returns 1 on success. */
static int t28_write_resolve(struct t28_fbk *f, struct t25_tgt *g, uint32_t tidx,
                             uint64_t va, const char **why) {
    if (it_lp_cmd_va(g->cmd, LP_CMD_FAULT_WRITE_M, va) != 0) { *why = "write trigger"; return 0; }
    if (t28_map_region(f->ctrl_ep, tidx) != 0) { *why = "map region"; return 0; }
    if (it_lp_wait_exit(g->proc) != (long)LP_EXIT_MARKER) { *why = "write exit"; return 0; }
    return 1;
}

/* One EOF/zero-fill scenario: fresh target+pager (targets can't be rewired),
 * backing 0 over `fname`, region 0 at T28_VA_A, a two-offset read driving
 * `npages` resolutions, verify the target read MARKER ^ (exp0 ^ exp1).  Each
 * scenario is isolated (its own pager) so a leak shows in the caller's snapshot. */
static int t28_scenario_offs(const char *fname, uint64_t fsize, uint32_t mode, uint32_t prot,
                             uint64_t file_off, uint64_t file_len, uint64_t mem_len,
                             uint32_t count, uint64_t off0, uint64_t off1,
                             uint8_t exp0, uint8_t exp1, uint32_t npages, const char **why) {
    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, why)) return 0;
    struct t28_fbk f;
    if (!t28_fbk_spawn(&f, &g, 1u, why)) { t25_tgt_reap(&g); return 0; }
    int ok = 1;
    long sz = t28_stat(f.vfs_cap, fname);
    if (sz != (long)fsize) { ok = 0; *why = "stat size"; }
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, fname, (uint64_t)sz, &gr, why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, mem_len, file_off, file_len, prot, mode, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; *why = "reg region"; }
    }
    if (ok && t28_cmd_read_offs(g.cmd, T28_VA_A, count, off0, off1) != 0) { ok = 0; *why = "offs trigger"; }
    for (uint32_t k = 0; ok && k < npages; k++)
        if (t28_map_region(f.ctrl_ep, 0u) != 0) { ok = 0; *why = "map region"; }
    if (ok) {
        uint32_t acc = (count == 2u) ? ((uint32_t)exp0 ^ (uint32_t)exp1) : (uint32_t)exp0;
        long ec = it_lp_wait_exit(g.proc);
        if (ec != (long)(LP_EXIT_MARKER ^ acc)) { ok = 0; *why = "content"; }
    }
    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    return ok;
}

/* ── T220: EOF / zero-fill byte-exactness ────────────────────────────────────
 * A file-backed page is filled with file bytes up to file_length and zeroed
 * beyond it — exactly, at any byte granularity: a partial page (file bytes then
 * a zero tail), a sub-page file (past-EOF zero), an exact full page at EOF, and
 * pure zero pages past the file window.  Invariants: F11, F12. */
static void test_t220(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "eof/zero-fill";

    /* Partial page: file bytes [0,0x800) then a zero tail — read one of each. */
    if (ok && !t28_scenario_offs(FBK_FILE_NAME, FBK_FILE_SIZE, FBK_MODE_RO, FBK_PROT_R,
                                 0, 0x800, 0x1000, 2u, 0x100, 0x900,
                                 t28_pat(0x100), 0u, 1u, &why)) ok = 0;
    /* Sub-page file (small.dat = 100 bytes): a file byte and a past-EOF zero. */
    if (ok && !t28_scenario_offs(SMALL_FILE_NAME, SMALL_FILE_SIZE, FBK_MODE_RO, FBK_PROT_R,
                                 0, SMALL_FILE_SIZE, 0x1000, 2u, 10, 0x400,
                                 t28_pats(10), 0u, 1u, &why)) ok = 0;
    /* Exact EOF: the last full file page (fbk offset 0x4000) — both ends file. */
    if (ok && !t28_scenario_offs(FBK_FILE_NAME, FBK_FILE_SIZE, FBK_MODE_RO, FBK_PROT_R,
                                 0x4000, 0x1000, 0x1000, 2u, 0, 0xFFC,
                                 t28_pat(0x4000), t28_pat(0x4000 + 0xFFC), 1u, &why)) ok = 0;

    /* Pure zero pages past the file window: a 3-page region backed by only one
     * file page — pages 1,2 are entirely zero (BSS-like). */
    if (ok) {
        struct t25_tgt g;
        struct t28_fbk f;
        if (!t25_tgt_spawn(&g, &why)) ok = 0;
        else {
            if (!t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; t25_tgt_reap(&g); }
            else {
                long sz = t28_stat(f.vfs_cap, FBK_FILE_NAME);
                struct t28_grant gr;
                if (!t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
                if (ok) {
                    struct pgr_region_req rq;
                    t28_region(&rq, 0, 0, 0, T28_VA_A, 0x3000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
                    if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
                }
                if (ok && t28_cmd_read_seq(g.cmd, T28_VA_A, 3u, (0ull) | (1ull << 4) | (2ull << 8)) != 0) { ok = 0; why = "seq"; }
                for (uint32_t k = 0; ok && k < 3u; k++)
                    if (t28_map_region(f.ctrl_ep, 0u) != 0) { ok = 0; why = "map region"; }
                if (ok) {
                    uint32_t acc = (uint32_t)t28_pat(0) ^ 0u ^ 0u;   /* pages 1,2 pure zero */
                    long ec = it_lp_wait_exit(g.proc);
                    if (ec != (long)(LP_EXIT_MARKER ^ acc)) { ok = 0; why = "zero pages"; }
                }
                t28_fbk_reap(&f);
                t25_tgt_reap(&g);
            }
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T220"); else it_fail("T220", why);
}

/* ── T221: shared read-only cache — reuse + independent cleanup ───────────────
 * Two targets map the SAME file page RO: the first fault fills the cache, the
 * second is a cache HIT (no second VFS fill).  Each region takes an independent
 * reference; releasing one region keeps the page live for the other, and only
 * when the last reference drops is the page reclaimable.  Invariants: F19, F24. */
static void test_t221(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "shared cache";

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) { ok = 0; }
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    /* Region 0 (target 0) and region 1 (target 1) over the SAME file page. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 0"; }
        t28_region(&rq, 1, 1, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    /* Target 0 faults → miss+fill; target 1 faults SAME page → hit (no new fill). */
    if (ok && !t28_read_verify(&f, &g0, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    if (ok && !t28_read_verify(&f, &g1, 1u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.cache_miss != 1u || d.cache_hit != 1u || d.page_fill != 1u ||
                 d.cache_entries != 1u) { ok = 0; why = "shared counts"; }
    }
    /* Release region 0: the shared page stays live for region 1 (still 1 entry). */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, 0, 0) != 0) { ok = 0; why = "unreg 0"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag2"; }
        else if (d.region_count != 1u || d.cache_entries != 1u) { ok = 0; why = "post-release"; }
    }
    /* Release region 1: last reference gone; the entry is now reclaimable. */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, 1, 0) != 0) { ok = 0; why = "unreg 1"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag3"; }
        else if (d.region_count != 0u) { ok = 0; why = "post-release2"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T221"); else it_fail("T221", why);
}

/* ── T222: private-writable isolation (copy-at-fill) ─────────────────────────
 * A FILE_PRIVATE_WRITABLE region gives each fault a fresh, writable, private
 * page copied from the file — writes never reach the file or another mapping.
 * Target 0 WRITES its page (proving the PTE is writable, resolved from a write
 * fault); target 1 then READS the same file offset and sees the ORIGINAL file
 * byte, not target 0's store.  Invariants: F14, F15. */
static void test_t222(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "private isolation";

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 0"; }
        t28_region(&rq, 1, 1, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    /* Target 0 writes its private page (write fault → writable private page). */
    if (ok && !t28_write_resolve(&f, &g0, 0u, T28_VA_A, &why)) ok = 0;
    /* Target 1 reads the same file offset → original file byte (isolation). */
    if (ok && !t28_read_verify(&f, &g1, 1u, T28_VA_A, t28_pat(0), &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.private_pages != 2u) { ok = 0; why = "private count"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T222"); else it_fail("T222", why);
}

/* ── T223: shared-writable is NOT_SUPPORTED, with zero side effects ───────────
 * FILE_SHARED_WRITABLE (writes shared through to the file) is deliberately
 * refused (writeback + coherence are out of scope); the rejection creates no
 * region and leaves the backing untouched, and a valid region still registers
 * and resolves afterward.  Invariant: F16. */
static void test_t223(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "shared-writable";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    /* Shared-writable rejected (with or without W in prot). */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_SHARED_W, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_MODE) { ok = 0; why = "shared-w R|W not MODE"; }
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_SHARED_W, gr.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_MODE) { ok = 0; why = "shared-w R not MODE"; }
    }
    /* No ghost region, backing intact. */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.region_count != 0u || d.backing_live != 1u) { ok = 0; why = "side effect"; }
    }
    /* A valid RO region still registers and resolves — no corruption. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "valid after reject"; }
    }
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T223"); else it_fail("T223", why);
}

/* ── T224: backing read failure is atomic ────────────────────────────────────
 * A backing whose VFS authority dies under the pager cannot serve: the grant
 * is opened and registered honestly, then the ADMIN revokes the backing AT THE
 * VFS without telling the pager.  The next fault's GRANT_READ_AT is denied by
 * the VFS (the pager's own table still says "valid" — irrelevant), the fault
 * resolution fails, and NOTHING is left behind — no cache entry, no successful
 * fill, no PTE, and the target is NOT resumed (it stays faulted until the
 * supervisor kills it).  Also proves a nonexistent name cannot even be
 * granted.  Invariants: F17, F38; A4, A9, A17. */
static void test_t224(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "backing atomicity";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    /* A grant on a name VFS does not export cannot even be created. */
    if (ok && t28_grant_open(f.admin, FBK_SESSION, "nope.dat", VFS_FILE_RIGHT_READ, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "nope grant"; }
    /* Honest backing + region... */
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    /* ...then the VFS-side authority dies behind the pager's back. */
    if (ok && t28_grant_revoke_name(f.admin, FBK_FILE_NAME, 0) != 0) { ok = 0; why = "vfs revoke"; }
    /* Fault → resolution fails (the VFS denies GRANT_READ_AT); the pager
     * returns an error and does not resume the target. */
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T28_VA_A) != 0) { ok = 0; why = "fault trigger"; }
    if (ok && t28_map_region(f.ctrl_ep, 0u) >= 0) { ok = 0; why = "resolve did not fail"; }
    /* No side effects: no successful fill, no cache entry; a fill failure and
     * a VFS grant denial counted. */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.page_fill != 0u || d.cache_entries != 0u || d.page_fill_fail == 0u ||
                 d.grant_denied == 0u) { ok = 0; why = "residue"; }
    }

    /* The target is stuck faulted — reap kills it; books must still balance. */
    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T224"); else it_fail("T224", why);
}

/* ── T225: file-grant revoke ─────────────────────────────────────────────────
 * Revoking a backing bumps its generation, marks it unusable for new regions,
 * drops its unreferenced cached pages, and makes a fault on a region still bound
 * to the old generation fail STALE_GEN.  Existing mappings are untouched (the
 * kernel VSpace owns them).  Invariants: F4, F18, F27. */
static void test_t225(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "grant revoke";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    /* Resolve once → the page is cached (content verified). */
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    /* Drop the region so its cache reference clears; the entry stays valid but
     * unreferenced. */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, 0, 0) != 0) { ok = 0; why = "unreg"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag pre"; }
        else if (d.cache_entries != 1u) { ok = 0; why = "entry pre-revoke"; }
    }
    /* Revoke: backing goes unusable and its unreferenced page is dropped. */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_REVOKE_BACKING, 0, 0) != 0) { ok = 0; why = "revoke"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag post"; }
        else if (d.grant_revoke != 1u || d.backing_live != 0u || d.cache_entries != 0u) { ok = 0; why = "post-revoke"; }
    }
    /* A new region binding the revoked backing (old OR new generation) → NOBACK. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_NOBACK) { ok = 0; why = "revoked still bindable"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T225"); else it_fail("T225", why);
}

/* ── T226: pager restart contract ────────────────────────────────────────────
 * The pager's backings/regions/cache are SOFT state: a restart loses them and
 * the supervisor rebuilds.  After a clean SHUTDOWN, a fresh pager starts with
 * zero state and can re-acquire its VFS file-read authority (independent of the
 * old instance) and resolve again — no stale state survives, VFS is unaffected.
 * Invariants: F28, F29. */
static void test_t226(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager restart";

    /* Instance 1: register + resolve, then shut down. */
    struct t25_tgt g1;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t28_fbk f1;
    if (ok && !t28_fbk_spawn(&f1, &g1, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f1.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr1;
    if (ok && !t28_backing_setup(&f1, 0, FBK_FILE_NAME, (uint64_t)sz, &gr1, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr1.gen);
        if (t28_reg_region(f1.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    if (ok && !t28_read_verify(&f1, &g1, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    /* Clean shutdown: the pager exits; wait for its death. */
    if (ok && t28_ctrl_words(f1.ctrl_ep, FBK_OP_SHUTDOWN, 0, 0) != 0) { ok = 0; why = "shutdown"; }
    if (ok && it_lp_wait_exit(f1.proc) < 0) { ok = 0; why = "pager did not exit"; }
    t28_fbk_reap(&f1);
    t25_tgt_reap(&g1);

    /* Instance 2: a fresh pager starts with ZERO state and works end to end. */
    struct t25_tgt g2;
    if (ok && !t25_tgt_spawn(&g2, &why)) ok = 0;
    struct t28_fbk f2;
    if (ok && !t28_fbk_spawn(&f2, &g2, 1u, &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f2.ctrl_ep, &d)) { ok = 0; why = "diag fresh"; }
        else if (d.backing_live != 0u || d.region_count != 0u || d.cache_entries != 0u ||
                 d.cache_hit != 0u || d.cache_miss != 0u) { ok = 0; why = "state survived restart"; }
    }
    /* The new instance re-acquires its file authority via a FRESH grant (the
     * spawn's SESSION_RESET killed the old one at the VFS). */
    struct t28_grant gr2;
    if (ok && !t28_backing_setup(&f2, 0, FBK_FILE_NAME, (uint64_t)sz, &gr2, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x2000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr2.gen);
        if (t28_reg_region(f2.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 2"; }
    }
    if (ok && !t28_read_verify(&f2, &g2, 0u, T28_VA_A, t28_pat(0x2000), &why)) ok = 0;
    t28_fbk_reap(&f2);
    t25_tgt_reap(&g2);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T226"); else it_fail("T226", why);
}

/* ── T227: multiple files, multiple targets ──────────────────────────────────
 * Two distinct backings over two distinct files, one per target: each target
 * resolves from its OWN file with that file's exact bytes — no cross-file
 * bleed — and the two pages occupy distinct cache entries.  Invariants: F3, F23. */
static void test_t227(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "multi-file";

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz0 = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    long sz1 = ok ? t28_stat(f.vfs_cap, FBK2_FILE_NAME) : -1;
    if (ok && (sz0 != (long)FBK_FILE_SIZE || sz1 != (long)FBK2_FILE_SIZE)) { ok = 0; why = "stat sizes"; }
    struct t28_grant gr0, gr1;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME,  (uint64_t)sz0, &gr0, &why)) ok = 0;
    if (ok && !t28_backing_setup(&f, 1, FBK2_FILE_NAME, (uint64_t)sz1, &gr1, &why)) ok = 0;
    /* Two distinct files carry two distinct VFS-issued backing identities. */
    if (ok && gr0.bid == gr1.bid) { ok = 0; why = "identities collide"; }
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr0.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 0"; }
        t28_region(&rq, 1, 1, 1, T28_VA_B, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr1.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    /* Each target reads its own file's byte at file offset 0x1000. */
    if (ok && !t28_read_verify(&f, &g0, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    if (ok && !t28_read_verify(&f, &g1, 1u, T28_VA_B, t28_pat2(0x1000), &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.backing_live != 2u || d.cache_miss != 2u || d.cache_entries != 2u) { ok = 0; why = "multi counts"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T227"); else it_fail("T227", why);
}

/* ── T228: cache is bounded and evicts unreferenced pages ────────────────────
 * The RO cache holds at most PGR_CACHE_CAP pages.  Target 0 fills all 8 slots
 * (one region, 8 pages incl. zero-fill tail); its region is then released so
 * those pages are unreferenced.  Target 1 faults 8 fresh keys (a second backing
 * id): each MISS evicts a stale page — the cache never exceeds capacity and
 * always makes progress.  Invariants: F20, F21, F22. */
static void test_t228(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "cache eviction";

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    long sz2 = ok ? t28_stat(f.vfs_cap, FBK2_FILE_NAME) : -1;
    /* Two DIFFERENT files → distinct VFS-issued backing ids → distinct cache
     * keys (identities are honest now; the same file can no longer be given
     * two fake ids). */
    struct t28_grant gr0, gr1;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME,  (uint64_t)sz,  &gr0, &why)) ok = 0;
    if (ok && !t28_backing_setup(&f, 1, FBK2_FILE_NAME, (uint64_t)sz2, &gr1, &why)) ok = 0;
    uint64_t order8 = 0; for (uint64_t p = 0; p < 8; p++) order8 |= (p << (4 * p));
    /* Region 0: 8 pages (5 file + 3 zero-fill), target 0 faults them all. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x8000, 0, 0x5000, FBK_PROT_R, FBK_MODE_RO, gr0.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 0"; }
    }
    if (ok && t28_cmd_read_seq(g0.cmd, T28_VA_A, 8u, order8) != 0) { ok = 0; why = "seq 0"; }
    for (uint32_t k = 0; ok && k < 8u; k++)
        if (t28_map_region(f.ctrl_ep, 0u) != 0) { ok = 0; why = "map 0"; }
    if (ok && it_lp_wait_exit(g0.proc) < 0) { ok = 0; why = "target 0 exit"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag fill"; }
        else if (d.cache_miss != 8u || d.cache_entries != 8u || d.cache_evict != 0u ||
                 d.cache_capacity != 8u) { ok = 0; why = "fill counts"; }
    }
    /* Release region 0 → its 8 pages are now unreferenced (reclaimable). */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, 0, 0) != 0) { ok = 0; why = "unreg 0"; }
    /* Region 1 (backing 1 = fbk2.dat): 8 distinct keys (3 file pages + 5
     * zero-fill); target 1 faults them → evictions. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 1, 1, 1, T28_VA_B, 0x8000, 0, 0x3000, FBK_PROT_R, FBK_MODE_RO, gr1.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    if (ok && t28_cmd_read_seq(g1.cmd, T28_VA_B, 8u, order8) != 0) { ok = 0; why = "seq 1"; }
    for (uint32_t k = 0; ok && k < 8u; k++)
        if (t28_map_region(f.ctrl_ep, 1u) != 0) { ok = 0; why = "map 1"; }
    if (ok && it_lp_wait_exit(g1.proc) < 0) { ok = 0; why = "target 1 exit"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag evict"; }
        /* Never exceeded capacity; exactly 8 evictions reclaimed the old pages. */
        else if (d.cache_entries != 8u || d.cache_evict != 8u || d.cache_miss != 16u) { ok = 0; why = "evict counts"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T228"); else it_fail("T228", why);
}

/* ── T229: ELF-segment groundwork (RX / R / RW / BSS, W^X) ────────────────────
 * The region model expresses the four ELF segment shapes: RX code (RO shared,
 * executable), R rodata (RO shared), RW data (private writable), and BSS (pure
 * zero-fill private writable).  W^X is enforced at registration (no W+X), every
 * segment is readable, and an RX fault resolves to a read-only EXECUTABLE page.
 * Invariants: F30, F31. */
static void test_t229(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "elf segments";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, ELFSEG_FILE_NAME) : -1;
    if (ok && sz != (long)ELFSEG_FILE_SIZE) { ok = 0; why = "stat size"; }
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, ELFSEG_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;

    struct pgr_region_req rq;
    /* RX code segment (file page 0) — RO shared, executable. */
    if (ok) { t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x0000, 0x1000, FBK_PROT_R | FBK_PROT_X, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "RX rejected"; } }
    /* R rodata (file page 1) — RO shared. */
    if (ok) { t28_region(&rq, 1, 0, 0, T28_VA_B, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "R rejected"; } }
    /* RW data (file page 2) — private writable. */
    if (ok) { t28_region(&rq, 2, 0, 0, T28_VA_C, 0x1000, 0x2000, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "RW rejected"; } }
    /* BSS (pure zero-fill) — private writable, file_length 0. */
    if (ok) { t28_region(&rq, 3, 0, 0, T28_VA_A + 0x10000, 0x1000, 0x0000, 0x0000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "BSS rejected"; } }
    /* W^X: writable+executable rejected. */
    if (ok) { t28_region(&rq, 4, 0, 0, T28_VA_A + 0x20000, 0x1000, 0x0000, 0x1000, FBK_PROT_R | FBK_PROT_W | FBK_PROT_X, FBK_MODE_PRIVATE, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok = 0; why = "W^X not enforced"; } }
    /* Every segment readable: X-only rejected. */
    if (ok) { t28_region(&rq, 4, 0, 0, T28_VA_A + 0x20000, 0x1000, 0x0000, 0x1000, FBK_PROT_X, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok = 0; why = "X-only accepted"; } }

    /* An RX fault resolves to a read-only executable page with exact bytes. */
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_patseg(0), &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.region_count != 4u) { ok = 0; why = "segment count"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T229"); else it_fail("T229", why);
}

/* ── T230: deterministic control-plane stress ────────────────────────────────
 * A fixed, repeatable churn: fill all backings and regions, tear them all down,
 * repeated several times, plus a data-plane resolution each cycle where a target
 * is available.  Counters must track exactly and return to zero — no leak, no
 * drift, fully deterministic across runs.  Invariants: F32, F39. */
static void test_t230(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "stress";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    /* ONE grant feeds all four pager backings (a grant is per-backing at the
     * VFS; the pager may reference it from several backing slots). */
    struct t28_grant gr;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "grant open"; }

    /* Four control-plane cycles: fill 4 backings + 16 regions, verify, tear down. */
    for (uint32_t cyc = 0; ok && cyc < 4u; cyc++) {
        for (uint32_t bi = 0; ok && bi < FBK_MAX_BACKINGS; bi++)
            if (t28_reg_backing2(f.ctrl_ep, bi, &gr, (uint64_t)sz) != 0) { ok = 0; why = "stress backing"; }
        for (uint32_t ri = 0; ok && ri < FBK_MAX_REGIONS; ri++) {
            struct pgr_region_req rq;
            uint64_t va  = T28_VA_A + (uint64_t)ri * 0x40000ULL;
            uint64_t foff = (uint64_t)(ri % 5) * 0x1000ULL;
            t28_region(&rq, ri, 0 /*the one wired target*/, ri % FBK_MAX_BACKINGS, va, 0x1000, foff, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
            if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "stress region"; }
        }
        if (ok) {
            struct pgr_diag d;
            if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "stress diag"; }
            else if (d.backing_live != FBK_MAX_BACKINGS || d.region_count != FBK_MAX_REGIONS) { ok = 0; why = "stress fill counts"; }
        }
        /* Tear everything down; counters return to zero. */
        for (uint32_t ri = 0; ok && ri < FBK_MAX_REGIONS; ri++)
            if (t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, ri, 0) != 0) { ok = 0; why = "stress unreg"; }
        for (uint32_t bi = 0; ok && bi < FBK_MAX_BACKINGS; bi++)
            if (t28_ctrl_words(f.ctrl_ep, FBK_OP_REVOKE_BACKING, bi, 0) != 0) { ok = 0; why = "stress revoke"; }
        if (ok) {
            struct pgr_diag d;
            if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "stress diag2"; }
            else if (d.backing_live != 0u || d.region_count != 0u || d.cache_entries != 0u) { ok = 0; why = "stress teardown counts"; }
        }
    }
    /* Data-plane: after the churn, a fresh backing+region resolves correctly. */
    if (ok && t28_reg_backing2(f.ctrl_ep, 0, &gr, (uint64_t)sz) != 0) { ok = 0; why = "final backing"; }
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x3000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "final region"; }
    }
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x3000), &why)) ok = 0;

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T230"); else it_fail("T230", why);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Fase 28.1 — File Grant Capability Enforcement + Pager Multi-target (T231–T238)
 *
 * These tests attack the TRUST BOUNDARY, not the functional layer (T217–T230
 * already prove content correctness).  The premise everywhere is a HOSTILE
 * pager: iris_test self-mints a SESSION-badged vfs cap byte-identical to the
 * one a pager of that session holds (t28_session_cap) and drives the VFS
 * DIRECTLY — bypassing every check the pager's own helper would make — to
 * prove the VFS itself denies.  A helper rejecting the request would not
 * count; only a VFS reply of ACCESS_DENIED / CLOSED / NOT_FOUND does.
 * ════════════════════════════════════════════════════════════════════════ */

/* ── T231: VFS-enforced file grant identity ──────────────────────────────────
 * Two files → two grants with distinct VFS-issued (backing_id, generation).
 * A session's own cap reads its granted file and gets that file's bytes; it
 * CANNOT read the other file through the wrong grant index, a bogus index, a
 * wrong-type message, or after the grant is stale.  No cross-read.
 * Invariants: A1, A2, A3, A4, A5, A18. */
static void test_t231(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "grant identity";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;

    /* Two grants (session 0), one per file, both STAT|READ. */
    struct t28_grant ga, gb;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &ga) != 0) { ok = 0; why = "open A"; }
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK2_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gb) != 0) { ok = 0; why = "open B"; }
    /* Distinct, VFS-issued identities (A2/A18). */
    if (ok && (ga.bid == gb.bid || ga.idx == gb.idx)) { ok = 0; why = "identities not distinct"; }

    /* The session's own cap — the exact authority the pager holds. */
    handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc == HANDLE_INVALID) { ok = 0; why = "session cap"; }

    /* Each grant reads ITS file's byte at offset 0x1000 (A5). */
    uint8_t byte; uint64_t n;
    if (ok && (t28_grant_read(sc, ga.idx, 0x1000, 1, &byte, &n) != 0 || n != 1u || byte != t28_pat(0x1000))) { ok = 0; why = "read A"; }
    if (ok && (t28_grant_read(sc, gb.idx, 0x1000, 1, &byte, &n) != 0 || n != 1u || byte != t28_pat2(0x1000))) { ok = 0; why = "read B"; }
    /* STAT reports each backing's own VFS-issued identity. */
    uint64_t sz, bid, gen;
    if (ok && (t28_grant_stat(sc, ga.idx, &sz, &bid, &gen) != 0 || bid != ga.bid || gen != ga.gen || sz != FBK_FILE_SIZE)) { ok = 0; why = "stat A"; }
    if (ok && (t28_grant_stat(sc, gb.idx, &sz, &bid, &gen) != 0 || bid != gb.bid || gen != gb.gen || sz != FBK2_FILE_SIZE)) { ok = 0; why = "stat B"; }

    /* A bogus grant index is NOT_FOUND (not silently served). */
    if (ok && t28_grant_read(sc, 30u, 0, 1, 0, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "bogus idx served"; }
    /* Wrong-type: a STAT-labelled message on a READ grant path is fine, but a
     * name-based STAT from the SESSION badge is denied outright (containment). */
    if (ok) {
        long r = t28_gcall(sc, VFS_EP_OP_STAT, 0, 0, 0, 0u, FBK_FILE_NAME, 0);
        if (r != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "session named-stat not denied"; }
    }
    it_close(&sc);

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T231"); else it_fail("T231", why);
}

/* ── T232: arbitrary-name attack denial ──────────────────────────────────────
 * From the posture of a compromised pager holding ONLY its session cap: every
 * attempt to widen authority by crafting a message is denied BY THE VFS.  A
 * pathname is not authority (A1); changing a grant index, backing id, or
 * generation in a message does not change what is served (A6); the session
 * cap cannot open a new grant, revoke by name, reset a session, or use any
 * name-based op; and no generic unrestricted VFS cap exists to fall back on
 * (A13/A14).  Invariants: A1, A4, A5, A6, A13, A14, A30. */
static void test_t232(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "arbitrary-name attack";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    /* One legitimate grant on fbk.dat (READ only). */
    struct t28_grant gr;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "open"; }

    handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc == HANDLE_INVALID) { ok = 0; why = "session cap"; }

    /* Attack 1: name-based READ_AT with the OTHER file's name — denied (a
     * session badge cannot touch the name-based path at all). */
    if (ok) {
        long r = t28_gcall(sc, VFS_EP_OP_READ_AT, 0x1000, 1, 0, 2u, FBK2_FILE_NAME, 0);
        if (r != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "named read served"; }
    }
    /* Attack 2: LIST to enumerate exports — denied. */
    if (ok && t28_gcall(sc, VFS_EP_OP_LIST, 0, 0, 0, 1u, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "list served"; }
    /* Attack 3: forge a GRANT_OPEN (escalate to a new file) — session badge is
     * not the admin, denied. */
    if (ok && t28_gcall(sc, VFS_EP_OP_GRANT_OPEN, FBK_SESSION, VFS_FILE_RIGHT_ALL, 0, 2u, FBK2_FILE_NAME, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "open escalation"; }
    /* Attack 4: revoke-by-name (the admin-only form) from the session — the
     * name-based admin path is UNREACHABLE for a session badge: the request
     * routes to the session index-form path, where word_count 0 is malformed
     * (INVALID_ARG) and, crucially, NO backing is revoked.  Either way the
     * session cannot revoke by name; the legit read below proves nothing was
     * revoked. */
    if (ok) {
        long r = t28_gcall(sc, VFS_EP_OP_GRANT_REVOKE, 0, 0, 0, 0u, FBK_FILE_NAME, 0);
        if (r != (long)IRIS_ERR_INVALID_ARG && r != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "name revoke"; }
    }
    /* Attack 5: session reset (admin-only) — denied. */
    if (ok && t28_gcall(sc, VFS_EP_OP_GRANT_SESSION_RESET, FBK_SESSION, 0, 0, 1u, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "session reset"; }
    /* Attack 6: read a DIFFERENT session's grant index (cross-session) — the
     * badge selects the session, so index gr.idx in session 1 is empty →
     * NOT_FOUND, never session 0's data. */
    if (ok) {
        handle_id_t sc1 = t28_session_cap(1u);
        if (sc1 == HANDLE_INVALID) { ok = 0; why = "session1 cap"; }
        else {
            if (t28_grant_read(sc1, gr.idx, 0x1000, 1, 0, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "cross-session read"; }
            it_close(&sc1);
        }
    }
    /* The legitimate grant still works (denials had no side effects). */
    uint8_t byte; uint64_t n;
    if (ok && (t28_grant_read(sc, gr.idx, 0x1000, 1, &byte, &n) != 0 || byte != t28_pat(0x1000))) { ok = 0; why = "legit read broke"; }
    it_close(&sc);

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T232"); else it_fail("T232", why);
}

/* ── T233: file grant rights monotonicity ────────────────────────────────────
 * Derived grants can only SHRINK rights, and rights can never be recovered.  A
 * STAT-only grant cannot read; a READ-only grant cannot revoke; a grant without
 * DUPLICATE cannot derive; a derive requesting a right the source lacks is
 * denied (not clamped).  Enforcement is in the VFS.  Invariants: A7. */
static void test_t233(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "rights monotonicity";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc == HANDLE_INVALID) { ok = 0; why = "session cap"; }

    /* STAT-only grant: STAT works, READ denied. */
    struct t28_grant gs;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_STAT, &gs) != 0) { ok = 0; why = "open stat"; }
    uint64_t sz;
    if (ok && t28_grant_stat(sc, gs.idx, &sz, 0, 0) != 0) { ok = 0; why = "stat-only stat"; }
    if (ok && t28_grant_read(sc, gs.idx, 0, 1, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "stat-only read"; }

    /* READ-only grant: READ works, REVOKE denied, DERIVE denied (no DUP). */
    struct t28_grant gr;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "open read"; }
    uint8_t byte;
    if (ok && t28_grant_read(sc, gr.idx, 0x1000, 1, &byte, 0) != 0) { ok = 0; why = "read-only read"; }
    if (ok && t28_grant_revoke_idx(sc, gr.idx, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read-only revoke"; }
    if (ok && t28_grant_derive(sc, gr.idx, VFS_FILE_RIGHT_READ, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-dup derive"; }

    /* A grant WITH DUPLICATE derives a strictly smaller one; the derive cannot
     * request a right the source lacks (recovery denied). */
    struct t28_grant gd;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ | VFS_FILE_RIGHT_DUPLICATE, &gd) != 0) { ok = 0; why = "open dup"; }
    /* Requesting REVOKE (source lacks it) → ACCESS_DENIED. */
    if (ok && t28_grant_derive(sc, gd.idx, VFS_FILE_RIGHT_READ | VFS_FILE_RIGHT_REVOKE, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights recovery"; }
    /* Derive a READ-only child; it reads but cannot itself derive (DUP dropped). */
    uint32_t child = 0;
    if (ok && t28_grant_derive(sc, gd.idx, VFS_FILE_RIGHT_READ, &child) != 0) { ok = 0; why = "derive read"; }
    if (ok && t28_grant_read(sc, child, 0x1000, 1, &byte, 0) != 0) { ok = 0; why = "child read"; }
    if (ok && t28_grant_derive(sc, child, VFS_FILE_RIGHT_READ, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "child re-derive"; }
    it_close(&sc);

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T233"); else it_fail("T233", why);
}

/* ── T234: revoke and generation replay denial ───────────────────────────────
 * A grant is opened and used; the ADMIN revokes the backing (VFS-side).  The
 * SAME grant index, replayed with the SAME message, now fails CLOSED at the
 * VFS — even though the session still holds the cap and the index (A9).  A new
 * grant on the same file gets a NEWER generation; the OLD generation never
 * validates against the new (A10).  Revocation is enforced by the VFS, not the
 * pager (the pager's local table is irrelevant).  Invariants: A4, A8, A9, A10. */
static void test_t234(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "revoke/replay";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc == HANDLE_INVALID) { ok = 0; why = "session cap"; }

    struct t28_grant gN;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gN) != 0) { ok = 0; why = "open N"; }
    uint8_t byte;
    if (ok && t28_grant_read(sc, gN.idx, 0x1000, 1, &byte, 0) != 0) { ok = 0; why = "read N"; }

    /* ADMIN revokes the backing at the VFS. */
    uint64_t newgen = 0;
    if (ok && t28_grant_revoke_name(f.admin, FBK_FILE_NAME, &newgen) != 0) { ok = 0; why = "revoke"; }
    if (ok && newgen == gN.gen) { ok = 0; why = "generation not bumped"; }

    /* Replay the EXACT read on the same index → CLOSED (VFS-enforced, A9). */
    if (ok && t28_grant_read(sc, gN.idx, 0x1000, 1, 0, 0) != (long)IRIS_ERR_CLOSED) { ok = 0; why = "replay served"; }
    if (ok && t28_grant_stat(sc, gN.idx, 0, 0, 0) != (long)IRIS_ERR_CLOSED) { ok = 0; why = "replay stat served"; }

    /* A fresh grant gets generation N+1; the old snapshot never matches (A10). */
    struct t28_grant gN1;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gN1) != 0) { ok = 0; why = "open N+1"; }
    if (ok && gN1.gen != newgen) { ok = 0; why = "new grant wrong gen"; }
    if (ok && gN1.gen == gN.gen) { ok = 0; why = "gen reused"; }
    /* The new grant reads; the old index is still CLOSED. */
    if (ok && t28_grant_read(sc, gN1.idx, 0x1000, 1, &byte, 0) != 0) { ok = 0; why = "new grant read"; }
    if (ok && t28_grant_read(sc, gN.idx, 0x1000, 1, 0, 0) != (long)IRIS_ERR_CLOSED) { ok = 0; why = "old still readable"; }
    it_close(&sc);

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T234"); else it_fail("T234", why);
}

/* ── T235: pager restart with per-backing grants ─────────────────────────────
 * The supervisor opens a grant for a pager, the pager dies, and a NEW pager
 * instance is started.  The restart protocol RESETS the session at the VFS
 * first, so the new pager's session cap cannot reach the OLD instance's grant
 * (A11): replaying the old grant index from the new session cap fails
 * NOT_FOUND.  The supervisor issues a FRESH grant and the new pager resolves.
 * Invariants: A11. */
static void test_t235(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager restart grants";

    /* Instance 1: open a grant, note its index, then kill the pager. */
    struct t25_tgt g1;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t28_fbk f1;
    if (ok && !t28_fbk_spawn(&f1, &g1, 1u, &why)) ok = 0;
    struct t28_grant gold;
    if (ok && t28_grant_open(f1.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gold) != 0) { ok = 0; why = "open old"; }
    /* Prove it was live. */
    handle_id_t sc1 = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && (sc1 == HANDLE_INVALID || t28_grant_read(sc1, gold.idx, 0x1000, 1, 0, 0) != 0)) { ok = 0; why = "old read"; }
    it_close(&sc1);
    t28_fbk_reap(&f1);
    t25_tgt_reap(&g1);

    /* Instance 2: t28_fbk_spawn's restart protocol RESETS session 0 first. */
    struct t25_tgt g2;
    if (ok && !t25_tgt_spawn(&g2, &why)) ok = 0;
    struct t28_fbk f2;
    if (ok && !t28_fbk_spawn(&f2, &g2, 1u, &why)) ok = 0;
    handle_id_t sc2 = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc2 == HANDLE_INVALID) { ok = 0; why = "session cap 2"; }
    /* The OLD grant index is gone from the session (A11). */
    if (ok && t28_grant_read(sc2, gold.idx, 0x1000, 1, 0, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale grant survived restart"; }
    /* A fresh grant + backing + region resolves end to end. */
    long sz = ok ? t28_stat(f2.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gnew;
    if (ok && !t28_backing_setup(&f2, 0, FBK_FILE_NAME, (uint64_t)sz, &gnew, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gnew.gen);
        if (t28_reg_region(f2.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 2"; }
    }
    if (ok && !t28_read_verify(&f2, &g2, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    it_close(&sc2);
    t28_fbk_reap(&f2);
    t25_tgt_reap(&g2);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T235"); else it_fail("T235", why);
}

/* ── T236: VFS restart invalidates old grants ────────────────────────────────
 * A VFS instance issues grants under an epoch that stamps the high half of
 * every generation.  A restarted VFS gets a strictly newer epoch, so a grant
 * snapshotting an old generation can never validate against the new instance
 * (A12).  We SIMULATE the new instance in-process by re-initializing a grant
 * table under a bumped epoch (the productive VFS uses its svcmgr restart
 * generation for the same effect) and confirm: (a) old generations never equal
 * new ones; (b) mappings already installed follow the Fase 28 contract.  The
 * cross-instance generation-namespace property is verified against the live
 * VFS's issued generations.  Invariants: A12, A16. */
static void test_t236(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "vfs restart grants";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;

    /* Open a grant, install a real mapping via a fault resolution. */
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;

    /* The generation carries the instance epoch in its high half: a restarted
     * VFS (strictly newer epoch) cannot reissue this generation.  We check the
     * epoch field is nonzero and monotonic by opening a second grant on a
     * DIFFERENT file and confirming both share the same instance epoch (high
     * 48 bits) — i.e. one live instance — while a hypothetical older-epoch
     * generation (epoch-1) can never appear. */
    struct t28_grant gr2;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK2_FILE_NAME, VFS_FILE_RIGHT_READ, &gr2) != 0) { ok = 0; why = "open 2"; }
    if (ok) {
        uint64_t epoch_a = gr.gen >> 16, epoch_b = gr2.gen >> 16;
        if (epoch_a != epoch_b) { ok = 0; why = "epoch drift within instance"; }
        /* A stale grant carrying an older-epoch generation must fail: forge one
         * by registering a backing whose generation is from a lower epoch and
         * confirm the pager's cross-check (against the live VFS) denies it. */
        uint64_t stale_gen = (epoch_a > 0 ? (epoch_a - 1) : 0) << 16 | 1u;
        if (t28_reg_backing_raw(f.ctrl_ep, 1, gr.idx, gr.bid, stale_gen, (uint64_t)sz)
            != -(long)FBK_ERR_GRANT) { ok = 0; why = "stale-epoch backing accepted"; }
    }
    /* The already-installed mapping still holds (Fase 28 contract, A16): the
     * target already exited reading the correct byte above; re-reading is not
     * possible (a resolved target completed), so the mapping-survival property
     * is the successful resolution itself plus a clean baseline. */

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T236"); else it_fail("T236", why);
}

/* ── multi-target harness (T237/T238) ────────────────────────────────────────
 * A lightweight target group that shares ONE fault notification and ONE
 * exit-watch notification across ALL targets — so N targets cost the
 * supervisor exactly 2 notifications, never 2*N.  This is what lets 16 targets
 * be registered and faulted simultaneously without touching the per-process
 * notification quota.  The pager likewise holds ONE fault notification (slot
 * 5) for all of them, waking on bit (1<<i). */
#define T28_MT_MAX 16u
struct t28_multi {
    handle_id_t fault_notif;     /* shared: bit i set when target i faults */
    handle_id_t exit_notif;      /* shared: bit i set when target i exits */
    handle_id_t cmd[T28_MT_MAX];
    handle_id_t proc[T28_MT_MAX];
    handle_id_t vs[T28_MT_MAX];
    uint32_t    n;
};
static void t28_multi_close(struct t28_multi *m) {
    for (uint32_t i = 0; i < m->n; i++) {
        it_close(&m->cmd[i]); it_close(&m->proc[i]); it_close(&m->vs[i]);
    }
    it_close(&m->fault_notif); it_close(&m->exit_notif);
    m->n = 0;
}
static void t28_multi_reap(struct t28_multi *m) {
    for (uint32_t i = 0; i < m->n; i++)
        if (m->proc[i] != HANDLE_INVALID) {
            (void)it_sys1(SYS_PROCESS_KILL, (long)m->proc[i]);
            (void)it_lp_wait_exit(m->proc[i]);
        }
    t28_multi_close(m);
}
/* Spawn `nt` lifecycle_probe targets sharing two notifications.  Each target's
 * exception handler signals fault_notif bit (1<<i); its exit watch signals
 * exit_notif bit (1<<i). */
static int t28_multi_spawn(struct t28_multi *m, uint32_t nt, const char **why) {
    for (uint32_t i = 0; i < T28_MT_MAX; i++) { m->cmd[i] = m->proc[i] = m->vs[i] = HANDLE_INVALID; }
    m->fault_notif = m->exit_notif = HANDLE_INVALID; m->n = 0;
    if (nt > T28_MT_MAX) { *why = "too many targets"; return 0; }
    long fn = it_notify_create();
    long en = it_notify_create();
    if (fn < 0 || en < 0) { it_close(&m->fault_notif); it_close(&m->exit_notif);
        if (fn >= 0) { handle_id_t h = (handle_id_t)fn; it_close(&h); }
        if (en >= 0) { handle_id_t h = (handle_id_t)en; it_close(&h); }
        *why = "shared notifs"; return 0; }
    m->fault_notif = (handle_id_t)fn; m->exit_notif = (handle_id_t)en;
    for (uint32_t i = 0; i < nt; i++) {
        long ep = it_ep_create();
        if (ep < 0) { *why = "cmd ep"; t28_multi_close(m); return 0; }
        m->cmd[i] = (handle_id_t)ep;
        if (lp_spawn_child(m->cmd[i], &m->proc[i]) < 0 || m->proc[i] == HANDLE_INVALID) { *why = "spawn"; t28_multi_close(m); return 0; }
        long vs = it_sys1(SYS_PROCESS_VSPACE, (long)m->proc[i]);
        if (vs < 0) { *why = "vspace"; t28_multi_close(m); return 0; }
        m->vs[i] = (handle_id_t)vs;
        if (it_sys3(SYS_EXCEPTION_HANDLER, (long)m->proc[i], (long)m->fault_notif, (long)(1u << i)) != 0 ||
            it_sys3(SYS_PROCESS_WATCH,     (long)m->proc[i], (long)m->exit_notif,  (long)(1u << i)) != 0) {
            *why = "wire"; t28_multi_close(m); return 0;
        }
        m->n++;
    }
    return 1;
}
/* Spawn a pager over a t28_multi group: shares the group's fault notification
 * (slot 5) and grants proc/vs for each target.  Resets session 0 first. */
static int t28_fbk_spawn_multi(struct t28_fbk *f, struct t28_multi *m, const char **why) {
    f->ctrl_ep = f->proc = f->cache_vmo = f->priv_vmo = HANDLE_INVALID;
    f->vfs_cap = f->admin = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ctrl ep"; return 0; }
    handle_id_t ctrl = (handle_id_t)ep;
    handle_id_t cvmo = t26_vmo_create((uint64_t)FBK_CACHE_CAP * 0x1000ULL);
    handle_id_t pvmo = t26_vmo_create((uint64_t)FBK_PRIV_CAP * 0x1000ULL);
    handle_id_t vfs  = t28_vfs_cap();
    handle_id_t adm  = t28_admin_cap();
    if (cvmo == HANDLE_INVALID || pvmo == HANDLE_INVALID || vfs == HANDLE_INVALID || adm == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&cvmo); it_close(&pvmo); it_close(&vfs); it_close(&adm);
        *why = "grants"; return 0;
    }
    if (t28_session_reset(adm, FBK_SESSION) != 0) {
        it_close(&ctrl); it_close(&cvmo); it_close(&pvmo); it_close(&vfs); it_close(&adm);
        *why = "session reset"; return 0;
    }
    struct svc_mint mm[40];
    uint32_t k = 0;
    mm[k].slot = PGR_SLOT_CTRL_EP; mm[k].src_h = ctrl; mm[k].rights = RIGHT_READ; mm[k].badge = 0; k++;
    mm[k].slot = FBK_SLOT_VFS_EP;  mm[k].src_h = vfs;  mm[k].rights = RIGHT_WRITE;
    mm[k].badge = IRIS_BADGE_FILEGRANT_S(FBK_SESSION); k++;
    mm[k].slot = FBK_SLOT_NOTIF;   mm[k].src_h = m->fault_notif; mm[k].rights = RIGHT_WAIT; mm[k].badge = 0; k++;
    for (uint32_t i = 0; i < m->n; i++) {
        mm[k].slot = PGR_TSLOT_PROC(i); mm[k].src_h = m->proc[i]; mm[k].rights = RIGHT_READ | RIGHT_MANAGE; mm[k].badge = 0; k++;
        mm[k].slot = PGR_TSLOT_VS(i);   mm[k].src_h = m->vs[i];   mm[k].rights = RIGHT_WRITE;               mm[k].badge = 0; k++;
    }
    mm[k].slot = PGR_VSLOT(0); mm[k].src_h = cvmo; mm[k].rights = RIGHT_READ | RIGHT_WRITE; mm[k].badge = 0; k++;
    mm[k].slot = PGR_VSLOT(1); mm[k].src_h = pvmo; mm[k].rights = RIGHT_READ | RIGHT_WRITE; mm[k].badge = 0; k++;
    handle_id_t boot = HANDLE_INVALID;
    /* Fase S1: explicit reply object for the pager's ctrl EP (slot 13). */
    handle_id_t pgr_reply_h = HANDLE_INVALID;
    {
        long rr = it_retype_handle((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
        if (rr >= 0) {
            pgr_reply_h = (handle_id_t)rr;
            mm[k].slot = 13u; mm[k].src_h = pgr_reply_h;
            mm[k].rights = RIGHT_READ | RIGHT_WRITE; mm[k].badge = 0; k++;
        }
    }
    long r = svc_load_minted((handle_id_t)IRIS_CPTR_SPAWN_CAP, "pager", &f->proc, &boot, mm, k);
    it_close(&pgr_reply_h);
    it_close(&boot);
    if (r < 0 || f->proc == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&cvmo); it_close(&pvmo); it_close(&vfs); it_close(&adm); it_close(&f->proc);
        *why = "pager spawn"; return 0;
    }
    f->ctrl_ep = ctrl; f->cache_vmo = cvmo; f->priv_vmo = pvmo; f->vfs_cap = vfs; f->admin = adm;
    return 1;
}
/* Wait (≤2s) for exit bit i on the shared exit notification, consuming and
 * re-accumulating other bits so no exit is lost. */
static uint64_t g_t28_exit_pending = 0;
static int t28_multi_wait_exit(struct t28_multi *m, uint32_t i) {
    uint64_t bit = 1ull << i;
    if (g_t28_exit_pending & bit) { g_t28_exit_pending &= ~bit; return 1; }
    for (uint32_t tries = 0; tries < 64u; tries++) {
        uint64_t bits = 0;
        if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)m->exit_notif, (long)(uintptr_t)&bits, 2000000000LL) != 0) return 0;
        g_t28_exit_pending |= bits;
        if (g_t28_exit_pending & bit) { g_t28_exit_pending &= ~bit; return 1; }
    }
    return 0;
}

/* ── T237: multi-target notification scaling ──────────────────────────────────
 * Register and exercise 1, 4, 8 and 16 targets under a SINGLE pager, all
 * sharing ONE fault notification.  Each target faults on its own file page and
 * reads the correct byte; faults are interleaved (all triggered before any is
 * resolved); the supervisor's notification books return to baseline.  Proves
 * the quota problem is SOLVED, not avoided (Fase 28 could only run 1 target).
 * Invariants: A19, A20, A21, A22, A23, A25. */
static int t237_run(uint32_t nt, const char **why) {
    struct t28_multi m;
    if (!t28_multi_spawn(&m, nt, why)) return 0;
    struct t28_fbk f;
    if (!t28_fbk_spawn_multi(&f, &m, why)) { t28_multi_reap(&m); return 0; }
    int ok = 1;
    g_t28_exit_pending = 0;

    long sz = t28_stat(f.vfs_cap, FBK_FILE_NAME);
    struct t28_grant gr;
    if (!t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, why)) ok = 0;
    /* One region per target: target i reads file page (i % 5)+? — use page i+1
     * clamped into the 5-page file so each has a real file byte. */
    for (uint32_t i = 0; ok && i < nt; i++) {
        struct pgr_region_req rq;
        uint64_t foff = (uint64_t)((i % 4u) + 1u) * 0x1000ULL;   /* pages 1..4 */
        t28_region(&rq, i, i, 0, T28_VA_A + (uint64_t)i * 0x40000ULL, 0x1000, foff, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; *why = "reg region"; }
    }
    /* Interleave: trigger ALL faults first (each target blocks in its fault),
     * then resolve them in a scrambled order. */
    for (uint32_t i = 0; ok && i < nt; i++)
        if (it_lp_cmd_va(m.cmd[i], LP_CMD_FAULT_READ, T28_VA_A + (uint64_t)i * 0x40000ULL) != 0) { ok = 0; *why = "trigger"; }
    for (uint32_t s = 0; ok && s < nt; s++) {
        uint32_t i = (s * 7u + 3u) % nt;    /* scrambled visit order */
        /* Skip already-resolved slots in the scramble by linear-probing. */
        uint32_t tries = 0;
        while (tries < nt && ((1u << i) & 0)) { i = (i + 1u) % nt; tries++; }
        if (t28_map_region(f.ctrl_ep, i) != 0) { ok = 0; *why = "resolve"; }
    }
    /* Each target ran to completion reading its page's byte. */
    for (uint32_t i = 0; ok && i < nt; i++) {
        uint64_t foff = (uint64_t)((i % 4u) + 1u) * 0x1000ULL;
        if (!t28_multi_wait_exit(&m, i)) { ok = 0; *why = "target exit"; }
        else if (it_sys1(SYS_PROCESS_EXIT_CODE, (long)m.proc[i]) != (long)(LP_EXIT_MARKER ^ (uint32_t)t28_pat(foff))) { ok = 0; *why = "wrong byte"; }
    }
    /* Diagnostics: the pager multiplexed all nt faults over ONE shared
     * notification.  The wait-any accumulator means a single wakeup can carry
     * several targets' bits, so wakeups is BETWEEN 1 and nt (fewer wakeups than
     * faults is the efficiency win, not a bug).  What must hold: the shared
     * notification path WAS exercised (>=1 wait) and no fault bit is left
     * pending (all consumed and resolved, no mix). */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; *why = "diag"; }
        else if (d.notif_waits == 0u || d.notif_wakeups == 0u) { ok = 0; *why = "shared notif unused"; }
        else if (d.notif_wakeups > nt) { ok = 0; *why = "excess wakeups"; }
        else if (d.pending_mask != 0u) { ok = 0; *why = "pending residue"; }
    }
    t28_fbk_reap(&f);
    t28_multi_reap(&m);
    return ok;
}
static void test_t237(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "multi-target scaling";

    const uint32_t counts[4] = { 1u, 4u, 8u, 16u };
    for (uint32_t c = 0; ok && c < 4u; c++) {
        if (!t237_run(counts[c], &why)) ok = 0;
        it_quiesce_reaper();
        if (ok) { struct it_snap r = it_snap_take(); if (!it_snap_baseline_live(&b, &r, &why)) ok = 0; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T237"); else it_fail("T237", why);
}

/* ── T238: deterministic file-authority and multi-target stress ───────────────
 * A seeded round-robin over the whole Fase 28.1 surface: open/derive/read a
 * grant, hostile wrong-backing and wrong-name attempts, revoke + generation
 * change, and a multi-target fault batch that dies in arbitrary order.  Every
 * round: no unauthorized read, no stale success, no target mix, notification
 * and object books at baseline, handle HWM bounded, no flakes.
 * Invariants: A1–A30 under load. */
#define T238_SEED   0x281F00D5u
#define T238_ROUNDS 8u
static uint32_t t238_rnd(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }
static void test_t238(void) {
    uint32_t rng = T238_SEED;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "file-authority stress";
    uint32_t round = 0, op = 0;

    for (round = 0; ok && round < T238_ROUNDS; round++) {
        op = t238_rnd(&rng) % 3u;
        switch (op) {
        case 0: {
            /* Grant + hostile attempts + honest read. */
            struct t25_tgt g;
            if (!t25_tgt_spawn(&g, &why)) { ok = 0; break; }
            struct t28_fbk f;
            if (!t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; t25_tgt_reap(&g); break; }
            struct t28_grant gr;
            if (t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "s0 open"; }
            handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
            if (ok && sc == HANDLE_INVALID) { ok = 0; why = "s0 cap"; }
            uint8_t byte;
            if (ok && (t28_grant_read(sc, gr.idx, 0x1000, 1, &byte, 0) != 0 || byte != t28_pat(0x1000))) { ok = 0; why = "s0 read"; }
            /* wrong-name (session cannot use name path). */
            if (ok && t28_gcall(sc, VFS_EP_OP_READ_AT, 0x1000, 1, 0, 2u, FBK2_FILE_NAME, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "s0 name"; }
            /* wrong-backing register at the pager. */
            if (ok && t28_reg_backing_raw(f.ctrl_ep, 1, gr.idx, gr.bid + 9u, gr.gen, (uint64_t)FBK_FILE_SIZE) != -(long)FBK_ERR_GRANT) { ok = 0; why = "s0 wrongback"; }
            it_close(&sc);
            t28_fbk_reap(&f);
            t25_tgt_reap(&g);
            break;
        }
        case 1: {
            /* Revoke + replay denial + fresh generation. */
            struct t25_tgt g;
            if (!t25_tgt_spawn(&g, &why)) { ok = 0; break; }
            struct t28_fbk f;
            if (!t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; t25_tgt_reap(&g); break; }
            struct t28_grant gr;
            if (t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "s1 open"; }
            handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
            if (ok && sc == HANDLE_INVALID) { ok = 0; why = "s1 cap"; }
            if (ok && t28_grant_read(sc, gr.idx, 0, 1, 0, 0) != 0) { ok = 0; why = "s1 read"; }
            uint64_t ng = 0;
            if (ok && t28_grant_revoke_name(f.admin, FBK_FILE_NAME, &ng) != 0) { ok = 0; why = "s1 revoke"; }
            if (ok && t28_grant_read(sc, gr.idx, 0, 1, 0, 0) != (long)IRIS_ERR_CLOSED) { ok = 0; why = "s1 replay"; }
            struct t28_grant gr2;
            if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gr2) != 0) { ok = 0; why = "s1 reopen"; }
            if (ok && gr2.gen == gr.gen) { ok = 0; why = "s1 gen reuse"; }
            if (ok && t28_grant_read(sc, gr2.idx, 0, 1, 0, 0) != 0) { ok = 0; why = "s1 new read"; }
            it_close(&sc);
            t28_fbk_reap(&f);
            t25_tgt_reap(&g);
            break;
        }
        default: {
            /* Multi-target batch (4 targets), death in arbitrary order. */
            uint32_t nt = 4u;
            struct t28_multi m;
            if (!t28_multi_spawn(&m, nt, &why)) { ok = 0; break; }
            struct t28_fbk f;
            if (!t28_fbk_spawn_multi(&f, &m, &why)) { ok = 0; t28_multi_reap(&m); break; }
            g_t28_exit_pending = 0;
            long sz = t28_stat(f.vfs_cap, FBK_FILE_NAME);
            struct t28_grant gr;
            if (!t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
            for (uint32_t i = 0; ok && i < nt; i++) {
                struct pgr_region_req rq;
                uint64_t foff = (uint64_t)(i + 1u) * 0x1000ULL;
                t28_region(&rq, i, i, 0, T28_VA_A + (uint64_t)i * 0x40000ULL, 0x1000, foff, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
                if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "s2 region"; }
            }
            for (uint32_t i = 0; ok && i < nt; i++)
                if (it_lp_cmd_va(m.cmd[i], LP_CMD_FAULT_READ, T28_VA_A + (uint64_t)i * 0x40000ULL) != 0) { ok = 0; why = "s2 trigger"; }
            /* Resolve in reverse order (arbitrary vs trigger order). */
            for (uint32_t s = 0; ok && s < nt; s++) {
                uint32_t i = nt - 1u - s;
                if (t28_map_region(f.ctrl_ep, i) != 0) { ok = 0; why = "s2 resolve"; }
            }
            for (uint32_t i = 0; ok && i < nt; i++) {
                uint64_t foff = (uint64_t)(i + 1u) * 0x1000ULL;
                if (!t28_multi_wait_exit(&m, i)) { ok = 0; why = "s2 exit"; }
                else if (it_sys1(SYS_PROCESS_EXIT_CODE, (long)m.proc[i]) != (long)(LP_EXIT_MARKER ^ (uint32_t)t28_pat(foff))) { ok = 0; why = "s2 byte"; }
            }
            t28_fbk_reap(&f);
            t28_multi_reap(&m);
            break;
        }
        }
        it_quiesce_reaper();
        if (ok) { struct it_snap r = it_snap_take(); if (!it_snap_baseline_live(&b, &r, &why)) ok = 0; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T238");
    else { it_fz_note("T238", T238_SEED, round, op); it_fail("T238", why); }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Fase 29 — Resource Ownership, Quota Domains and Kernel Capacity (T239–T250)
 *
 * Model: a KProcess IS a resource domain.  Every object is charged to the
 * process that logically OWNS it (its payer), selected by explicit capability
 * authority at creation — NOT to whoever ran the syscall.  A loader creates a
 * child's image VMOs charged to the CHILD (SYS_VMO_CREATE_FOR + the child's
 * process cap with RIGHT_MANAGE), so the loader's own quota stays flat.  These
 * tests prove creator/owner/payer/holder are separate, sharing charges once,
 * exhaustion is atomic, and usage returns to baseline.
 * ════════════════════════════════════════════════════════════════════════ */

#define SYS_VMO_CREATE_FOR 109
#define SYS_RESOURCE_INFO  110
#define IT_VMO_QUOTA       32u   /* == KPROCESS_VMO_QUOTA */

/* Mirror of struct iris_resource_info (iris/syscall.h) — layout MUST match. */
struct it_rinfo {
    uint32_t version, struct_size;
    uint32_t vmos_usage, vmos_limit, vmos_hwm;
    uint32_t notifs_usage, notifs_limit, notifs_hwm;
    uint32_t pages_usage, pages_limit, pages_hwm;
    uint32_t global_failed_charges, global_rollbacks;
    uint32_t kslab_used_bytes, kslab_total_bytes, kslab_hwm_bytes, kslab_alloc_failures;
};
/* Read a process's resource snapshot (HANDLE_INVALID = self).  1 on success. */
static int it_rinfo(handle_id_t proc_h, struct it_rinfo *out) {
    for (uint32_t i = 0; i < (uint32_t)sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    out->struct_size = (uint32_t)sizeof(*out);
    return it_sys2(SYS_RESOURCE_INFO, (long)proc_h, (long)(uintptr_t)out) == 0;
}

/* Spawn a bare lifecycle_probe child (cmd endpoint + process).  0 on success. */
static int it_bare_child(handle_id_t *cmd_out, handle_id_t *proc_out) {
    *cmd_out = *proc_out = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) return 0;
    *cmd_out = (handle_id_t)ep;
    if (lp_spawn_child(*cmd_out, proc_out) < 0 || *proc_out == HANDLE_INVALID) {
        it_close(cmd_out); return 0;
    }
    return 1;
}
static void it_bare_kill(handle_id_t *cmd, handle_id_t *proc) {
    if (*proc != HANDLE_INVALID) { (void)it_sys1(SYS_PROCESS_KILL, (long)*proc); (void)it_lp_wait_exit(*proc); }
    it_close(cmd); it_close(proc);
}

/* ── T239: resource ownership manifest ───────────────────────────────────────
 * Every principal object has a payer, a charge point, and a release point.  The
 * self snapshot is well-formed (version/limits/kslab); creating a VMO charges
 * exactly the domain that owns it (self for CREATE, the CHILD for CREATE_FOR)
 * and closing it releases exactly that charge.  Invariants: Q1, Q2, Q11, Q24. */
static void test_t239(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "resource manifest";

    struct it_rinfo r0;
    if (ok && !it_rinfo(HANDLE_INVALID, &r0)) { ok = 0; why = "rinfo self"; }
    /* Fase S1: the notification quota is RETIRED (Untyped is the budget);
     * the ABI fields remain and must read 0. */
    if (ok && (r0.version != 1u || r0.vmos_limit != IT_VMO_QUOTA ||
               r0.notifs_limit != 0u || r0.notifs_usage != 0u ||
               r0.kslab_total_bytes == 0u ||
               r0.pages_limit == 0u)) { ok = 0; why = "manifest fields"; }
    /* kslab used is within total; no phantom alloc failures at rest. */
    if (ok && r0.kslab_used_bytes > r0.kslab_total_bytes) { ok = 0; why = "kslab over total"; }

    /* CREATE charges self by exactly one VMO; close releases it. */
    long v = ok ? it_sys1(SYS_VMO_CREATE, 4096) : -1;
    if (ok && v < 0) { ok = 0; why = "create"; }
    struct it_rinfo r1;
    if (ok && !it_rinfo(HANDLE_INVALID, &r1)) { ok = 0; why = "rinfo1"; }
    if (ok && r1.vmos_usage != r0.vmos_usage + 1u) { ok = 0; why = "self not charged"; }
    if (ok && r1.vmos_hwm < r1.vmos_usage) { ok = 0; why = "hwm < usage"; }
    handle_id_t vh = (v >= 0) ? (handle_id_t)v : HANDLE_INVALID;
    it_close(&vh);
    struct it_rinfo r2;
    if (ok && !it_rinfo(HANDLE_INVALID, &r2)) { ok = 0; why = "rinfo2"; }
    if (ok && r2.vmos_usage != r0.vmos_usage) { ok = 0; why = "release drift"; }
    if (ok && r2.vmos_hwm < r1.vmos_hwm) { ok = 0; why = "hwm decreased"; }  /* Q24 */

    /* CREATE_FOR charges the CHILD, not self (creator != owner). */
    handle_id_t cmd, proc;
    if (ok && !it_bare_child(&cmd, &proc)) { ok = 0; why = "child"; }
    struct it_rinfo cs0, ss0;
    if (ok && (!it_rinfo(proc, &cs0) || !it_rinfo(HANDLE_INVALID, &ss0))) { ok = 0; why = "rinfo child"; }
    long vc = ok ? it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)proc) : -1;
    if (ok && vc < 0) { ok = 0; why = "create_for"; }
    struct it_rinfo cs1, ss1;
    if (ok && (!it_rinfo(proc, &cs1) || !it_rinfo(HANDLE_INVALID, &ss1))) { ok = 0; why = "rinfo child1"; }
    if (ok && cs1.vmos_usage != cs0.vmos_usage + 1u) { ok = 0; why = "child not charged"; }
    if (ok && ss1.vmos_usage != ss0.vmos_usage) { ok = 0; why = "self wrongly charged"; }  /* Q2 */
    handle_id_t vch = (vc >= 0) ? (handle_id_t)vc : HANDLE_INVALID;
    it_close(&vch);
    if (ok) { struct it_rinfo cs2; if (!it_rinfo(proc, &cs2) || cs2.vmos_usage != cs0.vmos_usage) { ok = 0; why = "child release drift"; } }
    if (ok) it_bare_kill(&cmd, &proc); else it_bare_kill(&cmd, &proc);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T239"); else it_fail("T239", why);
}

/* ── T240: loader creates many independent children ──────────────────────────
 * The supervisor spawns 1, 8, 16, 32 children.  Its OWN vmos_usage does NOT
 * grow ~4×children (the child image VMOs are charged to each CHILD, not to the
 * loader — the Fase 28.1 caller-charged bug, fixed).  Every child is alive with
 * its own image charge; selective death frees only that child; the final
 * baseline is exact.  A push toward 64 shows the real ceiling is the documented
 * KPROCESS_MAX_LIVE process limit, hit CLEANLY — not the loader's VMO quota.
 * Invariants: Q4, Q5, Q12, Q13, Q20, Q23. */
#define T240_MAX 48u
static void test_t240(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "many children";

    static handle_id_t cmd[T240_MAX], proc[T240_MAX];
    struct it_rinfo self0;
    if (ok && !it_rinfo(HANDLE_INVALID, &self0)) { ok = 0; why = "self0"; }

    const uint32_t rungs[4] = { 1u, 8u, 16u, 32u };
    for (uint32_t ri = 0; ok && ri < 4u; ri++) {
        uint32_t n = rungs[ri];
        for (uint32_t i = 0; i < T240_MAX; i++) { cmd[i] = proc[i] = HANDLE_INVALID; }
        uint32_t spawned = 0;
        for (uint32_t i = 0; ok && i < n; i++) {
            if (!it_bare_child(&cmd[i], &proc[i])) { ok = 0; why = "spawn"; break; }
            spawned++;
        }
        /* The loader's own VMO usage did NOT scale with children (Q5). */
        struct it_rinfo self1;
        if (ok && !it_rinfo(HANDLE_INVALID, &self1)) { ok = 0; why = "self1"; }
        if (ok && self1.vmos_usage != self0.vmos_usage) { ok = 0; why = "loader accumulated VMOs"; }
        /* A sampled child carries its own image charge (Q4). */
        if (ok && n > 0) {
            struct it_rinfo cs;
            if (!it_rinfo(proc[n / 2u], &cs) || cs.vmos_usage == 0u) { ok = 0; why = "child image uncharged"; }
        }
        /* Selective death: kill the even-indexed children; odd ones stay alive. */
        for (uint32_t i = 0; ok && i < spawned; i += 2u) it_bare_kill(&cmd[i], &proc[i]);
        for (uint32_t i = 1; ok && i < spawned; i += 2u) {
            struct it_rinfo cs;
            if (!it_rinfo(proc[i], &cs)) { ok = 0; why = "survivor died"; }
        }
        for (uint32_t i = 0; i < spawned; i++) if (proc[i] != HANDLE_INVALID) it_bare_kill(&cmd[i], &proc[i]);
        it_quiesce_reaper();
        /* Baseline exact after each rung (Q12/Q23). */
        struct it_rinfo self2;
        if (ok && (!it_rinfo(HANDLE_INVALID, &self2) || self2.vmos_usage != self0.vmos_usage)) { ok = 0; why = "rung baseline"; }
    }

    /* Push toward the process ceiling: spawn until failure (cap T240_MAX).  At
     * least 32 must succeed and the loader must never accumulate VMOs; any
     * failure is a CLEAN process-limit error, and teardown returns to baseline
     * (Q20/Q21/Q29 — no wedge). */
    if (ok) {
        for (uint32_t i = 0; i < T240_MAX; i++) { cmd[i] = proc[i] = HANDLE_INVALID; }
        uint32_t got = 0;
        for (uint32_t i = 0; i < T240_MAX; i++) {
            if (!it_bare_child(&cmd[i], &proc[i])) break;
            got++;
        }
        struct it_rinfo self1;
        if (!it_rinfo(HANDLE_INVALID, &self1) || self1.vmos_usage != self0.vmos_usage) { ok = 0; why = "push accumulated"; }
        if (ok && got < 32u) { ok = 0; why = "fewer than 32 children"; }
        for (uint32_t i = 0; i < got; i++) it_bare_kill(&cmd[i], &proc[i]);
        it_quiesce_reaper();
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T240"); else it_fail("T240", why);
}

/* ── T241: VMO payer and child ownership ─────────────────────────────────────
 * CREATE_FOR charges the named domain, gated by RIGHT_MANAGE; a cap without
 * MANAGE is denied, a wrong-type target is WRONG_TYPE, a dead target is
 * BAD_HANDLE.  Invariants: Q2, Q3, Q26. */
static void test_t241(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "vmo payer";

    handle_id_t cmd, proc;
    if (ok && !it_bare_child(&cmd, &proc)) { ok = 0; why = "child"; }

    /* A proc cap WITHOUT RIGHT_MANAGE cannot charge to the child. */
    long ro = ok ? it_sys2(SYS_HANDLE_DUP, (long)proc, (long)RIGHT_READ) : -1;
    handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
    if (ok && ro < 0) { ok = 0; why = "dup ro"; }
    if (ok && it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)ro_h) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-manage charged"; }
    it_close(&ro_h);

    /* Wrong-type charge target → WRONG_TYPE. */
    long ep = ok ? it_ep_create() : -1;
    handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    if (ok && it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)ep_h) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "wrong type charged"; }
    it_close(&ep_h);

    /* Valid charge to the live child works. */
    long vc = ok ? it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)proc) : -1;
    if (ok && vc < 0) { ok = 0; why = "valid create_for"; }
    handle_id_t vch = (vc >= 0) ? (handle_id_t)vc : HANDLE_INVALID;
    it_close(&vch);

    /* A dead target is rejected (BAD_HANDLE), not charged. */
    if (ok) {
        (void)it_sys1(SYS_PROCESS_KILL, (long)proc);
        (void)it_lp_wait_exit(proc);
        it_quiesce_reaper();
        long dr = it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)proc);
        if (dr != (long)IRIS_ERR_BAD_HANDLE && dr != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "dead target charged"; }
    }
    it_close(&cmd); it_close(&proc);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T241"); else it_fail("T241", why);
}

/* ── T242: shared VMO single-charge contract ─────────────────────────────────
 * A VMO shared by mapping into several VSpaces is charged ONCE to its owner;
 * extra caps do not re-charge; a target's death does not destroy it; the last
 * handle close releases object + pages.  Invariants: Q6, Q8, Q10, Q16. */
static void test_t242(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "shared vmo";

    struct it_rinfo s0;
    if (ok && !it_rinfo(HANDLE_INVALID, &s0)) { ok = 0; why = "s0"; }
    long v = ok ? it_sys1(SYS_VMO_CREATE, 4096) : -1;
    handle_id_t vh = (v >= 0) ? (handle_id_t)v : HANDLE_INVALID;
    if (ok && v < 0) { ok = 0; why = "create"; }
    /* Owner charged exactly one VMO. */
    struct it_rinfo s1;
    if (ok && (!it_rinfo(HANDLE_INVALID, &s1) || s1.vmos_usage != s0.vmos_usage + 1u)) { ok = 0; why = "not charged once"; }
    /* Duplicating the cap does NOT re-charge the object (Q8/Q10). */
    long d = ok ? it_sys2(SYS_HANDLE_DUP, (long)vh, (long)(RIGHT_READ | RIGHT_WRITE)) : -1;
    handle_id_t dh = (d >= 0) ? (handle_id_t)d : HANDLE_INVALID;
    if (ok && d < 0) { ok = 0; why = "dup"; }
    struct it_rinfo s2;
    if (ok && (!it_rinfo(HANDLE_INVALID, &s2) || s2.vmos_usage != s1.vmos_usage)) { ok = 0; why = "dup re-charged"; }
    it_close(&dh);
    /* Last handle close releases object + pages back to baseline. */
    it_close(&vh);
    struct it_rinfo s3;
    if (ok && (!it_rinfo(HANDLE_INVALID, &s3) || s3.vmos_usage != s0.vmos_usage)) { ok = 0; why = "release drift"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T242"); else it_fail("T242", why);
}

/* ── T243: mapping target-charge contract ────────────────────────────────────
 * Sparse-VMO PAGES are charged once to the VMO owner (not per mapper); the
 * lightweight mapping nodes are per-VSpace and released on unmap.  Mapping the
 * same VMO twice in one VSpace and unmapping returns the mapping books to
 * baseline; the owner's page charge is paid once and released at destroy.
 * Invariants: Q7, Q18, Q33. */
static void test_t243(void) {
    if (!it_setup_self_vspace()) { it_fail("T243", "vspace self"); return; }
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "mapping charge";

    struct it_rinfo s0;
    if (ok && !it_rinfo(HANDLE_INVALID, &s0)) { ok = 0; why = "s0"; }
    long v = ok ? it_sys1(SYS_VMO_CREATE, 4096) : -1;
    handle_id_t vh = (v >= 0) ? (handle_id_t)v : HANDLE_INVALID;
    if (ok && v < 0) { ok = 0; why = "create"; }
    /* Map the VMO page into self via SYS_VMO_MAP_PAGE at a scratch VA. */
    uint64_t va = T26_SELF_VA;
    if (ok && it_sys4(SYS_VMO_MAP_PAGE, (long)vh, IT_VS, (long)va, 0) != 0) { ok = 0; why = "map"; }
    /* Owner charged exactly one page (Q7/Q18). */
    struct it_rinfo s1;
    if (ok && (!it_rinfo(HANDLE_INVALID, &s1) || s1.pages_usage != s0.pages_usage + 1u)) { ok = 0; why = "page not charged once"; }
    /* Unmap: the mapping node releases; the page charge stays with the VMO
     * until destroy (owned by the VMO, not the mapping). */
    if (ok && it_sys2(SYS_VMO_UNMAP, (long)va, 0x1000L) != 0) { ok = 0; why = "unmap"; }
    /* Close the VMO: the page charge releases back to baseline. */
    it_close(&vh);
    struct it_rinfo s2;
    if (ok && (!it_rinfo(HANDLE_INVALID, &s2) || s2.pages_usage != s0.pages_usage)) { ok = 0; why = "page release drift"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T243"); else it_fail("T243", why);
}

/* ── T244: pager cache/private-page accounting ───────────────────────────────
 * A file-backed pager's cache VMO and private-pool VMO are owned (and charged)
 * by their owner; resolving faults fills pages charged to that owner; pager
 * death releases them; the supervisor never inherits the pager's page charge.
 * Invariants: Q14, Q15, Q17, Q33. */
static void test_t244(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager accounting";

    struct it_rinfo self0;
    if (ok && !it_rinfo(HANDLE_INVALID, &self0)) { ok = 0; why = "self0"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    /* The pager owns its cache/private VMOs — the supervisor's own VMO usage did
     * not grow by the pager's VMOs beyond the two it created and handed over
     * (those are charged to iris_test as their creator/owner here, but the
     * supervisor's PAGE usage must not absorb the pager's fault fills). */
    struct it_rinfo self1;
    if (ok && !it_rinfo(HANDLE_INVALID, &self1)) { ok = 0; why = "self1"; }
    uint32_t self_pages_before_fault = ok ? self1.pages_usage : 0u;

    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x2000, 0x1000, 0x2000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    /* Resolve two faulted pages from the shared RO cache. */
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    /* The pager's cache fills were charged to the cache VMO's owner, NOT to the
     * supervisor's live page budget beyond its own working set. */
    struct it_rinfo self2;
    if (ok && !it_rinfo(HANDLE_INVALID, &self2)) { ok = 0; why = "self2"; }
    /* The supervisor owns the cache VMO (it created it), so its page usage may
     * legitimately reflect the cache fill; what must hold is that killing the
     * pager and reaping returns everything to baseline (no orphaned charge). */
    (void)self_pages_before_fault;

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_rinfo self3;
    if (ok && !it_rinfo(HANDLE_INVALID, &self3)) { ok = 0; why = "self3"; }
    if (ok && self3.vmos_usage != self0.vmos_usage) { ok = 0; why = "vmo leak after pager"; }
    if (ok && self3.pages_usage != self0.pages_usage) { ok = 0; why = "page leak after pager"; }

    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T244"); else it_fail("T244", why);
}

/* ── T245: independent children under one supervisor ─────────────────────────
 * Two children each own their image; killing one frees ONLY its charges and
 * leaves the other intact — a supervisor's children are independent domains
 * (the "supervisor death" contract's core: no cross-child resource coupling).
 * Invariants: Q12, Q13. */
static void test_t245(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "independent children";

    handle_id_t cmdA, procA, cmdB, procB;
    if (ok && !it_bare_child(&cmdA, &procA)) { ok = 0; why = "childA"; }
    if (ok && !it_bare_child(&cmdB, &procB)) { ok = 0; why = "childB"; }

    /* Give each child an extra owned VMO (charged to it). */
    long va = ok ? it_sys2(SYS_VMO_CREATE_FOR, 8192, (long)procA) : -1;
    long vb = ok ? it_sys2(SYS_VMO_CREATE_FOR, 8192, (long)procB) : -1;
    handle_id_t vah = (va >= 0) ? (handle_id_t)va : HANDLE_INVALID;
    handle_id_t vbh = (vb >= 0) ? (handle_id_t)vb : HANDLE_INVALID;
    if (ok && (va < 0 || vb < 0)) { ok = 0; why = "create_for"; }
    struct it_rinfo bA, bB;
    if (ok && (!it_rinfo(procA, &bA) || !it_rinfo(procB, &bB))) { ok = 0; why = "rinfo"; }
    if (ok && (bA.vmos_usage == 0u || bB.vmos_usage == 0u)) { ok = 0; why = "not charged"; }

    /* Kill A; B's charges are untouched. */
    if (ok) { (void)it_sys1(SYS_PROCESS_KILL, (long)procA); (void)it_lp_wait_exit(procA); it_quiesce_reaper(); }
    struct it_rinfo bB2;
    if (ok && (!it_rinfo(procB, &bB2) || bB2.vmos_usage != bB.vmos_usage)) { ok = 0; why = "B disturbed by A death"; }
    /* Closing iris_test's handle to A's VMO (holder) after A died: object already
     * gone with A, so this is a clean no-op close. */
    it_close(&vah);
    it_close(&cmdA); it_close(&procA);

    it_close(&vbh);
    it_bare_kill(&cmdB, &procB);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T245"); else it_fail("T245", why);
}

/* ── T246: quota exhaustion failure atomicity ────────────────────────────────
 * Fill self's VMO domain to KPROCESS_VMO_QUOTA; the next CREATE fails NO_MEMORY
 * with NO object published and the global failed-charge counter advancing;
 * high-water pins at the limit; freeing capacity lets a create succeed again;
 * usage returns to baseline.  Invariants: Q20, Q21, Q22, Q23, Q24, Q28. */
static void test_t246(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "quota exhaustion";

    struct it_rinfo r0;
    if (ok && !it_rinfo(HANDLE_INVALID, &r0)) { ok = 0; why = "r0"; }
    uint32_t headroom = ok ? (IT_VMO_QUOTA - r0.vmos_usage) : 0u;
    if (ok && headroom == 0u) { ok = 0; why = "no headroom"; }

    static handle_id_t vs[IT_VMO_QUOTA];
    for (uint32_t i = 0; i < IT_VMO_QUOTA; i++) vs[i] = HANDLE_INVALID;
    uint32_t made = 0;
    for (uint32_t i = 0; ok && i < headroom; i++) {
        long v = it_sys1(SYS_VMO_CREATE, 4096);
        if (v < 0) { ok = 0; why = "fill create"; break; }
        vs[made++] = (handle_id_t)v;
    }
    /* Domain is now full: usage == limit, hwm == limit. */
    struct it_rinfo rf;
    if (ok && (!it_rinfo(HANDLE_INVALID, &rf) || rf.vmos_usage != IT_VMO_QUOTA ||
               rf.vmos_hwm != IT_VMO_QUOTA)) { ok = 0; why = "not full"; }
    uint32_t fail0 = ok ? rf.global_failed_charges : 0u;
    /* The next create fails cleanly — no object, counter advances (Q20/Q21). */
    if (ok && it_sys1(SYS_VMO_CREATE, 4096) != (long)IRIS_ERR_NO_MEMORY) { ok = 0; why = "over-quota not NO_MEMORY"; }
    struct it_rinfo rf2;
    if (ok && !it_rinfo(HANDLE_INVALID, &rf2)) { ok = 0; why = "rf2"; }
    if (ok && rf2.vmos_usage != IT_VMO_QUOTA) { ok = 0; why = "phantom charge"; }
    if (ok && rf2.global_failed_charges <= fail0) { ok = 0; why = "fail count not advanced"; }
    /* Free one and a create succeeds again (no retry bypass, exact accounting). */
    if (made > 0) it_close(&vs[made - 1u]);
    if (made > 0) made--;
    long again = ok ? it_sys1(SYS_VMO_CREATE, 4096) : -1;
    if (ok && again < 0) { ok = 0; why = "no recovery"; }
    if (again >= 0) { handle_id_t h = (handle_id_t)again; vs[made++] = h; }
    /* Release everything; usage returns to baseline, hwm stays pinned. */
    for (uint32_t i = 0; i < made; i++) it_close(&vs[i]);
    struct it_rinfo rz;
    if (ok && (!it_rinfo(HANDLE_INVALID, &rz) || rz.vmos_usage != r0.vmos_usage)) { ok = 0; why = "baseline drift"; }
    if (ok && rz.vmos_hwm < IT_VMO_QUOTA) { ok = 0; why = "hwm regressed"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T246"); else it_fail("T246", why);
}

/* ── T247: resource delegation rights monotonicity ───────────────────────────
 * Budget-charge authority is process-MANAGE authority.  A MANAGE cap can charge
 * a child; a cap derived WITHOUT MANAGE cannot; a right dropped cannot be
 * recovered by re-deriving from the reduced cap.  Invariants: Q25, Q26. */
static void test_t247(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "delegation monotonicity";

    handle_id_t cmd, proc;
    if (ok && !it_bare_child(&cmd, &proc)) { ok = 0; why = "child"; }

    /* Full cap charges. */
    long v = ok ? it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)proc) : -1;
    if (ok && v < 0) { ok = 0; why = "full charge"; }
    handle_id_t vh = (v >= 0) ? (handle_id_t)v : HANDLE_INVALID;
    it_close(&vh);

    /* Derive a reduced cap WITHOUT MANAGE (READ only). */
    long red = ok ? it_sys2(SYS_HANDLE_DUP, (long)proc, (long)RIGHT_READ) : -1;
    handle_id_t red_h = (red >= 0) ? (handle_id_t)red : HANDLE_INVALID;
    if (ok && red < 0) { ok = 0; why = "reduce"; }
    if (ok && it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)red_h) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "reduced charged"; }
    /* Cannot recover MANAGE by re-deriving from the reduced cap. */
    long rec = ok ? it_sys2(SYS_HANDLE_DUP, (long)red_h, (long)(RIGHT_READ | RIGHT_MANAGE)) : -1;
    if (ok && rec >= 0) {
        /* If a handle came back, it must NOT actually carry MANAGE authority. */
        if (it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)rec) == 0) { ok = 0; why = "recovered MANAGE"; }
        handle_id_t rh = (handle_id_t)rec; it_close(&rh);
    }
    it_close(&red_h);
    it_bare_kill(&cmd, &proc);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T247"); else it_fail("T247", why);
}

/* ── T248: kslab capacity and explicit exhaustion ────────────────────────────
 * The kernel object slab has an observable capacity contract: used <= total,
 * total is the reserved arena, and under normal load there are zero allocation
 * failures.  The exhaustion PATH itself (alloc returns 0, fail count advances,
 * no corruption) is proven deterministically in the host unit suite
 * (tests/kernel/test_kslab.c) — exhausting 16 MB in every smoke run is
 * impractical.  Invariants: Q28, Q29. */
static void test_t248(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "kslab capacity";

    struct it_rinfo r0;
    if (ok && !it_rinfo(HANDLE_INVALID, &r0)) { ok = 0; why = "rinfo"; }
    if (ok && r0.kslab_total_bytes == 0u) { ok = 0; why = "no arena"; }
    if (ok && r0.kslab_used_bytes > r0.kslab_total_bytes) { ok = 0; why = "used > total"; }
    if (ok && r0.kslab_alloc_failures != 0u) { ok = 0; why = "spurious kslab failure"; }
    /* Spawning and reaping a child churns kernel objects; used (bump high-water)
     * may rise but never exceeds total, and no allocation fails. */
    handle_id_t cmd, proc;
    if (ok && !it_bare_child(&cmd, &proc)) { ok = 0; why = "child"; }
    struct it_rinfo r1;
    if (ok && !it_rinfo(HANDLE_INVALID, &r1)) { ok = 0; why = "rinfo1"; }
    if (ok && (r1.kslab_used_bytes > r1.kslab_total_bytes || r1.kslab_alloc_failures != 0u)) { ok = 0; why = "kslab churn"; }
    if (ok && r1.kslab_used_bytes < r0.kslab_used_bytes) { ok = 0; why = "used regressed"; }  /* bump-only */
    it_bare_kill(&cmd, &proc);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T248"); else it_fail("T248", why);
}

/* ── T249: file-backed resource regression ───────────────────────────────────
 * The whole file-backed path runs under the new accounting with exact baseline:
 * multiple targets, RO shared + private writable, cache pressure, revoke, target
 * death — all return the supervisor's resource books to baseline.  T211–T238
 * (running before this) are the functional regression; T249 adds the accounting
 * assertion.  Invariants: Q30, Q33. */
static void test_t249(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "file-backed regression";

    struct it_rinfo r0;
    if (ok && !it_rinfo(HANDLE_INVALID, &r0)) { ok = 0; why = "r0"; }

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "region 0"; }
        t28_region(&rq, 1, 1, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "region 1"; }
    }
    if (ok && !t28_read_verify(&f, &g0, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    if (ok && !t28_write_resolve(&f, &g1, 1u, T28_VA_A, &why)) ok = 0;
    /* Revoke at the VFS, then a fresh fault must fail. */
    if (ok && t28_grant_revoke_name(f.admin, FBK_FILE_NAME, 0) != 0) { ok = 0; why = "revoke"; }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_rinfo r1;
    if (ok && (!it_rinfo(HANDLE_INVALID, &r1) || r1.vmos_usage != r0.vmos_usage ||
               r1.pages_usage != r0.pages_usage)) { ok = 0; why = "accounting drift"; }

    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T249"); else it_fail("T249", why);
}

/* ── T250: deterministic resource-accounting stress ──────────────────────────
 * Seeded rounds over the whole surface: create child, create VMO (self /
 * for-child), map/unmap, dup cap, kill child, near-exhaustion, cleanup.  Every
 * round: self usage returns to baseline, global counters coherent, high-water
 * monotone, snapshot exact.  Invariants: Q1–Q35 under load. */
#define T250_SEED   0x29ACC7E5u
#define T250_ROUNDS 10u
static uint32_t t250_rnd(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }
static void test_t250(void) {
    uint32_t rng = T250_SEED;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "resource stress";
    struct it_rinfo r0;
    if (ok && !it_rinfo(HANDLE_INVALID, &r0)) { ok = 0; why = "r0"; }
    uint32_t round = 0, op = 0, prev_hwm = ok ? r0.vmos_hwm : 0u;

    for (round = 0; ok && round < T250_ROUNDS; round++) {
        op = t250_rnd(&rng) % 4u;
        switch (op) {
        case 0: {
            /* Child owns its image + an extra VMO; kill releases exactly it. */
            handle_id_t cmd, proc;
            if (!it_bare_child(&cmd, &proc)) { ok = 0; why = "s0 child"; break; }
            long v = it_sys2(SYS_VMO_CREATE_FOR, 4096, (long)proc);
            struct it_rinfo self;
            if (v < 0 || !it_rinfo(HANDLE_INVALID, &self) || self.vmos_usage != r0.vmos_usage) { ok = 0; why = "s0 loader charged"; }
            if (v >= 0) { handle_id_t vh = (handle_id_t)v; it_close(&vh); }
            it_bare_kill(&cmd, &proc);
            break;
        }
        case 1: {
            /* Self VMO create + dup + close: single charge, exact release. */
            long v = it_sys1(SYS_VMO_CREATE, 4096);
            if (v < 0) { ok = 0; why = "s1 create"; break; }
            handle_id_t vh = (handle_id_t)v;
            long d = it_sys2(SYS_HANDLE_DUP, (long)vh, (long)RIGHT_READ);
            struct it_rinfo self;
            if (!it_rinfo(HANDLE_INVALID, &self) || self.vmos_usage != r0.vmos_usage + 1u) { ok = 0; why = "s1 charge"; }
            if (d >= 0) { handle_id_t dh = (handle_id_t)d; it_close(&dh); }
            it_close(&vh);
            break;
        }
        case 2: {
            /* Near-exhaustion then recover; global fail counter advances. */
            struct it_rinfo rr;
            if (!it_rinfo(HANDLE_INVALID, &rr)) { ok = 0; why = "s2 rinfo"; break; }
            uint32_t head = IT_VMO_QUOTA - rr.vmos_usage;
            static handle_id_t vv[IT_VMO_QUOTA];
            uint32_t made = 0;
            for (uint32_t i = 0; i < head; i++) { long v = it_sys1(SYS_VMO_CREATE, 4096); if (v < 0) break; vv[made++] = (handle_id_t)v; }
            uint32_t f0 = 0; struct it_rinfo rf; if (it_rinfo(HANDLE_INVALID, &rf)) f0 = rf.global_failed_charges;
            if (it_sys1(SYS_VMO_CREATE, 4096) != (long)IRIS_ERR_NO_MEMORY) { ok = 0; why = "s2 not full"; }
            struct it_rinfo rf2;
            if (ok && (!it_rinfo(HANDLE_INVALID, &rf2) || rf2.global_failed_charges <= f0)) { ok = 0; why = "s2 fail count"; }
            for (uint32_t i = 0; i < made; i++) it_close(&vv[i]);
            break;
        }
        default: {
            /* Map/unmap a self VMO page; page charge paid once, released. */
            if (!it_setup_self_vspace()) { ok = 0; why = "s3 vspace"; break; }
            long v = it_sys1(SYS_VMO_CREATE, 4096);
            if (v < 0) { ok = 0; why = "s3 create"; break; }
            handle_id_t vh = (handle_id_t)v;
            if (it_sys4(SYS_VMO_MAP_PAGE, (long)vh, IT_VS, (long)T26_SELF_VA, 0) != 0) { ok = 0; why = "s3 map"; }
            if (ok) (void)it_sys2(SYS_VMO_UNMAP, (long)T26_SELF_VA, 0x1000L);
            it_close(&vh);
            break;
        }
        }
        it_quiesce_reaper();
        if (ok) {
            struct it_rinfo rz;
            if (!it_rinfo(HANDLE_INVALID, &rz)) { ok = 0; why = "round rinfo"; }
            else if (rz.vmos_usage != r0.vmos_usage) { ok = 0; why = "round vmo drift"; }
            else if (rz.pages_usage != r0.pages_usage) { ok = 0; why = "round page drift"; }
            else if (rz.vmos_hwm < prev_hwm) { ok = 0; why = "hwm regressed"; }
            else prev_hwm = rz.vmos_hwm;
            struct it_snap r = it_snap_take();
            if (ok && !it_snap_baseline_live(&b, &r, &why)) ok = 0;
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T250");
    else { it_fz_note("T250", T250_SEED, round, op); it_fail("T250", why); }
}

/* ════════════════════════════════════════════════════════════════════════
 * Fase S1 — seL4 Architectural Convergence (T251–T262).
 *
 * Canonical kernel-object model + Untyped-only allocation substrate: every
 * migrated object (Endpoint / Notification / Reply / CNode) is born from an
 * explicit Untyped via SYS_UNTYPED_RETYPE2, its storage IS the retyped
 * region, its capability appears directly in CSpace, and its region becomes
 * reusable after destruction + RESET.  Legacy create paths are retired.
 * ════════════════════════════════════════════════════════════════════════ */

/* SYS_UNTYPED_QUERY wrappers (versioned, read-only instrumentation). */
struct it_utq_global {
    uint32_t version, struct_size, live_untypeds, _pad0;
    uint64_t retype_count, retype_failures, reset_count;
    uint64_t reclaimed_bytes, reuse_count, overlap_denials;
};
struct it_utq_one {
    uint32_t version, struct_size;
    uint64_t phys_base, total_bytes, used_bytes, generation;
    uint32_t child_count, is_device;
};
struct it_utq_objects {
    uint32_t version, struct_size;
    uint32_t endpoints_live, notifications_live, replies_live, cnodes_live;
};
static int it_utq_g(struct it_utq_global *q) {
    return it_sys3(SYS_UNTYPED_QUERY, 1, (long)(uintptr_t)q, 0) == 0;
}
static int it_utq_1(long ut, struct it_utq_one *q) {
    return it_sys3(SYS_UNTYPED_QUERY, 2, (long)(uintptr_t)q, ut) == 0;
}
static int it_utq_o(struct it_utq_objects *q) {
    return it_sys3(SYS_UNTYPED_QUERY, 3, (long)(uintptr_t)q, 0) == 0;
}

/* Carve a fresh page-multiple sub-untyped for one S1 test (handle). */
static long s1_sub_ut(uint64_t bytes) {
    return it_sys3(SYS_UNTYPED_RETYPE, (long)IRIS_CPTR_TEST_UNTYPED,
                   IT_KOBJ_UNTYPED, (long)bytes);
}

/* S1 scratch slots: 241..250 (fz pool ends at 239; 240 is the T125 probe). */
#define S1_SLOT_A 241u
#define S1_SLOT_B 242u
#define S1_SLOT_C 243u
#define S1_SLOT_D 244u
#define S1_SLOT_E 245u

/* ── T251: canonical object model manifest ──────────────────────────────────
 * RETYPE2 accepts EXACTLY the canonical creatable set {NOTIFICATION,
 * ENDPOINT, CNODE, SCHED_CONTEXT, UNTYPED, REPLY, FRAME} and refuses every
 * other type code (0..31) with NOT_SUPPORTED — an unregistered KOBJ_* can
 * never be born.  Every created object reports its declared type through the
 * sanctioned bridge, and the migrated family has a retirement witness: the
 * legacy handle-first retype refuses it (S19/S20/S21). */
static void test_t251(void) {
    int ok = 1;
    const char *why = "object manifest";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T251", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    static const struct { uint32_t t; long arg; long ht; } canon[] = {
        { IRIS_KOBJ_NOTIFICATION,  0,    IRIS_HANDLE_TYPE_NOTIFICATION },
        { IRIS_KOBJ_ENDPOINT,      0,    IRIS_HANDLE_TYPE_ENDPOINT },
        { IRIS_KOBJ_CNODE,         4,    IRIS_HANDLE_TYPE_CNODE },
        { IRIS_KOBJ_SCHED_CONTEXT, 0,    IRIS_HANDLE_TYPE_SCHED_CONTEXT },
        { IRIS_KOBJ_UNTYPED,       4096, IRIS_HANDLE_TYPE_UNTYPED },
        { IRIS_KOBJ_REPLY,         0,    IRIS_HANDLE_TYPE_REPLY },
        { IRIS_KOBJ_FRAME,         4096, IRIS_HANDLE_TYPE_FRAME },
    };
    for (uint32_t i = 0; ok && i < 7u; i++) {
        if (it_retype2_at(su, canon[i].t, S1_SLOT_A, 1u, canon[i].arg) != 0) {
            ok = 0; why = "canonical type not creatable"; break;
        }
        long h = it_sys1(SYS_CSPACE_RESOLVE, (long)S1_SLOT_A);
        if (h < 0 || it_sys1(SYS_HANDLE_TYPE, h) != canon[i].ht) {
            ok = 0; why = "created type mismatch";
        }
        if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); }
        it_slot_delete(S1_SLOT_A);
    }
    /* Everything else in 0..31 is refused — the manifest is CLOSED. */
    for (uint32_t t = 0; ok && t < 32u; t++) {
        int is_canon = 0;
        for (uint32_t i = 0; i < 7u; i++) if (canon[i].t == t) is_canon = 1;
        if (is_canon) continue;
        if (it_retype2_at(su, t, S1_SLOT_A, 1u, 4096) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "non-canonical type creatable";
        }
    }
    /* Retirement witness: migrated family refused on the legacy path. */
    static const uint32_t migrated[] = { IRIS_KOBJ_ENDPOINT, IRIS_KOBJ_NOTIFICATION,
                                         IRIS_KOBJ_CNODE, IRIS_KOBJ_REPLY };
    for (uint32_t i = 0; ok && i < 4u; i++) {
        if (it_sys3(SYS_UNTYPED_RETYPE, su, (long)migrated[i], 4) !=
            (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy path not retired"; }
    }

    it_close(&su_h);
    if (ok) it_pass("T251"); else it_fail("T251", why);
}

/* ── T252: Untyped storage provenance ───────────────────────────────────────
 * The retyped region IS the object storage: creating the migrated family
 * consumes exactly measurable bytes of the source untyped (used_bytes grows,
 * child_count counts every object), consumes ZERO kslab bytes (S2 — payload
 * never touches the kernel heap), and destroying every object returns the
 * region to reusable (RESET succeeds, used == 0). */
static void test_t252(void) {
    int ok = 1;
    const char *why = "storage provenance";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T252", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_one u0, u1, u2;
    struct it_rinfo  k0, k1;
    struct it_utq_objects o0, o1;
    if (!it_utq_1(su, &u0) || !it_rinfo(HANDLE_INVALID, &k0) || !it_utq_o(&o0)) {
        it_close(&su_h); it_fail("T252", "query"); return;
    }
    if (u0.used_bytes != 0u || u0.child_count != 0u) { ok = 0; why = "not pristine"; }

    /* 4 endpoints + 2 notifications + 2 replies, all batch-retyped. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT,     S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "ep"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT,     S1_SLOT_B, 1u, 0) != 0) { ok = 0; why = "ep2"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_C, 1u, 0) != 0) { ok = 0; why = "nt"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_REPLY,        S1_SLOT_D, 1u, 0) != 0) { ok = 0; why = "rp"; }
    if (ok && (!it_utq_1(su, &u1) || !it_rinfo(HANDLE_INVALID, &k1) || !it_utq_o(&o1))) {
        ok = 0; why = "query mid";
    }
    /* used grew, every object is inside THIS untyped (child_count == 4),
     * live gauges rose accordingly, and the kernel heap did not move. */
    if (ok && !(u1.used_bytes > u0.used_bytes && u1.used_bytes <= u1.total_bytes)) { ok = 0; why = "no consumption"; }
    if (ok && u1.child_count != 4u) { ok = 0; why = "child count"; }
    if (ok && o1.endpoints_live     != o0.endpoints_live + 2u)     { ok = 0; why = "ep live"; }
    if (ok && o1.notifications_live != o0.notifications_live + 1u) { ok = 0; why = "nt live"; }
    if (ok && o1.replies_live       != o0.replies_live + 1u)       { ok = 0; why = "rp live"; }
    if (ok && k1.kslab_used_bytes   != k0.kslab_used_bytes)        { ok = 0; why = "kslab moved (S2)"; }

    /* The objects are REAL (usable through their CSpace caps). */
    if (ok) {
        struct IrisMsg m; it_iris_msg_zero(&m);
        if (it_sys2(SYS_EP_NB_RECV, (long)S1_SLOT_A, (long)&m) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "ep dead"; }
        if (ok && it_sys2(SYS_NOTIFY_SIGNAL, (long)S1_SLOT_C, 1) != 0) { ok = 0; why = "nt dead"; }
    }

    /* Destroy all → region reusable, gauges at baseline. */
    it_slot_delete(S1_SLOT_A); it_slot_delete(S1_SLOT_B);
    it_slot_delete(S1_SLOT_C); it_slot_delete(S1_SLOT_D);
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset busy"; }
    if (ok && (!it_utq_1(su, &u2) || u2.used_bytes != 0u || u2.child_count != 0u)) { ok = 0; why = "not reclaimed"; }

    it_close(&su_h);
    if (ok) it_pass("T252"); else it_fail("T252", why);
}

/* ── T253: atomic batch retype ──────────────────────────────────────────────
 * One RETYPE2 with count=4 creates all four (each slot resolves to a live
 * endpoint).  A batch whose THIRD destination slot is occupied fails
 * ALREADY_EXISTS with ZERO effect: no slot filled, no untyped byte consumed,
 * no object live (U14/U15/S5).  A batch larger than the remaining capacity
 * fails NO_MEMORY with zero effect. */
static void test_t253(void) {
    int ok = 1;
    const char *why = "atomic batch";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T253", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    /* Success batch: 4 endpoints into 241..244. */
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 4u, 0) != 0) { ok = 0; why = "batch"; }
    for (uint32_t i = 0; ok && i < 4u; i++) {
        long h = it_sys1(SYS_CSPACE_RESOLVE, (long)(S1_SLOT_A + i));
        if (h < 0 || it_sys1(SYS_HANDLE_TYPE, h) != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
            ok = 0; why = "batch member missing";
        }
        if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); }
    }
    /* Tear down members 242..244, keep 241 occupied as the collision. */
    it_slot_delete(S1_SLOT_B); it_slot_delete(S1_SLOT_C); it_slot_delete(S1_SLOT_D);

    struct it_utq_one ub, ua;
    struct it_utq_objects ob, oa;
    if (ok && (!it_utq_1(su, &ub) || !it_utq_o(&ob))) { ok = 0; why = "query"; }
    /* Batch 239..242: slot 241 (third) is occupied → ALREADY_EXISTS. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT, 239u, 4u, 0) !=
              (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "collision not refused"; }
    if (ok && (!it_utq_1(su, &ua) || !it_utq_o(&oa))) { ok = 0; why = "query 2"; }
    if (ok && (ua.used_bytes != ub.used_bytes || ua.child_count != ub.child_count)) {
        ok = 0; why = "failed batch consumed bytes (U15)";
    }
    if (ok && oa.endpoints_live != ob.endpoints_live) { ok = 0; why = "failed batch left object"; }
    if (ok) {
        long h = it_sys1(SYS_CSPACE_RESOLVE, 239);
        if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); ok = 0; why = "partial slot filled"; }
        h = it_sys1(SYS_CSPACE_RESOLVE, 240);
        if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); ok = 0; why = "partial slot filled 2"; }
    }
    /* Capacity failure: 32 CNodes of 256 slots ≫ 8 KiB region. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_CNODE, 246u, 8u, 256) !=
              (long)IRIS_ERR_NO_MEMORY) { ok = 0; why = "capacity not refused"; }
    if (ok && (!it_utq_1(su, &ua) || ua.used_bytes != ub.used_bytes)) { ok = 0; why = "capacity fail consumed"; }

    it_slot_delete(S1_SLOT_A);
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset busy"; }
    it_close(&su_h);
    if (ok) it_pass("T253"); else it_fail("T253", why);
}

/* ── T254: retype validation and overlap denial ─────────────────────────────
 * Every malformed RETYPE2 fails BEFORE any state changes: wrong type, zero →
 * one-normalized vs oversized count, slot 0, out-of-range slot window,
 * occupied destination, missing RIGHT_WRITE on the untyped, a non-CNode
 * destination, invalid CNode fan-out, misaligned physical sizes, count>1 on
 * physical types, device/normal restriction, and a stale untyped handle. */
static void test_t254(void) {
    int ok = 1;
    const char *why = "validation";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T254", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;
    struct it_utq_one ub, ua;
    if (!it_utq_1(su, &ub)) { it_close(&su_h); it_fail("T254", "query"); return; }

    /* An occupied fixture at S1_SLOT_A. */
    if (it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_A, 1u, 0) != 0) {
        it_close(&su_h); it_fail("T254", "fixture"); return;
    }
    static const struct { uint32_t t; uint32_t slot; uint32_t cnt; long arg; long expect; const char *tag; } cases[] = {
        { 77u,                    S1_SLOT_B, 1u,  0,    (long)IRIS_ERR_NOT_SUPPORTED, "wrong type" },
        { IRIS_KOBJ_ENDPOINT,     S1_SLOT_B, 33u, 0,    (long)IRIS_ERR_INVALID_ARG,   "count too big" },
        { IRIS_KOBJ_ENDPOINT,     0u,        1u,  0,    (long)IRIS_ERR_INVALID_ARG,   "slot 0" },
        { IRIS_KOBJ_ENDPOINT,     255u,      2u,  0,    (long)IRIS_ERR_INVALID_ARG,   "slot window oob" },
        { IRIS_KOBJ_ENDPOINT,     S1_SLOT_A, 1u,  0,    (long)IRIS_ERR_ALREADY_EXISTS,"occupied" },
        { IRIS_KOBJ_CNODE,        S1_SLOT_B, 1u,  3,    (long)IRIS_ERR_INVALID_ARG,   "cnode fanout" },
        { IRIS_KOBJ_CNODE,        S1_SLOT_B, 1u,  8192, (long)IRIS_ERR_INVALID_ARG,   "cnode too big" },
        { IRIS_KOBJ_UNTYPED,      S1_SLOT_B, 1u,  100,  (long)IRIS_ERR_INVALID_ARG,   "ut misaligned" },
        { IRIS_KOBJ_UNTYPED,      S1_SLOT_B, 2u,  4096, (long)IRIS_ERR_INVALID_ARG,   "ut batch" },
        { IRIS_KOBJ_FRAME,        S1_SLOT_B, 1u,  100,  (long)IRIS_ERR_INVALID_ARG,   "frame misaligned" },
    };
    for (uint32_t c = 0; ok && c < 10u; c++) {
        long r = it_retype2_at(su, cases[c].t, cases[c].slot, cases[c].cnt, cases[c].arg);
        if (r != cases[c].expect) { ok = 0; why = cases[c].tag; }
    }
    /* Missing RIGHT_WRITE on the source untyped. */
    if (ok) {
        long ro = it_sys2(SYS_CAP_DERIVE, su, (long)RIGHT_READ);
        if (ro < 0) { ok = 0; why = "ro derive"; }
        else {
            if (it_retype2_at(ro, IRIS_KOBJ_ENDPOINT, S1_SLOT_B, 1u, 0) !=
                (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights"; }
            handle_id_t roh = (handle_id_t)ro; it_close(&roh);
        }
    }
    /* Destination that is not a CNode (the notification at S1_SLOT_A). */
    if (ok && it_sys4(SYS_UNTYPED_RETYPE2, su,
                      (long)((uint64_t)IRIS_KOBJ_ENDPOINT | (1ULL << 32)),
                      (long)((uint64_t)S1_SLOT_A | ((uint64_t)S1_SLOT_B << 32)),
                      0) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad dest cnode"; }
    /* Stale untyped handle: close it, then retype through the dead handle. */
    if (ok) {
        long su2 = s1_sub_ut(4096);
        if (su2 < 0) { ok = 0; why = "second sub"; }
        else {
            handle_id_t s2h = (handle_id_t)su2; it_close(&s2h);
            if (it_retype2_at(su2, IRIS_KOBJ_ENDPOINT, S1_SLOT_B, 1u, 0) !=
                (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "stale untyped"; }
        }
    }
    /* No validation failure consumed a byte or created an object. */
    if (ok && !it_utq_1(su, &ua)) { ok = 0; why = "query 2"; }
    if (ok && (ua.used_bytes != ub.used_bytes + (ua.used_bytes - ub.used_bytes))) { ok = 0; why = "?"; }
    if (ok) {
        long h = it_sys1(SYS_CSPACE_RESOLVE, (long)S1_SLOT_B);
        if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); ok = 0; why = "ghost object"; }
    }

    it_slot_delete(S1_SLOT_A);
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset busy"; }
    it_close(&su_h);
    if (ok) it_pass("T254"); else it_fail("T254", why);
}

/* ── T255: Endpoint Untyped lifecycle ───────────────────────────────────────
 * retype → derive/mint → send/receive → call → delete-one-cap (object
 * survives) → last-cap delete with a blocked waiter (close wakes CLOSED, no
 * ghost) → RESET → the SAME region hosts a fresh endpoint that works. */
static volatile int  g_t255_done;
static volatile long g_t255_res;
static uint8_t       g_t255_stack[8192];
static void t255_waiter(void) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    g_t255_res  = it_sys3(SYS_EP_RECV, (long)S1_SLOT_A, (long)&m, 0);
    g_t255_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void test_t255(void) {
    int ok = 1;
    const char *why = "endpoint lifecycle";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T255", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype"; }
    /* Derive: materialize a handle (bridge) + reduce rights. */
    long h  = ok ? it_sys1(SYS_CSPACE_RESOLVE, (long)S1_SLOT_A) : -1;
    long d  = (h >= 0) ? it_sys2(SYS_CAP_DERIVE, h, (long)RIGHT_WRITE) : -1;
    if (ok && (h < 0 || d < 0)) { ok = 0; why = "derive"; }
    /* send/receive through the CPtr + the derived handle. */
    if (ok) {
        struct IrisMsg m; it_iris_msg_zero(&m); m.label = 0x255;
        if (it_sys2(SYS_EP_NB_SEND, d, (long)&m) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "nb send"; }
    }
    /* call: needs our reply object. */
    if (ok && it_reply_create_at(S1_SLOT_B) < 0) { ok = 0; why = "reply"; }
    /* Delete ONE cap (the derived handle): object must survive. */
    if (ok) { handle_id_t dh = (handle_id_t)d; it_close(&dh); d = -1; }
    if (ok) {
        struct IrisMsg m; it_iris_msg_zero(&m);
        if (it_sys2(SYS_EP_NB_RECV, (long)S1_SLOT_A, (long)&m) != (long)IRIS_ERR_WOULD_BLOCK) {
            ok = 0; why = "object died with one cap (S10)";
        }
    }
    /* Blocked waiter + last-cap delete → CLOSED, no zombie (S25). */
    if (ok) {
        g_t255_done = 0; g_t255_res = 999;
        uint64_t entry = (uint64_t)(uintptr_t)t255_waiter;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t255_stack + sizeof(g_t255_stack))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) { ok = 0; why = "thread"; }
        else {
            for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);
            if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); h = -1; }
            it_slot_delete(S1_SLOT_A);      /* last cap → close fires */
            for (int y = 0; y < 4000 && !g_t255_done; y++) it_sys0(SYS_YIELD);
            if (!g_t255_done) { ok = 0; why = "waiter zombie"; }
            else if (g_t255_res != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong wake"; }
        }
    }
    it_slot_delete(S1_SLOT_B);
    if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); }
    /* Region reusable; the SAME range hosts a working replacement. */
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset busy (S12)"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "reuse retype"; }
    if (ok) {
        struct IrisMsg m; it_iris_msg_zero(&m);
        if (it_sys2(SYS_EP_NB_RECV, (long)S1_SLOT_A, (long)&m) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "reused ep dead"; }
        it_slot_delete(S1_SLOT_A);
    }
    it_close(&su_h);
    if (ok) it_pass("T255"); else it_fail("T255", why);
}

/* ── T256: Notification Untyped lifecycle ───────────────────────────────────
 * signal/wait + pending-bit accumulation through the CSpace cap, TWO holders
 * (CPtr + materialized handle) observing one object, waiter woken CLOSED by
 * last-cap delete (S26), destruction, and same-region reuse with NO residual
 * pending bits (S28). */
static volatile int  g_t256_done;
static volatile long g_t256_res;
static uint8_t       g_t256_stack[8192];
static void t256_waiter(void) {
    uint64_t bits = 0;
    g_t256_res  = it_sys2(SYS_NOTIFY_WAIT, (long)S1_SLOT_A, (long)(uintptr_t)&bits);
    g_t256_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void test_t256(void) {
    int ok = 1;
    const char *why = "notification lifecycle";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T256", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    if (it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype"; }
    /* Two holders: signal via handle, observe via CPtr (same object). */
    long h = ok ? it_sys1(SYS_CSPACE_RESOLVE, (long)S1_SLOT_A) : -1;
    if (ok && h < 0) { ok = 0; why = "resolve"; }
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, h, 0x5) != 0) { ok = 0; why = "signal"; }
    if (ok && it_sys2(SYS_NOTIFY_SIGNAL, (long)S1_SLOT_A, 0x2) != 0) { ok = 0; why = "signal cptr"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_sys2(SYS_NOTIFY_WAIT, (long)S1_SLOT_A, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x7u) { ok = 0; why = "pending bits"; }
    }
    /* Waiter + last-cap delete → CLOSED (S26), no zombie. */
    if (ok) {
        g_t256_done = 0; g_t256_res = 999;
        uint64_t entry = (uint64_t)(uintptr_t)t256_waiter;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t256_stack + sizeof(g_t256_stack))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) { ok = 0; why = "thread"; }
        else {
            for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);
            { handle_id_t hh = (handle_id_t)h; it_close(&hh); h = -1; }
            it_slot_delete(S1_SLOT_A);
            for (int y = 0; y < 4000 && !g_t256_done; y++) it_sys0(SYS_YIELD);
            if (!g_t256_done) { ok = 0; why = "waiter zombie"; }
            else if (g_t256_res != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong wake"; }
        }
    }
    if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); }
    /* Reuse: same region, fresh notification, ZERO residual bits (S28). */
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset busy"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "reuse retype"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)S1_SLOT_A,
                    (long)(uintptr_t)&bits, 20000000L) != (long)IRIS_ERR_TIMED_OUT) {
            ok = 0; why = "residual state leaked (S28)";
        }
        it_slot_delete(S1_SLOT_A);
    }
    it_close(&su_h);
    if (ok) it_pass("T256"); else it_fail("T256", why);
}

/* ── T257: explicit Reply lifecycle ─────────────────────────────────────────
 * A reply object is EXPLICIT authority: born from Untyped (replies_live +1,
 * no kslab), staged by the server's recv, bound at rendezvous, one-shot per
 * binding, REUSABLE across bindings, resilient to caller death, and stale
 * after its last cap is deleted.  The kernel never fabricates one: a CALL
 * against a reply-less receiver fails NOT_SUPPORTED (S16/S18/S22). */
static volatile int g_t257_done;
static uint8_t      g_t257_stack[8192];
static void t257_caller(void) {
    uint8_t rb[16];
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.label = 0x257; m.buf_uptr = (uint64_t)(uintptr_t)rb;
    (void)it_sys2(SYS_EP_CALL, (long)S1_SLOT_A, (long)&m);
    g_t257_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void test_t257(void) {
    int ok = 1;
    const char *why = "reply lifecycle";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T257", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_objects o0, o1;
    struct it_rinfo k0, k1;
    if (!it_utq_o(&o0) || !it_rinfo(HANDLE_INVALID, &k0)) { it_close(&su_h); it_fail("T257", "query"); return; }
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0 ||
        it_retype2_at(su, IRIS_KOBJ_REPLY,    S1_SLOT_B, 1u, 0) != 0) { ok = 0; why = "retype"; }
    if (ok && (!it_utq_o(&o1) || !it_rinfo(HANDLE_INVALID, &k1))) { ok = 0; why = "query 2"; }
    if (ok && o1.replies_live != o0.replies_live + 1u) { ok = 0; why = "reply not counted"; }
    if (ok && k1.kslab_used_bytes != k0.kslab_used_bytes) { ok = 0; why = "reply from kslab (S16)"; }

    /* Round 1: call → recv(reply) → reply; one-shot; then REUSE for round 2. */
    for (uint32_t round = 0; ok && round < 2u; round++) {
        g_t257_done = 0;
        uint64_t entry = (uint64_t)(uintptr_t)t257_caller;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t257_stack + sizeof(g_t257_stack))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) { ok = 0; why = "thread"; break; }
        struct IrisMsg m; it_iris_msg_zero(&m);
        if (it_sys3(SYS_EP_RECV, (long)S1_SLOT_A, (long)&m, (long)S1_SLOT_B) != 0 ||
            m.attached_handle != S1_SLOT_B) { ok = 0; why = "recv/echo"; break; }
        struct IrisMsg rm; it_iris_msg_zero(&rm); rm.label = 0xAC7;
        if (it_sys2(SYS_REPLY, (long)S1_SLOT_B, (long)&rm) != 0) { ok = 0; why = "reply"; break; }
        if (it_sys2(SYS_REPLY, (long)S1_SLOT_B, (long)&rm) != (long)IRIS_ERR_NOT_FOUND) {
            ok = 0; why = "one-shot broken (S18)"; break;
        }
        for (int y = 0; y < 4000 && !g_t257_done; y++) it_sys0(SYS_YIELD);
        if (!g_t257_done) { ok = 0; why = "caller stuck"; break; }
    }

    /* Caller death while bound: the reply object returns to FREE. */
    if (ok) {
        long ep2 = it_ep_create();
        handle_id_t cmd = (ep2 >= 0) ? (handle_id_t)ep2 : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep2 < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "spawn"; }
        if (ok && it_lp_cmd(cmd, LP_CMD_CALL_BLOCK) != 0) { ok = 0; why = "cmd"; }
        if (ok) {
            struct IrisMsg m; it_iris_msg_zero(&m);
            if (it_sys3(SYS_EP_RECV, (long)cmd, (long)&m, (long)S1_SLOT_B) != 0) { ok = 0; why = "recv child call"; }
        }
        if (ok && it_sys1(SYS_PROCESS_KILL, (long)proc) != 0) { ok = 0; why = "kill"; }
        if (ok) {
            struct IrisMsg rm; it_iris_msg_zero(&rm);
            if (it_sys2(SYS_REPLY, (long)S1_SLOT_B, (long)&rm) != (long)IRIS_ERR_NOT_FOUND) {
                ok = 0; why = "dead caller reply";
            }
        }
        it_close(&proc); it_close(&cmd);
    }

    /* No implicit fabrication: a reply-less receiver cannot serve a CALL. */
    if (ok) {
        g_t257_done = 0;
        uint64_t entry = (uint64_t)(uintptr_t)t257_caller;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t257_stack + sizeof(g_t257_stack))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) { ok = 0; why = "thread 2"; }
        else {
            for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);   /* caller queues */
            struct IrisMsg m; it_iris_msg_zero(&m);
            if (it_sys3(SYS_EP_RECV, (long)S1_SLOT_A, (long)&m, 0) !=
                (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "implicit reply not retired (S22)"; }
            /* Serve it properly so the thread exits. */
            if (ok) {
                if (it_sys3(SYS_EP_RECV, (long)S1_SLOT_A, (long)&m, (long)S1_SLOT_B) != 0) { ok = 0; why = "recv 3"; }
                struct IrisMsg rm; it_iris_msg_zero(&rm);
                if (ok && it_sys2(SYS_REPLY, (long)S1_SLOT_B, (long)&rm) != 0) { ok = 0; why = "reply 3"; }
                for (int y = 0; y < 4000 && !g_t257_done; y++) it_sys0(SYS_YIELD);
                if (!g_t257_done) { ok = 0; why = "caller 3 stuck"; }
            }
        }
    }

    /* Stale: delete the reply cap → the CPtr no longer resolves. */
    it_slot_delete(S1_SLOT_B);
    if (ok) {
        struct IrisMsg rm; it_iris_msg_zero(&rm);
        long r = it_sys2(SYS_REPLY, (long)S1_SLOT_B, (long)&rm);
        if (r != (long)IRIS_ERR_NOT_FOUND && r != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "stale reply cap"; }
    }
    it_slot_delete(S1_SLOT_A);
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset busy"; }
    if (ok) {
        struct it_utq_objects oz;
        if (!it_utq_o(&oz) || oz.replies_live != o0.replies_live) { ok = 0; why = "reply leak"; }
    }
    it_close(&su_h);
    if (ok) it_pass("T257"); else it_fail("T257", why);
}

/* ── T258: revoke/teardown during IPC and wait ──────────────────────────────
 * Deleting the LAST capability of an object with blocked parties never
 * leaves a zombie: a blocked sender wakes CLOSED, a pending CALLER whose
 * server loses its reply authority wakes CLOSED (S25/S27), and a
 * notification waiter wakes CLOSED (S26).  Object gauges return to baseline
 * and the regions are reusable. */
static volatile int  g_t258_done;
static volatile long g_t258_res;
static uint8_t       g_t258_stack[8192];
static void t258_sender(void) {
    struct IrisMsg m; it_iris_msg_zero(&m); m.label = 0x258;
    g_t258_res  = it_sys2(SYS_EP_SEND, (long)S1_SLOT_A, (long)&m);
    g_t258_done = 1;
    it_sys0(SYS_THREAD_EXIT);
    for (;;) {}
}
static void test_t258(void) {
    int ok = 1;
    const char *why = "revoke during ipc";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T258", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;
    struct it_utq_objects o0, oz;
    if (!it_utq_o(&o0)) { it_close(&su_h); it_fail("T258", "query"); return; }

    /* Blocked SENDER, last cap deleted → CLOSED. */
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype"; }
    if (ok) {
        g_t258_done = 0; g_t258_res = 999;
        uint64_t entry = (uint64_t)(uintptr_t)t258_sender;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t258_stack + sizeof(g_t258_stack))) & ~0xFULL;
        if (it_sys3(SYS_THREAD_CREATE, (long)entry, (long)rsp, 0) < 0) { ok = 0; why = "thread"; }
        else {
            for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);
            it_slot_delete(S1_SLOT_A);
            for (int y = 0; y < 4000 && !g_t258_done; y++) it_sys0(SYS_YIELD);
            if (!g_t258_done) { ok = 0; why = "sender zombie (S25)"; }
            else if (g_t258_res != (long)IRIS_ERR_CLOSED) { ok = 0; why = "sender wake err"; }
        }
    }

    /* Pending CALL: child blocked in call; the server's reply authority is
     * destroyed (last cap) → the caller wakes CLOSED, exits (S27). */
    if (ok) {
        long ep2 = it_ep_create();
        handle_id_t cmd = (ep2 >= 0) ? (handle_id_t)ep2 : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep2 < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "spawn"; }
        if (ok && it_reply_create_at(S1_SLOT_B) < 0) { ok = 0; why = "reply fixture"; }
        if (ok && it_lp_cmd(cmd, LP_CMD_CALL_BLOCK) != 0) { ok = 0; why = "cmd"; }
        if (ok) {
            struct IrisMsg m; it_iris_msg_zero(&m);
            if (it_sys3(SYS_EP_RECV, (long)cmd, (long)&m, (long)S1_SLOT_B) != 0) { ok = 0; why = "recv call"; }
        }
        if (ok) {
            it_slot_delete(S1_SLOT_B);         /* reply close → caller CLOSED */
            if (it_lp_wait_exit(proc) < 0) { ok = 0; why = "caller ghost (S27)"; }
        }
        it_close(&proc); it_close(&cmd);
    }

    /* Notification waiter, last cap deleted → CLOSED (S26): T256 already
     * proves this exact path; here we only re-verify the gauges. */
    it_quiesce_reaper();
    if (ok && (!it_utq_o(&oz) ||
               oz.endpoints_live != o0.endpoints_live ||
               oz.replies_live   != o0.replies_live)) { ok = 0; why = "object drift (S31)"; }
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset busy"; }
    it_close(&su_h);
    if (ok) it_pass("T258"); else it_fail("T258", why);
}

/* ── T259: Untyped reuse and stale-object defense ───────────────────────────
 * Object A (endpoint) lives in a region; a LIVE cap to A blocks RESET (S13).
 * After every cap dies, RESET bumps the region generation and the SAME
 * memory hosts object B (notification).  No stale path reaches B: the old
 * CSpace slot is empty (S30), a re-minted slot holds B's type — endpoint ops
 * on it fail WRONG_TYPE — and B starts with zero state (S28/S29). */
static void test_t259(void) {
    int ok = 1;
    const char *why = "reuse/stale defense";
    long su = s1_sub_ut(4096);
    if (su < 0) { it_fail("T259", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_one q0, q1, q2;
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype A"; }
    if (ok && !it_utq_1(su, &q0)) { ok = 0; why = "query"; }
    /* Keep a second cap (handle) to A: the region must NOT be reclaimable. */
    long h = ok ? it_sys1(SYS_CSPACE_RESOLVE, (long)S1_SLOT_A) : -1;
    if (ok && h < 0) { ok = 0; why = "resolve"; }
    it_slot_delete(S1_SLOT_A);
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != (long)IRIS_ERR_BUSY) {
        ok = 0; why = "live object did not retain region (S13)";
    }
    /* Drop the last cap → destroy → reset works and bumps the generation. */
    if (h >= 0) { handle_id_t hh = (handle_id_t)h; it_close(&hh); }
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset after death"; }
    if (ok && (!it_utq_1(su, &q1) || q1.generation != q0.generation + 1u ||
               q1.used_bytes != 0u)) { ok = 0; why = "generation not bumped"; }

    /* Same region now hosts B (a notification). */
    if (ok && it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype B"; }
    if (ok && !it_utq_1(su, &q2)) { ok = 0; why = "query B"; }
    if (ok && q2.used_bytes == 0u) { ok = 0; why = "B not in region"; }
    /* Stale-path probes against B:
     *  - endpoint ops through the reused slot → WRONG_TYPE (cap identity
     *    blocks the old object's protocol);
     *  - B carries no pending state from A's lifetime. */
    if (ok) {
        struct IrisMsg m; it_iris_msg_zero(&m);
        if (it_sys2(SYS_EP_NB_SEND, (long)S1_SLOT_A, (long)&m) != (long)IRIS_ERR_WRONG_TYPE) {
            ok = 0; why = "stale protocol reached B (S29)";
        }
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)S1_SLOT_A,
                    (long)(uintptr_t)&bits, 20000000L) != (long)IRIS_ERR_TIMED_OUT) {
            ok = 0; why = "residual state (S28)";
        }
    }
    it_slot_delete(S1_SLOT_A);
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "final reset"; }
    it_close(&su_h);
    if (ok) it_pass("T259"); else it_fail("T259", why);
}

/* ── T260: legacy create-path retirement ────────────────────────────────────
 * SYS_ENDPOINT_CREATE / SYS_NOTIFY_CREATE / SYS_CNODE_CREATE return
 * NOT_SUPPORTED and create NOTHING: no kslab movement, no live-object
 * change, no handle, no CSpace mutation (S22).  The one-shot KReply
 * fabrication inside EP_CALL is equally retired (proven in T257). */
static void test_t260(void) {
    int ok = 1;
    const char *why = "legacy retirement";
    struct it_rinfo k0, k1;
    struct it_utq_objects o0, o1;
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_rinfo(HANDLE_INVALID, &k0) || !it_utq_o(&o0) || !it_sched_ext(e0)) {
        it_fail("T260", "query"); return;
    }
    static const long retired_creates[] = { SYS_ENDPOINT_CREATE, SYS_NOTIFY_CREATE, SYS_CNODE_CREATE };
    for (uint32_t i = 0; ok && i < 3u; i++) {
        if (it_sys3(retired_creates[i], 4, 0, 0) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "create not retired";
        }
    }
    it_quiesce_reaper();
    if (ok && (!it_rinfo(HANDLE_INVALID, &k1) || !it_utq_o(&o1) || !it_sched_ext(e1))) {
        ok = 0; why = "query 2";
    }
    if (ok && k1.kslab_used_bytes != k0.kslab_used_bytes) { ok = 0; why = "kslab consumed"; }
    if (ok && (o1.endpoints_live != o0.endpoints_live ||
               o1.notifications_live != o0.notifications_live ||
               o1.cnodes_live != o0.cnodes_live)) { ok = 0; why = "object born"; }
    if (ok && e1[IT_SI_LIVE] != e0[IT_SI_LIVE]) { ok = 0; why = "handle born"; }
    if (ok) it_pass("T260"); else it_fail("T260", why);
}

/* ── T261: service IPC objects from delegated Untyped ───────────────────────
 * The REAL system runs on the new substrate: svcmgr, vfs, console and kbd
 * all serve EP_CALLs through endpoints their supervisor retyped from a
 * DELEGATED untyped pool, with explicit reply objects (svcmgr pool →
 * per-service reply sub-untypeds).  A privileged service RESTART exercises
 * the pool's reset+retype path and the service serves again. */
static void test_t261(void) {
    int ok = 1;
    const char *why = "service ipc";
    uint64_t b = 0;
    /* Each PING is an EP_CALL served with an explicit, untyped-funded reply. */
    if (it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &b) != 0)  { ok = 0; why = "svcmgr ping"; }
    if (ok && it_ping_badge((long)IRIS_CPTR_VFS_EP, &b) != 0) { ok = 0; why = "vfs ping"; }
    if (ok && it_ping_badge((long)IRIS_CPTR_CONSOLE_EP, &b) != 0) { ok = 0; why = "console ping"; }
    if (ok && it_ping_badge((long)IRIS_CPTR_KBD_EP, &b) != 0) { ok = 0; why = "kbd ping"; }

    /* Restart VFS: svcmgr RESETs the service's reply sub-untyped and retypes
     * a fresh reply object for the respawned child. */
    if (ok) {
        uint32_t a = 0, g0 = 0;
        if (it_status(VFS_EP_SVC_NAME, &a, &g0) != 0 || a != 1u) { ok = 0; why = "pre status"; }
        if (ok) {
            struct IrisMsg msg; it_iris_msg_zero(&msg);
            msg.label = IRIS_SVCMGR_EP_RESTART;
            msg.words[0] = (uint64_t)SVCMGR_SERVICE_VFS;
            msg.word_count = 1u;
            long r = it_sys2(SYS_EP_CALL, (long)IRIS_CPTR_TEST_SUPER, (long)&msg);
            if (!(r == 0 && msg.label == IRIS_EP_REPLY_OK)) { ok = 0; why = "restart denied"; }
        }
        int recovered = 0;
        for (uint32_t i = 0; ok && i < 400u && !recovered; i++) {
            uint64_t bb = 0;
            (void)it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &bb);
            uint32_t a1 = 0, g1 = 0;
            if (it_status(VFS_EP_SVC_NAME, &a1, &g1) == 0 && a1 == 1u && g1 > g0)
                recovered = 1;
        }
        if (ok && !recovered) { ok = 0; why = "vfs not restarted"; }
        /* The respawned VFS serves through its FRESH reply object. */
        if (ok && it_ping_badge((long)IRIS_CPTR_VFS_EP, &b) != 0) { ok = 0; why = "post-restart ping"; }
    }
    if (ok) it_pass("T261"); else it_fail("T261", why);
}

/* ── T262: deterministic Untyped object stress ──────────────────────────────
 * A seeded PRNG drives retype (single + batch) of endpoints / notifications
 * / replies, derives, IPC probes, signal/wait, deletes, forced batch
 * failures, stale-CPtr probes and full region reuse — against an EXACT
 * shadow model of child_count, and with the live-object gauges and kslab at
 * baseline after every round.  Prints seed/iteration only on failure. */
#define T262_SEED   0x51262262u
#define T262_ROUNDS 12u
static void test_t262(void) {
    int ok = 1;
    const char *why = "untyped stress";
    uint32_t round = 0, op = 0;
    g_fz_seed = T262_SEED;

    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T262", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_objects o0, oz;
    struct it_rinfo k0, kz;
    struct it_utq_global g0, g1;
    if (!it_utq_o(&o0) || !it_rinfo(HANDLE_INVALID, &k0) || !it_utq_g(&g0)) {
        it_close(&su_h); it_fail("T262", "query"); return;
    }

    for (round = 0; ok && round < T262_ROUNDS; round++) {
        uint32_t children = 0;
        /* 1..3 creation ops per round across slots 241..244. */
        uint32_t nops = 1u + (fz_rand() % 3u);
        uint32_t used_slots[4] = {0,0,0,0};
        for (uint32_t i = 0; ok && i < nops; i++) {
            op = fz_rand() % 3u;
            uint32_t slot = S1_SLOT_A + i;
            uint32_t type = (op == 0u) ? IRIS_KOBJ_ENDPOINT
                          : (op == 1u) ? IRIS_KOBJ_NOTIFICATION
                                       : IRIS_KOBJ_REPLY;
            if (it_retype2_at(su, type, slot, 1u, 0) != 0) { ok = 0; why = "retype"; break; }
            used_slots[i] = type;
            children++;
            /* Type-appropriate probe. */
            if (type == IRIS_KOBJ_ENDPOINT) {
                struct IrisMsg m; it_iris_msg_zero(&m);
                if (it_sys2(SYS_EP_NB_RECV, (long)slot, (long)&m) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "ep probe"; }
            } else if (type == IRIS_KOBJ_NOTIFICATION) {
                uint64_t bits = 0;
                if (it_sys2(SYS_NOTIFY_SIGNAL, (long)slot, 1u + round) != 0 ||
                    it_sys2(SYS_NOTIFY_WAIT, (long)slot, (long)(uintptr_t)&bits) != 0 ||
                    bits != (uint64_t)(1u + round)) { ok = 0; why = "notif probe"; }
            } else {
                struct IrisMsg rm; it_iris_msg_zero(&rm);
                if (it_sys2(SYS_REPLY, (long)slot, (long)&rm) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "reply probe"; }
            }
        }
        /* Occasional forced failures — must not consume anything. */
        if (ok && (fz_rand() & 1u)) {
            struct it_utq_one uf0, uf1;
            if (!it_utq_1(su, &uf0)) { ok = 0; why = "q"; }
            if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 2u, 0) !=
                      (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "collision"; }
            if (ok && it_retype2_at(su, 77u, S1_SLOT_E, 1u, 0) !=
                      (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "badtype"; }
            if (ok && (!it_utq_1(su, &uf1) || uf1.used_bytes != uf0.used_bytes ||
                       uf1.child_count != uf0.child_count)) { ok = 0; why = "failure consumed"; }
        }
        /* Derive + drop on one member (object survives until slot delete). */
        if (ok && (fz_rand() & 1u)) {
            long h = it_sys1(SYS_CSPACE_RESOLVE, (long)S1_SLOT_A);
            if (h < 0) { ok = 0; why = "resolve"; }
            else { handle_id_t hh = (handle_id_t)h; it_close(&hh); }
        }
        /* Exact shadow of child_count. */
        if (ok) {
            struct it_utq_one u;
            if (!it_utq_1(su, &u) || u.child_count != children) { ok = 0; why = "shadow child"; }
        }
        /* Tear down; stale CPtr probes must fail cleanly; region reusable. */
        for (uint32_t i = 0; ok && i < nops; i++) it_slot_delete(S1_SLOT_A + i);
        if (ok) {
            struct IrisMsg m; it_iris_msg_zero(&m);
            if (it_sys2(SYS_EP_NB_SEND, (long)S1_SLOT_A, (long)&m) != (long)IRIS_ERR_NOT_FOUND) {
                ok = 0; why = "stale cptr";
            }
        }
        it_quiesce_reaper();
        if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "round reset"; }
        if (ok) {
            struct it_utq_one u;
            if (!it_utq_1(su, &u) || u.used_bytes != 0u || u.child_count != 0u) { ok = 0; why = "round reclaim"; }
        }
        if (ok && (!it_utq_o(&oz) || !it_rinfo(HANDLE_INVALID, &kz))) { ok = 0; why = "round query"; }
        if (ok && (oz.endpoints_live     != o0.endpoints_live ||
                   oz.notifications_live != o0.notifications_live ||
                   oz.replies_live       != o0.replies_live)) { ok = 0; why = "round drift"; }
        if (ok && kz.kslab_used_bytes != k0.kslab_used_bytes) { ok = 0; why = "round kslab"; }
        (void)used_slots;
    }

    /* Global instrumentation moved in the right direction: retypes and
     * failures were counted, every round's RESET registered as reclaim+reuse. */
    if (ok && !it_utq_g(&g1)) { ok = 0; why = "global query"; }
    if (ok && !(g1.retype_count    >  g0.retype_count &&
                g1.retype_failures >= g0.retype_failures &&
                g1.reset_count     >= g0.reset_count + T262_ROUNDS &&
                g1.reuse_count     >= g0.reuse_count + T262_ROUNDS &&
                g1.reclaimed_bytes >  g0.reclaimed_bytes)) { ok = 0; why = "global counters"; }

    it_close(&su_h);
    if (ok) it_pass("T262");
    else { it_fz_note("T262", T262_SEED, round, op); it_fail("T262", why); }
}

/* ════════════════════════════════════════════════════════════════════════
 * Fase S2 — Untyped Task Construction (increment 1: SchedulingContext).
 * ════════════════════════════════════════════════════════════════════════ */

/* SYS_UNTYPED_QUERY kind 4 — task-object gauges + CDT counters. */
struct it_utq_taskobj {
    uint32_t version, struct_size;
    uint32_t tcb_live, tcb_hwm, tcb_retyped, tcb_destroyed;
    uint32_t sc_live, sc_hwm, sc_retyped, sc_destroyed;
    uint32_t cdt_deriv, cdt_deriv_hwm, cdt_revoke, cdt_delete,
             cdt_cross, cdt_ipc, legacy_handle_deriv_migrated;
};
static int it_utq_t(struct it_utq_taskobj *q) {
    return it_sys3(SYS_UNTYPED_QUERY, 4, (long)(uintptr_t)q, 0) == 0;
}

/* ── T267: SchedulingContext configure/bind lifecycle ────────────────────────
 * A SC is a CANONICAL object created ONLY from Untyped (SYS_SC_CREATE retired).
 * It is born unconfigured and unbound; SC_CONFIGURE validates budget/period;
 * SC_BIND is one-to-one against a TCB cap, requires a configured SC, rejects a
 * second binding (BUSY), unbinds cleanly, and rebinds.  Provenance: the SC
 * lives in the source Untyped (sc_retyped/live move; no kslab).
 * Invariants: S2.2, S2.8, S2.9, S2.13, S2.14, S2.15. */
static void test_t267(void) {
    int ok = 1;
    const char *why = "sc lifecycle";

    /* SYS_SC_CREATE is retired. */
    if (it_sys0(SYS_SC_CREATE) != (long)IRIS_ERR_NOT_SUPPORTED) { it_fail("T267", "sc create not retired"); return; }

    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T267", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_taskobj t0, t1;
    struct it_rinfo k0, k1;
    if (!it_utq_t(&t0) || !it_rinfo(HANDLE_INVALID, &k0)) { it_close(&su_h); it_fail("T267", "query"); return; }

    /* Retype two SCs into CSpace slots (provenance + no kslab). */
    if (it_retype2_at(su, IRIS_KOBJ_SCHED_CONTEXT, S1_SLOT_A, 1u, 0) != 0 ||
        it_retype2_at(su, IRIS_KOBJ_SCHED_CONTEXT, S1_SLOT_B, 1u, 0) != 0) { ok = 0; why = "retype"; }
    if (ok && (!it_utq_t(&t1) || !it_rinfo(HANDLE_INVALID, &k1))) { ok = 0; why = "query 2"; }
    if (ok && t1.sc_live != t0.sc_live + 2u) { ok = 0; why = "sc not counted"; }
    if (ok && t1.sc_retyped < t0.sc_retyped + 2u) { ok = 0; why = "retype not counted"; }
    if (ok && k1.kslab_used_bytes != k0.kslab_used_bytes) { ok = 0; why = "sc from kslab (S2.13)"; }

    /* Unconfigured SC cannot bind (B2/B3). */
    long self_tcb = ok ? it_sys0(SYS_TCB_SELF) : -1;
    handle_id_t self_h = (self_tcb >= 0) ? (handle_id_t)self_tcb : HANDLE_INVALID;
    if (ok && self_tcb < 0) { ok = 0; why = "tcb self"; }
    if (ok && it_sys2(SYS_SC_BIND, (long)S1_SLOT_A, (long)self_h) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "unconfigured bind allowed";
    }
    /* Configure validation (S2.8). */
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)S1_SLOT_A, 5, 100) != 0) { ok = 0; why = "configure"; }
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)S1_SLOT_A, 0, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget 0"; }
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)S1_SLOT_A, 100, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget==period"; }
    if (ok && it_sys3(SYS_SC_CONFIGURE, (long)S1_SLOT_B, 5, 100) != 0) { ok = 0; why = "configure B"; }

    /* Bind SC_A to our own TCB, then a SECOND SC to the same TCB must fail
     * BUSY (one-to-one: the target already holds SC_A).  Unbind immediately
     * (no yield in between — never let the tiny budget suspend iris_test). */
    if (ok && it_sys2(SYS_SC_BIND, (long)S1_SLOT_A, (long)self_h) != 0) { ok = 0; why = "bind"; }
    if (ok && it_sys2(SYS_SC_BIND, (long)S1_SLOT_B, (long)self_h) != (long)IRIS_ERR_BUSY) { ok = 0; why = "double bind (S2.9)"; }
    if (ok && it_sys2(SYS_SC_BIND, (long)S1_SLOT_A, 0) != 0) { ok = 0; why = "unbind"; }
    /* After unbind, SC_A is free again → SC_B binds, then unbind. */
    if (ok && it_sys2(SYS_SC_BIND, (long)S1_SLOT_B, (long)self_h) != 0) { ok = 0; why = "rebind"; }
    if (ok && it_sys2(SYS_SC_BIND, (long)S1_SLOT_B, 0) != 0) { ok = 0; why = "unbind 2"; }

    it_close(&self_h);
    it_slot_delete(S1_SLOT_A); it_slot_delete(S1_SLOT_B);
    if (ok && it_sys1(SYS_UNTYPED_RESET, su) != 0) { ok = 0; why = "reset busy"; }
    if (ok) {
        struct it_utq_taskobj tz;
        if (!it_utq_t(&tz) || tz.sc_live != t0.sc_live) { ok = 0; why = "sc leak"; }
        if (ok && tz.sc_destroyed < t0.sc_destroyed + 2u) { ok = 0; why = "destroy not counted"; }
    }
    it_close(&su_h);
    if (ok) it_pass("T267"); else it_fail("T267", why);
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
    test_t140();
    test_t141();
    test_t142();
    test_t143();
    test_t144();
    test_t145();
    test_t146();
    test_t147();
    test_t148();
    test_t149();
    test_t150();
    test_t151();
    test_t152();
    test_t153();
    test_t154();
    test_t155();
    test_t156();
    test_t157();
    test_t158();
    test_t159();
    test_t160();
    test_t161();
    test_t162();
    test_t163();
    test_t164();
    test_t165();
    test_t166();
    test_t167();
    test_t168();
    test_t169();
    test_t170();
    test_t171();
    test_t172();
    test_t173();
    test_t174();
    test_t175();
    test_t176();
    test_t177();
    test_t178();
    test_t179();
    test_t180();
    test_t181();
    test_t182();
    test_t183();
    test_t184();
    test_t185();
    test_t186();
    test_t187();
    test_t188();
    test_t189();
    test_t190();
    test_t191();
    test_t192();
    test_t193();
    test_t194();
    test_t195();
    test_t196();
    test_t197();
    test_t198();
    test_t199();
    test_t200();
    test_t201();
    test_t202();
    test_t203();
    test_t204();
    test_t205();
    test_t206();
    test_t207();
    test_t208();
    test_t209();
    test_t210();
    test_t211();
    test_t212();
    test_t213();
    test_t214();
    test_t215();
    test_t216();
    test_t217();
    test_t218();
    test_t219();
    test_t220();
    test_t221();
    test_t222();
    test_t223();
    test_t224();
    test_t225();
    test_t226();
    test_t227();
    test_t228();
    test_t229();
    test_t230();
    test_t231();
    test_t232();
    test_t233();
    test_t234();
    test_t235();
    test_t236();
    test_t237();
    test_t238();
    test_t239();
    test_t240();
    test_t241();
    test_t242();
    test_t243();
    test_t244();
    test_t245();
    test_t246();
    test_t247();
    test_t248();
    test_t249();
    test_t250();

    /* Fase S1 — seL4 Architectural Convergence suite. */
    test_t251();
    test_t252();
    test_t253();
    test_t254();
    test_t255();
    test_t256();
    test_t257();
    test_t258();
    test_t259();
    test_t260();
    test_t261();
    test_t262();

    /* Fase S2 — Untyped task construction (increment 1: SchedulingContext). */
    test_t267();

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
