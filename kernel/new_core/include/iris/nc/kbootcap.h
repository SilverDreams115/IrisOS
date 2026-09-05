#ifndef IRIS_NC_KBOOTCAP_H
#define IRIS_NC_KBOOTCAP_H

#include <iris/nc/kobject.h>
#include <stdint.h>

#define KBOOTCAP_POOL_SIZE 0u

/*
 * Stage 5 Step 2: ONE CAPABILITY, ONE AUTHORITY.
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
    /*
     * The I/O port range this capability authorises — meaningful only for
     * IRIS_BOOTCAP_IOPORT_CONTROL, and the reason the kernel no longer has a
     * device policy.
     *
     * Which ports a system may claim used to be a table in `syscall_priv.h`:
     * PS/2, the two serial ports, and QEMU's ACPI block.  The kernel had no
     * basis for that list — it is a fact about a machine and about who is
     * trusted with what on it, and neither is the kernel's to know.  Worse, it
     * was one list for everybody: init, svcmgr and the test suite all held the
     * same unrestricted control capability, so "svcmgr cannot claim CMOS" was
     * true only because the kernel happened to say so, and "svcmgr may claim
     * the serial port" was true for exactly the same reason.
     *
     * Now the range travels ON the capability.  Boot issues one covering the
     * whole port space to the root task, and a holder DERIVES a narrower
     * control capability for whoever it delegates to.  The containment check
     * is against the authority the caller actually holds, so confinement is
     * something a supervisor decides and can prove, rather than something the
     * kernel asserts on everyone's behalf.
     *
     * seL4's `IOPortControl` is unranged and confines by who holds it at all.
     * This is that, plus the ability to hand out a piece — which is what the
     * whitelist was imitating without being able to say who it applied to.
     */
    uint16_t port_first;
    uint16_t port_last;
};

/* A control capability over the whole port space (what boot issues). */
struct KBootstrapCap *kbootcap_alloc(uint32_t kind);
/* ...and one over a sub-range, derived from a holder's own. */
struct KBootstrapCap *kbootcap_alloc_ports(uint32_t kind, uint16_t first,
                                           uint16_t last);

/* Is [base, base+count) inside what this capability authorises?  Non-IOPORT
 * kinds carry no range and answer 0: a range question about a capability that
 * is not about ranges has no true answer. */
static inline int kbootcap_ports_contain(const struct KBootstrapCap *cap,
                                         uint16_t base, uint16_t count) {
    if (!cap || cap->kind != IRIS_BOOTCAP_IOPORT_CONTROL || count == 0u)
        return 0;
    uint32_t last = (uint32_t)base + (uint32_t)count - 1u;
    return (uint32_t)base >= (uint32_t)cap->port_first &&
           last <= (uint32_t)cap->port_last;
}
void                  kbootcap_free(struct KBootstrapCap *cap);

/* The capability IS this authority, or it is not.  There is no containment
 * test: a capability carrying two authorities cannot be constructed, and a
 * check that accepted one would be re-introducing the monolith. */
static inline int kbootcap_is(const struct KBootstrapCap *cap, uint32_t kind) {
    return cap && cap->kind == kind;
}

#endif
