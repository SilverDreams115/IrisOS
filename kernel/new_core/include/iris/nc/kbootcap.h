/* SPDX-License-Identifier: Apache-2.0 */
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
/*
 * Authority over CPU TIME (ledger A-20).
 *
 * seL4 hands the root task one `SchedControl` capability per core, and
 * `seL4_SchedControl_Configure` is the ONLY way a budget and a period reach a
 * scheduling context — so time is something you are GIVEN, delegated the same
 * way an IRQ line or an I/O port is.  IRIS's `SYS_SC_CONFIGURE` used to need
 * only RIGHT_WRITE on the scheduling context itself, which meant anyone who
 * could retype one out of an Untyped they held could grant themselves any
 * budget over any period.  A capability model in which the CPU is the one
 * resource nobody has to be granted is not a capability model.
 */
#define IRIS_BOOTCAP_SCHED_CONTROL  (1u << 7)  /* SYS_SC_CONFIGURE */
/*
 * Authority to carve ADDRESS-SPACE IDENTIFIER pools (ledger A-21).
 *
 * seL4's `ASIDControl`: one capability, in BootInfo, whose only power is
 * making pools.  Holding an Untyped lets you build the object; holding this
 * lets the object issue identifiers, and an address space with no identifier
 * cannot be run in.  Splitting the two is what stops "I have memory" from
 * meaning "I may have as many address spaces as I like".
 */
#define IRIS_BOOTCAP_ASID_CONTROL   (1u << 8)  /* IRIS_KOBJ_ASID_POOL retype */
/*
 * Authority to place a thread in a scheduling DOMAIN.
 *
 * seL4's `seL4_CapDomain`, whose only invocation is `seL4_DomainSet_Set`.
 * Domains are the top-level time partition: the schedule says which domain
 * owns the CPU for how long, and a thread runs only while its own domain is
 * the current one — whatever its priority, and whatever any other domain's
 * threads are doing.
 *
 * It is a separate authority from priority for the reason it is separate in
 * seL4: priority orders threads that COMPETE, and a domain decides whether
 * they compete at all.  A holder of a TCB capability may order its threads
 * within the time it has been given; moving one into somebody else's time is
 * a different question, and this is the capability that answers it.
 */
#define IRIS_BOOTCAP_DOMAIN_CONTROL (1u << 9)  /* Domain_Set */

/*
 * Authority to name a DEVICE at all — Stage 10-dma, seL4's `seL4_IOSpace`.
 *
 * Every other capability in IRIS bounds what a THREAD may reach.  This one
 * bounds what a DEVICE may reach, which is the one thing an I/O port or IRQ
 * capability cannot express: a driver holding those can program a DMA-capable
 * device with any physical address, and the device writes there past every
 * check the kernel makes.
 *
 * It is separate from IOPORT_CONTROL and IRQ_CONTROL because it answers a
 * different question.  Those say which registers a driver may touch and which
 * line it may hear; this says which MEMORY the device behind them may reach.
 * A system that hands out the first two without this one has handed out all of
 * memory, and the split is what makes that visible rather than implied.
 *
 * Minting an IOSpace from it takes a SOURCE-ID — the bus:device:function that
 * rides on the device's DMA requests.  The kernel does not discover that; the
 * holder does, and passes it, which is why no PCI enumeration lives in the
 * kernel.
 */
#define IRIS_BOOTCAP_IOSPACE_CONTROL (1u << 10)  /* IOSpace_Create */

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
/* ...and the only form a ring-3 caller may reach: charged to a named Untyped,
 * because a service that can make the kernel allocate can exhaust it. */
struct KUntyped;
struct KBootstrapCap *kbootcap_alloc_from(struct KUntyped *pool, uint32_t kind,
                                          uint16_t first, uint16_t last);

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
