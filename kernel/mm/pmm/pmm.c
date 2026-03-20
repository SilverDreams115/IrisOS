#include "pmm.h"
#include <iris/boot_info.h>

/*
 * PMM — bitmap allocator
 *
 * Una página = 4096 bytes = 1 bit en el bitmap.
 * Bit 0 = libre, Bit 1 = ocupada.
 *
 * El bitmap vive en la primera región USABLE suficientemente grande.
 * Direcciones físicas solamente — sin paging todavía.
 */

#define PMM_PAGE_SIZE  4096ULL
#define BITS_PER_ENTRY 64ULL

/* máximo de RAM que podemos rastrear: 4 GB con páginas de 4K = 1M páginas */
#define PMM_MAX_PAGES (1024ULL * 1024ULL)

/* bitmap: 1M bits = 128 KB */
#define BITMAP_ENTRIES (PMM_MAX_PAGES / BITS_PER_ENTRY)

static uint64_t bitmap[BITMAP_ENTRIES];
static uint64_t total_pages_count = 0;
static uint64_t used_pages_count  = 0;
static uint64_t last_search_index = 0;

/* ── bitmap helpers ─────────────────────────────────────────────────── */

static inline void bitmap_set(uint64_t page_index) {
    bitmap[page_index / BITS_PER_ENTRY] |= (1ULL << (page_index % BITS_PER_ENTRY));
}

static inline void bitmap_clear(uint64_t page_index) {
    bitmap[page_index / BITS_PER_ENTRY] &= ~(1ULL << (page_index % BITS_PER_ENTRY));
}

static inline int bitmap_test(uint64_t page_index) {
    return (bitmap[page_index / BITS_PER_ENTRY] >> (page_index % BITS_PER_ENTRY)) & 1;
}

/* ── init ───────────────────────────────────────────────────────────── */

void pmm_init(struct iris_boot_info *boot_info) {
    uint64_t i;

    /* marcar todo como ocupado por defecto */
    for (i = 0; i < BITMAP_ENTRIES; i++)
        bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;

    total_pages_count = 0;
    used_pages_count  = 0;
    last_search_index = 0;

    /* liberar regiones USABLE del memory map */
    for (i = 0; i < boot_info->mmap_entry_count; i++) {
        struct iris_mmap_entry *entry = &boot_info->mmap[i];
        uint64_t page_index, page_count, j;

        if (entry->type != IRIS_MEM_USABLE)
            continue;

        /* ignorar página cero — nunca debe ser allocable */
        if (entry->base == 0)
            continue;

        page_index = entry->base / PMM_PAGE_SIZE;
        page_count = entry->length / PMM_PAGE_SIZE;

        /* no sobrepasar el límite del bitmap */
        if (page_index >= PMM_MAX_PAGES)
            continue;
        if (page_index + page_count > PMM_MAX_PAGES)
            page_count = PMM_MAX_PAGES - page_index;

        for (j = 0; j < page_count; j++)
            bitmap_clear(page_index + j);

        total_pages_count += page_count;
    }

    /*
     * Marcar la primera página física (0x0000) como ocupada.
     * Marcar el rango del kernel como ocupado (0x200000 — 0x400000).
     * El kernel está en 0x200000 según el linker.ld.
     */
    bitmap_set(0);

    {
        uint64_t kernel_start_page = 0x200000 / PMM_PAGE_SIZE;
        uint64_t kernel_pages      = 0x200000 / PMM_PAGE_SIZE; /* 2 MB reservados */
        uint64_t k;
        for (k = 0; k < kernel_pages; k++) {
            if (bitmap_test(kernel_start_page + k) == 0) {
                bitmap_set(kernel_start_page + k);
                used_pages_count++;
            }
        }
    }

    /*
     * Marcar el bitmap mismo como ocupado.
     * El bitmap vive en BSS del kernel — ya cubierto por el rango anterior.
     * Si no, calcularlo explícitamente.
     */
}

/* ── alloc ──────────────────────────────────────────────────────────── */

uint64_t pmm_alloc_page(void) {
    uint64_t i;

    /* búsqueda desde last_search_index para amortizar el costo */
    for (i = last_search_index; i < PMM_MAX_PAGES; i++) {
        if (bitmap_test(i) == 0) {
            bitmap_set(i);
            used_pages_count++;
            last_search_index = i + 1;
            return i * PMM_PAGE_SIZE;
        }
    }

    /* wrap-around: buscar desde el inicio */
    for (i = 1; i < last_search_index; i++) {
        if (bitmap_test(i) == 0) {
            bitmap_set(i);
            used_pages_count++;
            last_search_index = i + 1;
            return i * PMM_PAGE_SIZE;
        }
    }

    /* sin memoria disponible */
    return 0;
}

/* ── free ───────────────────────────────────────────────────────────── */

void pmm_free_page(uint64_t phys_addr) {
    uint64_t page_index;

    if (phys_addr == 0)
        return;

    page_index = phys_addr / PMM_PAGE_SIZE;

    if (page_index >= PMM_MAX_PAGES)
        return;

    if (bitmap_test(page_index) == 0)
        return; /* doble free — ignorar */

    bitmap_clear(page_index);
    if (used_pages_count > 0)
        used_pages_count--;

    if (page_index < last_search_index)
        last_search_index = page_index;
}

/* ── stats ──────────────────────────────────────────────────────────── */

uint64_t pmm_total_pages(void) { return total_pages_count; }
uint64_t pmm_used_pages(void)  { return used_pages_count;  }
uint64_t pmm_free_pages(void)  { return total_pages_count - used_pages_count; }
