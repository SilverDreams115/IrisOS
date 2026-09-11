# IRIS IPC Architecture

IRIS has one IPC mechanism for messages — the **KEndpoint**, a synchronous
rendezvous — and one for signals, the **KNotification**. Both are seL4's.

This document described two mechanisms for most of its life: KChannel, an
asynchronous ring buffer, was the original and is fully retired (Phase 13). Its
syscall numbers are permanently reserved and answer `NOT_SUPPORTED`. What
follows is what exists.

## How an operation is named

Since ledger **A-32** there is one syscall for everything that acts on an
object:

```c
SYS_INVOKE(cptr, label, a1, a2, a3)
```

The capability says WHAT is being acted on, the label says WHICH method, and
neither can be given without the other. `EP_Send`, `EP_Recv`, `EP_Call`,
`Reply` and `ReplyRecv` are labels like any other — `kernel/include/iris/invoke.h`
is the list. Three syscall numbers survive (`EXIT`, `YIELD`, `CLOCK_GET`) and
each is there because it invokes nothing, which is why seL4 keeps `seL4_Yield`.

## How a message is carried

Since ledger **A-33** a message is a **MessageInfo word plus message
registers**, and a payload longer than that lives in the sending thread's
registered IPC buffer. There is no message struct in the ABI and no pointer to
one.

```
MessageInfo:  bits  0.. 3  length   — message words carried, 0..4
              bits  4..11  caps     — on a send, 1 if a capability travels;
                                       on a receive, the RIGHTS it landed with
              bits 12..24  buf      — bulk payload bytes in the IPC buffer
              bits 25..63  label    — the application's
```

`kernel/include/iris/ipc_msg.h` holds the layout and the register map, and is
shared by the kernel, the C services and the assembler — `kbd` is a driver
written in assembly and reads a message the same way C does.

`services/common/iris_msg.h` provides `struct iris_msg` and `iris_msg_send` /
`_recv` / `_call` / `_reply` / `_reply_recv`. That struct is ring-3
marshalling, the way seL4's `seL4_MessageInfo_t` and `seL4_SetMR` are; the
kernel has never seen it and holds no pointer to it.

What that buys: there is no address on the message path, so there is nothing to
validate and nothing for a second thread to unmap between the check and the
copy; and a short message never touches memory at either end.

### Bulk payloads

A payload lives in the page the sending thread registered with
`TCB_SetIPCBuffer` (ledger D-4), and the message carries a **length**, not an
address. **Both ends need a registered buffer**: the payload is copied from the
sender's page to the receiver's, so two threads of one process each need their
own. A thread with no buffer cannot send a payload at all — that is seL4's
answer and now IRIS's, and it replaced 256 bytes of kernel staging inside every
TCB that the user did not choose and could not name.

### Capability transfer

A message carries at most one capability, in `msg.cap` with `msg.cap_rights`.
The kernel stages it: the sender must really hold it, the rights are reduced to
what was asked for, and the badge is preserved, so the number a sender writes
is never delivered as written.

Since **A-29** the transfer is a **COPY**. The sender keeps what it sent and
the receiver's capability is a revocable derivation child of the sender's slot.
Giving a capability away is send-then-delete — two steps that both belong to
the sender.

A receiver says where a capability should land in `msg.recv_slot` and learns
what arrived in `msg.got_caps`: the RIGHTS it landed with, or zero for nothing.
Zero is unambiguous because a capability with no rights cannot be transferred
at all. It is not told WHERE, because it declared the slot.

A **Call** hands the receiving side two capabilities — the caller's gift, in
the declared slot, and the reply object it is now owed, in `msg.got_cap`. They
shared one field until A-33 gave each its own.

## Reply objects (KReply)

`EP_Call` blocks the caller and hands the receiver a reply capability, which is
**one-shot**: the second `Reply` on the same object answers `NOT_FOUND`, because
by then it may be bound to a different caller and a second reply would answer
somebody else's call with this one's payload.

A reply may be **deferred**: a server can stash the reply object and answer
later. `kbd` does this for blocking key reads — `KBD_EP_OP_READ` parks the
reply and answers it from the next IRQ scancode. The caller stays blocked until
that reply, or wakes with `CLOSED` if the object is torn down.

A reply always delivers `sender_badge = 0`. Reply identity is implied by the
one-shot object, never by a badge a server could write.

`ReplyRecv` is seL4's combined operation, and a passive server needs it: it
must not cross the gap between giving its donated time back and blocking again.
It carries no capability on the reply half, and the syscall enforces that — a
server's loop hands back the words its receive delivered, and those include the
reply object it must not give away.

## Close semantics

When the last capability to an endpoint goes away, every blocked sender and
receiver wakes with `IRIS_ERR_CLOSED`, staged capabilities are released without
being consumed, and the queue is cleared.

`EP_CancelBadgedSends` (ledger A-25) cancels the in-flight sends of ONE badge.
Without it, revoking a badged delegation stopped a client sending anything new
and left whatever it had already queued to be delivered afterwards —
revocation with a tail.

## Rights

| Right | Effect |
|-------|--------|
| `RIGHT_WRITE` | `EP_Send`, `EP_NBSend`, `EP_Call` |
| `RIGHT_READ` | `EP_Recv`, `EP_NBRecv` |
| `RIGHT_TRANSFER` | required on a capability being handed over |

## Notifications

A `KNotification` carries signal BITS, OR-ed together — not counts, and not
messages. `IRQ_SetNotification` routes a hardware line to one: the kernel masks
the line, signals `1 << irq` from interrupt context (no allocation, nothing
that can block) and the driver drains its device through its `KIoPort`
capability and re-arms with `IRQ_Ack`.

`TCB_BindNotification` (ledger A-23) is what lets ONE thread be a driver: a
thread blocked receiving on an endpoint still takes signals, and they arrive as
a message labelled `IRIS_MSG_LABEL_NOTIFICATION`. Without it a server had to
poll its endpoint and then sleep on its notification — a hundred wakeups a
second to find nothing.

## Faults

A fault is a message (ledger A-22). A faulting thread CALLs the endpoint its
supervisor registered with `TCB_SetFaultHandler`, the handler receives the
record as an ordinary message with a reply capability, and replying resumes the
thread. The badge on the endpoint capability says which target faulted.
Budget exhaustion is a separate registration (`TCB_SetTimeoutHandler`), because
a temporal supervisor is not the pager.

## Waiting on time

There is none in the kernel. `SLEEP`, `CLOCK_NANOSLEEP` and
`NOTIFY_WAIT_TIMEOUT` are retired (ledger A-24): a bounded wait is a request to
the ring-3 **timer service**, carrying a capability to the notification it
should signal — so waiting is an authority that can be refused, which a syscall
number never was.
