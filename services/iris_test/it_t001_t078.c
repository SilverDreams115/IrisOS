/*
 * it_t001_t078.c — tests T001 through T078.
 *
 * The suite's numbering is chronological, not thematic: T001 was written
 * stages before T078, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T001 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"


#include "../common/iris_msg.h"
/* ── T001: the three ambient answers are RETIRED (A-27) ────────────────────
 *
 * `SYS_GETPID` handed a thread its own task id and `SYS_THREAD_EXIT` ended it
 * without recording why.  Neither was authority — no number selected an object
 * and none conferred a right — but both answered a question from NOTHING, which
 * is the shape A-18 spent its length removing from the CSpace and A-24 from the
 * scheduler.  seL4 has an equivalent for neither: identity is what others hold
 * about you, and exiting says what happened.
 *
 * `SYS_CLOCK_GET` was the third candidate and it STAYED.  On x86 `rdtsc` is an
 * unprivileged instruction, so a monotonic read cannot be gated by anything and
 * retiring the syscall would have moved the same ungated read into an
 * instruction.  Reading a counter is not authority; BLOCKING on one is, and
 * A-24 made that a capability.  Ledger A-27. */
void test_t001(void) {
    int ok = 1;
    const char *why = "ambient answers";
    if (ok && it_sys0(SYS_GETPID) != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "GETPID still answers";
    }
    /* THREAD_EXIT is not called from this thread for the obvious reason; it is
     * exercised where it would have ended one — every helper thread in this
     * suite now exits through SYS_EXIT, which records a code. */
    if (ok) it_pass("T001"); else it_fail("T001", why);
}

/* ── T002: a granted clock can be ASKED (A-27) ─────────────────────────────
 *
 * `SYS_CLOCK_GET` stays, because it cannot be gated (see T001).  What the timer
 * service adds is that a client which was given a clock can ask ITS OWNER
 * rather than reaching around it — which is the shape every other resource in
 * this system has.  Two claims: it answers, and it MOVES, which is what says
 * the service is reading a clock rather than returning a constant. */
void test_t002(void) {
    int ok = 1;
    const char *why = "clock is a service";
    uint64_t t0 = 0, t1 = 0;

    if (it_invoke0((long)IRIS_CPTR_TIMER_EP, INV_CAP_IDENTIFY)
        != (long)IRIS_HANDLE_TYPE_ENDPOINT) { it_fail("T002", "no clock granted"); return; }

    if (ok && it_timer_uptime(&t0) != 0) { ok = 0; why = "uptime"; }
    /* One tick of the line is 10 ms; wait for more than one so the comparison
     * is about the clock advancing and not about scheduling luck. */
    if (ok) {
        uint64_t bits = 0;
        long n = it_notify_create();
        if (n < 0) { ok = 0; why = "notif"; }
        else if (it_wait_timeout(n, (long)(uintptr_t)&bits, 40000000L)
                 != (long)IRIS_ERR_TIMED_OUT) { ok = 0; why = "wait"; }
        if (n >= 0) { iris_cptr_t h = (iris_cptr_t)n; it_close(&h); }
    }
    if (ok && it_timer_uptime(&t1) != 0) { ok = 0; why = "uptime 2"; }
    if (ok && t1 <= t0) { ok = 0; why = "clock did not advance"; }

    if (ok) it_pass("T002"); else it_fail("T002", why);
}

/* ── T003: SYS_YIELD ────────────────────────────────────────────────────── */

void test_t003(void) {
    long r = it_sys0(SYS_YIELD);
    if (r == 0)
        it_pass("T003");
    else
        it_fail("T003", "yield non-zero");
}

void test_t008(void) {
    long vmo_raw = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, T008_VMO_SIZE);
    if (vmo_raw < 0) { it_fail("T008", "vmo create"); return; }
    iris_cptr_t vmo_h = (iris_cptr_t)vmo_raw;

    /* Map writable (flag=1) at T008_VMO_ADDR */
    long r = it_invoke(vmo_raw, INV_FRAME_MAP, IT_VS, (long)T008_VMO_ADDR, 1);
    if (r < 0) {
        it_close(&vmo_h);
        it_fail("T008", "vmo map"); return;
    }

    /* Write and read back via the mapped address */
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)T008_VMO_ADDR;
    *p = 0xDEADBEEFCAFEBABEULL;
    uint64_t readback = *p;

    /* Verify VMO size */
    long sz = it_invoke0(vmo_raw, INV_FRAME_SIZE);

    /* Unmap */
    long ur = it_invoke2(vmo_raw, INV_FRAME_UNMAP, IT_VS, (long)T008_VMO_ADDR);

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

void test_t009(void) {
    long n_raw = it_notify_create_slot();
    if (n_raw < 0) { it_fail("T009", "notify create"); return; }

    long r = it_invoke1(n_raw, INV_NOTIFY_SIGNAL, 0x3u);
    if (r < 0) {
        it_slot_delete((uint32_t)n_raw);
        it_fail("T009", "notify signal"); return;
    }

    uint64_t out_bits = 0;
    r = it_invoke1(n_raw, INV_NOTIFY_WAIT, (long)(uintptr_t)&out_bits);

    it_slot_delete((uint32_t)n_raw);

    if (r == 0 && out_bits == 0x3u)
        it_pass("T009");
    else
        it_fail("T009", "bits mismatch");
}

/* ── T010: NOTIFY_WAIT_TIMEOUT → TIMED_OUT ─────────────────────────────── */

void test_t010(void) {
    long n_raw = it_notify_create_slot();
    if (n_raw < 0) { it_fail("T010", "notify create"); return; }

    uint64_t out_bits = 0;
    long r = it_wait_timeout( n_raw,
                     (long)(uintptr_t)&out_bits, 50000000L);

    it_slot_delete((uint32_t)n_raw);

    if (r == (long)IRIS_ERR_TIMED_OUT)
        it_pass("T010");
    else
        it_fail("T010", "expected TIMED_OUT");
}

/* ── T011 / T012 RETIRED (Stage 4) ────────────────────────────────────────
 * Their SUBJECT was the handle namespace: T011 asserted SYS_HANDLE_TYPE on an
 * endpoint HANDLE, T012 that two HANDLES name one object.  Both questions are
 * real and both are asked of CSpace now — T292 covers the type of a slot for
 * every family, T293 covers identity across a rights-reduced, badged derive.
 * Migrating these two would have left them asserting the same thing twice
 * through a namespace that is being deleted, which the roadmap's rule calls a
 * test whose subject dies with the mechanism. */

/* ── T013: Rights enforcement on an endpoint (no WRITE → EP_SEND fails) ──── */
/* Phase 13/Track I: rewritten from KChannel to KEndpoint — EP_SEND requires
 * RIGHT_WRITE, so a READ-only cap is rejected with ACCESS_DENIED (same
 * rights-enforcement guarantee, no SYS_CHAN). */
void test_t013(void) {
    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T013", "ep create"); return; }
    iris_cptr_t ep_h = (iris_cptr_t)ep_raw;

    /* Derive a READ-only copy (no WRITE right) */
    long ro_raw = it_cs_reduce(ep_raw, RIGHT_READ);
    if (ro_raw < 0) {
        it_close(&ep_h);
        it_fail("T013", "ro derive"); return;
    }
    iris_cptr_t ro_h = (iris_cptr_t)ro_raw;

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    /* EP_SEND on a read-only endpoint cap must fail with ACCESS_DENIED. */
    long r = iris_msg_send(ro_raw, &msg);

    it_close(&ro_h);
    it_close(&ep_h);

    if (r == (long)IRIS_ERR_ACCESS_DENIED)
        it_pass("T013");
    else
        it_fail("T013", "expected ACCESS_DENIED");
}

/* ── T014: EP_NB_RECV on empty endpoint ─────────────────────────────────── */

void test_t014(void) {
    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T014", "ep create"); return; }

    struct iris_msg msg;
    iris_msg_zero(&msg);
    long r = iris_msg_nb_recv(ep_raw, &msg);

    it_slot_delete((uint32_t)ep_raw);

    if (r == (long)IRIS_ERR_WOULD_BLOCK)
        it_pass("T014");
    else
        it_fail("T014", "expected WOULD_BLOCK");
}

/* ── T015: EP_SEND / EP_RECV (two-thread rendezvous) ────────────────────── */

static iris_cptr_t g_t015_ep_h   = IRIS_CPTR_NULL;
static volatile int g_t015_done  = 0;
static          int g_t015_ok    = 0;
static uint8_t      g_t015_stack[8192];

static void t015_server(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    long r = iris_msg_recv((long)g_t015_ep_h, &msg);
    g_t015_ok   = (r == 0 && msg.label == 0xC0FFEEULL);
    g_t015_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t015(void) {
    g_t015_done = 0;
    g_t015_ok   = 0;

    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T015", "ep create"); return; }
    g_t015_ep_h = (iris_cptr_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t015_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t015_stack + sizeof(g_t015_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) {
        it_close(&g_t015_ep_h);
        it_fail("T015", "thread create"); return;
    }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    /* EP_SEND blocks until server is ready to rendezvous */
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label      = 0xC0FFEEULL;
    msg.word_count = 0;
    long r = iris_msg_send(ep_raw, &msg);

    /* Poll for server to set done flag (it runs after rendezvous returns) */
    for (int i = 0; i < 200 && !g_t015_done; i++)
        it_settle(1);

    it_close(&tid_h);
    it_close(&g_t015_ep_h);

    if (r == 0 && g_t015_ok)
        it_pass("T015");
    else
        it_fail("T015", "ep rendezvous");
}

/* ── T016: EP_CALL / SYS_REPLY (two-thread) ─────────────────────────────── */

static iris_cptr_t g_t016_ep_h   = IRIS_CPTR_NULL;
static volatile int g_t016_done  = 0;
static          int g_t016_ok    = 0;
static uint8_t      g_t016_stack[8192];

static void t016_server(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    long r = (msg.reply = 88, iris_msg_recv((long)g_t016_ep_h, &msg));
    if (r < 0 || msg.got_cap == IRIS_MSG_NO_CAP) {
        g_t016_done = 1;
        it_sys1(SYS_EXIT, 0);
        for (;;) {}
    }

    iris_cptr_t reply_h = (iris_cptr_t)msg.got_cap;
    struct iris_msg reply;
    iris_msg_zero(&reply);
    reply.label      = 0xFEEDBEEFULL;
    reply.word_count = 0;
    long rr = iris_msg_reply((long)reply_h, &reply);
    g_t016_ok   = (rr == 0);
    g_t016_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t016(void) {
    g_t016_done = 0;
    g_t016_ok   = 0;

    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T016", "ep create"); return; }
    g_t016_ep_h = (iris_cptr_t)ep_raw;
    if (it_reply_create_at(88) < 0) {
        it_close(&g_t016_ep_h);
        it_fail("T016", "reply create"); return;
    }

    uint64_t entry = (uint64_t)(uintptr_t)t016_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t016_stack + sizeof(g_t016_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) {
        it_close(&g_t016_ep_h);
        it_fail("T016", "thread create"); return;
    }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    /* EP_CALL blocks until server calls SYS_REPLY */
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label      = 0xABCDULL;
    msg.word_count = 0;
    long r = iris_msg_call(ep_raw, &msg);

    /* After EP_CALL returns the server has already replied */
    for (int i = 0; i < 200 && !g_t016_done; i++)
        it_settle(1);

    it_close(&tid_h);
    it_close(&g_t016_ep_h);
    it_slot_delete(88);

    if (r == 0 && g_t016_ok)
        it_pass("T016");
    else
        it_fail("T016", "ep_call/reply");
}

/* ── T018: EP_NB_SEND on empty endpoint → WOULD_BLOCK ──────────────────── */

void test_t018(void) {
    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T018", "ep create"); return; }

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = 0x1818ULL;
    long r = iris_msg_nb_send(ep_raw, &msg);

    it_slot_delete((uint32_t)ep_raw);

    if (r == (long)IRIS_ERR_WOULD_BLOCK)
        it_pass("T018");
    else
        it_fail("T018", "expected WOULD_BLOCK");
}

/* ── T019: endpoint close wakes blocked EP_RECV thread ──────────────────── */

static iris_cptr_t g_t019_ep_h    = IRIS_CPTR_NULL;
static volatile int g_t019_done   = 0;
static          int g_t019_result = 0;
static uint8_t      g_t019_stack[8192];

static void t019_thread(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    long r = iris_msg_recv((long)g_t019_ep_h, &msg);
    g_t019_result = (int)r;
    g_t019_done   = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t019(void) {
    g_t019_done   = 0;
    g_t019_result = 0;

    /* Step 4: the property under test — dropping the LAST capability to an
     * endpoint wakes a blocked receiver — is real in a CSpace-only kernel; only
     * the vehicle changes.  The endpoint lives in a slot and the drop is a
     * slot delete instead of a handle close. */
    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T019", "ep create"); return; }
    g_t019_ep_h = (iris_cptr_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t019_thread;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t019_stack + sizeof(g_t019_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) {
        it_slot_delete((uint32_t)g_t019_ep_h);
        g_t019_ep_h = IRIS_CPTR_NULL;
        it_fail("T019", "thread create"); return;
    }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    /* Let thread enter EP_RECV and block */
    it_settle(5);

    /* Delete the slot → last cap gone → endpoint close fires → thread wakes */
    it_slot_delete((uint32_t)g_t019_ep_h);
    g_t019_ep_h = IRIS_CPTR_NULL;

    for (int i = 0; i < 200 && !g_t019_done; i++)
        it_settle(1);

    it_close(&tid_h);

    if (g_t019_result == (int)IRIS_ERR_CLOSED)
        it_pass("T019");
    else
        it_fail("T019", "expected CLOSED on blocked recv");
}

/* ── T020: endpoint close wakes blocked EP_SEND thread ──────────────────── */

static iris_cptr_t g_t020_ep_h    = IRIS_CPTR_NULL;
static volatile int g_t020_done   = 0;
static          int g_t020_result = 0;
static uint8_t      g_t020_stack[8192];

static void t020_thread(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = 0x2020ULL;
    long r = iris_msg_send((long)g_t020_ep_h, &msg);
    g_t020_result = (int)r;
    g_t020_done   = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t020(void) {
    g_t020_done   = 0;
    g_t020_result = 0;

    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T020", "ep create"); return; }
    g_t020_ep_h = (iris_cptr_t)ep_raw;

    /* A second capability to the same endpoint — active_refs = 2.  It was a
     * handle dup; it is a CSpace copy, which is the only kind left. */
    long ep2_raw = it_cs_reduce(ep_raw, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
    if (ep2_raw < 0) {
        it_close(&g_t020_ep_h);
        it_fail("T020", "second cap"); return;
    }
    iris_cptr_t ep2_h = (iris_cptr_t)ep2_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t020_thread;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t020_stack + sizeof(g_t020_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) {
        it_close(&ep2_h);
        it_close(&g_t020_ep_h);
        it_fail("T020", "thread create"); return;
    }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    /* Let thread enter EP_SEND and block (no receiver present) */
    it_settle(5);

    /* Close both handles: active_refs 2→1→0 → endpoint close → thread wakes */
    it_close(&ep2_h);
    it_close(&g_t020_ep_h);

    for (int i = 0; i < 200 && !g_t020_done; i++)
        it_settle(1);

    it_close(&tid_h);

    if (g_t020_result == (int)IRIS_ERR_CLOSED)
        it_pass("T020");
    else
        it_fail("T020", "expected CLOSED on blocked send");
}

/* ── T021: SYS_REPLY twice → second returns NOT_FOUND ───────────────────── */

static iris_cptr_t g_t021_ep_h   = IRIS_CPTR_NULL;
static volatile int g_t021_done  = 0;
static          int g_t021_ok    = 0;
static uint8_t      g_t021_stack[8192];

static void t021_client(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = 0x2121ULL;
    long r = iris_msg_call((long)g_t021_ep_h, &msg);
    g_t021_ok   = (r == 0);
    g_t021_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t021(void) {
    g_t021_done = 0;
    g_t021_ok   = 0;

    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T021", "ep create"); return; }
    g_t021_ep_h = (iris_cptr_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t021_client;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t021_stack + sizeof(g_t021_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) {
        it_close(&g_t021_ep_h);
        it_fail("T021", "thread create"); return;
    }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    /* Main is server: receive the EP_CALL and get reply_h */
    if (it_reply_create_at(89) < 0) {
        it_close(&tid_h);
        it_close(&g_t021_ep_h);
        it_fail("T021", "reply create"); return;
    }
    struct iris_msg msg;
    iris_msg_zero(&msg);
    long r = (msg.reply = 89, iris_msg_recv(ep_raw, &msg));
    if (r < 0 || msg.got_cap == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t021_ep_h);
        it_slot_delete(89);
        it_fail("T021", "ep_recv"); return;
    }

    iris_cptr_t reply_h = (iris_cptr_t)msg.got_cap;

    /* First SYS_REPLY → client unblocks */
    struct iris_msg reply;
    iris_msg_zero(&reply);
    reply.label = 0xCAFEULL;
    long r1 = iris_msg_reply((long)reply_h, &reply);

    /* Wait for client to record EP_CALL result */
    for (int i = 0; i < 200 && !g_t021_done; i++)
        it_settle(1);

    /* Second SYS_REPLY on same handle → NOT_FOUND (caller pointer is NULL) */
    iris_msg_zero(&reply);
    long r2 = iris_msg_reply((long)reply_h, &reply);

    it_close(&tid_h);
    it_close(&g_t021_ep_h);
    it_slot_delete(89);

    if (r1 == 0 && g_t021_ok && r2 == (long)IRIS_ERR_NOT_FOUND)
        it_pass("T021");
    else
        it_fail("T021", "reply-twice");
}

/* Tentative declaration: the main thread's bulk buffer is defined with the
 * rest of the EP helpers below, but T022 is above them. */
uint8_t *g_ep_io_buf;

/* ── T022: EP_CALL + bulk kbuf round-trip ───────────────────────────────── */

static iris_cptr_t g_t022_ep_h   = IRIS_CPTR_NULL;
static volatile int g_t022_done  = 0;
static          int g_t022_ok    = 0;
static uint8_t      g_t022_stack[8192];

static void t022_server(void) {
    struct iris_msg rmsg;
    iris_msg_zero(&rmsg);
    /* D-4/A-33: the payload lands in this thread's OWN IPC buffer, and the
     * message says how MANY bytes rather than where they are.  There is
     * nowhere else they could be: a thread sends from the page it registered
     * or it does not send a payload at all. */
    long r = (rmsg.reply = 90, iris_msg_recv((long)g_t022_ep_h, &rmsg));
    if (r < 0 || rmsg.buf_len != 4u ||
            rmsg.got_cap == (uint32_t)IRIS_MSG_NO_CAP) {
        g_t022_done = 1;
        it_sys1(SYS_EXIT, 0);
        for (;;) {}
    }

    /* A-33: THIS thread's buffer, not the one that called us.  Each end
     * marshals in the page it registered; the kernel copied the request into
     * ours and will copy our reply out of it. */
    volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)g_it_thread_buf;
    int recv_ok = (b[0] == 0x10 && b[1] == 0x20 &&
                   b[2] == 0x30 && b[3] == 0x40);

    b[0] = (uint8_t)(b[0] + 1u);
    b[1] = (uint8_t)(b[1] + 1u);
    b[2] = (uint8_t)(b[2] + 1u);
    b[3] = (uint8_t)(b[3] + 1u);

    iris_cptr_t reply_h = (iris_cptr_t)rmsg.got_cap;
    struct iris_msg repl;
    iris_msg_zero(&repl);
    repl.label   = 0xB01FULL;
    repl.buf_len  = 4u;
    long rr = iris_msg_reply((long)reply_h, &repl);

    g_t022_ok   = (recv_ok && rr == 0);
    g_t022_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t022(void) {
    g_t022_done = 0;
    g_t022_ok   = 0;

    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T022", "ep create"); return; }
    g_t022_ep_h = (iris_cptr_t)ep_raw;
    if (it_reply_create_at(90) < 0) {
        it_close(&g_t022_ep_h);
        it_fail("T022", "reply create"); return;
    }

    uint64_t entry = (uint64_t)(uintptr_t)t022_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t022_stack + sizeof(g_t022_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) {
        it_close(&g_t022_ep_h);
        it_fail("T022", "thread create"); return;
    }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    g_ep_io_buf[0] = 0x10; g_ep_io_buf[1] = 0x20;
    g_ep_io_buf[2] = 0x30; g_ep_io_buf[3] = 0x40;

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = 0xCA11ULL;
    msg.buf_len  = 4u;
    long r = iris_msg_call(ep_raw, &msg);

    for (int i = 0; i < 200 && !g_t022_done; i++)
        it_settle(1);

    it_close(&tid_h);
    it_close(&g_t022_ep_h);
    it_slot_delete(90);

    int bulk_ok = (g_ep_io_buf[0] == 0x11 && g_ep_io_buf[1] == 0x21 &&
                   g_ep_io_buf[2] == 0x31 && g_ep_io_buf[3] == 0x41);

    if (r == 0 && g_t022_ok && bulk_ok)
        it_pass("T022");
    else
        it_fail("T022", "ep_call bulk round-trip");
}

/* ── T023: EP_SEND on read-only endpoint handle → ACCESS_DENIED ─────────── */

void test_t023(void) {
    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T023", "ep create"); return; }
    iris_cptr_t ep_h = (iris_cptr_t)ep_raw;

    /* Derive a READ-only copy (no WRITE right) */
    long ro_raw = it_cs_reduce(ep_raw, RIGHT_READ);
    if (ro_raw < 0) {
        it_close(&ep_h);
        it_fail("T023", "ro derive"); return;
    }
    iris_cptr_t ro_h = (iris_cptr_t)ro_raw;

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = 0x2323ULL;
    long r = iris_msg_send(ro_raw, &msg);

    it_close(&ro_h);
    it_close(&ep_h);

    if (r == (long)IRIS_ERR_ACCESS_DENIED)
        it_pass("T023");
    else
        it_fail("T023", "expected ACCESS_DENIED");
}
static iris_cptr_t g_t024_ep_h   = IRIS_CPTR_NULL;
static volatile int g_t024_done  = 0;
static          int g_t024_ok    = 0;
static uint32_t     g_t024_got_h = 0;
static uint8_t      g_t024_stack[8192];

static void t024_client(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = 0x2424ULL;
    /* Stage 4: the client declares where a reply-transferred cap should land.
     * Without a declaration the reply arrives with no capability at all —
     * handle materialisation is retired. */
    it_slot_delete((uint32_t)T024_GOT_SLOT);
    msg.recv_slot = (uint32_t)T024_GOT_SLOT;
    long r = iris_msg_call((long)g_t024_ep_h, &msg);
    g_t024_ok    = (r == 0 && msg.label == IRIS_EP_REPLY_OK);
    g_t024_got_h = msg.got_cap;
    g_t024_done  = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t024(void) {
    g_t024_done = 0; g_t024_ok = 0; g_t024_got_h = 0;

    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T024", "ep create"); return; }
    g_t024_ep_h = (iris_cptr_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t024_client;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t024_stack + sizeof(g_t024_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) {
        it_close(&g_t024_ep_h);
        it_fail("T024", "thread create"); return;
    }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    /* Main is server: receive the call, reply with an attached notification */
    if (it_reply_create_at(91) < 0) {
        it_close(&tid_h);
        it_close(&g_t024_ep_h);
        it_fail("T024", "reply create"); return;
    }
    struct iris_msg msg;
    iris_msg_zero(&msg);
    long r = (msg.reply = 91, iris_msg_recv(ep_raw, &msg));
    if (r < 0 || msg.got_cap == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t024_ep_h);
        it_slot_delete(91);
        it_fail("T024", "ep_recv"); return;
    }
    iris_cptr_t reply_h = (iris_cptr_t)msg.got_cap;

    long rr = -1;
    long notif_raw = it_notify_create_slot();
    if (notif_raw >= 0) {
        /* Phase S4 (Step 2): the reply's transfer source is a CSpace slot. */
        long src = it_xfer_dup(notif_raw, RIGHT_WRITE | RIGHT_WAIT);
        if (src >= 0) {
            struct iris_msg reply;
            iris_msg_zero(&reply);
            reply.label           = IRIS_EP_REPLY_OK;
            reply.cap = (uint32_t)src;
            reply.cap_rights = RIGHT_WRITE | RIGHT_WAIT;
            rr = iris_msg_reply((long)reply_h, &reply);
            it_xfer_release(src);
        }
        /* the master notification handle is ours regardless of the transfer */
        iris_cptr_t nh = (iris_cptr_t)notif_raw;
        it_close(&nh);
    }

    for (int i = 0; i < 200 && !g_t024_done; i++)
        it_settle(1);

    long ty = -1;
    if (g_t024_got_h != (uint32_t)IRIS_MSG_NO_CAP)
        ty = it_invoke0((long)g_t024_got_h, INV_CAP_IDENTIFY);

    iris_cptr_t got_h = (iris_cptr_t)g_t024_got_h;
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

static iris_cptr_t g_t025_ep_h  = IRIS_CPTR_NULL;
static volatile int g_t025_done = 0;
static          int g_t025_ok   = 0;
static uint8_t      g_t025_stack[8192];

static void t025_client(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = 0x2525ULL;
    long r = iris_msg_call((long)g_t025_ep_h, &msg);
    g_t025_ok   = (r == 0 &&
                   msg.got_cap == (uint32_t)IRIS_MSG_NO_CAP);
    g_t025_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t025(void) {
    g_t025_done = 0; g_t025_ok = 0;

    long ep_raw = it_ep_create_slot();
    if (ep_raw < 0) { it_fail("T025", "ep create"); return; }
    g_t025_ep_h = (iris_cptr_t)ep_raw;

    uint64_t entry = (uint64_t)(uintptr_t)t025_client;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t025_stack + sizeof(g_t025_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) {
        it_close(&g_t025_ep_h);
        it_fail("T025", "thread create"); return;
    }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    if (it_reply_create_at(92) < 0) {
        it_close(&tid_h);
        it_close(&g_t025_ep_h);
        it_fail("T025", "reply create"); return;
    }
    struct iris_msg msg;
    iris_msg_zero(&msg);
    long r = (msg.reply = 92, iris_msg_recv(ep_raw, &msg));
    if (r < 0 || msg.got_cap == (uint32_t)IRIS_MSG_NO_CAP) {
        it_close(&tid_h);
        it_close(&g_t025_ep_h);
        it_slot_delete(92);
        it_fail("T025", "ep_recv"); return;
    }
    iris_cptr_t reply_h = (iris_cptr_t)msg.got_cap;

    /* Phase S4 (Step 2): a SOURCE SLOT without RIGHT_TRANSFER → staging must
     * fail with ACCESS_DENIED and leave the slot intact. */
    iris_cptr_t notif_h = IRIS_CPTR_NULL;
    long r1 = -1;
    int  src_preserved = 0;
    long notif_raw = it_notify_create_slot();
    if (notif_raw >= 0) {
        notif_h = (iris_cptr_t)notif_raw;
        /* Stage 4: the fixture is a CSpace slot, so the reduced-rights copy is
         * a slot-to-slot derive (SYS_CSPACE_MINT).  SYS_CNODE_MINT's source is
         * handle-only and would simply not resolve a CPtr. */
        it_slot_delete(IT_XFER_SLOT_C);
        if (it_invoke2(notif_raw, INV_CSPACE_MINT, (long)((uint64_t)IT_XFER_SLOT_C << 32), (long)(RIGHT_WRITE | RIGHT_WAIT)) == 0) {
            struct iris_msg reply;
            iris_msg_zero(&reply);
            reply.label           = IRIS_EP_REPLY_OK;
            reply.cap = IT_XFER_SLOT_C;
            reply.cap_rights = RIGHT_WRITE | RIGHT_WAIT;
            r1 = iris_msg_reply((long)reply_h, &reply);
            /* denied staging must NOT consume the source slot */
            src_preserved =
                (it_invoke0((long)IT_XFER_SLOT_C, INV_CAP_IDENTIFY) >= 0);
            it_slot_delete(IT_XFER_SLOT_C);
        }
    }

    /* Clean reply on the SAME KReply handle must still succeed */
    struct iris_msg reply;
    iris_msg_zero(&reply);
    reply.label = IRIS_EP_REPLY_OK;
    long r2 = iris_msg_reply((long)reply_h, &reply);

    for (int i = 0; i < 200 && !g_t025_done; i++)
        it_settle(1);

    it_close(&notif_h);
    it_close(&tid_h);
    it_close(&g_t025_ep_h);
    it_slot_delete(92);

    if (r1 == (long)IRIS_ERR_ACCESS_DENIED && src_preserved &&
        r2 == 0 && g_t025_ok)
        it_pass("T025");
    else
        it_fail("T025", "non-transferable reply cap");
}

/* ── Phase 7.1: EP-based service path (svcmgr discovery + VFS) ───────────── */

/* Phase 8: the discovery endpoint is the well-known CPtr slot (kind 0x20
 * retired); it is a CNode slot index, NOT a handle — never close it. */
iris_cptr_t g_svcmgr_ep_h = (iris_cptr_t)IRIS_CPTR_SVCMGR_EP;
iris_cptr_t g_vfs_ep_h    = IRIS_CPTR_NULL;  /* from T026 lookup   */
static iris_cptr_t g_kbd_ep_h    = IRIS_CPTR_NULL;  /* from T034 lookup   */
static iris_cptr_t g_con_ep_h    = IRIS_CPTR_NULL;  /* from T036 lookup   */

/* EP_CALL buffer reuse: request payload AND reply bulk destination. */
/*
 * The main test thread's bulk-payload buffer (ledger D-4).
 *
 * A pointer, not an array, because it moves: it starts at the static fallback
 * and becomes the thread's REGISTERED IPC buffer at suite start.  Every one of
 * the forty sites below writes through it and names it in `msg.buf_uptr`, so
 * the migration is one assignment rather than forty edits — and naming the
 * registered address is what the kernel accepts, a foreign pointer being a
 * marshalling mistake it refuses.
 */
static uint8_t  g_ep_io_static[VFS_EP_DATA_MAX];
uint8_t *g_ep_io_buf = g_ep_io_static;

uint32_t it_stage_path(const char *path) {
    uint32_t n = 0;
    while (path[n] && n + 1u < IT_EP_IO_CAP) {
        g_ep_io_buf[n] = (uint8_t)path[n];
        n++;
    }
    g_ep_io_buf[n] = 0u;
    return n + 1u;
}

/* LOOKUP_NAME through svcmgr, taking the answer as a CAPABILITY IN A SLOT.
 *
 * The reply carries an endpoint cap.  Declaring a receive slot (EP_CALL reads
 * the declaration from attached_handle) makes the kernel install it there
 * instead of materialising a handle — the same authority, addressed the way
 * every other capability in this process is.  The slot lives in the
 * second-level CNode because the root has no room, which is exactly what
 * Stage 4's multi-level receive slots exist for.
 *
 * Returns the CPtr on success, or a negative error.  The slot is deleted
 * first, so a re-run (or a stale occupant) is clean. */
static long it_lookup_ep(const char *name, uint32_t dest_cptr) {
    it_slot_delete(dest_cptr);

    uint32_t len = it_stage_path(name);
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label           = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_len         = len;
    msg.recv_slot = dest_cptr;      /* receive-slot declaration */

    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    if (r != 0 || msg.label != IRIS_EP_REPLY_OK) return -1;
    if (msg.got_cap != dest_cptr) {
        /* Landed somewhere else (or nowhere): drop whatever arrived so the
         * failure does not leak authority into the next test. */
        if (msg.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
            iris_cptr_t h = (iris_cptr_t)msg.got_cap;
            it_close(&h);
        }
        return -1;
    }
    if (it_invoke0((long)dest_cptr, INV_CAP_IDENTIFY) != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
        it_slot_delete(dest_cptr);
        return -1;
    }
    return (long)dest_cptr;
}

/* ── T026: svcmgr EP LOOKUP_NAME("vfs.ep") → endpoint cap ───────────────── */

void test_t026(void) {
    long c = it_lookup_ep(VFS_EP_SVC_NAME, IT_LOOKUP_VFS);
    if (c < 0) { it_fail("T026", "vfs.ep lookup"); return; }
    g_vfs_ep_h = (iris_cptr_t)c;
    it_pass("T026");
}

/* ── T027: VFS EP ping ──────────────────────────────────────────────────── */

void test_t027(void) {
    if (g_vfs_ep_h == IRIS_CPTR_NULL) {
        it_fail("T027", "vfs ep missing"); return;
    }

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_EP_OP_PING;
    long r = iris_msg_call((long)g_vfs_ep_h, &msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T027");
    else
        it_fail("T027", "vfs ping");
}

/* ── T028: VFS EP READ_AT("iris.txt") content + EOF semantics ───────────── */

void test_t028(void) {
    if (g_vfs_ep_h == IRIS_CPTR_NULL) {
        it_fail("T028", "vfs ep missing"); return;
    }

    static const char expect[] = "Hello from IrisOS VFS!\n";
    const uint32_t expect_len = (uint32_t)(sizeof(expect) - 1u);

    uint32_t len = it_stage_path("iris.txt");
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;                 /* offset */
    msg.words[1]   = VFS_EP_DATA_MAX;   /* len (server clamps) */
    msg.word_count = 2;
    msg.buf_len    = len;
    long r = iris_msg_call((long)g_vfs_ep_h, &msg);

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
        iris_msg_zero(&msg);
        msg.label      = VFS_EP_OP_READ_AT;
        msg.words[0]   = expect_len;
        msg.words[1]   = VFS_EP_DATA_MAX;
        msg.word_count = 2;
        msg.buf_len    = len;
        r = iris_msg_call((long)g_vfs_ep_h, &msg);
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

void test_t029(void) {
    if (g_vfs_ep_h == IRIS_CPTR_NULL) {
        it_fail("T029", "vfs ep missing"); return;
    }

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = UINT64_C(0x0EEE);  /* not a VFS opcode */
    long r = iris_msg_call((long)g_vfs_ep_h, &msg);
    int unk_ok = (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
                  msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_SUPPORTED);

    iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_LIST;
    msg.words[0]   = 999;
    msg.word_count = 1;
    r = iris_msg_call((long)g_vfs_ep_h, &msg);
    int oob_ok = (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
                  msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND);

    if (unk_ok && oob_ok)
        it_pass("T029");
    else
        it_fail("T029", "vfs ep error codes");
}

/* ── T030: VFS EP malformed READ_AT paths → INVALID_ARG ─────────────────── */

static int t030_expect_inval(struct iris_msg *msg) {
    long r = iris_msg_call((long)g_vfs_ep_h, msg);
    return (r == 0 && msg->label == IRIS_EP_REPLY_ERR &&
            msg->words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG);
}

void test_t030(void) {
    if (g_vfs_ep_h == IRIS_CPTR_NULL) {
        it_fail("T030", "vfs ep missing"); return;
    }

    struct iris_msg msg;
    int ok = 1;

    /* (a) READ_AT with no path payload */
    iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;
    msg.words[1]   = 16;
    msg.word_count = 2;
    msg.buf_len    = 0;
    if (!t030_expect_inval(&msg)) ok = 0;

    /* (b) path not NUL-terminated (drop the NUL from buf_len) */
    uint32_t len = it_stage_path("iris.txt");
    iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;
    msg.words[1]   = 16;
    msg.word_count = 2;
    msg.buf_len    = len - 1u;
    if (!t030_expect_inval(&msg)) ok = 0;

    /* (c) oversized path (buf_len > VFS_EP_PATH_MAX) */
    for (uint32_t i = 0; i < VFS_EP_PATH_MAX; i++) g_ep_io_buf[i] = (uint8_t)'a';
    g_ep_io_buf[VFS_EP_PATH_MAX] = 0u;
    iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;
    msg.words[1]   = 16;
    msg.word_count = 2;
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
void test_t031(void) {
    if (g_svcmgr_ep_h == IRIS_CPTR_NULL) {
        it_fail("T031", "svcmgr ep missing"); return;
    }

    uint32_t len = it_stage_path("spoof.ep");
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_len  = len;
    long r = iris_msg_call((long)g_svcmgr_ep_h, &msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
        msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND &&
        msg.got_cap == (uint32_t)IRIS_MSG_NO_CAP) {
        it_pass("T031");
    } else {
        if (msg.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
            iris_cptr_t h = (iris_cptr_t)msg.got_cap;
            it_close(&h);
        }
        it_fail("T031", "spoof.ep lookup must NOT resolve");
    }
}

/* ── T032: legacy "vfs" KChannel name must no longer resolve (Phase 7.5) ─── */

/*
 * vfs is endpoint_only: svcmgr never creates the legacy service/reply
 * KChannel pair, so a lookup of the bare "vfs" name must return NOT_FOUND
 * with no capability attached. A resolvable "vfs" here would mean a stale
 * legacy KChannel route back into the stateful protocol.
 */
void test_t032(void) {
    if (g_svcmgr_ep_h == IRIS_CPTR_NULL) {
        it_fail("T032", "svcmgr ep missing"); return;
    }

    uint32_t len = it_stage_path("vfs");
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_len  = len;
    long r = iris_msg_call((long)g_svcmgr_ep_h, &msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
        msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND &&
        msg.got_cap == (uint32_t)IRIS_MSG_NO_CAP) {
        it_pass("T032");
    } else {
        if (msg.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
            iris_cptr_t h = (iris_cptr_t)msg.got_cap;
            it_close(&h);
        }
        it_fail("T032", "legacy vfs name must NOT resolve");
    }
}

/* ── T033: VFS EP STATUS (Phase 7.5) ─────────────────────────────────────── */

void test_t033(void) {
    if (g_vfs_ep_h == IRIS_CPTR_NULL) {
        it_fail("T033", "vfs ep missing"); return;
    }

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = VFS_EP_OP_STATUS;
    long r = iris_msg_call((long)g_vfs_ep_h, &msg);

    if (r != 0 || msg.label != IRIS_EP_REPLY_OK ||
        msg.words[1] < (uint64_t)VFS_BOOT_EXPORT_COUNT) {
        it_fail("T033", "vfs ep status");
        return;
    }

    /* STATUS with a bulk payload is malformed → INVALID_ARG */
    uint32_t len = it_stage_path("junk");
    iris_msg_zero(&msg);
    msg.label    = VFS_EP_OP_STATUS;
    msg.buf_len  = len;
    r = iris_msg_call((long)g_vfs_ep_h, &msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
        msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG)
        it_pass("T033");
    else
        it_fail("T033", "status payload must be rejected");
}

/* ── T034: svcmgr EP LOOKUP_NAME("kbd.ep") → endpoint cap + PING (7.4) ─── */

void test_t034(void) {
    if (g_svcmgr_ep_h == IRIS_CPTR_NULL) {
        it_fail("T034", "svcmgr ep missing"); return;
    }

    long c = it_lookup_ep(KBD_EP_SVC_NAME, IT_LOOKUP_KBD);
    if (c < 0) { it_fail("T034", "kbd.ep lookup"); return; }
    g_kbd_ep_h = (iris_cptr_t)c;

    struct iris_msg msg;
    long r;
    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    r = iris_msg_call((long)g_kbd_ep_h, &msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T034");
    else
        it_fail("T034", "kbd ping");
}

/* ── T035: kbd EP semantics — empty POLL, malformed requests (7.4) ────── */

static int t035_expect_err(struct iris_msg *msg, uint32_t want) {
    return msg->label == IRIS_EP_REPLY_ERR &&
           (uint32_t)msg->words[0] == want;
}

void test_t035(void) {
    if (g_kbd_ep_h == IRIS_CPTR_NULL) {
        it_fail("T035", "kbd ep missing"); return;
    }

    struct iris_msg msg;
    int ok = 1;

    /* POLL on an idle keyboard (headless: no keys) → WOULD_BLOCK, clean */
    iris_msg_zero(&msg);
    msg.label = KBD_EP_OP_POLL;
    if (iris_msg_call((long)g_kbd_ep_h, &msg) != 0 ||
        !t035_expect_err(&msg, KBD_EP_E_WOULD_BLOCK)) ok = 0;

    /* unknown opcode → NOT_SUPPORTED */
    iris_msg_zero(&msg);
    msg.label = 0x7777;
    if (iris_msg_call((long)g_kbd_ep_h, &msg) != 0 ||
        !t035_expect_err(&msg, KBD_EP_E_NOT_SUPPORTED)) ok = 0;

    /* bulk payload on POLL → INVALID_ARG */
    uint32_t len = it_stage_path("junk");
    iris_msg_zero(&msg);
    msg.label    = KBD_EP_OP_POLL;
    msg.buf_len  = len;
    if (iris_msg_call((long)g_kbd_ep_h, &msg) != 0 ||
        !t035_expect_err(&msg, KBD_EP_E_INVALID_ARG)) ok = 0;

    if (ok)
        it_pass("T035");
    else
        it_fail("T035", "kbd ep semantics");
}

/* ── T036: svcmgr EP LOOKUP_NAME("console.ep") → endpoint cap + PING (7.3) ─ */

void test_t036(void) {
    if (g_svcmgr_ep_h == IRIS_CPTR_NULL) {
        it_fail("T036", "svcmgr ep missing"); return;
    }

    long c = it_lookup_ep(CONSOLE_EP_SVC_NAME, IT_LOOKUP_CON);
    if (c < 0) { it_fail("T036", "console.ep lookup"); return; }
    g_con_ep_h = (iris_cptr_t)c;

    struct iris_msg msg;
    long r;
    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    r = iris_msg_call((long)g_con_ep_h, &msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T036");
    else
        it_fail("T036", "console ping");
}

/* ── T037: console EP WRITE — gated marker lands on the UART (7.3) ──────── */

void test_t037(void) {
    if (g_con_ep_h == IRIS_CPTR_NULL) {
        it_fail("T037", "console ep missing"); return;
    }

    static const char line[] = "[IRIS][TEST] console ep write OK\n";
    uint32_t len = (uint32_t)sizeof(line) - 1u;
    for (uint32_t i = 0; i < len; i++) g_ep_io_buf[i] = (uint8_t)line[i];

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = CONSOLE_EP_OP_WRITE;
    msg.buf_len  = len;
    long r = iris_msg_call((long)g_con_ep_h, &msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T037");
    else
        it_fail("T037", "console ep write");
}

/* ── T038: console EP SYNC + malformed requests (7.3) ───────────────────── */

void test_t038(void) {
    if (g_con_ep_h == IRIS_CPTR_NULL) {
        it_fail("T038", "console ep missing"); return;
    }

    struct iris_msg msg;
    int ok = 1;

    /* SYNC: deterministic barrier, no payload */
    iris_msg_zero(&msg);
    msg.label = CONSOLE_EP_OP_SYNC;
    if (iris_msg_call((long)g_con_ep_h, &msg) != 0 ||
        msg.label != IRIS_EP_REPLY_OK) ok = 0;

    /* SYNC with bulk payload → INVALID_ARG */
    uint32_t len = it_stage_path("junk");
    iris_msg_zero(&msg);
    msg.label    = CONSOLE_EP_OP_SYNC;
    msg.buf_len  = len;
    if (iris_msg_call((long)g_con_ep_h, &msg) != 0 ||
        msg.label != IRIS_EP_REPLY_ERR ||
        (uint32_t)msg.words[0] != (uint32_t)IRIS_ERR_INVALID_ARG) ok = 0;

    /* unknown opcode → NOT_SUPPORTED */
    iris_msg_zero(&msg);
    msg.label = 0x6666;
    if (iris_msg_call((long)g_con_ep_h, &msg) != 0 ||
        msg.label != IRIS_EP_REPLY_ERR ||
        (uint32_t)msg.words[0] != (uint32_t)IRIS_ERR_NOT_SUPPORTED) ok = 0;

    if (ok)
        it_pass("T038");
    else
        it_fail("T038", "console ep semantics");
}

/* ── T039: CPtr-first svcmgr discovery (Phase 8) ─────────────────────────── */

/*
 * init minted the svcmgr discovery endpoint into our root CNode at
 * IRIS_CPTR_SVCMGR_EP (slot 1, RIGHT_WRITE). EP_CALL resolves CSpace-first,
 * so the raw CPtr — never delivered as a handle — must work end to end:
 * PING, then a LOOKUP_NAME("vfs.ep") that returns a real endpoint cap.
 */
void test_t039(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    if (r != 0 || msg.label != IRIS_EP_REPLY_OK) {
        it_fail("T039", "cptr ping");
        return;
    }

    long c = it_lookup_ep(VFS_EP_SVC_NAME, IT_LOOKUP_TMP);
    it_slot_delete(IT_LOOKUP_TMP);
    if (c >= 0)
        it_pass("T039");
    else
        it_fail("T039", "cptr lookup");
}

/* ── T040: CPtr failure semantics (Phase 8) ──────────────────────────────── */

/*
 * slot 30 (IRIS_CPTR_TEST_FIX_A) = console KChannel cap (wrong type),
 * slot 31 (IRIS_CPTR_TEST_FIX_B) = svcmgr ep with RIGHT_TRANSFER only
 * (insufficient for EP_CALL's RIGHT_WRITE).
 *   - IRIS_CPTR_NULL → clean negative error (never a crash);
 *   - wrong type → IRIS_ERR_WRONG_TYPE from the CSpace path;
 *   - ACCESS_DENIED → hard stop: the dual resolver must NOT fall back to
 *     the handle table (a fallback would yield BAD_HANDLE instead, since
 *     raw values 30/31 are never live handles — generations start at 1).
 */
void test_t040(void) {
    struct iris_msg msg;
    int ok = 1;

    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = iris_msg_call(0L /* IRIS_CPTR_NULL */, &msg);
    if (r >= 0) ok = 0;

    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    r = iris_msg_call((long)IRIS_CPTR_TEST_FIX_A, &msg);
    if (r != (long)IRIS_ERR_WRONG_TYPE) ok = 0;

    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    r = iris_msg_call((long)IRIS_CPTR_TEST_FIX_B, &msg);
    if (r != (long)IRIS_ERR_ACCESS_DENIED) ok = 0;

    if (ok)
        it_pass("T040");
    else
        it_fail("T040", "cptr failure semantics");
}

/* ── T041: well-known CPtr slots resolve with the right type (Phase 8) ───── */

/*
 * SYS_CSPACE_RESOLVE materializes a slot into a handle; slots 1..4 must all
 * be live KEndpoints and a reserved-but-unminted slot must fail cleanly.
 */
void test_t041(void) {
    static const uint64_t slots[4] = {
        IRIS_CPTR_SVCMGR_EP, IRIS_CPTR_VFS_EP,
        IRIS_CPTR_CONSOLE_EP, IRIS_CPTR_KBD_EP,
    };
    int ok = 1;
    for (uint32_t i = 0; i < 4u; i++) {
        if (it_invoke0((long)slots[i], INV_CAP_IDENTIFY)
            != (long)IRIS_HANDLE_TYPE_ENDPOINT) { ok = 0; break; }
    }
    /* unminted reserved slot fails cleanly (no crash, negative error) */
    if (it_invoke0(29L, INV_CAP_IDENTIFY) >= 0) ok = 0;

    if (ok)
        it_pass("T041");
    else
        it_fail("T041", "well-known slot resolve");
}

/* ── T042: VFS READ_AT directly via IRIS_CPTR_VFS_EP (Phase 8) ───────────── */

void test_t042(void) {
    static const char expect[] = "Hello from IrisOS VFS!\n";
    const uint32_t expect_len = (uint32_t)(sizeof(expect) - 1u);

    uint32_t len = it_stage_path("iris.txt");
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0;
    msg.words[1]   = VFS_EP_DATA_MAX;
    msg.word_count = 2;
    msg.buf_len    = len;
    long r = iris_msg_call((long)IRIS_CPTR_VFS_EP, &msg);

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

void test_t043(void) {
    static const char line[] = "[IRIS][TEST] console cptr write OK\n";
    const uint32_t line_len = (uint32_t)(sizeof(line) - 1u);

    for (uint32_t i = 0; i < line_len; i++) g_ep_io_buf[i] = (uint8_t)line[i];

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = CONSOLE_EP_OP_WRITE;
    msg.buf_len  = line_len;
    long r = iris_msg_call((long)IRIS_CPTR_CONSOLE_EP, &msg);

    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T043");
    else
        it_fail("T043", "console via cptr");
}

/* ── T044: kbd PING via IRIS_CPTR_KBD_EP (Phase 8) ───────────────────────── */

void test_t044(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = iris_msg_call((long)IRIS_CPTR_KBD_EP, &msg);
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
void test_t045(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    long r = iris_msg_nb_recv((long)IRIS_CPTR_VFS_EP, &msg);
    if (r == (long)IRIS_ERR_ACCESS_DENIED)
        it_pass("T045");
    else
        it_fail("T045", "rights reduction on client slot");
}

/* ── T046 RETIRED (Stage 4) ───────────────────────────────────────────────
 * It asserted that a name lookup still yields a real HANDLE alongside the CPtr
 * slots — the interop guarantee for clients that had not migrated.  There are
 * none, and handle materialisation on delivery is retired.
 *
 * It is worth recording HOW it ended, because it stopped testing anything
 * before it stopped compiling: its check was `attached_handle >= 1024`, which
 * meant "this is a handle" only while handles were encoded as slot|gen<<10.
 * Once the reply landed in a declared receive slot the value was a two-level
 * CPtr (62288), still >= 1024, and the test passed on a magnitude comparison
 * that no longer meant anything.  T092 asserts the guarantee that replaced it:
 * a client that declares no slot gets the reply WITHOUT the capability. */

/* ── Phase 9: badges & sender identity (T047–T053) ───────────────────────── */

/* PING a slot and return the badge the server says it observed (words[1]);
 * stores -1 on transport/protocol failure. */
long it_ping_badge(long cptr, uint64_t *out_badge) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = iris_msg_call(cptr, &msg);
    if (r != 0 || msg.label != IRIS_EP_REPLY_OK || msg.word_count < 2u)
        return -1;
    *out_badge = msg.words[1];
    return 0;
}

/* T047: svcmgr observes our kernel-stamped badge on the discovery slot. */
void test_t047(void) {
    uint64_t b = 0;
    if (it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &b) == 0 &&
        b == IRIS_BADGE_IRIS_TEST)
        it_pass("T047");
    else
        it_fail("T047", "svcmgr badge");
}

/* T048: VFS observes the expected badge. */
void test_t048(void) {
    uint64_t b = 0;
    if (it_ping_badge((long)IRIS_CPTR_VFS_EP, &b) == 0 &&
        b == IRIS_BADGE_IRIS_TEST)
        it_pass("T048");
    else
        it_fail("T048", "vfs badge");
}

/* T049: console observes the expected badge. */
void test_t049(void) {
    uint64_t b = 0;
    if (it_ping_badge((long)IRIS_CPTR_CONSOLE_EP, &b) == 0 &&
        b == IRIS_BADGE_IRIS_TEST)
        it_pass("T049");
    else
        it_fail("T049", "console badge");
}

/* T050: kbd (assembly server) observes the expected badge. */
void test_t050(void) {
    uint64_t b = 0;
    if (it_ping_badge((long)IRIS_CPTR_KBD_EP, &b) == 0 &&
        b == IRIS_BADGE_IRIS_TEST)
        it_pass("T050");
    else
        it_fail("T050", "kbd badge");
}

/* T051: payload spoofing is impossible — whatever we write into
 * sender_badge is overwritten by the kernel at send time. */
void test_t051(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label        = IRIS_EP_OP_PING;
    msg.sender_badge = 0xDEADBEEFu;          /* forged identity attempt */
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.word_count >= 2u && msg.words[1] == IRIS_BADGE_IRIS_TEST)
        it_pass("T051");
    else
        it_fail("T051", "payload badge spoof must not work");
}

/* T052: legacy unbadged path stays compatible — a cap obtained via name
 * lookup (handle >= 1024, unbadged master dup) delivers badge 0. */
void test_t052(void) {
    uint32_t len = it_stage_path(CONSOLE_EP_SVC_NAME);
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_len  = len;
    /* Stage 4: a reply that carries a capability needs somewhere to put it —
     * handle materialisation is retired, so an undeclared receive gets the
     * message without the cap. */
    it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    msg.recv_slot = (uint32_t)IT_LOOKUP_TMP;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);

    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
        uint64_t b = 0xFFu;
        if (it_ping_badge((long)msg.got_cap, &b) == 0 && b == 0u)
            ok = 1;
        iris_cptr_t h = (iris_cptr_t)msg.got_cap;
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
void test_t053(void) {
    uint64_t b1 = 0, b2 = 0;
    int ok = 1;
    if (it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &b1) != 0) ok = 0;
    if (it_ping_badge((long)IRIS_CPTR_TEST_FIX_C, &b2) != 0) ok = 0;
    if (b1 != IRIS_BADGE_IRIS_TEST || b2 != IRIS_BADGE_TEST_B || b1 == b2)
        ok = 0;

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    if (iris_msg_call((long)IRIS_CPTR_TEST_FIX_B, &msg) !=
        (long)IRIS_ERR_ACCESS_DENIED)
        ok = 0;

    if (ok)
        it_pass("T053");
    else
        it_fail("T053", "distinct badges per cap");
}

/* ── Phase 10: service lifecycle, death/relookup & badge policy (T054–T062) ─ */

/* svcmgr STATUS oracle: name → {alive, generation}. Returns 0 on OK. */
long it_status(const char *name, uint32_t *alive, uint32_t *gen) {
    uint32_t len = it_stage_path(name);
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_STATUS;
    msg.buf_len  = len;
    /* Stage 4: a reply that carries a capability needs somewhere to put it —
     * handle materialisation is retired, so an undeclared receive gets the
     * message without the cap. */
    it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    msg.recv_slot = (uint32_t)IT_LOOKUP_TMP;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    if (r != 0 || msg.label != IRIS_EP_REPLY_OK || msg.word_count < 2u)
        return -1;
    if (alive) *alive = (uint32_t)msg.words[0];
    if (gen)   *gen   = (uint32_t)msg.words[1];
    return 0;
}

/* Generation cached at the pre-restart lookup (T056), checked stale in T059. */
static uint32_t g_vfs_gen0 = 0;

/* Phase 11: a dynamic test service registered by cap-transfer (T054), reused by
 * the lookup/unregister tests T063–T066. */
static iris_cptr_t g_ltst_ep = (iris_cptr_t)0;   /* IRIS_CPTR_NULL */
static uint32_t    g_ltst_id = 0;

/* Register endpoint `ep` under `name` via EP_CALL cap-transfer (attached_cap).
 * The dup is a give-away, released once the call lands (A-29); returns the
 * dynamic id, or -(error code). */
long it_register_ep(const char *name, iris_cptr_t ep) {
    /* The master svcmgr keeps must carry DUPLICATE so it can hand each client a
     * fresh WRITE cap on lookup (+TRANSFER so the cap is deliverable to it). */
    iris_rights_t mr = (iris_rights_t)(RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    /* Phase S4 (Step 2): the EP_CALL transfer source is a CSpace slot. */
    long d = it_xfer_dup((long)ep, (uint32_t)mr);
    if (d < 0) return d;
    uint32_t len = it_stage_path(name);
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label               = IRIS_SVCMGR_EP_REGISTER;
    msg.buf_len             = len;
    msg.cap        = (uint32_t)d;
    msg.cap_rights = (uint32_t)mr;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    it_xfer_release(d);
    if (r != 0) return r;
    if (msg.label != IRIS_EP_REPLY_OK) return -(long)(uint32_t)msg.words[0];
    return (long)(uint32_t)msg.words[0];
}

/* T054: cap-backed REGISTER over EP — the caller transfers a REAL endpoint cap
 * (attached_cap) and still gets a working reply (KReply + transfer coexist). */
void test_t054(void) {
    long e = it_ep_create_slot();
    if (e < 0) { it_fail("T054", "endpoint create"); return; }
    g_ltst_ep = (iris_cptr_t)e;

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
void test_t055(void) {
    uint32_t len = it_stage_path(VFS_EP_SVC_NAME);
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_len  = len;
    /* Stage 4: a reply that carries a capability needs somewhere to put it —
     * handle materialisation is retired, so an undeclared receive gets the
     * message without the cap. */
    it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    msg.recv_slot = (uint32_t)IT_LOOKUP_TMP;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
        iris_cptr_t cap = (iris_cptr_t)msg.got_cap;
        struct iris_msg p;
        iris_msg_zero(&p);
        p.label = IRIS_EP_OP_PING;
        long pr  = iris_msg_call((long)cap, &p);          /* WRITE works */
        /* The grant carries no RIGHT_DUPLICATE, so it cannot be derived from.
         * Asked of the slot: SYS_CSPACE_MINT is the derive, and it needs
         * RIGHT_DUPLICATE on the source exactly as the handle dup did. */
        it_slot_delete(IT_SCRATCH_0);
        long dup = it_invoke2((long)cap, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)RIGHT_WRITE);
        if (pr == 0 && p.label == IRIS_EP_REPLY_OK &&
            dup == (long)IRIS_ERR_ACCESS_DENIED)
            ok = 1;
        it_slot_delete(IT_SCRATCH_0);
        it_close(&cap);
    }
    if (ok) it_pass("T055"); else it_fail("T055", ".ep grant not tightened");
}

/* T056: STATUS oracle reports a live service + its generation (cached for T059). */
void test_t056(void) {
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
void test_t057(void) {
    uint32_t a = 0, g0 = 0;
    if (it_status(VFS_EP_SVC_NAME, &a, &g0) != 0 || a != 1u) {
        it_fail("T057", "pre-status"); return;
    }
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_SVCMGR_EP_RESTART;
    msg.words[0] = (uint64_t)SVCMGR_SERVICE_VFS;
    msg.word_count = 1u;
    long r = iris_msg_call((long)IRIS_CPTR_TEST_SUPER, &msg);
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
 * test (tests/kernel/test_knotification.c, Phase 10).  This runtime slot
 * confirms the kbd IRQ-notification WAIT slot is still functional after the
 * lifecycle changes (a non-blocking poll must not fault). */
void test_t058(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_EP_OP_PING;
    long r = iris_msg_call((long)IRIS_CPTR_KBD_EP, &msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK)
        it_pass("T058");
    else
        it_fail("T058", "kbd notification path");
}

/* T059: logical revocation — a client that cached the pre-restart generation
 * (T056) detects, via STATUS, that VFS has since been restarted (generation
 * advanced), so its old generation is stale and a relookup is required. */
void test_t059(void) {
    uint32_t a = 0, g = 0;
    if (it_status(VFS_EP_SVC_NAME, &a, &g) == 0 &&
        a == 1u && g > g_vfs_gen0 && g_vfs_gen0 != 0u)
        it_pass("T059");
    else
        it_fail("T059", "stale generation not detected");
}

/* T060: relookup after restart yields a working cap to the new instance. */
void test_t060(void) {
    uint32_t len = it_stage_path(VFS_EP_SVC_NAME);
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_len  = len;
    /* Stage 4: a reply that carries a capability needs somewhere to put it —
     * handle materialisation is retired, so an undeclared receive gets the
     * message without the cap. */
    it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    msg.recv_slot = (uint32_t)IT_LOOKUP_TMP;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
        iris_cptr_t cap = (iris_cptr_t)msg.got_cap;
        struct iris_msg p;
        iris_msg_zero(&p);
        p.label = IRIS_EP_OP_PING;
        long pr = iris_msg_call((long)cap, &p);
        if (pr == 0 && p.label == IRIS_EP_REPLY_OK) ok = 1;
        it_close(&cap);
    }
    if (ok) it_pass("T060"); else it_fail("T060", "new vfs cap after restart");
}

/* T061: REGISTER cannot spoof a reserved name — ".ep" endpoints and catalog
 * service names are rejected regardless of the caller's badge. */
void test_t061(void) {
    static const char *const reserved[] = {
        VFS_EP_SVC_NAME, CONSOLE_EP_SVC_NAME, "vfs",
    };
    int ok = 1;
    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t len = it_stage_path(reserved[i]);
        struct iris_msg msg;
        iris_msg_zero(&msg);
        msg.label    = IRIS_SVCMGR_EP_REGISTER;
        msg.buf_len  = len;
        long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
        if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
              msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_ACCESS_DENIED))
            ok = 0;
    }
    if (ok) it_pass("T061"); else it_fail("T061", "reserved name spoof");
}

/* T062: badge policy regression guard — core servers still observe the
 * caller's kernel-stamped badge after all lifecycle changes. */
void test_t062(void) {
    uint64_t bv = 0, bc = 0;
    if (it_ping_badge((long)IRIS_CPTR_VFS_EP, &bv) == 0 &&
        bv == IRIS_BADGE_IRIS_TEST &&
        it_ping_badge((long)IRIS_CPTR_CONSOLE_EP, &bc) == 0 &&
        bc == IRIS_BADGE_IRIS_TEST)
        it_pass("T062");
    else
        it_fail("T062", "badge policy regressed");
}

/* ── Phase 11: endpoint cap-transfer & cap-backed REGISTER (T063–T066) ────── */

/* T063: LOOKUP of the cap-registered name returns a REAL, usable endpoint cap —
 * SYS_HANDLE_SAME_OBJECT proves it is the very endpoint object iris_test
 * created and transferred (not a forged number, and not the reply cap). */
void test_t063(void) {
    if (g_ltst_ep == (iris_cptr_t)0) { it_fail("T063", "no ltst ep"); return; }
    uint32_t len = it_stage_path("ltst.svc");
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_len  = len;
    /* Stage 4: a reply that carries a capability needs somewhere to put it —
     * handle materialisation is retired, so an undeclared receive gets the
     * message without the cap. */
    it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    msg.recv_slot = (uint32_t)IT_LOOKUP_TMP;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    int ok = 0;
    if (r == 0 && msg.label == IRIS_EP_REPLY_OK &&
        msg.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
        iris_cptr_t got = (iris_cptr_t)msg.got_cap;
        long ty   = it_invoke0((long)got, INV_CAP_IDENTIFY);
        long same = it_invoke1((long)got, INV_CAP_SAME_OBJECT, (long)g_ltst_ep);
        if (ty == (long)IRIS_HANDLE_TYPE_ENDPOINT && same == 1) ok = 1;
        it_close(&got);
    }
    if (ok) it_pass("T063"); else it_fail("T063", "lookup real cap");
}

/* T064: REGISTER without a cap fails (INVALID_ARG); REGISTER of a wrong-type
 * cap (a notification) also fails — only endpoints are registrable. */
void test_t064(void) {
    int ok = 1;
    /* no cap */
    uint32_t len = it_stage_path("nocap.svc");
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_REGISTER;
    msg.buf_len  = len;                        /* attached_cap = NO_CAP */
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
          msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG)) ok = 0;

    /* wrong type: transfer a notification cap */
    long n = it_notify_create();
    if (n < 0) { it_fail("T064", "notify create"); return; }
    iris_cptr_t notif = (iris_cptr_t)n;
    /* Phase S4 (Step 2): the EP_CALL transfer source is a CSpace slot. */
    long d = it_xfer_dup((long)notif, (uint32_t)RIGHT_WRITE);
    if (d < 0) { it_close(&notif); it_fail("T064", "xfer slot"); return; }
    len = it_stage_path("wrongtype.svc");
    iris_msg_zero(&msg);
    msg.label               = IRIS_SVCMGR_EP_REGISTER;
    msg.buf_len             = len;
    msg.cap        = (uint32_t)d;
    msg.cap_rights = (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER);
    r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    it_xfer_release(d);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
          msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG)) ok = 0;
    it_close(&notif);

    if (ok) it_pass("T064"); else it_fail("T064", "register reject paths");
}

/* T065: UNREGISTER by a non-owner, non-supervisor badge (TEST_B via fixture
 * slot 28) is denied; UNREGISTER by the owner succeeds. */
void test_t065(void) {
    int ok = 1;
    struct iris_msg msg;
    /* wrong badge (0xB2) via the second-identity fixture → ACCESS_DENIED */
    iris_msg_zero(&msg);
    msg.label      = IRIS_SVCMGR_EP_UNREGISTER;
    msg.words[0]   = g_ltst_id;
    msg.word_count = 1u;
    long r = iris_msg_call((long)IRIS_CPTR_TEST_FIX_C, &msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
          msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_ACCESS_DENIED)) ok = 0;

    /* owner badge (IRIS_TEST via slot 1) → OK */
    iris_msg_zero(&msg);
    msg.label      = IRIS_SVCMGR_EP_UNREGISTER;
    msg.words[0]   = g_ltst_id;
    msg.word_count = 1u;
    r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    if (!(r == 0 && msg.label == IRIS_EP_REPLY_OK)) ok = 0;

    if (ok) it_pass("T065"); else it_fail("T065", "unregister owner policy");
}

/* T066: after UNREGISTER the name no longer resolves (NOT_FOUND). */
void test_t066(void) {
    uint32_t len = it_stage_path("ltst.svc");
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_len  = len;
    /* Stage 4: a reply that carries a capability needs somewhere to put it —
     * handle materialisation is retired, so an undeclared receive gets the
     * message without the cap. */
    it_slot_delete((uint32_t)IT_LOOKUP_TMP);
    msg.recv_slot = (uint32_t)IT_LOOKUP_TMP;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    int ok = (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
              msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND);
    if (g_ltst_ep != (iris_cptr_t)0) it_close(&g_ltst_ep);
    if (ok) it_pass("T066"); else it_fail("T066", "lookup after unregister");
}

/* ── Phase 12: endpoint-first svcmgr — DIAG over EP + no legacy fallback ──── */

/* T067: svcmgr DIAG over the endpoint (replaces legacy KChannel SVCMGR_MSG_DIAG
 * as the productive path) returns the expected catalog snapshot. */
void test_t067(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_SVCMGR_EP_DIAG;
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
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
void test_t068(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = UINT64_C(0xF0FE);                   /* not a real svcmgr opcode */
    long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &msg);
    if (r == 0 && msg.label == IRIS_EP_REPLY_ERR &&
        msg.words[0] == (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG)
        it_pass("T068");
    else
        it_fail("T068", "unknown opcode no-fallback");
}

/* ── Phase 13: device-cap CPtr resolution (T069) ─────────────────────────── */

/* T069: a device/authority cap (the spawn KBootstrapCap) minted into a CPtr
 * slot is invocable BY CPtr — SYS_INITRD_COUNT resolves it through CSpace
 * (cspace_or_handle_resolve_obj).  A wrong-type CPtr slot (slot 1 = endpoint)
 * is rejected with ACCESS_DENIED: the namespace split holds and there is no
 * fallback.  This is the prerequisite that lets device caps stop travelling
 * over KChannel at bootstrap. */
void test_t069(void) {
    /* Stage 5 Step 2: the authority named here is the ioport CONTROL
     * capability — one capability, one authority — instead of a second copy of
     * the monolith probed through SYS_INITRD_COUNT.  What the test proves is
     * unchanged and is what its name says: an authority capability is invocable
     * BY CPTR, and a capability of the wrong type in the same argument is
     * refused rather than being looked up somewhere else. */
    it_slot_delete(IT_DEV_SLOT_A);
    long ok_cap = it_ioport_create((long)IRIS_CPTR_IOPORT_CONTROL, 0x2F8, 8, (long)IT_DEV_SLOT_A);
    it_slot_delete(IT_DEV_SLOT_A);
    long wrong  = it_ioport_create((long)IRIS_CPTR_SVCMGR_EP, 0x2F8, 8, (long)IT_DEV_SLOT_A);
    it_slot_delete(IT_DEV_SLOT_A);
    if (ok_cap == 0 && wrong == (long)IRIS_ERR_ACCESS_DENIED)
        it_pass("T069");
    else
        it_fail("T069", "device cap via cptr");
}

/* ── Phase 13 / Track G: retired SYS_CHAN ABI (T070) ─────────────────────── */

/* T070: the ENTIRE SYS_CHAN_* ABI is retired in Track G — KChannel is no longer
 * a productive IPC mechanism.  Every channel syscall number falls through the
 * dispatch to IRIS_ERR_NOT_SUPPORTED.  Args are irrelevant: the dispatch rejects
 * the syscall number before touching them.  Locks the reservation. */
void test_t070(void) {
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

/* ── T071: cascade revoke over the NATIVE CDT (Phase S4, Step 3) ────────────
 *
 * Runtime coverage for recursive revocation.  Until Phase S4 this exercised the
 * legacy handle tree (SYS_CAP_DERIVE/SYS_CAP_REVOKE); that tree is retired and
 * the mechanism is now the CSpace CDT: SYS_CSPACE_MINT derives slot→slot
 * (installing a real MDB child) and SYS_CSPACE_REVOKE removes the whole
 * descendant subtree.  Proves that revoking a root slot transitively destroys
 * child + grandchild while the ROOT ITSELF survives, and that the revoked
 * slots are genuinely empty afterwards — not phantom authority. */
void test_t071(void) {
    long eh = it_ep_create();
    if (eh < 0) { it_fail("T071", "ep create"); return; }
    iris_cptr_t root_h = (iris_cptr_t)eh;

    /* Bridge the endpoint into a scratch slot: it is the derivation ROOT. */
    long root = it_cdt_root(root_h, IT_SCRATCH_0);
    if (root < 0) { it_close(&root_h); it_fail("T071", "root slot"); return; }

    /* child derived from root; grandchild derived from child.  SAME_RIGHTS
     * keeps RIGHT_DUPLICATE so the child can itself be a derivation source. */
    long child  = it_cdt_derive(root, IT_SCRATCH_1, RIGHT_SAME_RIGHTS);
    long gchild = (child >= 0)
                ? it_cdt_derive(child, IT_SCRATCH_2, RIGHT_SAME_RIGHTS)
                : -1;

    int before_ok = (child >= 0) && (gchild >= 0)
                 && it_cdt_alive(child) && it_cdt_alive(gchild)
                 && it_cdt_alive(root);

    /* Revoke transitively deletes child + grandchild; the invoked slot is not
     * its own descendant and must remain valid. */
    long rv = it_cdt_revoke(root);

    int child_dead  = !it_cdt_alive(child);
    int gchild_dead = !it_cdt_alive(gchild);
    int root_alive  = it_cdt_alive(root);

    it_slot_delete(IT_SCRATCH_0);
    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_2);
    it_close(&root_h);

    if (before_ok && rv >= 0 && child_dead && gchild_dead && root_alive)
        it_pass("T071");
    else
        it_fail("T071", "cascade revoke");
}

/* ── T072: derivation rights reduction + revoke failure paths ───────────────
 *
 * Native-CDT form (Phase S4, Step 3).  Proves (a) a cap derived with reduced
 * rights cannot itself be a derivation source once RIGHT_DUPLICATE is dropped
 * (ACCESS_DENIED — no rights escalation), (b) revoking an EMPTY slot fails
 * cleanly (negative error, no panic), and (c) a valid revoke tears down the
 * one child that exists while the invoked slot survives. */
void test_t072(void) {
    long eh = it_ep_create();
    if (eh < 0) { it_fail("T072", "ep create"); return; }
    iris_cptr_t root_h = (iris_cptr_t)eh;

    long root = it_cdt_root(root_h, IT_SCRATCH_0);
    if (root < 0) { it_close(&root_h); it_fail("T072", "root slot"); return; }

    /* Read-only child (drops DUPLICATE/TRANSFER). */
    long ro = it_cdt_derive(root, IT_SCRATCH_1, RIGHT_READ);
    /* Deriving from a cap without RIGHT_DUPLICATE must be denied. */
    long escalate = (ro >= 0)
                  ? it_invoke2(ro, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_2 << 32), (long)RIGHT_SAME_RIGHTS)
                  : 0;

    /* Revoking an EMPTY slot → clean negative error, no panic. */
    it_slot_delete(IT_SCRATCH_3);
    long bad_revoke = it_cdt_revoke((long)IT_SCRATCH_3);

    /* Valid revoke of root deletes its single child (ro). */
    long ok_revoke = it_cdt_revoke(root);
    int  ro_dead   = !it_cdt_alive(ro);
    int  root_alive = it_cdt_alive(root);

    it_slot_delete(IT_SCRATCH_0);
    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_2);
    it_close(&root_h);

    if (ro >= 0 && escalate == (long)IRIS_ERR_ACCESS_DENIED &&
        bad_revoke < 0 && ok_revoke >= 0 && ro_dead && root_alive)
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
void test_t073(void) {
    long ep = it_ep_create_slot();
    if (ep < 0) { it_fail("T073", "ep create"); return; }
    iris_cptr_t ep_h = (iris_cptr_t)ep;

    /* Phase S4 (Step 2): the SOURCE is a CSpace slot.  Three failure shapes,
     * each of which must leave the source cap exactly where it was. */
    iris_cptr_t root = T28_OWN_ROOT_CNODE;

    /* (a) A source slot WITHOUT RIGHT_TRANSFER → ACCESS_DENIED, slot intact.
     * Stage 4: the fixture is a slot, so the reduced copy is a slot-to-slot
     * derive; SYS_CNODE_MINT's source is handle-only. */
    it_slot_delete(IT_XFER_SLOT_C);
    if (it_invoke2(ep, INV_CSPACE_MINT, (long)((uint64_t)IT_XFER_SLOT_C << 32), (long)RIGHT_READ) != 0) {
        it_close(&ep_h); it_fail("T073", "mint notrans"); return;
    }
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label           = 0x73;
    msg.cap = IT_XFER_SLOT_C;
    msg.cap_rights = (uint32_t)RIGHT_READ;
    long a = iris_msg_nb_send(ep, &msg);
    int  denied = (a == (long)IRIS_ERR_ACCESS_DENIED);
    /* not consumed: the slot still resolves */
    int  preserved = (it_invoke0((long)IT_XFER_SLOT_C, INV_CAP_IDENTIFY) >= 0);

    /* (b) An EMPTY source slot → clean NOT_FOUND (no cap, nothing staged). */
    (void)it_invoke1((long)root, INV_CNODE_DELETE, (long)IT_XFER_SLOT_D);
    struct iris_msg msg2;
    iris_msg_zero(&msg2);
    msg2.label           = 0x73;
    msg2.cap = IT_XFER_SLOT_D;
    msg2.cap_rights = (uint32_t)RIGHT_TRANSFER;
    long b = iris_msg_nb_send(ep, &msg2);
    int  empty_clean = (b == (long)IRIS_ERR_NOT_FOUND);

    /* (c) A MALFORMED source CPtr → INVALID_ARG, with nothing staged.  This
     * used to feed a real HANDLE value and assert the transfer source lives in
     * exactly one namespace; there is one namespace now, so the equivalent
     * hostile input is a CPtr that does not address a capability. */
    struct iris_msg msg3;
    iris_msg_zero(&msg3);
    msg3.label           = 0x73;
    msg3.cap = (uint32_t)(IT_XFER_SLOT_C | (1u << 16));  /* alias */
    msg3.cap_rights = (uint32_t)RIGHT_TRANSFER;
    int handle_rejected = (iris_msg_nb_send(ep, &msg3) ==
                           (long)IRIS_ERR_INVALID_ARG);

    it_slot_delete(IT_XFER_SLOT_C);
    it_close(&ep_h);

    if (denied && preserved && empty_clean && handle_rejected)
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
static iris_cptr_t g_t074_ep_h  = IRIS_CPTR_NULL;
static volatile int g_t074_done = 0;
static          int g_t074_r1   = 999;
static          int g_t074_r2   = 999;
static uint8_t      g_t074_stack[8192];

static void t074_server(void) {
    struct iris_msg msg;
    iris_msg_zero(&msg);
    long rr = (msg.reply = 93, iris_msg_recv((long)g_t074_ep_h, &msg));
    if (rr == 0) {
        iris_cptr_t reply_h = (iris_cptr_t)msg.got_cap;
        struct iris_msg rmsg;
        iris_msg_zero(&rmsg);
        rmsg.label = 0x74;
        g_t074_r1 = (int)iris_msg_reply((long)reply_h, &rmsg);
        /* Second reply on the consumed one-shot cap must be rejected. */
        struct iris_msg rmsg2;
        iris_msg_zero(&rmsg2);
        rmsg2.label = 0x74;
        g_t074_r2 = (int)iris_msg_reply((long)reply_h, &rmsg2);
    } else {
        g_t074_r1 = (int)rr;
    }
    g_t074_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t074(void) {
    g_t074_done = 0; g_t074_r1 = 999; g_t074_r2 = 999;

    long ep = it_ep_create();
    if (ep < 0) { it_fail("T074", "ep create"); return; }
    g_t074_ep_h = (iris_cptr_t)ep;
    if (it_reply_create_at(93) < 0) {
        it_close(&g_t074_ep_h);
        it_fail("T074", "reply create"); return;
    }

    uint64_t entry = (uint64_t)(uintptr_t)t074_server;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t074_stack + sizeof(g_t074_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, 0);
    if (tid < 0) { it_close(&g_t074_ep_h); it_fail("T074", "thread create"); return; }
    iris_cptr_t tid_h = (iris_cptr_t)tid;

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = 0x74;
    long r = iris_msg_call(ep, &msg);

    for (int i = 0; i < 200 && !g_t074_done; i++)
        it_settle(1);

    it_close(&tid_h);
    it_close(&g_t074_ep_h);
    it_slot_delete(93);

    if (r == 0 && g_t074_r1 == 0 && g_t074_r2 == (int)IRIS_ERR_NOT_FOUND)
        it_pass("T074");
    else
        it_fail("T074", "reply one-shot");
}

/* Spawn a lifecycle_probe child, minting `cmd_ep_h` into its command slot.
 * Returns 0 and fills *out_proc_h on success, or a negative error. */
/* Stage 7 Step 9: `cn_leaf` is where the child's ROOT CSpace is kept, so the
 * caller can go on minting into it — naming the CSpace it holds instead of the
 * process it does not. */
long lp_spawn_child_cn(uint32_t cn_leaf, iris_cptr_t cmd_ep_h,
                              iris_cptr_t *out_proc_h) {
    struct svc_mint mints[2] = { 0 };
    uint32_t n = 0;
    mints[n].slot   = LP_CPTR_CMD_EP;
    /* A CPtr source wins over src_h in svc_mint and mints slot-to-slot, so the
     * child's capability is an MDB child of ours. */
    if (((uint32_t)cmd_ep_h & IRIS_CPTR_LIMIT) == 0u && cmd_ep_h != 0)
        mints[n].src_cptr = (uint64_t)cmd_ep_h;
    else
        IT_MINT_SRC(mints[n], cmd_ep_h);
    mints[n].rights = RIGHT_READ | RIGHT_WRITE;
    mints[n].badge  = 0;
    n++;
    /* Phase S1: the child serves EP_CALLs on its command endpoint, so it needs
     * an explicit reply object at slot 13 (LP_CPTR_REPLY).  Retyped fresh
     * from the test untyped; the parent drops its handle right after the
     * mint so child death still fires close-wakes-caller. */
    iris_cptr_t reply_h = IRIS_CPTR_NULL;
    {
        long rr = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
        if (rr >= 0) {
            reply_h = (iris_cptr_t)rr;
            mints[n].slot   = 13u;
            IT_MINT_SRC(mints[n], reply_h);
            mints[n].rights = RIGHT_READ | RIGHT_WRITE;
            mints[n].badge  = 0;
            n++;
        }
    }
    iris_cptr_t boot_h = IRIS_CPTR_NULL;
    *out_proc_h = IRIS_CPTR_NULL;
    if (cn_leaf) it_slot_delete(IT_OBJ_CPTR(IT_CHILD_CN_LEAF(cn_leaf - 1u)));
    long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "lifecycle_probe",
                             out_proc_h, &boot_h, mints, n,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/0,
                               cn_leaf ? IT_CHILD_CN_DEST(cn_leaf - 1u) : 0u,
                               it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(*out_proc_h);
    it_close(&reply_h);  /* the child's slot-13 mint is the only reply cap */
    it_close(&boot_h);   /* Track I: no bootstrap channel (IRIS_CPTR_NULL anyway) */
    return r;
}

/* The common case: a child the caller does not delegate into after spawn. */
long lp_spawn_child(iris_cptr_t cmd_ep_h, iris_cptr_t *out_proc_h) {
    return lp_spawn_child_cn(0u, cmd_ep_h, out_proc_h);
}

/* ── T075: spawn/exit smoke ─────────────────────────────────────────────────
 * Foundation test: spawn the child, drive it to run and exit via one command
 * endpoint, and observe its exit code — proving the harness works end to end. */
void test_t075(void) {
    long ep = it_ep_create();
    if (ep < 0) { it_fail("T075", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;

    iris_cptr_t proc_h = IRIS_CPTR_NULL;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h);
        it_fail("T075", "spawn"); return;
    }

    /* Watch the child for exit on a notification, bit 0. */
    long n = it_notify_create();
    iris_cptr_t watch_h = (n >= 0) ? (iris_cptr_t)n : IRIS_CPTR_NULL;
    int watch_ok = (watch_h != IRIS_CPTR_NULL) &&
                   (it_invoke2(it_child_tcb((long)proc_h), INV_TCB_WATCH, (long)watch_h, 1) == 0);

    /* Drive the child: EP_NB_SEND until it is blocked in EP_RECV (bounded). */
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = 0x75;
    int sent = 0;
    for (int i = 0; i < 300 && !sent; i++) {
        if (iris_msg_nb_send((long)cmd_ep_h, &msg) == 0) sent = 1;
        else it_settle(1);
    }

    /* Wait (bounded) for the death signal, then read the exit code. */
    uint64_t bits = 0;
    long ws = watch_ok
        ? it_wait_timeout( (long)watch_h, (long)(uintptr_t)&bits, 2000000000L)
        : -1;
    long code = it_invoke0(it_child_tcb((long)proc_h), INV_TCB_EXIT_CODE);

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
void test_t077(void) {
    long ep = it_ep_create();
    if (ep < 0) { it_fail("T077", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;

    iris_cptr_t proc_h = IRIS_CPTR_NULL;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h);
        it_fail("T077", "spawn"); return;
    }

    /* Let the child reach EP_RECV and block. */
    it_settle(10);

    /* Kill the child while it is blocked (RIGHT_MANAGE on the child handle). */
    long kr   = it_kill((long)proc_h);
    int  dead = (it_alive((long)proc_h) == 0);

    /* Blocked recv must have been cancelled: no stale receiver remains. */
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = 0x77;
    long s = iris_msg_nb_send((long)cmd_ep_h, &msg);
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
void test_t078(void) {
    long ep = it_ep_create();
    if (ep < 0) { it_fail("T078", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;

    iris_cptr_t proc_h = IRIS_CPTR_NULL;
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h);
        it_fail("T078", "spawn"); return;
    }

    /* Client call: rendezvous with the child's recv, then the child exits
     * unanswered.  EP_CALL must return CLOSED, not hang. */
    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label    = 0x78;
    long r = iris_msg_call((long)cmd_ep_h, &msg);

    /* Confirm the child actually died (bounded). */
    int dead = 0;
    for (int i = 0; i < 200 && !dead; i++) {
        if (it_alive((long)proc_h) == 0) dead = 1;
        else it_settle(1);
    }

    it_close(&proc_h);
    it_close(&cmd_ep_h);

    if (r == (long)IRIS_ERR_CLOSED && dead)
        it_pass("T078");
    else
        it_fail("T078", "server death client recovery");
}

void test_t076(void) {
    long ep = it_ep_create();
    if (ep < 0) { it_fail("T076", "ep create"); return; }
    iris_cptr_t cmd_ep_h = (iris_cptr_t)ep;

    iris_cptr_t proc_h = IRIS_CPTR_NULL;
    /* This test maps into the child, so the spawn keeps its address space. */
    it_child_keep_vspace();
    if (lp_spawn_child(cmd_ep_h, &proc_h) < 0 || proc_h == IRIS_CPTR_NULL) {
        it_close(&cmd_ep_h);
        it_fail("T076", "spawn"); return;
    }

    /* One-page VMO mapped writable into the CHILD's address space. */
    long vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);
    iris_cptr_t vmo_h = (vmo >= 0) ? (iris_cptr_t)vmo : IRIS_CPTR_NULL;
    long mi = (vmo_h != IRIS_CPTR_NULL)
        ? it_invoke((long)vmo_h, INV_FRAME_MAP, it_child_vspace(proc_h), (long)LP_MAP_VA, 1)
        : -1;

    /* Let the child reach EP_RECV and block (mapping stays live). */
    it_settle(10);

    /* Kill the child while the mapping is live — teardown must auto-unmap it. */
    long kr   = it_kill((long)proc_h);
    int  dead = (it_alive((long)proc_h) == 0);

    /* VMO must have survived intact: re-map into the PARENT and read/write it. */
    int reusable = 0;
    long pm = it_invoke((long)vmo_h, INV_FRAME_MAP, IT_VS, (long)LP_MAP_VA, 1);
    if (pm == 0) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)LP_MAP_VA;
        p[0] = 0xA5; p[4095] = 0x5A;
        reusable = (p[0] == 0xA5 && p[4095] == 0x5A);
        it_invoke2((long)vmo_h, INV_FRAME_UNMAP, IT_VS, (long)LP_MAP_VA);
    }

    it_child_drop_vspace(proc_h);   /* Step 15: give the child's back */
    it_close(&vmo_h);
    it_close(&proc_h);
    it_close(&cmd_ep_h);

    if (mi == 0 && kr == 0 && dead && reusable)
        it_pass("T076");
    else
        it_fail("T076", "child mapping teardown");
}
