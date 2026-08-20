#include <iris/nc/kioport.h>
#include <iris/nc/kobject.h>
#include <iris/kslab.h>
#include <iris/nc/kuntyped.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kioport_live;

static void kioport_close(struct KObject *obj) {
    (void)obj;
}

static void kioport_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kioport_live, 1u, memory_order_relaxed);
    kobject_storage_free(obj, (uint32_t)sizeof(struct KIoPort), 0);
}

/*
 * Stage 6 Step 6 — a device capability is a kernel object, so it is charged.
 *
 * Claiming a port range fabricates an object the kernel used to pay for; the
 * claimer names the Untyped instead.  seL4 does not allocate for these at all
 * (an IOPort cap has no object behind it), which is the deeper divergence the
 * ledger records — charging is what IRIS can do without changing what a
 * capability IS.
 */
static const struct KObjectOps kioport_ops = {
    .close   = kioport_close,
    .destroy = kioport_destroy,
};

struct KIoPort *kioport_alloc(uint16_t base_port, uint16_t count) {
    if (count == 0) return 0;
    struct KIoPort *port = kslab_alloc((uint32_t)sizeof(struct KIoPort));
    if (!port) return 0;
    kobject_init(&port->base, KOBJ_IOPORT, &kioport_ops);
    port->base_port = base_port;
    port->count     = count;
    atomic_fetch_add_explicit(&kioport_live, 1u, memory_order_relaxed);
    return port;
}

struct KIoPort *kioport_alloc_from(struct KUntyped *pool, uint16_t base_port,
                                   uint16_t count) {
    if (count == 0) return 0;
    if (!pool) return kioport_alloc(base_port, count);
    struct KIoPort *port = kuntyped_alloc_child_top(pool, sizeof(struct KIoPort));
    if (!port) return 0;
    kobject_init_in_untyped(&port->base, KOBJ_IOPORT, &kioport_ops,
                            (uint32_t)sizeof(struct KIoPort));
    port->base_port = base_port;
    port->count     = count;
    atomic_fetch_add_explicit(&kioport_live, 1u, memory_order_relaxed);
    return port;
}

void kioport_free(struct KIoPort *port) {
    if (!port) return;
    kobject_release(&port->base);
}

uint32_t kioport_live_count(void) {
    return atomic_load_explicit(&kioport_live, memory_order_relaxed);
}
