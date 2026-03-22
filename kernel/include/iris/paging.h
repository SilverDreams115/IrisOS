#ifndef IRIS_PAGING_H
#define IRIS_PAGING_H

#include <stdint.h>

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_NX       (1ULL << 63)

/* ── Virtual address space layout ─────────────────────────────────
 *
 *  0x0000000000000000 - 0x00007FFFFFFFFFFF  user space (lower half)
 *    0x0000001000      user text start (identity-mapped for now)
 *    0x0000200000      user text/data (kernel ELF loads here physically)
 *    0x0000400000      user heap base  (SYS_BRK starts here)
 *    0x0000600000      user heap max
 *    0x007FFFF000      user stack top  (grows down)
 *
 *  0xFFFFFFFF80000000 - 0xFFFFFFFFFFFFFFFF  kernel space (higher half)
 *    KERNEL_VIRT_BASE + 0x200000            kernel text (.text)
 *    KERNEL_VIRT_BASE + 0x205000            kernel data (.data/.bss)
 *
 *  MMIO / framebuffer: identity-mapped physical address, kernel-only
 *  Guard pages: not mapped (any access → page fault)
 * ────────────────────────────────────────────────────────────────── */

#define KERNEL_VIRT_BASE    0xFFFFFFFF80000000ULL
#define PHYS_TO_VIRT(p)     ((p) + KERNEL_VIRT_BASE)
#define VIRT_TO_PHYS(v)     ((v) - KERNEL_VIRT_BASE)

/* kernel load address (physical and virtual) */
#define KERNEL_PHYS_BASE    0x200000ULL
#define KERNEL_VIRT_TEXT    (KERNEL_VIRT_BASE + KERNEL_PHYS_BASE)

/* user space bounds */
#define USER_SPACE_BASE     0x1000ULL
#define USER_SPACE_TOP      0x0000800000000000ULL

/* user heap region — 1 GB virtual available for brk growth */
#define USER_TEXT_BASE  0x0000000000200000ULL
#define USER_HEAP_BASE      0x0000000000400000ULL
#define USER_HEAP_MAX       0x0000000040000000ULL   /* 1 GB ceiling */

/* user stack region (top, grows down) */
#define USER_STACK_TOP      0x7FFFF000ULL
#define USER_STACK_SIZE     0x8000ULL        /* 32 KB */
#define USER_STACK_BASE     (USER_STACK_TOP - USER_STACK_SIZE)

/* identity-mapped low memory ceiling (kernel-only) */
#define IDENTITY_MAP_END    (64ULL * 1024 * 1024)  /* 64 MB */

void     paging_init(uint64_t fb_phys, uint64_t fb_size);
void     paging_map(uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t paging_virt_to_phys(uint64_t virt);
uint64_t paging_create_user_space(void);
void     paging_map_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t pml4_get_current(void);

#endif
