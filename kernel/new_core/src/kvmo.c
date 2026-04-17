#include <iris/nc/kvmo.h>
#include <iris/pmm.h>
#include <stdint.h>

static struct KVmo pool[KVMO_POOL_SIZE];
static uint8_t     pool_used[KVMO_POOL_SIZE];

static void kvmo_destroy(struct KObject *obj) {
    struct KVmo *v = (struct KVmo *)obj;
    if (v->demand) {
        for (uint32_t i = 0; i < 256u; i++) {
            if (v->pages[i])
                pmm_free_page(v->pages[i]);
        }
    } else if (v->owned && v->phys) {
        uint64_t pages = (v->size + 0xFFFULL) >> 12;
        for (uint64_t i = 0; i < pages; i++)
            pmm_free_page(v->phys + i * 0x1000ULL);
    }
    for (int i = 0; i < (int)KVMO_POOL_SIZE; i++) {
        if (&pool[i] == v) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps kvmo_ops = { .destroy = kvmo_destroy };

static struct KVmo *kvmo_alloc(void) {
    for (int i = 0; i < (int)KVMO_POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            struct KVmo *v = &pool[i];
            uint8_t *p = (uint8_t *)v;
            for (uint32_t j = 0; j < sizeof(*v); j++) p[j] = 0;
            kobject_init(&v->base, KOBJ_VMO, &kvmo_ops);
            return v;
        }
    }
    return 0;
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
    if (kvmo_size_to_pages(size, &pages) != IRIS_OK)
        return 0;
    struct KVmo *v = kvmo_alloc();
    if (!v) return 0;
    v->size   = size;
    v->owned  = 1;
    v->demand = 1;
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

void kvmo_free(struct KVmo *v) {
    kobject_release(&v->base);
}

uint32_t kvmo_live_count(void) {
    uint32_t live = 0;
    for (uint32_t i = 0; i < KVMO_POOL_SIZE; i++) {
        if (pool_used[i]) live++;
    }
    return live;
}
