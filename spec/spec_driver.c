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
#include <iris/nc/kendpoint.h>
#include <iris/nc/knotification.h>
#include <iris/irq_routing.h>
#include <iris/task.h>
#include <iris/nc/kobject.h>
#include <iris/nc/rights.h>
#include <iris/kpage.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/rights.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NCNODES 4u
#define SPC     16u
#define NSLOTS  (NCNODES * SPC)

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
    KOBJ_REPLY, KOBJ_SCHED_CONTEXT
};
static const char *const g_type_names[] = {
    "ep", "notif", "cnode", "ut", "reply", "sc"
};

static void obj_destroy(struct KObject *o) { (void)o; }
static const struct KObjectOps g_ops = { .close = NULL, .destroy = obj_destroy };

/* Objects are never freed here: the driver is a short-lived process and a
 * destructor that returned storage would make the dump depend on the
 * allocator.  What matters is whether a SLOT still names one. */
static struct KObject *obj_new(kobject_type_t t) {
    struct KObject *o = (struct KObject *)kpage_alloc((uint32_t)sizeof(*o));
    if (!o) return NULL;
    memset(o, 0, sizeof(*o));
    kobject_init(o, t, &g_ops);
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
            printf("==\n");
            for (unsigned ci = 0; ci < NCNODES; ci++)
                for (unsigned si = 0; si < SPC; si++)
                    (void)kcnode_slot_delete(g_cn[ci], si);
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
            printf("r %d\n", (int)kcnode_mint(cn_of(a), idx_of(a), o,
                                              RIGHT_READ | RIGHT_WRITE |
                                              RIGHT_DUPLICATE | RIGHT_TRANSFER));
        } else if (!strcmp(op, "mint") &&
                   sscanf(line, "%*s %u %u %u %u", &a, &b, &c, &d) == 4) {
            if (a >= NSLOTS || b >= NSLOTS) continue;
            printf("r %d\n", (int)kcnode_slot_derive(cn_of(a), idx_of(a),
                                                     cn_of(b), idx_of(b),
                                                     (iris_rights_t)c,
                                                     (uint64_t)d));
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
    return 0;
}
