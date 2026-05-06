#include <iris/nc/kioport.h>
#include <iris/nc/kobject.h>
#include <iris/kpage.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kioport_live;

static void kioport_close(struct KObject *obj) {
    (void)obj;
}

static void kioport_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kioport_live, 1u, memory_order_relaxed);
    kpage_free((struct KIoPort *)obj, (uint32_t)sizeof(struct KIoPort));
}

static const struct KObjectOps kioport_ops = {
    .close   = kioport_close,
    .destroy = kioport_destroy,
};

struct KIoPort *kioport_alloc(uint16_t base_port, uint16_t count) {
    if (count == 0) return 0;
    struct KIoPort *port = kpage_alloc((uint32_t)sizeof(struct KIoPort));
    if (!port) return 0;
    kobject_init(&port->base, KOBJ_IOPORT, &kioport_ops);
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
