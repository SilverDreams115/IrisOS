/*
 * elf_loader.c — ELF64 static executable loader.
 *
 * Loads an ELF64 ET_EXEC binary from kernel memory (initrd) into a fresh
 * isolated address space.  Only PT_LOAD segments are processed.
 *
 * Security contract:
 *   - All offsets and sizes are bounds-checked against the provided image size.
 *   - All virtual addresses are validated to lie within the user private window.
 *   - The entry point must reside within a loaded segment.
 *   - On any failure every allocated page is freed before returning.
 */

#include <iris/elf_loader.h>
#include <iris/paging.h>
#include <iris/pmm.h>
#include <stdint.h>

/* ── ELF64 header / program header structs ─────────────────────────────── */

#define ELFMAG0  0x7Fu
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'
#define ELFCLASS64  2u
#define ELFDATA2LSB 1u     /* little-endian */
#define ET_EXEC     2u     /* static executable */
#define EM_X86_64   62u
#define PT_LOAD     1u
#define PF_X        1u     /* execute */
#define PF_W        2u     /* write */
#define PF_R        4u     /* read */
#define EI_MAG0     0u
#define EI_MAG1     1u
#define EI_MAG2     2u
#define EI_MAG3     3u
#define EI_CLASS    4u
#define EI_DATA     5u
#define EI_NIDENT   16u

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;     /* program header table offset */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize; /* size of one program header entry */
    uint16_t e_phnum;     /* number of program header entries */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;   /* offset in file */
    uint64_t p_vaddr;    /* virtual address */
    uint64_t p_paddr;    /* physical address (ignored) */
    uint64_t p_filesz;   /* size of segment in file */
    uint64_t p_memsz;    /* size of segment in memory */
    uint64_t p_align;
} Elf64_Phdr;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void elf_memzero(uint8_t *dst, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) dst[i] = 0;
}

static void elf_memcopy(uint8_t *dst, const uint8_t *src, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
}

/* Round up v to the next multiple of align (must be power of two). */
static uint64_t align_up(uint64_t v, uint64_t align) {
    return (v + align - 1u) & ~(align - 1u);
}

/* ── Validation ─────────────────────────────────────────────────────────── */

static int elf_check_header(const Elf64_Ehdr *eh, uint32_t size) {
    if (size < (uint32_t)sizeof(Elf64_Ehdr))           return 0;
    if (eh->e_ident[EI_MAG0] != ELFMAG0)               return 0;
    if (eh->e_ident[EI_MAG1] != ELFMAG1)               return 0;
    if (eh->e_ident[EI_MAG2] != ELFMAG2)               return 0;
    if (eh->e_ident[EI_MAG3] != ELFMAG3)               return 0;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64)            return 0;
    if (eh->e_ident[EI_DATA]  != ELFDATA2LSB)           return 0;
    if (eh->e_type    != ET_EXEC)                       return 0;
    if (eh->e_machine != EM_X86_64)                     return 0;
    if (eh->e_phentsize < (uint16_t)sizeof(Elf64_Phdr)) return 0;
    if (eh->e_phnum == 0)                               return 0;
    return 1;
}

/*
 * Check that a PT_LOAD segment's virtual address range falls within the
 * user private window and does not overflow.
 */
static int elf_vaddr_in_user_private(uint64_t vaddr, uint64_t memsz) {
    if (memsz == 0)                                     return 0;
    if (vaddr < USER_PRIVATE_BASE)                      return 0;
    /* check for overflow */
    if (vaddr + memsz < vaddr)                          return 0;
    if (vaddr + memsz > USER_PRIVATE_BASE + USER_PRIVATE_SIZE) return 0;
    /* must not overlap the user stack region */
    if (vaddr + memsz > USER_STACK_BASE)                return 0;
    return 1;
}

/* ── Segment loading ────────────────────────────────────────────────────── */

/*
 * Load a single PT_LOAD segment into cr3.
 *
 * Allocates ceil(p_memsz / PAGE_SIZE) pages, copies p_filesz bytes from
 * the ELF image, zero-fills the BSS tail, maps the pages with appropriate
 * permissions, and records the allocation in out->segs[seg_idx].
 *
 * On failure returns -1 and the caller must clean up via elf_loader_free_image.
 */
static int elf_load_segment(uint64_t cr3,
                            const uint8_t *elf_base,
                            uint32_t elf_size,
                            const Elf64_Phdr *ph,
                            iris_elf_image_t *out,
                            uint32_t seg_idx)
{
    uint64_t vaddr_page = ph->p_vaddr & ~0xFFFULL;
    uint64_t vaddr_end  = align_up(ph->p_vaddr + ph->p_memsz, 0x1000ULL);
    uint32_t page_count = (uint32_t)((vaddr_end - vaddr_page) / 0x1000ULL);

    /* bounds check: file data must be within the ELF image */
    if (ph->p_filesz > 0) {
        if (ph->p_offset > elf_size)             return -1;
        if (ph->p_offset + ph->p_filesz > elf_size) return -1;
    }

    /* allocate backing pages */
    uint64_t phys_base = pmm_alloc_pages(page_count);
    if (phys_base == 0)                          return -1;

    /* zero the entire allocation (covers BSS / padding) */
    uint8_t *kptr = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys_base);
    elf_memzero(kptr, page_count * 0x1000u);

    /* copy file data: may start at a sub-page offset within the first page */
    if (ph->p_filesz > 0) {
        uint32_t page_offset = (uint32_t)(ph->p_vaddr & 0xFFFULL);
        const uint8_t *src = elf_base + ph->p_offset;
        elf_memcopy(kptr + page_offset, src, (uint32_t)ph->p_filesz);
    }

    /* determine page flags from ELF segment permissions */
    uint64_t pg_flags = PAGE_PRESENT | PAGE_USER;
    if (ph->p_flags & PF_W) pg_flags |= PAGE_WRITABLE;
    if (!(ph->p_flags & PF_X)) pg_flags |= PAGE_NX;

    /* map each page into the new address space */
    for (uint32_t pg = 0; pg < page_count; pg++) {
        uint64_t virt = vaddr_page + (uint64_t)pg * 0x1000ULL;
        uint64_t phys = phys_base  + (uint64_t)pg * 0x1000ULL;
        if (paging_map_checked_in(cr3, virt, phys, pg_flags) != 0) {
            /* partial map: free this allocation; caller frees earlier ones */
            for (uint32_t f = 0; f < page_count; f++)
                pmm_free_page(phys_base + (uint64_t)f * 0x1000ULL);
            return -1;
        }
    }

    out->segs[seg_idx].phys_base  = phys_base;
    out->segs[seg_idx].page_count = page_count;
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

iris_error_t elf_loader_load(const void *data, uint32_t size,
                             iris_elf_image_t *out)
{
    const uint8_t   *elf_base = (const uint8_t *)data;
    const Elf64_Ehdr *eh;
    uint32_t seg_idx = 0;

    if (!data || size < (uint32_t)sizeof(Elf64_Ehdr) || !out)
        return IRIS_ERR_INVALID_ARG;

    /* zero the output struct — safe state for partial-failure cleanup */
    elf_memzero((uint8_t *)out, (uint32_t)sizeof(*out));

    eh = (const Elf64_Ehdr *)elf_base;

    if (!elf_check_header(eh, size))
        return IRIS_ERR_INVALID_ARG;

    if (eh->e_phnum > ELF_LOADER_MAX_LOAD_SEGS)
        return IRIS_ERR_OVERFLOW;

    /* bounds check program header table */
    {
        uint64_t ph_table_end = eh->e_phoff +
            (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
        if (ph_table_end > size)
            return IRIS_ERR_INVALID_ARG;
    }

    /* create the new address space */
    uint64_t cr3 = paging_create_user_space();
    if (cr3 == 0)
        return IRIS_ERR_NO_MEMORY;

    out->cr3_phys = cr3;

    /* walk PT_LOAD segments */
    int entry_covered = 0;
    for (uint32_t i = 0; i < (uint32_t)eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (elf_base + eh->e_phoff + (uint64_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;

        if (!elf_vaddr_in_user_private(ph->p_vaddr, ph->p_memsz))
            goto fail;

        if (ph->p_filesz > ph->p_memsz)
            goto fail;

        if (seg_idx >= ELF_LOADER_MAX_LOAD_SEGS)
            goto fail;

        if (elf_load_segment(cr3, elf_base, size, ph, out, seg_idx) != 0)
            goto fail;

        /* check if entry point falls within this segment */
        if (eh->e_entry >= ph->p_vaddr &&
            eh->e_entry <  ph->p_vaddr + ph->p_memsz)
            entry_covered = 1;

        seg_idx++;
    }

    if (seg_idx == 0)
        goto fail;  /* no loadable segments */

    if (!entry_covered)
        goto fail;  /* entry point outside all loaded segments */

    out->seg_count   = seg_idx;
    out->entry_vaddr = eh->e_entry;
    return IRIS_OK;

fail:
    elf_loader_free_image(out);
    return IRIS_ERR_INVALID_ARG;
}

void elf_loader_free_image(iris_elf_image_t *img) {
    if (!img) return;

    for (uint32_t i = 0; i < img->seg_count; i++) {
        for (uint32_t pg = 0; pg < img->segs[i].page_count; pg++) {
            pmm_free_page(img->segs[i].phys_base + (uint64_t)pg * 0x1000ULL);
        }
    }

    if (img->cr3_phys != 0) {
        paging_destroy_user_space(img->cr3_phys);
    }

    elf_memzero((uint8_t *)img, (uint32_t)sizeof(*img));
}
