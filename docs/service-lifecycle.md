# Service lifecycle, death/relookup & badge policy (Phase 10)

Phase 10 turns the Phase 9 sender **identity** (kernel-stamped badges) into
real **policy**: badge-authenticated registration, tightened `.ep` lookup
grants, a liveness/generation oracle, real death→respawn recovery, and a
notification close-while-wait guarantee. It builds entirely on existing
kernel primitives (at the time `SYS_PROCESS_KILL/WATCH/STATUS`, endpoint and
notification close) — no memory-model or namespace changes.

> **Naming note (Stage 7).**  The policy below is unchanged, but the primitives
> it names are: a supervisor watches, kills and reads the exit code of the
> **thread** it holds — `SYS_TCB_WATCH`, `SYS_TCB_EXIT`, `SYS_TCB_EXIT_CODE`,
> `SYS_TCB_GET_INFO` — because `svc_load_minted_ws` hands back the child's
> first thread and there is no process object to name.  Read
> `SYS_PROCESS_KILL/WATCH/STATUS` below as their `TCB` equivalents.

## Badge-based service identity

Reserved badges (`iris/endpoint_proto.h`):

| Badge | Entity | Authority |
|---|---|---|
| `IRIS_BADGE_NONE` (0) | unbadged bootstrap / legacy masters (init pre-identity) | **supervisor** |
| `IRIS_BADGE_INIT` (0x111) | init | **supervisor** |
| `IRIS_BADGE_SVCMGR` (0x110) | svcmgr | **supervisor** |
| `IRIS_BADGE_KBD/VFS/SH` (0x101–0x103) | core catalog services | client |
| `IRIS_BADGE_CONSOLE` (0x104) | console | client |
| `IRIS_BADGE_IRIS_TEST` (0x1F0) | test harness | client |
| `IRIS_BADGE_DYNAMIC_BASE` (0x200+) | runtime-registered services | client |

`iris_badge_is_supervisor(badge)` is the single authority predicate: only
supervisors may receive re-mintable caps from `.ep` lookups or drive
privileged lifecycle ops (RESTART).

## REGISTER / UNREGISTER policy

`svcmgr` enforces:

- **Reserved names** — `<image>.ep` endpoint names and catalog service names
  (`vfs`, `kbd`, `sh`, …) can never be registered at runtime
  (`IRIS_ERR_ACCESS_DENIED`). This is the anti-spoofing rule that keeps a
  looked-up `vfs.ep` authoritative.
- **EP REGISTER** (`0xF002`) is **badge-authenticated and cap-backed** (Phase 11):
  the caller transfers its service endpoint with the message (`msg.cap`, in
  the capability argument word since ledger A-33); svcmgr validates it is an
  endpoint and stores the real cap, so `LOOKUP_NAME` returns a usable cap.
  The kernel-delivered badge becomes the `owner_badge`. A REGISTER without a
  cap, or with a wrong-type cap, fails.
- **EP UNREGISTER** (`0xF003`) requires `badge == owner_badge` (or a
  supervisor) — a client cannot unregister another identity's service.

## `.ep` lookup grant tightening

`IRIS_SVCMGR_EP_LOOKUP_NAME` previously returned `WRITE|DUPLICATE` to every
caller. Now an ordinary client receives a **call-only** cap (`RIGHT_WRITE`);
`RIGHT_DUPLICATE`/`RIGHT_TRANSFER` (re-mint/forward authority) is granted
only to supervisor badges — init, which mints `vfs.ep`/`kbd.ep` into
children, holds one (T046).

## Death model, generation & STATUS oracle

Each catalog service carries a `generation` (1 at first boot). The exit-watch
path (`SYS_TCB_WATCH` on the child's first thread) respawns a service on exit; Phase 10 bumps
`generation` on every respawn. `IRIS_SVCMGR_EP_STATUS` (`0xF005`, open to any
caller) maps a name to `{alive, generation}` and is the **non-blocking
liveness oracle** that lets a client poll a restart without blocking on a
possibly-dead endpoint.

## Real restart (RESTART) + relookup

`IRIS_SVCMGR_EP_RESTART` (`0xF006`, **supervisor only**) calls `SYS_TCB_EXIT`
on the service's thread; the watch path respawns it and bumps the
generation. A client recovers by: poll `STATUS` (bounded, no sleep — each
EP_CALL yields the CPU) until `alive && generation > cached`, then re-LOOKUP
to obtain a fresh cap. Demonstrated end-to-end by T057/T060 killing and
recovering VFS.

## Revocation (initial / logical)

Phase 10 implements **logical revocation by generation**: caps are not
force-closed (a CSpace slot holds an active ref, so an endpoint with live
client caps cannot be kernel-closed). Instead the supervisor's registry is
the source of truth — a client validates freshness against `STATUS`; a cached
generation older than the current one is **stale** and must be relooked up
(T059).

Recursive revocation is no longer deferred: the native CSpace CDT/MDB landed in
Phase S3, and `SYS_CSPACE_REVOKE` destroys a capability's whole descendance
across CNodes and address spaces.  Generation-based staleness remains the
supervision policy because it answers a different question — *is this service
the one I looked up?* — and it is svcmgr's to answer, not the kernel's.

## Notification close-while-wait

`knotification` wakes **all** blocked waiters and clears the waiter array on
close, so a waiter observes `IRIS_ERR_CLOSED` (no leak, no deadlock,
idempotent). Covered by a dedicated host test (`test_knotification.c`); the
kbd IRQ-notification path is unaffected.

## Legacy svcmgr loop (retired)

The KChannel `SVCMGR_MSG_*` loop was the second transport this document was
written to describe. It went with KChannel itself in Phase 13: there is one
endpoint protocol, every registration carries a real `owner_badge`, and every
number that loop used answers `NOT_SUPPORTED` — as does every other retired
number, since ledger A-32 left exactly three (`EXIT`, `YIELD`, `CLOCK_GET`).

## What remains (debt)

Formal/recursive revocation; init deconstruction; SMP IPC; fuzzing.
