#include <iris/kpage.h>
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/serial.h>
#include <stdint.h>

#define KPAGE_PAGE_SIZE 0x1000ULL
#define KPAGE_MAGIC     0x4B50414745554C4CULL /* "KPAGEULL" */

struct KPageHeader {
    uint64_t magic;
    uint64_t phys_base;
    uint32_t request_size;
    uint32_t page_count;   /* rounded up to next power-of-2 for buddy alignment */
};

#define KPAGE_HEADER_SIZE \
    ((uint32_t)((sizeof(struct KPageHeader) + 15u) & ~15u))

static uint32_t kpage_npages(uint32_t size) {
    return (size + (uint32_t)(KPAGE_PAGE_SIZE - 1)) / (uint32_t)KPAGE_PAGE_SIZE;
}

/* Round n up to the next power of two (n > 0). */
static uint32_t kpage_next_pow2(uint32_t n) {
    if (n <= 1u) return 1u;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
    return n + 1u;
}

static void kpage_panic(const char *msg) {
    serial_write("[IRIS][KPAGE] FATAL: ");
    serial_write(msg);
    serial_write("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

/*
 * kpage_alloc — allocate size bytes backed by contiguous PMM pages.
 *
 * Rounds the total allocation (header + size) up to the next power-of-two
 * page count so that it can be returned as a single buddy block on free.
 * All pages are zeroed before returning.
 */
void *kpage_alloc(uint32_t size) {
    if (size == 0) return 0;
    if (size > UINT32_MAX - KPAGE_HEADER_SIZE) return 0;

    uint32_t npages      = kpage_npages(size + KPAGE_HEADER_SIZE);
    uint32_t alloc_pages = kpage_next_pow2(npages); /* buddy block must be power-of-2 */
    uint64_t phys        = pmm_alloc_pages(alloc_pages);
    if (phys == 0) return 0;

    uint8_t *base  = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
    uint32_t total = alloc_pages * (uint32_t)KPAGE_PAGE_SIZE;
    for (uint32_t i = 0; i < total; i++) base[i] = 0;

    struct KPageHeader *hdr = (struct KPageHeader *)(void *)base;
    hdr->magic        = KPAGE_MAGIC;
    hdr->phys_base    = phys;
    hdr->request_size = size;
    hdr->page_count   = alloc_pages;   /* rounded count for pmm_free_contig */

    return (void *)(base + KPAGE_HEADER_SIZE);
}

/*
 * kpage_free — return pages to the PMM as a single buddy block.
 *
 * ptr must be the exact pointer returned by kpage_alloc.
 * size must be the exact size passed to kpage_alloc.
 */
void kpage_free(void *ptr, uint32_t size) {
    if (!ptr || size == 0) return;

    uint8_t *base = (uint8_t *)ptr - KPAGE_HEADER_SIZE;
    struct KPageHeader *hdr = (struct KPageHeader *)(void *)base;

    if (hdr->magic != KPAGE_MAGIC)
        kpage_panic("invalid allocation header");
    if (hdr->request_size != size)
        kpage_panic("size mismatch on free");
    if (hdr->page_count == 0)
        kpage_panic("corrupt page count");

    hdr->magic = 0;
    pmm_free_contig(hdr->phys_base, hdr->page_count);
}
