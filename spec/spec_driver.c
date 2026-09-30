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

uint32_t syscall_restart_count(void) { return 0u; }

static struct KCNode *g_cn[NCNODES];

/* A seeded object needs SOME type; the trace picks one and nothing here
 * depends on which, because the authority model does not. */
static const kobject_type_t g_types[] = {
    KOBJ_ENDPOINT, KOBJ_NOTIFICATION, KOBJ_CNODE, KOBJ_UNTYPED
};
static const char *const g_type_names[] = { "ep", "notif", "cnode", "ut" };

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

    while (fgets(line, sizeof line, stdin)) {
        char op[16];
        unsigned a, b, c, d;
        if (sscanf(line, "%15s", op) != 1) continue;

        if (!strcmp(op, "seed") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
            if (a >= NSLOTS) continue;
            struct KObject *o = obj_new(g_types[b % 4u]);
            if (!o) return 2;
            (void)kcnode_mint(cn_of(a), idx_of(a), o, RIGHT_READ | RIGHT_WRITE |
                              RIGHT_DUPLICATE | RIGHT_TRANSFER);
        } else if (!strcmp(op, "mint") &&
                   sscanf(line, "%*s %u %u %u %u", &a, &b, &c, &d) == 4) {
            if (a >= NSLOTS || b >= NSLOTS) continue;
            (void)kcnode_slot_derive(cn_of(a), idx_of(a), cn_of(b), idx_of(b),
                                     (iris_rights_t)c, (uint64_t)d);
        } else if (!strcmp(op, "delete") &&
                   sscanf(line, "%*s %u", &a) == 1) {
            if (a >= NSLOTS) continue;
            (void)kcnode_slot_delete(cn_of(a), idx_of(a));
        } else if (!strcmp(op, "revoke") &&
                   sscanf(line, "%*s %u", &a) == 1) {
            uint32_t n = 0;
            if (a >= NSLOTS) continue;
            (void)kcnode_slot_revoke(cn_of(a), idx_of(a), &n);
        }
    }

    dump();
    return 0;
}
