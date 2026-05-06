#include <iris/nc/kirqcap.h>
#include <iris/nc/kobject.h>
#include <iris/kpage.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kirqcap_live;

static void kirqcap_close(struct KObject *obj) {
    (void)obj;
}

static void kirqcap_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kirqcap_live, 1u, memory_order_relaxed);
    kpage_free((struct KIrqCap *)obj, (uint32_t)sizeof(struct KIrqCap));
}

static const struct KObjectOps kirqcap_ops = {
    .close   = kirqcap_close,
    .destroy = kirqcap_destroy,
};

struct KIrqCap *kirqcap_alloc(uint8_t irq_num) {
    struct KIrqCap *cap = kpage_alloc((uint32_t)sizeof(struct KIrqCap));
    if (!cap) return 0;
    kobject_init(&cap->base, KOBJ_IRQ_CAP, &kirqcap_ops);
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
