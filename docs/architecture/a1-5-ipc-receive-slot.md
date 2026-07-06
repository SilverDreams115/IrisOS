# A1.5 IPC receive-slot design

Status: ACCEPTED — implemented in this phase.  Companion to
`a1-authority-namespace-endgame.md`: after A1 made every persistent object
CSpace-invocable, IPC cap *delivery* was the largest remaining producer of
handles.  A1.5 adds an **optional, receiver-declared receive-slot** so a
transferred cap can land directly in the receiver's CSpace.

## Semantics

The **receiver** declares, per receive operation, an empty direct slot of its
root CNode: *"if this receive delivers a transferred cap, install it there."*

- Declaration is per-call and consumed by (at most) one delivery.
- The declared slot must be a **direct root-CNode slot** (single-level CPtr,
  `1..slot_count-1`); multi-level receive paths are out of scope
  (`IRIS_ERR_INVALID_ARG`).
- The slot must be **empty at declaration time** (fail-fast:
  `IRIS_ERR_ALREADY_EXISTS` before the endpoint is touched, so a failed
  declaration never involves the sender and never consumes the sender's cap).
- The **sender is oblivious**: nothing changes on the send side; staging
  (RIGHT_TRANSFER check, `rights_reduce`, badge capture) is identical.

Declaration points:

| Syscall | Declares via | Meaning |
|---|---|---|
| `SYS_EP_RECV` / `SYS_EP_NB_RECV` | input-hint field `msg.attached_cap` | slot for the cap a sender attached (EP_SEND/NB_SEND `attached_handle`, or EP_CALL `attached_cap`) |
| `SYS_EP_CALL` | input field `msg.attached_handle` | slot for a cap the **reply** transfers back |

## Message ABI

**No layout change.**  `struct IrisMsg` stays 80 bytes; no field added; no
syscall number or signature changes.  Two previously-dead input values are
given meaning, and the A1 namespace split (`<1024` CPtr / `>=1024` handle)
is reused as the output discriminator:

- `EP_RECV`/`EP_NB_RECV` already read the user msg as *hints* before
  blocking (`buf_uptr` since Ph69).  `attached_cap` is now a second hint:
  - `0` → legacy (no receive-slot);
  - `1..1023` → receive-slot declaration;
  - `>= 1024` → **ignored** (treated as 0).  Rationale: receivers that reuse
    a msg buffer without zeroing may have a stale *output* value there, and
    outputs are always `0` or a handle `>= 1024` — never `1..1023` — so no
    legacy pattern can accidentally declare a slot.
- `EP_CALL` today *rejects* `attached_handle != 0` with `INVALID_ARG`
  (the field is reserved for the reply cap).  Now: `1..1023` declares the
  reply receive-slot; `>= 1024` keeps failing with `INVALID_ARG` exactly as
  before.  No legacy caller is affected (they were forced to pass 0).

Output discriminator (both `attached_handle` and `attached_cap`):

```text
0        → no cap delivered
1..1023  → cap installed in the receiver's CSpace at this CPtr
>= 1024  → cap materialized as a handle (legacy / fallback)
```

`receive_slot = 0` ⇒ bit-for-bit legacy behavior.

## Slot policy

- Empty required.  Occupied at declaration → `IRIS_ERR_ALREADY_EXISTS` (canonical occupied-slot error in IRIS, used instead of the generic BUSY), fail-fast,
  endpoint untouched, sender untouched.  **No overwrite, ever** — the
  install primitive (`kcnode_mint_excl_badged`, the SYS_PROC_CSPACE_MINT backend) is check-and-install
  under the CNode lock, unlike `kcnode_mint*` which has MOVE/overwrite
  semantics.
- Out-of-range slot (or CPTR-namespace value that is not a direct root
  slot) → `IRIS_ERR_INVALID_ARG` at declaration.
- No root CNode (OOM-degraded process) → `IRIS_ERR_NOT_FOUND`.
- TOCTOU window: the receiver process (another thread, or its supervisor
  via `SYS_PROC_CSPACE_MINT`) can fill the slot between declaration and
  delivery.  If install fails *at delivery time*, the cap **falls back to
  handle materialization** — the receiver detects the location via the
  `<1024`/`>=1024` discriminator.  This is a delivery-location degradation,
  not an authority fallback: no rights change, no authority is lost, no
  ghost cap.  (It mirrors the pre-existing soft-failure philosophy where a
  full handle table delivers `IRIS_MSG_NO_CAP`.)

## Rights

Unchanged from the existing transfer machinery, which already runs before
any receive-slot logic:

- sender must hold `RIGHT_TRANSFER` (`syscall_ipc_stage_cap_peek_badged`);
- `rights_reduce(sender_rights, requested)` is what the slot receives —
  `receiver_rights ⊆ sender_rights`, never escalated;
- the installed slot's rights gate every later CPtr invocation through the
  A1 dual resolvers;
- **badge**: the staged badge (kernel-read from the sender's handle) is
  installed verbatim in the slot (`kcnode_mint_excl_badged`, the SYS_PROC_CSPACE_MINT backend), same
  preservation contract as handle delivery.  `sender_badge` stamping is
  untouched — receive-slot never lets userland forge a badge.

## Failure atomicity

| Failure | When | Outcome |
|---|---|---|
| slot occupied / invalid / no root CNode | declaration (recv entry) | recv syscall fails `ALREADY_EXISTS`/`INVALID_ARG`/`NOT_FOUND` before touching the endpoint; sender (if any) stays queued with its staged cap; nothing consumed |
| sender lacks `RIGHT_TRANSFER` / rights reduce to none | staging (send entry) | unchanged: send fails, sender keeps its handle |
| slot filled between declaration and delivery | delivery | fallback to handle materialization; discriminator tells the receiver |
| receiver's handle table full on fallback | delivery | unchanged soft failure: cap destroyed, field = `IRIS_MSG_NO_CAP` |
| receiver killed while blocked with declared slot | death | `task_cancel_blocked_waits` dequeues it; no delivery ever starts; slot stays empty (no ghost); declaration dies with the task |
| sender killed while staged | death | A1.10: cancel path releases the staging ref only; the source handle is never consumed (it dies with the process table, or stays valid if only the thread dies) |
| endpoint closed while receiver waits | close | unchanged: receiver wakes with `IRIS_ERR_CLOSED`; slot stays empty |
| endpoint closed while a SENDER waits with a staged cap | close | A1.10: sender wakes with `IRIS_ERR_CLOSED` and KEEPS its source cap (staging ref released, handle untouched) |
| reply-slot declared but reply carries no cap | reply | declaration is simply unused; every recv-family entry rewrites the field, so it cannot leak into a later operation |
| reply loses the one-shot race (`NOT_FOUND`) | reply | A1.10: nothing delivered → the server keeps its source cap (pre-A1.10 this destroyed it) |

Install is all-or-nothing under the CNode spinlock; the staging ref is
released only after a successful install (slot holds its own refs) or
passed to the legacy handle path.

## A1.10 — staged-cap atomicity contract (two-phase staging)

A1.9 fixed EP_NB_SEND consuming the source cap on a failed delivery and
left the blocking paths as documented remaining work.  A1.10 closes that:
**every** transfer path now stages in two phases:

```
peek    — validate RIGHT_TRANSFER, rights_reduce, capture badge, retain
          the object; the sender's handle is NOT touched.
commit  — close the source handle, ONLY once delivery is committed
          (a receiver is determined).
```

Rule: `peek/retain != consume` — the source cap is consumed exactly when
the delivery is committed, never before.  Commit points per path:

| Path | Commit point | Non-delivery exits (source kept) |
|---|---|---|
| `EP_NB_SEND` | receiver dequeued (A1.9) | `CLOSED`, `WOULD_BLOCK` |
| `EP_SEND` immediate rendezvous | receiver dequeued, before routed delivery | `CLOSED` at entry |
| `EP_SEND` queued sender | the RECEIVER commits when it takes the staged cap (`EP_RECV`/`EP_NB_RECV`), outside `ep->lock` | endpoint close, waiter cancel (kill) |
| `EP_CALL` immediate rendezvous | receiver dequeued, before routed delivery | `CLOSED` at entry (pre-A1.10 this also leaked the staging ref) |
| `EP_CALL` queued caller | receiver take, as above | endpoint close, waiter cancel |
| `SYS_REPLY` | blocked caller determined under `rp->lock` | staging errors, lost one-shot race (`NOT_FOUND`) |

Mechanics: a queued sender carries the un-consumed source handle in
`task->ep_cap_src_h` next to the staged object (`ep_cap_obj`).  The
receiver's take clears both and commits via
`handle_table_close(&sender->process->handle_table, src_h)` — done
**outside** `ep->lock`, because a handle close can fire object-close
callbacks that themselves take endpoint locks (the `ht->lock → ep->lock`
order must never invert).  `kendpoint_obj_close` and
`kendpoint_cancel_waiter` release the staging ref and clear
`ep_cap_src_h` without consuming; cleanup is idempotent (double cancel
is a no-op).

The commit is generation-guarded (`handle_table_close` on a stale id is
a benign no-op), so a same-process thread racing a close of the source
handle degrades the move into a copy of authority the process already
held — same acceptance as A1.9.  Note the peek window for a *blocking*
send lasts until rendezvous, so the source handle stays visible in the
sender's table while it is queued; two concurrent transfers of the same
handle by the same process can then both deliver (documented limitation,
`RIGHT_TRANSFER` is checked on both).

Consequences locked by tests:
- a canceled/closed blocking send or call is *fully* undone: source cap
  intact and usable, no ghost cap anywhere, no staged-ref leak;
- `SYS_REPLY` to an already-invoked KReply returns `NOT_FOUND` and no
  longer destroys the server's attached cap (the only intentional
  consume-without-install left is the receiver-table-full soft failure
  above, where the *message* was delivered);
- the consume-at-stage helpers (`syscall_ipc_stage_cap`,
  `syscall_ipc_stage_cap_badged`) are retired — do not reintroduce.

## Compatibility

- No declaration ⇒ exact legacy behavior (attached handle `>= 1024`).
- Reply caps: **completely untouched**.  The KReply is created and inserted
  into the server's handle table by the same code as before and always
  arrives as a handle in `attached_handle`; receive-slot routing only
  applies to `syscall_ipc_deliver_cap*` (transferred caps), never to the
  KReply insertion sites.
- Fastpaths (`ep_send_fastpath` / `ep_recv_fastpath`) only handle cap-less
  messages — no routing needed, untouched.

## Tests

- **T084** — basic: sender thread EP_SENDs an endpoint cap; receiver
  declares slot 36 → cap lands as CPtr, invocable (`EP_NB_SEND` →
  `WOULD_BLOCK` proves resolution+rights); legacy recv without slot still
  yields a handle.
- **T085** — rights reduction: notification cap transferred WRITE-only into
  slot 37 → `NOTIFY_SIGNAL` by CPtr works, `NOTIFY_WAIT` → `ACCESS_DENIED`.
- **T086** — occupied/invalid atomicity: declaring occupied slot 36 →
  `ALREADY_EXISTS` fail-fast; slot 300 → `INVALID_ARG`; the blocked sender
  is unharmed and a follow-up good declaration receives its cap intact.
- **T087** — EP_CALL: caller transfers a cap into the server's slot 38;
  reply transfers a cap into the caller's declared reply-slot 39; the
  reply cap itself stays a one-shot handle (`>= 1024`, second reply →
  `NOT_FOUND`, T074 contract).
- **T088** — death cleanup: receiver thread killed while blocked with a
  declared slot → slot has no ghost (resolve fails), endpoint clean, and a
  later real transfer into the same slot works; endpoint-close variant
  wakes the receiver with `CLOSED` and leaves the slot empty.
- **T103** — (A1.10) sender blocked in `EP_SEND` with a staged cap,
  endpoint closed → `CLOSED`, source cap intact and still functional,
  exact handle balance.
- **T104** — (A1.10) caller blocked in `EP_CALL` with `attached_cap`,
  endpoint closed → `CLOSED`, source cap intact, reply-caps counter flat
  (no ghost KReply).
- **T105** — (A1.10) reply-cap one-shot race: second `SYS_REPLY` with an
  attached cap → `NOT_FOUND`, server keeps its cap; first reply intact.
- **T106** — (A1.10) endpoint close with two queued staged senders → both
  `CLOSED`, both source caps survive, each staging ref released exactly
  once.

Host tests: `kcnode_mint_excl_badged` primitive (empty install, ALREADY_EXISTS on
occupied with slot content untouched, out-of-range, refcount/badge/rights).

## A1.6 — in-tree adoption

Receive-slots are service practice, not just a kernel/test mechanism.
Userland declares and discriminates through `iris/ipc_recv_slot.h`
(input-only helpers over the protocol above; no new surface).

- **svcmgr** declares a receive-slot on every discovery-endpoint recv:
  a REGISTER-transferred endpoint cap lands directly in its root CNode
  (registration pool: slots 64..255, one per live dynamic service) and
  is stored CSpace-canonically (`public_cptr`).  LOOKUP materializes a
  per-request ephemeral master via `SYS_CSPACE_RESOLVE` (the sanctioned
  bridge) for the reduced-rights DUP and closes it; UNREGISTER releases
  the pool slot with `SYS_CNODE_DELETE`.  svcmgr finds its own
  root-CNode handle (inserted by `kprocess_create` as every process's
  first handle) by a startup type probe; probe failure or pool
  exhaustion degrades to the legacy handle path.  A cap attached to a
  non-REGISTER opcode is discarded in both landing modes (previously it
  leaked into the handle table).
- **init**'s productive `vfs.ep` lookup declares a reply receive-slot
  (slot 16 of the free 16..29 per-process pool); the session cap lands
  in init's CSpace and every boot-path VFS EP_CALL invokes it by CPtr.
  The three supervisor lookups stay on handle delivery deliberately:
  they feed `SYS_PROC_CSPACE_MINT` as source handles (handle-layer
  working set by design).
- **vfs** needs no change: its endpoints already arrive as pre-start
  CSpace mints (Fase 8) and the only caps it receives are KReply caps
  (ephemeral by design).  Its *clients* adopt receive-slots (init, T091).
- **Tests**: T089 (svcmgr CSpace-backed registration lifecycle + slot
  reuse via a legacy client), T090 (reply-slot lookup lands as CPtr;
  occupied slot fails fast ALREADY_EXISTS; NOT_FOUND leaves the slot
  empty), T091 (vfs.ep by reply-slot + real VFS READ_AT by CPtr),
  T092 (slotless client still gets the legacy handle).  Suite: 88/88.

Remaining handle producers after adoption: object-creation returns,
self-references, handle-layer ops, reply caps (permanent), the
sanctioned `SYS_CSPACE_RESOLVE`/`SYS_CNODE_FETCH` bridges, supervisor
mint-source lookups, and slotless legacy clients (sh's kbd/console use
is pre-start mints already; dynamic clients choose per call).
