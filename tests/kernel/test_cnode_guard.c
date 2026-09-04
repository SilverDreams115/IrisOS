/*
 * test_cnode_guard.c — CNode guards (Stage 8-cap, ledger D-2).
 *
 * A guard belongs to the CAPABILITY, not to the CNode: seL4 stores it in
 * cap_cnode_cap, so two capabilities to one CNode can carry different guards
 * and one holder can see a sparse CSpace without changing anybody else's view.
 * IRIS's KCSlot IS the capability, so the guard lives in the slot.
 *
 * Within one level a CPtr reads [guard][index] from MSB to LSB — the same
 * relative order seL4 resolves in, so a CSpace laid out for seL4 addresses the
 * same way here.
 *
 * The guard cases below are the ones a walk can get wrong: the default must be
 * indistinguishable from the pre-guard kernel (G-1), a mismatch must FAIL
 * rather than fall through to some other slot (G-4), and the guard that is
 * checked must be the one on the capability the walk descended THROUGH, not
 * the one on the CNode it lands in (G-8).
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
#include <iris/kpage.h>
#include <string.h>

static void cg_destroy(struct KObject *obj) { (void)obj; }
static const struct KObjectOps cg_ops = { .close = NULL, .destroy = cg_destroy };

static struct KObject *cg_obj(void) {
    struct KObject *o = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (o) kobject_init(o, KOBJ_ENDPOINT, &cg_ops);
    return o;
}

/* Build root(8 slots) -> slot `s` holds mid(8 slots) -> mid slot `m` holds a
 * leaf endpoint.  Returns the leaf so a test can assert identity. */
static struct KObject *cg_two_level(struct KCNode **out_root,
                                    struct KCNode **out_mid,
                                    uint32_t s, uint32_t m) {
    struct KCNode *root = kcnode_alloc(8);
    struct KCNode *mid  = kcnode_alloc(8);
    struct KObject *leaf = cg_obj();
    if (!root || !mid || !leaf) return NULL;

    kobject_active_retain(&root->base);          /* structural root */
    if (kcnode_mint(root, s, &mid->base, RIGHT_READ | RIGHT_WRITE) != IRIS_OK)
        return NULL;
    if (kcnode_mint(mid, m, leaf, RIGHT_READ) != IRIS_OK)
        return NULL;

    *out_root = root;
    *out_mid  = mid;
    return leaf;
}

void test_cnode_guard(void) {
    TEST_SUITE("CNode guards (Stage 8-cap / D-2)");

    /* ── G-1: the default is no guard, and resolves exactly as before ──────
     * Every slot ever installed starts guard_bits = 0.  This is the assertion
     * that makes the whole feature additive: if it fails, guards changed the
     * meaning of a CPtr that predates them. */
    {
        struct KCNode *root, *mid;
        struct KObject *leaf = cg_two_level(&root, &mid, 3, 5);
        ASSERT_NOT_NULL(leaf);

        /* root radix 3, mid radix 3: cptr = mid_idx << 3 | root_idx */
        iris_cptr_t cptr = (iris_cptr_t)((5u << 3) | 3u);
        struct KObject *out; iris_rights_t r;
        ASSERT_EQ(cspace_resolve_cap(root, cptr, RIGHT_NONE, &out, &r), IRIS_OK);
        ASSERT_EQ(out, leaf);
        kobject_active_release(out); kobject_release(out);
    }

    /* ── G-2: a guard is refused on anything that is not a CNode ───────────
     * A guard on a non-CNode capability has no meaning, and silently storing
     * one would make the slot lie about how it resolves. */
    {
        struct KCNode *root = kcnode_alloc(8);
        struct KObject *ep  = cg_obj();
        ASSERT_NOT_NULL(root); ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 2, ep, RIGHT_READ), IRIS_OK);
        ASSERT_EQ(kcnode_slot_set_guard(root, 2, 1u, 1u), IRIS_ERR_WRONG_TYPE);
        /* and an empty slot has nothing to guard */
        ASSERT_EQ(kcnode_slot_set_guard(root, 4, 1u, 1u), IRIS_ERR_NOT_FOUND);
    }

    /* ── G-3: a guard must fit the width it declares ───────────────────────
     * Accepting a wider value and truncating it would silently produce a
     * capability that resolves for an address the caller never asked for. */
    {
        struct KCNode *root, *mid;
        ASSERT_NOT_NULL(cg_two_level(&root, &mid, 3, 5));
        ASSERT_EQ(kcnode_slot_set_guard(root, 3, 0x4u, 2u), IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(kcnode_slot_set_guard(root, 3, 0x3u, 2u), IRIS_OK);
        /* width beyond the CPtr space is refused outright */
        ASSERT_EQ(kcnode_slot_set_guard(root, 3, 0u,
                                        (uint8_t)(KCNODE_GUARD_BITS_MAX + 1u)),
                  IRIS_ERR_INVALID_ARG);
    }

    /* ── G-4: the guard bits must MATCH, and a mismatch fails ──────────────
     * Not "falls through to another slot", not "is ignored" — the point of a
     * guard is that an address which does not carry it is not an address. */
    {
        struct KCNode *root, *mid;
        struct KObject *leaf = cg_two_level(&root, &mid, 3, 5);
        ASSERT_NOT_NULL(leaf);
        /* guard 0b10, 2 bits, on the capability to `mid` */
        ASSERT_EQ(kcnode_slot_set_guard(root, 3, 0x2u, 2u), IRIS_OK);

        struct KObject *out; iris_rights_t r;

        /* Correct: [guard=0b10][mid_idx=5][root_idx=3] */
        iris_cptr_t ok = (iris_cptr_t)((0x2u << 6) | (5u << 3) | 3u);
        ASSERT_EQ(cspace_resolve_cap(root, ok, RIGHT_NONE, &out, &r), IRIS_OK);
        ASSERT_EQ(out, leaf);
        kobject_active_release(out); kobject_release(out);

        /* Wrong guard: same slot, different guard bits → NOT_FOUND */
        iris_cptr_t bad = (iris_cptr_t)((0x1u << 6) | (5u << 3) | 3u);
        ASSERT_EQ(cspace_resolve_cap(root, bad, RIGHT_NONE, &out, &r),
                  IRIS_ERR_NOT_FOUND);

        /* The pre-guard address is no longer valid — the guard really is
         * consumed, not merely compared against whatever happened to be there. */
        iris_cptr_t old = (iris_cptr_t)((5u << 3) | 3u);
        ASSERT_EQ(cspace_resolve_cap(root, old, RIGHT_NONE, &out, &r),
                  IRIS_ERR_NOT_FOUND);
    }

    /* ── G-5: width 0 removes the guard and restores the old address ───────*/
    {
        struct KCNode *root, *mid;
        struct KObject *leaf = cg_two_level(&root, &mid, 3, 5);
        ASSERT_NOT_NULL(leaf);
        ASSERT_EQ(kcnode_slot_set_guard(root, 3, 0x2u, 2u), IRIS_OK);
        ASSERT_EQ(kcnode_slot_set_guard(root, 3, 0u, 0u), IRIS_OK);

        struct KObject *out; iris_rights_t r;
        iris_cptr_t plain = (iris_cptr_t)((5u << 3) | 3u);
        ASSERT_EQ(cspace_resolve_cap(root, plain, RIGHT_NONE, &out, &r), IRIS_OK);
        ASSERT_EQ(out, leaf);
        kobject_active_release(out); kobject_release(out);
    }

    /* ── G-6: leftover bits are still rejected, guard or no guard ──────────
     * Stage 4 Step 6b made CPtr resolution injective: bits remaining at a
     * non-CNode terminal are INVALID_ARG, not a silent alias.  A guard
     * consumes bits, so this rule has to be re-derived on top of it or the
     * alias comes back one level up. */
    {
        struct KCNode *root, *mid;
        ASSERT_NOT_NULL(cg_two_level(&root, &mid, 3, 5));
        ASSERT_EQ(kcnode_slot_set_guard(root, 3, 0x2u, 2u), IRIS_OK);

        struct KObject *out; iris_rights_t r;
        /* one bit above the guard: nothing left to descend into */
        iris_cptr_t extra = (iris_cptr_t)((1u << 8) | (0x2u << 6) | (5u << 3) | 3u);
        ASSERT_EQ(cspace_resolve_cap(root, extra, RIGHT_NONE, &out, &r),
                  IRIS_ERR_INVALID_ARG);
    }

    /* ── G-7: a guard is CAPABILITY-local, not object-local ────────────────
     * The property that makes guards worth having: two capabilities to the
     * same CNode, guarded differently, resolve at different addresses.  If the
     * guard lived on the KCNode this test could not pass, and IRIS would have
     * a guard that is not seL4's guard. */
    {
        struct KCNode *root = kcnode_alloc(8);
        struct KCNode *mid  = kcnode_alloc(8);
        struct KObject *leaf = cg_obj();
        ASSERT_NOT_NULL(root); ASSERT_NOT_NULL(mid); ASSERT_NOT_NULL(leaf);
        kobject_active_retain(&root->base);

        /* two capabilities to the SAME mid CNode, in slots 1 and 2 */
        ASSERT_EQ(kcnode_mint(root, 1, &mid->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        ASSERT_EQ(kcnode_mint(root, 2, &mid->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        ASSERT_EQ(kcnode_mint(mid, 6, leaf, RIGHT_READ), IRIS_OK);

        ASSERT_EQ(kcnode_slot_set_guard(root, 1, 0x1u, 1u), IRIS_OK);
        ASSERT_EQ(kcnode_slot_set_guard(root, 2, 0x0u, 1u), IRIS_OK);

        struct KObject *out; iris_rights_t r;

        /* through slot 1, guard must be 1 */
        iris_cptr_t via1_ok  = (iris_cptr_t)((0x1u << 6) | (6u << 3) | 1u);
        iris_cptr_t via1_bad = (iris_cptr_t)((0x0u << 6) | (6u << 3) | 1u);
        ASSERT_EQ(cspace_resolve_cap(root, via1_ok, RIGHT_NONE, &out, &r), IRIS_OK);
        ASSERT_EQ(out, leaf);
        kobject_active_release(out); kobject_release(out);
        ASSERT_EQ(cspace_resolve_cap(root, via1_bad, RIGHT_NONE, &out, &r),
                  IRIS_ERR_NOT_FOUND);

        /* through slot 2, guard must be 0 — same object, different address */
        iris_cptr_t via2_ok  = (iris_cptr_t)((0x0u << 6) | (6u << 3) | 2u);
        iris_cptr_t via2_bad = (iris_cptr_t)((0x1u << 6) | (6u << 3) | 2u);
        ASSERT_EQ(cspace_resolve_cap(root, via2_ok, RIGHT_NONE, &out, &r), IRIS_OK);
        ASSERT_EQ(out, leaf);
        kobject_active_release(out); kobject_release(out);
        ASSERT_EQ(cspace_resolve_cap(root, via2_bad, RIGHT_NONE, &out, &r),
                  IRIS_ERR_NOT_FOUND);
    }

    /* ── G-8: the guard checked is the one on the capability DESCENDED
     * THROUGH, not one sitting on the CNode being entered ──────────────────
     * The off-by-one-level mistake this feature invites.  A guard installed on
     * a capability that is never descended through must have no effect. */
    {
        struct KCNode *root, *mid;
        struct KObject *leaf = cg_two_level(&root, &mid, 3, 5);
        ASSERT_NOT_NULL(leaf);

        /* Guard the LEAF's slot inside mid.  The leaf is not a CNode, so this
         * is refused — and therefore cannot perturb the walk. */
        ASSERT_EQ(kcnode_slot_set_guard(mid, 5, 0x3u, 2u), IRIS_ERR_WRONG_TYPE);

        struct KObject *out; iris_rights_t r;
        iris_cptr_t cptr = (iris_cptr_t)((5u << 3) | 3u);
        ASSERT_EQ(cspace_resolve_cap(root, cptr, RIGHT_NONE, &out, &r), IRIS_OK);
        ASSERT_EQ(out, leaf);
        kobject_active_release(out); kobject_release(out);
    }
}
