/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/nc/kobject.h>
#include <iris/nc/kobject_dispatch.h>
#include <iris/serial.h>
#include <iris/nc/kuntyped.h>
#include <iris/kslab.h>
#include <iris/panic.h>

void kobject_init(struct KObject *obj, kobject_type_t type) {
    IRIS_ASSERT(obj != 0, "kobject_init: NULL object");
    obj->type = type;
#ifdef IRIS_HOST_TEST
    obj->ops = 0;                      /* a real object: dispatch on type */
#endif
    obj->ut_block_bytes = 0u;          /* kernel-slab storage */
    obj->destroy_next   = 0;           /* not queued for teardown */
    spinlock_init(&obj->lock);
    atomic_store_explicit(&obj->refcount, 1u, memory_order_relaxed);
    atomic_store_explicit(&obj->active_refs, 0u, memory_order_relaxed);
}

#ifdef IRIS_HOST_TEST
/*
 * A host test fixture: a real TYPE on storage the kernel did not allocate.
 *
 * The suites build objects of type KOBJ_ENDPOINT, KOBJ_CNODE and so on out of
 * heap memory, to drive CSpace and MDB logic that switches on type — and they
 * must observe destruction without the real destructor running, because the
 * real one would hand a heap block back to an Untyped that never owned it.
 * That is the ONE thing a type cannot answer, so it is the one place a pointer
 * survives, and it survives only in a build that is not the kernel.
 */
void kobject_init_test(struct KObject *obj, kobject_type_t type,
                       const struct KObjectOps *ops) {
    kobject_init(obj, type);
    IRIS_ASSERT(ops != 0 && ops->destroy != 0,
                "kobject_init_test: fixture with no destructor");
    obj->ops = ops;
}
#endif

void kobject_init_in_untyped(struct KObject *obj, kobject_type_t type,
                             uint32_t block_bytes) {
    kobject_init(obj, type);
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

/*
 * ── Destruction, dispatched on the TYPE ─────────────────────────────────────
 *
 * seL4 does this and stores no pointer in the object.  IRIS stored one, in 18
 * `KObjectOps` tables across 17 files, and the cost was never the indirect
 * call: `scripts/check_lock_order.py` §9.1 admits it cannot follow
 * `ops->destroy`, so the lock hierarchy went UNCHECKED exactly where a
 * destructor runs, which is where it matters most.  The mapping from type to
 * destructor also existed nowhere a reader or a tool could see it — only as
 * initialisers scattered through the files that defined the objects.
 *
 * Here it is, as a table, in one place.
 *
 * `default:` is a PANIC and not a silent return.  An object whose type has no
 * destructor is a leak or a type confusion, and both are worth stopping for at
 * the moment they are provable rather than at the moment they are fatal.
 */
static void kobject_dispatch_close(struct KObject *obj) {
#ifdef IRIS_HOST_TEST
    if (obj->ops) { if (obj->ops->close) obj->ops->close(obj); return; }
#endif
    switch (obj->type) {
    case KOBJ_NOTIFICATION:   knotification_close(obj);    break;
    case KOBJ_BOOTSTRAP_CAP:  kbootcap_close(obj);         break;
    case KOBJ_IRQ_CAP:        kirqcap_close(obj);          break;
    case KOBJ_IOPORT:         kioport_close(obj);          break;
    case KOBJ_ENDPOINT:       kendpoint_obj_close(obj);    break;
    case KOBJ_CNODE:          kcnode_obj_close(obj);       break;
    case KOBJ_SCHED_CONTEXT:  kschedctx_obj_close(obj);    break;
    case KOBJ_UNTYPED:        kuntyped_obj_close(obj);     break;
    case KOBJ_REPLY:          kreply_obj_close(obj);       break;
    case KOBJ_VSPACE:         kvspace_obj_close(obj);      break;
    case KOBJ_FRAME:          kframe_obj_close(obj);       break;
    case KOBJ_PAGE_TABLE:     kpagetable_obj_close(obj);   break;
    case KOBJ_ASID_POOL:      kasidpool_obj_close(obj);    break;
    case KOBJ_IOSPACE:        kiospace_obj_close(obj);     break;
    case KOBJ_IO_PAGE_TABLE:  kiopagetable_obj_close(obj); break;
    /* A TCB has no close hook: nothing is published about a thread that is
     * not already unwound by exit.  Stated rather than defaulted, so that
     * adding one is a decision and not an omission. */
    case KOBJ_TCB:            break;
    default:                  break;   /* the reserved wire types */
    }
}

static void kobject_dispatch_destroy(struct KObject *obj) {
#ifdef IRIS_HOST_TEST
    if (obj->ops) { obj->ops->destroy(obj); return; }
#endif
    switch (obj->type) {
    case KOBJ_NOTIFICATION:   knotification_destroy_ut(obj);  break;
    /*
     * The one type whose storage decides its destructor.  A bootstrap cap is
     * born either in the kernel slab or in an Untyped child block, and the
     * object already records which in `ut_block_bytes` — so this reads the
     * fact off the object instead of needing a second table to remember it.
     */
    case KOBJ_BOOTSTRAP_CAP:
        if (obj->ut_block_bytes) kbootcap_destroy_ut(obj);
        else                     kbootcap_destroy(obj);
        break;
    case KOBJ_IRQ_CAP:        kirqcap_destroy(obj);           break;
    case KOBJ_IOPORT:         kioport_destroy(obj);           break;
    case KOBJ_ENDPOINT:       kendpoint_obj_destroy_ut(obj);  break;
    case KOBJ_CNODE:          kcnode_obj_destroy(obj);        break;
    case KOBJ_SCHED_CONTEXT:  kschedctx_obj_destroy_ut(obj);  break;
    case KOBJ_UNTYPED:        kuntyped_obj_destroy(obj);      break;
    case KOBJ_REPLY:          kreply_obj_destroy_ut(obj);     break;
    case KOBJ_TCB:            ktcb_obj_destroy(obj);          break;
    case KOBJ_VSPACE:         kvspace_obj_destroy(obj);       break;
    case KOBJ_FRAME:          kframe_obj_destroy(obj);        break;
    case KOBJ_PAGE_TABLE:     kpagetable_obj_destroy(obj);    break;
    case KOBJ_ASID_POOL:      kasidpool_obj_destroy(obj);     break;
    case KOBJ_IOSPACE:        kiospace_obj_destroy(obj);      break;
    case KOBJ_IO_PAGE_TABLE:  kiopagetable_obj_destroy(obj);  break;
    default:
        iris_panic("kobject_destroy: object type has no destructor");
    }
}

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
        kobject_dispatch_destroy(obj);
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
        kobject_dispatch_close(obj);
    }
}
