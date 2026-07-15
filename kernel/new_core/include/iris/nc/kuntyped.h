#ifndef IRIS_NC_KUNTYPED_H
#define IRIS_NC_KUNTYPED_H

#ifdef __KERNEL__
#include <stdint.h>
#include <stdatomic.h>
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>

#define KUNTYPED_ALIGN  64u   /* sub-allocation granularity (cache-line) */

/* Fase S1: hard bounds for one SYS_UNTYPED_RETYPE2 batch — the whole batch is
 * carved, initialized and published under the untyped lock, so it must stay
 * small enough to keep the IRQ-off window short. */
#define KUNTYPED_RETYPE_MAX_COUNT  32u
#define KUNTYPED_RETYPE_MAX_BYTES  (128u * 1024u)

struct KUntyped {
    struct KObject      base;        /* must be first */
    irq_spinlock_t      lock;        /* guards used (bump pointer) */
    uint64_t            phys_base;   /* physical start of managed region */
    uint64_t            total_size;  /* total bytes */
    uint64_t            used;        /* bump-pointer offset — advances monotonically */
    _Atomic uint32_t    child_count; /* live typed objects / sub-untypeds allocated from here */
    int                 is_device;   /* 0=normal RAM (zero-fill on bump), 1=device memory */
    struct KUntyped    *alloc_parent; /* non-NULL when this KUntyped was created via RETYPE */
    uint64_t            generation;  /* Fase S1: bumped on every successful RESET —
                                      * a reused region never shares a generation
                                      * with the objects that lived there before */
};

/* Create a KUntyped covering [phys_base, phys_base+size).
 * Caller must have already removed the physical region from the PMM.
 * Returns NULL if the KUntyped header itself cannot be kpage_alloc'd. */
struct KUntyped *kuntyped_create(uint64_t phys_base, uint64_t size, int is_device);

/* Drop the owner reference; frees the KUntyped header (physical region NOT freed). */
void kuntyped_destroy_ref(struct KUntyped *u);

/* Carve 'bytes' (rounded up to KUNTYPED_ALIGN) from the bump pointer.
 * Zero-fills RAM regions before returning.
 * Returns kernel-virtual pointer on success, NULL if insufficient space. */
void    *kuntyped_bump_alloc(struct KUntyped *u, uint64_t bytes);

/* Like kuntyped_bump_alloc but returns the physical address of the carved region.
 * Does NOT zero-fill (sub-untyped's own allocator zeroes lazily at use time). */
uint64_t kuntyped_bump_alloc_phys(struct KUntyped *u, uint64_t bytes);

/*
 * Ph79: parent-tracked typed-object allocation.
 *
 * kuntyped_alloc_child: carve KUNTYPED_ALIGN + obj_bytes from the bump pointer.
 *   The first KUNTYPED_ALIGN bytes store a parent back-pointer; the remaining
 *   obj_bytes (aligned) are the object's memory.  Returns a pointer to the
 *   object area (NOT the block start).  Increments child_count and retains
 *   a reference on the parent to keep it alive.
 *
 * kuntyped_release_child: call from _destroy_ut callbacks.
 *   Backs up to the header, reads the parent pointer, zeroes the entire block,
 *   then decrements child_count and releases the parent reference.
 *   obj_bytes must match what was passed to kuntyped_alloc_child.
 */
void *kuntyped_alloc_child(struct KUntyped *u, uint64_t obj_bytes);
void  kuntyped_release_child(void *obj_ptr, uint64_t obj_bytes);

uint64_t kuntyped_available(struct KUntyped *u);

/* Carve a PAGE_SIZE-aligned physical region of 'size' bytes from the bump
 * pointer.  Rounds up the current 'used' offset to the next PAGE_SIZE boundary
 * before carving.  'size' must be > 0 and a multiple of PAGE_SIZE (4096).
 * Returns physical base address, or 0 on insufficient space or bad alignment. */
uint64_t kuntyped_bump_alloc_phys_page(struct KUntyped *u, uint64_t size);

/* Fase 18: live KUntyped object count (additive diagnostics). */
uint32_t kuntyped_live_count(void);

/*
 * Fase S1: atomic batch carve for SYS_UNTYPED_RETYPE2.
 *
 * Carves 'count' child blocks of (KUNTYPED_ALIGN + align_up(obj_bytes)) each
 * from the bump pointer in ONE critical section: capacity is checked for the
 * whole batch before any byte is consumed, every block is zero-filled, the
 * parent back-pointer is written, child_count += count and one parent retain
 * per child is taken.  On failure nothing is consumed (U14/U15).
 *
 * out_ptrs[i] receives the object area of child i (block + KUNTYPED_ALIGN).
 * Children are destroyed individually via kuntyped_release_child.
 */
iris_error_t kuntyped_alloc_children_atomic(struct KUntyped *u,
                                            uint64_t obj_bytes,
                                            uint32_t count,
                                            void **out_ptrs);

/* Fase S1: exact rollback of a batch that could not be published.  Only
 * succeeds when no later carve happened (used == the batch end); the caller
 * must already have released every child (child_count decremented). */
void kuntyped_unbump_exact(struct KUntyped *u, uint64_t start_used,
                           uint64_t end_used);

/* Fase S1 instrumentation — global untyped/retype counters (testable via
 * SYS_UNTYPED_QUERY).  All monotonic except live gauges. */
struct kuntyped_stats {
    uint64_t retype_count;        /* successful RETYPE/RETYPE2 object creations */
    uint64_t retype_failures;     /* validation/capacity denials */
    uint64_t reset_count;         /* successful SYS_UNTYPED_RESET operations */
    uint64_t reclaimed_bytes;     /* bytes returned to reusable state by RESET */
    uint64_t reuse_count;         /* RESETs of a region that had been consumed */
    uint64_t overlap_denials;     /* batch/capacity/occupied-slot denials */
};
void kuntyped_stats_get(struct kuntyped_stats *out);
void kuntyped_stat_retype(uint64_t objects);
void kuntyped_stat_retype_failure(void);
void kuntyped_stat_overlap_denial(void);
void kuntyped_stat_reset(uint64_t reclaimed, int was_used);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KUNTYPED_H */
