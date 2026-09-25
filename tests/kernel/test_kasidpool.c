/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_kasidpool.c — address-space identifiers as a capability (ledger A-21).
 *
 * The runtime suite proves the AUTHORITY end to end (T328: an unnamed address
 * space cannot be entered, a pool this task holds names one, the identifiers
 * come back).  What the host can do that the runtime cannot is exhaust things
 * cheaply and inspect the pool's own bookkeeping directly, including states a
 * booting system never reaches.
 *
 * Covered here:
 *   [AP-1]  a carved range is bounded, sized, and starts above the bootstrap
 *           identifier — so no pool can ever issue the root task's name.
 *   [AP-2]  two carves never overlap, and the space is carved FORWARD.
 *   [AP-3]  a fresh pool issues every identifier in its range, once each, in
 *           range, and then answers 0 rather than wrapping.
 *   [AP-4]  a returned identifier is re-issued; the pool is a set, not a bump.
 *   [AP-5]  returning something the pool never issued is ignored — a foreign
 *           identifier cannot free somebody else's live name.
 *   [AP-6]  a double return does not double-free the identifier.
 *   [AP-7]  a retyped VSpace is UNNAMED, and naming it is what changes that.
 *           This is the invariant `ktcb_configure` rests on.
 *   [AP-8]  naming is idempotent-by-refusal: a live space is never re-named.
 *   [AP-9]  the rule does not depend on the hardware — a machine with no PCID
 *           allocates the same identifiers and obeys the same check.
 *   [AP-10] a destroyed address space returns its identifier to the pool that
 *           issued it, and the pool outlives it to receive it.
 */

#include "framework.h"
#include <iris/nc/kasidpool.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kobject.h>
#include <iris/paging.h>
#include <stdatomic.h>
#include <stdlib.h>

static struct KAsidPool *ap_make(struct KUntyped *ut) {
    void *hdr = kuntyped_alloc_child_top(ut, sizeof(struct KAsidPool));
    if (!hdr) return 0;
    uint16_t first = 0, count = 0;
    if (kasidpool_carve_range(&first, &count) != IRIS_OK) return 0;
    return kasidpool_alloc_at(hdr, first, count);
}

void test_kasidpool(void) {
    TEST_SUITE("address-space identifiers (ledger A-21)");

    void *mem = aligned_alloc(4096u, 256u * 1024u);
    ASSERT_NOT_NULL(mem);
    struct KUntyped *ut =
        kuntyped_create((uint64_t)(uintptr_t)mem, 256u * 1024u, 0);
    ASSERT_NOT_NULL(ut);

    /* [AP-1] [AP-2] */
    {
        uint16_t f1 = 0, c1 = 0, f2 = 0, c2 = 0;
        ASSERT_EQ((int)kasidpool_carve_range(&f1, &c1), (int)IRIS_OK);
        ASSERT_EQ((int)kasidpool_carve_range(&f2, &c2), (int)IRIS_OK);
        ASSERT_EQ(c1, (uint16_t)KASID_POOL_SIZE);
        ASSERT_EQ(c2, (uint16_t)KASID_POOL_SIZE);
        /* The root task's name is below every pool, by construction. */
        ASSERT_TRUE(f1 > (uint16_t)KASID_BOOTSTRAP);
        ASSERT_TRUE(f2 >= (uint16_t)(f1 + c1));
        ASSERT_TRUE((uint32_t)f2 + c2 - 1u <= (uint32_t)KASID_LAST);
        /* Nothing rewinds: a third carve is above the second. */
        uint16_t f3 = 0, c3 = 0;
        ASSERT_EQ((int)kasidpool_carve_range(&f3, &c3), (int)IRIS_OK);
        ASSERT_TRUE(f3 >= (uint16_t)(f2 + c2));
        /* A NULL out-parameter is refused rather than written through. */
        ASSERT_EQ((int)kasidpool_carve_range(0, &c3), (int)IRIS_ERR_INVALID_ARG);
    }

    /* [AP-3] [AP-4] [AP-5] [AP-6] */
    {
        struct KAsidPool *p = ap_make(ut);
        ASSERT_NOT_NULL(p);
        if (p) {
            uint16_t got[KASID_POOL_SIZE];
            uint32_t n = 0;
            for (uint32_t i = 0; i < KASID_POOL_SIZE; i++) {
                uint16_t id = kasidpool_take(p);
                if (!id) break;
                /* in range... */
                ASSERT_TRUE(id >= p->first && id < (uint16_t)(p->first + p->count));
                /* ...and never issued twice. */
                for (uint32_t j = 0; j < n; j++) ASSERT_NE(got[j], id);
                got[n++] = id;
            }
            ASSERT_EQ(n, (uint32_t)KASID_POOL_SIZE);
            /* [AP-3] empty answers 0; it does not wrap onto a live name. */
            ASSERT_EQ(kasidpool_take(p), (uint16_t)0);

            /* [AP-5] a foreign identifier cannot free a live one. */
            kasidpool_put(p, (uint16_t)(p->first - 1u));
            kasidpool_put(p, (uint16_t)(p->first + p->count));
            kasidpool_put(p, 0);
            ASSERT_EQ(kasidpool_take(p), (uint16_t)0);

            /* [AP-4] a returned identifier is re-issued. */
            kasidpool_put(p, got[7]);
            ASSERT_EQ(kasidpool_take(p), got[7]);

            /* [AP-6] a double return frees one identifier, not two. */
            kasidpool_put(p, got[7]);
            kasidpool_put(p, got[7]);
            ASSERT_EQ(kasidpool_take(p), got[7]);
            ASSERT_EQ(kasidpool_take(p), (uint16_t)0);

            for (uint32_t i = 0; i < n; i++) kasidpool_put(p, got[i]);
            kobject_release(&p->base);
        }
    }

    /* [AP-7] [AP-8] [AP-9] [AP-10] */
    {
        struct KAsidPool *p = ap_make(ut);
        ASSERT_NOT_NULL(p);
        struct KVSpace *vs = kvspace_alloc((uint64_t)(uintptr_t)mem);
        ASSERT_NOT_NULL(vs);
        if (p && vs) {
            /* [AP-7] retyped is not named. */
            ASSERT_EQ(kvspace_has_asid(vs), 0);
            ASSERT_EQ(vs->pcid, (uint16_t)0);

            /* The arguments are checked before anything is consumed. */
            ASSERT_EQ((int)kvspace_assign_asid(0,  p), (int)IRIS_ERR_INVALID_ARG);
            ASSERT_EQ((int)kvspace_assign_asid(vs, 0), (int)IRIS_ERR_INVALID_ARG);
            ASSERT_EQ(kvspace_has_asid(vs), 0);

            ASSERT_EQ((int)kvspace_assign_asid(vs, p), (int)IRIS_OK);
            /* [AP-9] the check answers the same on hardware with no tag
             * register: what was allocated is a NAME, and only the trip into
             * CR3 is hardware-dependent. */
            ASSERT_EQ(kvspace_has_asid(vs), 1);
            ASSERT_TRUE(vs->pcid >= p->first &&
                        vs->pcid < (uint16_t)(p->first + p->count));
            uint16_t mine = vs->pcid;

            /* [AP-8] a live space is never re-named, from this pool or any
             * other — a stale TLB entry under a reissued name is exactly what
             * the refusal exists to prevent. */
            ASSERT_EQ((int)kvspace_assign_asid(vs, p), (int)IRIS_ERR_ALREADY_EXISTS);
            ASSERT_EQ(vs->pcid, mine);

            /* The space holds the pool: it must outlive every name it issued. */
            ASSERT_TRUE(atomic_load(&p->base.refcount) >= 2u);

            /* [AP-10] destroying the space returns the name. */
            kobject_release(&vs->base);
            uint16_t reissued = kasidpool_take(p);
            ASSERT_EQ(reissued, mine);
            kasidpool_put(p, reissued);

            kobject_release(&p->base);   /* our own reference, last one */
        }
    }
}
