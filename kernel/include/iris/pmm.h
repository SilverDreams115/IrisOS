#ifndef IRIS_PMM_H
#define IRIS_PMM_H

#include <stdint.h>
#include <iris/boot_info.h>

#define PMM_PAGE_SIZE 4096ULL

void     pmm_init(struct iris_boot_info *boot_info);
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys_addr);
uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);
uint64_t pmm_free_pages(void);

#endif
