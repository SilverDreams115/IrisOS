#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/kslab.h>
#include <iris/panic.h>

void kobject_init(struct KObject *obj, kobject_type_t type,
                  const struct KObjectOps *ops) {
    /* "ops != NULL for every initialized KObject" — the header states it as an
     * invariant and kobject_release dereferences ops->destroy unconditionally,
     * so a NULL here is a jump through a null pointer at some unrelated later
     * moment.  Checked where it is established, not where it is used. */
    IRIS_ASSERT(obj != 0, "kobject_init: NULL object");
    IRIS_ASSERT(ops != 0 && ops->destroy != 0,
                "kobject_init: object with no destructor");
    obj->type = type;
    obj->ops  = ops;
    obj->ut_block_bytes = 0u;          /* kernel-slab storage */
    spinlock_init(&obj->lock);
    atomic_store_explicit(&obj->refcount, 1u, memory_order_relaxed);
    atomic_store_explicit(&obj->active_refs, 0u, memory_order_relaxed);
}

void kobject_init_in_untyped(struct KObject *obj, kobject_type_t type,
                             const struct KObjectOps *ops,
                             uint32_t block_bytes) {
    kobject_init(obj, type, ops);
    obj->ut_block_bytes = block_bytes;
}

void kobject_storage_free(struct KObject *obj, uint32_t slab_bytes,
                          struct KUntyped **out_pool) {
    uint32_t block = obj->ut_block_bytes;
    if (out_pool) *out_pool = 0;
    if (block == 0u) { kslab_free(obj, slab_bytes); return; }
    /* Read the parent out of the block before release zeroes it — the caller
     * may hold its own retain on that pool and must drop it AFTER this. */
    if (out_pool) *out_pool = kuntyped_child_parent(obj);
    kuntyped_release_child(obj, block);
}

void kobject_retain(struct KObject *obj) {
    /* relaxed fetch_add: retain alone does not establish happens-before.
     * The required acquire happens on the subsequent use of the object. */
    uint32_t prev = atomic_fetch_add_explicit(&obj->refcount, 1u,
                                              memory_order_relaxed);
    /*
     * Retaining from zero is resurrection: the destructor has run or is
     * running, and the storage may already be back in its Untyped.  Every
     * caller reaches this through a reference it already holds, so a zero here
     * means one was dropped twice somewhere upstream — and the symptom without
     * this check is a use-after-free that surfaces as an unrelated object
     * behaving strangely much later.  This was a comment for the whole life of
     * the file; it is the check now.
     */
    IRIS_ASSERT(prev >= 1u, "kobject_retain: resurrect from refcount 0");
}

void kobject_release(struct KObject *obj) {
    /* release on decrement: publishes all preceding writes to the potential
     * destructor that may run on another thread. */
    uint32_t prev = atomic_fetch_sub_explicit(&obj->refcount, 1u,
                                              memory_order_release);
    IRIS_ASSERT(prev >= 1u, "kobject_release: refcount underflow");
    if (prev == 1u) {
        /* acquire fence: ensures this thread sees all writes that preceded
         * the release decrements issued by other threads. */
        atomic_thread_fence(memory_order_acquire);
        obj->ops->destroy(obj);
    }
}

void kobject_active_retain(struct KObject *obj) {
    uint32_t prev = atomic_fetch_add_explicit(&obj->active_refs, 1u,
                                              memory_order_relaxed);
    (void)prev;
}

void kobject_active_release(struct KObject *obj) {
    uint32_t prev = atomic_fetch_sub_explicit(&obj->active_refs, 1u,
                                              memory_order_release);
    IRIS_ASSERT(prev >= 1u, "kobject_active_release: active_refs underflow");
    if (prev == 1u) {
        atomic_thread_fence(memory_order_acquire);
        if (obj->ops->close) {
            obj->ops->close(obj);
        }
    }
}
