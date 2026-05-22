#ifndef IRIS_PMM_H
#define IRIS_PMM_H

#include <stdint.h>
#include <iris/boot_info.h>

#define PMM_PAGE_SIZE 4096ULL

void     pmm_init(struct iris_boot_info *boot_info);

/* Must be called once, immediately after paging_init(), to activate the
 * O(log N) buddy allocator.  Before this call pmm uses an O(N/64) bitmap
 * scan; after it uses power-of-2 buddy free lists with coalescing. */
void     pmm_buddy_setup(void);

uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(uint32_t n); /* allocates 2^ceil_log2(n) contiguous pages */

/* Allocate the largest available buddy block.
 * Writes the block order (pages = 2^order) into *order_out.
 * Returns physical base address, or 0 if PMM is empty.
 * Only valid after pmm_buddy_setup(). */
uint64_t pmm_alloc_block(uint32_t *order_out);

void     pmm_free_page(uint64_t phys_addr);

/* Free n contiguous pages starting at phys_base that were allocated as a
 * single pmm_alloc_pages(n) call.  n is rounded up to the buddy block
 * size internally.  Replaces the old per-page free loop in kpage_free. */
void     pmm_free_contig(uint64_t phys_base, uint32_t n);

uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);
uint64_t pmm_free_pages(void);

#endif
