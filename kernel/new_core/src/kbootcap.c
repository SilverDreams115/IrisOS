#include <iris/nc/kbootcap.h>
#include <iris/kpage.h>
#include <stdatomic.h>
#include <stdint.h>

static void kbootcap_close(struct KObject *obj) {
    (void)obj;
}

static void kbootcap_destroy(struct KObject *obj) {
    kpage_free((struct KBootstrapCap *)obj, (uint32_t)sizeof(struct KBootstrapCap));
}

static const struct KObjectOps kbootcap_ops = {
    .close = kbootcap_close,
    .destroy = kbootcap_destroy,
};

struct KBootstrapCap *kbootcap_alloc(uint32_t permissions) {
    struct KBootstrapCap *cap = kpage_alloc((uint32_t)sizeof(struct KBootstrapCap));
    if (!cap) return 0;
    kobject_init(&cap->base, KOBJ_BOOTSTRAP_CAP, &kbootcap_ops);
    cap->permissions = permissions;
    return cap;
}

void kbootcap_free(struct KBootstrapCap *cap) {
    if (!cap) return;
    kobject_release(&cap->base);
}

struct KBootstrapCap *kbootcap_clone_restricted(const struct KBootstrapCap *src,
                                                uint32_t new_permissions) {
    struct KBootstrapCap *clone;
    uint32_t permissions;

    if (!src) return 0;

    spinlock_lock((spinlock_t *)&src->base.lock);
    permissions = src->permissions & new_permissions;
    spinlock_unlock((spinlock_t *)&src->base.lock);

    clone = kbootcap_alloc(permissions);
    return clone;
}
