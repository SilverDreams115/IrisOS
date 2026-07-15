#ifndef IRIS_KSLAB_H
#define IRIS_KSLAB_H

#include <stdint.h>

/*
 * kslab — power-of-2 slab allocator for kernel objects.
 *
 * Backed by a single contiguous physical region reserved from the PMM at boot.
 * All typed kernel object headers (KProcess, KEndpoint, KNotification, ...) allocate
 * through kslab_alloc instead of the PMM directly, allowing the PMM to be fully
 * drained into user-visible KUntyped caps.
 *
 * Size classes: 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768.
 * kslab_alloc rounds up to the next class; kslab_free returns to the same class.
 *
 * kslab_init must be called once after pmm_buddy_setup and before any kobject
 * allocation.  Returned memory is zeroed (matching kpage_alloc semantics).
 */
void  kslab_init (uint64_t phys_base, uint32_t num_pages);
void *kslab_alloc(uint32_t size);
void  kslab_free (void *ptr, uint32_t size);

/* Fase 29 — capacity-contract observability (see
 * docs/architecture/kernel-capacity-limits.md).  used == bump high-water (the
 * bump pointer never retreats; frees are reused via per-class free-lists);
 * fail counts allocations that hit the arena ceiling. */
uint32_t kslab_used_bytes(void);
uint32_t kslab_total_bytes(void);
uint32_t kslab_fail_count(void);

#endif /* IRIS_KSLAB_H */
