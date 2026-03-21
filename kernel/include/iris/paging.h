#ifndef IRIS_PAGING_H
#define IRIS_PAGING_H

#include <stdint.h>

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_NX       (1ULL << 63)

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define PHYS_TO_VIRT(p)  ((p) + KERNEL_VIRT_BASE)
#define VIRT_TO_PHYS(v)  ((v) - KERNEL_VIRT_BASE)

void     paging_init(uint64_t fb_phys, uint64_t fb_size);
void     paging_map(uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t paging_virt_to_phys(uint64_t virt);
uint64_t paging_create_user_space(void);

#endif
