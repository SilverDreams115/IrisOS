# Service lifecycle, death/relookup & badge policy (Fase 10)

Fase 10 turns the Fase 9 sender **identity** (kernel-stamped badges) into
real **policy**: badge-authenticated registration, tightened `.ep` lookup
grants, a liveness/generation oracle, real death→respawn recovery, and a
notification close-while-wait guarantee. It builds entirely on existing
kernel primitives (`SYS_PROCESS_KILL/WATCH/STATUS`, endpoint/notification
close) — no memory-model or namespace changes.

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

`svcmgr` enforces, on **both** transports:

- **Reserved names** — `<image>.ep` endpoint names and catalog service names
  (`vfs`, `kbd`, `sh`, …) can never be registered at runtime
  (`IRIS_ERR_ACCESS_DENIED`). This is the anti-spoofing rule that keeps a
  looked-up `vfs.ep` authoritative.
- **EP REGISTER** (`0xF002`) is **badge-authenticated and cap-backed** (Fase 11):
  the caller transfers its service endpoint in `IrisMsg.attached_cap`; svcmgr
  validates it is an endpoint and stores the real cap, so `LOOKUP_NAME` returns
  a usable cap. The kernel-stamped `sender_badge` becomes the `owner_badge`. A
  REGISTER without a cap, or with a wrong-type cap, fails. (The legacy KChannel
  REGISTER remains only as a compatibility boundary, `owner_badge = 0`.)
- **EP UNREGISTER** (`0xF003`) requires `sender_badge == owner_badge` (or a
  supervisor) — a client cannot unregister another identity's service.

## `.ep` lookup grant tightening

`IRIS_SVCMGR_EP_LOOKUP_NAME` previously returned `WRITE|DUPLICATE` to every
caller. Now an ordinary client receives a **call-only** cap (`RIGHT_WRITE`);
`RIGHT_DUPLICATE`/`RIGHT_TRANSFER` (re-mint/forward authority) is granted
only to supervisor badges. The **legacy KChannel lookup** keeps the wide
grant for bootstrap re-minting (init mints `vfs.ep`/`kbd.ep` into children),
preserving T046.

## Death model, generation & STATUS oracle

Each catalog service carries a `generation` (1 at first boot). The existing
`SYS_PROCESS_WATCH` path already respawns a service on exit; Fase 10 bumps
`generation` on every respawn. `IRIS_SVCMGR_EP_STATUS` (`0xF005`, open to any
caller) maps a name to `{alive, generation}` and is the **non-blocking
liveness oracle** that lets a client poll a restart without blocking on a
possibly-dead endpoint.

## Real restart (RESTART) + relookup

`IRIS_SVCMGR_EP_RESTART` (`0xF006`, **supervisor only**) calls
`SYS_PROCESS_KILL` on the service; the watch path respawns it and bumps the
generation. A client recovers by: poll `STATUS` (bounded, no sleep — each
EP_CALL yields the CPU) until `alive && generation > cached`, then re-LOOKUP
to obtain a fresh cap. Demonstrated end-to-end by T057/T060 killing and
recovering VFS.

## Revocation (initial / logical)

Fase 10 implements **logical revocation by generation**: caps are not
force-closed (handle-table entries hold active refs, so an endpoint with live
client caps cannot be kernel-closed). Instead the supervisor's registry is
the source of truth — a client validates freshness against `STATUS`; a cached
generation older than the current one is **stale** and must be relooked up
(T059). Full CSpace recursive revocation is deferred.

## Notification close-while-wait

`knotification` wakes **all** blocked waiters and clears the waiter array on
close, so a waiter observes `IRIS_ERR_CLOSED` (no leak, no deadlock,
idempotent). Covered by a dedicated host test (`test_knotification.c`); the
kbd IRQ-notification path is unaffected.

## Legacy svcmgr loop (compatibility boundary)

The KChannel `SVCMGR_MSG_*` loop is **retained** (T001–T012, T046). It is now
classified as a compatibility boundary: reserved-name policy applies to it,
registrations are `owner_badge = 0`, and authenticated lifecycle ops live on
the EP path. Full retirement awaits a later phase.

## What remains (debt)

Formal/recursive revocation; full svcmgr legacy-loop retirement; cap-backed
authenticated REGISTER over EP (needs an EP_CALL cap-transfer extension);
init deconstruction; KChannel retirement; SMP IPC; fuzzing.
