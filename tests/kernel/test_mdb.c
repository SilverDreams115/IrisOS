/*
 * test_mdb.c — host unit tests for the native MDB/CDT (Fase S3).
 *
 * Exercises the canonical slot primitives directly (kcnode_slot_*), across
 * multiple CNodes (a CNode stands in for a process's CSpace here — the MDB is
 * process-agnostic: links are slot pointers).  Every mutation is followed by
 * kcnode_mdb_validate over the full CNode set, which must report zero errors.
 * Closes with a deterministic model-based fuzzer (Parte L).
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/rights.h>
#include <iris/kpage.h>
#include <stdint.h>

/* ── tracked test objects ───────────────────────────────────────────────── */

static int g_destroyed;
static void mdb_tracked_destroy(struct KObject *o) { (void)o; g_destroyed++; }
static const struct KObjectOps mdb_ops = { .close = NULL, .destroy = mdb_tracked_destroy };

static struct KObject *mk_obj(kobject_type_t type) {
    struct KObject *o = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (!o) return NULL;
    kobject_init(o, type, &mdb_ops);
    return o;
}

/* Validate a CNode set: expect 0 errors and (optionally) node/root counts. */
static void expect_valid(struct KCNode **set, uint32_t n,
                         int exp_nodes, int exp_roots) {
    struct mdb_validate_report rep;
    uint32_t errs = kcnode_mdb_validate(set, n, &rep);
    ASSERT_EQ(errs, 0u);
    if (exp_nodes >= 0) ASSERT_EQ(rep.nodes, (uint32_t)exp_nodes);
    if (exp_roots >= 0) ASSERT_EQ(rep.roots, (uint32_t)exp_roots);
}

/* Install a root cap holding `obj` at (cn, idx). */
static iris_error_t install_root(struct KCNode *cn, uint32_t idx,
                                 struct KObject *obj, iris_rights_t r) {
    return kcnode_slot_install_linked(cn, idx, obj, r, 0, 0, 0, 1, 1);
}

/* ── K.1 — basic operations ─────────────────────────────────────────────── */

static void test_mdb_basic(struct KCNode *cn) {
    struct KCNode *set[1] = { cn };
    struct KObject *obj = mk_obj(KOBJ_ENDPOINT);
    ASSERT_NOT_NULL(obj);

    /* root install */
    ASSERT_EQ(install_root(cn, 1, obj, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(obj); /* slot holds its own ref */
    expect_valid(set, 1, 1, 1);

    /* copy → child, same rights */
    ASSERT_EQ(kcnode_slot_derive(cn, 1, cn, 2, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    expect_valid(set, 1, 2, 1);
    { struct KObject *o; iris_rights_t r;
      ASSERT_EQ(kcnode_fetch(cn, 2, &o, &r), IRIS_OK);
      ASSERT_EQ(r, (iris_rights_t)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));
      kobject_active_release(o); kobject_release(o); }

    /* mint → child, reduced rights */
    ASSERT_EQ(kcnode_slot_derive(cn, 1, cn, 3, RIGHT_READ, 0), IRIS_OK);
    { struct KObject *o; iris_rights_t r;
      ASSERT_EQ(kcnode_fetch(cn, 3, &o, &r), IRIS_OK);
      ASSERT_EQ(r, (iris_rights_t)RIGHT_READ);
      kobject_active_release(o); kobject_release(o); }
    expect_valid(set, 1, 3, 1);

    /* mint cannot amplify: request WRITE from a READ-only source cap.
     * (source slot 3 holds only READ; and lacks DUPLICATE → ACCESS_DENIED) */
    ASSERT_EQ(kcnode_slot_derive(cn, 3, cn, 4, RIGHT_READ | RIGHT_WRITE, 0),
              IRIS_ERR_ACCESS_DENIED);
    expect_valid(set, 1, 3, 1); /* nothing changed */

    /* occupied target → ALREADY_EXISTS, no change */
    ASSERT_EQ(kcnode_slot_derive(cn, 1, cn, 2, RIGHT_SAME_RIGHTS, 0),
              IRIS_ERR_ALREADY_EXISTS);
    expect_valid(set, 1, 3, 1);

    /* delete a leaf (slot 3) removes only it */
    ASSERT_EQ(kcnode_slot_delete(cn, 3), IRIS_OK);
    expect_valid(set, 1, 2, 1);

    /* teardown */
    ASSERT_EQ(kcnode_slot_delete(cn, 2), IRIS_OK);
    ASSERT_EQ(kcnode_slot_delete(cn, 1), IRIS_OK);
    expect_valid(set, 1, 0, 0);
}

/* ── K.1/K.3 — badge rules + delete ≠ revoke + intermediate delete ──────── */

static void test_mdb_badge_and_delete(struct KCNode *cn) {
    struct KCNode *set[1] = { cn };
    struct KObject *ep = mk_obj(KOBJ_ENDPOINT);
    struct KObject *tcb = mk_obj(KOBJ_TCB);
    ASSERT_NOT_NULL(ep); ASSERT_NOT_NULL(tcb);

    ASSERT_EQ(install_root(cn, 1, ep, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(ep);

    /* fresh badge on an endpoint is allowed */
    ASSERT_EQ(kcnode_slot_derive(cn, 1, cn, 2, RIGHT_SAME_RIGHTS, 0x1234u), IRIS_OK);
    { struct KObject *o; iris_rights_t r; uint64_t b;
      ASSERT_EQ(kcnode_fetch_badged(cn, 2, &o, &r, &b), IRIS_OK);
      ASSERT_EQ(b, 0x1234u);
      kobject_active_release(o); kobject_release(o); }

    /* re-badge of an already-badged cap → ACCESS_DENIED */
    ASSERT_EQ(kcnode_slot_derive(cn, 2, cn, 3, RIGHT_SAME_RIGHTS, 0x9999u),
              IRIS_ERR_ACCESS_DENIED);
    /* inherit (badge 0) keeps 0x1234 */
    ASSERT_EQ(kcnode_slot_derive(cn, 2, cn, 3, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    { struct KObject *o; iris_rights_t r; uint64_t b;
      ASSERT_EQ(kcnode_fetch_badged(cn, 3, &o, &r, &b), IRIS_OK);
      ASSERT_EQ(b, 0x1234u);
      kobject_active_release(o); kobject_release(o); }

    /* fresh badge on a non-identity type (TCB) → INVALID_ARG */
    ASSERT_EQ(install_root(cn, 8, tcb, RIGHT_READ | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(tcb);
    ASSERT_EQ(kcnode_slot_derive(cn, 8, cn, 9, RIGHT_SAME_RIGHTS, 0x1u),
              IRIS_ERR_INVALID_ARG);
    ASSERT_EQ(kcnode_slot_delete(cn, 8), IRIS_OK);

    /* Build A(1) → B(2) → C(3); B already has child C.  Delete B: C must be
     * reparented to A (grandparent adoption), NOT destroyed. */
    g_destroyed = 0;
    ASSERT_EQ(kcnode_slot_delete(cn, 3), IRIS_OK); /* clear, rebuild cleanly */
    /* A=slot1 (root, ep), B=slot2 child of A, C=slot3 child of B */
    ASSERT_EQ(kcnode_slot_derive(cn, 2, cn, 3, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    expect_valid(set, 1, 3, 1);
    ASSERT_EQ(kcnode_slot_delete(cn, 2), IRIS_OK); /* delete intermediate B */
    ASSERT_EQ(g_destroyed, 0);                     /* object survives (slots 1,3 hold it) */
    expect_valid(set, 1, 2, 1);                    /* A + C, one root */
    { struct KObject *o; iris_rights_t r;          /* C still resolves */
      ASSERT_EQ(kcnode_fetch(cn, 3, &o, &r), IRIS_OK);
      kobject_active_release(o); kobject_release(o); }

    /* revoke from A: C (its surviving descendant) dies; A survives. */
    uint32_t revd = 0;
    ASSERT_EQ(kcnode_slot_revoke(cn, 1, &revd), IRIS_OK);
    ASSERT_EQ(revd, 1u);
    expect_valid(set, 1, 1, 1);
    { struct KObject *o; iris_rights_t r;      /* C no longer resolves */
      ASSERT_EQ(kcnode_fetch(cn, 3, &o, &r), IRIS_ERR_NOT_FOUND); }

    ASSERT_EQ(kcnode_slot_delete(cn, 1), IRIS_OK);
    expect_valid(set, 1, 0, 0);
}

/* ── K.2/K.5 — deep tree across CNodes + revoke, siblings survive ───────── */

static void test_mdb_deep_and_revoke(struct KCNode *a, struct KCNode *b,
                                     struct KCNode *c) {
    struct KCNode *set[3] = { a, b, c };
    struct KObject *obj = mk_obj(KOBJ_ENDPOINT);
    ASSERT_NOT_NULL(obj);

    /* root in A[1] */
    ASSERT_EQ(install_root(a, 1, obj, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(obj);

    /* A[1] → B[1] → C[1] → C[2] → ... depth 8, crossing CNodes */
    ASSERT_EQ(kcnode_slot_derive(a, 1, b, 1, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_slot_derive(b, 1, c, 1, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_slot_derive(c, 1, c, 2, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_slot_derive(c, 2, c, 3, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_slot_derive(c, 3, c, 4, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_slot_derive(c, 4, c, 5, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_slot_derive(c, 5, c, 6, RIGHT_SAME_RIGHTS, 0), IRIS_OK);

    /* An INDEPENDENT sibling of A[1]: another root cap (separate authority). */
    struct KObject *obj2 = mk_obj(KOBJ_ENDPOINT);
    ASSERT_NOT_NULL(obj2);
    ASSERT_EQ(install_root(a, 2, obj2, RIGHT_READ | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(obj2);
    ASSERT_EQ(kcnode_slot_derive(a, 2, b, 2, RIGHT_SAME_RIGHTS, 0), IRIS_OK);

    struct mdb_validate_report rep;
    ASSERT_EQ(kcnode_mdb_validate(set, 3, &rep), 0u);
    ASSERT_EQ(rep.nodes, 10u);
    ASSERT_EQ(rep.roots, 2u);           /* A[1] and A[2] */
    ASSERT_TRUE(rep.max_depth >= 7u);

    /* Revoke from A[1]: its 7-node descendance (B[1], C[1..6]) dies; A[1]
     * survives; the independent A[2]→B[2] tree (2 nodes) is untouched. */
    uint32_t revd = 0;
    ASSERT_EQ(kcnode_slot_revoke(a, 1, &revd), IRIS_OK);
    ASSERT_EQ(revd, 7u);
    ASSERT_EQ(kcnode_mdb_validate(set, 3, &rep), 0u);
    ASSERT_EQ(rep.nodes, 3u);           /* A[1] survivor + A[2]→B[2] */
    /* A[1] itself survives; A[2]→B[2] survives */
    { struct KObject *o; iris_rights_t r;
      ASSERT_EQ(kcnode_fetch(a, 1, &o, &r), IRIS_OK); kobject_active_release(o); kobject_release(o);
      ASSERT_EQ(kcnode_fetch(b, 2, &o, &r), IRIS_OK); kobject_active_release(o); kobject_release(o);
      ASSERT_EQ(kcnode_fetch(c, 1, &o, &r), IRIS_ERR_NOT_FOUND); }

    /* cleanup */
    ASSERT_EQ(kcnode_slot_delete(a, 1), IRIS_OK);
    ASSERT_EQ(kcnode_slot_revoke(a, 2, &revd), IRIS_OK);
    ASSERT_EQ(revd, 1u);
    ASSERT_EQ(kcnode_slot_delete(a, 2), IRIS_OK);
    ASSERT_EQ(kcnode_mdb_validate(set, 3, &rep), 0u);
    ASSERT_EQ(rep.nodes, 0u);
}

/* ── K.4 — move preserves descendance, source slot fully cleared ────────── */

static void test_mdb_move(struct KCNode *a, struct KCNode *b) {
    struct KCNode *set[2] = { a, b };
    struct KObject *obj = mk_obj(KOBJ_ENDPOINT);
    ASSERT_NOT_NULL(obj);

    /* A[1] root → A[2] child → A[3] grandchild */
    ASSERT_EQ(install_root(a, 1, obj, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(obj);
    ASSERT_EQ(kcnode_slot_derive(a, 1, a, 2, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_slot_derive(a, 2, a, 3, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    expect_valid(set, 2, 3, 1);

    /* Move the intermediate B-node (A[2]) to another CNode B[5]. */
    ASSERT_EQ(kcnode_slot_move(a, 2, b, 5), IRIS_OK);
    /* source A[2] fully empty */
    { struct KObject *o; iris_rights_t r;
      ASSERT_EQ(kcnode_fetch(a, 2, &o, &r), IRIS_ERR_NOT_FOUND); }
    expect_valid(set, 2, 3, 1);   /* still 3 nodes, still one root (A[1]) */

    /* A[3] is still a descendant of the moved node (now B[5]); revoke from
     * A[1] must still reach both B[5] and A[3]. */
    uint32_t revd = 0;
    ASSERT_EQ(kcnode_slot_revoke(a, 1, &revd), IRIS_OK);
    ASSERT_EQ(revd, 2u);          /* B[5] and A[3] */
    expect_valid(set, 2, 1, 1);
    ASSERT_EQ(kcnode_slot_delete(a, 1), IRIS_OK);
    expect_valid(set, 2, 0, 0);
}

/* ── K.7/K.8 — teardown: CNode destruction deletes (not revokes) ────────── */

static void test_mdb_teardown(struct KCNode *survivor) {
    struct KCNode *victim = kcnode_alloc(16);
    ASSERT_NOT_NULL(victim);
    struct KObject *obj = mk_obj(KOBJ_ENDPOINT);
    ASSERT_NOT_NULL(obj);

    /* root in victim, child in survivor (an "external" descendant). */
    ASSERT_EQ(install_root(victim, 1, obj, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(obj);
    ASSERT_EQ(kcnode_slot_derive(victim, 1, survivor, 5, RIGHT_SAME_RIGHTS, 0), IRIS_OK);

    struct KCNode *both[2] = { victim, survivor };
    expect_valid(both, 2, 2, 1);

    /* Destroy the victim CNode.  Its close = per-slot DELETE (not revoke):
     * the external descendant in `survivor` must SURVIVE, promoted to root. */
    g_destroyed = 0;
    kcnode_close(victim);         /* last ref → destroy → close deletes slots */

    struct KCNode *only[1] = { survivor };
    struct mdb_validate_report rep;
    ASSERT_EQ(kcnode_mdb_validate(only, 1, &rep), 0u);
    ASSERT_EQ(rep.nodes, 1u);     /* survivor[5] remains */
    ASSERT_EQ(rep.roots, 1u);     /* promoted to root */
    { struct KObject *o; iris_rights_t r;
      ASSERT_EQ(kcnode_fetch(survivor, 5, &o, &r), IRIS_OK);
      kobject_active_release(o); kobject_release(o); }
    ASSERT_EQ(g_destroyed, 0);    /* object still referenced by survivor[5] */

    /* deleting the survivor drops the last ref → object destroyed */
    ASSERT_EQ(kcnode_slot_delete(survivor, 5), IRIS_OK);
    ASSERT_EQ(g_destroyed, 1);
}

/* ── Reuse: a re-installed slot inherits NO metadata ────────────────────── */

static void test_mdb_reuse(struct KCNode *cn) {
    struct KCNode *set[1] = { cn };
    struct KObject *o1 = mk_obj(KOBJ_ENDPOINT);
    ASSERT_EQ(install_root(cn, 7, o1, RIGHT_READ | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(o1);
    ASSERT_EQ(kcnode_slot_derive(cn, 7, cn, 8, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_slot_delete(cn, 8), IRIS_OK);
    ASSERT_EQ(kcnode_slot_delete(cn, 7), IRIS_OK);
    /* slot 8 reused as a fresh root: must be a clean root, no stale parent. */
    struct KObject *o2 = mk_obj(KOBJ_ENDPOINT);
    ASSERT_EQ(install_root(cn, 8, o2, RIGHT_READ), IRIS_OK);
    kobject_release(o2);
    expect_valid(set, 1, 1, 1);
    ASSERT_EQ(kcnode_slot_delete(cn, 8), IRIS_OK);
}

/* ── Parte L — deterministic model-based fuzzer ─────────────────────────── */

#define FZ_CNODES 3u
#define FZ_SLOTS  16u     /* usable slots per CNode: 1..15 */
#define FZ_CAP    (FZ_CNODES * FZ_SLOTS)

/* Reference model: parallel graph.  occ[i] = occupied; par[i] = parent flat
 * index or -1 for root.  Flat index = cnode*FZ_SLOTS + slot. */
static uint8_t fz_occ[FZ_CAP];
static int     fz_par[FZ_CAP];

static uint32_t fz_rng_state;
static uint32_t fz_rng(void) {
    fz_rng_state = fz_rng_state * 1664525u + 1013904223u;
    return fz_rng_state >> 8;
}

static int fz_flat(uint32_t c, uint32_t s) { return (int)(c * FZ_SLOTS + s); }

/* Map a live slot pointer back to its flat index within the CNode set. */
static int fz_ptr_to_flat(struct KCNode **cns, struct KCSlot *p) {
    if (!p) return -1;
    for (uint32_t c = 0; c < FZ_CNODES; c++)
        if (p >= cns[c]->slots && p < cns[c]->slots + cns[c]->slot_count)
            return fz_flat(c, (uint32_t)(p - cns[c]->slots));
    return -1;
}

/* Compare the KERNEL parent structure to the model, node by node.  KCSlot is
 * public, so the test reads mdb_parent directly.  Returns 0 on full match. */
static int fz_structural_match(struct KCNode **cns) {
    for (uint32_t c = 0; c < FZ_CNODES; c++) {
        for (uint32_t s = 0; s < FZ_SLOTS; s++) {
            int flat = fz_flat(c, s);
            struct KCSlot *slot = &cns[c]->slots[s];
            int k_occ = slot->object != 0;
            if (k_occ != (int)fz_occ[flat]) return 1;
            if (!k_occ) continue;
            int k_par = fz_ptr_to_flat(cns, slot->mdb_parent);
            if (k_par != fz_par[flat]) return 2;
        }
    }
    return 0;
}

/* model: is `d` in the subtree of `r` (strict descendant)? */
static int fz_is_descendant(int r, int d) {
    int cur = fz_par[d];
    int guard = 0;
    while (cur >= 0 && guard++ < (int)FZ_CAP) {
        if (cur == r) return 1;
        cur = fz_par[cur];
    }
    return 0;
}

/* model revoke: remove every strict descendant of r.  TWO-PASS — the
 * descendant set is computed against the INTACT parent vector first, then
 * removed; clearing par[] during the scan would break the ancestor chain of
 * a deeper node visited later and under-count. */
static uint32_t fz_model_revoke(int r) {
    uint8_t victim[FZ_CAP];
    for (int i = 0; i < (int)FZ_CAP; i++)
        victim[i] = (fz_occ[i] && i != r && fz_is_descendant(r, i)) ? 1u : 0u;
    uint32_t n = 0;
    for (int i = 0; i < (int)FZ_CAP; i++)
        if (victim[i]) { fz_occ[i] = 0; fz_par[i] = -1; n++; }
    return n;
}

/* model delete: reparent children of d to d's parent; if d was root, promote. */
static void fz_model_delete(int d) {
    int gp = fz_par[d];
    for (int i = 0; i < (int)FZ_CAP; i++)
        if (fz_occ[i] && fz_par[i] == d) fz_par[i] = gp;
    fz_occ[d] = 0; fz_par[d] = -1;
}

static void test_mdb_fuzz(struct KCNode **cns, uint32_t seed) {
    /* Start from a genuinely empty kernel state (drain any residue from an
     * earlier sub-test or a prior seed that stopped early). */
    for (uint32_t c = 0; c < FZ_CNODES; c++)
        for (uint32_t s = 1u; s < FZ_SLOTS; s++) {
            uint32_t r; (void)kcnode_slot_revoke(cns[c], s, &r);
            (void)kcnode_slot_delete(cns[c], s);
        }
    for (int i = 0; i < (int)FZ_CAP; i++) { fz_occ[i] = 0; fz_par[i] = -1; }
    fz_rng_state = seed;

    struct mdb_validate_report rep;
    int model_nodes = 0;

    for (int step = 0; step < 4000; step++) {
        uint32_t op = fz_rng() % 100u;
        uint32_t c  = fz_rng() % FZ_CNODES;
        uint32_t s  = 1u + (fz_rng() % (FZ_SLOTS - 1u));
        int flat = fz_flat(c, s);

        if (op < 25u) {
            /* install root */
            if (!fz_occ[flat]) {
                struct KObject *o = mk_obj(KOBJ_ENDPOINT);
                if (o) {
                    iris_error_t e = install_root(cns[c], s, o,
                                     RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
                    kobject_release(o);
                    if (e == IRIS_OK) { fz_occ[flat] = 1; fz_par[flat] = -1; model_nodes++; }
                }
            }
        } else if (op < 60u) {
            /* derive from a random occupied source into (c,s) */
            uint32_t sc = fz_rng() % FZ_CNODES;
            uint32_t ss = 1u + (fz_rng() % (FZ_SLOTS - 1u));
            int sflat = fz_flat(sc, ss);
            if (fz_occ[sflat] && !fz_occ[flat] && sflat != flat) {
                iris_error_t e = kcnode_slot_derive(cns[sc], ss, cns[c], s,
                                                    RIGHT_SAME_RIGHTS, 0);
                if (e == IRIS_OK) { fz_occ[flat] = 1; fz_par[flat] = sflat; model_nodes++; }
            }
        } else if (op < 75u) {
            /* move (c,s) → random empty dest */
            uint32_t dc = fz_rng() % FZ_CNODES;
            uint32_t ds = 1u + (fz_rng() % (FZ_SLOTS - 1u));
            int dflat = fz_flat(dc, ds);
            if (fz_occ[flat] && !fz_occ[dflat] && flat != dflat) {
                iris_error_t e = kcnode_slot_move(cns[c], s, cns[dc], ds);
                if (e == IRIS_OK) {
                    /* model: relabel flat→dflat (same node identity) */
                    for (int i = 0; i < (int)FZ_CAP; i++)
                        if (fz_par[i] == flat) fz_par[i] = dflat;
                    fz_occ[dflat] = 1; fz_par[dflat] = fz_par[flat];
                    fz_occ[flat] = 0; fz_par[flat] = -1;
                }
            }
        } else if (op < 90u) {
            /* delete (c,s) */
            if (fz_occ[flat]) {
                iris_error_t e = kcnode_slot_delete(cns[c], s);
                if (e == IRIS_OK) { fz_model_delete(flat); model_nodes--; }
            }
        } else {
            /* revoke (c,s) */
            if (fz_occ[flat]) {
                uint32_t krevd = 0;
                iris_error_t e = kcnode_slot_revoke(cns[c], s, &krevd);
                if (e == IRIS_OK) {
                    uint32_t mrevd = fz_model_revoke(flat);
                    if (krevd != mrevd) {
                        fprintf(stderr, "  MDB FUZZ seed=%u step=%d revoke flat=%d k=%u m=%u\n",
                                seed, step, flat, krevd, mrevd);
                        g_fail++;
                    }
                    model_nodes -= (int)mrevd;
                }
            }
        }

        /* Invariants after every op: validator clean + node count matches +
         * full parent structure matches the reference model. */
        uint32_t errs = kcnode_mdb_validate(cns, FZ_CNODES, &rep);
        int smatch = fz_structural_match(cns);
        if (errs != 0u || rep.nodes != (uint32_t)model_nodes || smatch != 0) {
            fprintf(stderr, "  MDB FUZZ seed=%u step=%d errs=%u nodes k=%u m=%d struct=%d\n",
                    seed, step, errs, rep.nodes, model_nodes, smatch);
            g_fail++;
            return;   /* stop on first divergence, seed reported */
        }
    }
    g_pass++;   /* one pass credit for a clean 4000-op run */

    /* drain: revoke every root, then delete, leaving the CNodes empty. */
    for (uint32_t c = 0; c < FZ_CNODES; c++)
        for (uint32_t s = 1u; s < FZ_SLOTS; s++) {
            uint32_t r;
            (void)kcnode_slot_revoke(cns[c], s, &r);
            (void)kcnode_slot_delete(cns[c], s);
        }
    ASSERT_EQ(kcnode_mdb_validate(cns, FZ_CNODES, &rep), 0u);
    ASSERT_EQ(rep.nodes, 0u);
}

void test_mdb(void) {
    TEST_SUITE("mdb/cdt (Fase S3)");

    struct KCNode *a = kcnode_alloc(16);
    struct KCNode *b = kcnode_alloc(16);
    struct KCNode *c = kcnode_alloc(16);
    ASSERT_NOT_NULL(a); ASSERT_NOT_NULL(b); ASSERT_NOT_NULL(c);

    test_mdb_basic(a);
    test_mdb_badge_and_delete(a);
    test_mdb_deep_and_revoke(a, b, c);
    test_mdb_move(a, b);
    test_mdb_teardown(a);
    test_mdb_reuse(a);

    /* Model-based fuzzing — several fixed seeds (reproducible). */
    struct KCNode *fz[FZ_CNODES] = { a, b, c };
    static const uint32_t seeds[] = { 1u, 42u, 1337u, 0xC0FFEEu, 0xDEADBEEFu };
    for (uint32_t i = 0; i < sizeof(seeds)/sizeof(seeds[0]); i++)
        test_mdb_fuzz(fz, seeds[i]);

    kcnode_close(a);
    kcnode_close(b);
    kcnode_close(c);
}
