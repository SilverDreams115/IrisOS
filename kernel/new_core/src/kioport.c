#include <iris/nc/kioport.h>
#include <iris/nc/kobject.h>
#include <stdint.h>

static struct KIoPort pool[KIOPORT_POOL_SIZE];
static uint8_t        pool_used[KIOPORT_POOL_SIZE];

static void kioport_close(struct KObject *obj) {
    (void)obj;
}

static void kioport_destroy(struct KObject *obj) {
    struct KIoPort *port = (struct KIoPort *)obj;
    uint32_t i;
    for (i = 0; i < KIOPORT_POOL_SIZE; i++) {
        if (&pool[i] == port) {
            pool_used[i] = 0;
            return;
        }
    }
}

static const struct KObjectOps kioport_ops = {
    .close   = kioport_close,
    .destroy = kioport_destroy,
};

struct KIoPort *kioport_alloc(uint16_t base_port, uint16_t count) {
    uint32_t i;
    if (count == 0) return 0;
    for (i = 0; i < KIOPORT_POOL_SIZE; i++) {
        if (pool_used[i]) continue;
        pool_used[i] = 1;
        {
            struct KIoPort *port = &pool[i];
            uint8_t *raw = (uint8_t *)port;
            uint32_t j;
            for (j = 0; j < sizeof(*port); j++) raw[j] = 0;
            kobject_init(&port->base, KOBJ_IOPORT, &kioport_ops);
            port->base_port = base_port;
            port->count     = count;
            return port;
        }
    }
    return 0;
}

void kioport_free(struct KIoPort *port) {
    if (!port) return;
    kobject_release(&port->base);
}

uint32_t kioport_live_count(void) {
    uint32_t count = 0;
    uint32_t i;
    for (i = 0; i < KIOPORT_POOL_SIZE; i++) {
        if (pool_used[i]) count++;
    }
    return count;
}
