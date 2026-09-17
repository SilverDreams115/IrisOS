/*
 * it_base.c — what every test in the suite is built out of.
 *
 * Serial output, pass/fail accounting, the CSpace slot helpers and the
 * rotating object pool, object fabrication (retype into a slot), the child
 * and thread bookkeeping, and the bounded wait that asks the timer service
 * instead of the kernel (ledger A-24).
 *
 * Nothing here tests anything.  It is the part of the old single file that
 * every one of the test files needs, which is why it is its own.
 */
#include "it_priv.h"


/* ── Serial output ──────────────────────────────────────────────────────── */

iris_cptr_t g_serial_h = IRIS_CPTR_NULL;

void it_serial_write(const char *s) {
    if (g_serial_h == IRIS_CPTR_NULL || !s) return;
    while (*s) {
        long v;
        do {
            v = it_invoke1((long)g_serial_h, INV_IOPORT_IN, 5);
        } while (v < 0 || !((uint8_t)v & 0x20u));
        (void)it_invoke2((long)g_serial_h, INV_IOPORT_OUT, 0, (long)(uint8_t)*s++);
    }
}

void it_log_num(uint32_t n) {
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

/* The same, in hex, because an address printed in decimal is an address
 * nobody can compare against a boot log or a QEMU monitor dump. */
void it_log_hex(uint64_t v) {
    char buf[19];
    uint32_t i = 18;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    } else {
        while (v > 0) {
            uint32_t d = (uint32_t)(v & 0xFu);
            buf[--i] = (char)(d < 10u ? ('0' + (int)d) : ('a' + (int)(d - 10u)));
            v >>= 4;
        }
    }
    it_serial_write("0x");
    it_serial_write(buf + i);
}

/* ── Test framework ─────────────────────────────────────────────────────── */

uint32_t g_pass  = 0;
uint32_t g_total = 0;
static struct it_child g_it_children[IT_CHILD_MAX];
static uint32_t g_it_child_next;

static void it_child_record(iris_cptr_t proc_h, uint32_t leaf) {
    /* A process CPtr is a slot, and a slot is reused: drop any stale entry
     * naming the same one first, or a dead child's thread would answer for a
     * live child that happened to land in its slot. */
    for (uint32_t i = 0; i < IT_CHILD_MAX; i++)
        if (g_it_children[i].proc == (uint32_t)proc_h) g_it_children[i].leaf = 0;
    uint32_t k = __atomic_fetch_add(&g_it_child_next, 1u, __ATOMIC_RELAXED)
                 % IT_CHILD_MAX;
    g_it_children[k].proc = (uint32_t)proc_h;
    g_it_children[k].leaf = leaf;
}

/* Claim the next child-thread leaf, clearing whatever was in it.  The leaf is
 * remembered so the spawn that follows can bind it to the process capability
 * it produced — the two halves of one record, written where each is known. */
static uint32_t g_it_child_pending;
/*
 * The address space is kept only when the next spawn is ASKED to keep it.
 *
 * Not because it is awkward to hold, but because holding it is a real cost the
 * suite is the auditor of: a VSpace capability keeps that address space and
 * every page table in it alive past the child's death, and the drift checks
 * that end most tests count exactly that.  Making it opt-in is the same
 * decision svc_loader's `keep_vspace_dest` states — a spawner with no reason
 * to map into its child holds nothing — enforced here by the tests that would
 * otherwise fail.
 */
static uint8_t g_it_child_keep_vs;
void it_child_keep_vspace(void) { g_it_child_keep_vs = 1u; }
void it_child_bind(iris_cptr_t proc_h) {
    if (proc_h != IRIS_CPTR_NULL && g_it_child_pending)
        it_child_record(proc_h, g_it_child_pending);
    g_it_child_pending = 0;
    g_it_child_keep_vs = 0u;
}

/*
 * Claim the pair of leaves the next spawn will publish into, emptying both
 * first.  Idempotent until it_child_bind consumes it, which is what makes the
 * two accessors below safe to pass as two arguments of one call: C does not
 * order argument evaluation, so whichever runs first claims and the other sees
 * the same leaf.
 *
 * Emptying the VSpace leaf here is not tidiness.  A capability to a dead
 * child's address space keeps that address space alive, and with it every page
 * table in it — each a child entry on a budget the loader wants to RESET for
 * the next spawn.  Recycling this slot is where the suite lets go.
 */
static uint32_t it_child_pending_leaf(void) {
    if (!g_it_child_pending) {
        uint32_t leaf = IT_CHILD_TCB_LEAF(
            __atomic_load_n(&g_it_child_next, __ATOMIC_RELAXED) % IT_CHILD_MAX);
        (void)it_invoke1((long)IT_CHILD_CN_SLOT, INV_CNODE_DELETE, (long)leaf);
        g_it_child_pending = leaf;
    }
    return g_it_child_pending;
}
long it_child_tcb_dest(void) {
    return (long)IT_CHILD_CN_DEST_(it_child_pending_leaf());
}
long it_child_vs_dest(void) {
    if (!g_it_child_keep_vs) return 0;
    uint32_t vs_leaf = it_child_pending_leaf() + IT_CHILD_MAX;
    /* Emptied HERE rather than beside the thread leaf, and only when a VSpace
     * is actually being claimed.  Deleting it on every spawn would destroy a
     * previous child's address space at the moment a new one is created, and
     * the two cancel in the live-VSpace gauge — which T136 reads as "the child
     * I just spawned was not counted".  A recycle must not be invisible. */
    (void)it_invoke1((long)IT_CHILD_CN_SLOT, INV_CNODE_DELETE, (long)vs_leaf);
    return (long)IT_CHILD_CN_DEST_(vs_leaf);
}

/* The thread a child was started with, or 0 if this child was not recorded. */
/*
 * Stage 7-proc: the table's THREAD half has collapsed into an identity.
 *
 * It existed because the capability the suite passed around named a PROCESS
 * and the operations named a thread, so something had to map one to the other
 * — the smallest form of the table a process server keeps.  A spawn hands back
 * the child's first THREAD now (svc_loader retypes it into the process leaf,
 * and it is what claims the leaf), so the mapping is the identity and saying
 * so is better than looking it up: the registration then names the slot the
 * suite actually holds for the child's life, rather than a minted copy in a
 * side CNode whose lifetime is the table's.
 *
 * The table survives for the VSPACE half, which is still a separate leaf.
 */
long it_child_tcb(iris_cptr_t proc_h) {
    return (long)proc_h;
}
/*
 * Stage 7 Step 15 — and the address space it runs in.
 *
 * SYS_PROCESS_VSPACE used to answer this: the kernel read `child->vspace` out
 * of a KProcess, so a supervisor reached an object it did not hold by naming a
 * different one.  The loader RETYPED this VSpace and held it through the whole
 * spawn; it only threw it away at the end.  Now it hands it over, and the
 * table remembers it beside the thread.
 */
long it_child_vspace(iris_cptr_t proc_h) {
    for (uint32_t k = 0; k < IT_CHILD_MAX; k++)
        if (g_it_children[k].proc == (uint32_t)proc_h && g_it_children[k].leaf)
            return (long)IT_CHILD_TCB_CPTR(g_it_children[k].leaf + IT_CHILD_MAX);
    return 0;
}
/* Let go of a child's address space without waiting for its table slot to be
 * recycled — the discipline svc_loader's `keep_vspace_dest` documents: a
 * VSpace capability keeps every page table in that address space alive, and
 * those are children of a budget somebody wants back. */
void it_child_drop_vspace(iris_cptr_t proc_h) {
    for (uint32_t k = 0; k < IT_CHILD_MAX; k++)
        if (g_it_children[k].proc == (uint32_t)proc_h && g_it_children[k].leaf)
            (void)it_invoke1((long)IT_CHILD_CN_SLOT, INV_CNODE_DELETE, (long)(g_it_children[k].leaf + IT_CHILD_MAX));
}
/*
 * Stage 7 Step 13 — killing a child is stopping the EXECUTION you hold.
 *
 * SYS_PROCESS_KILL took a process capability and stopped every thread in it.
 * Every child the suite spawns is single-threaded (svc_loader composes exactly
 * one), so exiting its first thread ends it — and the process OBJECT goes when
 * the last capability to it does, because a thread holds a reference to the
 * process it joined and the creation reference is dropped as soon as
 * SYS_PROCESS_CREATE has published a capability.
 *
 * A child not in the table has no thread the suite can name, and 0 is
 * IRIS_CPTR_NULL, so this reports INVALID_ARG rather than pretending to kill.
 */
long it_kill(long proc_cptr) {
    return it_invoke0(it_child_tcb((iris_cptr_t)proc_cptr), INV_TCB_EXIT);
}

/*
 * Stage 7 Step 13 — "is it still running" asked of the EXECUTION.
 *
 * SYS_PROCESS_STATUS answered 1 or 0 for a process.  A thread answers with its
 * STATE, which is more than the question asked for and is the point: the two
 * values that mean "not running any more" are TERMINATED (execution over, the
 * object may outlive it through a surviving capability) and DEAD (the backing
 * slot is free), and every other value is a way of being alive.  Errors travel
 * unchanged so a caller can still tell "not alive" from "cannot ask".
 */
long it_tcb_alive(long tcb_cptr) {
    struct iris_tcb_info info;
    info.state = 0u;
    long r = it_invoke1(tcb_cptr, INV_TCB_GET_INFO, (long)(uintptr_t)&info);
    if (r != 0) return r;
    return (info.state == IT_TASK_TERMINATED || info.state == IT_TASK_DEAD) ? 0 : 1;
}
long it_alive(long proc_cptr) {
    return it_tcb_alive(it_child_tcb((iris_cptr_t)proc_cptr));
}

/* Fresh reply authority for target `i`: the object is one-shot, so a test that
 * serves several faults retypes one per fault.  1 on success. */
int it_fault_reply_fresh(uint32_t i) {
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)IT_FAULT_LEAF(i));
    return it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_REPLY | (1ULL << 32)), (long)((uint64_t)IT_OBJ_CNODE_SLOT |
                          ((uint64_t)IT_FAULT_LEAF(i) << 32)), 0) == 0;
}

/*
 * Retype a fresh reply-object CNode for a pager about to be spawned, and fill
 * the first `nleaves` of it.  1 on success.
 *
 * How many leaves are filled is AUTHORITY, not bookkeeping: a leaf holding a
 * reply object is a slot the pager can receive a fault with, and an empty one
 * is a fault it can never take delivery of.  T184's containment battery rests
 * on exactly that — the victim's leaf is never filled, so the pager cannot
 * receive the victim's fault even though both travel on endpoints.
 */
int it_pgr_mbox_fresh(uint32_t nleaves) {
    (void)it_invoke1(0, INV_CNODE_DELETE, (long)IT_PGR_MBOX_SLOT);
    if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_CNODE | (1ULL << 32)), (long)((uint64_t)IT_PGR_MBOX_SLOT << 32), (long)IT_PGR_MBOX_SLOTS) != 0) return 0;
    /* Leaf 0 is the CNode's guard slot. */
    for (uint32_t leaf = 1; leaf <= nleaves; leaf++) {
        if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_REPLY | (1ULL << 32)), (long)((uint64_t)IT_PGR_MBOX_SLOT |
                           ((uint64_t)leaf << 32)), 0) != 0) return 0;
    }
    return 1;
}
uint32_t g_it_obj_slot_next;

long it_retype2_at(long ut, uint32_t obj_type, uint32_t slot,
                          uint32_t count, long obj_arg) {
    return it_invoke(ut, INV_UNTYPED_RETYPE, (long)((uint64_t)obj_type | ((uint64_t)count << 32)), (long)((uint64_t)slot << 32), obj_arg);
}

/*
 * Take the next leaf of the rotating object pool.
 *
 * Every fabrication helper below had this same five lines inlined, and they
 * all did the same dangerous thing silently: delete whatever is in the leaf
 * before installing.  That delete is correct when the previous occupant was
 * abandoned by a finished test — which is the pool's contract — and it
 * DESTROYS A LIVE CAPABILITY when it was not.
 *
 * So the take is counted.  `g_it_pool_evictions` is the number of times this
 * run the allocator recycled a leaf that still held something, which is the
 * measurement that matters: T324 counts the debt AT REST, and a debt at rest
 * is harmless until the counter comes round to it.  This counts the coming
 * round.
 */
uint32_t g_it_pool_evictions = 0;
uint32_t g_it_pool_evict_by_type[20] = { 0 };

static uint32_t it_pool_leaf_take(void) {
    uint32_t leaf = IT_OBJ_POOL_FIRST +
                    (__atomic_fetch_add(&g_it_obj_slot_next, 1u,
                                        __ATOMIC_RELAXED) %
                     (IT_OBJ_SLOT_SPAN - IT_OBJ_POOL_FIRST));
    long t = it_invoke0((long)IT_OBJ_CPTR(leaf), INV_CAP_IDENTIFY);
    if (t >= 0) {
        __atomic_fetch_add(&g_it_pool_evictions, 1u, __ATOMIC_RELAXED);
        if (t < 20) __atomic_fetch_add(&g_it_pool_evict_by_type[t], 1u,
                                       __ATOMIC_RELAXED);
    }
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)leaf);
    return leaf;
}

long it_retype_slot_alloc(long ut, uint32_t obj_type, long obj_arg) {
    uint32_t leaf = it_pool_leaf_take();
    long r = it_invoke(ut, INV_UNTYPED_RETYPE, (long)((uint64_t)obj_type | (1ULL << 32)), (long)((uint64_t)IT_OBJ_CNODE_SLOT |
                            ((uint64_t)leaf << 32)), obj_arg);
    return (r < 0) ? r : (long)IT_OBJ_CPTR(leaf);
}

/* Step 6c: the CSpace form of "a rights-reduced copy of this capability".
 *
 * SYS_HANDLE_DUP's replacement.  It derives src_cptr into a fresh leaf of the
 * second-level CNode — a real MDB child of the source, which the handle dup
 * never was — and returns the CPtr.  Released with it_close like any other
 * capability the suite fabricates.  Same rotating-pool contract as
 * it_retype_slot_alloc: delete before minting, never hold across a test. */
/* ...and the badged form: a copy of `src_cptr` stamped with `badge`, which is
 * how one endpoint serves many clients distinguishably (A-22 uses it to say
 * WHICH target a fault came from). */
long it_cs_badge(long src_cptr, uint32_t rights, uint32_t badge) {
    uint32_t leaf = it_pool_leaf_take();
    long r = it_invoke2(src_cptr, INV_CSPACE_MINT, (long)(((uint64_t)leaf << 32) | (uint64_t)IT_OBJ_CNODE_SLOT), (long)((uint64_t)rights | ((uint64_t)badge << 32)));
    return (r != 0) ? r : (long)IT_OBJ_CPTR(leaf);
}

long it_cs_reduce(long src_cptr, uint32_t rights) {
    uint32_t leaf = it_pool_leaf_take();
    long r = it_invoke2(src_cptr, INV_CSPACE_MINT, (long)(((uint64_t)leaf << 32) | (uint64_t)IT_OBJ_CNODE_SLOT), (long)rights);
    return (r != 0) ? r : (long)IT_OBJ_CPTR(leaf);
}

/*
 * ── Ledger A-24: waiting, without a kernel that knows how to wait ──────────
 *
 * `SYS_SLEEP`, `SYS_CLOCK_NANOSLEEP` and `SYS_NOTIFY_WAIT_TIMEOUT` are
 * retired.  What the suite used them for was two different things wearing one
 * syscall, and they are separated here because only one of them was ever about
 * TIME:
 *
 *   - "let the child reach its blocking syscall" is a SCHEDULING request.  It
 *     used SYS_SLEEP(1..10) because a timed block happened to be there, and
 *     what it wanted was for somebody else to run.  `it_settle` yields, which
 *     says that, needs no timer service, and is safe to call from any thread.
 *
 *   - "fail instead of hanging if this event never comes" IS about time, and
 *     is now a request to the timer service: arm it to signal the same
 *     notification with a reserved bit, then wait normally.  The bit is how a
 *     timeout is told from the event.
 */
/*
 * "Let the others run" — and on more than one processor that stopped being
 * the same thing as "yield a lot" (SMP roadmap §9.3 step 4).
 *
 * It was a fixed count of yields, `rounds * 16 + 8`.  On one core that WAS a
 * wait: every yield handed the CPU to somebody else and did not come back
 * until they had run, so counting yields and counting other threads' turns
 * were the same count.  On four cores it is not a wait at all.  The yields
 * cycle THIS core's run queue at syscall speed while the thread being waited
 * for sits in another core's queue, and eighty-eight of them can pass before
 * that core has taken a single timer interrupt.
 *
 * Three tests failed exactly this way when threads began to spread across
 * processors — a helper's counter had not moved (T083), a killed thread had
 * not been reaped (T118, T119).  None of them was a race in the kernel; each
 * was a wait that had quietly stopped waiting.
 *
 * So a floor of real elapsed TIME goes underneath the yields, one scheduler
 * tick per round, because a tick is the unit in which another core actually
 * gets around to its run queue.  The yields stay: they are still how this
 * thread gives its own core away, and on one processor they are still what
 * makes the wait short.
 *
 * `SYS_CLOCK_GET` and not the timer service: this is called from tests that
 * have not looked the timer up yet, from tests that are testing the timer, and
 * from threads with no capability to it.  The iteration cap is there so that a
 * clock which answers nothing degrades this to exactly what it used to be
 * rather than hanging the suite.
 */
/*
 * A bounded wait for something ANOTHER THREAD has to do.  See IT_AWAIT in
 * it_priv.h for why it is shaped this way.
 */
void it_await_open(struct it_await *w, uint32_t yields) {
    if (!w) return;
    w->t0     = 0;
    w->yields = yields ? yields : 1u;
    w->spins  = 0u;
}

int it_await_more(struct it_await *w) {
    if (!w) return 0;
    (void)it_sys1(SYS_YIELD, 0);
    w->spins++;

    /* Phase 1: the yields the loop this replaces did, and no more. */
    if (w->spins < w->yields) return 1;

    /* Phase 2: one scheduler tick, which is the unit in which another core
     * gets around to its run queue.  Entered only when phase 1 came up empty —
     * which on one processor means the wait was going to fail anyway. */
    if (w->spins == w->yields) { w->t0 = it_sys0(SYS_CLOCK_GET); return 1; }
    if (w->spins > w->yields + IT_AWAIT_TAIL_SPINS) return 0;
    if (w->t0 <= 0) return 0;                       /* no clock: yields only */
    long now = it_sys0(SYS_CLOCK_GET);
    if (now <= 0) return 0;
    return (uint64_t)(now - w->t0) < IRIS_TICK_NS;
}

void it_settle(uint32_t rounds) {
    uint32_t yields = rounds * 16u + 8u;
    long     t0     = it_sys0(SYS_CLOCK_GET);
    uint64_t want   = (uint64_t)rounds * IRIS_TICK_NS;
    uint32_t cap    = yields * 64u;

    for (uint32_t i = 0; i < yields; i++) (void)it_sys1(SYS_YIELD, 0);
    if (t0 <= 0 || want == 0u) return;

    for (uint32_t i = 0; i < cap; i++) {
        long now = it_sys0(SYS_CLOCK_GET);
        if (now <= 0) return;                       /* no clock: yields only */
        if ((uint64_t)(now - t0) >= want) return;
        (void)it_sys1(SYS_YIELD, 0);
    }
}

/*
 * Ledger A-27: ask the clock's OWNER what time it is.
 *
 * `SYS_CLOCK_GET` handed any task a timestamp for the asking and is retired.
 * The timer service counts the ticks of the line it holds, which is a clock,
 * and answers for anybody it was granted to.
 */
long it_timer_uptime(uint64_t *out_ns) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label = TMR_OP_UPTIME;
    long r = iris_msg_call((long)IRIS_CPTR_TIMER_EP, &m);
    if (r != 0) return r;
    if (out_ns) *out_ns = m.words[0];
    return 0;
}

/*
 * A bounded wait.  0 and the observed bits, or IRIS_ERR_TIMED_OUT.
 *
 * The signature is the one SYS_NOTIFY_WAIT_TIMEOUT had, deliberately: what
 * changed is who does the waiting, not what a caller asks for.
 *
 * A stale timeout from an earlier bounded wait that ended early is drained
 * first.  That staleness is inherent — an armed timer nobody wants still fires
 * — and it used to be hidden by the kernel cancelling the deadline when the
 * thread woke, which is precisely the bookkeeping about somebody else's
 * waiting that it should not have been doing.
 */
long it_wait_timeout(long notif, long out_bits_uptr, long ns) {
    uint64_t *out = (uint64_t *)(uintptr_t)out_bits_uptr;
    uint64_t  bits = 0;

    if (it_invoke1(notif, INV_NOTIFY_POLL, (long)(uintptr_t)&bits) == 0) {
        bits &= ~IRIS_TIMER_BIT;
        if (bits) { if (out) *out = bits; return 0; }
    }

    long give = it_cs_reduce(notif, RIGHT_WRITE | RIGHT_TRANSFER);
    if (give < 0) return give;
    uint64_t token = 0;
    if (iris_timer_arm((long)IRIS_CPTR_TIMER_EP, give, IRIS_TIMER_BIT,
                       (uint64_t)ns, &token) != 0) {
        /* A refused arm leaves the copy where it was: give the leaf back
         * rather than letting the rotating pool evict it later (T324). */
        it_slot_delete((uint32_t)give);
        return (long)IRIS_ERR_NOT_FOUND;
    }
    /* Ledger A-29: the transfer is a COPY — the service now holds a derivation
     * CHILD of `give`, and this slot is ours to drop.  Dropping it is the whole
     * point of deriving it: what the service keeps is a capability to signal
     * this notification and nothing else, and it stops being reachable from
     * here.  (Under the old MOVE the send consumed the slot for us, which is
     * why leaving it behind went unnoticed until the counts moved.) */
    it_slot_delete((uint32_t)give);

    for (;;) {
        bits = 0;
        long r = it_invoke1(notif, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits);
        if (r != 0) { (void)iris_timer_cancel((long)IRIS_CPTR_TIMER_EP, token); return r; }
        if (bits & ~IRIS_TIMER_BIT) {
            /* The event won.  Take the timer back rather than leaving the
             * service holding a deadline nobody is waiting for. */
            (void)iris_timer_cancel((long)IRIS_CPTR_TIMER_EP, token);
            if (out) *out = bits & ~IRIS_TIMER_BIT;
            return 0;
        }
        if (bits & IRIS_TIMER_BIT) { if (out) *out = 0; return (long)IRIS_ERR_TIMED_OUT; }
    }
}


/* A KVMO published into a CSpace slot instead of a handle (Stage 4: arg2 of
 * SYS_VMO_CREATE is a destination slot).  Same rotating-pool contract as
 * it_retype_slot_alloc; released with it_close.
 *
 * A KVMO is fabricated from kernel memory rather than retyped from an Untyped,
 * so the slot is an MDB LEGACY root — that is KVMO's own debt (ledger: FROZEN,
 * memory-server), not something the slot introduces.  What changes here is
 * only WHERE the capability lives. */
/*
 * Stage 7 Step 14 — a device capability is charged to a budget the caller
 * names.  base and count share arg1 (base | count << 16), which frees arg2 to
 * say who pays; the suite pays out of its own delegated pool.
 *
 * Wrapped rather than spelled out at eighteen call sites, because what those
 * sites are testing is AUTHORITY — which control capability creates which
 * device — and the packing is not the subject of a single one of them.
 */
/* `dest` is a SLOT of the caller's own root here; the syscall takes the
 * standard destination packing (cnode | slot<<32), so the wrapper does the
 * shift and the eighteen call sites keep saying which slot they mean. */
long it_ioport_create(long auth, long base, long count, long dest) {
    return it_invoke(auth, INV_BOOT_CREATE_IOPORT, (long)((uint64_t)(uint16_t)base |
                          ((uint64_t)(uint16_t)count << 16)), (long)IRIS_CPTR_TEST_UNTYPED, (long)((uint64_t)dest << 32));
}
/* Derive a narrowed I/O-port CONTROL capability into `dest` (Stage 5).  The
 * kernel has no port whitelist any more; the range that bounds what a holder
 * may claim travels on the authority, and this is how a holder hands out a
 * piece of its own. */
long it_ioport_narrow(long auth, long first, long last, uint32_t dest) {
    (void)it_invoke1(0, INV_CNODE_DELETE, (long)dest);
    return it_invoke(auth, INV_BOOT_IOPORT_NARROW, (long)((uint64_t)(uint16_t)first |
                          ((uint64_t)(uint16_t)last << 16)), (long)IRIS_CPTR_TEST_UNTYPED, (long)((uint64_t)dest << 32));
}

long it_irqcap_create(long auth, long irq, long dest) {
    return it_invoke(auth, INV_BOOT_CREATE_IRQCAP, irq, (long)IRIS_CPTR_TEST_UNTYPED, (long)((uint64_t)dest << 32));
}

/* An initrd image published into a CSpace slot as a FRAME (ledger D-5).  Same
 * rotating-pool contract as it_retype_slot_alloc.  The call returns the image
 * SIZE, so success is a positive number rather than zero. */
/* The size of the image the last successful it_initrd_vmo_slot handed over.
 * A frame does not answer "how big was the file" — the region is rounded to
 * pages — and the call that produced it does, so the answer is kept here
 * rather than asked for again. */
long g_it_initrd_size;

long it_initrd_vmo_slot(long auth_cptr, long index) {
    uint32_t leaf = it_pool_leaf_take();
    /* Stage 6 Step 5: the image copy is charged to the suite's own budget,
     * not to the small per-child pool its address space came from. */
    long r = it_invoke(auth_cptr, INV_BOOT_INITRD_FRAME, index, (long)(((uint64_t)leaf << 32) | (uint64_t)IT_OBJ_CNODE_SLOT), (long)IRIS_CPTR_TEST_UNTYPED);
    if (r <= 0) return (r == 0) ? (long)IRIS_ERR_NOT_FOUND : r;
    g_it_initrd_size = r;
    return (long)IT_OBJ_CPTR(leaf);
}

/* SYS_TCB_SELF into a rotating leaf: the caller's own TCB as a capability. */
/*
 * The caller's own TCB, in the ROTATING pool — and that is the right pool for
 * it, which is the distinction the thread CNode exists to draw.
 *
 * A capability belongs in the rotating pool when its life is a test's, and in
 * the thread CNode when its life is a THREAD's.  `SYS_TCB_SELF` hands back a
 * fresh capability each call (an MDB LEGACY ROOT — ledger D-6, no ancestor,
 * unreachable by any revoke), and tests take it, use it and abandon it.  Left
 * in the thread CNode those accumulate forever, one unrevocable root per call,
 * with two of the eleven call sites inside loops: measured at +36 roots.
 * Recycled, they cost nothing.
 *
 * Publishing it once and memoising was the other repair, and it is wrong here:
 * T083 asks for a capability it can close and revoke on its own, and a shared
 * one makes that the suite's.
 */
/*
 * The caller's own TCB, DERIVED from the one its spawner delegated.
 *
 * `SYS_TCB_SELF` is retired (ledger A-18): it handed a thread a capability to
 * itself asking for no capability at all, and published an MDB root nothing
 * could revoke.  The main thread's TCB is the one the loader configured, which
 * arrives at IRIS_CPTR_OWN_TCB, so this is a mint of it — a FRESH capability
 * the caller can close and revoke on its own account, which is what T083 and
 * the fuzz batteries want, and now with a parent.
 *
 * A HELPER thread cannot use this: OWN_TCB names the process's first thread,
 * not whoever asks.  Those are handed their own TCB by their creator, in the
 * entry register (IT_THREAD_ARG_SELF_TCB) — the one per-thread channel a
 * freshly started thread has.
 */
long it_own_tcb_derived(void) {
    return it_cs_reduce((long)IRIS_CPTR_OWN_TCB,
                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
}


/* SYS_VSPACE_SELF into a rotating leaf: the caller's own address space as a
 * capability.  Stage 7 Step 15: this replaces it_proc_vspace_slot, which asked
 * a PROCESS for an address space — the self case being the only one that
 * survives, because the others are held by whoever spawned the child. */
long it_vspace_self_slot(void) {
    return it_cs_reduce((long)IRIS_CPTR_OWN_VSPACE,
                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
}

/*
 * A KFrame retyped into a CSpace slot: the memory the suite fabricates.
 *
 * Ledger D-5 — this replaces it_vmo_create_in and it_vmo_create_slot, which
 * produced a KVmo: a kernel-owned array of pages populated lazily, on a
 * schedule the holder did not choose.  A frame is the same memory with the
 * allocation decision back where the budget is, and the two factories were a
 * second way to spell a call RETYPE2 already made.
 *
 * `bytes` must be a page multiple; a frame maps as a whole (D-10).  Which
 * budget pays is the `ut` argument, which is the whole of "charged to X".
 */
long it_frame_create_slot(long ut, uint64_t bytes) {
    return it_retype_slot_alloc(ut, IRIS_KOBJ_FRAME, (long)bytes);
}

long it_ep_create_slot(void) {
    return it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_ENDPOINT, 0);
}

long it_notify_create_slot(void) {
    return it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_NOTIFICATION, 0);
}

long it_ep_create(void) {
    return it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_ENDPOINT, 0);
}

long it_notify_create(void) {
    return it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_NOTIFICATION, 0);
}

/* Retype a reply object into a FIXED root slot (tests hand it to a receive as
 * `msg.reply` and reply on the `msg.got_cap` the receive echoes back).  Delete
 * with it_slot_delete when the test is done. */
long it_reply_create_at(uint32_t slot) {
    return it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY,
                         slot, 1u, 0);
}

/* ...and the guard that says so out loud.
 *
 * The dangerous direction is the one the comment above does NOT cover: a
 * caller that already holds a two-level CPtr and computes `cptr >> 8` before
 * calling, producing a LEAF index (4..199) that is indistinguishable from a
 * root slot.  The delete then lands on the root CNode, at a slot the suite's
 * well-known map owns — and the capability it destroys belongs to some test
 * that has not run yet.  Nothing fails where the mistake is.
 *
 * So the suite states which root slots it ever means to delete.  Everything
 * else at root level is refused and counted, and T319 asserts the count is
 * zero: the class becomes a test failure instead of a later NOT_FOUND with no
 * author. */
uint32_t g_it_slot_guard_hits = 0;
uint32_t g_it_slot_guard_last = 0;

/* The root slots the suite must still hold when the last test runs: the
 * bootstrap capabilities, its own fixtures, its delegated untyped, and the two
 * CNodes every fabrication goes through.  Stated as what must SURVIVE rather
 * than as what may be deleted, because the scratch slots are many, churn, and
 * are harmless — while these are few, fixed, and fatal. */
static int it_root_slot_is_load_bearing(uint32_t s) {
    if (s >= 1u && s <= 15u) return 1;      /* well-known bootstrap caps    */
    if (s == 18u || s == 19u) return 1;     /* OWN_VSPACE / OWN_TCB         */
    if (s == 24u || s == 25u || s == 26u) return 1;
    if (s >= 27u && s <= 28u) return 1;     /* TEST_SUPER / TEST_FIX_C      */
    if (s == 30u || s == 31u) return 1;     /* TEST_FIX_A / TEST_FIX_B      */
    if (s == (uint32_t)IRIS_CPTR_TEST_UNTYPED) return 1;          /* 55 */
    if (s == 56u || s == 58u || s == 59u) return 1;               /* vspace, vfs */
    if (s == (uint32_t)IRIS_CPTR_DEVICE_UNTYPED) return 1;        /* 64 */
    if (s == (uint32_t)IRIS_CPTR_PCI_EP) return 1;                /* 62 */
    if (s == 66u) return 1;   /* IT_IPCBUF_CNODE_SLOT, asserted below */
    if (s == IT_OBJ_CNODE_SLOT) return 1;                         /* 80 */
    if (s == (uint32_t)IRIS_CPTR_FB_CONTROL) return 1;            /* 99 */
    if (s == IT_SERIAL_SLOT) return 1;                            /* 255 */
    return 0;
}

void it_slot_delete(uint32_t slot) {
    if (slot >= 256u) {
        (void)it_invoke1((long)(slot & 0xFFu), INV_CNODE_DELETE, (long)(slot >> 8));
        return;
    }
    if (it_root_slot_is_load_bearing(slot)) {
        __atomic_fetch_add(&g_it_slot_guard_hits, 1u, __ATOMIC_RELAXED);
        g_it_slot_guard_last = slot;
        it_serial_write("[slot-guard] refused root delete of slot ");
        it_log_num(slot);
        it_serial_write("\n");
        return;
    }
    (void)it_invoke1(0, INV_CNODE_DELETE, (long)slot);
}

/* A transfer source is a slot-to-slot mint, which also installs the result as
 * an MDB child of the source.  The SYS_CNODE_MINT branch this used to carry
 * for handle sources is gone with the namespace. */
long it_xfer_slot(iris_cptr_t src_h, uint32_t slot, uint32_t rights) {
    it_slot_delete(slot);
    long r = it_invoke2((long)src_h, INV_CSPACE_MINT, (long)((uint64_t)slot << 32), (long)(rights | RIGHT_TRANSFER));
    return (r != 0) ? r : (long)slot;
}
int it_slot_is_notif(long slot) {
    return it_invoke0(slot, INV_CAP_IDENTIFY) == (long)IRIS_HANDLE_TYPE_NOTIFICATION;
}

/* Mint a source slot with EXACTLY the requested rights (no implicit
 * RIGHT_TRANSFER) — used by the negative tests that must be denied. */
long it_xfer_slot_norights(long src_h, uint32_t slot, uint32_t rights) {
    it_slot_delete(slot);
    long r = it_invoke2(src_h, INV_CSPACE_MINT, (long)((uint64_t)slot << 32), (long)rights);
    return (r != 0) ? r : (long)slot;
}

/* ── Phase S4 (Step 3): native-CDT derivation helpers ─────────────────────
 * The legacy handle tree (SYS_CAP_DERIVE/SYS_CAP_REVOKE) is being retired.
 * Its replacement is the CSpace CDT: derivation is SYS_CSPACE_MINT slot→slot
 * (a real MDB child of the source) and revocation is SYS_CSPACE_REVOKE, which
 * removes the ENTIRE descendant subtree across CNodes and processes while the
 * invoked slot and its siblings survive.
 *
 * it_cdt_root  — copy a cap into a scratch slot so it can act as a derivation
 *                root without the test's fixture slot rotating out from under
 *                it.  Returns the CPtr.
 * it_cdt_derive— derive src_cptr into dest_slot with the requested rights.
 * it_cdt_alive — does this slot still name a live capability?
 * it_cdt_revoke— revoke the slot's descendants (>= 0 on success). */
long it_cdt_root(iris_cptr_t src_h, uint32_t slot) {
    it_slot_delete(slot);
    long r = it_invoke2((long)src_h, INV_CSPACE_MINT, (long)((uint64_t)slot << 32), (long)RIGHT_SAME_RIGHTS);
    return (r != 0) ? r : (long)slot;
}

long it_cdt_derive(long src_cptr, uint32_t dest_slot, uint32_t rights) {
    it_slot_delete(dest_slot);
    long r = it_invoke2(src_cptr, INV_CSPACE_MINT, (long)((uint64_t)dest_slot << 32), (long)rights);
    return (r != 0) ? r : (long)dest_slot;
}

/* Common fixture shape: a rights-reduced copy of a cap that currently lives in
 * a HANDLE.  Bridges the source into root_slot and derives into dest_slot;
 * returns the derived CPtr.  Both slots are the caller's to release. */
long it_cdt_reduced(iris_cptr_t src_h, uint32_t root_slot,
                           uint32_t dest_slot, uint32_t rights) {
    long r = it_cdt_root(src_h, root_slot);
    if (r < 0) return r;
    return it_cdt_derive(r, dest_slot, rights);
}

int it_cdt_alive(long cptr) {
    return it_invoke0(cptr, INV_CAP_IDENTIFY) >= 0;
}

long it_cdt_revoke(long cptr) {
    return it_invoke0(cptr, INV_CSPACE_REVOKE);
}
static uint32_t g_it_xfer_next;
long it_xfer_dup(long src_h, uint32_t rights) {
    uint32_t slot = IT_XFER_SLOT_A +
        (__atomic_fetch_add(&g_it_xfer_next, 1u, __ATOMIC_RELAXED)
         % IT_XFER_SLOT_SPAN);
    return it_xfer_slot((iris_cptr_t)src_h, slot, rights);
}

/* Ledger A-29: the transfer is a COPY, so a sender that meant to give the
 * capability away drops its own copy once the send has landed — send-then-
 * delete is seL4's move.  Every site that expects its transfer to succeed
 * calls this; the sites that expect it to FAIL keep the copy on purpose and
 * assert it is still there. */
void it_xfer_release(long cptr) {
    if (cptr >= 0) it_slot_delete((uint32_t)cptr);
}

void it_pass(const char *id) {
    g_pass++;
    g_total++;
    it_serial_write("[IRIS][TEST] ");
    it_serial_write(id);
    it_serial_write(" ok\n");
}

void it_fail(const char *id, const char *reason) {
    g_total++;
    it_serial_write("[IRIS][TEST] ");
    it_serial_write(id);
    it_serial_write(" FAIL: ");
    it_serial_write(reason);
    it_serial_write("\n");
}

/* ── Message helpers ────────────────────────────────────────────────────── */

/* it_chan_msg_zero retired — Phase 13/Track I (no KChannel tests remain). */

/* iris_msg_zero lives in common/iris_msg.h now, with the message (A-33). */
/* Release a capability the suite holds, whichever namespace names it.
 *
 * This is what lets the suite migrate WITHOUT moving a single release point.
 * Drift tests assert object gauges return to a baseline, so WHEN a capability
 * is dropped is part of what they measure; rewriting call sites moves that
 * moment and those tests fail.  Teaching the universal release helper both
 * namespaces keeps every call site, and every lifetime, exactly where it was.
 *
 * The tag bit is the discriminator, not a magnitude: a CPtr is 31 bits wide
 * now and multi-level ones are far above any old threshold.  The handle branch
 * disappears with the handle namespace. */
void it_close(iris_cptr_t *h) {
    if (*h == IRIS_CPTR_NULL) return;
    if (((uint32_t)*h & 0xFFu) == IT_OBJ_CNODE_SLOT ||
        ((uint32_t)*h & 0xFFu) == IT_CHILD_CN_SLOT ||
        ((uint32_t)*h & 0xFFu) == IT_LOADER_WS_SLOT) {
        /* Only capabilities THIS suite fabricated are released by deleting
         * their slot, and it knows them by the second-level CNode they live
         * in: the object CNode it retypes into, and the loader workspace the
         * spawn helpers publish into.  Scoping matters: a plain HANDLE_CLOSE
         * on a CPtr used to be a harmless failed call, so several places close
         * values that are really receive-slot CPtrs they do not own.  Deleting
         * those would destroy a live capability instead of doing nothing.
         *
         * Missing the workspace here is not a leak that shows up locally: the
         * loader allocates one leaf per LIVE process and scans for a free one,
         * so undeleted process caps exhaust the workspace and every spawn
         * fails, dozens of tests later. */
        it_slot_delete((uint32_t)*h);
    }
    *h = IRIS_CPTR_NULL;
}
