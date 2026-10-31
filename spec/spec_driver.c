/* SPDX-License-Identifier: Apache-2.0 */
/*
 * spec_driver.c — the REAL kernel, driven by the same trace as the model.
 *
 * Phase 1 of the IRIS-V plan needs two things that answer the same questions
 * and were built independently: `spec/authority_model.py` is one, and this is
 * the other end.  It links the actual `kcnode.c`, `kobject.c` and `cspace.c`
 * — not a copy, not a summary — reads a trace of capability operations on
 * stdin, applies them, and prints the resulting CSpace in the format the
 * model prints.  `spec/differential.py` compares the two, byte for byte, over
 * randomly generated traces.
 *
 * ── Why a driver and not another unit test ─────────────────────────────────
 *
 * A unit test asserts what its author already believed.  This asserts nothing
 * at all: it renders a state.  The judgement is made by comparing two
 * independent implementations of the same specification, which is the only
 * kind of check that can find something neither author thought of — and the
 * defects this tree has shipped were all of exactly that kind.
 *
 * ── The trace language ─────────────────────────────────────────────────────
 *
 *   seed   <slot> <type>                  a fresh object, as an MDB root
 *   mint   <src> <dst> <rights> <badge>   derive: `kcnode_slot_derive`
 *   delete <slot>                         `kcnode_slot_delete`
 *   revoke <slot>                         `kcnode_slot_revoke`
 *
 * A slot id is flat: `cnode * SPC + index`, so a trace says nothing about how
 * CNodes are laid out and the two sides need not agree about that.
 *
 * ── What is deliberately NOT here yet ──────────────────────────────────────
 *
 * Untyped retype.  It needs backing storage and a second lifecycle, and the
 * authority claims this file exists to check — rights reduce, badges seal,
 * derivation is a forest, revoke takes the subtree and not the root, an
 * object lives exactly while a slot names it — are all reachable without it.
 * `seed` stands in for the moment a capability enters the tree as a root.
 * Extending the trace with `retype` is the next step and changes nothing in
 * the comparison machinery.
 */
#include <iris/nc/kcnode.h>
#include <iris/nc/cspace.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/knotification.h>
#include <iris/irq_routing.h>
#include <iris/task.h>
#include <iris/nc/kobject.h>
#include <iris/nc/rights.h>
#include <iris/kpage.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kframe.h>
#include <iris/paging.h>
#include <stdatomic.h>
#include <iris/nc/rights.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NCNODES 4u
#define SPC     16u
#define NSLOTS  (NCNODES * SPC)

/*
 * ── The VSpace side, so D-13 is checked against `kvspace.c` and not only
 *    against a model of it ────────────────────────────────────────────────
 *
 * `inv_D13` — a space that maps an object holds a capability naming it — is
 * proved over symbolic state by `spec/prove.py`.  Nothing tied it to the C,
 * which is the gap the ledger registers: the proved side is the one no
 * hardware executes, so it is the worse of the two to have wrong.
 *
 * One space, a few frames, and two operations are enough to close that for
 * the mechanism: a map installs a capability as a SIBLING of the slot mapped
 * through, and any path that removes a capability runs the sweep.  The dump
 * then carries, per frame, how many mappings the space has and whether it
 * still holds authority — the two sides of the invariant, read out of the real
 * kernel and compared with the model on every trace.
 */
#define NVFRAMES  3u
#define VS_POOL_BYTES  (256u * 1024u)
#define VA_OF(i)  (USER_PRIVATE_BASE + ((uint64_t)(i) << 12))

static struct KVSpace *g_vs;
static struct KFrame   *g_fr[NVFRAMES];
static unsigned         g_nfr;
static uint64_t         g_fr_pa = 0x62000000ULL;

/* Lazily, so a trace that never maps pays nothing — the same reason the
 * kernel carves `map_caps` on first map. */
static struct KVSpace *vs_ensure(void) {
    if (g_vs) return g_vs;
    void *mem = kpage_alloc(VS_POOL_BYTES);
    struct KUntyped *pool;
    if (!mem) return 0;
    g_vs = kvspace_alloc(0x5EC0000ULL);
    if (!g_vs) return 0;
    pool = kuntyped_create((uint64_t)(uintptr_t)mem, VS_POOL_BYTES, 0);
    if (!pool) return 0;
    kvspace_set_pt_pool(g_vs, pool);
    return g_vs;
}

static void vs_teardown(void) {
    if (!g_vs) return;
    kvspace_invalidate(g_vs);
    for (unsigned i = 0; i < g_nfr; i++) g_fr[i] = 0;
    g_nfr = 0;
    g_vs = 0;          /* the pool's pages are the harness's to leak per trace */
}

/*
 * ── Four symbols that are outside the authority model's boundary ───────────
 *
 * They are defined here rather than linked in, and that is a statement about
 * scope rather than a convenience.  IPC delivery, fault calls, IRQ routing
 * and the syscall restart counter are all things a capability can AUTHORISE,
 * and none of them changes what a CSpace holds — so the model this driver is
 * compared against does not describe them, and pulling the syscall layer in
 * would mean comparing against a model that stays silent about most of what
 * got linked.
 *
 * When the trace language grows IPC, these stop being stubs and the model
 * grows the same operations at the same time.  Until then, a call to one of
 * them from inside a CSpace operation would be a call this driver cannot
 * honour, and it would rather fail loudly than answer plausibly.
 */
int kendpoint_deliver_notification(struct task *t, uint64_t bits) {
    (void)t; (void)bits;
    fprintf(stderr, "spec_driver: IPC is outside the model's boundary\n");
    abort();
}

int kendpoint_fault_call(struct task *t, struct KEndpoint *ep,
                         const struct ipc_stage *msg) {
    (void)t; (void)ep; (void)msg;
    fprintf(stderr, "spec_driver: faults are outside the model's boundary\n");
    abort();
}

void irq_routing_unregister_notification(struct KNotification *n) { (void)n; }

/*
 * The syscall layer's own counters and its restart hook — dispatch, not
 * authority, and so outside what the model describes.  `retype` reaches none
 * of them: it neither parks a thread nor counts a numbered call.
 */
uint32_t ipc_buffers_registered(void);
uint32_t ipc_buffers_registered(void) { return 0u; }
uint32_t syscall_abandon_count(void);
uint32_t syscall_abandon_count(void) { return 0u; }
uint32_t syscall_numbered_call_count(void);
uint32_t syscall_numbered_call_count(void) { return 0u; }
void syscall_request_restart(struct task *t);
void syscall_request_restart(struct task *t) {
    (void)t;
    fprintf(stderr, "spec_driver: blocking is outside the model's boundary\n");
    abort();
}

uint32_t syscall_restart_count(void) { return 0u; }

/*
 * ── The Untyped, and why `retype` goes through the SYSCALL ─────────────────
 *
 * Every other operation in this driver calls a `kcnode_*` primitive, which
 * is fine because those primitives ARE the authority model.  Retype is not:
 * `sys_untyped_retype2` validates the type, computes the payload, carves the
 * block, initialises the object and installs the capability as an MDB child
 * of the Untyped's slot — and reimplementing that here would mean comparing
 * the model against a second copy of the kernel's intent rather than against
 * the kernel.  So the driver builds what the syscall needs: a task with a
 * CSpace root, and an Untyped with real backing memory installed in a slot.
 *
 * The cost is an addressing restriction, stated rather than worked around:
 * the syscall takes CPtrs, and this CSpace is one level deep, so `retype`
 * can only name slots in CNode 0 — ids 0..15.  The trace generator knows it.
 * Two-level addressing would mean guards and depth, which is its own phase.
 */
#define UT_BYTES (1u << 20)

static struct KCNode *g_cn[NCNODES];
static struct task   *g_task;

void test_set_current_task(struct task *t);   /* tests/kernel/stubs.c */
uint64_t sys_untyped_retype2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
/*
 * ── The SYSCALL layer, driven as well as the primitives ───────────────────
 *
 * Every other capability op here calls a `kcnode_*` primitive, and that was
 * enough until it wasn't: the rule "slot 0 is IRIS_CPTR_NULL, so nothing may
 * be published there" lives in the SYSCALLS and not in the primitives, and
 * two of the four syscalls that publish were missing it.  The differential
 * could not see that, because it was never driving those four.
 *
 * So `smint`, `smove` and `srotate` go through the door a ring-3 caller uses.
 * They are restricted to CNode 0 — the syscalls take CPtrs and this CSpace is
 * one level deep — which is stated rather than hidden, and is why the
 * primitive-level `mint` stays: it is the one that still reaches across
 * CNodes.
 */
uint64_t sys_cspace_mint(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cspace_move(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cspace_rotate(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cspace_revoke(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cnode_delete(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cap_identify(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cap_same_object(uint64_t a0, uint64_t a1, uint64_t a2);

/* A seeded object needs SOME type; the trace picks one and nothing here
 * depends on which, because the authority model does not. */
static const kobject_type_t g_types[] = {
    KOBJ_ENDPOINT, KOBJ_NOTIFICATION, KOBJ_CNODE, KOBJ_UNTYPED,
    KOBJ_REPLY, KOBJ_SCHED_CONTEXT, KOBJ_FRAME
};
static const char *const g_type_names[] = {
    "ep", "notif", "cnode", "ut", "reply", "sc", "frame"
};

/*
 * ── D-7's target property, made observable ─────────────────────────────────
 *
 * The ledger's D-7 row says object lifetime is REFERENCE COUNTED where seL4's
 * is derivation-driven, and that the two "can disagree" — recording three
 * separate incidents where they did.  Nothing checked it, and this driver
 * could not: its destructor was a no-op and its objects were never freed, so
 * the one question D-7 is about — *is this object still alive?* — had no
 * answer here at all.
 *
 * Now it has one.  The destructor still returns no storage, because freeing it
 * would make the dump depend on the allocator, but it MARKS the object dead
 * and the live count goes into the dump beside the slots.  The model computes
 * the same number a completely different way — how many distinct objects at
 * least one slot names — so every trace in `spec-differential` and all 194481
 * in `spec-exhaustive` now assert:
 *
 *     an object is alive  <=>  a capability names it
 *
 * which is D-7's target stated exactly, checked against the real kcnode.c
 * rather than against the model that already believes it.
 */
#define MAX_TRACKED 8192
static struct KObject *g_objs[MAX_TRACKED];
/*
 * And how many bytes each one is, because they are no longer all one size.
 *
 * Every fabricated object used to be `sizeof(struct KObject)`, which held
 * because the driver only ever touched a fabricated object through `base` --
 * authority operations read the type, the rights and the lock and nothing
 * else.  `nsig`/`npoll` are the first operations here that reach into a
 * BODY, and a notification tagged onto a bare KObject makes that read land
 * past the allocation: signalling bits 0x3 polled back 0x1011, which was the
 * heap, and the signal itself had already written there.
 *
 * So a fabricated object of type T is now a valid T.  The same defect the
 * `seed` comment already names -- an object whose type is a lie -- and the
 * same cure it chose for Untyped.
 */
static uint32_t        g_objsz[MAX_TRACKED];
static unsigned        g_nobjs;
static unsigned        g_alive;

static void obj_destroy(struct KObject *o) {
    (void)o;
    if (g_alive) g_alive--;
}
static const struct KObjectOps g_ops = { .close = NULL, .destroy = obj_destroy };

/* Storage is not returned — see above — but the object is tracked so the
 * trace can be freed as a whole at `reset`, which is what keeps an exhaustive
 * run's memory flat across millions of traces. */
static uint32_t obj_bytes(kobject_type_t t) {
    /* A body only where the driver reads one.  Zeroed, so the notification is
     * the one configuration the model can mirror: no pending bits, not
     * closed, no waiter queued and no thread bound. */
    if (t == KOBJ_NOTIFICATION) return (uint32_t)sizeof(struct KNotification);
    return (uint32_t)sizeof(struct KObject);
}

static struct KObject *obj_new(kobject_type_t t) {
    uint32_t nbytes = obj_bytes(t);
    struct KObject *o = (struct KObject *)kpage_alloc(nbytes);
    if (!o) return NULL;
    memset(o, 0, nbytes);
    kobject_init_test(o, t, &g_ops);
    if (g_nobjs < MAX_TRACKED) {
        g_objsz[g_nobjs] = nbytes;
        g_objs[g_nobjs++] = o;
    }
    g_alive++;
    return o;
}

static struct KCNode *cn_of(unsigned sid)  { return g_cn[sid / SPC]; }
static uint32_t       idx_of(unsigned sid) { return (uint32_t)(sid % SPC); }

/* The slot a KCSlot pointer belongs to, or -1.  The model addresses slots by
 * number and the C addresses them by pointer; this is the only place the two
 * have to be reconciled. */
static int sid_of_slot(const struct KCSlot *p) {
    if (!p) return -1;
    for (unsigned s = 0; s < NSLOTS; s++)
        if (&g_cn[s / SPC]->slots[s % SPC] == p) return (int)s;
    return -1;
}

static const char *type_name(kobject_type_t t) {
    for (unsigned i = 0; i < sizeof g_types / sizeof g_types[0]; i++)
        if (g_types[i] == t) return g_type_names[i];
    return "?";
}

/*
 * The dump, in the model's format.
 *
 * Objects are renumbered in slot order.  That is what lets two
 * implementations that share no pointers be compared at all: identity in the
 * output is "the Nth distinct object seen scanning slots upward", which both
 * sides can compute and neither can fake.
 */
static void dump(void) {
    const struct KObject *seen[NSLOTS];
    unsigned nseen = 0;

    for (unsigned s = 0; s < NSLOTS; s++) {
        const struct KCSlot *sl = &cn_of(s)->slots[idx_of(s)];
        if (!sl->object) continue;
        unsigned k = 0;
        for (; k < nseen; k++) if (seen[k] == sl->object) break;
        if (k == nseen) seen[nseen++] = sl->object;
    }
    for (unsigned s = 0; s < NSLOTS; s++) {
        const struct KCSlot *sl = &cn_of(s)->slots[idx_of(s)];
        if (!sl->object) continue;
        unsigned canon = 0;
        for (unsigned k = 0; k < nseen; k++)
            if (seen[k] == sl->object) { canon = k + 1; break; }
        int par = sid_of_slot(sl->mdb_parent);
        if (par >= 0)
            printf("slot %u obj=%u type=%s rights=%u badge=%llu parent=%d\n",
                   s, canon, type_name(sl->object->type),
                   (unsigned)sl->rights, (unsigned long long)sl->badge, par);
        else
            printf("slot %u obj=%u type=%s rights=%u badge=%llu parent=-\n",
                   s, canon, type_name(sl->object->type),
                   (unsigned)sl->rights, (unsigned long long)sl->badge);
    }
    /*
     * D-7: how many objects the KERNEL still considers alive.
     *
     * `nseen` above is how many DISTINCT objects the slots name, computed by
     * walking the CSpace.  `g_alive` is how many the reference counts have not
     * yet destroyed, computed by counting destructor calls.  The two are
     * arrived at by completely different means and D-7 says they may diverge —
     * so the dump carries both, and the model computes the first
     * independently.  A trace where they differ is the fourth incident.
     */
    unsigned named_tracked = 0;
    for (unsigned k = 0; k < nseen; k++) {
        for (unsigned oi = 0; oi < g_nobjs; oi++)
            if (g_objs[oi] == seen[k]) { named_tracked++; break; }
    }
    printf("live %u named %u\n", g_alive, named_tracked);
    /*
     * D-7's mechanism, measured: how many slots seL4's isFinalCapability
     * calls final.  The model counts the objects exactly one slot names,
     * which is the same number if the predicate is right.
     */
    printf("final %u\n", kcnode_count_final_caps(g_cn, NCNODES));
    /*
     * D-13, both sides of it, per frame this trace created.
     *
     * `mapped` is the kernel's own `mapped_count`; `cap` is what
     * `kvspace_map_cap_has` answers, which is the walk of the space's
     * capability chunks.  The invariant is `mapped > 0 -> cap`, and it is NOT
     * asserted here: both numbers are printed and the model computes them
     * independently, so a disagreement names which side is wrong instead of
     * stopping at "something is".
     */
    for (unsigned i = 0; i < g_nfr; i++) {
        unsigned m = g_fr[i] ? (unsigned)atomic_load_explicit(
                                   &g_fr[i]->mapped_count, memory_order_relaxed)
                             : 0u;
        int hc = (g_fr[i] && g_vs) ? kvspace_map_cap_has(g_vs, &g_fr[i]->base)
                                   : 0;
        printf("frame %u mapped %u cap %d\n", i, m, hc);
    }
    /*
     * D-7's shadow: every slot delete compared the predicate against the
     * count.  The model prints 0, because the two agreeing is the claim.
     */
    printf("shadow %u\n", kcnode_final_shadow_mismatch());
}

int main(void) {
    char line[256];

    for (unsigned i = 0; i < NCNODES; i++) {
        g_cn[i] = kcnode_alloc(SPC);
        if (!g_cn[i]) { fprintf(stderr, "spec_driver: no cnode\n"); return 2; }
        kobject_active_retain(&g_cn[i]->base);
    }

    /* A caller, so the syscall layer has a CSpace to resolve CPtrs in. */
    g_task = (struct task *)kpage_alloc((uint32_t)sizeof(struct task));
    if (!g_task) { fprintf(stderr, "spec_driver: no task\n"); return 2; }
    memset(g_task, 0, sizeof(*g_task));
    g_task->cspace_root = g_cn[0];
    test_set_current_task(g_task);

    while (fgets(line, sizeof line, stdin)) {
        char op[16];
        unsigned a, b, c, d;
        if (sscanf(line, "%15s", op) != 1) continue;

        /*
         * `reset` — end this trace, print its dump, and empty the CSpace so
         * the next one starts clean in the SAME process.
         *
         * Exhaustive enumeration (`spec/exhaustive.py`) runs tens of
         * thousands of short traces, and a process launch per trace costs
         * more than the trace does.  The reset DELETES every slot rather
         * than reallocating the CNodes, so the objects are freed through the
         * ordinary lifecycle and nothing leaks across traces — which matters
         * when there are 38416 of them and the page allocator is finite.
         *
         * Deleting every slot of every CNode also exercises teardown far
         * harder than the random lane does: it is the only thing here that
         * empties a populated CSpace completely, every single time.
         */
        if (!strcmp(op, "reset")) {
            printf("--\n");
            dump();
            for (unsigned ci = 0; ci < NCNODES; ci++)
                for (unsigned si = 0; si < SPC; si++)
                    (void)kcnode_slot_delete(g_cn[ci], si);
            /*
             * Every slot is gone, so every object this trace made should be
             * destroyed — D-7's property applied to the whole trace at once,
             * and the loudest place to check it.
             *
             * Printed BEFORE the `==` separator on purpose: after it, the line
             * lands in the next segment's result section, where the comparator
             * keeps only lines beginning "r " and would drop it without a
             * word.  Inside this segment it is part of the dump, the model has
             * no such line, and the trace fails.  A leak detector whose output
             * is discarded is worse than none, because it reads like evidence.
             */
            if (g_alive != 0u)
                printf("LEAK %u objects alive with no slot naming them\n",
                       g_alive);
            printf("==\n");
            vs_teardown();
            for (unsigned oi = 0; oi < g_nobjs; oi++)
                kpage_free(g_objs[oi], g_objsz[oi]);
            g_nobjs = 0;
            g_alive = 0;
            fflush(stdout);
            continue;
        }

        /*
         * Every operation's RESULT is printed, not just the final state.
         *
         * Comparing states alone misses a whole class: an operation the
         * kernel refuses and the model accepts agrees perfectly whenever the
         * acceptance would not have changed anything visible.  The refusal IS
         * the specification for most of the rights rules — "a mint without
         * DUPLICATE is ACCESS_DENIED" is a statement about a return value and
         * about nothing else — so the return value is part of what the two
         * implementations have to agree about.
         */
        if (!strcmp(op, "seed") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
            struct KObject *o;
            if (a >= NSLOTS) continue;
            /*
             * There is no such thing as a fake Untyped here.
             *
             * `seed` used to fabricate a bare KObject carrying the UNTYPED
             * tag and no region, which the model — reasonably — treated as an
             * Untyped that works.  A retype from it answered NO_MEMORY, and
             * the disagreement was the driver's, not either side's.  An
             * object whose type is a lie is worse than an object that is
             * missing.
             */
            if ((b % 4u) == 3u) {
                void *mem = kpage_alloc(UT_BYTES);
                struct KUntyped *u;
                if (!mem) return 2;
                u = kuntyped_create((uint64_t)(uintptr_t)mem, UT_BYTES, 0);
                if (!u) return 2;
                o = &u->base;
            } else {
                o = obj_new(g_types[b % 4u]);
            }
            if (!o) return 2;
            {
                int mr = (int)kcnode_mint(cn_of(a), idx_of(a), o,
                                          RIGHT_READ | RIGHT_WRITE |
                                          RIGHT_DUPLICATE | RIGHT_TRANSFER);
                /*
                 * Drop the BIRTH reference: the slot holds its own now.
                 *
                 * Without this the driver kept a reference to everything it
                 * ever seeded, so no seeded object could ever die and the
                 * liveness figure above read `live 3 named 2` — which looks
                 * exactly like the leak D-7 warns about and was the driver
                 * holding the object up.  The host suites already do this
                 * (`kcnode_mint(...); kobject_release(marker)`); the driver
                 * did not, because until now nothing here could tell.
                 *
                 * On a FAILED mint this destroys the object, which is right:
                 * nothing names it.
                 */
                if (o->type != KOBJ_UNTYPED) kobject_release(o);
                printf("r %d\n", mr);
            }
        } else if (!strcmp(op, "mint") &&
                   sscanf(line, "%*s %u %u %u %u", &a, &b, &c, &d) == 4) {
            if (a >= NSLOTS || b >= NSLOTS) continue;
            printf("r %d\n", (int)kcnode_slot_derive(cn_of(a), idx_of(a),
                                                     cn_of(b), idx_of(b),
                                                     (iris_rights_t)c,
                                                     (uint64_t)d));
        } else if (!strcmp(op, "rotate") &&
                   sscanf(line, "%*s %u %u %u", &a, &b, &c) == 3) {
            /*
             * `kcnode_slot_rotate` — the primitive, not the syscall.  The
             * syscall's CPtr resolution is the same code `mint` and `move`
             * already drive; what nothing reached was the rotate itself, and
             * it is inside the boundary (`syscall_cspace.c` is linked here).
             */
            if (a >= NSLOTS || b >= NSLOTS || c >= NSLOTS) continue;
            printf("r %d\n", (int)kcnode_slot_rotate(
                       cn_of(a), idx_of(a), cn_of(b), idx_of(b),
                       cn_of(c), idx_of(c)));
        } else if (!strcmp(op, "delete") &&
                   sscanf(line, "%*s %u", &a) == 1) {
            if (a >= NSLOTS) continue;
            printf("r %d\n", (int)kcnode_slot_delete(cn_of(a), idx_of(a)));
        } else if (!strcmp(op, "ut") && sscanf(line, "%*s %u", &a) == 1) {
            /* Install an Untyped with real backing memory, so a retype from
             * it carves something that exists. */
            void *mem = kpage_alloc(UT_BYTES);
            struct KUntyped *u;
            if (a >= SPC) continue;                 /* root CNode only */
            if (!mem) return 2;
            u = kuntyped_create((uint64_t)(uintptr_t)mem, UT_BYTES, 0);
            if (!u) return 2;
            printf("r %d\n", (int)kcnode_mint(g_cn[0], a, &u->base,
                                              RIGHT_READ | RIGHT_WRITE |
                                              RIGHT_DUPLICATE | RIGHT_TRANSFER));
        } else if (!strcmp(op, "retype") &&
                   sscanf(line, "%*s %u %u %u", &a, &b, &c) == 3) {
            static const uint32_t ty[] = {
                KOBJ_ENDPOINT, KOBJ_NOTIFICATION, KOBJ_REPLY, KOBJ_SCHED_CONTEXT
            };
            if (a >= SPC || c >= SPC) continue;     /* root CNode only */
            printf("r %d\n", (int)(long)(int64_t)sys_untyped_retype2(
                       (uint64_t)a,
                       (uint64_t)ty[b % 4u] | (1ULL << 32),
                       (uint64_t)0u | ((uint64_t)c << 32),
                       0));
        } else if (!strcmp(op, "smint") &&
                   sscanf(line, "%*s %u %u %u %u", &a, &b, &c, &d) == 4) {
            if (a >= SPC || b >= SPC) continue;
            printf("r %d\n", (int)(long)(int64_t)sys_cspace_mint(
                       (uint64_t)a,
                       (uint64_t)0u | ((uint64_t)b << 32),
                       (uint64_t)c | ((uint64_t)d << 32)));
        } else if (!strcmp(op, "smove") &&
                   sscanf(line, "%*s %u %u", &a, &b) == 2) {
            if (a >= SPC || b >= SPC) continue;
            printf("r %d\n", (int)(long)(int64_t)sys_cspace_move(
                       (uint64_t)a, (uint64_t)0u | ((uint64_t)b << 32), 0));
        } else if (!strcmp(op, "srevoke") && sscanf(line, "%*s %u", &a) == 1) {
            if (a >= SPC) continue;
            printf("r %d\n", (int)(long)(int64_t)sys_cspace_revoke((uint64_t)a, 0, 0));
        } else if (!strcmp(op, "sdelete") && sscanf(line, "%*s %u", &a) == 1) {
            if (a >= SPC) continue;
            /* cptr 0 addresses the caller's own root; arg1 is the slot. */
            printf("r %d\n", (int)(long)(int64_t)sys_cnode_delete(0, (uint64_t)a, 0));
        } else if (!strcmp(op, "vframe") &&
                   sscanf(line, "%*s %u", &a) == 1) {
            /* A frame with real backing, named by a capability in the root
             * CNode — the source a map derives the space's capability FROM. */
            struct KFrame *f;
            if (a >= SPC || g_nfr >= NVFRAMES) continue;
            if (!vs_ensure()) return 2;
            f = kframe_alloc(g_fr_pa, 4096, 0);
            if (!f) return 2;
            g_fr_pa += 0x1000ULL;
            g_fr[g_nfr++] = f;
            printf("r %d\n", (int)kcnode_mint(g_cn[0], a, &f->base,
                                               RIGHT_READ | RIGHT_WRITE |
                                               RIGHT_DUPLICATE));
        } else if (!strcmp(op, "vmap") &&
                   sscanf(line, "%*s %u %u", &a, &b) == 2) {
            /* Through the SLOT, which is what makes the space's capability a
             * sibling of it and what a revoke of an ancestor then reaches. */
            const struct KCSlot *sl;
            if (a >= NSLOTS) continue;
            if (!vs_ensure()) return 2;
            sl = &cn_of(a)->slots[idx_of(a)];
            if (!sl->object || sl->object->type != KOBJ_FRAME) {
                printf("r %d\n", (int)IRIS_ERR_WRONG_TYPE);
            } else {
                printf("r %d\n", (int)kframe_map_page_from(
                           (struct KFrame *)sl->object, g_vs, VA_OF(b), 0u,
                           cn_of(a), idx_of(a)));
            }
        } else if (!strcmp(op, "vunmap") &&
                   sscanf(line, "%*s %u %u", &a, &b) == 2) {
            if (a >= NVFRAMES) continue;
            if (!g_vs || a >= g_nfr || !g_fr[a]) {
                printf("r %d\n", (int)IRIS_ERR_NOT_FOUND);
            } else {
                printf("r %d\n", (int)kframe_unmap_page(g_fr[a], g_vs,
                                                         VA_OF(b)));
            }
        } else if (!strcmp(op, "nsig") &&
                   sscanf(line, "%*s %u %u", &a, &b) == 2) {
            /*
             * ── A DATA channel, against the real `knotification.c` ─────────
             *
             * Every other operation here moves AUTHORITY.  This one moves
             * sixty-four bits a sender chose, which is the only data channel
             * the kernel has that needs no scheduler: `signal` ORs bits into
             * the word and `poll` exchanges it to zero and never blocks, so
             * both run in a single-threaded harness exactly as they run in
             * the kernel.  With no thread waiting and none bound, the waiter
             * and bound-delivery arms of `knotification_signal` are inert,
             * which is the one configuration a harness with no scheduler can
             * state honestly.
             *
             * That is what makes an information-flow claim about DATA
             * checkable here rather than only in a model.  `spec-prove`'s
             * non-interference rows are about the result codes a subject
             * sees; these two ops are about the bits it receives.
             *
             * Both go through `cspace_resolve_only_notification` rather than
             * reaching into the slot, because the question is not "what do
             * the bits do" — it is "who is allowed to move them".  The
             * resolver is the gate the real syscalls use, it checks TYPE
             * before RIGHTS, and signalling needs WRITE while polling needs
             * WAIT (`sys_notify_signal`/`sys_notify_poll`).  Those are not
             * the same right, so a capability can carry one and not the
             * other, and a reimplemented check here would have agreed with a
             * reimplemented check in the model and tested neither.
             */
            static const uint64_t pat[4] = { 1u, 3u, 0x8000000000000000ull, 0u };
            struct KNotification *n; iris_rights_t nr;
            iris_error_t rr;
            if (a >= SPC) continue;
            rr = cspace_resolve_only_notification(g_task->cspace_root,
                                                  (iris_cptr_t)a,
                                                  RIGHT_WRITE, &n, &nr);
            (void)nr;
            if (rr != IRIS_OK) {
                printf("r %d\n", (int)rr);
            } else {
                knotification_signal(n, pat[b % 4u]);
                kobject_release(&n->base);   /* the resolver dropped the active ref */
                printf("r 0\n");
            }
        } else if (!strcmp(op, "npoll") && sscanf(line, "%*s %u", &a) == 1) {
            /*
             * The bits are the OBSERVABLE, so they are the result: the
             * comparator checks them on every trace without a dump line.
             *
             * Printed UNSIGNED while a refusal is printed signed, so the two
             * can never be confused.  It matters for one pattern: bit 63
             * polls back as 9223372036854775808, and as a signed value that
             * is INT64_MIN — a number that reads nothing like `-13`.
             *
             * Zero pending is IRIS_ERR_WOULD_BLOCK and not `0`, which is
             * `sys_notify_poll`'s own answer, in its words: "the same answer,
             * in the same words, that SYS_EP_NB_RECV gives".  So a successful
             * poll always carries at least one bit, and the success and
             * failure spaces do not overlap at all.
             */
            struct KNotification *n; iris_rights_t nr;
            iris_error_t rr;
            if (a >= SPC) continue;
            rr = cspace_resolve_only_notification(g_task->cspace_root,
                                                  (iris_cptr_t)a,
                                                  RIGHT_WAIT, &n, &nr);
            (void)nr;
            if (rr != IRIS_OK) {
                printf("r %d\n", (int)rr);
            } else {
                uint64_t got = knotification_take_pending(n);
                kobject_release(&n->base);
                if (got == 0u) printf("r %d\n", (int)IRIS_ERR_WOULD_BLOCK);
                else           printf("r %llu\n", (unsigned long long)got);
            }
        } else if (!strcmp(op, "ident") && sscanf(line, "%*s %u", &a) == 1) {
            /* A QUERY, and it is in the trace for the same reason the return
             * codes are: "what does this slot hold" is a fact both sides must
             * agree about, and it is observable where the dump's canonical
             * numbering is not. */
            if (a >= SPC) continue;
            printf("r %d\n", (int)(long)(int64_t)sys_cap_identify((uint64_t)a, 0, 0));
        } else if (!strcmp(op, "same") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
            if (a >= SPC || b >= SPC) continue;
            printf("r %d\n", (int)(long)(int64_t)sys_cap_same_object(
                       (uint64_t)a, (uint64_t)b, 0));
        } else if (!strcmp(op, "revoke") &&
                   sscanf(line, "%*s %u", &a) == 1) {
            uint32_t n = 0;
            if (a >= NSLOTS) continue;
            printf("r %d\n", (int)kcnode_slot_revoke(cn_of(a), idx_of(a), &n));
        }
    }

    printf("--\n");
    dump();
    /*
     * To stderr, so it is not part of the compared dump: how many times the
     * shadow comparison actually RAN.  `shadow 0` from a check that never
     * executed is not evidence, and the number is the difference between the
     * two readings.
     */
    fprintf(stderr, "shadow-checks %u mismatches %u\n",
            kcnode_final_shadow_checks(), kcnode_final_shadow_mismatch());
    return 0;
}
