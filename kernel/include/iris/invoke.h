#ifndef IRIS_INVOKE_H
#define IRIS_INVOKE_H
/*
 * invoke.h — the invocation label space (ledger A-31).
 *
 * THE SHAPE THIS ADOPTS
 *
 * seL4 has a handful of real syscalls and they are all IPC.  Everything else —
 * configuring a thread, retyping memory, routing an interrupt — is an
 * INVOCATION: a message sent to a capability, whose LABEL names the method.
 * The kernel resolves the capability, switches on its TYPE, and then switches
 * on the label.  A method therefore cannot be reached without naming the
 * object it acts on, and the same label means different things on different
 * kinds of capability, because it is scoped to the type.
 *
 * IRIS has always checked authority that way — every live syscall already
 * resolves a CPtr and checks rights on it — but it SELECTED the method with a
 * global syscall number, which is the one thing about its shape that is not
 * seL4's.  This header is where that stops.
 *
 * THE LABEL SPACE
 *
 * Two ranges, and the split is not cosmetic:
 *
 *   1..0xFF      per-TYPE methods.  The number alone means nothing; the pair
 *                (type, label) names the method.  INV_TCB_SUSPEND and
 *                INV_EP_SEND are both 1, and that is the point — it is what
 *                makes the type check load-bearing rather than decorative.
 *
 *   0x100..0x1FF GENERIC methods, valid on a capability of ANY type, because
 *                what they act on is the SLOT rather than the object in it:
 *                copy this capability, move it, revoke what came from it, ask
 *                what it is.  seL4 expresses these as CNode invocations, with
 *                the CNode as the object and (index, depth) as arguments;
 *                IRIS invokes them on the slot directly.  That difference is
 *                real and is recorded rather than hidden — see A-31 — but it
 *                is a difference about WHICH object a method hangs off, not
 *                about whether a method needs one.
 *
 * ARGUMENTS
 *
 * An invocation is `(cptr, label, a1, a2, a3)`.  Three method arguments is
 * what the widest existing operation needs (`Untyped_Retype`, `TCB_Configure`,
 * `Frame_Map`), and it is why the syscall entry grew a fifth register.
 *
 * This header is shared by the kernel and ring 3 on purpose: a label is ABI.
 */

/* ── KOBJ_TCB ───────────────────────────────────────────────────────────── */
#define INV_TCB_SUSPEND               1u
#define INV_TCB_RESUME                2u
#define INV_TCB_SET_PRIORITY          3u
#define INV_TCB_EXIT                  4u
#define INV_TCB_GET_INFO              5u
#define INV_TCB_READ_REGS             6u
#define INV_TCB_WRITE_REGS            7u
#define INV_TCB_CONFIGURE             8u
#define INV_TCB_WATCH                 9u
#define INV_TCB_SET_FAULT_HANDLER    10u
#define INV_TCB_SET_TIMEOUT_HANDLER  11u
#define INV_TCB_EXIT_CODE            12u
#define INV_TCB_SET_IPC_BUFFER       13u
#define INV_TCB_BIND_NOTIFICATION    14u

/* ── KOBJ_ENDPOINT ──────────────────────────────────────────────────────── */
#define INV_EP_SEND                   1u
#define INV_EP_NB_SEND                2u
#define INV_EP_RECV                   3u
#define INV_EP_NB_RECV                4u
#define INV_EP_CALL                   5u
#define INV_EP_CANCEL_BADGED_SENDS    6u
#define INV_EP_REPLY_RECV             7u

/* ── KOBJ_NOTIFICATION ──────────────────────────────────────────────────── */
#define INV_NOTIFY_SIGNAL             1u
#define INV_NOTIFY_WAIT               2u
#define INV_NOTIFY_POLL               3u

/* ── KOBJ_REPLY ─────────────────────────────────────────────────────────── */
#define INV_REPLY_SEND                1u

/* ── KOBJ_UNTYPED ───────────────────────────────────────────────────────── */
#define INV_UNTYPED_INFO              1u
#define INV_UNTYPED_QUERY             2u
#define INV_UNTYPED_RESET             3u
#define INV_UNTYPED_RETYPE            4u
#define INV_UNTYPED_SET_DEVICE_BUDGET 5u

/* ── KOBJ_CNODE ─────────────────────────────────────────────────────────── */
#define INV_CNODE_DELETE              1u
#define INV_CNODE_SWAP                2u

/* ── KOBJ_SCHED_CONTEXT ─────────────────────────────────────────────────── */
#define INV_SC_BIND                   1u
#define INV_SC_CONSUMED               2u
#define INV_SC_YIELD_TO               3u
#define INV_SC_CONFIGURE              4u
#define INV_SC_SET_ON_CALLER          5u

/* ── KOBJ_FRAME ─────────────────────────────────────────────────────────── */
#define INV_FRAME_MAP                 1u
#define INV_FRAME_UNMAP               2u
#define INV_FRAME_SIZE                3u

/* ── KOBJ_PAGE_TABLE ────────────────────────────────────────────────────── */
#define INV_PAGE_TABLE_MAP            1u

/* ── KOBJ_ASID_POOL ─────────────────────────────────────────────────────── */
#define INV_ASID_POOL_ASSIGN          1u

/* ── KOBJ_IRQ_CAP ───────────────────────────────────────────────────────── */
#define INV_IRQ_SET_NOTIFICATION      1u
#define INV_IRQ_ACK                   2u
#define INV_IRQ_CLEAR                 3u

/* ── KOBJ_IOPORT ────────────────────────────────────────────────────────── */
#define INV_IOPORT_IN                 1u
#define INV_IOPORT_OUT                2u

/* ── KOBJ_VSPACE ────────────────────────────────────────────────────────── */
/* (none yet: a VSpace is named BY the mapping invocations, never invoked) */

/*
 * ── KOBJ_BOOTSTRAP_CAP ─────────────────────────────────────────────────────
 * One capability type, several distinct authorities told apart by a tag the
 * kernel checks (`kbootcap_is`).  The label says which METHOD; the tag on the
 * capability says whether this holder may ask.  Both are checked.
 */
#define INV_BOOT_FRAMEBUFFER_INFO     1u
#define INV_BOOT_INITRD_COUNT         2u
#define INV_BOOT_INITRD_FRAME         3u
#define INV_BOOT_IOPORT_NARROW        4u
#define INV_BOOT_CREATE_IOPORT        5u
#define INV_BOOT_CREATE_IRQCAP        6u
#define INV_BOOT_KLOG_DRAIN           7u
#define INV_BOOT_SCHED_INFO           8u
#define INV_BOOT_POWEROFF             9u

/* ── generic: the SLOT, whatever it holds ───────────────────────────────── */
#define INV_GENERIC_BASE          0x100u
#define INV_CAP_IDENTIFY          0x100u
#define INV_CAP_SAME_OBJECT       0x101u
#define INV_CSPACE_MINT           0x102u
#define INV_CSPACE_MOVE           0x103u
#define INV_CSPACE_REVOKE         0x104u
#define INV_CSPACE_SET_GUARD      0x105u
#define INV_GENERIC_LAST          0x105u

#endif /* IRIS_INVOKE_H */
