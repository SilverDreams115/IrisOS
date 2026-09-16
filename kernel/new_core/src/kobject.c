#include <iris/nc/kobject.h>
#include <iris/serial.h>
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

static void kobject_underflow_num(uint32_t v) {
    char b[12];
    int  n = 0;
    if (v == 0u) { serial_write("0"); return; }
    while (v && n < 11) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n--) { char c[2]; c[0] = b[n]; c[1] = 0; serial_write(c); }
}

static void kobject_underflow_report(const char *what, const struct KObject *obj) {
    /* `serial_write`, not `klog_write`: klog is a RING that ring 3 drains, and
     * the next instruction after this is a panic that halts the machine.  A
     * diagnostic that only survives if somebody later asks for it is no
     * diagnostic at all on the one path where it matters. */
    serial_write("\n[IRIS][KOBJ] ");
    serial_write(what);
    serial_write(" type=");
    kobject_underflow_num((uint32_t)obj->type);
    serial_write(" refcount=");
    kobject_underflow_num(atomic_load_explicit(&obj->refcount, memory_order_relaxed));
    serial_write(" active=");
    kobject_underflow_num(atomic_load_explicit(&obj->active_refs, memory_order_relaxed));
    serial_write(" ut_block=");
    kobject_underflow_num(obj->ut_block_bytes);
    serial_write("\n");
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
    if (prev < 1u) kobject_underflow_report("resurrect from refcount 0", obj);
    IRIS_ASSERT(prev >= 1u, "kobject_retain: resurrect from refcount 0");
}

/*
 * An underflow names the OBJECT, not just the counter (SMP roadmap §9.3
 * step 5).
 *
 * "refcount underflow" is true of every double-release in the kernel and tells
 * the next person nothing about which one they have.  With four processors
 * reaching one object the answer is usually the type plus what the counters
 * were, and both are one line away at the moment it is still true.  It runs
 * only on the way to a panic, so it costs nothing that matters.
 */

void kobject_release(struct KObject *obj) {
    /* release on decrement: publishes all preceding writes to the potential
     * destructor that may run on another thread. */
    uint32_t prev = atomic_fetch_sub_explicit(&obj->refcount, 1u,
                                              memory_order_release);
    if (prev < 1u) kobject_underflow_report("refcount underflow", obj);
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
    if (prev < 1u) kobject_underflow_report("active_refs underflow", obj);
    IRIS_ASSERT(prev >= 1u, "kobject_active_release: active_refs underflow");
    if (prev == 1u) {
        atomic_thread_fence(memory_order_acquire);
        if (obj->ops->close) {
            obj->ops->close(obj);
        }
    }
}
