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

/*
 * Defines rather than an enum, for the same reason the syscall numbers are:
 * `services/kbd/main.S` is a driver written in assembly and it invokes
 * capabilities like everything else.  A label is ABI, and ABI has to be
 * readable by the assembler.
 */
#define INV_INVALID 0u


/* ── KOBJ_TCB ──────────────────────────────────────────────────────── */
#define INV_TCB_SUSPEND                     1u
#define INV_TCB_RESUME                      2u
#define INV_TCB_SET_PRIORITY                3u
#define INV_TCB_EXIT                        4u
#define INV_TCB_GET_INFO                    5u
#define INV_TCB_READ_REGS                   6u
#define INV_TCB_WRITE_REGS                  7u
#define INV_TCB_CONFIGURE                   8u
#define INV_TCB_WATCH                       9u
#define INV_TCB_SET_FAULT_HANDLER          10u
#define INV_TCB_SET_TIMEOUT_HANDLER        11u
#define INV_TCB_EXIT_CODE                  12u
#define INV_TCB_SET_IPC_BUFFER             13u
#define INV_TCB_BIND_NOTIFICATION          14u

/* ── KOBJ_ENDPOINT ─────────────────────────────────────────────────── */
#define INV_EP_SEND                        15u
#define INV_EP_NB_SEND                     16u
#define INV_EP_RECV                        17u
#define INV_EP_NB_RECV                     18u
#define INV_EP_CALL                        19u
#define INV_EP_CANCEL_BADGED_SENDS         20u
#define INV_EP_REPLY_RECV                  21u

/* ── KOBJ_NOTIFICATION ─────────────────────────────────────────────── */
#define INV_NOTIFY_SIGNAL                  22u
#define INV_NOTIFY_WAIT                    23u
#define INV_NOTIFY_POLL                    24u

/* ── KOBJ_REPLY ────────────────────────────────────────────────────── */
#define INV_REPLY_SEND                     25u

/* ── KOBJ_UNTYPED ──────────────────────────────────────────────────── */
#define INV_UNTYPED_INFO                   26u
#define INV_UNTYPED_QUERY                  27u
#define INV_UNTYPED_RESET                  28u
#define INV_UNTYPED_RETYPE                 29u
#define INV_UNTYPED_SET_DEVICE_BUDGET      30u

/* ── KOBJ_CNODE ────────────────────────────────────────────────────── */
#define INV_CNODE_DELETE                   31u
#define INV_CNODE_SWAP                     32u

/* ── KOBJ_SCHED_CONTEXT ────────────────────────────────────────────── */
#define INV_SC_BIND                        33u
#define INV_SC_CONSUMED                    34u
#define INV_SC_YIELD_TO                    35u
#define INV_SC_CONFIGURE                   36u
#define INV_SC_SET_ON_CALLER               37u

/* ── KOBJ_FRAME ────────────────────────────────────────────────────── */
#define INV_FRAME_MAP                      38u
#define INV_FRAME_UNMAP                    39u
#define INV_FRAME_SIZE                     40u

/* ── KOBJ_PAGE_TABLE ───────────────────────────────────────────────── */
#define INV_PAGE_TABLE_MAP                 41u

/* ── KOBJ_ASID_POOL ────────────────────────────────────────────────── */
#define INV_ASID_POOL_ASSIGN               42u

/* ── KOBJ_IRQ_CAP ──────────────────────────────────────────────────── */
#define INV_IRQ_SET_NOTIFICATION           43u
#define INV_IRQ_ACK                        44u
#define INV_IRQ_CLEAR                      45u

/* ── KOBJ_IOPORT ───────────────────────────────────────────────────── */
#define INV_IOPORT_IN                      46u
#define INV_IOPORT_OUT                     47u

/* ── KOBJ_BOOTSTRAP_CAP ────────────────────────────────────────────── */
/*
 * One capability type carrying several distinct authorities, told apart by a
 * tag the kernel checks (`kbootcap_is`).  The label says which METHOD; the tag
 * says whether this holder may ask.  Both are checked, and they are different
 * questions.
 */
#define INV_BOOT_FRAMEBUFFER_INFO          48u
#define INV_BOOT_INITRD_COUNT              49u
#define INV_BOOT_INITRD_FRAME              50u
#define INV_BOOT_IOPORT_NARROW             51u
#define INV_BOOT_CREATE_IOPORT             52u
#define INV_BOOT_CREATE_IRQCAP             53u
#define INV_BOOT_KLOG_DRAIN                54u
#define INV_BOOT_SCHED_INFO                55u
#define INV_BOOT_POWEROFF                  56u

/* ── the SLOT, whatever it holds ───────────────────────────────────── */
/*
 * What these act on is the slot rather than the object in it: copy this
 * capability, move it, revoke what came from it, ask what it is.  seL4
 * expresses them as CNode invocations, with the CNode as the object and
 * (index, depth) as arguments; IRIS invokes them on the slot directly.  That
 * difference is about WHICH object a method hangs off, not about whether a
 * method needs one, and A-31 records it rather than rounding it away.
 */
#define INV_CAP_IDENTIFY                   57u
#define INV_CAP_SAME_OBJECT                58u
#define INV_CSPACE_MINT                    59u
#define INV_CSPACE_MOVE                    60u
#define INV_CSPACE_REVOKE                  61u
#define INV_CSPACE_SET_GUARD               62u

/* First unassigned.  A label is never reused, for the same reason a syscall
 * number never was: a stale caller must get a refusal, not somebody else's
 * method. */
#define INV_LABEL_COUNT                    63u

#endif /* IRIS_INVOKE_H */
