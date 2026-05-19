#include <iris/nc/kobject.h>

void kobject_init(struct KObject *obj, kobject_type_t type,
                  const struct KObjectOps *ops) {
    obj->type = type;
    obj->ops  = ops;
    spinlock_init(&obj->lock);
    atomic_store_explicit(&obj->refcount, 1u, memory_order_relaxed);
    atomic_store_explicit(&obj->active_refs, 0u, memory_order_relaxed);
}

void kobject_retain(struct KObject *obj) {
    /* relaxed fetch_add: retain alone does not establish happens-before.
     * The required acquire happens on the subsequent use of the object. */
    uint32_t prev = atomic_fetch_add_explicit(&obj->refcount, 1u,
                                              memory_order_relaxed);
    (void)prev;
    /* assert(prev >= 1) — never retain from refcount 0 */
}

void kobject_release(struct KObject *obj) {
    /* release on decrement: publishes all preceding writes to the potential
     * destructor that may run on another thread. */
    uint32_t prev = atomic_fetch_sub_explicit(&obj->refcount, 1u,
                                              memory_order_release);
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
    if (prev == 1u) {
        atomic_thread_fence(memory_order_acquire);
        if (obj->ops->close) {
            obj->ops->close(obj);
        }
    }
}
