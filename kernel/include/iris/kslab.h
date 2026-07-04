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

#endif /* IRIS_KSLAB_H */
