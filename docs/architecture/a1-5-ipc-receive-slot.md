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
- The slot must be **empty at declaration time** (fail-fast: `IRIS_ERR_BUSY`
  before the endpoint is touched, so a failed declaration never involves the
  sender and never consumes the sender's cap).
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

- Empty required.  Occupied at declaration → `IRIS_ERR_BUSY`, fail-fast,
  endpoint untouched, sender untouched.  **No overwrite, ever** — the
  install primitive (`kcnode_install_empty_badged`) is check-and-install
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

- sender must hold `RIGHT_TRANSFER` (`syscall_ipc_stage_cap_badged`);
- `rights_reduce(sender_rights, requested)` is what the slot receives —
  `receiver_rights ⊆ sender_rights`, never escalated;
- the installed slot's rights gate every later CPtr invocation through the
  A1 dual resolvers;
- **badge**: the staged badge (kernel-read from the sender's handle) is
  installed verbatim in the slot (`kcnode_install_empty_badged`), same
  preservation contract as handle delivery.  `sender_badge` stamping is
  untouched — receive-slot never lets userland forge a badge.

## Failure atomicity

| Failure | When | Outcome |
|---|---|---|
| slot occupied / invalid / no root CNode | declaration (recv entry) | recv syscall fails `BUSY`/`INVALID_ARG`/`NOT_FOUND` before touching the endpoint; sender (if any) stays queued with its staged cap; nothing consumed |
| sender lacks `RIGHT_TRANSFER` / rights reduce to none | staging (send entry) | unchanged: send fails, sender keeps its handle |
| slot filled between declaration and delivery | delivery | fallback to handle materialization; discriminator tells the receiver |
| receiver's handle table full on fallback | delivery | unchanged soft failure: cap destroyed, field = `IRIS_MSG_NO_CAP` |
| receiver killed while blocked with declared slot | death | `task_cancel_blocked_waits` dequeues it; no delivery ever starts; slot stays empty (no ghost); declaration dies with the task |
| sender killed while staged | death | unchanged: cancel path releases the staged cap (T073/T077 semantics) |
| endpoint closed while receiver waits | close | unchanged: receiver wakes with `IRIS_ERR_CLOSED`; slot stays empty |
| reply-slot declared but reply carries no cap | reply | declaration is simply unused; every recv-family entry rewrites the field, so it cannot leak into a later operation |

Install is all-or-nothing under the CNode spinlock; the staging ref is
released only after a successful install (slot holds its own refs) or
passed to the legacy handle path.

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
  `BUSY` fail-fast; slot 300 → `INVALID_ARG`; the blocked sender is
  unharmed and a follow-up good declaration receives its cap intact.
- **T087** — EP_CALL: caller transfers a cap into the server's slot 38;
  reply transfers a cap into the caller's declared reply-slot 39; the
  reply cap itself stays a one-shot handle (`>= 1024`, second reply →
  `NOT_FOUND`, T074 contract).
- **T088** — death cleanup: receiver thread killed while blocked with a
  declared slot → slot has no ghost (resolve fails), endpoint clean, and a
  later real transfer into the same slot works; endpoint-close variant
  wakes the receiver with `CLOSED` and leaves the slot empty.

Host tests: `kcnode_install_empty_badged` primitive (empty install, BUSY on
occupied with slot content untouched, out-of-range, refcount/badge/rights).
