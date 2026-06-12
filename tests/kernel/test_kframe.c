/*
 * test_kframe.c — Fase 5 / 5.1 unit tests for KFrame capability model.
 *
 * Tests (FR-1..FR-22): Fase 5 — alloc/destroy/CSpace resolution.
 *   [FR-1]  KOBJ_FRAME enum exists, is non-zero, and distinct from all others.
 *   [FR-2]  kframe_alloc returns non-NULL for valid paddr/size/parent.
 *   [FR-3]  kframe_alloc sets paddr, size, alloc_parent correctly.
 *   [FR-4]  kframe_alloc increments parent child_count and retains parent ref.
 *   [FR-5]  kframe_alloc returns NULL for size=0.
 *   [FR-6]  kframe_alloc returns NULL for unaligned size (size not multiple of 4096).
 *   [FR-7]  kframe_alloc with NULL parent succeeds; alloc_parent is NULL.
 *   [FR-8]  kobject_release on KFrame triggers destroy; child_count decremented.
 *   [FR-9]  After frame destroy, parent child_count is back to 0 (reset unblocked).
 *   [FR-10] cspace_resolve_frame succeeds when slot holds a KOBJ_FRAME.
 *   [FR-11] cspace_resolve_frame returns IRIS_ERR_WRONG_TYPE for a non-frame slot.
 *   [FR-12] cspace_resolve_frame returns IRIS_ERR_ACCESS_DENIED with insufficient rights.
 *   [FR-13] cspace_resolve_cap with CPTR_NULL returns IRIS_ERR_INVALID_ARG.
 *   [FR-14] cspace_or_handle_resolve_frame ACCESS_DENIED hard-stops; no handle fallback.
 *   [FR-15] cspace_or_handle_resolve_frame falls back to handle table if CSpace is absent.
 *   [FR-16] kuntyped_bump_alloc_phys_page returns page-aligned address.
 *   [FR-17] kuntyped_bump_alloc_phys_page fails when insufficient space.
 *   [FR-18] kuntyped_bump_alloc_phys_page fails for unaligned size.
 *   [FR-19] kframe_va_valid accepts a valid page-aligned user address.
 *   [FR-20] kframe_va_valid rejects unaligned address.
 *   [FR-21] kframe_va_valid rejects address below USER_PRIVATE_BASE.
 *   [FR-22] kframe_va_valid rejects address >= USER_SPACE_TOP.
 *
 * Tests (FR-23..FR-40): Fase 5.1 — paging stub integrity + mapping lifecycle.
 *   [FR-23] paging stub: map is visible via paging_virt_to_phys_in; isolated by cr3.
 *   [FR-24] paging stub: duplicate (cr3,va) returns non-zero (BUSY); original unchanged.
 *   [FR-25] paging stub: paging_unmap_in removes entry; virt_to_phys returns 0 after.
 *   [FR-26] kframe_map_page increments mapped_count on success.
 *   [FR-27] kframe_map_page returns IRIS_ERR_BUSY for duplicate VA; mapped_count unchanged.
 *   [FR-28] kframe_unmap_page decrements mapped_count and clears PTE.
 *   [FR-29] kframe_unmap_page returns IRIS_ERR_NOT_FOUND for unmapped VA.
 *   [FR-30] kframe_unmap_page returns IRIS_ERR_INVALID_ARG if VA maps a different frame.
 *   [FR-31] kframe_map_page returns IRIS_ERR_INVALID_ARG for unaligned VA.
 *   [FR-32] kframe_map_page returns IRIS_ERR_BAD_HANDLE for invalidated VSpace.
 *   [FR-33] kvspace_invalidate auto-unmaps KFrame mappings; mapped_count==0 after (Fase 6).
 *   [FR-34] W^X enforcement: WRITABLE+EXEC simultaneously rejected by kframe_map_page.
 *   [FR-35] After map+unmap, mapped_count==0; parent child_count unaffected by map/unmap.
 *   [FR-36] kframe_alloc initialises mapped_count to 0.
 *   [FR-37] Multiple sequential map+unmap cycles leave mapped_count==0.
 *
 * Tests (FR-38..FR-40): Fase 6 / 6.3 — KVSpace dynamic mapping model.
 *   [FR-38] kframe_map_page registers a back-ref node; kframe_unmap_page clears it.
 *   [FR-39] kvspace_invalidate clears all mapping records and auto-unmaps every PTE.
 *   [FR-40] Dynamic pool supports 64 pages (>32 old fixed limit); kvspace_invalidate
 *           cleans all.
 *
 * Tests (FR-41): Fase 6.1 — demand paging removed regression.
 *   [FR-41] No PTE installed for an unmapped VA: paging_virt_to_phys_in returns 0 and
 *           no frame has mapped_count > 0 after VSpace ops without an explicit map call.
 *
 * Tests (FR-42..FR-50): Fase 6.2 — bootstrap_kframe_map / bootstrap Frame-backed maps.
 *   [FR-42] bootstrap_kframe_map returns non-NULL for valid vs/paddr/va/flags.
 *   [FR-43] bootstrap_kframe_map installs a PTE; paging_virt_to_phys_in returns paddr.
 *   [FR-44] bootstrap_kframe_map increments KVSpace mapping_count by 1.
 *   [FR-45] bootstrap_kframe_map increments KFrame mapped_count to 1.
 *   [FR-46] bootstrap_kframe_map returns NULL for NULL vs.
 *   [FR-47] bootstrap_kframe_map returns NULL for duplicate VA (BUSY from kframe_map_page).
 *   [FR-48] After kvspace_invalidate, bootstrap KFrame has mapped_count==0 and
 *           mapping_count==0; no stale PTE remains.
 *   [FR-49] kprocess_register_bootstrap_frame rejects NULL proc or NULL frame.
 *   [FR-50] kprocess_register_bootstrap_frame enforces the 32-slot limit;
 *           kprocess_release_bootstrap_frames drops all retains and resets count.
 *
 * Tests (FR-51..FR-62): Fase 6.3 — VMO-to-Frame capability migration.
 *   [FR-51] kframe_alloc_vmo_page sets vmo_owner; kframe_alloc sets vmo_owner=NULL.
 *   [FR-52] kframe_alloc_vmo_page returns NULL for NULL vmo.
 *   [FR-53] kframe_alloc_vmo_page retains VMO refcount; releasing last frame restores it.
 *   [FR-54] kvspace_unmap_page removes PTE, decrements mapping_count, releases frame.
 *   [FR-55] kvspace_unmap_page returns IRIS_ERR_NOT_FOUND for unmapped VA.
 *   [FR-56] kvspace_unmap_page returns IRIS_ERR_BAD_HANDLE for invalidated VSpace.
 *   [FR-57] VMO retain released only after all KFrame mappings removed (destroy order).
 *   [FR-58] Same VMO mapped in two VSpaces; invalidating one does not affect the other.
 *   [FR-59] kvspace_unmap_page handles multiple distinct VAs in the same VSpace.
 *   [FR-60] kframe_alloc_vmo_page followed by kframe_map_page tracks in vs->mappings.
 *   [FR-61] kframe_map_page with flags > 3 returns IRIS_ERR_INVALID_ARG.
 *   [FR-62] Dynamic pool can hold far more than 32 entries without failure.
 *
 * Tests (FR-63..FR-69): Fase 6.4 — Memory stress / fuzz / invariant audit.
 *   [FR-63] kframe_unmap_page safely handles the case where the mapping retain is the
 *           last retain on the frame (alloc retain already released).  mapped_count
 *           must be decremented BEFORE kobject_release so kframe_obj_destroy always
 *           sees mapped_count == 0.  Tests the ordering bug fixed in Fase 6.4.
 *   [FR-64] kslab_alloc failure in kframe_map_page → IRIS_ERR_NO_MEMORY; no PTE
 *           installed; mapping_count and mapped_count unchanged.
 *   [FR-65] paging_map_checked_in failure in kframe_map_page (after kslab_alloc
 *           succeeds) → IRIS_ERR_NO_MEMORY; KFrameMapping node freed; no PTE
 *           installed; mapping_count and mapped_count unchanged.
 *   [FR-66] Multi-page rollback simulation: map N pages, then unmap pages 0..k-1 via
 *           kvspace_unmap_page (mirrors rollback_vmo_maps); verify no stale PTEs,
 *           mapping_count == N-k, mapped_counts all correct.
 *   [FR-67] kvspace_obj_destroy safety net: directly destroy a VSpace with active
 *           mappings (no prior kvspace_invalidate).  Safety net must release all
 *           mapping nodes, decrement mapped_count, and release frame retains without
 *           panic even when mapping retain is the last frame retain.
 *   [FR-68] Map/unmap 1000-cycle stress on a single VA: mapping_count == 0 and
 *           mapped_count == 0 after every unmap; no PTE remains; no accumulation.
 *   [FR-69] mapping_count consistency with interleaved map/unmap: count always equals
 *           the number of live mappings in the list after every operation.
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
#include <iris/nc/kvmo.h>
#include <iris/paging.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

/* Declared in stubs.c — minimal KVmo with no PMM dependency, for unit tests. */
struct KVmo *kvmo_make_stub(void);

/* ── Helpers ─────────────────────────────────────────────────────────── */

static struct KProcess *fr_make_proc(void) {
    struct KProcess *p = (struct KProcess *)malloc(sizeof(struct KProcess));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    handle_table_init(&p->handle_table);
    p->cspace_root_h = HANDLE_INVALID;
    return p;
}

static void fr_free_proc(struct KProcess *p) {
    handle_table_close_all(&p->handle_table);
    free(p);
}

static struct KCNode *fr_setup_root(struct KProcess *p) {
    struct KCNode *root = kcnode_alloc(KCNODE_DEFAULT_SLOTS);
    if (!root) return NULL;
    handle_id_t rh = handle_table_insert(
        &p->handle_table, &root->base,
        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    kobject_release(&root->base);
    if (rh == HANDLE_INVALID) return NULL;
    p->cspace_root_h = rh;
    return root;
}

static struct KUntyped *fr_make_untyped(uint64_t phys, uint64_t size) {
    /* 4-KiB minimum so the bump allocator can carve at least one page. */
    struct KUntyped *u = kuntyped_create(phys, size, 0);
    return u;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

void test_kframe(void) {
    TEST_SUITE("FR: KFrame capability model (Fase 5)");

    /* FR-1: KOBJ_FRAME enum */
    ASSERT_NE((int)KOBJ_FRAME, 0);
    ASSERT_NE((int)KOBJ_FRAME, (int)KOBJ_UNTYPED);
    ASSERT_NE((int)KOBJ_FRAME, (int)KOBJ_VSPACE);
    ASSERT_NE((int)KOBJ_FRAME, (int)KOBJ_CNODE);
    ASSERT_NE((int)KOBJ_FRAME, (int)KOBJ_ENDPOINT);

    /* FR-2/FR-3: kframe_alloc basic allocation */
    {
        struct KUntyped *u = fr_make_untyped(0x100000, 0x8000);
        ASSERT_NOT_NULL(u);
        struct KFrame *f = kframe_alloc(0x200000, 4096, u);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ(f->paddr, (uint64_t)0x200000);
        ASSERT_EQ(f->size,  (uint64_t)4096);
        ASSERT_EQ(f->alloc_parent, u);
        ASSERT_EQ(f->base.type, KOBJ_FRAME);
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kobject_release(&u->base);
    }

    /* FR-4: kframe_alloc increments parent child_count and retains parent */
    {
        struct KUntyped *u = fr_make_untyped(0x100000, 0x8000);
        ASSERT_NOT_NULL(u);
        uint32_t rc_before  = atomic_load(&u->base.refcount);
        uint32_t cc_before  = atomic_load(&u->child_count);
        struct KFrame *f = kframe_alloc(0x200000, 4096, u);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ((int)atomic_load(&u->child_count), (int)(cc_before + 1));
        ASSERT_EQ((int)atomic_load(&u->base.refcount), (int)(rc_before + 1));
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kobject_release(&u->base);
    }

    /* FR-5: kframe_alloc with size=0 returns NULL */
    {
        struct KUntyped *u = fr_make_untyped(0x100000, 0x8000);
        ASSERT_NOT_NULL(u);
        struct KFrame *f = kframe_alloc(0x200000, 0, u);
        ASSERT_NULL(f);
        kobject_release(&u->base);
    }

    /* FR-6: kframe_alloc with unaligned size returns NULL */
    {
        struct KUntyped *u = fr_make_untyped(0x100000, 0x8000);
        ASSERT_NOT_NULL(u);
        struct KFrame *f = kframe_alloc(0x200000, 100, u);
        ASSERT_NULL(f);
        kobject_release(&u->base);
    }

    /* FR-7: kframe_alloc with NULL parent */
    {
        struct KFrame *f = kframe_alloc(0x200000, 4096, NULL);
        ASSERT_NOT_NULL(f);
        ASSERT_NULL(f->alloc_parent);
        kobject_active_release(&f->base);
        kobject_release(&f->base);
    }

    /* FR-8/FR-9: destroy decrements child_count */
    {
        struct KUntyped *u = fr_make_untyped(0x100000, 0x8000);
        ASSERT_NOT_NULL(u);
        struct KFrame *f = kframe_alloc(0x300000, 4096, u);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ((int)atomic_load(&u->child_count), 1);
        kobject_active_release(&f->base);
        kobject_release(&f->base); /* triggers destroy */
        ASSERT_EQ((int)atomic_load(&u->child_count), 0);
        kobject_release(&u->base);
    }

    /* FR-10: cspace_resolve_frame succeeds with correct type */
    {
        struct KProcess *p = fr_make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = fr_setup_root(p);
        ASSERT_NOT_NULL(root);
        struct KFrame *f = kframe_alloc(0x400000, 4096, NULL);
        ASSERT_NOT_NULL(f);
        iris_rights_t all = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        /* Insert into slot 3 (first unreserved slot after null/bootstrap/vspace). */
        iris_error_t ie = kcnode_mint(root, 3u, &f->base, all);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        kobject_release(&f->base);

        struct KFrame   *out;
        iris_rights_t    r;
        ie = cspace_resolve_frame(p, 3u, RIGHT_READ, &out, &r);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        ASSERT_EQ(out->paddr, (uint64_t)0x400000);
        kobject_active_release(&out->base);
        kobject_release(&out->base);
        fr_free_proc(p);
    }

    /* FR-11: cspace_resolve_frame returns WRONG_TYPE for non-frame slot */
    {
        struct KProcess *p = fr_make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = fr_setup_root(p);
        ASSERT_NOT_NULL(root);
        struct KUntyped *u = fr_make_untyped(0x100000, 0x8000);
        ASSERT_NOT_NULL(u);
        iris_rights_t all = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        iris_error_t ie = kcnode_mint(root, 3u, &u->base, all);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        kobject_release(&u->base);

        struct KFrame *out;
        iris_rights_t  r;
        ie = cspace_resolve_frame(p, 3u, RIGHT_READ, &out, &r);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_WRONG_TYPE);
        fr_free_proc(p);
    }

    /* FR-12: cspace_resolve_frame returns ACCESS_DENIED with insufficient rights */
    {
        struct KProcess *p = fr_make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = fr_setup_root(p);
        ASSERT_NOT_NULL(root);
        struct KFrame *f = kframe_alloc(0x400000, 4096, NULL);
        ASSERT_NOT_NULL(f);
        /* Insert with READ-only rights. */
        iris_error_t ie = kcnode_mint(root, 3u, &f->base, RIGHT_READ);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        kobject_release(&f->base);

        struct KFrame *out;
        iris_rights_t  r;
        ie = cspace_resolve_frame(p, 3u, RIGHT_WRITE, &out, &r);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_ACCESS_DENIED);
        fr_free_proc(p);
    }

    /* FR-13: CPTR_NULL returns INVALID_ARG */
    {
        struct KProcess *p = fr_make_proc();
        ASSERT_NOT_NULL(p);
        fr_setup_root(p);
        struct KFrame *out;
        iris_rights_t  r;
        iris_error_t ie = cspace_resolve_frame(p, CPTR_NULL, RIGHT_READ, &out, &r);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_INVALID_ARG);
        fr_free_proc(p);
    }

    /* FR-14: ACCESS_DENIED hard-stops; handle-table fallback not attempted */
    {
        struct KProcess *p = fr_make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = fr_setup_root(p);
        ASSERT_NOT_NULL(root);
        struct KFrame *f = kframe_alloc(0x400000, 4096, NULL);
        ASSERT_NOT_NULL(f);
        /* Insert with READ-only rights into CSpace. */
        iris_error_t ie = kcnode_mint(root, 3u, &f->base, RIGHT_READ);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        /* Insert the same frame into handle table with WRITE. */
        handle_id_t hid = handle_table_insert(
            &p->handle_table, &f->base,
            RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
        ASSERT_NE((int)hid, (int)HANDLE_INVALID);
        kobject_release(&f->base);

        struct KFrame *out;
        iris_rights_t  r;
        /* Resolve via cptr 3 — CSpace returns ACCESS_DENIED; must NOT fall back. */
        ie = cspace_or_handle_resolve_frame(p, 3u, RIGHT_WRITE, &out, &r);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_ACCESS_DENIED);
        fr_free_proc(p);
    }

    /* FR-15: fallback to handle table when CSpace absent */
    {
        struct KProcess *p = fr_make_proc(); /* no CSpace root */
        ASSERT_NOT_NULL(p);
        struct KFrame *f = kframe_alloc(0x500000, 4096, NULL);
        ASSERT_NOT_NULL(f);
        iris_rights_t all = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        handle_id_t hid = handle_table_insert(&p->handle_table, &f->base, all);
        ASSERT_NE((int)hid, (int)HANDLE_INVALID);
        kobject_release(&f->base);

        struct KFrame *out;
        iris_rights_t  r;
        iris_error_t ie = cspace_or_handle_resolve_frame(
            p, (iris_cptr_t)hid, RIGHT_READ, &out, &r);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        ASSERT_EQ(out->paddr, (uint64_t)0x500000);
        kobject_active_release(&out->base);
        kobject_release(&out->base);
        fr_free_proc(p);
    }

    /* FR-16: kuntyped_bump_alloc_phys_page returns page-aligned address */
    {
        /* Use 2 pages so bump has room after first alignment step. */
        struct KUntyped *u = fr_make_untyped(0x200000, 0x8000);
        ASSERT_NOT_NULL(u);
        uint64_t phys = kuntyped_bump_alloc_phys_page(u, 4096);
        ASSERT_NE(phys, (uint64_t)0);
        ASSERT_EQ(phys & 0xFFFULL, (uint64_t)0); /* page-aligned */
        ASSERT_TRUE(phys >= 0x200000 && phys < 0x208000);
        kobject_release(&u->base);
    }

    /* FR-17: kuntyped_bump_alloc_phys_page fails when not enough space */
    {
        struct KUntyped *u = fr_make_untyped(0x100000, 100);
        ASSERT_NOT_NULL(u);
        uint64_t phys = kuntyped_bump_alloc_phys_page(u, 4096); /* 4 KiB > 100 bytes */
        ASSERT_EQ(phys, (uint64_t)0);
        kobject_release(&u->base);
    }

    /* FR-18: kuntyped_bump_alloc_phys_page fails for unaligned size */
    {
        struct KUntyped *u = fr_make_untyped(0x100000, 0x8000);
        ASSERT_NOT_NULL(u);
        uint64_t phys = kuntyped_bump_alloc_phys_page(u, 100); /* not page-aligned */
        ASSERT_EQ(phys, (uint64_t)0);
        kobject_release(&u->base);
    }

    /* FR-19: kframe_va_valid accepts a valid page-aligned user address */
    {
        /* USER_PRIVATE_BASE = 0x0000008000000000 */
        uint64_t va = USER_PRIVATE_BASE;
        ASSERT_TRUE(kframe_va_valid(va));
        va = USER_PRIVATE_BASE + 0x1000;
        ASSERT_TRUE(kframe_va_valid(va));
    }

    /* FR-20: kframe_va_valid rejects unaligned address */
    {
        uint64_t va = USER_PRIVATE_BASE + 1;
        ASSERT_TRUE(!kframe_va_valid(va));
    }

    /* FR-21: kframe_va_valid rejects address below USER_PRIVATE_BASE */
    {
        uint64_t va = USER_PRIVATE_BASE - 0x1000;
        ASSERT_TRUE(!kframe_va_valid(va));
        ASSERT_TRUE(!kframe_va_valid(0x1000));
        ASSERT_TRUE(!kframe_va_valid(0));
    }

    /* FR-22: kframe_va_valid rejects address >= USER_SPACE_TOP */
    {
        ASSERT_TRUE(!kframe_va_valid(USER_SPACE_TOP));
        ASSERT_TRUE(!kframe_va_valid(USER_SPACE_TOP + 0x1000));
        /* Kernel address (bits 63:47 set) */
        ASSERT_TRUE(!kframe_va_valid(0xFFFF800000000000ULL));
    }

    /* ── Fase 5.1: paging stub integrity ─────────────────────────────── */

    /* FR-23: paging stub tracks real state; result isolated by cr3.
     * Verifies that paging_virt_to_phys_in now returns the recorded phys
     * instead of always returning 0 (the old false-positive stub behaviour). */
    {
        paging_stub_reset();
        uint64_t cr3   = 0xCAFE000ULL;
        uint64_t va    = USER_PRIVATE_BASE;
        uint64_t phys  = 0x100000ULL;
        uint64_t flags = PAGE_PRESENT | PAGE_USER | PAGE_NX;
        ASSERT_EQ(paging_map_checked_in(cr3, va, phys, flags), 0);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, va), phys);
        /* Different cr3 must not see the mapping. */
        ASSERT_EQ(paging_virt_to_phys_in(0xDEAD000ULL, va), (uint64_t)0);
        paging_stub_reset();
    }

    /* FR-24: duplicate (cr3, va) is rejected; original phys is preserved. */
    {
        paging_stub_reset();
        uint64_t cr3  = 0xCAFE001ULL;
        uint64_t va   = USER_PRIVATE_BASE;
        uint64_t flags = PAGE_PRESENT | PAGE_USER | PAGE_NX;
        ASSERT_EQ(paging_map_checked_in(cr3, va, 0x100000ULL, flags), 0);
        /* Second map to the same (cr3, va) must fail. */
        ASSERT_NE(paging_map_checked_in(cr3, va, 0x200000ULL, flags), 0);
        /* Original mapping must be unchanged. */
        ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0x100000ULL);
        paging_stub_reset();
    }

    /* FR-25: paging_unmap_in removes the entry; virt_to_phys returns 0 after.
     * Previously the stub had no paging_unmap_in at all, so unmap could
     * never be tested in host-side unit tests. */
    {
        paging_stub_reset();
        uint64_t cr3  = 0xCAFE002ULL;
        uint64_t va   = USER_PRIVATE_BASE;
        uint64_t flags = PAGE_PRESENT | PAGE_USER | PAGE_NX;
        paging_map_checked_in(cr3, va, 0x100000ULL, flags);
        ASSERT_NE(paging_virt_to_phys_in(cr3, va), (uint64_t)0);
        paging_unmap_in(cr3, va);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0);
        /* Can re-map the same VA after unmap. */
        ASSERT_EQ(paging_map_checked_in(cr3, va, 0x200000ULL, flags), 0);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0x200000ULL);
        paging_stub_reset();
    }

    /* ── Fase 5.1: kframe_map_page / kframe_unmap_page lifecycle ─────── */

    /* FR-26: kframe_map_page increments mapped_count on success. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF000ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x300000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);

        iris_error_t ie = kframe_map_page(f, vs, USER_PRIVATE_BASE, 0u /* read-only, NX */);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);
        /* PTE must be visible in the stub. */
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF000ULL, USER_PRIVATE_BASE), (uint64_t)0x300000ULL);

        kframe_unmap_page(f, vs, USER_PRIVATE_BASE);
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-27: kframe_map_page returns IRIS_ERR_BUSY for duplicate VA;
     * the failing frame's mapped_count must not be incremented. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF001ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f1 = kframe_alloc(0x400000ULL, 4096, NULL);
        struct KFrame *f2 = kframe_alloc(0x500000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f1);
        ASSERT_NOT_NULL(f2);

        ASSERT_EQ((int)kframe_map_page(f1, vs, USER_PRIVATE_BASE, 0u), (int)IRIS_OK);
        /* Second map to same VA — must fail BUSY. */
        ASSERT_EQ((int)kframe_map_page(f2, vs, USER_PRIVATE_BASE, 0u), (int)IRIS_ERR_BUSY);
        /* f2 mapped_count must remain 0 (failed map must not count). */
        ASSERT_EQ((int)atomic_load(&f2->mapped_count), 0);
        /* f1 mapped_count must remain 1. */
        ASSERT_EQ((int)atomic_load(&f1->mapped_count), 1);
        /* Original PTE unchanged: still maps f1's phys. */
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF001ULL, USER_PRIVATE_BASE), (uint64_t)0x400000ULL);

        kframe_unmap_page(f1, vs, USER_PRIVATE_BASE);
        kobject_active_release(&f1->base);
        kobject_release(&f1->base);
        kobject_active_release(&f2->base);
        kobject_release(&f2->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-28: kframe_unmap_page decrements mapped_count and clears PTE. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF002ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x600000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        kframe_map_page(f, vs, USER_PRIVATE_BASE, 0u);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);
        ASSERT_NE(paging_virt_to_phys_in(0xBEEF002ULL, USER_PRIVATE_BASE), (uint64_t)0);

        iris_error_t ie = kframe_unmap_page(f, vs, USER_PRIVATE_BASE);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF002ULL, USER_PRIVATE_BASE), (uint64_t)0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-29: kframe_unmap_page returns IRIS_ERR_NOT_FOUND for unmapped VA;
     * mapped_count must not change. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF003ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x700000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        iris_error_t ie = kframe_unmap_page(f, vs, USER_PRIVATE_BASE);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_NOT_FOUND);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-30: kframe_unmap_page returns IRIS_ERR_INVALID_ARG if VA maps a
     * different frame's physical page; the original mapping is untouched. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF004ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f1 = kframe_alloc(0x800000ULL, 4096, NULL);
        struct KFrame *f2 = kframe_alloc(0x900000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f1);
        ASSERT_NOT_NULL(f2);

        kframe_map_page(f1, vs, USER_PRIVATE_BASE, 0u);
        /* Attempt to unmap via f2 which has a different paddr. */
        iris_error_t ie = kframe_unmap_page(f2, vs, USER_PRIVATE_BASE);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_INVALID_ARG);
        /* f1 still mapped; mapped_count unchanged. */
        ASSERT_EQ((int)atomic_load(&f1->mapped_count), 1);
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF004ULL, USER_PRIVATE_BASE), (uint64_t)0x800000ULL);

        kframe_unmap_page(f1, vs, USER_PRIVATE_BASE);
        kobject_active_release(&f1->base);
        kobject_release(&f1->base);
        kobject_active_release(&f2->base);
        kobject_release(&f2->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-31: kframe_map_page returns IRIS_ERR_INVALID_ARG for unaligned VA. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF005ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0xA00000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        iris_error_t ie = kframe_map_page(f, vs, USER_PRIVATE_BASE + 1u, 0u);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF005ULL, USER_PRIVATE_BASE + 1u), (uint64_t)0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-32: kframe_map_page returns IRIS_ERR_BAD_HANDLE for invalidated VSpace. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF006ULL);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base); /* extra ref so object survives kvspace_free */
        kvspace_invalidate(vs);
        struct KFrame *f = kframe_alloc(0xB00000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        iris_error_t ie = kframe_map_page(f, vs, USER_PRIVATE_BASE, 0u);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_BAD_HANDLE);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kobject_release(&vs->base); /* drop extra ref */
        kvspace_free(vs);           /* drop alloc ref → destroy */
        paging_stub_reset();
    }

    /* FR-33: kvspace_invalidate auto-unmaps KFrame mappings (Fase 6).
     * After invalidation: mapped_count == 0, PTE gone, frame destroy succeeds.
     * Subsequent kframe_unmap_page via the dead VSpace returns BAD_HANDLE. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF007ULL);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base); /* keep alive past kvspace_invalidate */
        struct KFrame *f = kframe_alloc(0xC00000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        /* Map while VSpace is valid. */
        ASSERT_EQ((int)kframe_map_page(f, vs, USER_PRIVATE_BASE, 0u), (int)IRIS_OK);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);

        /* kvspace_invalidate now auto-unmaps all KFrame PTEs and releases retains. */
        kvspace_invalidate(vs);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);   /* cleaned by invalidate */
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF007ULL, USER_PRIVATE_BASE), (uint64_t)0);

        /* Unmap via dead VSpace correctly returns BAD_HANDLE. */
        ASSERT_EQ((int)kframe_unmap_page(f, vs, USER_PRIVATE_BASE), (int)IRIS_ERR_BAD_HANDLE);

        /* Frame destroy succeeds: mapped_count == 0. */
        kobject_active_release(&f->base);
        kobject_release(&f->base);  /* triggers destroy; IRIS_ASSERT passes */
        kobject_release(&vs->base); /* extra retain */
        kvspace_free(vs);           /* alloc ref → destroy */
        paging_stub_reset();
    }

    /* FR-34: W^X enforcement — WRITABLE (bit0) + EXEC (bit1) simultaneously
     * must be rejected by kframe_map_page; no PTE installed. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF008ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0xD00000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        iris_error_t ie = kframe_map_page(f, vs, USER_PRIVATE_BASE, 3u /* W|X */);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF008ULL, USER_PRIVATE_BASE), (uint64_t)0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-35: After map+unmap, mapped_count==0; parent child_count is unaffected
     * by mapping operations (map/unmap is orthogonal to KUntyped bookkeeping). */
    {
        paging_stub_reset();
        struct KUntyped *u = fr_make_untyped(0x100000, 0x8000);
        ASSERT_NOT_NULL(u);
        struct KFrame *f = kframe_alloc(0xE00000ULL, 4096, u);
        ASSERT_NOT_NULL(f);
        struct KVSpace *vs = kvspace_alloc(0xBEEF009ULL);
        ASSERT_NOT_NULL(vs);

        ASSERT_EQ((int)atomic_load(&u->child_count), 1);
        kframe_map_page(f, vs, USER_PRIVATE_BASE, 0u);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);
        ASSERT_EQ((int)atomic_load(&u->child_count), 1); /* unaffected */
        kframe_unmap_page(f, vs, USER_PRIVATE_BASE);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ((int)atomic_load(&u->child_count), 1); /* still unaffected */

        kobject_active_release(&f->base);
        kobject_release(&f->base); /* destroy: child_count → 0 */
        ASSERT_EQ((int)atomic_load(&u->child_count), 0);
        kobject_release(&u->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-36: kframe_alloc initialises mapped_count to 0. */
    {
        struct KFrame *f = kframe_alloc(0xF00000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0u);
        kobject_active_release(&f->base);
        kobject_release(&f->base);
    }

    /* FR-37: Multiple sequential map+unmap cycles leave mapped_count==0
     * and re-mapping the same VA succeeds after each unmap. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF00AULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0xFF0000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        for (int i = 0; i < 4; i++) {
            iris_error_t ie = kframe_map_page(f, vs, USER_PRIVATE_BASE, 0u);
            ASSERT_EQ((int)ie, (int)IRIS_OK);
            ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);
            ie = kframe_unmap_page(f, vs, USER_PRIVATE_BASE);
            ASSERT_EQ((int)ie, (int)IRIS_OK);
            ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
            ASSERT_EQ(paging_virt_to_phys_in(0xBEEF00AULL, USER_PRIVATE_BASE), (uint64_t)0);
        }

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* ── Fase 6: KVSpace back-reference model ────────────────────────── */

    /* FR-38: kframe_map_page registers a back-reference slot in KVSpace;
     * kframe_unmap_page clears it.  mapping_count tracks both transitions. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF00BULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x1000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ((int)kframe_map_page(f, vs, USER_PRIVATE_BASE, 0u), (int)IRIS_OK);
        ASSERT_EQ((int)vs->mapping_count, 1);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);

        ASSERT_EQ((int)kframe_unmap_page(f, vs, USER_PRIVATE_BASE), (int)IRIS_OK);
        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-39: kvspace_invalidate clears all mapping records and auto-unmaps
     * every PTE; mapped_count reaches 0 for all frames. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF00CULL);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base); /* keep object alive post-invalidate */
        struct KFrame *f1 = kframe_alloc(0x1100000ULL, 4096, NULL);
        struct KFrame *f2 = kframe_alloc(0x1200000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f1);
        ASSERT_NOT_NULL(f2);
        uint64_t va1 = USER_PRIVATE_BASE;
        uint64_t va2 = USER_PRIVATE_BASE + 0x1000;

        kframe_map_page(f1, vs, va1, 0u);
        kframe_map_page(f2, vs, va2, 0u);
        ASSERT_EQ((int)vs->mapping_count, 2);
        ASSERT_EQ((int)atomic_load(&f1->mapped_count), 1);
        ASSERT_EQ((int)atomic_load(&f2->mapped_count), 1);

        kvspace_invalidate(vs);

        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ((int)atomic_load(&f1->mapped_count), 0);
        ASSERT_EQ((int)atomic_load(&f2->mapped_count), 0);
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF00CULL, va1), (uint64_t)0);
        ASSERT_EQ(paging_virt_to_phys_in(0xBEEF00CULL, va2), (uint64_t)0);

        /* Frames can be destroyed without panic (mapped_count == 0). */
        kobject_active_release(&f1->base); kobject_release(&f1->base);
        kobject_active_release(&f2->base); kobject_release(&f2->base);
        kobject_release(&vs->base); /* extra retain */
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-40: Dynamic mapping pool supports 64 pages (>32 old fixed limit);
     * kvspace_invalidate cleans all 64 nodes and resets mapped_count for each
     * frame to 0.  With the slab-backed linked list there is no compile-time
     * slot ceiling, so all 64 maps must succeed. */
    {
#define FR40_COUNT 64
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xBEEF00DULL);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base); /* keep object alive past kvspace_invalidate */

        struct KFrame *frames[FR40_COUNT];
        int i;
        for (i = 0; i < FR40_COUNT; i++) {
            frames[i] = kframe_alloc(0x2000000ULL + (uint64_t)i * 0x1000, 4096, NULL);
            ASSERT_NOT_NULL(frames[i]);
        }

        for (i = 0; i < FR40_COUNT; i++) {
            uint64_t va = USER_PRIVATE_BASE + (uint64_t)i * 0x1000;
            ASSERT_EQ((int)kframe_map_page(frames[i], vs, va, 0u), (int)IRIS_OK);
        }
        ASSERT_EQ((int)vs->mapping_count, FR40_COUNT);

        /* kvspace_invalidate must release all 64 mapping nodes. */
        kvspace_invalidate(vs);
        ASSERT_EQ((int)vs->mapping_count, 0);
        for (i = 0; i < FR40_COUNT; i++)
            ASSERT_EQ((int)atomic_load(&frames[i]->mapped_count), 0);

        /* Frames can now be destroyed cleanly. */
        for (i = 0; i < FR40_COUNT; i++) {
            kobject_active_release(&frames[i]->base);
            kobject_release(&frames[i]->base);
        }
        kobject_release(&vs->base); /* extra retain */
        kvspace_free(vs);
        paging_stub_reset();
#undef FR40_COUNT
    }

    /* FR-41: Fase 6.1 regression — no silent demand allocation.
     * A VSpace with no explicit kframe_map_page call must have no PTEs.
     * Validates that neither KVSpace creation, kvspace_invalidate, nor any
     * internal kernel path installs a PTE without an explicit map operation.
     * If demand paging is re-introduced at the VSpace or KFrame layer, this
     * test will fail: paging_virt_to_phys_in will return non-zero. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xFA110ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x3000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        /* No map call: VA must be absent. */
        ASSERT_EQ(paging_virt_to_phys_in(0xFA110ULL, USER_PRIVATE_BASE), (uint64_t)0);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ((int)vs->mapping_count, 0);

        /* kvspace_invalidate on a VSpace with no mappings is a safe no-op. */
        kvspace_invalidate(vs);
        ASSERT_EQ(paging_virt_to_phys_in(0xFA110ULL, USER_PRIVATE_BASE), (uint64_t)0);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* ── Fase 6.2: bootstrap_kframe_map tests ──────────────────────────────── */

    /* FR-42: bootstrap_kframe_map returns non-NULL for valid inputs. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xBB0000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        uint64_t paddr  = 0xC000000ULL;
        uint64_t user_va = USER_PRIVATE_BASE + 0x1000ULL;
        struct KFrame *f = bootstrap_kframe_map(vs, paddr, user_va, 0u);
        ASSERT_NOT_NULL(f);
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-43: bootstrap_kframe_map installs a PTE; paging_virt_to_phys_in returns paddr. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xBB1000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        uint64_t paddr   = 0xD000000ULL;
        uint64_t user_va = USER_PRIVATE_BASE + 0x2000ULL;
        struct KFrame *f = bootstrap_kframe_map(vs, paddr, user_va, 0u);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, user_va), paddr);
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-44: bootstrap_kframe_map increments KVSpace mapping_count by 1. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xBB2000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        ASSERT_EQ((int)vs->mapping_count, 0);
        struct KFrame *f = bootstrap_kframe_map(
            vs, 0xE000000ULL, USER_PRIVATE_BASE + 0x3000ULL, 1u /* WRITABLE */);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ((int)vs->mapping_count, 1);
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-45: bootstrap_kframe_map increments KFrame mapped_count to 1. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xBB3000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = bootstrap_kframe_map(
            vs, 0xF000000ULL, USER_PRIVATE_BASE + 0x4000ULL, 2u /* EXEC */);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-46: bootstrap_kframe_map returns NULL for NULL vs. */
    {
        paging_stub_reset();
        struct KFrame *f = bootstrap_kframe_map(
            NULL, 0x1000000ULL, USER_PRIVATE_BASE + 0x5000ULL, 0u);
        ASSERT_NULL(f);
        paging_stub_reset();
    }

    /* FR-47: bootstrap_kframe_map returns NULL for duplicate VA (kframe_map_page
     * returns IRIS_ERR_BUSY for an already-occupied address).  The original
     * mapping must remain intact. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xBB4000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        uint64_t paddr1  = 0x10000000ULL;
        uint64_t paddr2  = 0x20000000ULL;
        uint64_t user_va = USER_PRIVATE_BASE + 0x1000ULL;

        /* First map: must succeed. */
        struct KFrame *f1 = bootstrap_kframe_map(vs, paddr1, user_va, 0u);
        ASSERT_NOT_NULL(f1);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, user_va), paddr1);
        ASSERT_EQ((int)vs->mapping_count, 1);

        /* Duplicate VA with a different phys: must return NULL (BUSY). */
        struct KFrame *f2 = bootstrap_kframe_map(vs, paddr2, user_va, 0u);
        ASSERT_NULL(f2);

        /* Original mapping untouched. */
        ASSERT_EQ(paging_virt_to_phys_in(cr3, user_va), paddr1);
        ASSERT_EQ((int)vs->mapping_count, 1);
        ASSERT_EQ((int)atomic_load(&f1->mapped_count), 1);

        kobject_active_release(&f1->base);
        kobject_release(&f1->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-48: After kvspace_invalidate, bootstrap KFrame has mapped_count==0,
     * mapping_count==0, and no PTE remains. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xBB5000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        uint64_t paddr   = 0x21000000ULL;
        uint64_t user_va = USER_PRIVATE_BASE + 0x7000ULL;
        struct KFrame *f = bootstrap_kframe_map(vs, paddr, user_va, 0u);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);
        ASSERT_EQ((int)vs->mapping_count, 1);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, user_va), paddr);

        /* Invalidating the VSpace sweeps all bootstrap KFrame mappings. */
        kvspace_invalidate(vs);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, user_va), (uint64_t)0);

        /* Release the alloc retain (maps are gone, so mapped_count==0). */
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-49: kprocess_register_bootstrap_frame rejects NULL proc or NULL frame. */
    {
        struct KProcess *p = fr_make_proc();
        ASSERT_NOT_NULL(p);
        struct KFrame *f = kframe_alloc(0x1000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        ASSERT_EQ((int)kprocess_register_bootstrap_frame(NULL, f),
                  (int)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ((int)kprocess_register_bootstrap_frame(p, NULL),
                  (int)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ((int)p->bootstrap_frame_count, 0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        fr_free_proc(p);
    }

    /* FR-50: kprocess_register_bootstrap_frame enforces the 32-slot limit;
     * kprocess_release_bootstrap_frames drops all alloc retains and resets count. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xBB6000ULL;
        struct KProcess *p = fr_make_proc();
        ASSERT_NOT_NULL(p);
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);

        /* Fill all 32 bootstrap frame slots. */
        int i;
        for (i = 0; i < 32; i++) {
            uint64_t va = USER_PRIVATE_BASE + (uint64_t)(i + 1) * 0x1000ULL;
            struct KFrame *f = bootstrap_kframe_map(
                vs, 0x30000000ULL + (uint64_t)i * 0x1000ULL, va, 0u);
            ASSERT_NOT_NULL(f);
            ASSERT_EQ((int)kprocess_register_bootstrap_frame(p, f), (int)IRIS_OK);
        }
        ASSERT_EQ((int)p->bootstrap_frame_count, 32);

        /* One more register must fail. */
        struct KFrame *f_extra = kframe_alloc(0x40000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f_extra);
        ASSERT_EQ((int)kprocess_register_bootstrap_frame(p, f_extra),
                  (int)IRIS_ERR_NO_MEMORY);
        ASSERT_EQ((int)p->bootstrap_frame_count, 32);

        /* kvspace_invalidate clears all mapping slots first. */
        kvspace_invalidate(vs);

        /* kprocess_release_bootstrap_frames drops all alloc retains. */
        kprocess_release_bootstrap_frames(p);
        ASSERT_EQ((int)p->bootstrap_frame_count, 0);

        kobject_active_release(&f_extra->base);
        kobject_release(&f_extra->base);
        kvspace_free(vs);
        fr_free_proc(p);
        paging_stub_reset();
    }

    /* ── Fase 6.3: VMO-to-Frame capability migration tests ──────────────── */

    /* FR-51: kframe_alloc_vmo_page sets vmo_owner; kframe_alloc sets vmo_owner=NULL. */
    {
        struct KVmo *v = kvmo_make_stub();
        ASSERT_NOT_NULL(v);

        struct KFrame *f1 = kframe_alloc(0x50000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f1);
        ASSERT_NULL(f1->vmo_owner);

        struct KFrame *f2 = kframe_alloc_vmo_page(0x51000000ULL, v);
        ASSERT_NOT_NULL(f2);
        ASSERT_EQ(f2->vmo_owner, v);

        kobject_active_release(&f1->base);
        kobject_release(&f1->base);
        kobject_active_release(&f2->base);
        kobject_release(&f2->base); /* triggers kframe_obj_destroy → releases vmo retain */
        kobject_release(&v->base);  /* drop alloc retain */
    }

    /* FR-52: kframe_alloc_vmo_page returns NULL for NULL vmo. */
    {
        struct KFrame *f = kframe_alloc_vmo_page(0x52000000ULL, NULL);
        ASSERT_NULL(f);
    }

    /* FR-53: kframe_alloc_vmo_page retains VMO refcount; releasing last frame
     * restores VMO refcount to its pre-alloc value. */
    {
        struct KVmo *v = kvmo_make_stub();
        ASSERT_NOT_NULL(v);
        uint32_t rc_before = atomic_load(&v->base.refcount);

        struct KFrame *f = kframe_alloc_vmo_page(0x53000000ULL, v);
        ASSERT_NOT_NULL(f);
        /* kframe_alloc_vmo_page did kobject_retain(vmo) → refcount incremented. */
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)(rc_before + 1));

        /* Release frame alloc retain.  kframe_obj_destroy → kobject_release(vmo). */
        kobject_active_release(&f->base);
        kobject_release(&f->base);
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)rc_before);

        kobject_release(&v->base);
    }

    /* FR-54: kvspace_unmap_page removes PTE, decrements mapping_count, and
     * releases the frame retain (mapped_count → 0). */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xCC0000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x54000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);
        uint64_t va = USER_PRIVATE_BASE + 0x1000ULL;

        kframe_map_page(f, vs, va, 0u);
        ASSERT_EQ((int)vs->mapping_count, 1);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);
        ASSERT_NE(paging_virt_to_phys_in(cr3, va), (uint64_t)0);

        iris_error_t ie = kvspace_unmap_page(vs, va);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-55: kvspace_unmap_page returns IRIS_ERR_NOT_FOUND for unmapped VA. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xCC1000ULL);
        ASSERT_NOT_NULL(vs);
        iris_error_t ie = kvspace_unmap_page(vs, USER_PRIVATE_BASE);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_NOT_FOUND);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-56: kvspace_unmap_page returns IRIS_ERR_BAD_HANDLE for invalidated VSpace. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xCC2000ULL);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base);
        kvspace_invalidate(vs);
        iris_error_t ie = kvspace_unmap_page(vs, USER_PRIVATE_BASE);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_BAD_HANDLE);
        kobject_release(&vs->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-57: VMO retain released only after all KFrame mappings for that VMO
     * are removed via kvspace_unmap_page.  Verifies correct destroy ordering:
     * kvspace_unmap_page → mapped_count=0 → kframe_obj_destroy → vmo release. */
    {
        paging_stub_reset();
        struct KVmo *v = kvmo_make_stub();
        ASSERT_NOT_NULL(v);
        uint32_t vmo_rc_alloc = atomic_load(&v->base.refcount);

        struct KVSpace *vs = kvspace_alloc(0xCC3000ULL);
        ASSERT_NOT_NULL(vs);
        uint64_t va = USER_PRIVATE_BASE + 0x2000ULL;

        struct KFrame *f = kframe_alloc_vmo_page(0x57000000ULL, v);
        ASSERT_NOT_NULL(f);
        /* vmo retain count: vmo_rc_alloc+1 (frame retain). */
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)(vmo_rc_alloc + 1));

        kframe_map_page(f, vs, va, 0u);
        kobject_release(&f->base); /* drop alloc retain; mapping retain still holds */
        /* Frame is still alive (mapping retain). VMO retain unchanged. */
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)(vmo_rc_alloc + 1));

        /* kvspace_unmap_page: removes mapping node, releases frame mapping retain.
         * Frame refcount hits 0 → kframe_obj_destroy → kobject_release(vmo). */
        kvspace_unmap_page(vs, va);
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)vmo_rc_alloc);

        kvspace_free(vs);
        kobject_release(&v->base); /* drop alloc retain → vmo destroy */
        paging_stub_reset();
    }

    /* FR-58: Same VMO mapped in two VSpaces.  Invalidating one VSpace releases
     * that VSpace's KFrame retain; the other VSpace and its frame retain remain
     * live until it is also invalidated. */
    {
        paging_stub_reset();
        struct KVmo *v = kvmo_make_stub();
        ASSERT_NOT_NULL(v);
        uint32_t vmo_rc_alloc = atomic_load(&v->base.refcount);

        uint64_t paddr = 0x58000000ULL;
        uint64_t va    = USER_PRIVATE_BASE + 0x3000ULL;

        struct KVSpace *vs1 = kvspace_alloc(0xCC4000ULL);
        struct KVSpace *vs2 = kvspace_alloc(0xCC5000ULL);
        ASSERT_NOT_NULL(vs1);
        ASSERT_NOT_NULL(vs2);
        kobject_retain(&vs1->base);
        kobject_retain(&vs2->base);

        struct KFrame *f1 = kframe_alloc_vmo_page(paddr, v);
        struct KFrame *f2 = kframe_alloc_vmo_page(paddr, v);
        ASSERT_NOT_NULL(f1);
        ASSERT_NOT_NULL(f2);
        /* Two frame retains on VMO. */
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)(vmo_rc_alloc + 2));

        kframe_map_page(f1, vs1, va, 0u);
        kframe_map_page(f2, vs2, va, 0u);
        kobject_release(&f1->base); /* drop alloc retain; mapping retain lives */
        kobject_release(&f2->base);

        /* Invalidate vs1: releases f1 mapping retain → kframe_obj_destroy → vmo--. */
        kvspace_invalidate(vs1);
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)(vmo_rc_alloc + 1));

        /* vs2 still intact; f2 still holds VMO retain. */
        kvspace_invalidate(vs2);
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)vmo_rc_alloc);

        kobject_release(&vs1->base);
        kobject_release(&vs2->base);
        kvspace_free(vs1);
        kvspace_free(vs2);
        kobject_release(&v->base);
        paging_stub_reset();
    }

    /* FR-59: kvspace_unmap_page handles multiple distinct VAs correctly —
     * unmapping one VA does not affect mappings at other VAs. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xCC6000ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *fa = kframe_alloc(0x59000000ULL, 4096, NULL);
        struct KFrame *fb = kframe_alloc(0x59001000ULL, 4096, NULL);
        ASSERT_NOT_NULL(fa);
        ASSERT_NOT_NULL(fb);
        uint64_t va1 = USER_PRIVATE_BASE + 0x4000ULL;
        uint64_t va2 = USER_PRIVATE_BASE + 0x5000ULL;

        kframe_map_page(fa, vs, va1, 0u);
        kframe_map_page(fb, vs, va2, 0u);
        ASSERT_EQ((int)vs->mapping_count, 2);

        kvspace_unmap_page(vs, va1);
        ASSERT_EQ((int)vs->mapping_count, 1);
        ASSERT_EQ(paging_virt_to_phys_in(0xCC6000ULL, va1), (uint64_t)0);
        /* va2 still mapped. */
        ASSERT_NE(paging_virt_to_phys_in(0xCC6000ULL, va2), (uint64_t)0);
        ASSERT_EQ((int)atomic_load(&fb->mapped_count), 1);

        kvspace_unmap_page(vs, va2);
        ASSERT_EQ((int)vs->mapping_count, 0);

        kobject_active_release(&fa->base);
        kobject_release(&fa->base);
        kobject_active_release(&fb->base);
        kobject_release(&fb->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-60: kframe_alloc_vmo_page + kframe_map_page registers in vs->mappings;
     * kvspace_invalidate releases the VMO retain held via vmo_owner. */
    {
        paging_stub_reset();
        struct KVmo *v = kvmo_make_stub();
        ASSERT_NOT_NULL(v);
        uint32_t vmo_rc_alloc = atomic_load(&v->base.refcount);

        struct KVSpace *vs = kvspace_alloc(0xCC7000ULL);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base);
        uint64_t va = USER_PRIVATE_BASE + 0x6000ULL;

        struct KFrame *f = kframe_alloc_vmo_page(0x60000000ULL, v);
        ASSERT_NOT_NULL(f);
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)(vmo_rc_alloc + 1));

        kframe_map_page(f, vs, va, 0u);
        kobject_release(&f->base); /* drop alloc retain */
        ASSERT_EQ((int)vs->mapping_count, 1);

        kvspace_invalidate(vs);
        ASSERT_EQ((int)vs->mapping_count, 0);
        /* Frame destroy released vmo retain. */
        ASSERT_EQ((int)atomic_load(&v->base.refcount), (int)vmo_rc_alloc);

        kobject_release(&vs->base);
        kvspace_free(vs);
        kobject_release(&v->base);
        paging_stub_reset();
    }

    /* FR-61: kframe_map_page with flags > 3 (bits beyond W|X) returns
     * IRIS_ERR_INVALID_ARG; no PTE installed, no mapping node created. */
    {
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xCC8000ULL);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x61000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);

        /* flag bit 2 set — invalid */
        iris_error_t ie = kframe_map_page(f, vs, USER_PRIVATE_BASE, 4u);
        ASSERT_EQ((int)ie, (int)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ(paging_virt_to_phys_in(0xCC8000ULL, USER_PRIVATE_BASE), (uint64_t)0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-62: Dynamic pool far exceeds 32 entries — map 128 frames, verify all
     * tracked; kvspace_invalidate cleans all without ASSERT or crash. */
    {
#define FR62_COUNT 128
        paging_stub_reset();
        struct KVSpace *vs = kvspace_alloc(0xCC9000ULL);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base);

        struct KFrame *frames[FR62_COUNT];
        int i;
        for (i = 0; i < FR62_COUNT; i++) {
            frames[i] = kframe_alloc(0x62000000ULL + (uint64_t)i * 0x1000, 4096, NULL);
            ASSERT_NOT_NULL(frames[i]);
            uint64_t va = USER_PRIVATE_BASE + (uint64_t)i * 0x1000;
            ASSERT_EQ((int)kframe_map_page(frames[i], vs, va, 0u), (int)IRIS_OK);
        }
        ASSERT_EQ((int)vs->mapping_count, FR62_COUNT);

        kvspace_invalidate(vs);
        ASSERT_EQ((int)vs->mapping_count, 0);
        for (i = 0; i < FR62_COUNT; i++) {
            ASSERT_EQ((int)atomic_load(&frames[i]->mapped_count), 0);
            kobject_active_release(&frames[i]->base);
            kobject_release(&frames[i]->base);
        }
        kobject_release(&vs->base);
        kvspace_free(vs);
        paging_stub_reset();
#undef FR62_COUNT
    }

    TEST_SUITE("FR: KFrame stress / invariant audit (Fase 6.4)");

    /* FR-63: kframe_unmap_page must decrement mapped_count BEFORE calling
     * kobject_release.  If the mapping retain is the last retain on the frame,
     * kobject_release triggers kframe_obj_destroy, which IRIS_ASSERTs
     * mapped_count == 0.  Calling kobject_release first (the old bug) would fire
     * that assert and abort.  This test verifies the fixed ordering. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xCD0000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base); /* keep vs alive after frame destroy */

        struct KFrame *f = kframe_alloc(0x63000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);
        uint64_t va = USER_PRIVATE_BASE + 0x7000ULL;

        iris_error_t ie = kframe_map_page(f, vs, va, 0u);
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);
        ASSERT_EQ((int)vs->mapping_count, 1);

        /* Release alloc retain: only the mapping retain now keeps f alive.
         * f->base.refcount == 1 (mapping retain only). */
        kobject_release(&f->base);

        /* kframe_unmap_page must decrement mapped_count (1→0) BEFORE calling
         * kobject_release (refcount 1→0 → kframe_obj_destroy).  If ordering is
         * wrong, kframe_obj_destroy fires with mapped_count==1 and aborts. */
        ie = kframe_unmap_page(f, vs, va); /* f destroyed here if fix is correct */
        ASSERT_EQ((int)ie, (int)IRIS_OK);
        /* VA unmapped; mapping list empty. */
        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0);

        kobject_release(&vs->base); /* drop extra retain */
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-64: kslab_alloc failure inside kframe_map_page returns IRIS_ERR_NO_MEMORY.
     * No PTE must be installed and mapping_count / mapped_count must be unchanged. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xCD1000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x64000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);
        uint64_t va = USER_PRIVATE_BASE + 0x8000ULL;

        /* Inject: next kslab_alloc (the KFrameMapping node) returns NULL. */
        kslab_fail_after(0);
        iris_error_t ie = kframe_map_page(f, vs, va, 0u);
        kslab_clear_fail();

        ASSERT_EQ((int)ie, (int)IRIS_ERR_NO_MEMORY);
        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-65: paging_map_checked_in failure after successful kslab_alloc inside
     * kframe_map_page.  The KFrameMapping node must be freed (no leak); no PTE
     * installed; mapping_count and mapped_count unchanged. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xCD2000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        struct KFrame *f = kframe_alloc(0x65000000ULL, 4096, NULL);
        ASSERT_NOT_NULL(f);
        uint64_t va = USER_PRIVATE_BASE + 0x9000ULL;

        /* Inject: kslab_alloc succeeds (let 1 call through), then paging fails. */
        paging_force_fail_next();
        iris_error_t ie = kframe_map_page(f, vs, va, 0u);
        paging_clear_force_fail();

        ASSERT_EQ((int)ie, (int)IRIS_ERR_NO_MEMORY);
        ASSERT_EQ((int)vs->mapping_count, 0);
        ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
        ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0);

        kobject_active_release(&f->base);
        kobject_release(&f->base);
        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-66: Multi-page rollback simulation — mirrors rollback_vmo_maps.
     * Map 8 pages into a VSpace, then use kvspace_unmap_page to roll back
     * pages 0..4 (simulating a mid-loop failure at page 5).  Verify:
     *   - Unmapped pages have no PTE and mapped_count == 0.
     *   - Still-mapped pages retain their PTEs and mapped_count == 1.
     *   - mapping_count == remaining pages. */
    {
#define FR66_TOTAL 8
#define FR66_ROLLBACK 5
        paging_stub_reset();
        uint64_t cr3 = 0xCD3000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base);

        struct KFrame *frames[FR66_TOTAL];
        int i;
        for (i = 0; i < FR66_TOTAL; i++) {
            frames[i] = kframe_alloc(0x66000000ULL + (uint64_t)i * 0x1000, 4096, NULL);
            ASSERT_NOT_NULL(frames[i]);
            uint64_t va = USER_PRIVATE_BASE + (uint64_t)i * 0x1000;
            ASSERT_EQ((int)kframe_map_page(frames[i], vs, va, 0u), (int)IRIS_OK);
        }
        ASSERT_EQ((int)vs->mapping_count, FR66_TOTAL);

        /* Roll back first FR66_ROLLBACK pages (mirrors rollback_vmo_maps). */
        for (i = 0; i < FR66_ROLLBACK; i++) {
            uint64_t va = USER_PRIVATE_BASE + (uint64_t)i * 0x1000;
            iris_error_t ie = kvspace_unmap_page(vs, va);
            ASSERT_EQ((int)ie, (int)IRIS_OK);
            ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0);
            ASSERT_EQ((int)atomic_load(&frames[i]->mapped_count), 0);
        }
        ASSERT_EQ((int)vs->mapping_count, FR66_TOTAL - FR66_ROLLBACK);

        /* Remaining pages still mapped correctly. */
        for (i = FR66_ROLLBACK; i < FR66_TOTAL; i++) {
            uint64_t va = USER_PRIVATE_BASE + (uint64_t)i * 0x1000;
            ASSERT_NE(paging_virt_to_phys_in(cr3, va), (uint64_t)0);
            ASSERT_EQ((int)atomic_load(&frames[i]->mapped_count), 1);
        }

        /* Clean up: invalidate removes the rest. */
        kvspace_invalidate(vs);
        ASSERT_EQ((int)vs->mapping_count, 0);
        for (i = 0; i < FR66_TOTAL; i++) {
            ASSERT_EQ((int)atomic_load(&frames[i]->mapped_count), 0);
            kobject_active_release(&frames[i]->base);
            kobject_release(&frames[i]->base);
        }
        kobject_release(&vs->base);
        kvspace_free(vs);
        paging_stub_reset();
#undef FR66_TOTAL
#undef FR66_ROLLBACK
    }

    /* FR-67: kvspace_obj_destroy safety net — directly destroy a VSpace that
     * still has active mappings (kvspace_invalidate was NOT called).  The safety
     * net inside kvspace_obj_destroy must release every mapping node, decrement
     * mapped_count, and release frame retains without panic.  This also exercises
     * the case where the mapping retain is the last retain on the frame (since the
     * alloc retain was released before calling kvspace_free). */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xCD4000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);

        /* Map 4 frames.  Release each alloc retain so only the mapping retain
         * remains — stresses the destroy ordering inside kvspace_obj_destroy. */
        for (int i = 0; i < 4; i++) {
            struct KFrame *f = kframe_alloc(0x67000000ULL + (uint64_t)i * 0x1000, 4096, NULL);
            ASSERT_NOT_NULL(f);
            uint64_t va = USER_PRIVATE_BASE + (uint64_t)i * 0x1000;
            ASSERT_EQ((int)kframe_map_page(f, vs, va, 0u), (int)IRIS_OK);
            kobject_release(&f->base); /* drop alloc retain: mapping retain is last */
        }
        ASSERT_EQ((int)vs->mapping_count, 4);

        /* Directly release vs without kvspace_invalidate.
         * kvspace_obj_destroy safety net handles remaining mappings.
         * If destroy ordering were wrong (release before mapped_count decrement),
         * kframe_obj_destroy would panic. */
        kvspace_free(vs); /* → kobject_release → refcount 1→0 → kvspace_obj_destroy */
        /* Reaching here without abort() means the safety net is correct. */
        paging_stub_reset();
    }

    /* FR-68: Map/unmap 1000-cycle stress on a single VA.
     * After each unmap: mapping_count == 0, mapped_count == 0, no PTE.
     * Verifies no accumulation of nodes, retains, or paging entries. */
    {
        paging_stub_reset();
        uint64_t cr3 = 0xCD5000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        uint64_t va = USER_PRIVATE_BASE;

        for (int i = 0; i < 1000; i++) {
            uint64_t paddr = 0x68000000ULL + (uint64_t)(i & 0xFF) * 0x1000;
            struct KFrame *f = kframe_alloc(paddr, 4096, NULL);
            ASSERT_NOT_NULL(f);

            iris_error_t ie = kframe_map_page(f, vs, va, 0u);
            ASSERT_EQ((int)ie, (int)IRIS_OK);
            ASSERT_EQ((int)vs->mapping_count, 1);
            ASSERT_EQ((int)atomic_load(&f->mapped_count), 1);

            ie = kvspace_unmap_page(vs, va);
            ASSERT_EQ((int)ie, (int)IRIS_OK);
            ASSERT_EQ((int)vs->mapping_count, 0);
            ASSERT_EQ((int)atomic_load(&f->mapped_count), 0);
            ASSERT_EQ(paging_virt_to_phys_in(cr3, va), (uint64_t)0);

            kobject_active_release(&f->base);
            kobject_release(&f->base);
        }

        kvspace_free(vs);
        paging_stub_reset();
    }

    /* FR-69: mapping_count consistency with interleaved map/unmap operations.
     * mapping_count must equal the actual number of live mappings at all times.
     * Maps 8 frames across distinct VAs, then unmaps in non-sequential order,
     * verifying the count after each unmap. */
    {
#define FR69_N 8
        paging_stub_reset();
        uint64_t cr3 = 0xCD6000ULL;
        struct KVSpace *vs = kvspace_alloc(cr3);
        ASSERT_NOT_NULL(vs);
        kobject_retain(&vs->base);

        struct KFrame *frames[FR69_N];
        uint64_t       vas[FR69_N];
        int i;
        for (i = 0; i < FR69_N; i++) {
            frames[i] = kframe_alloc(0x69000000ULL + (uint64_t)i * 0x1000, 4096, NULL);
            ASSERT_NOT_NULL(frames[i]);
            vas[i] = USER_PRIVATE_BASE + (uint64_t)i * 0x1000;
            ASSERT_EQ((int)kframe_map_page(frames[i], vs, vas[i], 0u), (int)IRIS_OK);
        }
        ASSERT_EQ((int)vs->mapping_count, FR69_N);

        /* Unmap in a non-sequential order: 3, 0, 6, 2, 7, 1, 4, 5. */
        static const int order[FR69_N] = { 3, 0, 6, 2, 7, 1, 4, 5 };
        for (i = 0; i < FR69_N; i++) {
            int idx = order[i];
            iris_error_t ie = kvspace_unmap_page(vs, vas[idx]);
            ASSERT_EQ((int)ie, (int)IRIS_OK);
            ASSERT_EQ((int)vs->mapping_count, FR69_N - i - 1);
            ASSERT_EQ((int)atomic_load(&frames[idx]->mapped_count), 0);
            ASSERT_EQ(paging_virt_to_phys_in(cr3, vas[idx]), (uint64_t)0);
        }
        ASSERT_EQ((int)vs->mapping_count, 0);

        for (i = 0; i < FR69_N; i++) {
            kobject_active_release(&frames[i]->base);
            kobject_release(&frames[i]->base);
        }
        kobject_release(&vs->base);
        kvspace_free(vs);
        paging_stub_reset();
#undef FR69_N
    }
}
