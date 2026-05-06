#include <iris/nc/kvmo.h>
#include <iris/nc/kprocess.h>
#include <iris/kpage.h>
#include <iris/paging.h>
#include <iris/pmm.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kvmo_live;

static void kvmo_destroy(struct KObject *obj) {
    struct KVmo *v = (struct KVmo *)obj;
    if (v->demand) {
        for (uint32_t i = 0; i < v->page_capacity; i++) {
            if (v->pages[i])
                pmm_free_page(v->pages[i]);
        }
        if (v->pages_meta_phys) {
            for (uint32_t i = 0; i < v->pages_meta_pages; i++)
                pmm_free_page(v->pages_meta_phys + (uint64_t)i * PMM_PAGE_SIZE);
        }
    } else if (v->owned && v->phys) {
        uint64_t pages = (v->size + 0xFFFULL) >> 12;
        for (uint64_t i = 0; i < pages; i++)
            pmm_free_page(v->phys + i * 0x1000ULL);
    }
    if (v->owner) {
        struct KProcess *owner = v->owner;
        v->owner = 0;
        kprocess_quota_release_vmo(owner);
        kobject_release(&owner->base);
    }
    atomic_fetch_sub_explicit(&kvmo_live, 1u, memory_order_relaxed);
    kpage_free(v, (uint32_t)sizeof(struct KVmo));
}

static const struct KObjectOps kvmo_ops = { .destroy = kvmo_destroy };

static struct KVmo *kvmo_alloc(void) {
    struct KVmo *v = kpage_alloc((uint32_t)sizeof(struct KVmo));
    if (!v) return 0;
    kobject_init(&v->base, KOBJ_VMO, &kvmo_ops);
    atomic_fetch_add_explicit(&kvmo_live, 1u, memory_order_relaxed);
    return v;
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

struct KVmo *kvmo_create(uint64_t size) {
    uint32_t pages = 0;
    uint64_t meta_bytes = 0;
    uint32_t meta_pages = 0;
    uint64_t meta_phys = 0;
    uint64_t *meta = 0;

    if (kvmo_size_to_pages(size, &pages) != IRIS_OK)
        return 0;
    struct KVmo *v = kvmo_alloc();
    if (!v) return 0;

    meta_bytes = (uint64_t)pages * sizeof(uint64_t);
    meta_pages = (uint32_t)((meta_bytes + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE);
    meta_phys = pmm_alloc_pages(meta_pages);
    if (!meta_phys) {
        kvmo_free(v);
        return 0;
    }

    meta = (uint64_t *)(uintptr_t)PHYS_TO_VIRT(meta_phys);
    for (uint64_t i = 0; i < (uint64_t)pages; i++) meta[i] = 0;

    v->size   = size;
    v->owned  = 1;
    v->demand = 1;
    v->page_capacity = pages;
    v->pages_meta_pages = meta_pages;
    v->pages_meta_phys = meta_phys;
    v->pages = meta;
    return v;
}

struct KVmo *kvmo_wrap(uint64_t phys, uint64_t size) {
    struct KVmo *v = kvmo_alloc();
    if (!v) return 0;
    v->phys  = phys;
    v->size  = size;
    v->owned = 0;
    return v;
}

iris_error_t kvmo_bind_owner(struct KVmo *v, struct KProcess *owner) {
    iris_error_t r;
    if (!v || !owner) return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&v->base.lock);
    if (v->owner) {
        r = (v->owner == owner) ? IRIS_OK : IRIS_ERR_BUSY;
        spinlock_unlock(&v->base.lock);
        return r;
    }
    spinlock_unlock(&v->base.lock);

    r = kprocess_quota_acquire_vmo(owner);
    if (r != IRIS_OK) return r;
    kobject_retain(&owner->base);

    spinlock_lock(&v->base.lock);
    if (v->owner) {
        spinlock_unlock(&v->base.lock);
        kobject_release(&owner->base);
        kprocess_quota_release_vmo(owner);
        return (v->owner == owner) ? IRIS_OK : IRIS_ERR_BUSY;
    }
    v->owner = owner;
    spinlock_unlock(&v->base.lock);
    return IRIS_OK;
}

void kvmo_free(struct KVmo *v) {
    kobject_release(&v->base);
}

uint32_t kvmo_live_count(void) {
    return atomic_load_explicit(&kvmo_live, memory_order_relaxed);
}
