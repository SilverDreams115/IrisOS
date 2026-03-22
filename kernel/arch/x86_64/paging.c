#include <iris/paging.h>
#include <iris/pmm.h>

#define PAGE_SIZE   4096ULL
#define HUGE_SIZE   (2ULL * 1024 * 1024)
#define ENTRIES     512ULL
/* IDENTITY_MB removed — use IDENTITY_MAP_END from paging.h */

#define PML4_IDX(v) (((v) >> 39) & 0x1FFULL)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FFULL)
#define PD_IDX(v)   (((v) >> 21) & 0x1FFULL)
#define PT_IDX(v)   (((v) >> 12) & 0x1FFULL)

static uint64_t pml4_phys = 0;
static int phys_window_ready = 0;

static uint64_t *phys_to_ptr(uint64_t phys) {
    if (!phys_window_ready)
        return (uint64_t *)(uintptr_t)phys;
    return (uint64_t *)(uintptr_t)PHYS_TO_VIRT(phys);
}

static uint64_t read_cr3_phys(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ~0xFFFULL;
}

static uint64_t *walk_pt(uint64_t cr3, uint64_t virt) {
    uint64_t *pml4 = phys_to_ptr(cr3);
    uint64_t entry = pml4[PML4_IDX(virt)];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_HUGE)) return 0;

    uint64_t *pdpt = phys_to_ptr(entry & ~0xFFFULL);
    entry = pdpt[PDPT_IDX(virt)];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_HUGE)) return 0;

    uint64_t *pd = phys_to_ptr(entry & ~0xFFFULL);
    entry = pd[PD_IDX(virt)];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_HUGE)) return 0;

    return phys_to_ptr(entry & ~0xFFFULL);
}

static void destroy_pt_level(uint64_t table_phys, int level) {
    uint64_t *table = phys_to_ptr(table_phys);

    if (level == 1) {
        pmm_free_page(table_phys);
        return;
    }

    for (uint64_t i = 0; i < ENTRIES; i++) {
        uint64_t entry = table[i];
        if (!(entry & PAGE_PRESENT)) continue;
        if (entry & PAGE_HUGE) continue;
        destroy_pt_level(entry & ~0xFFFULL, level - 1);
        table[i] = 0;
    }

    pmm_free_page(table_phys);
}

static uint64_t alloc_table(void) {
    uint64_t page = pmm_alloc_page();
    if (page == 0) return 0;
    uint64_t *table = phys_to_ptr(page);
    for (uint64_t i = 0; i < ENTRIES; i++)
        table[i] = 0;
    return page;
}

static uint64_t *get_or_create(uint64_t *table, uint64_t index, uint64_t flags) {
    if (!(table[index] & PAGE_PRESENT)) {
        uint64_t new_table = alloc_table();
        if (new_table == 0) return 0;
        table[index] = new_table | flags;
    } else {
        /* table already exists — OR in any new permission bits needed.
         * Critical: if caller needs PAGE_USER, existing kernel-only
         * intermediate tables must be upgraded or ring 3 access will fault. */
        table[index] |= (flags & (PAGE_USER | PAGE_WRITABLE));
    }
    if (table[index] & PAGE_HUGE) return 0;
    return phys_to_ptr(table[index] & ~0xFFFULL);
}

static int paging_map_root(uint64_t root_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    /* intermediate table flags: always P+RW, carry USER bit if leaf needs it */
    uint64_t tbl_flags = PAGE_PRESENT | PAGE_WRITABLE;
    if (flags & PAGE_USER) tbl_flags |= PAGE_USER;
    uint64_t *pml4 = phys_to_ptr(root_phys);
    uint64_t *pdpt = get_or_create(pml4, PML4_IDX(virt), tbl_flags);
    if (!pdpt) return -1;
    uint64_t *pd   = get_or_create(pdpt, PDPT_IDX(virt), tbl_flags);
    if (!pd) return -1;
    uint64_t *pt   = get_or_create(pd,   PD_IDX(virt),   tbl_flags);
    if (!pt) return -1;
    pt[PT_IDX(virt)] = (phys & ~0xFFFULL) | flags | PAGE_PRESENT;
    return 0;
}

void paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)paging_map_root(pml4_phys, virt, phys, flags);
}

static void paging_map_huge(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t tbl_flags = PAGE_PRESENT | PAGE_WRITABLE;
    if (flags & PAGE_USER) tbl_flags |= PAGE_USER;
    uint64_t *pml4 = phys_to_ptr(pml4_phys);
    uint64_t *pdpt = get_or_create(pml4, PML4_IDX(virt), tbl_flags);
    if (!pdpt) return;
    uint64_t *pd   = get_or_create(pdpt, PDPT_IDX(virt), tbl_flags);
    if (!pd) return;
    pd[PD_IDX(virt)] = (phys & ~0x1FFFFFULL) | flags | PAGE_PRESENT | PAGE_HUGE;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    uint64_t *pml4 = phys_to_ptr(read_cr3_phys());
    uint64_t entry;

    entry = pml4[PML4_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_ptr(entry & ~0xFFFULL);

    entry = pdpt[PDPT_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    uint64_t *pd = phys_to_ptr(entry & ~0xFFFULL);

    entry = pd[PD_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (entry & PAGE_HUGE)
        return (entry & ~0x1FFFFFULL) | (virt & 0x1FFFFFULL);

    uint64_t *pt = phys_to_ptr(entry & ~0xFFFULL);
    entry = pt[PT_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    return (entry & ~0xFFFULL) | (virt & 0xFFFULL);
}

void paging_init(uint64_t fb_phys, uint64_t fb_size) {
    uint64_t i;

    pml4_phys = alloc_table();
    if (pml4_phys == 0) return;

    for (i = 0; i < IDENTITY_MAP_END; i += HUGE_SIZE)
        paging_map_huge(i, i, PAGE_PRESENT | PAGE_WRITABLE); /* kernel only */

    for (i = 0; i < PHYS_WINDOW_END; i += HUGE_SIZE)
        paging_map_huge(PHYS_TO_VIRT(i), i, PAGE_PRESENT | PAGE_WRITABLE);

    /* map kernel text+data: KERNEL_PHYS_BASE .. KERNEL_PHYS_BASE+4MB */
    for (i = 0; i < 0x400000; i += PAGE_SIZE)
        paging_map(KERNEL_VIRT_TEXT + i,
                   KERNEL_PHYS_BASE + i,
                   PAGE_PRESENT | PAGE_WRITABLE); /* W^X applied in Fix 14 */

    if (fb_phys != 0 && fb_size != 0) {
        uint64_t fb_base = fb_phys & ~(HUGE_SIZE - 1);
        uint64_t fb_end  = (fb_phys + fb_size + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);
        for (i = fb_base; i < fb_end; i += HUGE_SIZE)
            paging_map_huge(i, i, PAGE_PRESENT | PAGE_WRITABLE); /* kernel only */
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
    phys_window_ready = 1;
}

uint64_t paging_create_user_space(void)
{
    uint64_t new_pml4_phys = pmm_alloc_pages(1);
    if (new_pml4_phys == 0) return 0;

    uint64_t *kernel_pml4 = phys_to_ptr(pml4_phys);
    uint64_t *user_pml4   = phys_to_ptr(new_pml4_phys);

    for (uint64_t i = 0; i < ENTRIES; i++) {
        user_pml4[i] = 0;
    }

    /* Shared low kernel window: identity-mapped low memory remains available
     * to kernel code under any CR3, but user mappings live elsewhere. */
    user_pml4[USER_SHARED_PML4_INDEX] = kernel_pml4[USER_SHARED_PML4_INDEX];

    /* Higher half is always shared kernel space. */
    for (uint64_t i = 256; i < ENTRIES; i++) {
        user_pml4[i] = kernel_pml4[i];
    }

    return new_pml4_phys;
}

uint64_t pml4_get_current(void) {
    return pml4_phys;
}

void paging_map_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)paging_map_checked_in(cr3, virt, phys, flags);
}

int paging_map_checked_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (cr3 == 0) return -1;

    /* Deshabilitar interrupciones mientras modificamos page tables ajenas,
     * manteniendo atomica la secuencia para callers de bootstrap/rollback. */
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    int rc = paging_map_root(cr3, virt, phys, flags);
    __asm__ volatile ("pushq %0; popfq" : : "r"(rflags) : "memory");
    return rc;
}

uint64_t paging_virt_to_phys_in(uint64_t cr3, uint64_t virt) {
    if (cr3 == 0) return 0;

    uint64_t *pml4 = phys_to_ptr(cr3);
    uint64_t entry = pml4[PML4_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;

    uint64_t *pdpt = phys_to_ptr(entry & ~0xFFFULL);
    entry = pdpt[PDPT_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;

    uint64_t *pd = phys_to_ptr(entry & ~0xFFFULL);
    entry = pd[PD_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (entry & PAGE_HUGE)
        return (entry & ~0x1FFFFFULL) | (virt & 0x1FFFFFULL);

    uint64_t *pt = phys_to_ptr(entry & ~0xFFFULL);
    entry = pt[PT_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    return (entry & ~0xFFFULL) | (virt & 0xFFFULL);
}

void paging_unmap_in(uint64_t cr3, uint64_t virt) {
    uint64_t *pt = walk_pt(cr3, virt);
    if (!pt) return;
    pt[PT_IDX(virt)] = 0;
}

void paging_write_u64_in(uint64_t cr3, uint64_t virt, uint64_t value) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    uint64_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    *(volatile uint64_t *)(uintptr_t)virt = value;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
    __asm__ volatile ("pushq %0; popfq" : : "r"(rflags) : "memory");
}

void paging_destroy_user_space(uint64_t cr3) {
    if (cr3 == 0) return;

    uint64_t *pml4 = phys_to_ptr(cr3);
    uint64_t root = pml4[USER_PRIVATE_PML4_INDEX];
    if (root & PAGE_PRESENT) {
        destroy_pt_level(root & ~0xFFFULL, 3);
        pml4[USER_PRIVATE_PML4_INDEX] = 0;
    }

    pmm_free_page(cr3);
}
