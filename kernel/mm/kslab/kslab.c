#include <iris/kslab.h>
#include <iris/paging.h>
#include <iris/nc/spinlock.h>
#include <stdint.h>

/*
 * 11 power-of-2 size classes: 2^5 (32) through 2^15 (32768).
 * The largest class (32768) covers struct KProcess (~26KB with embedded HandleTable).
 */
#define KSLAB_MIN_LOG2   5u    /* 32 bytes  */
#define KSLAB_MAX_LOG2  15u    /* 32768 bytes */
#define KSLAB_NUM_CLASSES (KSLAB_MAX_LOG2 - KSLAB_MIN_LOG2 + 1u)  /* 11 */

static uint8_t        *kslab_base  = 0;
static uint32_t        kslab_total = 0;
static uint32_t        kslab_used  = 0;   /* bump pointer (byte offset) */
static void           *kslab_free_heads[KSLAB_NUM_CLASSES];
static irq_spinlock_t  kslab_lock;

/* Round size up to the next power-of-2 ≥ 2^KSLAB_MIN_LOG2. */
static uint32_t kslab_round(uint32_t size) {
    if (size <= (1u << KSLAB_MIN_LOG2)) return KSLAB_MIN_LOG2;
    /* Compute ceil_log2(size) using CLZ */
    uint32_t log2 = 32u - (uint32_t)__builtin_clz(size - 1u);
    if (log2 < KSLAB_MIN_LOG2) log2 = KSLAB_MIN_LOG2;
    return log2;
}

void kslab_init(uint64_t phys_base, uint32_t num_pages) {
    kslab_base  = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys_base);
    kslab_total = num_pages * 4096u;
    kslab_used  = 0u;
    irq_spinlock_init(&kslab_lock);
    for (uint32_t i = 0u; i < KSLAB_NUM_CLASSES; i++)
        kslab_free_heads[i] = 0;
    /* Zero the backing region so bump-allocated objects start clean. */
    for (uint32_t i = 0u; i < kslab_total; i++) kslab_base[i] = 0;
}

void *kslab_alloc(uint32_t size) {
    if (!size || !kslab_base) return 0;

    uint32_t log2 = kslab_round(size);
    if (log2 > KSLAB_MAX_LOG2) return 0;
    uint32_t block  = 1u << log2;
    uint32_t idx    = log2 - KSLAB_MIN_LOG2;
    void    *ptr    = 0;

    uint64_t flags = irq_spinlock_lock(&kslab_lock);

    if (kslab_free_heads[idx]) {
        /* Pop from free-list */
        ptr = kslab_free_heads[idx];
        kslab_free_heads[idx] = *(void **)ptr;
    } else {
        /* Bump-allocate, aligned to block boundary */
        uint32_t aligned = (kslab_used + block - 1u) & ~(block - 1u);
        if (aligned + block > kslab_total) {
            irq_spinlock_unlock(&kslab_lock, flags);
            return 0;
        }
        ptr        = (void *)(kslab_base + aligned);
        kslab_used = aligned + block;
    }

    irq_spinlock_unlock(&kslab_lock, flags);

    /* Zero-fill (bump region is pre-zeroed; free-list reuse needs explicit zero). */
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0u; i < block; i++) p[i] = 0;

    return ptr;
}

void kslab_free(void *ptr, uint32_t size) {
    if (!ptr || !size || !kslab_base) return;
    /* Bounds check: must be within the backing region. */
    if ((uint8_t *)ptr < kslab_base || (uint8_t *)ptr >= kslab_base + kslab_total) return;

    uint32_t log2 = kslab_round(size);
    if (log2 > KSLAB_MAX_LOG2) return;
    uint32_t idx = log2 - KSLAB_MIN_LOG2;

    uint64_t flags = irq_spinlock_lock(&kslab_lock);
    *(void **)ptr          = kslab_free_heads[idx];
    kslab_free_heads[idx]  = ptr;
    irq_spinlock_unlock(&kslab_lock, flags);
}
