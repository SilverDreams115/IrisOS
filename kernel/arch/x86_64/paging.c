#include <iris/paging.h>
#include <iris/pmm.h>

#define PAGE_SIZE   4096ULL
#define HUGE_SIZE   (2ULL * 1024 * 1024)
#define ENTRIES     512ULL
#define IDENTITY_MB 64ULL

#define PML4_IDX(v) (((v) >> 39) & 0x1FFULL)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FFULL)
#define PD_IDX(v)   (((v) >> 21) & 0x1FFULL)
#define PT_IDX(v)   (((v) >> 12) & 0x1FFULL)

static uint64_t pml4_phys = 0;

static uint64_t *phys_to_ptr(uint64_t phys) {
    return (uint64_t *)(uintptr_t)phys;
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
    }
    if (table[index] & PAGE_HUGE) return 0;
    return phys_to_ptr(table[index] & ~0xFFFULL);
}

void paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    /* intermediate table flags: always P+RW, carry USER bit if leaf needs it */
    uint64_t tbl_flags = PAGE_PRESENT | PAGE_WRITABLE;
    if (flags & PAGE_USER) tbl_flags |= PAGE_USER;
    uint64_t *pml4 = phys_to_ptr(pml4_phys);
    uint64_t *pdpt = get_or_create(pml4, PML4_IDX(virt), tbl_flags);
    if (!pdpt) return;
    uint64_t *pd   = get_or_create(pdpt, PDPT_IDX(virt), tbl_flags);
    if (!pd) return;
    uint64_t *pt   = get_or_create(pd,   PD_IDX(virt),   tbl_flags);
    if (!pt) return;
    pt[PT_IDX(virt)] = (phys & ~0xFFFULL) | flags | PAGE_PRESENT;
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
    uint64_t *pml4 = phys_to_ptr(pml4_phys);
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

    uint64_t identity_end = IDENTITY_MB * 1024ULL * 1024ULL;
    for (i = 0; i < identity_end; i += HUGE_SIZE)
        paging_map_huge(i, i, PAGE_PRESENT | PAGE_WRITABLE); /* kernel only */

    for (i = 0; i < 0x400000; i += PAGE_SIZE)
        paging_map(KERNEL_VIRT_BASE + 0x200000 + i,
                   0x200000 + i,
                   PAGE_PRESENT | PAGE_WRITABLE);

    if (fb_phys != 0 && fb_size != 0) {
        uint64_t fb_base = fb_phys & ~(HUGE_SIZE - 1);
        uint64_t fb_end  = (fb_phys + fb_size + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);
        for (i = fb_base; i < fb_end; i += HUGE_SIZE)
            paging_map_huge(i, i, PAGE_PRESENT | PAGE_WRITABLE); /* kernel only */
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t paging_create_user_space(void) {
    /* Allocate a fresh PML4 for a user process.
     * Copy kernel-half entries (PML4 indices 256-511) from the
     * current kernel PML4 so the kernel remains mapped after CR3 switch.
     * Lower half (0-255) is left zeroed — user gets its own mappings. */
    uint64_t new_pml4_phys = alloc_table();
    if (new_pml4_phys == 0) return 0;

    uint64_t *kernel_pml4 = phys_to_ptr(pml4_phys);
    uint64_t *user_pml4   = phys_to_ptr(new_pml4_phys);

    for (uint64_t i = 256; i < 512; i++)
        user_pml4[i] = kernel_pml4[i];

    return new_pml4_phys;
}
