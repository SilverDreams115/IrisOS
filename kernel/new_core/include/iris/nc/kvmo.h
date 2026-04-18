#ifndef IRIS_NC_KVMO_H
#define IRIS_NC_KVMO_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <stdint.h>

#define KVMO_POOL_SIZE 128u
#define KVMO_MAX_PAGES 16384u
#define KVMO_MAX_SIZE  ((uint64_t)KVMO_MAX_PAGES * 0x1000ULL)

/* Virtual Memory Object — represents a physical memory region.
 * Can be mapped into a process address space via SYS_VMO_MAP. */
struct KVmo {
    struct KObject base;    /* must be first */
    uint64_t       phys;    /* physical base address (0 for demand VMOs) */
    uint64_t       size;    /* size in bytes */
    uint8_t        owned;   /* 1 = PMM-allocated, 0 = external (MMIO/FB) */
    uint8_t        demand;  /* 1 = lazy per-page allocation */
    uint32_t       page_capacity;     /* slots in pages[] for demand VMOs */
    uint32_t       pages_meta_pages;  /* PMM pages backing pages[] metadata */
    uint64_t       pages_meta_phys;   /* physical base of pages[] metadata */
    uint64_t      *pages;             /* phys per page; 0 = not yet allocated */
};

iris_error_t kvmo_size_to_pages(uint64_t size, uint32_t *out_pages);
struct KVmo *kvmo_create(uint64_t size);             /* allocate from PMM */
struct KVmo *kvmo_wrap  (uint64_t phys, uint64_t size); /* wrap existing phys (MMIO) */
void         kvmo_free  (struct KVmo *v);
uint32_t     kvmo_live_count(void);

#endif
