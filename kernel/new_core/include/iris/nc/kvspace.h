#ifndef IRIS_NC_KVSPACE_H
#define IRIS_NC_KVSPACE_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

/*
 * KVSpace — VSpace capability object (Fase 4).
 *
 * Represents formal authority over a process address space.
 * In Fase 4 this is a non-owning wrapper: KProcess owns the page tables;
 * KVSpace records the cr3 value and an invalidation flag so capability
 * holders can detect a reaped address space without dangling pointers.
 *
 * Ownership model (Fase 4 — transitional non-owning wrapper):
 *   - KProcess holds one lifecycle ref (kobject_retain).
 *   - Each CNode slot holds lifecycle + active ref via kcnode_mint.
 *   - Page tables are owned by KProcess; KVSpace does NOT free them.
 *   - kvspace_invalidate() zeroes cr3 and clears valid before page table reap.
 *
 * Planned migration (future phases):
 *   - Fase 5: KVSpace becomes owner of cr3 / page table root.
 *   - Fase 6: demand paging removed; map/unmap require explicit KVSpace cap.
 */
struct KVSpace {
    struct KObject  base;   /* must be first */
    spinlock_t      lock;   /* guards cr3 and valid */
    uint64_t        cr3;    /* physical PML4 root; 0 after invalidation */
    int             valid;  /* 1 while process address space is live; 0 after reap */
};

/* Allocate a new KVSpace wrapping the given cr3. Returns NULL on OOM.
 * Caller holds the alloc lifecycle ref (refcount=1, active_refs=0) on return. */
struct KVSpace *kvspace_alloc(uint64_t cr3);

/* Mark the VSpace invalid and zero cr3.  Called by kprocess_reap_address_space
 * before paging_destroy_user_space so no capability holder can read a freed cr3. */
void kvspace_invalidate(struct KVSpace *vs);

/* Drop the caller's lifecycle reference (kobject_release). */
void kvspace_free(struct KVSpace *vs);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KVSPACE_H */
