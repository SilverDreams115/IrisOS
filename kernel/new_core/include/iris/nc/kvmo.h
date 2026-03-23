#ifndef IRIS_NC_KVMO_H
#define IRIS_NC_KVMO_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <stdint.h>

#define KVMO_MAX_PAGES (1024ULL * 1024ULL)
#define KVMO_MAX_SIZE  (KVMO_MAX_PAGES * 0x1000ULL)

/* Virtual Memory Object — represents a physical memory region.
 * Can be mapped into a process address space via SYS_VMO_MAP. */
struct KVmo {
    struct KObject base;    /* must be first */
    uint64_t       phys;    /* physical base address */
    uint64_t       size;    /* size in bytes */
    uint8_t        owned;   /* 1 = PMM-allocated, 0 = external (MMIO/FB) */
};

iris_error_t kvmo_size_to_pages(uint64_t size, uint32_t *out_pages);
struct KVmo *kvmo_create(uint64_t size);             /* allocate from PMM */
struct KVmo *kvmo_wrap  (uint64_t phys, uint64_t size); /* wrap existing phys (MMIO) */
void         kvmo_free  (struct KVmo *v);

#endif
