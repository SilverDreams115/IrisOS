# IRIS — IPC Model (Phase S1)

Synchronous endpoints (rendezvous), asynchronous notifications and explicit
seL4-MCS-style reply objects. Complements `a1-5-ipc-receive-slot.md` and
`ipc-stress-invariants.md` (invariants I1–I18, which S1 preserves).

## Endpoint

No change to the rendezvous semantics, staged caps (A1.10 two-phase), badges
(Phase 9) or bulk (Ph69). What changes is the object's ORIGIN: only
`UntypedRetype`.

## Explicit reply objects (S1)

The kernel NO LONGER fabricates a KReply per Call. The model:

```
server:  UntypedRetype(…, KOBJ_REPLY, …)      → reply cap in its CSpace
server:  EP_Recv(ep, recv_slot, reply_cptr)
kernel:  stage (exclusive claim; BUSY if already staged/bound)
rendezvous with EP_Call:
         bind(caller); the reply object is delivered in the receive's
         capability register, and the caller's gift lands in recv_slot
server:  Reply(reply_cptr, msg)
kernel:  one-shot per binding; the OBJECT returns to free and is reusable
```

- A recv with no reply (reply cptr = 0) cannot serve Calls: the Call fails
  `NOT_SUPPORTED` without consuming anything (the blocked receiver stays
  queued; a queued call-mode sender is not dequeued). S22: the legacy path
  creates no hidden objects.
- A server that "parks" a reply while it keeps serving uses TWO reply objects
  and alternates (kbd: slots 13/14).
- Caller death → unbind (object reusable; `Reply` → NOT_FOUND).
- The reply's last cap deleted with a caller bound → the caller wakes CLOSED.
- A supervisor that mints the reply into a child must DROP its own copy: a
  retained copy would suppress close-wakes-caller on the child's death.

ABI (ledgers A-32, A-33): every one of these is a label on `SYS_INVOKE`, and a
message is a MessageInfo word plus message registers — `INV_EP_RECV` /
`INV_EP_NB_RECV` take the receive slot in a1 and the reply object in a2;
`INV_EP_CALL` and `INV_EP_SEND` take MessageInfo in a1, mr0..mr3 in a2..a5, a
transferred capability in a6 and a receive slot in a7; `INV_EP_REPLY_RECV`
takes the reply object in the capability word, because it is one operation and
needs both halves' arguments at once. A receive returns badge, MessageInfo and
the delivered capability alongside its message registers, so a server's loop
shuffles nothing.

## Notification

No semantic change (signal/wait, pending bits, IRQ delivery, shared pager
notification; timed waits went to ring 3 in A-24). Origin: only
`UntypedRetype`. The notification quota was retired: creating notifications
requires Untyped + slots.

## Counters

`iris_ipc_stat_reply_caps` counts reply BINDINGS (one per Call rendezvous),
preserving the exact balances of I16–I18 and of T109/T110.
