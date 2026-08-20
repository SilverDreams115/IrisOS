#include <iris/paging.h>
#include <iris/pmm.h>

#define PAGE_SIZE   4096ULL
#define HUGE_SIZE   (2ULL * 1024 * 1024)
#define ENTRIES     512ULL

#define PML4_IDX(v) (((v) >> 39) & 0x1FFULL)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FFULL)
#define PD_IDX(v)   (((v) >> 21) & 0x1FFULL)
#define PT_IDX(v)   (((v) >> 12) & 0x1FFULL)

/* Linker-script section boundary symbols — used to apply W^X permissions. */
extern char __text_start, __text_end;
extern char __rodata_start, __rodata_end;
extern char __data_start;
extern char __kernel_end;

static uint64_t pml4_phys = 0;
static int phys_window_ready = 0;

int iris_smap_enabled = 0;
int iris_pcid_enabled = 0;

/* Enable IA32_EFER.NXE so PAGE_NX (bit 63) in PTEs is honoured.
 * Must be called before any NX-flagged page table entry is loaded into the TLB. */
static void paging_enable_nxe(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= (1u << 11);  /* NXE bit */
    __asm__ volatile ("wrmsr" : : "c"(0xC0000080u), "a"(lo), "d"(hi));
}

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

static int paging_query_access_root(uint64_t cr3, uint64_t virt, uint64_t *out_flags) {
    uint64_t effective = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t *pml4 = phys_to_ptr(cr3);
    uint64_t entry = pml4[PML4_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return -1;
    effective &= (entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER));

    uint64_t *pdpt = phys_to_ptr(entry & ~0xFFFULL);
    entry = pdpt[PDPT_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return -1;
    effective &= (entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER));

    uint64_t *pd = phys_to_ptr(entry & ~0xFFFULL);
    entry = pd[PD_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return -1;
    effective &= (entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER));
    if (entry & PAGE_HUGE) {
        *out_flags = effective;
        return 0;
    }

    uint64_t *pt = phys_to_ptr(entry & ~0xFFFULL);
    entry = pt[PT_IDX(virt)];
    if (!(entry & PAGE_PRESENT)) return -1;
    effective &= (entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER));
    *out_flags = effective;
    return 0;
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

/*
 * Where an intermediate page table comes from — and by Stage 6-pure, the
 * answer is "only here, only for the two address spaces with no holder".
 *
 * Stage 6 Step 2 gave this function a `pool` argument so a USER address space
 * could charge its levels to a budget.  Stage 6-pure Step 2 stopped the
 * kernel creating user levels at all — the holder retypes a KOBJ_PAGE_TABLE
 * and installs it — so every caller now passes the PMM path and the pooled
 * branch was dead code that still claimed the kernel could spend somebody's
 * budget behind their back.  The two callers left are the KERNEL's own address
 * space and the root task's pre-run maps, neither of which has a holder to ask.
 */
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

/*
 * Stage 6-pure Step 1 — install a table the USER retyped, and never carve.
 *
 * paging_missing_level_in reports the deepest level that already exists for
 * `virt`, so an invocation can say exactly which table the holder still owes
 * instead of failing with a bare NO_MEMORY.  paging_install_table_in puts a
 * retyped page at the first missing level and returns which one that was.
 *
 * Together they are seL4's contract: the kernel walks and reports, the holder
 * supplies.  Nothing here allocates.
 */
int paging_missing_level_in(uint64_t cr3, uint64_t virt) {
    if (cr3 == 0) return -1;
    uint64_t *pml4 = phys_to_ptr(cr3);
    uint64_t e = pml4[PML4_IDX(virt)];
    if (!(e & PAGE_PRESENT)) return 3;              /* needs a PDPT */
    uint64_t *pdpt = phys_to_ptr(e & ~0xFFFULL);
    e = pdpt[PDPT_IDX(virt)];
    if (!(e & PAGE_PRESENT)) return 2;              /* needs a PD */
    if (e & PAGE_HUGE)       return -1;             /* 1 GiB leaf: no table fits */
    uint64_t *pd = phys_to_ptr(e & ~0xFFFULL);
    e = pd[PD_IDX(virt)];
    if (!(e & PAGE_PRESENT)) return 1;              /* needs a PT */
    if (e & PAGE_HUGE)       return -1;             /* 2 MiB leaf: no table fits */
    return 0;                                       /* the walk is complete */
}

int paging_install_table_in(uint64_t cr3, uint64_t virt, uint64_t table_phys,
                            uint64_t flags) {
    if (cr3 == 0 || table_phys == 0 || (table_phys & 0xFFFULL)) return -1;

    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    int level = paging_missing_level_in(cr3, virt);
    if (level > 0) {
        uint64_t *tbl;
        uint64_t  idx;
        if (level == 3) { tbl = phys_to_ptr(cr3); idx = PML4_IDX(virt); }
        else {
            uint64_t *pml4 = phys_to_ptr(cr3);
            uint64_t *pdpt = phys_to_ptr(pml4[PML4_IDX(virt)] & ~0xFFFULL);
            if (level == 2) { tbl = pdpt; idx = PDPT_IDX(virt); }
            else {
                uint64_t *pd = phys_to_ptr(pdpt[PDPT_IDX(virt)] & ~0xFFFULL);
                tbl = pd; idx = PD_IDX(virt);
            }
        }
        tbl[idx] = (table_phys & ~0xFFFULL) | flags | PAGE_PRESENT;
    }

    __asm__ volatile ("pushq %0; popfq" : : "r"(rflags) : "memory");
    return level;
}

/*
 * Stage 6-pure — take one holder-retyped level back OUT of the walk.
 *
 * The counterpart of paging_install_table_in, and the reason teardown no
 * longer has to guess where a level came from: a VSpace detaches every table
 * it holds a capability to, and whatever is still hanging off the PML4 after
 * that is by construction kernel-carved and safe to hand to the PMM.
 *
 * `table_phys` is checked against the entry before it is cleared.  A record
 * that no longer describes the walk — the level was replaced, a huge page went
 * in, the parent is already gone — detaches nothing and says so, which is what
 * makes calling this on every table of a half-torn-down address space safe.
 */
int paging_detach_table_in(uint64_t cr3, uint64_t virt, int level,
                           uint64_t table_phys) {
    if (cr3 == 0 || table_phys == 0 || level < 1 || level > 3) return -1;

    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    int       rc  = -1;
    uint64_t *tbl = phys_to_ptr(cr3);
    uint64_t  idx = PML4_IDX(virt);

    for (int cur = 3; cur > level; cur--) {
        uint64_t e = tbl[idx];
        if (!(e & PAGE_PRESENT) || (e & PAGE_HUGE)) goto out;
        tbl = phys_to_ptr(e & ~0xFFFULL);
        idx = (cur == 3) ? PDPT_IDX(virt) : PD_IDX(virt);
    }
    if ((tbl[idx] & PAGE_PRESENT) &&
        !(tbl[idx] & PAGE_HUGE) &&
        (tbl[idx] & ~0xFFFULL) == (table_phys & ~0xFFFULL)) {
        tbl[idx] = 0;
        rc = 0;
    }
out:
    __asm__ volatile ("pushq %0; popfq" : : "r"(rflags) : "memory");
    /* No TLB shootdown: the only caller is address-space teardown, where this
     * CR3 is not loaded on any CPU and the next context switch reloads CR3
     * wholesale.  A future caller that detaches a level from a LIVE address
     * space owes a flush of the whole subtree, not one invlpg of `virt`. */
    return rc;
}

/*
 * A map that allocates NOTHING.  Returns -1 for a bad address space and the
 * missing level (1..3) when a table the holder must supply is absent, so the
 * caller can name it; 0 on success.
 */
int paging_map_strict_in(uint64_t cr3, uint64_t virt, uint64_t phys,
                         uint64_t flags) {
    if (cr3 == 0) return -1;

    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    int level = paging_missing_level_in(cr3, virt);
    if (level == 0) {
        uint64_t *pt = walk_pt(cr3, virt);
        if (pt) pt[PT_IDX(virt)] = (phys & ~0xFFFULL) | flags | PAGE_PRESENT;
        else    level = -1;
    }
    __asm__ volatile ("pushq %0; popfq" : : "r"(rflags) : "memory");
    return level;
}

static int paging_map_root(uint64_t root_phys, uint64_t virt, uint64_t phys,
                           uint64_t flags) {
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
    /* Kernel address space: its tables are kernel memory by definition. */
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

int paging_query_access(uint64_t virt, uint64_t *out_flags) {
    if (!out_flags) return -1;
    return paging_query_access_root(read_cr3_phys(), virt, out_flags);
}

void paging_init(uint64_t fb_phys, uint64_t fb_size) {
    uint64_t i;
    uint64_t text_phys_start   = (uint64_t)&__text_start;
    uint64_t rodata_phys_start = (uint64_t)&__rodata_start;
    uint64_t data_phys_start   = (uint64_t)&__data_start;
    uint64_t kernel_phys_end   = (uint64_t)&__kernel_end;

    /* NXE must be set in IA32_EFER before any NX-flagged PTE reaches the TLB. */
    paging_enable_nxe();

    pml4_phys = alloc_table();
    if (pml4_phys == 0) return;

    /* Pre-kernel identity map: 0 .. KERNEL_PHYS_BASE.
     * KERNEL_PHYS_BASE == HUGE_SIZE (2 MiB) → exactly one huge page; NX because
     * no kernel code executes from physical 0..2MiB. */
    for (i = 0; i < KERNEL_PHYS_BASE; i += HUGE_SIZE)
        paging_map_huge(i, i, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);

    /* Kernel identity map: per-section W^X with 4 KiB granularity.
     *   .text                   R-X   (executable, not writable)
     *   .rodata                 R--   (NX, not writable)
     *   .data + .bss            RW-   (NX, writable)  */
    for (i = text_phys_start; i < rodata_phys_start; i += PAGE_SIZE)
        paging_map(i, i, PAGE_PRESENT);
    for (i = rodata_phys_start; i < data_phys_start; i += PAGE_SIZE)
        paging_map(i, i, PAGE_PRESENT | PAGE_NX);
    for (i = data_phys_start; i < kernel_phys_end; i += PAGE_SIZE)
        paging_map(i, i, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);

    /* Post-kernel identity map: rounded-up-to-2MiB boundary .. IDENTITY_MAP_END. */
    uint64_t post_kernel = (kernel_phys_end + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);
    for (i = post_kernel; i < IDENTITY_MAP_END; i += HUGE_SIZE)
        paging_map_huge(i, i, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);

    /* Physmap window: full 4 GiB physical space, NX (data only). */
    for (i = 0; i < PHYS_WINDOW_END; i += HUGE_SIZE)
        paging_map_huge(PHYS_TO_VIRT(i), i, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);

    /* KERNEL_VIRT_TEXT alias: same per-section W^X as identity map above. */
    for (i = text_phys_start; i < rodata_phys_start; i += PAGE_SIZE)
        paging_map(KERNEL_VIRT_TEXT + (i - KERNEL_PHYS_BASE), i, PAGE_PRESENT);
    for (i = rodata_phys_start; i < data_phys_start; i += PAGE_SIZE)
        paging_map(KERNEL_VIRT_TEXT + (i - KERNEL_PHYS_BASE), i, PAGE_PRESENT | PAGE_NX);
    for (i = data_phys_start; i < kernel_phys_end; i += PAGE_SIZE)
        paging_map(KERNEL_VIRT_TEXT + (i - KERNEL_PHYS_BASE), i, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);

    /* Framebuffer: identity mapped, writable, NX (MMIO — no code). */
    if (fb_phys != 0 && fb_size != 0) {
        uint64_t fb_base = fb_phys & ~(HUGE_SIZE - 1);
        uint64_t fb_end  = (fb_phys + fb_size + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);
        for (i = fb_base; i < fb_end; i += HUGE_SIZE)
            paging_map_huge(i, i, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
    phys_window_ready = 1;

    /* Enable SMEP and SMAP if the CPU supports them (CPUID leaf 7, EBX).
     * SMEP (CR4[20]): prevents kernel from executing user-page code.
     * SMAP (CR4[21]): faults on kernel access to user pages without STAC.
     * Both checked independently so we get SMEP even on CPUs without SMAP. */
    {
        uint32_t ebx7 = 0;
        {
            uint32_t eax_r, ecx_r = 0, edx_r;
            __asm__ volatile ("cpuid"
                : "=a"(eax_r), "=b"(ebx7), "=c"(ecx_r), "=d"(edx_r)
                : "a"(7u), "c"(0u));
        }
        uint64_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        if (ebx7 & (1u << 7))  cr4 |= (1ULL << 20);   /* SMEP */
        if (ebx7 & (1u << 20)) {
            cr4 |= (1ULL << 21);                        /* SMAP */
            iris_smap_enabled = 1;
        }
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
}

/*
 * Stage 6-pure Step 4 — initialise a page the HOLDER supplied as a PML4.
 *
 * Split out of paging_create_user_space_from because the two halves answered
 * to different owners once an address space became something a holder retypes:
 * where the page comes from is the holder's business, and what has to be in it
 * before CR3 can point at it is the kernel's.  Nothing here allocates.
 *
 * What has to be in it: nothing of the previous occupant (a stale entry is a
 * walk into somebody else's memory), the shared low window so kernel code
 * remains addressable under this CR3, and the higher half, which every address
 * space shares with the kernel.
 */
void paging_init_user_pml4(uint64_t pml4_page_phys)
{
    if (pml4_page_phys == 0) return;
    uint64_t *kernel_pml4 = phys_to_ptr(pml4_phys);
    uint64_t *user_pml4   = phys_to_ptr(pml4_page_phys);

    for (uint64_t i = 0; i < ENTRIES; i++) user_pml4[i] = 0;
    user_pml4[USER_SHARED_PML4_INDEX] = kernel_pml4[USER_SHARED_PML4_INDEX];
    for (uint64_t i = 256; i < ENTRIES; i++) user_pml4[i] = kernel_pml4[i];
}

uint64_t paging_create_user_space(void)
{
    /* The PMM, and only the PMM.  Stage 6-pure Step 4 made a spawned
     * process's PML4 the storage of a KOBJ_VSPACE its holder retyped, so the
     * budget-funded variant this used to have had no callers left.  What comes
     * through here is the root task's address space and the kernel selftest's:
     * both built before any Untyped exists. */
    uint64_t new_pml4_phys = pmm_alloc_pages(1);
    if (new_pml4_phys == 0) return 0;
    paging_init_user_pml4(new_pml4_phys);
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

    /* Disable interrupts while modifying a foreign page table to keep the
     * map sequence atomic for bootstrap/rollback callers. */
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

int paging_query_access_in(uint64_t cr3, uint64_t virt, uint64_t *out_flags) {
    if (cr3 == 0 || !out_flags) return -1;
    return paging_query_access_root(cr3, virt, out_flags);
}

/* Phase 19 — local TLB invalidation counter (additive diagnostics).  Every
 * paging_unmap_in issues one invlpg on the current CPU; the count lets VM tests
 * confirm local invalidation happened (V21).  SMP shootdown is NOT implemented;
 * this counts local invalidations only (see vspace-frame-hardening.md). */
static _Atomic uint32_t paging_tlb_invlpg;

uint32_t paging_tlb_invalidate_count(void) {
    return __atomic_load_n(&paging_tlb_invlpg, __ATOMIC_RELAXED);
}

void paging_unmap_in(uint64_t cr3, uint64_t virt) {
    uint64_t *pt = walk_pt(cr3, virt);
    if (!pt) return;
    pt[PT_IDX(virt)] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    __atomic_fetch_add(&paging_tlb_invlpg, 1u, __ATOMIC_RELAXED);
}

void paging_destroy_user_space_from(uint64_t cr3, int pml4_pooled) {
    if (cr3 == 0) return;

    /*
     * Only the private window, and only what the kernel carved.
     *
     * Holder-retyped levels are gone from the walk by now
     * (kvspace_detach_tables), wherever in the address space they were
     * installed — so the single entry swept here is not a claim that user
     * mappings live only under this index, it is a claim about who allocated
     * what is still reachable.  alloc_table() is called for exactly two
     * address spaces (the kernel's, and the root task's pre-run maps) and
     * every page it produces hangs below USER_PRIVATE_PML4_INDEX.
     */
    uint64_t *pml4 = phys_to_ptr(cr3);
    uint64_t root = pml4[USER_PRIVATE_PML4_INDEX];
    if (root & PAGE_PRESENT) {
        destroy_pt_level(root & ~0xFFFULL, 3);
        pml4[USER_PRIVATE_PML4_INDEX] = 0;
    }

    /* Stage 6 Step 3: a pooled PML4 is inside somebody's Untyped — returning
     * it to the buddy allocator would hand out memory that region still owns.
     * Its child entry is returned by the VSpace along with the tables. */
    if (!pml4_pooled) pmm_free_page(cr3);
}

void paging_destroy_user_space(uint64_t cr3) {
    paging_destroy_user_space_from(cr3, 0);
}

/* paging_enable_pcid — enable CR4.PCIDE (Process Context Identifiers).
 * Called from kernel_main after paging_init().  No-op if the CPU lacks PCID
 * support (CPUID.01H:ECX[17]).  Sets iris_pcid_enabled = 1 on success so the
 * scheduler ORs the per-process PCID into CR3 loads. */
void paging_enable_pcid(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1u), "c"(0u));
    if (!((ecx >> 17) & 1u)) return;  /* CPUID.01H:ECX[17] = PCID feature */

    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 17);   /* CR4.PCIDE */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    iris_pcid_enabled = 1;
}
