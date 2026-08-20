#include <iris/nc/kbootcap.h>
#include <iris/kslab.h>
#include <stdatomic.h>
#include <stdint.h>

static void kbootcap_close(struct KObject *obj) {
    (void)obj;
}

static void kbootcap_destroy(struct KObject *obj) {
    kslab_free((struct KBootstrapCap *)obj, (uint32_t)sizeof(struct KBootstrapCap));
}

static const struct KObjectOps kbootcap_ops = {
    .close = kbootcap_close,
    .destroy = kbootcap_destroy,
};

struct KBootstrapCap *kbootcap_alloc(uint32_t kind) {
    struct KBootstrapCap *cap;

    /* One authority, or no capability.  A zero kind authorises nothing and a
     * multi-bit kind is the monolith this stage retired, so both are refused
     * at the only place a boot capability can come into existence. */
    if (kind == 0u || (kind & (kind - 1u)) != 0u) return 0;

    cap = kslab_alloc((uint32_t)sizeof(struct KBootstrapCap));
    if (!cap) return 0;
    kobject_init(&cap->base, KOBJ_BOOTSTRAP_CAP, &kbootcap_ops);
    cap->kind = kind;
    return cap;
}

void kbootcap_free(struct KBootstrapCap *cap) {
    if (!cap) return;
    kobject_release(&cap->base);
}

/* kbootcap_clone_restricted is REMOVED with SYS_BOOTCAP_RESTRICT (Stage 5
 * Step 2): narrowing a mask by rebuilding the object existed only because one
 * object carried several authorities.  Giving up an authority is deleting the
 * slot that holds it, and taking one back is revoking it through the CDT. */
