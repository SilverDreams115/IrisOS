#include "framework.h"
#include <iris/nc/rights.h>

void test_rights(void) {
    TEST_SUITE("rights");

    /* rights_check: zero required always fails */
    ASSERT_TRUE(!rights_check(RIGHT_READ, RIGHT_NONE));

    /* rights_check: exact match */
    ASSERT_TRUE(rights_check(RIGHT_READ | RIGHT_WRITE, RIGHT_READ));
    ASSERT_TRUE(rights_check(RIGHT_READ | RIGHT_WRITE, RIGHT_WRITE));
    ASSERT_TRUE(rights_check(RIGHT_READ | RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE));

    /* rights_check: missing bit fails */
    ASSERT_TRUE(!rights_check(RIGHT_READ, RIGHT_WRITE));
    ASSERT_TRUE(!rights_check(RIGHT_READ, RIGHT_READ | RIGHT_DUPLICATE));

    /* rights_reduce: intersection is never an elevation */
    iris_rights_t full = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
    ASSERT_EQ(rights_reduce(full, RIGHT_READ), RIGHT_READ);
    ASSERT_EQ(rights_reduce(full, RIGHT_READ | RIGHT_WRITE), RIGHT_READ | RIGHT_WRITE);
    ASSERT_EQ(rights_reduce(RIGHT_READ, RIGHT_WRITE), RIGHT_NONE);

    /* rights_reduce: SAME_RIGHTS strips the flag and returns base */
    iris_rights_t base = RIGHT_READ | RIGHT_DUPLICATE;
    iris_rights_t r = rights_reduce(base, base | RIGHT_SAME_RIGHTS);
    ASSERT_EQ(r, base);
    ASSERT_TRUE(!(r & RIGHT_SAME_RIGHTS));

    /* rights_reduce: monotonic — result is always a subset of base */
    iris_rights_t res = rights_reduce(RIGHT_READ, RIGHT_READ | RIGHT_WRITE);
    ASSERT_TRUE((res & ~RIGHT_READ) == 0);

    /* RIGHT_NONE passthrough */
    ASSERT_EQ(rights_reduce(RIGHT_NONE, RIGHT_READ), RIGHT_NONE);
    ASSERT_EQ(rights_reduce(RIGHT_READ, RIGHT_NONE), RIGHT_NONE);

    /*
     * ── RG-1..RG-5: the rights SET, pinned (ledger D-3) ──────────────────
     *
     * D-3 was carried as "deliberate, REVISABLE" for eight stages and is now
     * declared permanent, so the set has to be a set that cannot drift.
     *
     * IRIS's rights are not seL4's, and the difference is deliberate.  seL4
     * has Read/Write/Grant/GrantReply and copyability is not a right at all —
     * any holder may copy anything it holds, because copying grants no
     * authority the holder did not already have.  That is true of AUTHORITY
     * and false of CONFINEMENT: a child that can copy its endpoint capability
     * can seed a third party with it.  `RIGHT_DUPLICATE` refuses that at the
     * door, and deleting it would not move the property into the derivation
     * tree — the tree records what was derived, it does not prevent deriving.
     *
     * So: "IRIS uses seL4's rights" is never a correct sentence, and these
     * assertions are what keeps that statement's opposite checkable.
     */

    /* RG-1: the seven rights, at the values every stored capability and every
     * on-disk manifest already assumes.  Changing one silently re-labels the
     * authority of every capability minted before the change. */
    ASSERT_EQ((unsigned long)RIGHT_NONE,      0x00UL);
    ASSERT_EQ((unsigned long)RIGHT_READ,      0x01UL);
    ASSERT_EQ((unsigned long)RIGHT_WRITE,     0x02UL);
    ASSERT_EQ((unsigned long)RIGHT_DUPLICATE, 0x04UL);
    ASSERT_EQ((unsigned long)RIGHT_TRANSFER,  0x08UL);
    ASSERT_EQ((unsigned long)RIGHT_WAIT,      0x10UL);
    ASSERT_EQ((unsigned long)RIGHT_ROUTE,     0x20UL);
    ASSERT_EQ((unsigned long)RIGHT_MANAGE,    0x40UL);

    /* RG-2: distinct single bits, and no right overlaps another.  Two rights
     * sharing a bit is not a typo that shows up as a failure somewhere — it is
     * one authority silently granting a second one, everywhere, forever. */
    {
        const iris_rights_t all[] = {
            RIGHT_READ, RIGHT_WRITE, RIGHT_DUPLICATE, RIGHT_TRANSFER,
            RIGHT_WAIT, RIGHT_ROUTE, RIGHT_MANAGE,
        };
        const unsigned n = sizeof(all) / sizeof(all[0]);
        iris_rights_t seen = 0;
        for (unsigned i = 0; i < n; i++) {
            ASSERT_TRUE(all[i] != 0);
            ASSERT_TRUE((all[i] & (all[i] - 1u)) == 0u);   /* one bit */
            ASSERT_TRUE((seen & all[i]) == 0u);            /* not already used */
            seen |= all[i];
        }
        ASSERT_EQ((unsigned long)seen, 0x7FUL);            /* exactly seven */
    }

    /* RG-3: the request flag is not a right.  It shares the word, so a check
     * that treated it as one would let a caller ask for an authority by
     * setting a flag the kernel strips everywhere else. */
    ASSERT_TRUE((RIGHT_SAME_RIGHTS & 0x7FU) == 0u);
    ASSERT_TRUE(!rights_check(RIGHT_SAME_RIGHTS, RIGHT_READ));
    ASSERT_TRUE(!rights_check(RIGHT_READ, RIGHT_SAME_RIGHTS));

    /* RG-4: a check requires ALL the bits asked for, not any of them.  The
     * difference between `(rights & required) == required` and `(rights &
     * required) != 0` is the difference between "has this authority" and "has
     * some of this authority", and every two-right check in the kernel — the
     * IPC buffer's READ|WRITE among them — depends on the first reading. */
    ASSERT_TRUE(!rights_check(RIGHT_READ, RIGHT_READ | RIGHT_WRITE));
    ASSERT_TRUE(!rights_check(RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE));
    ASSERT_TRUE(rights_check(RIGHT_READ | RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE));
    ASSERT_TRUE(!rights_check(RIGHT_MANAGE, RIGHT_READ | RIGHT_MANAGE));

    /* RG-5: no reduction elevates, for every pair in the set.  Exhaustive
     * rather than sampled, because "reduce" is the only operation that ever
     * writes a rights word the holder did not already have. */
    {
        const iris_rights_t all[] = {
            RIGHT_NONE, RIGHT_READ, RIGHT_WRITE, RIGHT_DUPLICATE,
            RIGHT_TRANSFER, RIGHT_WAIT, RIGHT_ROUTE, RIGHT_MANAGE,
        };
        const unsigned n = sizeof(all) / sizeof(all[0]);
        for (unsigned i = 0; i < n; i++)
            for (unsigned j = 0; j < n; j++) {
                iris_rights_t got = rights_reduce(all[i], all[j]);
                ASSERT_TRUE((got & ~all[i]) == 0u);
            }
    }
}
