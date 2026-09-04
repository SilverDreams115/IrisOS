/*
 * test_syscall_cspace.c — the CSpace SYSCALL layer, under host unit test.
 *
 * Why this file exists.  Until it, 76% of the kernel's C had no host unit
 * tests, and the largest untested piece was the syscall layer: ~4,600 lines
 * whose entire job is validating arguments and checking authority before
 * anything else runs.  It was covered only by the runtime suite, through the
 * syscall boundary, where a rejection and a crash look similar from ring 3 and
 * where fault injection is not available at all.
 *
 * That is the wrong way round.  The object layer — which is careful, and which
 * these suites do test exhaustively — is protected BY the syscall layer.  Every
 * assertion about kcnode or cspace assumes something upstream already rejected
 * the malformed CPtr, the wrong type, the missing right.  Those rejections had
 * never been asserted anywhere they could be enumerated.
 *
 * What is asserted here is only what the syscall layer is FOR: that it says no.
 * The successful paths belong to the runtime suite, which exercises them as a
 * real service through a real CSpace; what the runtime suite cannot do is walk
 * the refusal cases one at a time and prove each returns the specific error it
 * promises rather than the nearest plausible one.
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/cspace.h>
#include <iris/nc/rights.h>
#include <iris/task.h>
#include <iris/syscall.h>
#include <iris/kpage.h>
#include <string.h>

void test_set_current_task(struct task *t);
uint32_t test_restart_count(void);

/* Handlers under test (declared in syscall_priv.h, which is kernel-private). */
uint64_t sys_cspace_mint(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cspace_revoke(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cspace_set_guard(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cap_identify(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cap_same_object(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cspace_self(uint64_t a0, uint64_t a1, uint64_t a2);

/* A syscall's negative return is the error, sign-extended into 64 bits. */
static long sc_err(uint64_t r) { return (long)(int64_t)r; }

static void sk_destroy(struct KObject *o) { (void)o; }
static const struct KObjectOps sk_ops = { .close = NULL, .destroy = sk_destroy };

static struct KObject *sk_obj(kobject_type_t type) {
    struct KObject *o = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (o) kobject_init(o, type, &sk_ops);
    return o;
}

/* A caller with a root CSpace, installed as the current task. */
static struct task *sk_caller(struct KCNode **out_root) {
    struct task *t = (struct task *)kpage_alloc((uint32_t)sizeof(struct task));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    struct KCNode *root = kcnode_alloc(256);
    if (!root) return NULL;
    kobject_active_retain(&root->base);     /* structural root, as a real one holds */
    t->cspace_root = root;
    test_set_current_task(t);
    if (out_root) *out_root = root;
    return t;
}

static void sk_teardown(void) { test_set_current_task(NULL); }

void test_syscall_cspace(void) {
    TEST_SUITE("CSpace syscall layer (authority checks)");

    /* ── SC-1: no caller, or a caller with no CSpace, is INVALID_ARG ──────
     * The first line of every handler.  It is not a formality: everything
     * after it dereferences the root. */
    {
        test_set_current_task(NULL);
        ASSERT_EQ(sc_err(sys_cspace_mint(1, 2, 0)),        (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cspace_revoke(1, 0, 0)),      (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cspace_set_guard(1, 0, 0)),   (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cap_identify(1, 0, 0)),       (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cap_same_object(1, 2, 0)),    (long)IRIS_ERR_INVALID_ARG);

        struct task *t = (struct task *)kpage_alloc((uint32_t)sizeof(struct task));
        ASSERT_NOT_NULL(t);
        memset(t, 0, sizeof(*t));          /* cspace_root == NULL */
        test_set_current_task(t);
        ASSERT_EQ(sc_err(sys_cspace_mint(1, 2, 0)),      (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cspace_revoke(1, 0, 0)),    (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cspace_set_guard(1, 0, 0)), (long)IRIS_ERR_INVALID_ARG);
        sk_teardown();
    }

    /* ── SC-2: a HANDLE value is INVALID_ARG, with no fallback ────────────
     * Stage 4 deleted the handle table; the boundary survives as a rejection.
     * This is the charter's "one authority namespace" made testable: a value
     * at or above HANDLE_TAG is not an address in some other table, it is a
     * malformed argument. */
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        const uint64_t handle_shaped = (uint64_t)1u << 31;   /* HANDLE_TAG */
        ASSERT_EQ(sc_err(sys_cspace_mint(handle_shaped, 2, 0)),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cspace_revoke(handle_shaped, 0, 0)),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cspace_set_guard(handle_shaped, 0, 0)),
                  (long)IRIS_ERR_INVALID_ARG);
        sk_teardown();
    }

    /* ── SC-3: CPTR_NULL is never an address ─────────────────────────────*/
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(sc_err(sys_cspace_mint(CPTR_NULL, 2, 0)), (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(sc_err(sys_cap_identify(CPTR_NULL, 0, 0)), (long)IRIS_ERR_INVALID_ARG);
        sk_teardown();
    }

    /* ── SC-4: minting into slot 0 is refused ────────────────────────────
     * Slot 0 is the null slot in every CNode by kernel convention and is never
     * populated.  If a mint could fill it, CPTR_NULL would resolve — and every
     * "is this capability null" test in the system is a comparison against 0. */
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        struct KObject *ep = sk_obj(KOBJ_ENDPOINT);
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 5, ep, RIGHT_READ | RIGHT_DUPLICATE), IRIS_OK);

        /* dest slot 0 (high half of arg1 == 0) */
        ASSERT_EQ(sc_err(sys_cspace_mint(5, 0, (uint64_t)RIGHT_READ)),
                  (long)IRIS_ERR_INVALID_ARG);
        sk_teardown();
    }

    /* ── SC-5: a guard is refused on anything that is not a CNode ─────────
     * The syscall-level half of what host G-2 asserts at the object level: a
     * guard on a non-CNode capability has no meaning, and storing one would
     * make the slot lie about how it resolves. */
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        struct KObject *ep = sk_obj(KOBJ_ENDPOINT);
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 7, ep, RIGHT_READ), IRIS_OK);
        ASSERT_EQ(sc_err(sys_cspace_set_guard(7, 0, 1)), (long)IRIS_ERR_WRONG_TYPE);

        /* an empty slot has nothing to guard */
        ASSERT_EQ(sc_err(sys_cspace_set_guard(9, 0, 1)), (long)IRIS_ERR_NOT_FOUND);
        sk_teardown();
    }

    /* ── SC-6: a guard wider than the CPtr space is refused at the syscall
     * boundary, before the object layer is reached ──────────────────────── */
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        struct KCNode *mid = kcnode_alloc(8);
        ASSERT_NOT_NULL(mid);
        ASSERT_EQ(kcnode_mint(root, 11, &mid->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);

        ASSERT_EQ(sc_err(sys_cspace_set_guard(11, 0, KCNODE_GUARD_BITS_MAX + 1u)),
                  (long)IRIS_ERR_INVALID_ARG);
        /* and a guard that does not fit the width it declares */
        ASSERT_EQ(sc_err(sys_cspace_set_guard(11, 0x4, 2)),
                  (long)IRIS_ERR_INVALID_ARG);
        /* the legal one succeeds, so the refusals above are about the argument
         * and not about the slot */
        ASSERT_EQ(sc_err(sys_cspace_set_guard(11, 0x3, 2)), 0);
        sk_teardown();
    }

    /* ── SC-7: revoke on an empty slot is NOT_FOUND, not success ─────────
     * A revoke that quietly reports "0 revoked" for a slot that does not exist
     * lets a caller believe it has torn down a delegation it never made. */
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(sc_err(sys_cspace_revoke(13, 0, 0)), (long)IRIS_ERR_NOT_FOUND);
        sk_teardown();
    }

    /* ── SC-8: identify reports the TYPE, and same_object compares IDENTITY
     * rather than rights ────────────────────────────────────────────────
     * The second half is the load-bearing one: two capabilities to one object,
     * minted with different rights, are the SAME object.  A comparison that
     * folded rights in would make "prove this is the cap I sent you" fail for
     * every legitimately reduced delegation. */
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        struct KObject *ep = sk_obj(KOBJ_ENDPOINT);
        struct KObject *nt = sk_obj(KOBJ_NOTIFICATION);
        ASSERT_NOT_NULL(ep); ASSERT_NOT_NULL(nt);

        ASSERT_EQ(kcnode_mint(root, 21, ep, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        ASSERT_EQ(kcnode_mint(root, 22, ep, RIGHT_READ), IRIS_OK);  /* reduced */
        ASSERT_EQ(kcnode_mint(root, 23, nt, RIGHT_READ), IRIS_OK);

        ASSERT_EQ((long)sys_cap_identify(21, 0, 0), (long)KOBJ_ENDPOINT);
        ASSERT_EQ((long)sys_cap_identify(23, 0, 0), (long)KOBJ_NOTIFICATION);

        ASSERT_EQ((long)sys_cap_same_object(21, 22, 0), 1L);  /* same object */
        ASSERT_EQ((long)sys_cap_same_object(21, 23, 0), 0L);  /* different    */

        /* an empty slot is not "some object" */
        ASSERT_EQ(sc_err(sys_cap_identify(24, 0, 0)), (long)IRIS_ERR_NOT_FOUND);
        sk_teardown();
    }

    /* ── SC-9: CSPACE_SELF needs a destination and refuses slot 0 ────────
     * Every capability is created INTO a slot since Stage 4; a creator with no
     * destination has nowhere to put its result and must not invent one. */
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(sc_err(sys_cspace_self(0, 0, 0)), (long)IRIS_ERR_INVALID_ARG);
        sk_teardown();
    }

    /* ── SC-10: revoke PARKS when the subtree is wider than one slice ────
     * The unit-level half of T311.  From ring 3 a revoke that finished and one
     * that parked both return, and only the restart gauge tells them apart;
     * here the difference is directly observable, so the preemption point can
     * be asserted deterministically rather than inferred from a counter that
     * anything else in the system could also have moved. */
    {
        struct KCNode *root; struct task *t = sk_caller(&root);
        ASSERT_NOT_NULL(t);
        struct KObject *ep = sk_obj(KOBJ_ENDPOINT);
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 40, ep, RIGHT_READ | RIGHT_DUPLICATE), IRIS_OK);

        /* Derive more children than one slice can take. */
        uint32_t made = 0;
        for (uint32_t i = 0; i < 24u; i++) {
            if (sc_err(sys_cspace_mint(40, ((uint64_t)(41u + i) << 32),
                                       (uint64_t)RIGHT_READ)) == 0)
                made++;
        }
        ASSERT_EQ(made, 24u);

        uint32_t before = test_restart_count();
        t->sc_restart = 0;
        (void)sys_cspace_revoke(40, 0, 0);
        /* It asked to be re-executed rather than running the whole subtree. */
        ASSERT_EQ(test_restart_count() > before, 1);
        ASSERT_EQ(t->sc_restart, 1u);

        /* Driving it the way the dispatcher would drives it to completion, and
         * the total is the whole job — not the last slice. */
        uint32_t guard = 0;
        while (t->sc_restart && guard++ < 100u) {
            t->sc_restart = 0;
            t->sc_reentry = 1;
            (void)sys_cspace_revoke(40, 0, 0);
        }
        ASSERT_EQ(t->sc_restart, 0u);
        ASSERT_EQ((uint32_t)t->sc_acc, made);
        sk_teardown();
    }

    /* Leave no caller installed: the object-layer suites are written against a
     * kernel with no current task, and the CSpace walk's root-guard lookup is
     * deliberately inert in that case. */
    test_set_current_task(NULL);
}
