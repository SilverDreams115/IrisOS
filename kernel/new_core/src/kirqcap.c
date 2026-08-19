#include <iris/nc/kirqcap.h>
#include <iris/nc/kobject.h>
#include <iris/kslab.h>
#include <iris/nc/kuntyped.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kirqcap_live;

static void kirqcap_close(struct KObject *obj) {
    (void)obj;
}

static void kirqcap_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kirqcap_live, 1u, memory_order_relaxed);
    kslab_free((struct KIrqCap *)obj, (uint32_t)sizeof(struct KIrqCap));
}

static const struct KObjectOps kirqcap_ops = {
    .close   = kirqcap_close,
    .destroy = kirqcap_destroy,
};

/*
 * Stage 6 Etapa 6 — a device capability is a kernel object, so it is charged.
 *
 * Claiming an interrupt line or a port range fabricates an object the kernel
 * used to pay for; the claimer names the Untyped instead.  seL4 does not
 * allocate for these at all (an IRQHandler cap has no object behind it), which
 * is the deeper divergence the ledger records — charging is what IRIS can do
 * without changing what a capability IS.
 */
static void kirqcap_destroy_ut(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kirqcap_live, 1u, memory_order_relaxed);
    kuntyped_release_child(obj, sizeof(struct KIrqCap));
}

static const struct KObjectOps kirqcap_ops_ut = {
    .close   = kirqcap_close,
    .destroy = kirqcap_destroy_ut,
};

struct KIrqCap *kirqcap_alloc(uint8_t irq_num) {
    struct KIrqCap *cap = kslab_alloc((uint32_t)sizeof(struct KIrqCap));
    if (!cap) return 0;
    kobject_init(&cap->base, KOBJ_IRQ_CAP, &kirqcap_ops);
    cap->irq_num = irq_num;
    atomic_fetch_add_explicit(&kirqcap_live, 1u, memory_order_relaxed);
    return cap;
}

struct KIrqCap *kirqcap_alloc_from(struct KUntyped *pool, uint8_t irq_num) {
    if (!pool) return kirqcap_alloc(irq_num);
    struct KIrqCap *cap = kuntyped_alloc_child_top(pool, sizeof(struct KIrqCap));
    if (!cap) return 0;
    kobject_init(&cap->base, KOBJ_IRQ_CAP, &kirqcap_ops_ut);
    cap->irq_num = irq_num;
    atomic_fetch_add_explicit(&kirqcap_live, 1u, memory_order_relaxed);
    return cap;
}

void kirqcap_free(struct KIrqCap *cap) {
    if (!cap) return;
    kobject_release(&cap->base);
}

uint32_t kirqcap_live_count(void) {
    return atomic_load_explicit(&kirqcap_live, memory_order_relaxed);
}
