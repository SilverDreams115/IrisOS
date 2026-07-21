# IRIS — IPC Model (Fase S1)

Synchronous endpoints (rendezvous), asynchronous notifications and explicit
seL4-MCS-style reply objects. Complements `a1-5-ipc-receive-slot.md` and
`ipc-stress-invariants.md` (invariants I1–I18, which S1 preserves).

## Endpoint

No change to the rendezvous semantics, staged caps (A1.10 two-phase), badges
(Fase 9) or bulk (Ph69). What changes is the object's ORIGIN: only
`SYS_UNTYPED_RETYPE2`.

## Explicit reply objects (S1)

The kernel NO LONGER fabricates a KReply per EP_CALL. The model:

```
server:  RETYPE2(…, KOBJ_REPLY, …)   → reply cap in its CSpace
server:  SYS_EP_RECV(ep, msg, reply_cptr)   ← new arg2
kernel:  stage (exclusive claim; BUSY if already staged/bound)
rendezvous with EP_CALL:
         bind(caller); msg.attached_handle = reply_cptr (echoed)
server:  SYS_REPLY(msg.attached_handle, reply)
kernel:  one-shot per binding; the OBJECT returns to free and is reusable
```

- A recv with no reply (arg2 = 0) cannot serve CALLs: the CALL fails
  `NOT_SUPPORTED` without consuming anything (the blocked receiver stays
  queued; a queued call-mode sender is not dequeued). S22: the legacy path
  creates no hidden objects.
- A server that "parks" a reply while it keeps serving uses TWO reply objects
  and alternates (kbd: slots 13/14).
- Caller death → unbind (object reusable; SYS_REPLY → NOT_FOUND).
- The reply's last cap deleted with a caller bound → the caller wakes CLOSED.
- A supervisor that mints the reply into a child must DROP its own copy: a
  retained copy would suppress close-wakes-caller on the child's death.

ABI: `SYS_EP_RECV`/`SYS_EP_NB_RECV` arg2 passes the reply CPtr (0 = none).
Justified, documented change: some in-tree callers passed uninitialized
garbage in rdx through 2-arg wrappers; all migrated to explicit 3-arg
wrappers. `SYS_REPLY` does not change (dual-resolve of the echoed value).
`SYS_EP_CALL` does not change.

## Notification

No semantic change (signal/wait/timeout, pending bits, IRQ delivery, shared
pager notification). Origin: only RETYPE2. The notification quota was retired:
creating notifications requires Untyped + slots.

## Counters

`iris_ipc_stat_reply_caps` counts reply BINDINGS (one per CALL rendezvous),
preserving the exact balances of I16–I18 and of T109/T110.
