#ifndef IRIS_NC_KVSPACE_H
#define IRIS_NC_KVSPACE_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

/*
 * KVSpace — VSpace capability object (Fase 6).
 *
 * Represents formal authority over a process address space.
 * KProcess holds the page tables; KVSpace records cr3 and an invalidation
 * flag so capability holders can detect a reaped address space without
 * dangling pointers.
 *
 * Ownership model:
 *   - KProcess holds one lifecycle ref (kobject_retain).
 *   - Each CNode slot holds lifecycle + active ref via kcnode_mint.
 *   - Page tables are owned by KProcess; KVSpace does NOT free them.
 *   - kvspace_invalidate() zeroes cr3, clears valid, and auto-unmaps all
 *     KFrame mappings before page table reap (Fase 6).
 *
 * KFrame back-reference model (Fase 6 / 6.3):
 *   - kframe_map_page() allocates a KFrameMapping node (kslab), retains the
 *     frame, and prepends the node to the singly-linked mappings list.
 *   - kframe_unmap_page() finds the node by (frame,va), unlinks it, frees it,
 *     and releases the frame retain.
 *   - kvspace_invalidate() takes the entire mappings list out under the lock,
 *     then for each node: calls paging_unmap_in, frees the node, decrements
 *     frame->mapped_count, and releases the frame retain so that
 *     kframe_obj_destroy() always sees mapped_count == 0.
 *
 * The mapping list is dynamically allocated (no fixed slot ceiling) so that
 * runtime VMO maps can coexist with bootstrap KFrame maps without overflowing
 * a compile-time constant.
 */

/* Forward declaration — full definition in iris/nc/kframe.h. */
struct KFrame;

struct KFrameMapping {
    struct KFrame        *frame;   /* non-NULL = slot live */
    uint64_t              user_va;
    struct KFrameMapping *next;    /* next node in singly-linked list; NULL = end */
};

struct KVSpace {
    struct KObject        base;          /* must be first */
    spinlock_t            lock;          /* guards all fields below */
    uint64_t              cr3;           /* physical PML4; 0 after invalidation */
    int                   valid;         /* 1 = live, 0 = reaped */
    uint32_t              mapping_count; /* number of live KFrameMapping nodes */
    struct KFrameMapping *mappings;      /* singly-linked list head; NULL = empty */
};

/* Allocate a new KVSpace wrapping the given cr3. Returns NULL on OOM.
 * Caller holds the alloc lifecycle ref (refcount=1, active_refs=0) on return. */
struct KVSpace *kvspace_alloc(uint64_t cr3);

/* Mark the VSpace invalid and zero cr3.  Called by kprocess_reap_address_space
 * before paging_destroy_user_space so no capability holder can read a freed cr3. */
void kvspace_invalidate(struct KVSpace *vs);

/* Unmap the page at user_va from vs.  Finds the KFrameMapping node by VA,
 * removes the PTE, decrements frame->mapped_count, releases the frame retain,
 * and frees the mapping node.  Returns IRIS_ERR_NOT_FOUND if user_va is not
 * currently mapped in vs (silently tolerated by callers that sweep a range). */
iris_error_t kvspace_unmap_page(struct KVSpace *vs, uint64_t user_va);

/* Drop the caller's lifecycle reference (kobject_release). */
void kvspace_free(struct KVSpace *vs);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KVSPACE_H */
