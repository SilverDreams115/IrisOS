#include <iris/nc/kinitrdentry.h>
#include <iris/kpage.h>
#include <stdint.h>

static void kinitrdentry_close(struct KObject *obj) { (void)obj; }

static void kinitrdentry_destroy(struct KObject *obj) {
    kpage_free((struct KInitrdEntry *)obj, (uint32_t)sizeof(struct KInitrdEntry));
}

static const struct KObjectOps kinitrdentry_ops = {
    .close   = kinitrdentry_close,
    .destroy = kinitrdentry_destroy
};

struct KInitrdEntry *kinitrdentry_alloc(const void *data, uint32_t size) {
    struct KInitrdEntry *e = kpage_alloc((uint32_t)sizeof(struct KInitrdEntry));
    if (!e) return 0;
    kobject_init(&e->base, KOBJ_INITRD_ENTRY, &kinitrdentry_ops);
    e->data = data;
    e->size = size;
    return e;
}

void kinitrdentry_free(struct KInitrdEntry *e) {
    kobject_release(&e->base);
}
