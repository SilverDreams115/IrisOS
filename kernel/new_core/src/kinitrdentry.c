#include <iris/nc/kinitrdentry.h>
#include <stdint.h>

static struct KInitrdEntry pool[KINITRDENTRY_POOL_SIZE];
static uint8_t             pool_used[KINITRDENTRY_POOL_SIZE];

static void kinitrdentry_close(struct KObject *obj) { (void)obj; }

static void kinitrdentry_destroy(struct KObject *obj) {
    struct KInitrdEntry *e = (struct KInitrdEntry *)obj;
    for (int i = 0; i < (int)KINITRDENTRY_POOL_SIZE; i++) {
        if (&pool[i] == e) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps kinitrdentry_ops = {
    .close   = kinitrdentry_close,
    .destroy = kinitrdentry_destroy
};

struct KInitrdEntry *kinitrdentry_alloc(const void *data, uint32_t size) {
    for (int i = 0; i < (int)KINITRDENTRY_POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            struct KInitrdEntry *e = &pool[i];
            uint8_t *p = (uint8_t *)e;
            for (uint32_t j = 0; j < (uint32_t)sizeof(*e); j++) p[j] = 0;
            kobject_init(&e->base, KOBJ_INITRD_ENTRY, &kinitrdentry_ops);
            e->data = data;
            e->size = size;
            return e;
        }
    }
    return 0;
}

void kinitrdentry_free(struct KInitrdEntry *e) {
    kobject_release(&e->base);
}
