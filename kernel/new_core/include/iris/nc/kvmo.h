#ifndef IRIS_NC_KVMO_H
#define IRIS_NC_KVMO_H

#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>


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
                             * (Phase 6.1 removed it; FR-41 regression). */
    /* owner DELETED (Stage 7-mem).  A VMO was charged to a PROCESS: the
     * object counted against a per-process ceiling of 32 and its pages against
     * a counter, both invented by the kernel.  Stage 7 Step 14 made the memory
     * come from a budget the caller NAMES, which is the only accounting a
     * capability system needs — the Untyped is the ceiling, and it is one
     * somebody delegated.  A second ceiling on top contradicted it, the same
     * way the page quota did in Step 2 and the live-process ceiling in Step 3. */
    /* Stage 6 Step 5: the Untyped this VMO's pages, metadata and header were
     * carved from — the payer's own budget, retained for the VMO's lifetime.
     * NULL means the kernel funded it: the root task, a wrapped device region
     * (framebuffer), or a kernel selftest. */
    struct KUntyped *pool;
    uint32_t       page_capacity;     /* slots in pages[] for sparse VMOs */
    uint32_t       pages_meta_pages;  /* PMM pages backing pages[] metadata */
    uint64_t       pages_meta_phys;   /* physical base of pages[] metadata */
    uint64_t      *pages;             /* phys per page; 0 = not yet allocated */

    /* Per-page shard spinlocks for sparse VMOs.
     * Lock: page_shards[page_idx & (KVMO_PAGE_SHARDS - 1)]
     * base.lock is still used for VMO-level metadata. */
    spinlock_t     page_shards[KVMO_PAGE_SHARDS];
};

iris_error_t kvmo_size_to_pages(uint64_t size, uint32_t *out_pages);
struct KVmo *kvmo_create(uint64_t size);

/* Stage 6 Step 5 — the same VMO, with its header, its page-address array and
 * every page it later populates carved from `pool` (the payer's budget).
 * NULL funds it from kernel memory: the root task and the kernel selftests. */
struct KVmo *kvmo_create_from(uint64_t size, struct KUntyped *pool);

/* One page for a sparse VMO, from its pool when it has one. */
uint64_t     kvmo_alloc_page(struct KVmo *v);             /* allocate from PMM */
struct KVmo *kvmo_wrap  (uint64_t phys, uint64_t size); /* wrap existing phys (MMIO) */
void         kvmo_free  (struct KVmo *v);
uint32_t     kvmo_live_count(void);

#endif
