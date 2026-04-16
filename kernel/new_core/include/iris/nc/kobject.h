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
    KOBJ_IRQ_CAP,   /* authorizes routing a specific hardware IRQ line */
    KOBJ_IOPORT,    /* authorizes IN/OUT access to a contiguous I/O port range */
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

void kobject_init(struct KObject *obj, kobject_type_t type,
                  const struct KObjectOps *ops);
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
