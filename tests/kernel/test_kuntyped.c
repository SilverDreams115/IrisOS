#include "framework.h"
#include <iris/nc/kuntyped.h>
#include <stdlib.h>

void test_kuntyped(void) {
    TEST_SUITE("kuntyped");

    /* ── create / initial state ── */
    const uint64_t BUF_SZ = 1024u;
    void *buf = malloc(BUF_SZ);
    ASSERT_NOT_NULL(buf);
    uint64_t phys = (uint64_t)(uintptr_t)buf;

    struct KUntyped *u = kuntyped_create(phys, BUF_SZ, 0);
    ASSERT_NOT_NULL(u);
    ASSERT_EQ(u->total_size, BUF_SZ);
    ASSERT_EQ(u->used, 0u);
    ASSERT_EQ(atomic_load(&u->child_count), 0u);
    ASSERT_EQ(kuntyped_available(u), BUF_SZ);

    /* ── bump_alloc: rounds up to KUNTYPED_ALIGN, zero-fills ── */
    /* Pre-fill backing memory so zero-fill is verifiable. */
    for (uint64_t i = 0; i < BUF_SZ; i++) ((uint8_t *)buf)[i] = 0xFF;

    void *p = kuntyped_bump_alloc(u, 1u);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(u->used, KUNTYPED_ALIGN);
    int zeroed = 1;
    for (uint64_t i = 0; i < KUNTYPED_ALIGN; i++)
        if (((uint8_t *)p)[i] != 0) { zeroed = 0; break; }
    ASSERT_TRUE(zeroed);

    /* ── bump_alloc_phys: returns physical address ── */
    uint64_t paddr = kuntyped_bump_alloc_phys(u, 1u);
    ASSERT_NE(paddr, (uint64_t)0);
    ASSERT_EQ(u->used, KUNTYPED_ALIGN * 2u);
    ASSERT_EQ(kuntyped_available(u), BUF_SZ - KUNTYPED_ALIGN * 2u);

    /* ── out of space ── */
    uint64_t remaining = kuntyped_available(u);
    ASSERT_NULL(kuntyped_bump_alloc(u, remaining + 1u));
    ASSERT_EQ(kuntyped_available(u), remaining);

    /* ── alloc_child: increments child_count + retains parent ── */
    const uint64_t BUF2_SZ = 512u;
    void *buf2 = malloc(BUF2_SZ);
    ASSERT_NOT_NULL(buf2);
    struct KUntyped *u2 = kuntyped_create((uint64_t)(uintptr_t)buf2, BUF2_SZ, 0);
    ASSERT_NOT_NULL(u2);

    void *child = kuntyped_alloc_child(u2, 32u);
    ASSERT_NOT_NULL(child);
    ASSERT_EQ(atomic_load(&u2->child_count), 1u);
    ASSERT_EQ(atomic_load(&u2->base.refcount), 2u);  /* owner + child retain */

    /* ── release_child: decrements child_count + releases parent ref ── */
    kuntyped_release_child(child, 32u);
    ASSERT_EQ(atomic_load(&u2->child_count), 0u);
    ASSERT_EQ(atomic_load(&u2->base.refcount), 1u);

    /* ── cleanup ── */
    kuntyped_destroy_ref(u);
    kuntyped_destroy_ref(u2);
    free(buf);
    free(buf2);
}
