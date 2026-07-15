#ifndef IRIS_NC_KVMO_H
#define IRIS_NC_KVMO_H

#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

struct KProcess;

#define KVMO_POOL_SIZE 0u
#define KVMO_MAX_PAGES 16384u
#define KVMO_MAX_SIZE  ((uint64_t)KVMO_MAX_PAGES * 0x1000ULL)

/*
 * Number of per-page shard locks in each sparse VMO.
 *
 * A single base.lock for all sparse per-page allocations creates a bottleneck
 * when multiple threads of the same process touch different pages
 * simultaneously (e.g. concurrent SYS_VMO_MAP population).
 * Shard locks reduce contention: page_idx is hashed to a shard with a bitmask,
 * so pages that fall in different shards can be allocated concurrently.
 *
 * 16 shards (4-bit hash) balances struct size against concurrency benefit.
 * Must be a power of two.
 */
#define KVMO_PAGE_SHARDS 16u

/* Virtual Memory Object — represents a physical memory region.
 * Can be mapped into a process address space via SYS_VMO_MAP. */
struct KVmo {
    struct KObject base;    /* must be first */
    uint64_t       phys;    /* physical base address (0 for sparse VMOs) */
    uint64_t       size;    /* size in bytes */
    uint8_t        owned;   /* 1 = PMM-allocated, 0 = external (MMIO/FB) */
    uint8_t        sparse;  /* 1 = sparse VMO: per-page physical frames
                             * (pages[]), allocated EAGERLY at map time —
                             * there is NO fault-driven demand paging
                             * (Fase 6.1 removed it; FR-41 regression). */
    struct KProcess *owner; /* creator retained for quota accounting */
    uint32_t       page_capacity;     /* slots in pages[] for sparse VMOs */
    uint32_t       pages_meta_pages;  /* PMM pages backing pages[] metadata */
    uint64_t       pages_meta_phys;   /* physical base of pages[] metadata */
    uint64_t      *pages;             /* phys per page; 0 = not yet allocated */

    /* Per-page shard spinlocks for sparse VMOs.
     * Lock: page_shards[page_idx & (KVMO_PAGE_SHARDS - 1)]
     * base.lock is still used for VMO-level metadata (owner binding, etc.). */
    spinlock_t     page_shards[KVMO_PAGE_SHARDS];
};

iris_error_t kvmo_size_to_pages(uint64_t size, uint32_t *out_pages);
struct KVmo *kvmo_create(uint64_t size);             /* allocate from PMM */
struct KVmo *kvmo_wrap  (uint64_t phys, uint64_t size); /* wrap existing phys (MMIO) */
iris_error_t kvmo_bind_owner(struct KVmo *v, struct KProcess *owner);
/* Fase 29: the VMO's payer domain — the process charged for the VMO object and
 * for its sparse physical pages (charged once, at page allocation; released at
 * kvmo_destroy).  NULL only if never bound. */
struct KProcess *kvmo_owner(const struct KVmo *v);
void         kvmo_free  (struct KVmo *v);
uint32_t     kvmo_live_count(void);

#endif
