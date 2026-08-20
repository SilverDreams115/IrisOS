/*
 * kpagetable.c — the page table as a retyped object (Stage 6-pure, Etapa 1).
 *
 * See kpagetable.h for what this changes and why.  The lifecycle here is
 * deliberately thin: a KPageTable is storage plus the record of where it is
 * installed.  Installing and removing it is the VSpace's business
 * (kvspace_map_table / kvspace_unmap_tables), because that is what owns the
 * hardware walk it becomes part of.
 */

#include <iris/nc/kpagetable.h>
#include <iris/nc/kuntyped.h>
#include <iris/paging.h>
#include <stdatomic.h>

static _Atomic uint32_t kpagetable_live;

uint32_t kpagetable_live_count(void) {
    return atomic_load_explicit(&kpagetable_live, memory_order_relaxed);
}

static void kpagetable_obj_close(struct KObject *obj) {
    (void)obj;
    /* Nothing to wake.  A table installed in a VSpace is held by that VSpace's
     * table list, so its refcount cannot reach zero while it is part of a live
     * walk — the close callback is never the thing that unmaps it. */
}

static void kpagetable_obj_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kpagetable_live, 1u, memory_order_relaxed);
    /* The 4 KiB region goes back the way every retyped region does: it stays
     * inside the Untyped, and the child entry the header block holds is what
     * is returned — so RESET reclaims the whole region once nothing is live. */
    kobject_storage_free(obj, (uint32_t)sizeof(struct KPageTable), 0);
}

static const struct KObjectOps kpagetable_ops = {
    .close   = kpagetable_obj_close,
    .destroy = kpagetable_obj_destroy,
};

struct KPageTable *kpagetable_alloc_at(void *mem, uint64_t paddr) {
    if (!mem || !paddr || (paddr & 0xFFFULL)) return 0;

    struct KPageTable *pt = (struct KPageTable *)mem;  /* block arrives zeroed */
    kobject_init_in_untyped(&pt->base, KOBJ_PAGE_TABLE, &kpagetable_ops,
                            (uint32_t)sizeof(struct KPageTable));
    pt->paddr     = paddr;
    pt->mapped_vs = 0;
    pt->mapped_va = 0;
    pt->level     = KPT_LEVEL_UNMAPPED;
    pt->next      = 0;
    atomic_fetch_add_explicit(&kpagetable_live, 1u, memory_order_relaxed);
    return pt;
}

void kpagetable_zero(struct KPageTable *pt) {
    if (!pt) return;
    uint64_t *t = (uint64_t *)(uintptr_t)PHYS_TO_VIRT(pt->paddr);
    for (uint32_t i = 0; i < 512u; i++) t[i] = 0;
}
