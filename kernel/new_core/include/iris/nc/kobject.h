#ifndef IRIS_NC_KOBJECT_H
#define IRIS_NC_KOBJECT_H

/*
 * kobject.h — kernel object base type.
 *
 * The struct definitions and API here are KERNEL-INTERNAL.
 * Userland code (services) may include this header for forward declarations
 * only; the actual struct layout and functions are guarded by __KERNEL__.
 * Userland only needs handle_id_t (handle.h) and KChanMsg (kchannel.h).
 */

#ifdef __KERNEL__
#include <stdint.h>
#include <stdatomic.h>
#include <iris/nc/spinlock.h>

typedef enum {
    KOBJ_PROCESS,
    KOBJ_CHANNEL,
    KOBJ_NOTIFICATION,
    KOBJ_BOOTSTRAP_CAP,
    KOBJ_VMO,
    KOBJ_IRQ_CAP,      /* authorizes routing a specific hardware IRQ line */
    KOBJ_IOPORT,       /* authorizes IN/OUT access to a contiguous I/O port range */
    KOBJ_INITRD_ENTRY, /* immutable reference to a named ELF image in the kernel initrd */
    KOBJ_ENDPOINT,       /* seL4-style synchronous IPC rendezvous point */
    KOBJ_CNODE,          /* seL4-style capability node — fixed array of capability slots */
    KOBJ_SCHED_CONTEXT,  /* Ph74: scheduling context — budget/period for time protection */
    KOBJ_UNTYPED,        /* Ph76: seL4-style untyped memory cap — physical region for typed-object creation */
    KOBJ_REPLY,          /* Ph85: seL4-style one-shot reply capability — delivered by EP_CALL rendezvous */
    KOBJ_TCB,            /* Ph96: thread control block capability — wraps a live struct task */
    KOBJ_VSPACE,         /* Phase 4: virtual address space capability — wraps a process PML4 */
    KOBJ_FRAME,          /* Phase 5: physical memory frame capability — typed region from KUntyped */
    KOBJ_PAGE_TABLE,     /* Stage 6-pure: a paging level the USER retyped and maps explicitly */
} kobject_type_t;

struct KObject;

struct KObjectOps {
    void (*close)(struct KObject *obj);
    void (*destroy)(struct KObject *obj);
};

struct KObject {
    kobject_type_t            type;
    _Atomic uint32_t          refcount;    /* lifecycle ref — not visible to callers */
    _Atomic uint32_t          active_refs; /* published handle/global refs */
    spinlock_t                lock;        /* guards mutable state — separate from lifecycle */
    const struct KObjectOps  *ops;         /* non-NULL for every initialized object */
    /* Stage 6: where this object's STORAGE came from, and how much of it.
     * 0 = the kernel slab.  Non-zero = a child block of an Untyped, of exactly
     * this many bytes — what kuntyped_release_child needs to hand it back.
     *
     * It lives here, on the object, because it is a fact about the object and
     * not about the code that frees it.  Every migrated type otherwise needed
     * a SECOND ops table and a SECOND destructor that differed from its slab
     * twin by one call, and the two drifted: the shape of the release protocol
     * had to be re-derived seven times. */
    uint32_t                  ut_block_bytes;
};

/*
 * Invariants:
 *   - ops != NULL for every initialized KObject
 *   - ops->destroy() is the only real destruction path
 *   - refcount >= 1 while any HandleEntry points to this object
 *   - ops->destroy() is called exactly once, when refcount reaches 0
 *   - ops->destroy() must not touch refcount
 *   - refcount and lock protect different things — never used interchangeably
 */

struct KUntyped;

void kobject_init(struct KObject *obj, kobject_type_t type,
                  const struct KObjectOps *ops);
/* Same, for an object placement-built in an Untyped child block of
 * `block_bytes` bytes (what was passed to kuntyped_alloc_child*).  The block
 * arrives zeroed, so this only records where the storage came from. */
void kobject_init_in_untyped(struct KObject *obj, kobject_type_t type,
                             const struct KObjectOps *ops,
                             uint32_t block_bytes);
/*
 * Release the object's STORAGE — the last act of any destroy callback.
 *
 * Returns the Untyped child block if the object was born in one, else frees
 * `slab_bytes` to the kernel slab.  The object must not be touched afterwards.
 * `out_pool` receives the Untyped the block belonged to (NULL for a slab
 * object) WITHOUT dropping any reference: a type that keeps its own retain on
 * that pool releases it after this returns, never before — the block is inside
 * the pool's region, and it is what records who the parent was.
 */
void kobject_storage_free(struct KObject *obj, uint32_t slab_bytes,
                          struct KUntyped **out_pool);
void kobject_retain(struct KObject *obj);
void kobject_release(struct KObject *obj);
void kobject_active_retain(struct KObject *obj);
void kobject_active_release(struct KObject *obj);

#else /* !__KERNEL__ — userland forward declaration only */

/*
 * Opaque forward declaration for userland.  Userland never dereferences
 * struct KObject; it only holds handle_id_t tokens from the kernel.
 */
struct KObject;

#endif /* __KERNEL__ */

#endif /* IRIS_NC_KOBJECT_H */
