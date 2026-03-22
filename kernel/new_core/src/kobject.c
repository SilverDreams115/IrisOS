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
    /* fetch_add con relaxed: el retain no establece happens-before por sí solo.
     * El acquire necesario ocurre en el uso posterior del objeto. */
    uint32_t prev = atomic_fetch_add_explicit(&obj->refcount, 1u,
                                              memory_order_relaxed);
    (void)prev;
    /* assert(prev >= 1) — nunca retener desde refcount 0 */
}

void kobject_release(struct KObject *obj) {
    /* release en el decremento: publica todas las escrituras previas
     * al posible destructor que corre en otro thread. */
    uint32_t prev = atomic_fetch_sub_explicit(&obj->refcount, 1u,
                                              memory_order_release);
    if (prev == 1u) {
        /* acquire fence: asegura que este thread ve todas las escrituras
         * que precedieron a los release de otros threads. */
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
