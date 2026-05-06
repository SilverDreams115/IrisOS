#ifndef IRIS_KPAGE_H
#define IRIS_KPAGE_H

#include <stdint.h>

/*
 * kpage — page-granular kernel object allocator.
 *
 * Replaces fixed BSS pool arrays for kernel objects whose size is in the
 * 1–64 KB range (KChannel, KProcess, KVmo, etc.).  Objects are allocated
 * directly from the PMM and returned as physmap-virtual pointers.
 *
 * Contract:
 *   - kpage_alloc zeros the allocated pages before returning.
 *   - kpage_free validates the allocation metadata before releasing pages.
 *   - ptr passed to kpage_free must be the pointer returned by kpage_alloc.
 *   - size passed to kpage_free must match the original request; a mismatch is
 *     treated as a kernel bug and triggers a fatal stop instead of UB.
 *   - Returns NULL on allocation failure (PMM exhausted).
 *
 * Not suitable for sub-page objects used in large numbers; use a slab cache
 * for those cases.  For kernel objects requiring whole-page backing this
 * removes the static pool ceiling entirely.
 */
void *kpage_alloc(uint32_t size);
void  kpage_free (void *ptr, uint32_t size);

#endif /* IRIS_KPAGE_H */
