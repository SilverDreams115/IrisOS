#include <iris/pmm.h>
#include <iris/boot_info.h>

#define PMM_PAGE_SIZE  4096ULL
#define BITS_PER_ENTRY 64ULL
#define PMM_MAX_PAGES  (1024ULL * 1024ULL)
#define BITMAP_ENTRIES (PMM_MAX_PAGES / BITS_PER_ENTRY)

static uint64_t bitmap[BITMAP_ENTRIES];
static uint64_t total_pages_count = 0;
static uint64_t used_pages_count  = 0;
static uint64_t last_search_index = 0;

static inline void bitmap_set(uint64_t page_index) {
    bitmap[page_index / BITS_PER_ENTRY] |= (1ULL << (page_index % BITS_PER_ENTRY));
}
static inline void bitmap_clear(uint64_t page_index) {
    bitmap[page_index / BITS_PER_ENTRY] &= ~(1ULL << (page_index % BITS_PER_ENTRY));
}
static inline int bitmap_test(uint64_t page_index) {
    return (bitmap[page_index / BITS_PER_ENTRY] >> (page_index % BITS_PER_ENTRY)) & 1;
}

void pmm_init(struct iris_boot_info *boot_info) {
    uint64_t i;

    for (i = 0; i < BITMAP_ENTRIES; i++)
        bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;

    total_pages_count = 0;
    used_pages_count  = 0;
    last_search_index = 0;

    for (i = 0; i < boot_info->mmap_entry_count; i++) {
        struct iris_mmap_entry *entry = &boot_info->mmap[i];
        uint64_t page_index, page_count, j;

        if (entry->type != IRIS_MEM_USABLE) continue;
        if (entry->base == 0) continue;

        page_index = entry->base / PMM_PAGE_SIZE;
        page_count = entry->length / PMM_PAGE_SIZE;

        if (page_index >= PMM_MAX_PAGES) continue;
        if (page_index + page_count > PMM_MAX_PAGES)
            page_count = PMM_MAX_PAGES - page_index;

        for (j = 0; j < page_count; j++)
            bitmap_clear(page_index + j);

        total_pages_count += page_count;
    }

    bitmap_set(0);

    {
        uint64_t kernel_start_page = 0x200000 / PMM_PAGE_SIZE;
        uint64_t kernel_pages      = 0x200000 / PMM_PAGE_SIZE;
        uint64_t k;
        for (k = 0; k < kernel_pages; k++) {
            if (bitmap_test(kernel_start_page + k) == 0) {
                bitmap_set(kernel_start_page + k);
                used_pages_count++;
            }
        }
    }
}

uint64_t pmm_alloc_page(void) {
    uint64_t i;

    for (i = last_search_index; i < PMM_MAX_PAGES; i++) {
        if (bitmap_test(i) == 0) {
            bitmap_set(i);
            used_pages_count++;
            last_search_index = i + 1;
            return i * PMM_PAGE_SIZE;
        }
    }
    for (i = 1; i < last_search_index; i++) {
        if (bitmap_test(i) == 0) {
            bitmap_set(i);
            used_pages_count++;
            last_search_index = i + 1;
            return i * PMM_PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys_addr) {
    uint64_t page_index;
    if (phys_addr == 0) return;
    page_index = phys_addr / PMM_PAGE_SIZE;
    if (page_index >= PMM_MAX_PAGES) return;
    if (bitmap_test(page_index) == 0) return;
    bitmap_clear(page_index);
    if (used_pages_count > 0) used_pages_count--;
    if (page_index < last_search_index) last_search_index = page_index;
}

uint64_t pmm_total_pages(void) { return total_pages_count; }
uint64_t pmm_used_pages(void)  { return used_pages_count;  }
uint64_t pmm_free_pages(void)  { return total_pages_count - used_pages_count; }
