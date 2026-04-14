#include <iris/nc/kbootcap.h>
#include <stdint.h>

static struct KBootstrapCap pool[KBOOTCAP_POOL_SIZE];
static uint8_t              pool_used[KBOOTCAP_POOL_SIZE];

static void kbootcap_close(struct KObject *obj) {
    (void)obj;
}

static void kbootcap_destroy(struct KObject *obj) {
    struct KBootstrapCap *cap = (struct KBootstrapCap *)obj;
    for (uint32_t i = 0; i < KBOOTCAP_POOL_SIZE; i++) {
        if (&pool[i] == cap) {
            pool_used[i] = 0;
            return;
        }
    }
}

static const struct KObjectOps kbootcap_ops = {
    .close = kbootcap_close,
    .destroy = kbootcap_destroy,
};

struct KBootstrapCap *kbootcap_alloc(uint32_t permissions) {
    for (uint32_t i = 0; i < KBOOTCAP_POOL_SIZE; i++) {
        if (pool_used[i]) continue;
        pool_used[i] = 1;
        {
            struct KBootstrapCap *cap = &pool[i];
            uint8_t *raw = (uint8_t *)cap;
            for (uint32_t j = 0; j < sizeof(*cap); j++) raw[j] = 0;
            kobject_init(&cap->base, KOBJ_BOOTSTRAP_CAP, &kbootcap_ops);
            cap->permissions = permissions;
            return cap;
        }
    }
    return 0;
}

void kbootcap_free(struct KBootstrapCap *cap) {
    if (!cap) return;
    kobject_release(&cap->base);
}
