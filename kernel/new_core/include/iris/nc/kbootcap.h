#ifndef IRIS_NC_KBOOTCAP_H
#define IRIS_NC_KBOOTCAP_H

#include <iris/nc/kobject.h>
#include <stdint.h>

#define KBOOTCAP_POOL_SIZE 0u

/*
 * Stage 5 Etapa 2: ONE CAPABILITY, ONE AUTHORITY.
 *
 * A boot capability used to be a monolith carrying a permission MASK — spawn,
 * hardware, debug and framebuffer authority on a single object, delegated
 * whole and narrowed by cloning a weaker copy (SYS_BOOTCAP_RESTRICT).  That is
 * not how a capability system delegates: seL4 has IRQControl, IOPortControl
 * and ASIDControl as separate objects, and you hand over the one you mean.
 *
 * Every boot capability now carries exactly one `kind`, and every kernel check
 * is exact equality (kbootcap_is).  A capability that merely CONTAINS an
 * authority cannot exist, so authorising by containment cannot come back: the
 * subset test and the clone-and-narrow syscall are both gone.
 */
#define IRIS_BOOTCAP_NONE           0u
#define IRIS_BOOTCAP_PROC_CONTROL   (1u << 0)  /* SYS_PROCESS_CREATE */
/* (1u << 1) was IRIS_BOOTCAP_HW_ACCESS: one bit that authorised BOTH IRQ and
 * ioport capability creation.  Split below; the bit is not reused. */
#define IRIS_BOOTCAP_DEBUG_CONTROL  (1u << 2)  /* SYS_KLOG_DRAIN / SYS_SCHED_INFO / SYS_POWEROFF */
#define IRIS_BOOTCAP_FB_CONTROL     (1u << 3)  /* SYS_FRAMEBUFFER_VMO (one-shot) */
#define IRIS_BOOTCAP_IRQ_CONTROL    (1u << 4)  /* SYS_CAP_CREATE_IRQCAP */
#define IRIS_BOOTCAP_IOPORT_CONTROL (1u << 5)  /* SYS_CAP_CREATE_IOPORT */
#define IRIS_BOOTCAP_INITRD_CONTROL (1u << 6)  /* SYS_INITRD_COUNT / SYS_INITRD_VMO */

struct KBootstrapCap {
    struct KObject base;
    uint32_t kind;             /* exactly one IRIS_BOOTCAP_* value */
};

struct KBootstrapCap *kbootcap_alloc(uint32_t kind);
void                  kbootcap_free(struct KBootstrapCap *cap);

/* The capability IS this authority, or it is not.  There is no containment
 * test: a capability carrying two authorities cannot be constructed, and a
 * check that accepted one would be re-introducing the monolith. */
static inline int kbootcap_is(const struct KBootstrapCap *cap, uint32_t kind) {
    return cap && cap->kind == kind;
}

#endif
