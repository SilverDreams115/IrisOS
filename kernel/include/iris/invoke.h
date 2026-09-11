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
 * A method therefore cannot be named without naming the object it acts on,
 * which is the property; the syscall NUMBER, which named a method and nothing
 * else, is what IRIS is retiring.
 *
 * THE LABELS ARE GLOBALLY UNIQUE, AND THAT IS seL4'S ARRANGEMENT
 *
 * The first cut of this header scoped labels to the invoked TYPE, so that
 * `TCB_Suspend` and `EP_Send` were both 1 and the pair named the method.  That
 * is not what seL4 does: `enum invocation_label` is one flat list —
 * `UntypedRetype`, `TCBSuspend`, `CNodeRevoke`, `IRQIssueIRQHandler` — with
 * arch-specific labels continuing after the generic ones.  seL4's kernel still
 * switches on the capability's type first, but that switch is there to reach
 * the decoder that knows how to read the arguments, not to disambiguate a
 * number that would otherwise be ambiguous.
 *
 * Unique labels are not merely more faithful here, they are cheaper, and the
 * reason is specific to this kernel.  A scoped label space forces the
 * dispatcher to learn the type before it can choose the method, which means a
 * CSpace walk that the method then repeats when it resolves the capability
 * properly.  Two walks per invocation, forever, for a disambiguation seL4 does
 * not need.  With unique labels the dispatcher routes on the label alone and
 * the method does the ONE walk it always did.
 *
 * So WHERE is the type checked?  Where it always was: inside the method, by
 * the resolver that fetches the capability with the type it requires.  A label
 * sent to the wrong kind of capability gets `IRIS_ERR_WRONG_TYPE` — which says
 * what is actually wrong, and which the kernel only became able to say
 * consistently at A-30.  That is a better answer than seL4's
 * `IllegalOperation` for the same mistake, and it costs nothing extra because
 * the check was already being made.
 *
 * ARGUMENTS
 *
 * An invocation is `(cptr, label, a1, a2, a3)`.  Three method arguments is
 * what the widest existing operation needs — `Untyped_Retype`,
 * `TCB_Configure`, `Frame_Map` — and it is why the syscall entry grew a fifth
 * register.
 *
 * This header is shared by the kernel and ring 3 on purpose: a label is ABI.
 * A label is never reused, for the same reason a syscall number never was.
 */

enum iris_invocation_label {
    INV_INVALID = 0,

    /* ── KOBJ_TCB ──────────────────────────────────────────────────────── */
    INV_TCB_SUSPEND = 1,
    INV_TCB_RESUME,
    INV_TCB_SET_PRIORITY,
    INV_TCB_EXIT,
    INV_TCB_GET_INFO,
    INV_TCB_READ_REGS,
    INV_TCB_WRITE_REGS,
    INV_TCB_CONFIGURE,
    INV_TCB_WATCH,
    INV_TCB_SET_FAULT_HANDLER,
    INV_TCB_SET_TIMEOUT_HANDLER,
    INV_TCB_EXIT_CODE,
    INV_TCB_SET_IPC_BUFFER,
    INV_TCB_BIND_NOTIFICATION,

    /* ── KOBJ_ENDPOINT ─────────────────────────────────────────────────── */
    INV_EP_SEND,
    INV_EP_NB_SEND,
    INV_EP_RECV,
    INV_EP_NB_RECV,
    INV_EP_CALL,
    INV_EP_CANCEL_BADGED_SENDS,
    INV_EP_REPLY_RECV,

    /* ── KOBJ_NOTIFICATION ─────────────────────────────────────────────── */
    INV_NOTIFY_SIGNAL,
    INV_NOTIFY_WAIT,
    INV_NOTIFY_POLL,

    /* ── KOBJ_REPLY ────────────────────────────────────────────────────── */
    INV_REPLY_SEND,

    /* ── KOBJ_UNTYPED ──────────────────────────────────────────────────── */
    INV_UNTYPED_INFO,
    INV_UNTYPED_QUERY,
    INV_UNTYPED_RESET,
    INV_UNTYPED_RETYPE,
    INV_UNTYPED_SET_DEVICE_BUDGET,

    /* ── KOBJ_CNODE ────────────────────────────────────────────────────── */
    INV_CNODE_DELETE,
    INV_CNODE_SWAP,

    /* ── KOBJ_SCHED_CONTEXT ────────────────────────────────────────────── */
    INV_SC_BIND,
    INV_SC_CONSUMED,
    INV_SC_YIELD_TO,
    INV_SC_CONFIGURE,
    INV_SC_SET_ON_CALLER,

    /* ── KOBJ_FRAME ────────────────────────────────────────────────────── */
    INV_FRAME_MAP,
    INV_FRAME_UNMAP,
    INV_FRAME_SIZE,

    /* ── KOBJ_PAGE_TABLE ───────────────────────────────────────────────── */
    INV_PAGE_TABLE_MAP,

    /* ── KOBJ_ASID_POOL ────────────────────────────────────────────────── */
    INV_ASID_POOL_ASSIGN,

    /* ── KOBJ_IRQ_CAP ──────────────────────────────────────────────────── */
    INV_IRQ_SET_NOTIFICATION,
    INV_IRQ_ACK,
    INV_IRQ_CLEAR,

    /* ── KOBJ_IOPORT ───────────────────────────────────────────────────── */
    INV_IOPORT_IN,
    INV_IOPORT_OUT,

    /*
     * ── KOBJ_BOOTSTRAP_CAP ─────────────────────────────────────────────
     * One capability type carrying several distinct authorities, told apart by
     * a tag the kernel checks (`kbootcap_is`).  The label says which METHOD;
     * the tag says whether this holder may ask.  Both are checked, and they
     * are different questions.
     */
    INV_BOOT_FRAMEBUFFER_INFO,
    INV_BOOT_INITRD_COUNT,
    INV_BOOT_INITRD_FRAME,
    INV_BOOT_IOPORT_NARROW,
    INV_BOOT_CREATE_IOPORT,
    INV_BOOT_CREATE_IRQCAP,
    INV_BOOT_KLOG_DRAIN,
    INV_BOOT_SCHED_INFO,
    INV_BOOT_POWEROFF,

    /*
     * ── the SLOT, whatever it holds ────────────────────────────────────
     * What these act on is the slot rather than the object in it: copy this
     * capability, move it, revoke what came from it, ask what it is.  seL4
     * expresses them as CNode invocations, with the CNode as the object and
     * (index, depth) as arguments; IRIS invokes them on the slot directly.
     * That difference is about WHICH object a method hangs off, not about
     * whether a method needs one, and it is recorded in A-31 rather than
     * rounded away.
     */
    INV_CAP_IDENTIFY,
    INV_CAP_SAME_OBJECT,
    INV_CSPACE_MINT,
    INV_CSPACE_MOVE,
    INV_CSPACE_REVOKE,
    INV_CSPACE_SET_GUARD,

    INV_LABEL_COUNT   /* first unassigned; never reuse a retired label */
};

#endif /* IRIS_INVOKE_H */
