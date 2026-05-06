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
    uint32_t page_count;
};

#define KPAGE_HEADER_SIZE \
    ((uint32_t)((sizeof(struct KPageHeader) + 15u) & ~15u))

static uint32_t kpage_npages(uint32_t size) {
    return (size + (uint32_t)(KPAGE_PAGE_SIZE - 1)) / (uint32_t)KPAGE_PAGE_SIZE;
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
 * Rounds size up to the next page boundary, allocates from PMM, maps via the
 * physmap window (always accessible in the kernel's virtual address space),
 * and zeroes every byte before returning.  The zeroing is mandatory: callers
 * must not assume any particular prior content, and it prevents leaking freed
 * object state across allocations.
 */
void *kpage_alloc(uint32_t size) {
    if (size == 0) return 0;
    if (size > UINT32_MAX - KPAGE_HEADER_SIZE) return 0;

    uint32_t npages = kpage_npages(size + KPAGE_HEADER_SIZE);
    uint64_t phys = pmm_alloc_pages(npages);
    if (phys == 0) return 0;

    uint8_t *base = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
    uint32_t total = npages * (uint32_t)KPAGE_PAGE_SIZE;
    for (uint32_t i = 0; i < total; i++) base[i] = 0;

    struct KPageHeader *hdr = (struct KPageHeader *)(void *)base;
    hdr->magic        = KPAGE_MAGIC;
    hdr->phys_base    = phys;
    hdr->request_size = size;
    hdr->page_count   = npages;

    return (void *)(base + KPAGE_HEADER_SIZE);
}

/*
 * kpage_free — return pages to the PMM.
 *
 * ptr must be the exact pointer returned by kpage_alloc.
 * size must be the exact size passed to kpage_alloc.
 * Metadata is validated before any page is returned to PMM.
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
    for (uint32_t i = 0; i < hdr->page_count; i++)
        pmm_free_page(hdr->phys_base + (uint64_t)i * KPAGE_PAGE_SIZE);
}
