#include <iris/nc/kirqcap.h>
#include <iris/nc/kobject.h>
#include <stdint.h>

static struct KIrqCap pool[KIRQCAP_POOL_SIZE];
static uint8_t        pool_used[KIRQCAP_POOL_SIZE];

static void kirqcap_close(struct KObject *obj) {
    (void)obj;
}

static void kirqcap_destroy(struct KObject *obj) {
    struct KIrqCap *cap = (struct KIrqCap *)obj;
    uint32_t i;
    for (i = 0; i < KIRQCAP_POOL_SIZE; i++) {
        if (&pool[i] == cap) {
            pool_used[i] = 0;
            return;
        }
    }
}

static const struct KObjectOps kirqcap_ops = {
    .close   = kirqcap_close,
    .destroy = kirqcap_destroy,
};

struct KIrqCap *kirqcap_alloc(uint8_t irq_num) {
    uint32_t i;
    for (i = 0; i < KIRQCAP_POOL_SIZE; i++) {
        if (pool_used[i]) continue;
        pool_used[i] = 1;
        {
            struct KIrqCap *cap = &pool[i];
            uint8_t *raw = (uint8_t *)cap;
            uint32_t j;
            for (j = 0; j < sizeof(*cap); j++) raw[j] = 0;
            kobject_init(&cap->base, KOBJ_IRQ_CAP, &kirqcap_ops);
            cap->irq_num = irq_num;
            return cap;
        }
    }
    return 0;
}

void kirqcap_free(struct KIrqCap *cap) {
    if (!cap) return;
    kobject_release(&cap->base);
}

uint32_t kirqcap_live_count(void) {
    uint32_t count = 0;
    uint32_t i;
    for (i = 0; i < KIRQCAP_POOL_SIZE; i++) {
        if (pool_used[i]) count++;
    }
    return count;
}
