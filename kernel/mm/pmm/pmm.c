/*
 * pmm.c — Physical Memory Manager: bitmap + two-phase buddy allocator.
 *
 * Phase 1 (pmm_init → pmm_buddy_setup): O(N/64) bitmap scan using ctzll.
 * Phase 2 (after pmm_buddy_setup, called once post paging_init):
 *   O(log N) buddy allocator with power-of-2 blocks and coalescing.
 *   BuddyNode metadata is stored inline in free physical pages via PHYS_TO_VIRT,
 *   which is why pmm_buddy_setup requires the physmap to be active.
 *
 * Invariant (buddy mode):
 *   bitmap bit=0  ⟺  page is free  ⟺  page_blk_order[page_idx] > 0 for the
 *   block's start page (all interior pages have page_blk_order=0).
 */
#include <stdint.h>
#include <iris/pmm.h>
#include <iris/boot_info.h>
#include <iris/panic.h>
#include <iris/nc/spinlock.h>
#include <iris/paging.h>

#define PMM_PAGE_SIZE   4096ULL
#define BITS_PER_WORD   64ULL
#define MAX_ORDER       20
#define PMM_MAX_PAGES   (1024ULL * 1024ULL)   /* 4 GB / 4 KB = 1M pages */
#define BITMAP_WORDS    (PMM_MAX_PAGES / BITS_PER_WORD)  /* 16K uint64_t = 128 KB */

/* ── Canonical free/used state: bitmap (present in both phases) ─── */
static uint64_t bitmap[BITMAP_WORDS];

/* ── Buddy metadata (activated by pmm_buddy_setup) ─────────────── */

struct BuddyNode { struct BuddyNode *next; struct BuddyNode *prev; };

/* per-page: 0 = allocated (or interior of a free block);
 *           order+1 = this page is the START of a free block at that order */
static uint8_t page_blk_order[PMM_MAX_PAGES];   /* 1 MB BSS */

/* Doubly-linked free list sentinels, one per order (stored in BSS — not in page memory) */
static struct BuddyNode fl_heads[MAX_ORDER + 1];

static int buddy_active = 0;

/* ── Counters and lock ──────────────────────────────────────────── */
static uint64_t total_pages_count = 0;
static uint64_t used_pages_count  = 0;
static volatile uint64_t last_search_index = 0; /* pre-buddy hint */
static irq_spinlock_t pmm_lock;

extern char __kernel_start;
extern char __kernel_end;

/* ── Bitmap helpers ─────────────────────────────────────────────── */
static inline void bitmap_set(uint64_t i)   { bitmap[i/64] |= (1ULL << (i%64)); }
static inline void bitmap_clear(uint64_t i) { bitmap[i/64] &= ~(1ULL << (i%64)); }
static inline int  bitmap_test(uint64_t i)  { return (int)((bitmap[i/64] >> (i%64)) & 1); }

/* Set/clear a power-of-2 aligned block of pages at once (buddy-mode helpers). */
static void bitmap_set_block(uint64_t page_idx, uint32_t order) {
    uint64_t count = 1ULL << order;
    if (order >= 6u) {
        /* block is ≥64 pages and is 64-page aligned → operate word-by-word */
        for (uint64_t w = 0; w < (count >> 6); w++)
            bitmap[page_idx / 64 + w] = ~0ULL;
    } else {
        for (uint64_t i = 0; i < count; i++) bitmap_set(page_idx + i);
    }
}
static void bitmap_clear_block(uint64_t page_idx, uint32_t order) {
    uint64_t count = 1ULL << order;
    if (order >= 6u) {
        for (uint64_t w = 0; w < (count >> 6); w++)
            bitmap[page_idx / 64 + w] = 0ULL;
    } else {
        for (uint64_t i = 0; i < count; i++) bitmap_clear(page_idx + i);
    }
}

/* ── Buddy free-list helpers (called with pmm_lock held) ─────────── */
static void fl_init(struct BuddyNode *h)  { h->next = h->prev = h; }
static int  fl_empty(struct BuddyNode *h) { return h->next == h; }
static void fl_insert(struct BuddyNode *h, struct BuddyNode *n) {
    n->next = h->next; n->prev = h;
    h->next->prev = n; h->next = n;
}
static void fl_remove(struct BuddyNode *n) {
    n->prev->next = n->next; n->next->prev = n->prev;
    n->next = n->prev = n;
}
static struct BuddyNode *fl_pop(struct BuddyNode *h) {
    struct BuddyNode *n = h->next; fl_remove(n); return n;
}

/* Return a free block of 2^order pages to the buddy system (with coalescing).
 * Bitmap bits for the block must already be CLEAR before calling. */
static void buddy_release(uint64_t page_idx, uint32_t order) {
    while (order < MAX_ORDER) {
        uint64_t buddy_idx = page_idx ^ (1ULL << order);
        if (buddy_idx + (1ULL << order) > PMM_MAX_PAGES) break;
        if (page_blk_order[buddy_idx] != (uint8_t)(order + 1u)) break;
        /* Buddy is free at same order — coalesce into next order */
        struct BuddyNode *bn =
            (struct BuddyNode *)(uintptr_t)PHYS_TO_VIRT(buddy_idx * PMM_PAGE_SIZE);
        fl_remove(bn);
        page_blk_order[buddy_idx] = 0;
        if (buddy_idx < page_idx) page_idx = buddy_idx;
        order++;
    }
    struct BuddyNode *bn =
        (struct BuddyNode *)(uintptr_t)PHYS_TO_VIRT(page_idx * PMM_PAGE_SIZE);
    fl_insert(&fl_heads[order], bn);
    page_blk_order[page_idx] = (uint8_t)(order + 1u);
}

/* Allocate a block of exactly 2^order pages (with splitting).
 * Sets bitmap bits and updates used_pages_count on success.
 * Returns physical address, or 0 if no suitable block exists. */
static uint64_t buddy_claim(uint32_t order) {
    uint32_t avail = order;
    while (avail <= MAX_ORDER && fl_empty(&fl_heads[avail])) avail++;
    if (avail > MAX_ORDER) return 0;

    struct BuddyNode *bn = fl_pop(&fl_heads[avail]);
    uint64_t phys    = VIRT_TO_PHYS((uint64_t)(uintptr_t)bn);
    uint64_t pg      = phys / PMM_PAGE_SIZE;
    page_blk_order[pg] = 0;

    /* Split down to requested order, returning upper halves to free lists */
    while (avail > order) {
        avail--;
        uint64_t upper_pg = pg + (1ULL << avail);
        struct BuddyNode *up =
            (struct BuddyNode *)(uintptr_t)PHYS_TO_VIRT(upper_pg * PMM_PAGE_SIZE);
        fl_insert(&fl_heads[avail], up);
        page_blk_order[upper_pg] = (uint8_t)(avail + 1u);
    }

    bitmap_set_block(pg, order);
    used_pages_count += (1ULL << order);
    return phys;
}

/* ── Public API ─────────────────────────────────────────────────── */

void pmm_init(struct iris_boot_info *boot_info) {
    irq_spinlock_init(&pmm_lock);

    /* Mark everything as allocated; free ranges are cleared below */
    for (uint64_t i = 0; i < BITMAP_WORDS; i++) bitmap[i] = ~0ULL;
    total_pages_count = 0;
    used_pages_count  = 0;
    last_search_index = 0;

    for (uint64_t i = 0; i < boot_info->mmap_entry_count; i++) {
        struct iris_mmap_entry *entry = &boot_info->mmap[i];
        if (entry->type != IRIS_MEM_USABLE) continue;
        if (entry->base == 0) continue;

        uint64_t pg    = entry->base / PMM_PAGE_SIZE;
        uint64_t count = entry->length / PMM_PAGE_SIZE;
        if (pg >= PMM_MAX_PAGES) continue;
        if (pg + count > PMM_MAX_PAGES) count = PMM_MAX_PAGES - pg;

        for (uint64_t j = 0; j < count; j++) bitmap_clear(pg + j);
        total_pages_count += count;
    }

    bitmap_set(0);  /* physical page 0 always reserved */

    /* Reserve pages occupied by the kernel image */
    {
        uint64_t ks = (uint64_t)(uintptr_t)&__kernel_start;
        uint64_t ke = (uint64_t)(uintptr_t)&__kernel_end;
        uint64_t pg_s = ks / PMM_PAGE_SIZE;
        uint64_t pg_e = (ke + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        for (uint64_t k = pg_s; k < pg_e && k < PMM_MAX_PAGES; k++) {
            if (!bitmap_test(k)) { bitmap_set(k); used_pages_count++; }
        }
    }
}

/* Convert the bitmap into buddy free lists.  Must be called once, after
 * paging_init() has established the physmap window (PHYS_TO_VIRT). */
void pmm_buddy_setup(void) {
    for (uint32_t i = 0; i <= MAX_ORDER; i++) fl_init(&fl_heads[i]);

    /* Walk all free pages and decompose contiguous runs into buddy blocks */
    uint64_t pg = 1;
    while (pg < PMM_MAX_PAGES) {
        if (bitmap_test((uint64_t)pg)) { pg++; continue; }

        /* Found start of a free run; find its end */
        uint64_t run_start = pg;
        while (pg < PMM_MAX_PAGES && !bitmap_test(pg)) pg++;
        uint64_t remaining = pg - run_start;

        /* Decompose [run_start, run_start+remaining) into aligned buddy blocks */
        uint64_t pos = run_start;
        while (remaining > 0) {
            /* Largest order such that 2^order divides pos AND 2^order <= remaining */
            uint32_t ord = 0;
            while (ord + 1u <= MAX_ORDER &&
                   (pos & ((1ULL << (ord + 1u)) - 1u)) == 0 &&
                   (1ULL << (ord + 1u)) <= remaining)
                ord++;
            /* Insert block into free list (bitmap already clear for these pages) */
            struct BuddyNode *bn =
                (struct BuddyNode *)(uintptr_t)PHYS_TO_VIRT(pos * PMM_PAGE_SIZE);
            fl_insert(&fl_heads[ord], bn);
            page_blk_order[pos] = (uint8_t)(ord + 1u);
            pos       += 1ULL << ord;
            remaining -= 1ULL << ord;
        }
    }

    buddy_active = 1;
}

uint64_t pmm_alloc_page(void) {
    uint64_t result = 0;
    uint64_t saved  = irq_spinlock_lock(&pmm_lock);

    if (buddy_active) {
        result = buddy_claim(0);
    } else {
        /* O(N/64) bitmap scan with ctzll */
        uint64_t from_word = last_search_index / 64;
        for (uint64_t w = from_word; w < BITMAP_WORDS && !result; w++) {
            uint64_t free_mask = ~bitmap[w];
            if (!free_mask) continue;
            /* Skip page 0 in word 0 */
            if (w == 0) free_mask &= ~1ULL;
            if (!free_mask) continue;
            uint64_t bit = (uint64_t)__builtin_ctzll(free_mask);
            uint64_t idx = w * 64 + bit;
            if (idx >= PMM_MAX_PAGES) break;
            bitmap_set(idx);
            used_pages_count++;
            last_search_index = idx + 1;
            result = idx * PMM_PAGE_SIZE;
        }
        if (!result) {
            /* Wrap around */
            for (uint64_t w = 0; w < from_word && !result; w++) {
                uint64_t free_mask = ~bitmap[w];
                if (w == 0) free_mask &= ~1ULL;
                if (!free_mask) continue;
                uint64_t bit = (uint64_t)__builtin_ctzll(free_mask);
                uint64_t idx = w * 64 + bit;
                if (idx >= PMM_MAX_PAGES) break;
                bitmap_set(idx);
                used_pages_count++;
                last_search_index = idx + 1;
                result = idx * PMM_PAGE_SIZE;
            }
        }
    }

    irq_spinlock_unlock(&pmm_lock, saved);
    return result;
}

/* Round n up to next power of two.  n must be > 0. */
static uint32_t next_pow2(uint32_t n) {
    if (n <= 1u) return 1u;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
    return n + 1u;
}
static uint32_t ceil_log2(uint32_t n) {
    if (n <= 1u) return 0;
    uint32_t p = next_pow2(n);
    return (uint32_t)__builtin_ctz(p);
}

/* Allocate 2^ceil_log2(n) contiguous pages. */
uint64_t pmm_alloc_pages(uint32_t n) {
    if (n == 0) return 0;
    uint64_t result = 0;
    uint64_t saved  = irq_spinlock_lock(&pmm_lock);

    if (buddy_active) {
        result = buddy_claim(ceil_log2(n));
    } else {
        /* Pre-buddy: linear scan for n contiguous pages */
        uint64_t from = (last_search_index > 0) ? last_search_index : 1u;
        uint64_t start = 0, count = 0;

        for (uint64_t i = from; i < PMM_MAX_PAGES; i++) {
            if (!bitmap_test(i)) {
                if (count == 0) start = i;
                if (++count == (uint64_t)n) { goto found_pre; }
            } else { count = 0; }
        }
        count = 0;
        for (uint64_t i = 1; i < from; i++) {
            if (!bitmap_test(i)) {
                if (count == 0) start = i;
                if (++count == (uint64_t)n) { goto found_pre; }
            } else { count = 0; }
        }
        goto done_pre;
found_pre:
        for (uint64_t j = start; j < start + n; j++) { bitmap_set(j); used_pages_count++; }
        last_search_index = start + n;
        result = start * PMM_PAGE_SIZE;
done_pre:;
    }

    irq_spinlock_unlock(&pmm_lock, saved);
    return result;
}

uint64_t pmm_alloc_block(uint32_t *order_out) {
    if (!buddy_active) return 0;
    uint64_t saved = irq_spinlock_lock(&pmm_lock);
    uint64_t result = 0;

    /* Find the highest non-empty order */
    for (uint32_t ord = MAX_ORDER; ; ord--) {
        if (!fl_empty(&fl_heads[ord])) {
            result = buddy_claim(ord);
            if (order_out) *order_out = ord;
            break;
        }
        if (ord == 0) break;
    }

    irq_spinlock_unlock(&pmm_lock, saved);
    return result;
}

void pmm_free_page(uint64_t phys_addr) {
    if (phys_addr == 0) return;
    uint64_t pg = phys_addr / PMM_PAGE_SIZE;
    if (pg >= PMM_MAX_PAGES) return;

    uint64_t saved = irq_spinlock_lock(&pmm_lock);

    if (!bitmap_test(pg)) {
        irq_spinlock_unlock(&pmm_lock, saved);
        iris_panic("pmm_free_page: double-free detected");
    }

    bitmap_clear(pg);
    if (used_pages_count > 0) used_pages_count--;

    if (buddy_active) {
        buddy_release(pg, 0);
    } else {
        if (pg < last_search_index) last_search_index = pg;
    }

    irq_spinlock_unlock(&pmm_lock, saved);
}

void pmm_free_contig(uint64_t phys_base, uint32_t n) {
    if (phys_base == 0 || n == 0) return;
    uint64_t pg    = phys_base / PMM_PAGE_SIZE;
    uint32_t order = ceil_log2(n);
    uint64_t count = 1ULL << order;

    if (pg + count > PMM_MAX_PAGES) return;

    uint64_t saved = irq_spinlock_lock(&pmm_lock);

    if (buddy_active) {
        bitmap_clear_block(pg, order);
        if (used_pages_count >= count) used_pages_count -= count;
        buddy_release(pg, order);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            if (bitmap_test(pg + i)) {
                bitmap_clear(pg + i);
                if (used_pages_count > 0) used_pages_count--;
            }
        }
        if (pg < last_search_index) last_search_index = pg;
    }

    irq_spinlock_unlock(&pmm_lock, saved);
}

uint64_t pmm_total_pages(void) { return total_pages_count; }
uint64_t pmm_used_pages(void)  { return used_pages_count;  }
uint64_t pmm_free_pages(void)  { return total_pages_count - used_pages_count; }
