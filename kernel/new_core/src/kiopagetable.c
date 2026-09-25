/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kiopagetable.c — one level of a device's translation tables (Stage 10-dma).
 *
 * The lifecycle is as thin as `KPageTable`'s and for the same reason: this is
 * storage plus the record of where it is installed.  Installing it and taking
 * it out again is the IOSpace's business, because that is what owns the walk
 * it becomes part of — and, unlike a CPU page table, what owns the cache flush
 * that makes the write visible to the device.
 */

#include <iris/nc/kiopagetable.h>
#include <iris/nc/kuntyped.h>
#include <iris/paging.h>
#include <stdatomic.h>

static _Atomic uint32_t kiopagetable_live;

uint32_t kiopagetable_live_count(void) {
    return atomic_load_explicit(&kiopagetable_live, memory_order_relaxed);
}

static void kiopagetable_obj_close(struct KObject *obj) {
    (void)obj;
    /* Nothing to wake.  A level installed in an IOSpace is held by that
     * space's table list, so its refcount cannot reach zero while it is part
     * of a live walk. */
}

static void kiopagetable_obj_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kiopagetable_live, 1u, memory_order_relaxed);
    kobject_storage_free(obj, (uint32_t)sizeof(struct KIOPageTable), 0);
}

static const struct KObjectOps kiopagetable_ops = {
    .close   = kiopagetable_obj_close,
    .destroy = kiopagetable_obj_destroy,
};

struct KIOPageTable *kiopagetable_alloc_at(void *mem, uint64_t paddr) {
    if (!mem || !paddr || (paddr & 0xFFFULL)) return 0;

    struct KIOPageTable *pt = (struct KIOPageTable *)mem;  /* arrives zeroed */
    kobject_init_in_untyped(&pt->base, KOBJ_IO_PAGE_TABLE, &kiopagetable_ops,
                            (uint32_t)sizeof(struct KIOPageTable));
    pt->paddr      = paddr;
    pt->mapped_io  = 0;
    pt->mapped_dma = 0;
    pt->level      = KIOPT_LEVEL_UNMAPPED;
    pt->next       = 0;
    atomic_fetch_add_explicit(&kiopagetable_live, 1u, memory_order_relaxed);
    return pt;
}

void kiopagetable_zero(struct KIOPageTable *pt) {
    if (!pt) return;
    uint64_t *t = (uint64_t *)(uintptr_t)PHYS_TO_VIRT(pt->paddr);
    for (uint32_t i = 0; i < 512u; i++) t[i] = 0;
}
