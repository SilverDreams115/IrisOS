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
#define USER_PRIVATE_BASE   0x0000008000000000ULL

extern int iris_smap_enabled;
extern int iris_pcid_enabled;

int      paging_map_checked_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
struct KUntyped;
void     paging_destroy_user_space_from(uint64_t cr3, int tables_pooled);
/* Stage 6-pure Step 1 — the walk, reported; the table, installed. */
void     paging_init_user_pml4(uint64_t pml4_page_phys);
int      paging_missing_level_in(uint64_t cr3, uint64_t virt);
int      paging_install_table_in(uint64_t cr3, uint64_t virt,
                                 uint64_t table_phys, uint64_t flags);
int      paging_map_strict_in(uint64_t cr3, uint64_t virt, uint64_t phys,
                              uint64_t flags);
uint64_t paging_virt_to_phys_in(uint64_t cr3, uint64_t virt);
void     paging_unmap_in(uint64_t cr3, uint64_t virt);

/* Reset all stub mapping state — call at the start/end of each mapping test.
 * Also clears paging_force_fail state. */
void paging_stub_reset(void);
/* Stage 6-pure Step 1: model the paging walk (levels must be installed).
 * Off by default — see the comment in stubs.c. */
void paging_stub_strict_levels(int on);

/* ── Failure injection hooks (Phase 6.4) ────────────────────────────────────
 * These are test-only; never present in the real kernel build.             */

/* kslab_fail_after(n): next kslab_alloc call after n successful ones fails.
 * n=0 fails the very next call.  kslab_clear_fail() re-enables normal alloc. */
void kslab_fail_after(int n);
void kslab_clear_fail(void);

/* paging_force_fail_next(): next paging_map_checked_in call returns -1,
 * simulating page-table allocation failure after KFrameMapping was alloc'd.
 * paging_clear_force_fail() / paging_stub_reset() disables this. */
void paging_force_fail_next(void);
void paging_clear_force_fail(void);

#endif
