#ifndef IRIS_NC_KUNTYPED_H
#define IRIS_NC_KUNTYPED_H

#ifdef __KERNEL__
#include <stdint.h>
#include <stdatomic.h>
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>

#define KUNTYPED_ALIGN  64u   /* sub-allocation granularity (cache-line) */

struct KUntyped {
    struct KObject      base;        /* must be first */
    irq_spinlock_t      lock;        /* guards used (bump pointer) */
    uint64_t            phys_base;   /* physical start of managed region */
    uint64_t            total_size;  /* total bytes */
    uint64_t            used;        /* bump-pointer offset — advances monotonically */
    _Atomic uint32_t    child_count; /* live typed objects / sub-untypeds allocated from here */
    int                 is_device;   /* 0=normal RAM (zero-fill on bump), 1=device memory */
    struct KUntyped    *alloc_parent; /* non-NULL when this KUntyped was created via RETYPE */
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

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KUNTYPED_H */
