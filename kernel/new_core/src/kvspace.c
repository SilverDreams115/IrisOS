#include <iris/nc/kvspace.h>
#include <iris/kslab.h>

static void kvspace_obj_close(struct KObject *obj) {
    (void)obj;
}

static void kvspace_obj_destroy(struct KObject *obj) {
    kslab_free((struct KVSpace *)obj, (uint32_t)sizeof(struct KVSpace));
}

static const struct KObjectOps kvspace_ops = {
    .close   = kvspace_obj_close,
    .destroy = kvspace_obj_destroy,
};

struct KVSpace *kvspace_alloc(uint64_t cr3) {
    struct KVSpace *vs = kslab_alloc((uint32_t)sizeof(struct KVSpace));
    if (!vs) return 0;
    kobject_init(&vs->base, KOBJ_VSPACE, &kvspace_ops);
    spinlock_init(&vs->lock);
    vs->cr3   = cr3;
    vs->valid = 1;
    return vs;
}

void kvspace_invalidate(struct KVSpace *vs) {
    if (!vs) return;
    spinlock_lock(&vs->lock);
    vs->valid = 0;
    vs->cr3   = 0;
    spinlock_unlock(&vs->lock);
}

void kvspace_free(struct KVSpace *vs) {
    if (!vs) return;
    kobject_release(&vs->base);
}
