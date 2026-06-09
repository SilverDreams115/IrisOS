#ifndef IRIS_PAGING_H
#define IRIS_PAGING_H
#include <stdint.h>

/* Test stub: PHYS_TO_VIRT is identity — tests use malloc'd virtual addresses
 * as "physical" addresses.  Page flag constants must match the real header. */
#define PHYS_TO_VIRT(p)   ((uint64_t)(p))
#define VIRT_TO_PHYS(v)   ((uint64_t)(v))

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_NX       (1ULL << 63)

#define USER_SPACE_TOP      0x0000800000000000ULL

extern int iris_smap_enabled;
extern int iris_pcid_enabled;

#endif
