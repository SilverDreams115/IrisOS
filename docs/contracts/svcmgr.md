# `svcmgr` contract

## Purpose

Defines the current service-manager contract for discovery, runtime
publication, bootstrap delegation, supervision, and global status aggregation.

> The KChannel half of this contract — `SVCMGR_MSG_LOOKUP`, `LOOKUP_NAME`,
> `REGISTER`, `UNREGISTER`, `STATUS`, `DIAG` and the reply messages — was
> retired with KChannel itself in Phase 13. The opcodes stay defined in
> `iris/svcmgr_proto.h` as retirement witnesses; nothing serves them. What
> follows is the endpoint contract, which is the whole contract.

## Responsibilities

`svcmgr` currently owns:

- autostart of built-in userland services from the service catalog
- runtime service discovery for normal clients
- dynamic runtime publication of extra service endpoints
- service endpoint rights reduction for lookup replies
- service lifecycle supervision through `TCB_Watch` on the child's first
  thread (`SYS_PROCESS_WATCH` retired with the process object, Stage 7)
- bounded restart policy for autostart services
- global aggregated diagnostics over kernel and service-local status surfaces

`svcmgr` does not own:

- bootloader handoff
- ELF loading implementation
- low-level IRQ delivery implementation
- kernel object creation policy

## Bootstrap prerequisites

Everything `svcmgr` needs arrives as a **pre-start CSpace mint**, and `RBX` is
0 — the bootstrap channel went with KChannel:

- `IRIS_CPTR_SPAWN_CAP` — the spawn/hardware authority. Without it, `svcmgr`
  logs a fatal bootstrap error and exits.
- `IRIS_CPTR_OWN_EP` — the receive side of its own endpoint.
- `IRIS_CPTR_CONSOLE_EP` — the console it logs through.
- one `KIrqCap` per declared IRQ-routed service, one `KIoPort` per declared
  hardware-I/O service.

## Runtime endpoint model

Discovery is by name or by numeric id, over the endpoint. A published entry
is `(service_id, name, master cap, client_rights, owner_badge, generation)`.
Built-in entries come from the service catalog; dynamic ones are registered
at runtime and take ids ≥ 0x40.

## Wire format

A MessageInfo word plus message registers (ledger A-33) over `EP_Call` /
`Reply` — labels on `SYS_INVOKE` since ledger A-32. A request's opcode is the
message LABEL; `words[]` are the message registers; a name travels as the bulk
payload in the caller's registered IPC buffer; a capability travels in
`msg.cap` and arrives in the slot the receiver declared.

Replies are `IRIS_EP_REPLY_OK` or `IRIS_EP_REPLY_ERR` with
`words[0] = (uint32_t)iris_error_t`. Exactly one reply per request, malformed
ones included.

## Lookup contract — `IRIS_SVCMGR_EP_LOOKUP_NAME` (0xF001)

- Request: payload = NUL-terminated service name (`buf_len` includes the NUL).
- Reply OK: the endpoint cap in the caller's receive slot; `words[0]` =
  `service_id`.
- Unknown name → `IRIS_ERR_NOT_FOUND`, and no capability travels.

Rights granted: `RIGHT_WRITE` to an ordinary client. `RIGHT_DUPLICATE` and
`RIGHT_TRANSFER` — re-mint and forward authority — are granted only to a
supervisor badge (`iris_badge_is_supervisor()`: init, svcmgr, unbadged).

`IRIS_SVCMGR_EP_LOOKUP_ID` (0xF004) is the same lookup keyed on
`words[0] = service_id`.

## Dynamic publication contract

### `IRIS_SVCMGR_EP_REGISTER` (0xF002)

- Request: payload = NUL-terminated name; `msg.cap` = the endpoint capability
  to publish, transferred to svcmgr.
- Reply OK: `words[0]` = assigned `service_id`.
- The transferred capability is validated `KOBJ_ENDPOINT`; the badge the
  kernel delivered becomes the entry's `owner_badge`.
- Rejections, all of which release the transferred capability rather than
  leaking it: reserved name (`*.ep`, catalog names) → `ACCESS_DENIED`; no cap
  or wrong type → `INVALID_ARG`; name or id already taken → `BUSY`.

Registration is **cap-backed**, not possession-of-a-channel: a lookup returns
a same-object capability to what was registered, so publishing something
means handing over the real authority.

### `IRIS_SVCMGR_EP_UNREGISTER` (0xF003)

- Request: `words[0]` = `service_id`.
- Requires `badge == owner_badge`, or a supervisor badge.
- The stored capability is closed, which invalidates already-distributed
  derivations with `IRIS_ERR_CLOSED`; repeat unregister of a removed entry is
  a no-op.
- Automatic cleanup on publisher death is not implemented.

## Lifecycle contract (Phase 10)

### `IRIS_SVCMGR_EP_STATUS` (0xF005)

`words[0]` = service_id → `words[0]` = alive (1/0), `words[1]` = generation.
Open to any caller: it is a read-only liveness oracle, and it is what lets a
client poll a restart instead of blocking on a dead endpoint. The generation
bumps on every restart or revoke, so a client can detect a stale capability.

### `IRIS_SVCMGR_EP_RESTART` (0xF006)

`words[0]` = service_id → `words[0]` = the new generation. **Privileged**:
only a supervisor badge; anything else gets `ACCESS_DENIED`. Kills the
service and lets the watch-driven respawn path bring it back.

## Diagnostics contract — `IRIS_SVCMGR_EP_DIAG` (0xF007)

Open to any caller. Reply: `words[0]` = catalog service count, `[1]` = ready
services, `[2]` = active dynamic registrations, `[3]` = catalog version.

It is svcmgr-local by construction: four counters svcmgr already has, no
round-trip to anybody. The wider view is assembled by whoever wants it —
`vfs` answers `VFS_EP_OP_STATUS` on its own endpoint, and a client that needs
both makes both calls. `SYS_DIAG_SNAPSHOT` is not called; it was retired in
Phase 51. Service-local status remains the source of truth.

Unknown or malformed opcodes fail with `INVALID_ARG` — there is no silent
fallback anywhere (T068).

## Supervision and restart contract

For each tracked service slot, `svcmgr` stores the first thread's TCB, the
IRQ number, the service id and a short name.

On the death notification (Phase 13 / Track B — the kernel signals bit
`1 << service_id` on `svcmgr`'s death notification, so the bit index names the
exiting slot directly):

- the current master capabilities for that service are closed, which wakes
  blocked clients with `CLOSED` rather than leaving them queued on a corpse;
- the tracked thread reference is released;
- if the service is autostarted and restart budget remains, `svcmgr` respawns
  it and bumps the generation.

Restart policy is declarative: `autostart`, `restart_on_exit`,
`restart_limit`. Current built-in services: `kbd` and `vfs` restart up to 3
times; `sh` is autostarted with no restart budget.

## Current invariants

- `svcmgr` is the healthy-path discovery authority for `kbd`, `vfs`, and `sh`.
- `svcmgr` supervises service exit by notification, not by polling.
- Stale master endpoints are closed before replacement so blocked clients fail
  fast.
- Reserved names (`*.ep`, catalog names) are never runtime-registrable.
- `svcmgr` can aggregate health only if both kernel diagnostics and
  service-local status paths are functioning.

See [service-lifecycle.md](../service-lifecycle.md) for the badge policy this
contract enforces.
