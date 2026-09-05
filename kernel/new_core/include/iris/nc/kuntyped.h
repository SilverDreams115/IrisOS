#ifndef IRIS_NC_KUNTYPED_H
#define IRIS_NC_KUNTYPED_H

#ifdef __KERNEL__
#include <stdint.h>
#include <stdatomic.h>
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>

#define KUNTYPED_ALIGN  64u   /* sub-allocation granularity (cache-line) */

/* Phase S1: hard bounds for one SYS_UNTYPED_RETYPE2 batch — the whole batch is
 * carved, initialized and published under the untyped lock, so it must stay
 * small enough to keep the IRQ-off window short. */
#define KUNTYPED_RETYPE_MAX_COUNT  32u
#define KUNTYPED_RETYPE_MAX_BYTES  (128u * 1024u)

struct KUntyped {
    struct KObject      base;        /* must be first */
    irq_spinlock_t      lock;        /* guards both bump pointers */
    uint64_t            phys_base;   /* physical start of managed region */
    uint64_t            total_size;  /* total bytes */
    uint64_t            used;        /* bottom bump — advances monotonically */
    /* Stage 6 Step 1: the TOP bump, growing down from phys_base+total_size.
     * Object HEADERS are carved here so that they never perturb the page
     * alignment the bottom carve depends on: a 64-byte header taken from the
     * bottom would push the next page-aligned carve to the following page and
     * waste almost 4 KiB per frame.  The two ends meet exactly once — every
     * bounds check is `used + used_top + need <= total_size`. */
    uint64_t            used_top;
    _Atomic uint32_t    child_count; /* live typed objects / sub-untypeds allocated from here */
    int                 is_device;   /* 0=normal RAM (zero-fill on bump), 1=device memory */
    struct KUntyped    *alloc_parent; /* non-NULL when this KUntyped was created via RETYPE */
    uint64_t            generation;  /* Phase S1: bumped on every successful RESET —
                                      * a reused region never shares a generation
                                      * with the objects that lived there before */
    /*
     * Where the HEADERS of objects retyped out of a DEVICE region come from
     * (ledger D-9).  NULL for RAM Untypeds, which carve their own.
     *
     * A device region is MMIO.  The kernel can hand it out and a driver can
     * map it, but nothing can be stored in it: a `struct KFrame` written into
     * a framebuffer is pixels, and read back it is whatever the display
     * controller left there.  So a device retype needs RAM from somewhere, and
     * the only answer that does not put the kernel back in the business of
     * allocating on somebody's behalf is that the HOLDER names it.
     *
     * Set once, by `SYS_UNTYPED_SET_DEVICE_BUDGET`, and retained while set —
     * so the RAM Untyped cannot be RESET out from under headers that describe
     * live device frames.  A device Untyped with none refuses to retype, which
     * is the honest failure: the kernel does not know whose memory to spend
     * and will not guess.
     */
    struct KUntyped    *hdr_budget;
};

/* Create a KUntyped covering [phys_base, phys_base+size).
 * Caller must have already removed the physical region from the PMM.
 * Returns NULL if the KUntyped header itself cannot be kpage_alloc'd. */
struct KUntyped *kuntyped_create(uint64_t phys_base, uint64_t size, int is_device);

/* D-9: name the RAM Untyped that pays for the headers of objects retyped out
 * of this DEVICE Untyped.  Refuses on a RAM Untyped, on a device budget, and
 * on a second call — a pairing that could move would let a holder strand the
 * headers of live objects in a region it then reset. */
iris_error_t kuntyped_set_hdr_budget(struct KUntyped *dev, struct KUntyped *ram);

/* Stage 6 Step 4 — placement-init a sub-untyped whose header is a child block
 * of its parent (kuntyped_alloc_child_top).  The block carries the child_count
 * entry and the parent retain, so `alloc_parent` stays NULL. */
struct KUntyped *kuntyped_create_at(void *mem, uint64_t phys_base,
                                    uint64_t size, int is_device);

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

/*
 * Stage 6 Step 1: same contract as kuntyped_alloc_child — parent back-pointer
 * in the block header, child_count incremented, parent retained, released by
 * kuntyped_release_child — but carved from the TOP of the region.
 *
 * For objects that describe a page-aligned region carved from the same Untyped
 * (a Frame today, a page table next), so that paying for the header does not
 * cost a page of alignment.  The release path is direction-agnostic: it reads
 * the parent out of the block and never rewinds either pointer.
 */
void *kuntyped_alloc_child_top(struct KUntyped *u, uint64_t obj_bytes);

/* The Untyped a child block belongs to, without releasing anything.  Reading
 * it before release is the only way to know who to drop a pool retain on,
 * because release zeroes the block that records it. */
struct KUntyped *kuntyped_child_parent(const void *obj_ptr);

void  kuntyped_release_child(void *obj_ptr, uint64_t obj_bytes);

/* Stage 6 Step 2 — a page-aligned carve with child accounting and no object
 * header: page tables.  They have no KObject, but they ARE derived memory, so
 * they hold a child_count entry (and a parent retain) for as long as the
 * address space that installed them lives.  That is what makes RESET refuse
 * while somebody's page tables are still inside the region. */
uint64_t kuntyped_alloc_page_child(struct KUntyped *u);
/* A CONTIGUOUS multi-page carve holding ONE child entry — for arrays the
 * kernel indexes, such as a VMO's page table of physical addresses. */
uint64_t kuntyped_alloc_pages_child(struct KUntyped *u, uint64_t bytes);
void     kuntyped_release_page_child(struct KUntyped *u);

uint64_t kuntyped_available(struct KUntyped *u);

/* Carve a PAGE_SIZE-aligned physical region of 'size' bytes from the bump
 * pointer.  Rounds up the current 'used' offset to the next PAGE_SIZE boundary
 * before carving.  'size' must be > 0 and a multiple of PAGE_SIZE (4096).
 * Returns physical base address, or 0 on insufficient space or bad alignment. */
uint64_t kuntyped_bump_alloc_phys_page(struct KUntyped *u, uint64_t size);

/* Phase 18: live KUntyped object count (additive diagnostics). */
uint32_t kuntyped_live_count(void);

/*
 * Phase S1: atomic batch carve for SYS_UNTYPED_RETYPE2.
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

/* Phase S1: exact rollback of a batch that could not be published.  Only
 * succeeds when no later carve happened (used == the batch end); the caller
 * must already have released every child (child_count decremented). */
void kuntyped_unbump_exact(struct KUntyped *u, uint64_t start_used,
                           uint64_t end_used);

/* Phase S1 instrumentation — global untyped/retype counters (testable via
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
