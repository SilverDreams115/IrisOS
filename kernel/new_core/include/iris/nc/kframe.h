#ifndef IRIS_NC_KFRAME_H
#define IRIS_NC_KFRAME_H

#ifdef __KERNEL__
#include <stdint.h>
#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <iris/nc/kuntyped.h>

/* Forward declaration — full definition in iris/nc/kvmo.h. */
struct KVmo;

/*
 * KFrame — Frame capability object (Fase 5).
 *
 * Represents formal authority over a contiguous physical memory region
 * suitable for mapping into a VSpace.  Created exclusively via
 * SYS_UNTYPED_RETYPE with KOBJ_FRAME from a KUntyped parent.
 *
 * Ownership model:
 *   - Physical region is owned by the parent KUntyped (bump-allocated).
 *   - KFrame header is heap-allocated (kslab) independently.
 *   - kframe_destroy decrements parent->child_count and releases the
 *     parent retain, mirroring the sub-untyped teardown pattern.
 *
 * Bootstrap frames (Fase 6.2):
 *   bootstrap_kframe_map creates a KFrame with alloc_parent=NULL for
 *   one physical page and maps it immediately into a KVSpace.  The
 *   alloc retain is held by the caller (stored in KProcess.bootstrap_frames).
 *   Physical memory lifetime is managed externally (task struct fields);
 *   KFrame destroy only calls kslab_free.
 *
 * VMO-backed frames (Fase 6.3):
 *   kframe_alloc_vmo_page creates a KFrame with alloc_parent=NULL and
 *   vmo_owner=v (retaining v).  Physical memory is owned by the VMO.
 *   kframe_obj_destroy releases the vmo_owner retain; this delays kvmo_destroy
 *   (and thus pmm_free_page on VMO pages) until all KFrames for the VMO's
 *   pages are destroyed, ensuring no use-after-free in the page table.
 *
 * Mapping lifecycle (Fase 5.1):
 *   mapped_count tracks how many PTEs currently point at this frame across
 *   all VSpaces.  kframe_map_page increments it; kframe_unmap_page decrements
 *   it.  kframe_obj_destroy asserts mapped_count == 0: callers must unmap
 *   all pages before dropping the last capability reference.
 */
struct KFrame {
    struct KObject   base;          /* must be first */
    uint64_t         paddr;         /* physical base (PAGE_SIZE aligned) */
    uint64_t         size;          /* byte size (PAGE_SIZE multiple, >= 4096) */
    struct KUntyped *alloc_parent;  /* parent for child_count bookkeeping; NULL for VMO/bootstrap frames */
    struct KVmo     *vmo_owner;     /* VMO that owns the physical page; NULL if no VMO parent */
    _Atomic uint32_t mapped_count;  /* PTEs referencing this frame; must be 0 at destroy */
};

/* Forward declaration — full definition in iris/nc/kvspace.h. */
struct KVSpace;

/* Allocate a KFrame header from the kernel slab allocator and initialise it.
 * Does NOT touch the physical region at paddr.
 * Increments alloc_parent->child_count and retains alloc_parent.
 * Returns NULL on OOM.  Caller holds the alloc lifecycle ref (refcount=1). */
struct KFrame *kframe_alloc(uint64_t paddr, uint64_t size,
                             struct KUntyped *alloc_parent);

/*
 * kframe_alloc_vmo_page — Fase 6.3: allocate a 4-KiB KFrame backed by a VMO page.
 *
 * Creates a KFrame with alloc_parent=NULL and vmo_owner=vmo.  Retains vmo so
 * that the VMO is not destroyed while any KFrame for its pages remains alive.
 * Physical lifetime is managed by the VMO (pmm_free_page in kvmo_destroy).
 *
 * Returns NULL if vmo is NULL or kslab_alloc fails.
 * Caller holds the alloc lifecycle ref; typically released immediately after
 * kframe_map_page succeeds (the mapping retain is held by KVSpace.mappings).
 *
 * Do NOT pass a wrap (MMIO) VMO — use kframe_alloc(paddr, 4096, NULL) for those.
 */
struct KFrame *kframe_alloc_vmo_page(uint64_t paddr, struct KVmo *vmo);

/*
 * kframe_map_page — install one 4-KiB PTE for this frame in a VSpace.
 *
 * map_flags: bit 0 = MAP_WRITABLE, bit 1 = MAP_EXEC.
 *   W^X: both bits set → IRIS_ERR_INVALID_ARG.
 *   Writable maps require RIGHT_WRITE on the frame cap; the caller (syscall
 *   layer) enforces rights before calling this function.
 *
 * Returns:
 *   IRIS_OK           — PTE installed; mapped_count incremented.
 *   IRIS_ERR_INVALID_ARG — bad VA alignment, kernel VA, or bad flags.
 *   IRIS_ERR_BAD_HANDLE  — VSpace has been invalidated (cr3 == 0).
 *   IRIS_ERR_BUSY        — user_va already has a PTE in this VSpace.
 *   IRIS_ERR_NO_MEMORY   — page-table allocation failed.
 *
 * Single-core TLB note: no TLB flush is needed on map (new PTE, no stale
 * entry).  SMP shootdown is deferred to Fase 6.
 */
iris_error_t kframe_map_page(struct KFrame *f, struct KVSpace *vs,
                              uint64_t user_va, uint64_t map_flags);

/*
 * kframe_unmap_page — remove the PTE at user_va in a VSpace if it belongs
 * to this frame.
 *
 * Returns:
 *   IRIS_OK              — PTE removed; mapped_count decremented; invlpg issued.
 *   IRIS_ERR_INVALID_ARG — bad VA, or the VA maps a different frame's paddr.
 *   IRIS_ERR_BAD_HANDLE  — VSpace has been invalidated.
 *   IRIS_ERR_NOT_FOUND   — user_va has no PTE in this VSpace.
 */
iris_error_t kframe_unmap_page(struct KFrame *f, struct KVSpace *vs,
                                uint64_t user_va);

/*
 * bootstrap_kframe_map — Bootstrap-only internal helper (Fase 6.2).
 *
 * Allocates a KFrame (alloc_parent=NULL) for the physical page at paddr
 * (4 KiB, page-aligned) and immediately maps it into vs at user_va with
 * the given map_flags (same bit encoding as kframe_map_page).
 *
 * Returns the KFrame with the alloc retain held by the caller.
 * The caller MUST call kobject_release(&f->base) after kvspace_invalidate
 * has released the mapping retain — kframe_obj_destroy asserts
 * mapped_count == 0 at destruction time.
 *
 * Returns NULL on any failure; no PTE is installed and no mapping slot
 * is consumed on failure.  paddr is never freed by this function.
 *
 * Do NOT call from runtime syscall paths — bootstrap use only.
 */
struct KFrame *bootstrap_kframe_map(struct KVSpace *vs,
                                     uint64_t       paddr,
                                     uint64_t       user_va,
                                     uint64_t       map_flags);

/* VA validation helper: returns 1 if va is a valid page-aligned user address.
 * Checks: 4K alignment, canonical, in [USER_PRIVATE_BASE, USER_SPACE_TOP). */
static inline int kframe_va_valid(uint64_t va) {
    /* Must be page-aligned. */
    if (va & 0xFFFULL) return 0;
    /* Must be in user private window: [0x0000008000000000, 0x0000800000000000). */
    if (va < 0x0000008000000000ULL) return 0;
    if (va >= 0x0000800000000000ULL) return 0;
    /* Canonical x86-64: bits 63:47 must all be 0 for user-space addresses. */
    if (va >> 47ULL) return 0;
    return 1;
}

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KFRAME_H */
