#include <iris/nc/kvmo.h>
#include <iris/nc/kprocess.h>
#include <iris/kslab.h>
#include <iris/paging.h>
#include <iris/pmm.h>
#include <iris/nc/kuntyped.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kvmo_live;

static void kvmo_destroy(struct KObject *obj) {
    struct KVmo *v = (struct KVmo *)obj;
    struct KUntyped *pool = v->pool;
    if (v->sparse) {
        for (uint32_t i = 0; i < v->page_capacity; i++) {
            if (v->pages[i]) {
                /* Stage 6 Step 5: a pooled page is inside somebody's Untyped.
                 * Returning it to the buddy allocator would hand out memory
                 * that region still owns; what it gets back is its child
                 * entry, so the holder can RESET and reuse the whole region. */
                if (pool) kuntyped_release_page_child(pool);
                else      pmm_free_page(v->pages[i]);
            }
        }
        if (v->pages_meta_phys) {
            if (pool) kuntyped_release_page_child(pool);
            else      pmm_free_contig(v->pages_meta_phys, v->pages_meta_pages);
        }
    } else if (v->owned && v->phys) {
        uint64_t pages = (v->size + 0xFFFULL) >> 12;
        for (uint64_t i = 0; i < pages; i++)
            pmm_free_page(v->phys + i * 0x1000ULL);
    }
    atomic_fetch_sub_explicit(&kvmo_live, 1u, memory_order_relaxed);
    /* Storage first (a block holds its own parent retain), pool retain last:
     * the block lives inside the region the pool owns. */
    v->pool = 0;
    kobject_storage_free(obj, (uint32_t)sizeof(struct KVmo), 0);
    if (pool) kobject_release(&pool->base);
}

static const struct KObjectOps kvmo_ops = { .destroy = kvmo_destroy };

static struct KVmo *kvmo_alloc_in(struct KUntyped *pool) {
    struct KVmo *v = pool ? kuntyped_alloc_child_top(pool, sizeof(struct KVmo))
                          : kslab_alloc((uint32_t)sizeof(struct KVmo));
    if (!v) return 0;
    if (pool) kobject_init_in_untyped(&v->base, KOBJ_VMO, &kvmo_ops,
                                      (uint32_t)sizeof(struct KVmo));
    else      kobject_init(&v->base, KOBJ_VMO, &kvmo_ops);
    for (uint32_t i = 0; i < KVMO_PAGE_SHARDS; i++)
        spinlock_init(&v->page_shards[i]);
    if (pool) {
        kobject_retain(&pool->base);
        v->pool = pool;
    }
    atomic_fetch_add_explicit(&kvmo_live, 1u, memory_order_relaxed);
    return v;
}

/* kvmo_alloc (the budget-less form) went with kvmo_wrap: every VMO that
 * remains is charged to a pool its payer named, which is the whole of Stage 6
 * Step 5.  A helper that allocated from nowhere had exactly one caller and it
 * was the MMIO wrapper. */

/*
 * Stage 6 Step 5 — one page of a VMO, from the budget that pays for it.
 *
 * Anonymous user memory was the last thing the kernel handed out for free: a
 * process asked for a VMO and got PMM pages, bounded only by a per-process
 * quota the kernel invented.  A VMO's pages now come from the Untyped its
 * PAYER was given, so "who pays" has the same answer here as everywhere else.
 */
uint64_t kvmo_alloc_page(struct KVmo *v) {
    if (!v) return 0;
    return v->pool ? kuntyped_alloc_page_child(v->pool) : pmm_alloc_page();
}

iris_error_t kvmo_size_to_pages(uint64_t size, uint32_t *out_pages) {
    uint64_t rounded;
    uint64_t pages;

    if (!out_pages) return IRIS_ERR_INVALID_ARG;
    if (size == 0) return IRIS_ERR_INVALID_ARG;
    if (size > KVMO_MAX_SIZE) return IRIS_ERR_INVALID_ARG;
    if (size > UINT64_MAX - 0xFFFULL) return IRIS_ERR_INVALID_ARG;

    rounded = (size + 0xFFFULL) & ~0xFFFULL;
    pages = rounded >> 12;
    if (pages == 0 || pages > KVMO_MAX_PAGES || pages > UINT32_MAX)
        return IRIS_ERR_INVALID_ARG;

    *out_pages = (uint32_t)pages;
    return IRIS_OK;
}

struct KVmo *kvmo_create_from(uint64_t size, struct KUntyped *pool) {
    uint32_t pages = 0;
    uint64_t meta_bytes = 0;
    uint32_t meta_pages = 0;
    uint64_t meta_phys = 0;
    uint64_t *meta = 0;

    if (kvmo_size_to_pages(size, &pages) != IRIS_OK)
        return 0;
    struct KVmo *v = kvmo_alloc_in(pool);
    if (!v) return 0;

    meta_bytes = (uint64_t)pages * sizeof(uint64_t);
    meta_pages = (uint32_t)((meta_bytes + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE);
    /* The metadata is an array the kernel indexes, so it must be contiguous:
     * one carve, one child entry. */
    meta_phys = pool
        ? kuntyped_alloc_pages_child(pool, (uint64_t)meta_pages * PMM_PAGE_SIZE)
        : pmm_alloc_pages(meta_pages);
    if (!meta_phys) {
        kvmo_free(v);
        return 0;
    }

    meta = (uint64_t *)(uintptr_t)PHYS_TO_VIRT(meta_phys);
    for (uint64_t i = 0; i < (uint64_t)pages; i++) meta[i] = 0;

    v->size   = size;
    v->owned  = 1;
    v->sparse = 1;
    v->page_capacity = pages;
    v->pages_meta_pages = meta_pages;
    v->pages_meta_phys = meta_phys;
    v->pages = meta;
    return v;
}

struct KVmo *kvmo_create(uint64_t size) {
    return kvmo_create_from(size, 0);
}

/*
 * kvmo_wrap is DELETED (Stage 6).  Its only caller was SYS_FRAMEBUFFER_VMO,
 * which wrapped MMIO in a kernel-made object because there was no other way to
 * hand a driver a physical region.  There is now: a DEVICE Untyped (ledger
 * D-9), retyped by whoever holds it.
 */



void kvmo_free(struct KVmo *v) {
    kobject_release(&v->base);
}

uint32_t kvmo_live_count(void) {
    return atomic_load_explicit(&kvmo_live, memory_order_relaxed);
}
