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
 *   [PT-9]  a level enters a walk EMPTY, whatever it held before — the
 *           invariant that makes reusing a released table safe at all.
 *   [PT-10] teardown takes the holder's levels back out of the walk, exactly
 *           those, idempotently, and never on a stale record.
 *   [PT-11] a bind claim taken for a composition that failed goes back, so the
 *           same address space can be offered to a retry.
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

    /*
     * [PT-9] a level enters a walk EMPTY.
     *
     * Teardown detaches a table from the walk; it does not scrub it, and a PT
     * in particular keeps all 512 of its leaf entries.  A holder that kept the
     * capability therefore holds a page full of a dead address space's PTEs,
     * and nothing binds a table to the level it once filled — so re-installing
     * it as a PDPT would have the MMU read those leaf entries as pointers to
     * page tables and walk into arbitrary physical memory.  Zeroing at INSTALL
     * is what makes the object reusable; this is the assertion that it happens.
     */
    {
        struct KPageTable *dirty = pt_make(ut);
        ASSERT_NOT_NULL(dirty);
        if (dirty) {
            uint64_t *page = (uint64_t *)(uintptr_t)PHYS_TO_VIRT(dirty->paddr);
            for (uint32_t i = 0; i < 512u; i++) page[i] = 0xDEADBEEF00000001ULL;

            struct KVSpace *fresh = kvspace_alloc(cr3 + 0x9000ULL);
            ASSERT_NOT_NULL(fresh);
            if (fresh) {
                kvspace_set_pt_pool(fresh, ut);
                ASSERT_EQ((int)kvspace_map_table(fresh, dirty,
                                                 PT_VA + 0x20000000000ULL),
                          (int)IRIS_OK);
                int clean = 1;
                for (uint32_t i = 0; i < 512u; i++)
                    if (page[i] != 0ULL) { clean = 0; break; }
                ASSERT_TRUE(clean);
                kobject_release(&fresh->base);
            }
            kobject_release(&dirty->base);
        }
    }

    /*
     * [PT-10] the levels the HOLDER supplied leave the walk at teardown.
     *
     * This is what lets paging_destroy_user_space_from free what is left to
     * the PMM without asking where anything came from: after the detach the
     * only thing still reachable is what the kernel itself carved.  The root
     * task is why it has to be per-TABLE and not per-address-space — its PML4
     * is a PMM page while every level it installs after kvspace_end_bootstrap
     * comes from an Untyped, so one answer for the whole walk is wrong for it
     * whichever way it is given.
     */
    {
        uint64_t        dcr3 = cr3 + 0x11000ULL;
        struct KVSpace *dvs  = kvspace_alloc(dcr3);
        ASSERT_NOT_NULL(dvs);
        if (dvs) {
            kvspace_set_pt_pool(dvs, ut);
            struct KPageTable *lv[3];
            uint64_t dva = PT_VA + 0x30000000000ULL;
            for (int i = 0; i < 3; i++) {
                lv[i] = pt_make(ut);
                ASSERT_NOT_NULL(lv[i]);
                if (lv[i])
                    ASSERT_EQ((int)kvspace_map_table(dvs, lv[i], dva), (int)IRIS_OK);
            }
            /* The walk is complete: a fourth table has nothing to fill. */
            ASSERT_EQ(paging_missing_level_in(dcr3, dva), 0);

            kvspace_detach_tables(dvs, dcr3);
            /* ...and now it is empty again, from the top. */
            ASSERT_EQ(paging_missing_level_in(dcr3, dva), 3);

            /* Idempotent: a second pass detaches nothing and says so, which is
             * what makes it safe on a half-torn-down address space. */
            kvspace_detach_tables(dvs, dcr3);
            ASSERT_EQ(paging_missing_level_in(dcr3, dva), 3);

            /* A record that no longer describes the walk detaches nothing —
             * identity is checked, so a stale (va, level) cannot take somebody
             * else's level out. */
            if (lv[0])
                ASSERT_EQ(paging_detach_table_in(dcr3, dva, KPT_LEVEL_PDPT,
                                                 lv[0]->paddr), -1);

            for (int i = 0; i < 3; i++) if (lv[i]) kobject_release(&lv[i]->base);
            kobject_release(&dvs->base);
        }
    }

    /*
     * [PT-11] a claim taken for a composition that did not happen goes back.
     *
     * SYS_PROCESS_CREATE binds the address space several fallible steps before
     * anything owns it.  Without the unwind, a create that failed for want of
     * memory left the caller holding a VSpace that would answer BUSY for the
     * rest of the system's life — recoverable only by destroying it and
     * retyping another, which costs a page of a budget that never rewinds.
     */
    {
        void *hdr = kuntyped_alloc_child_top(ut, sizeof(struct KVSpace));
        ASSERT_NOT_NULL(hdr);
        if (hdr) {
            struct KVSpace *bv = kvspace_alloc_at(hdr, cr3 + 0x21000ULL);
            ASSERT_NOT_NULL(bv);
            if (bv) {
                ASSERT_EQ((int)kvspace_bind(bv), (int)IRIS_OK);
                ASSERT_EQ((int)kvspace_bind(bv), (int)IRIS_ERR_BUSY);
                kvspace_unbind(bv);
                ASSERT_EQ((int)kvspace_bind(bv), (int)IRIS_OK);
                kobject_release(&bv->base);
            }
        }
    }

    paging_stub_strict_levels(0);
    free(mem);
}
