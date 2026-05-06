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
 *  0x0000000000000000 - 0x0000007FFFFFFFFF  shared low window
 *    0x0000000000000000 - 0x0000000003FFFFFF  low physical identity map
 *      kernel-only; shared by every CR3 so kernel code can still access
 *      low physical pages directly during IRQ/exception/paging paths.
 *
 *  0x0000008000000000 - 0x000000FFFFFFFFFF  private user process window
 *    0x0000008000200000  user text base
 *    0x0000008000400000  (formerly heap base — retired Phase 20)
 *    0x00000080FFFFF000  user stack top
 *
 *  0xFFFFFFFF80000000 - 0xFFFFFFFFFFFFFFFF  kernel space (higher half)
 *    0xFFFF800000000000 - 0xFFFF8000FFFFFFFF  physmap window (first 4 GB)
 *    KERNEL_VIRT_BASE + 0x200000            kernel text (.text)
 *    KERNEL_VIRT_BASE + 0x205000            kernel data (.data/.bss)
 *
 *  MMIO / framebuffer: identity-mapped physical address, kernel-only
 *  Guard pages: not mapped (any access → page fault)
 * ────────────────────────────────────────────────────────────────── */

#define KERNEL_VIRT_BASE    0xFFFFFFFF80000000ULL
#define KERNEL_PHYS_WINDOW_BASE 0xFFFF800000000000ULL
#define PHYS_TO_VIRT(p)     ((p) + KERNEL_PHYS_WINDOW_BASE)
#define VIRT_TO_PHYS(v)     ((v) - KERNEL_PHYS_WINDOW_BASE)

/* kernel load address (physical and virtual) */
#define KERNEL_PHYS_BASE    0x200000ULL
#define KERNEL_VIRT_TEXT    (KERNEL_VIRT_BASE + KERNEL_PHYS_BASE)

/* user processes reuse a small kernel-built image slice (.text + .rodata)
 * but map it into a private virtual window preserving internal offsets. */
#define USER_IMAGE_SOURCE_BASE  KERNEL_PHYS_BASE
#define USER_IMAGE_MAP_SIZE     0x0000000000014000ULL

/* user space bounds */
#define USER_SPACE_BASE     0x1000ULL
#define USER_SPACE_TOP      0x0000800000000000ULL

/* shared/private lower-half ownership split */
#define USER_SHARED_PML4_INDEX   0ULL
#define USER_PRIVATE_PML4_INDEX  1ULL
#define USER_PRIVATE_BASE        0x0000008000000000ULL
#define USER_PRIVATE_SIZE        (1ULL << 39) /* one full PML4 slot = 512 GB */

/* user text base */
#define USER_TEXT_BASE      (USER_PRIVATE_BASE + 0x00200000ULL)
/* USER_HEAP_BASE and USER_HEAP_MAX retired in Phase 20 — SYS_BRK removed.
 * Heap memory is now managed via SYS_VMO_CREATE + SYS_VMO_MAP. */

/* user VMO mappings — caller-chosen virtual addresses must stay inside this
 * private per-process window and never overlap fixed text/heap/stack regions. */
#define USER_VMO_BASE       (USER_PRIVATE_BASE + 0x50000000ULL)

/* user stack region (top, grows down) */
#define USER_STACK_TOP      (USER_PRIVATE_BASE + USER_PRIVATE_SIZE - 0x1000ULL)
#define USER_STACK_SIZE     0x8000ULL        /* 32 KB */
#define USER_STACK_BASE     (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_VMO_TOP        USER_STACK_BASE

/* identity-mapped low memory ceiling (kernel-only) */
#define IDENTITY_MAP_END    (64ULL * 1024 * 1024)  /* 64 MB */
#define PHYS_WINDOW_END     (4ULL * 1024 * 1024 * 1024) /* 4 GB */

/* ── Kernel stack virtual region ───────────────────────────────────
 *
 * Each task gets a slot of KSTACK_SLOT_SIZE bytes:
 *   [slot_base + 0               ..  slot_base + PAGE_SIZE)      guard page — NOT mapped
 *   [slot_base + PAGE_SIZE       ..  slot_base + PAGE_SIZE*2)    kstack page 0
 *   [slot_base + PAGE_SIZE*2     ..  slot_base + PAGE_SIZE*3)    kstack page 1
 *
 * A stack overflow that writes past kstack[0] immediately hits the
 * unmapped guard page, producing a #PF instead of silent corruption.
 *
 * Region starts at PHYS_WINDOW_END (= PHYS_TO_VIRT(4 GB)), which is
 * right above the physmap window and well below KERNEL_VIRT_BASE.
 * Both are within PML4 entry 256, sharing the kernel PDPT, so
 * mappings added here propagate to every process address space.
 *
 * Total virtual footprint: TASK_MAX * KSTACK_SLOT_SIZE = 64 * 12288 = 768 KB.
 * ─────────────────────────────────────────────────────────────────── */
#define KSTACK_VIRT_BASE  (KERNEL_PHYS_WINDOW_BASE + PHYS_WINDOW_END)  /* 0xFFFF800100000000 */
#define KSTACK_SLOT_SIZE  (3ULL * 4096ULL)   /* guard + 2 stack pages = 12 288 bytes */

/* Set to 1 by paging_init() when SMAP is active in CR4.
 * Checked by usercopy.c to emit STAC/CLAC around user memory accesses. */
extern int iris_smap_enabled;

void     paging_init(uint64_t fb_phys, uint64_t fb_size);
void     paging_map(uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t paging_virt_to_phys(uint64_t virt);
int      paging_query_access(uint64_t virt, uint64_t *out_flags);
uint64_t paging_create_user_space(void);
/* Legacy best-effort mapper. Critical syscalls and rollback-sensitive paths
 * must use paging_map_checked_in() instead. */
void     paging_map_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
int      paging_map_checked_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t paging_virt_to_phys_in(uint64_t cr3, uint64_t virt);
int      paging_query_access_in(uint64_t cr3, uint64_t virt, uint64_t *out_flags);
void     paging_unmap_in(uint64_t cr3, uint64_t virt);
void     paging_write_u64_in(uint64_t cr3, uint64_t virt, uint64_t value);
void     paging_destroy_user_space(uint64_t cr3);
uint64_t pml4_get_current(void);

#endif
