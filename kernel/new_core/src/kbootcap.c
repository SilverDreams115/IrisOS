#include <iris/nc/kbootcap.h>
#include <iris/kslab.h>
#include <iris/nc/kuntyped.h>
#include <stdatomic.h>
#include <stdint.h>

static void kbootcap_close(struct KObject *obj) {
    (void)obj;
}

static void kbootcap_destroy(struct KObject *obj) {
    kslab_free((struct KBootstrapCap *)obj, (uint32_t)sizeof(struct KBootstrapCap));
}

/* Untyped-backed: the block goes back to the region it was carved from. */
static void kbootcap_destroy_ut(struct KObject *obj) {
    kuntyped_release_child(obj, (uint64_t)sizeof(struct KBootstrapCap));
}

static const struct KObjectOps kbootcap_ops = {
    .close = kbootcap_close,
    .destroy = kbootcap_destroy,
};

static const struct KObjectOps kbootcap_ops_ut = {
    .close = kbootcap_close,
    .destroy = kbootcap_destroy_ut,
};

struct KBootstrapCap *kbootcap_alloc_ports(uint32_t kind, uint16_t first,
                                           uint16_t last) {
    struct KBootstrapCap *cap;

    /* One authority, or no capability.  A zero kind authorises nothing and a
     * multi-bit kind is the monolith this stage retired, so both are refused
     * at the only place a boot capability can come into existence. */
    if (kind == 0u || (kind & (kind - 1u)) != 0u) return 0;
    /* An inverted range authorises nothing and would read as "everything" to
     * an unsigned comparison written the other way round. */
    if (first > last) return 0;

    cap = kslab_alloc((uint32_t)sizeof(struct KBootstrapCap));
    if (!cap) return 0;
    kobject_init(&cap->base, KOBJ_BOOTSTRAP_CAP, &kbootcap_ops);
    cap->kind       = kind;
    cap->port_first = first;
    cap->port_last  = last;
    return cap;
}

struct KBootstrapCap *kbootcap_alloc(uint32_t kind) {
    /* The whole port space.  Only boot calls this: everyone else derives from
     * what it holds, which is the point of the range being on the capability. */
    return kbootcap_alloc_ports(kind, 0u, 0xFFFFu);
}

/*
 * The same capability, charged to an Untyped the caller NAMED.
 *
 * This is the only form a ring-3 caller may reach.  `kbootcap_alloc_ports`
 * takes its object from the kernel slab, which is correct for the boot path —
 * bounded, once, before any Untyped exists — and would be a hole anywhere
 * else: a service that can make the kernel allocate is a service that can
 * exhaust it, and charter M3 says the kernel does not implicitly allocate
 * memory on somebody's behalf.  A narrowed control capability is memory, so
 * whoever asks for one pays for it, exactly as they do for the KIoPort the
 * authority goes on to create.
 */
struct KBootstrapCap *kbootcap_alloc_from(struct KUntyped *pool, uint32_t kind,
                                          uint16_t first, uint16_t last) {
    if (!pool) return 0;
    if (kind == 0u || (kind & (kind - 1u)) != 0u) return 0;
    if (first > last) return 0;

    struct KBootstrapCap *cap =
        kuntyped_alloc_child_top(pool, sizeof(struct KBootstrapCap));
    if (!cap) return 0;
    kobject_init_in_untyped(&cap->base, KOBJ_BOOTSTRAP_CAP, &kbootcap_ops_ut,
                            (uint32_t)sizeof(struct KBootstrapCap));
    cap->kind       = kind;
    cap->port_first = first;
    cap->port_last  = last;
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
