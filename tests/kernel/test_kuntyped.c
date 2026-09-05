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

    /* ── Stage 6 Step 1: the two-ended carve (UT-TOP-1..5) ───────────────
     *
     * Object headers are carved from the TOP of an Untyped so that paying for
     * a header does not push the next page-aligned carve onto the following
     * page.  What must hold: the two ends never overlap, a top carve does not
     * move the bottom one, `available` reports what is left BETWEEN them, and
     * a header block released from the top is accounted exactly like one
     * released from the bottom.
     *
     * The page-alignment property is the reason the second pointer exists, so
     * it is asserted directly rather than inferred from the sizes. */
    {
        const uint64_t B = 64u * 1024u;          /* 16 pages */
        void *raw = aligned_alloc(4096u, B);
        ASSERT_NOT_NULL(raw);
        struct KUntyped *ut = kuntyped_create((uint64_t)(uintptr_t)raw, B, 0);
        ASSERT_NOT_NULL(ut);

        /* [UT-TOP-1] a top carve grows used_top, not used, and is inside the
         * top of the region. */
        void *h1 = kuntyped_alloc_child_top(ut, 96u);
        ASSERT_NOT_NULL(h1);
        ASSERT_EQ(ut->used, 0u);
        ASSERT_TRUE(ut->used_top >= 96u + KUNTYPED_ALIGN);
        ASSERT_TRUE((uint64_t)(uintptr_t)h1 >=
                    (uint64_t)(uintptr_t)raw + B - ut->used_top);
        ASSERT_TRUE((uint64_t)(uintptr_t)h1 < (uint64_t)(uintptr_t)raw + B);
        ASSERT_EQ(atomic_load(&ut->child_count), 1u);
        ASSERT_EQ(kuntyped_available(ut), B - ut->used_top);

        /* [UT-TOP-2] the page carve is unmoved by the header: it still starts
         * at offset 0.  This is the whole point of the second pointer — from
         * the bottom, that 160-byte header would have cost a full page. */
        uint64_t pg = kuntyped_bump_alloc_phys_page(ut, 4096u);
        ASSERT_EQ(pg, (uint64_t)(uintptr_t)raw);
        ASSERT_EQ(ut->used, 4096u);

        /* [UT-TOP-3] header and page do not overlap, in either direction. */
        ASSERT_TRUE((uint64_t)(uintptr_t)h1 >= pg + 4096u);

        /* [UT-TOP-4] the ends meet exactly once: fill the bottom to the last
         * page the top leaves free, then one more of either must fail. */
        uint64_t free_bytes = kuntyped_available(ut);
        uint64_t whole_pages = free_bytes & ~0xFFFULL;
        if (whole_pages) {
            uint64_t p2 = kuntyped_bump_alloc_phys_page(ut, whole_pages);
            ASSERT_NE(p2, (uint64_t)0);
        }
        ASSERT_TRUE(kuntyped_available(ut) < 4096u);
        ASSERT_EQ(kuntyped_bump_alloc_phys_page(ut, 4096u), (uint64_t)0);
        ASSERT_NULL(kuntyped_alloc_child_top(ut, 4096u));
        /* A refused carve moves neither pointer. */
        uint64_t used_before = ut->used, top_before = ut->used_top;
        ASSERT_NULL(kuntyped_alloc_child_top(ut, B));
        ASSERT_EQ(ut->used, used_before);
        ASSERT_EQ(ut->used_top, top_before);

        /* [UT-TOP-5] release from the top is accounted like any other child. */
        uint32_t before = atomic_load(&ut->child_count);
        kuntyped_release_child(h1, 96u);
        ASSERT_EQ(atomic_load(&ut->child_count), before - 1u);

        kuntyped_destroy_ref(ut);
        free(raw);
    }

    /* ── cleanup ── */
    kuntyped_destroy_ref(u);
    kuntyped_destroy_ref(u2);
    free(buf);
    free(buf2);

    /*
     * ── HB-1..HB-5: the DEVICE header budget (ledger D-9) ────────────────
     *
     * A device Untyped describes MMIO, and MMIO is not storage: a struct
     * written into a framebuffer is pixels, and read back it is whatever the
     * display controller left there.  So the headers of objects retyped out of
     * a device region have to be RAM somebody named — the kernel taking them
     * from its own slab is charter M3's exact prohibition.
     *
     * The pairing carries two properties that both come from one retain, and
     * these assert the rules that make them hold.
     */
    {
        void *rambuf = malloc(4096u);
        ASSERT_NOT_NULL(rambuf);
        struct KUntyped *ram = kuntyped_create((uint64_t)(uintptr_t)rambuf,
                                               4096u, 0);
        struct KUntyped *ram2 = kuntyped_create((uint64_t)(uintptr_t)rambuf,
                                                4096u, 0);
        struct KUntyped *dev = kuntyped_create(0xFD000000ull, 65536u, 1);
        struct KUntyped *dev2 = kuntyped_create(0xFE000000ull, 65536u, 1);
        ASSERT_NOT_NULL(ram); ASSERT_NOT_NULL(ram2);
        ASSERT_NOT_NULL(dev); ASSERT_NOT_NULL(dev2);

        /* HB-1: a fresh device Untyped has no budget, which is what makes a
         * retype out of it refuse rather than fall back to kernel memory. */
        ASSERT_NULL(dev->hdr_budget);

        /* HB-2: a RAM Untyped is not something to pair — it carves its own. */
        ASSERT_EQ(kuntyped_set_hdr_budget(ram, ram2),
                  (long)IRIS_ERR_INVALID_ARG);
        /* HB-3: nor is a DEVICE region a budget: it cannot hold headers, which
         * is the entire reason the pairing exists. */
        ASSERT_EQ(kuntyped_set_hdr_budget(dev, dev2),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(kuntyped_set_hdr_budget(dev, dev),
                  (long)IRIS_ERR_INVALID_ARG);

        /* HB-4: a valid pairing takes, and RETAINS — the retain is what makes
         * the budget's own RESET refuse while headers carved from it are
         * alive, without any bookkeeping to keep in step. */
        uint32_t before = atomic_load(&ram->base.refcount);
        ASSERT_EQ(kuntyped_set_hdr_budget(dev, ram), 0L);
        ASSERT_TRUE(dev->hdr_budget == ram);
        ASSERT_EQ(atomic_load(&ram->base.refcount), before + 1u);

        /* HB-5: SET ONCE.  A pairing that could move would let a holder point
         * at a second budget and reset the first, reclaiming a region while
         * the headers describing live device frames were still in it. */
        ASSERT_EQ(kuntyped_set_hdr_budget(dev, ram2),
                  (long)IRIS_ERR_ALREADY_EXISTS);
        ASSERT_TRUE(dev->hdr_budget == ram);
    }
}
