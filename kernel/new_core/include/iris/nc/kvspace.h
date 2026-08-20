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

struct KUntyped;

struct KVSpace {
    struct KObject        base;          /* must be first */
    spinlock_t            lock;          /* guards all fields below */
    uint64_t              cr3;           /* physical PML4; 0 after invalidation */
    int                   valid;         /* 1 = live, 0 = reaped */
    uint32_t              mapping_count; /* number of live KFrameMapping nodes */
    struct KFrameMapping *mappings;      /* singly-linked list head; NULL = empty */
    /* The Untyped this address space's KERNEL-SIDE bookkeeping comes out of:
     * its mapping records, and (when pml4_from_pool) the page its PML4 lives
     * in.  Retained for as long as the VSpace lives.
     *
     * Stage 6-pure Etapa 3: this is no longer "who pays for page tables" —
     * nobody does, because the kernel does not create them.  It also no longer
     * decides whether a map is strict; that is `kernel_funded` below.  One
     * field answering three questions is why the root task could not be strict
     * without also being charged, and vice versa. */
    struct KUntyped      *pt_pool;
    /* The PML4 is a page child of pt_pool rather than a PMM page, so teardown
     * must return its child entry.  Replaces the old pt_count, which counted
     * kernel-carved page tables and, since the kernel stopped carving them,
     * only ever held 0 or 1 — the PML4. */
    uint8_t               pml4_from_pool;
    /*
     * The bootstrap exception, and the whole of it.
     *
     * 1 means the kernel supplies this address space's paging levels from the
     * PMM reserve, because there is no userland able to supply them: the root
     * task's text, stack and BootInfo are mapped before it exists.  It is
     * cleared the moment the root task can speak for itself, after which the
     * root task is as strict as every other address space and supplies its own
     * levels through its own budget.
     *
     * No other address space ever sets it.  A spawned process has a holder
     * from the moment it is created, so it owes its own levels from the first
     * map — there is no window in which the kernel would have to guess.
     */
    uint8_t               kernel_funded;
    /* Stage 6-pure Etapa 4: an address space the HOLDER retyped is bound to at
     * most one process.  Binding is what gives it a cr3 to be — before that it
     * is a page and a header — and two processes sharing one would be two
     * CR3-loads of the same walk with different teardowns behind them. */
    uint8_t               bound;
    /* Stage 6 Etapa 6: mapping records carved from pt_pool, and the free list
     * they return to.  Mappings churn — map, unmap, map again — and a bump
     * allocator never rewinds, so carving one per map would leak the budget at
     * the rate the process maps.  A fixed-size free list makes reuse exact:
     * the VSpace carves only when the list is empty, and returns every node to
     * its Untyped when the address space is destroyed. */
    struct KFrameMapping *free_nodes;
    uint32_t              node_count;   /* nodes ever carved from pt_pool */
    /* Stage 6-pure Etapa 1: the page tables the HOLDER retyped and installed
     * here, newest first.  The VSpace holds one reference to each for as long
     * as it is part of the walk, which is what stops a holder from RESETting
     * the region a live address space is standing on — the same guarantee the
     * charged path got from child_count, now attached to a real capability. */
    struct KPageTable    *tables;
};

struct KPageTable;

/*
 * Install a page table the caller retyped, at the first level missing for
 * `vaddr`.  Takes vs->lock.  The VSpace retains `pt` and links it into its
 * table list; the level it filled is recorded on the table.
 *
 * IRIS_ERR_ALREADY_EXISTS  the walk for `vaddr` is complete, or this table is
 *                          already installed somewhere.
 * IRIS_ERR_INVALID_ARG     a huge-page leaf covers `vaddr`, or vs is dead.
 */
iris_error_t kvspace_map_table(struct KVSpace *vs, struct KPageTable *pt,
                               uint64_t vaddr);

/* Mapping-record allocator.  Caller holds no VSpace lock: both take it. */
struct KFrameMapping *kvspace_node_alloc(struct KVSpace *vs);
void                  kvspace_node_free(struct KVSpace *vs,
                                        struct KFrameMapping *m);

/* Attach the pool this address space's kernel-side bookkeeping comes from.
 * Retains `pool`; called once.  Safe to call after the VSpace has mapped
 * things — records already handed out from the bootstrap arena are recognised
 * by their address, not by this field (kvspace_node_free). */
void kvspace_set_pt_pool(struct KVSpace *vs, struct KUntyped *pool);

/* End the bootstrap exception: from here on this address space supplies its
 * own paging levels like every other.  Called once, when the root task becomes
 * able to speak for itself. */
void kvspace_end_bootstrap(struct KVSpace *vs);

/* Allocate a new KVSpace wrapping the given cr3. Returns NULL on OOM.
 * Caller holds the alloc lifecycle ref (refcount=1, active_refs=0) on return. */
struct KVSpace *kvspace_alloc(uint64_t cr3);

/* Stage 6 Etapa 3 — placement-init a KVSpace whose header is a child block of
 * the same Untyped that pays for its PML4 and page tables.  `mem` must be a
 * zeroed block of at least sizeof(struct KVSpace), from
 * kuntyped_alloc_child_top.
 *
 * Returns NULL for a NULL `mem` and CANNOT FAIL otherwise — it only writes
 * fields.  sys_process_create depends on that: it carves this header BEFORE
 * the PML4 so that a pooled PML4 always has a VSpace to own it, and there is
 * no window in which kprocess_reap_address_space could mistake an
 * Untyped-owned cr3 for a PMM page.  A future variant that can fail after
 * taking the block has to give that caller a way to unwind the PML4. */
struct KVSpace *kvspace_alloc_at(void *mem, uint64_t cr3);

/*
 * Stage 6-pure Etapa 4 — the address space as a retyped object.
 *
 * `mem` is a top-carved header block and `pml4_phys` the page-aligned region
 * carved from the same Untyped; the page becomes the PML4 and `pool` becomes
 * where this address space's kernel-side bookkeeping comes from.  The result
 * is an UNBOUND address space: real, mappable-into once bound, and owned by
 * whoever holds the capability.
 */
struct KVSpace *kvspace_retype_at(void *mem, uint64_t pml4_phys,
                                  struct KUntyped *pool);

/* Bind a retyped address space to a process.  Fails if it is already bound —
 * one address space, one process, because teardown is per-process. */
iris_error_t kvspace_bind(struct KVSpace *vs);

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

/* Fase 19: live KVSpace object count (additive diagnostics; see kvspace.c). */
uint32_t kvspace_live_count(void);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KVSPACE_H */
