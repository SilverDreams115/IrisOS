/*
 * test_pagetable.c — the page table as a capability (Stage 6-pure).
 *
 * The runtime suite proves this end to end on real hardware (T302), which is
 * the only place the MMU is real.  What the host can do that the runtime
 * cannot is drive the WALK exhaustively: turn on level modelling and every
 * combination of "which levels exist" is reachable and cheap, including the
 * ones a booting system never produces.
 *
 * Covered here:
 *   [PT-1]  a retyped level reports itself unmapped, and knows its page.
 *   [PT-2]  installing fills the deepest missing level, top down, one per call.
 *   [PT-3]  a level is installed at most once — BUSY, not ALREADY_EXISTS,
 *           because a client loop must tell "this object is spent" from "this
 *           level is already there".
 *   [PT-4]  a complete walk answers ALREADY_EXISTS and does not consume the
 *           table, so the caller can install it somewhere else.
 *   [PT-5]  the address is authority: outside the user private window is
 *           refused, so a holder cannot splice a page into the kernel's walk.
 *   [PT-6]  a dead VSpace refuses installs.
 *   [PT-7]  teardown returns every installed level to its Untyped, so the
 *           region becomes reclaimable.
 *   [PT-8]  the bootstrap exception: a kernel-funded address space maps
 *           without being owed anything, and stops being kernel-funded for
 *           good once its holder can speak.
 */

#include "framework.h"
#include <iris/nc/kpagetable.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kobject.h>
#include <iris/paging.h>
#include <stdlib.h>

#define PT_VA   (USER_PRIVATE_BASE + 0x40000000ULL)   /* a fresh 1 GiB window */

static struct KPageTable *pt_make(struct KUntyped *ut) {
    void *hdr = kuntyped_alloc_child_top(ut, sizeof(struct KPageTable));
    if (!hdr) return 0;
    uint64_t phys = kuntyped_bump_alloc_phys_page(ut, 4096u);
    if (!phys) return 0;
    return kpagetable_alloc_at(hdr, phys);
}

void test_pagetable(void) {
    TEST_SUITE("page table as a capability (Stage 6-pure)");
    paging_stub_strict_levels(1);

    void *mem = aligned_alloc(4096u, 512u * 1024u);
    ASSERT_NOT_NULL(mem);
    struct KUntyped *ut =
        kuntyped_create((uint64_t)(uintptr_t)mem, 512u * 1024u, 0);
    ASSERT_NOT_NULL(ut);

    uint64_t cr3 = (uint64_t)(uintptr_t)mem;   /* any non-zero identity */

    /* [PT-1] */
    struct KPageTable *a = pt_make(ut);
    ASSERT_NOT_NULL(a);
    if (a) {
        ASSERT_EQ(a->level, (uint32_t)KPT_LEVEL_UNMAPPED);
        ASSERT_TRUE(a->mapped_vs == NULL);
        ASSERT_EQ(a->paddr & 0xFFFULL, (uint64_t)0);
    }

    struct KVSpace *vs = kvspace_alloc(cr3);
    ASSERT_NOT_NULL(vs);
    kvspace_set_pt_pool(vs, ut);

    /* [PT-5] the address decides which PML4 slot is written, so it is checked
     * BEFORE anything is consumed — the table survives a refused install. */
    if (a && vs) {
        ASSERT_EQ((int)kvspace_map_table(vs, a, 0xFFFF800000000000ULL),
                  (int)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ((int)kvspace_map_table(vs, a, 0x1000ULL),
                  (int)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(a->level, (uint32_t)KPT_LEVEL_UNMAPPED);
    }

    /* [PT-2] top down: PDPT, then PD, then PT — one invocation each. */
    if (a && vs) {
        ASSERT_EQ((int)kvspace_map_table(vs, a, PT_VA), (int)IRIS_OK);
        ASSERT_EQ(a->level, (uint32_t)KPT_LEVEL_PDPT);
        ASSERT_TRUE(a->mapped_vs == vs);
    }
    struct KPageTable *b = pt_make(ut);
    ASSERT_NOT_NULL(b);
    if (b && vs) {
        ASSERT_EQ((int)kvspace_map_table(vs, b, PT_VA), (int)IRIS_OK);
        ASSERT_EQ(b->level, (uint32_t)KPT_LEVEL_PD);
    }
    struct KPageTable *c = pt_make(ut);
    ASSERT_NOT_NULL(c);
    if (c && vs) {
        ASSERT_EQ((int)kvspace_map_table(vs, c, PT_VA), (int)IRIS_OK);
        ASSERT_EQ(c->level, (uint32_t)KPT_LEVEL_PT);
    }

    /* [PT-3] a spent table is BUSY wherever it is offered. */
    if (a && vs)
        ASSERT_EQ((int)kvspace_map_table(vs, a, PT_VA + 0x40000000ULL),
                  (int)IRIS_ERR_BUSY);

    /* [PT-4] a complete walk consumes nothing. */
    struct KPageTable *d = pt_make(ut);
    ASSERT_NOT_NULL(d);
    if (d && vs) {
        ASSERT_EQ((int)kvspace_map_table(vs, d, PT_VA),
                  (int)IRIS_ERR_ALREADY_EXISTS);
        ASSERT_EQ(d->level, (uint32_t)KPT_LEVEL_UNMAPPED);
        ASSERT_TRUE(d->mapped_vs == NULL);
        /* ...so it still installs somewhere the walk is not complete. */
        ASSERT_EQ((int)kvspace_map_table(vs, d, PT_VA + 0x8000000000ULL),
                  (int)IRIS_OK);
    }

    /* [PT-6] a reaped address space installs nothing. */
    struct KPageTable *e = pt_make(ut);
    ASSERT_NOT_NULL(e);
    if (e && vs) {
        kvspace_invalidate(vs);
        ASSERT_EQ((int)kvspace_map_table(vs, e, PT_VA + 0x10000000000ULL),
                  (int)IRIS_ERR_BAD_HANDLE);
    }

    /* [PT-7] teardown hands every installed level back, so the region becomes
     * reclaimable — the guarantee that lets a holder RESET and reuse it.
     *
     * The creation references go first, standing in for the CSpace slots that
     * hold them in a real system: while a holder still names a table, the
     * table is alive whatever the address space does, which is the whole point
     * of it being a capability.  Only once the VSpace's reference is the last
     * one does releasing the VSpace return anything. */
    if (a) kobject_release(&a->base);
    if (b) kobject_release(&b->base);
    if (c) kobject_release(&c->base);
    if (d) kobject_release(&d->base);
    if (vs) {
        uint32_t before = atomic_load(&ut->child_count);
        ASSERT_TRUE(before > 0u);
        kobject_release(&vs->base);          /* last ref: destructor runs */
        uint32_t after = atomic_load(&ut->child_count);
        ASSERT_TRUE(after < before);
    }
    if (e) kobject_release(&e->base);

    /*
     * [PT-8] the bootstrap exception, and the fact that it ends.
     *
     * The root task's text, stack and BootInfo are mapped before it exists, so
     * there is nobody to ask for the levels and the kernel supplies them.
     * That is the ONLY address space this is ever true of, and only until it
     * can speak for itself — which is what kvspace_end_bootstrap says.  A test
     * for it is worth having because the flag is invisible from userland: a
     * kernel that silently kept funding the root task forever would look
     * exactly like one that stopped.
     */
    {
        struct KVSpace *boot = kvspace_alloc(cr3 + 0x1000ULL);
        ASSERT_NOT_NULL(boot);
        if (boot) {
            /* kvspace_alloc is the root task's constructor: kernel-funded. */
            ASSERT_EQ((int)boot->kernel_funded, 1);
            /* ...and kvspace_alloc_at, every other address space, is not. */
            void *hdr = kuntyped_alloc_child_top(ut, sizeof(struct KVSpace));
            ASSERT_NOT_NULL(hdr);
            if (hdr) {
                struct KVSpace *spawned = kvspace_alloc_at(hdr, cr3 + 0x2000ULL);
                ASSERT_NOT_NULL(spawned);
                if (spawned) {
                    ASSERT_EQ((int)spawned->kernel_funded, 0);
                    kobject_release(&spawned->base);
                }
            }
            kvspace_end_bootstrap(boot);
            ASSERT_EQ((int)boot->kernel_funded, 0);
            kobject_release(&boot->base);
        }
    }

    paging_stub_strict_levels(0);
    free(mem);
}
