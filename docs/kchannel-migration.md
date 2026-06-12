# KChannel Migration Guide

KChannel is the legacy IPC mechanism in IRIS (ring buffer, 128 msgs × 84 bytes). This document tracks the migration to KEndpoint and documents the debt classification for each KChannel user.

## Current inventory (as of Fase 7.3/7.6 + Fase 8 start)

The table below lists every service that calls `SYS_CHAN_*` in production code
(counts are raw `SYS_CHAN_` occurrences per service tree).

| Service | Call sites | Role of KChannel | Migration stage |
|---------|-----------|-----------------|----------------|
| `svcmgr` | 15 | Bootstrap delivery, dynamic registration, service activation, legacy main loop, legacy console writes | **Fase 7.5** state + 7.3/8: receives the console EP send side (kind 0x22) and publishes `"console.ep"`; mints the discovery EP into every child's root CNode (`SYS_PROC_CSPACE_MINT`, slot 1); still the last legacy console **writer** |
| `vfs` | 2 | Bootstrap receive only | **Fase 7.5 DONE** — endpoint-only service; **7.3**: console output over `"console.ep"` (`[VFS] console ep OK`) |
| `sh` | 1 | Bootstrap receive only | **Fase 7.4 DONE (kbd)**; **7.3**: console output over `"console.ep"` (`[SH] console ep OK`); **Fase 8**: svcmgr discovery via CPtr slot 1 (`[SH] svcmgr cptr OK`) |
| `init` | 61 | Console bootstrap; svcmgr channel; kbd legacy probes (S2/S7); IPC selftests; `.ep` spoof-register probe | **Fase 7.2 (S5/S6)**; **7.3**: init creates/owns the console endpoint and logs through it when up (`[USER] console ep OK`); **Fase 8**: mints CPtr fixtures into iris_test (slots 1/2/3) |
| `console` | 4 | Bootstrap receive; legacy receive path (svcmgr writes + SYNC v2) | **Fase 7.3 DONE** — endpoint-first loop (`CONSOLE_EP_OP_WRITE`/`SYNC`/PING, see `docs/console-endpoint.md`); EP writes are synchronous (per-write flush barrier); legacy path remains only for svcmgr |
| `fb` | 1 | Bootstrap receive | Pending |
| `userboot` | 1 | Single `SYS_CHAN_SEND` to child bootstrap channel | Pending |
| `kbd` | 5 | Legacy HELLO/STATUS/SUBSCRIBE probes (non-blocking drain), bootstrap | **Fase 7.4 + 7.6 DONE** — events to sh via `"kbd.ep"` (ring + parked KReply); IRQ1 arrives as a KNotification signal (kind 0x23, `SYS_NOTIFY_WAIT_TIMEOUT`); `KBD_MSG_IRQ_SCANCODE` no longer dispatched |

`iris_test` KChannel tests (T001–T012) are regression tests for the KChannel syscall layer; they are kept indefinitely.

## Coexistence rules (Fase 7 / 7.1 constraint)

1. `handle_id_t` and `KChannel` must not be removed until **all** production callers are migrated.
2. svcmgr creates its KEndpoint (`state->ep_h`) **before** `svcmgr_autostart_services` so every catalog service receives the ep cap in its bootstrap bag. Fase 7.1 adds per-service endpoints (`own_service_ep`, bootstrap kind 0x21) created lazily in `boot_service` and kept across restarts.
3. Services that have not migrated continue to use KChannel unchanged. The svcmgr main loop drains EP requests first (NB), then falls through to `SYS_CHAN_RECV_TIMEOUT(10ms)` for legacy clients. The VFS uses the same pattern (`vfs_ep_drain` burst, then `SYS_CHAN_RECV_TIMEOUT(5ms)`), and degrades to a plain blocking legacy loop if its endpoint dies (`[VFS] ep lost`).
4. **Fase 7.2 update:** the VFS endpoint path is now *mandatory* for sh and init. sh prints `[SH] vfs ep FAILED` (and the smoke gate fails) when the lookup fails — there is no silent fallback. init exits with code 5 if `"vfs.ep"` cannot be resolved.
5. Both lookup paths (EP and legacy) must resolve `".ep"` names identically; dynamic registration of `".ep"` names is rejected (runtime-tested: init S4 register-side probe + iris_test T031 lookup-side probe). This includes `"console.ep"` (Fase 7.3), which is bootstrap-delivered by init (kind 0x22), never runtime-registered.
6. **Fase 8 update:** CPtr-first discovery coexists with handle-based discovery. svcmgr mints its discovery EP into every child's root CNode (slot `IRIS_CPTR_SVCMGR_EP` = 1) *in addition to* the kind-0x20 bootstrap cap; consumers that adopt the CPtr gate loudly (`[SH] svcmgr cptr OK`). The mint is non-fatal so non-CSpace children keep booting.

## Debt classification

### Class A — svcmgr legacy path (Fase 7 debt)

svcmgr still uses KChannel for:
- `SVCMGR_MSG_LOOKUP_NAME` / `SVCMGR_MSG_REGISTER` / `SVCMGR_MSG_UNREGISTER` in the main switch-case loop.
- Dynamic service public channel management (`svcmgr_send_dynamic_reply`).
- Service activation probes (`svcmgr_activate_service`).

The EP path (`svcmgr_handle_ep_request`) handles the same operations but over KEndpoint. Both paths are maintained until all callers switch to `IRIS_SVCMGR_EP_LOOKUP_NAME`. The legacy path should be removed once `init`, `sh`, and other callers have migrated.

### Class B — service bootstrap phase (all services)

Every service receives bootstrap parameters via `SYS_CHAN_RECV` on a one-shot bootstrap channel. This pattern is used by: `console`, `fb`, `vfs`, `sh`, and `svcmgr` itself.

The bootstrap channel is inherently short-lived (one receive, then closed). Migration here means replacing the bootstrap message format with an EP_CALL to svcmgr or a structured boot capability object. Lower priority than the operational loops.

### Class C — operational request/reply loops (Fase 7.1: VFS/SH migrated)

`vfs` and `sh` used KChannel for every file operation (`SYS_CHAN_SEND` +
`SYS_CHAN_RECV` on a reply channel). Fase 7.1 moved the operational path to
`SYS_EP_CALL` + `SYS_REPLY`:

- **Server**: `services/vfs/vfs_ep.c` implements the stateless protocol of
  `iris/vfs_ep_proto.h` (LIST / STAT / READ_AT / PING) — see
  `docs/vfs-endpoint.md`. One reply per request, validated payloads,
  host-unit-tested.
- **Client**: sh's `ls` and `cat` issue EP_CALLs against `"vfs.ep"`
  (endpoint-only since Fase 7.2 — the legacy fallback was removed and svcmgr
  no longer forwards the vfs service/reply channels to sh).
- **init (Fase 7.2)**: the S5/S6 healthy-path probes use the stateless EP
  protocol (LIST / STAT / READ_AT), resolved through `"svcmgr.ep"` →
  `"vfs.ep"`, fail-fast with no legacy fallback.
- **Remaining legacy Class C debt**: the VFS stateful channel protocol
  (`iris/vfs_proto.h` OPEN/READ/CLOSE) now has zero production clients; the
  server loop in `vfs.c` stays alive purely as compatibility until Fase 7.5
  removes it (a stateful EP replacement stays blocked on sender identity).

### Class D — keyboard event channel (sh ← kbd: RESOLVED in Fase 7.4)

The `sh ← kbd` event KChannel is gone: sh pulls one event per
`EP_CALL(KBD_EP_OP_READ)` on `"kbd.ep"`; kbd buffers scancodes in a 16-deep
ring and parks the per-call KReply when the ring is empty, answering it from
the next IRQ scancode (see `docs/kbd-endpoint.md`).

Class D residue after Fase 7.6:
- ~~kernel IRQ routing delivers `KBD_MSG_IRQ_SCANCODE` over KChannel~~ —
  **RESOLVED (Fase 7.6)**: `SYS_IRQ_ROUTE_REGISTER` now accepts a
  KNotification destination; svcmgr owns the master (catalog flag
  `irq_notify = 1`), the kernel signals `1 << irq` from IRQ context and kbd
  waits on `SYS_NOTIFY_WAIT_TIMEOUT` (see `docs/kbd-endpoint.md`);
- init S2/S7 probes and the svcmgr STATUS query still use kbd's legacy
  channel pair (init unsubscribes after S7 so sh is the only key consumer).

### Class E — console write path (Fase 7.3: endpoint-first)

The console serves `CONSOLE_EP_OP_WRITE` / `SYNC` / PING over its endpoint
(`"console.ep"`, created by init since console is init-spawned — see
`docs/console-endpoint.md`). init, sh, vfs and iris_test write through it;
EP writes are synchronous, so each call is its own flush barrier (the
Fase 7.1 interleaving class of bugs cannot recur on this path). Remaining
legacy writer: svcmgr (retires with its legacy loop).

## Migration steps for a Class C service (procedure as executed for vfs, Fase 7.1)

1. Add `#include <iris/endpoint_proto.h>` and `#include <iris/ipc_msg.h>`.
2. Define service-specific opcodes in the `0x0100–0xEFFF` range in a new `vfs_ep_proto.h`. Prefer a **stateless** request shape (full addressing per request): `IrisMsg` has no sender identity, so per-client server state cannot be bound safely.
3. Flag the service `own_service_ep = 1` in the catalog: svcmgr creates the endpoint, sends the receive side at bootstrap (kind 0x21, before `INITRD_CAP`) and publishes the send side as `"<image>.ep"`. (`IRIS_SVCMGR_EP_REGISTER` is unimplementable over EP_CALL — request-side cap transfer is forbidden; see `docs/endpoint-protocol.md`.)
4. Clients obtain the cap with `IRIS_SVCMGR_EP_LOOKUP_NAME("<image>.ep")` over the svcmgr discovery endpoint (bootstrap kind 0x20); the cap arrives via reply-cap transfer.
5. Replace each `SYS_CHAN_SEND` + `SYS_CHAN_RECV` pair with one `SYS_EP_CALL`. Re-stage request payloads before every call (EP_CALL reuses `buf_uptr` for the reply).
6. On the server, keep the legacy loop and drain the endpoint first: `EP_NB_RECV` burst → `SYS_REPLY` exactly once per reply cap → `SYS_CHAN_RECV_TIMEOUT` for legacy clients. Degrade to legacy-only if the endpoint dies.
7. Coexistence phase only: keep a runtime fallback while the endpoint path is being proven. Once the EP path is gated in smoke (next step), REMOVE the fallback — a surviving fallback masks endpoint regressions (this is what Fase 7.2 did for sh).
8. Gate the new path in `scripts/run_qemu_headless.sh` with markers that only print when the EP path actually runs, plus runtime tests that FAIL (not skip) when the endpoint is missing (T026–T030).
9. Once all legacy clients are migrated: remove the KChannel path, remove reply channel handles from bootstrap, drop `SYS_CHAN_*` imports.

## Migration order

```
Phase A (complete):  svcmgr EP coexistence (Fase 7)
Phase B (complete):  vfs EP server — stateless LIST/STAT/READ_AT (Fase 7.1)
Phase C (complete*): sh vfs operations over EP (Fase 7.1)
                     *kbd event channel deferred to Phase G
Phase C2 (complete): init S5/S6 VFS probes over EP; sh fallback removed;
                     VFS legacy protocol left clientless (Fase 7.2)
Phase D (7.3 DONE):  console endpoint ("console.ep": EP WRITE/SYNC/PING;
                     init/sh/vfs/iris_test endpoint-first; svcmgr is the
                     last legacy writer)
Phase E:             fb, userboot (single-call; trivial)
Phase F:             init remaining channels (orchestrator; requires all above)
Phase G (7.4+7.6):   kbd event channel sh←kbd DONE (EP pull + parked reply);
                     kernel IRQ→kbd DONE (IRQ→KNotification, kind 0x23);
                     residue: init/svcmgr kbd HELLO/STATUS probes
Phase H (7.5 DONE):  clientless VFS stateful protocol removed (vfs_proto.h
                     deleted, vfs endpoint_only, svcmgr status via EP);
                     svcmgr legacy loop still pending
Phase I (8, started): CPtr-first handoff — SYS_PROC_CSPACE_MINT + well-known
                     slot IRIS_CPTR_SVCMGR_EP; sh discovers svcmgr by CPtr;
                     next: replace bootstrap KChannel caps with minted slots
```

## What must NOT happen during migration

- Do not remove `KChannel`, `handle_id_t`, `SYS_CHAN_*` syscall numbers until Phase F is complete and all smoke tests pass with the legacy path disabled.
- Do not migrate the bootstrap channel in the same commit as the operational loop (two independent changes).
- Do not add `SYS_EP_CALL` inside IRQ context (IRQ handlers cannot block; use KNotification instead).
- Do not change the kernel memory model (no KFrame/KVSpace lifecycle changes) as part of IPC migration.
