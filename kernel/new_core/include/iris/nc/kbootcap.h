#ifndef IRIS_NC_KBOOTCAP_H
#define IRIS_NC_KBOOTCAP_H

#include <iris/nc/kobject.h>
#include <stdint.h>

#define KBOOTCAP_POOL_SIZE 0u

/*
 * Stage 5 Etapa 2: a boot capability is being split from a monolith carrying a
 * permission MASK into a set of capabilities each carrying exactly ONE
 * authority.  The two models live in the same field during the split:
 *
 *   • migrated authorities are checked with kbootcap_is (exact equality) —
 *     the cap IS the IRQ-control cap, or it is not;
 *   • the not-yet-migrated bits are still checked with kbootcap_allows
 *     (subset), which is what makes a monolith a monolith.
 *
 * kbootcap_allows and SYS_BOOTCAP_RESTRICT retire together when the last bit
 * moves: with one authority per capability there is nothing left to narrow —
 * you delegate the capability you mean, and revoke it through the CDT.
 */
#define IRIS_BOOTCAP_NONE          0u
#define IRIS_BOOTCAP_SPAWN_SERVICE (1u << 0)
/* (1u << 1) was IRIS_BOOTCAP_HW_ACCESS: one bit that authorised BOTH IRQ and
 * ioport capability creation.  Split below; the bit is not reused. */
#define IRIS_BOOTCAP_DEBUG_CONTROL  (1u << 2)  /* SYS_KLOG_DRAIN / SYS_SCHED_INFO / SYS_POWEROFF */
#define IRIS_BOOTCAP_FRAMEBUFFER   (1u << 3)  /* may call SYS_FRAMEBUFFER_VMO (one-shot) */
#define IRIS_BOOTCAP_IRQ_CONTROL    (1u << 4)  /* SYS_CAP_CREATE_IRQCAP only */
#define IRIS_BOOTCAP_IOPORT_CONTROL (1u << 5)  /* SYS_CAP_CREATE_IOPORT only */

struct KBootstrapCap {
    struct KObject base;
    uint32_t permissions;
};

struct KBootstrapCap *kbootcap_alloc(uint32_t permissions);
void                  kbootcap_free(struct KBootstrapCap *cap);
struct KBootstrapCap *kbootcap_clone_restricted(const struct KBootstrapCap *src,
                                                uint32_t new_permissions);

static inline int kbootcap_allows(const struct KBootstrapCap *cap, uint32_t permissions) {
    return cap && (cap->permissions & permissions) == permissions;
}

/* Exact match: the capability carries this authority AND NOTHING ELSE.  A
 * monolith that happens to include the bit does not pass — which is the point:
 * once an authority is split out, the only cap that authorises it is the one
 * that exists for it. */
static inline int kbootcap_is(const struct KBootstrapCap *cap, uint32_t kind) {
    return cap && cap->permissions == kind;
}

#endif
