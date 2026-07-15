# Fase 24 — Service restart / supervision model

Status: ACCEPTED — implemented in this phase.  Companion to
`device-driver-isolation.md` (Fase 23), `service-authority-minimization.md`
(Fase 22) and the earlier hardening docs.  Fases 22–23 minimized and contained
what a service holds; Fase 24 hardens what happens when a service or driver
DIES.  The audit and tests T172–T180 make the supervision policy explicit,
auditable, and capability-preserving: a restarted service comes back with a new
generation and only its declared authority, a crash-loop stops at its limit, and
a stale generation cannot touch the new instance.

**No kernel bug was found**, and one test-only harness bug (a packed STATUS
reply read past IRIS_MSG_WORDS) was found and fixed in the harness.  The only
product change is additive: an explicit supervision-policy classification on the
catalog and its exposure through the STATUS oracle (no ABI break, no kernel
change).  The runtime selftest suite rises from 167/167 to 176/176.

The bar for this phase is explicit.  Not accepted: *"the service starts again."*
Accepted: *"supervision preserves authority, generation, cleanup, and the truth
of the system."*

---

## Philosophy

Launching a service once is not supervision.  A real OS must know, at every
moment, which services are alive, which died, whether they should return, with
what authority, and what stale state their death left behind.  Supervision in
IRIS is an EXPLICIT capability-compatible policy, not an implicit "respawn on
exit": every service carries a written classification, restart is bounded, and a
restarted instance inherits nothing stale — it is a new generation with exactly
the authority its manifest declares (Fase 22) and, for a driver, exactly its
device caps (Fase 23).

Two supervision surfaces exist:

- **svcmgr over catalog services** — the productive supervisor.  It owns each
  catalog service's process, watches it (`SYS_PROCESS_WATCH` → death
  notification), and applies the catalog policy: restart up to the limit,
  bumping the generation each time; on budget exhaustion it stops and marks the
  service degraded.
- **any process over its children** — a supervisor is just a process holding
  proc caps plus a policy.  The test suite acts as a supervisor over
  `lifecycle_probe` children to exercise the kernel primitives (watch, kill,
  reap, endpoint cleanup) every supervisor relies on, without spending the real
  services' restart budgets.

## Supervision policy manifest

| Service | Criticality | Restart | Limit | Generation | Reason |
|---|---|---|---|---|---|
| vfs | CRITICAL_RESTART | yes | 3 | bumps per restart | filesystem — the system depends on it |
| kbd | OPTIONAL_RESTART | yes | 3 | bumps per restart | keyboard input — desirable, non-fatal |
| sh | OPTIONAL_NO_RESTART | no | 0 | n/a | the user shell — a one-shot session |
| console | CRITICAL (init-spawned) | no (by init) | — | n/a | serial output; init owns it, documented not auto-restarted |
| fb | OPTIONAL (init-spawned) | no | — | n/a | framebuffer painter; init owns it |
| svcmgr | CRITICAL_NO_RESTART | no | — | n/a | the supervisor itself — its loss is unrecoverable |
| init | CRITICAL_NO_RESTART | no | — | n/a | the root task |
| iris_test / lifecycle_probe | TEST_ONLY | n/a | — | supervised by iris_test | test children, never productive |

The catalog entries (kbd/vfs/sh) carry an explicit `supervision` field
(`IRIS_SUPERVISION_*`); T172 asserts every catalog service declares a policy
consistent with its `restart_on_exit`/`restart_limit` flags (a RESTART class has
a non-zero limit, a NO_RESTART class has a zero limit) and consistent with the
Fase 22 authority manifest (a driver stays a driver).  console/fb/svcmgr/init are
init-spawned (not catalog); their policy is documented here.

## Generations

A catalog service's generation starts at 1 on first boot and bumps on every
death→respawn and every explicit RESTART.  A client that cached a generation
detects, via the STATUS oracle, that the instance it holds a cap to has been
replaced, and relookups.  The generation is monotonic — it never decreases.  A
stale generation confers no authority: the old instance is gone, its endpoint
recv side is closed by teardown, and any cached cap to it is dead.

Dynamic registrations (`SYS_SVCMGR_EP_REGISTER`) are OWNER-managed: they carry a
generation but are not death-watched by svcmgr (see Gaps).  The owner registers
and must unregister; a stale registration id cannot act after unregister (second
unregister is NOT_FOUND).

## Contracts

- **Registration**: cap-backed (the registrant transfers its endpoint); reserved
  `.ep`/catalog names are rejected; a duplicate name is BUSY; badge-authenticated
  (owner_badge = sender_badge).
- **Lookup**: returns the CURRENT instance's cap with the tightened client
  rights (Fase 22 grant tightening); a gone name is NOT_FOUND — no stale cap.
- **Unregister**: owner-badge or supervisor only; a stale/never-registered id is
  NOT_FOUND; idempotent.
- **Restart**: kills the service; the watch path respawns it, bumps the
  generation and the restart_count, and re-establishes its endpoint (the
  KEndpoint survives restarts).  The new instance receives ONLY its manifest
  authority — no Fase 22 client caps beyond `client_eps`, no Fase 23 device caps
  beyond its declared set, no leaked proc/endpoint/notification/IRQ refs.
- **Crash-loop**: restart is bounded by `restart_count < restart_limit`.  On
  reaching the limit the service is left DOWN and marked `degraded` (observable
  via STATUS); it does not revive without explicit action.
- **Critical service**: a critical service's loss is an explicit state.  vfs is
  CRITICAL_RESTART (restarted, then degraded on exhaustion); svcmgr/init are
  CRITICAL_NO_RESTART (documented unrecoverable — the test suite never kills them
  destructively).
- **Generation**: monotonic; a stale generation cannot unregister or modify the
  current one.

## STATUS policy exposure (additive)

The `IRIS_SVCMGR_EP_STATUS` reply is extended additively.  IPC carries only 4
words (`IRIS_MSG_WORDS`), so the policy is packed:

```text
words[0] = alive
words[1] = generation
words[2] = supervision class (IRIS_SUPERVISION_*)
words[3] = restart_count | (restart_limit << 8) | (degraded << 16)
```

Legacy callers reading only `words[0..1]` are unaffected.  This is the only
product change in the phase; no new syscall, no struct-layout change visible to
the kernel.

## Tests

```text
T172  policy manifest consistency: every catalog service declares an explicit
      supervision class consistent with its restart flags and authority manifest.
T173  restartable recovery: one real RESTART of kbd — generation and
      restart_count advance, the service is alive, kbd.ep still answers (endpoint
      survives restart), live books at baseline.
T174  stale generation/registration: a dynamic registration, once unregistered,
      cannot be unregistered again (NOT_FOUND) and no longer resolves; a fresh
      registration is independent; catalog generation is monotonic.
T175  crash-loop limit: a supervised probe that dies immediately is respawned
      exactly `limit` times, then the supervisor stops and marks it degraded —
      no infinite loop, no per-attempt leak, baseline.
T176  client during death: a probe blocked as a caller is killed mid-call; the
      wait is cancelled, the child reaps, and a following NB-send to the endpoint
      reports WOULD_BLOCK — no phantom receiver.
T177  driver restart authority: a driver-like probe restarted with the same
      manifest is still contained (DEV_PROBE breach 0) and holds ONLY its command
      endpoint + device cap — no spawn/proc/untyped, no peer caps, no IRQ ghost.
T178  critical service policy: vfs is CRITICAL_RESTART with a budget and alive;
      sh is OPTIONAL_NO_RESTART with zero budget — asserted via STATUS, no
      destructive kill of a critical service.
T179  death during register/unregister: a registration owned by iris_test
      survives an unrelated probe death; unregister removes it; repeat is
      NOT_FOUND; registry gauge at baseline.
T180  supervision stress: seeded spawn/kill/fault-crash/register/lookup/
      unregister/stale-unregister churn; no stale generation acts, no registry
      ghost, no client stuck, registry and live snapshot at baseline.
```

## Relationships

- **Least-authority (Fase 22)**: a restarted service is minted from the same
  `client_eps` manifest — restart cannot reintroduce a removed peer cap (T177).
- **Device isolation (Fase 23)**: a restarted driver receives only its declared
  device caps; old IRQ routes are cleared by teardown before the new instance
  registers its own (T177).
- **Fault endpoint (Fase 20)**: a fault-crash is handled like a kill — the
  faulting task with no handler is terminated, the watch path treats it as a
  death (T180 fault-crash rounds).
- **Lifecycle/reap (Fase 16)**: restart relies on the deferred-reap slot-reuse
  fix; the supervisor drains the reaper before every baseline.
- **IPC/KReply (Fase 16/20)**: a blocked client's KReply is cancelled on the
  server's death — no KReply drift (T176).

## Remaining gaps

- **Dynamic-registration death-watch**: svcmgr does not watch the processes
  behind dynamic (`REGISTER`) registrations; STATUS reports `alive=1` "by
  contract" (registered ⇒ owner-alive).  A registrant that dies without
  unregistering leaves a registration whose endpoint has no server — a client
  that CALLs it would block.  The contract is owner-managed cleanup; adding an
  optional death-watch (the registrant supplies a proc cap) is future work.
- **Backoff timing**: crash-loop limiting is a deterministic count, not a
  time-based backoff.  A real backoff (exponential delay) is deferred; the count
  is sufficient to stop a loop.
- **Critical-service self-recovery**: svcmgr and init are CRITICAL_NO_RESTART —
  their loss is documented as unrecoverable, not handled by a higher supervisor.
  A minimal-root-task redesign (Fase-future) could add a watchdog.
- **Real driver restart budget**: T173 spends one of kbd's three restarts; a
  full crash-loop-to-degraded test on a REAL driver would exhaust its budget and
  break the running system, so that path is covered by the probe supervisor
  (T175) instead.

## Adding a new service without breaking supervision

1. Give it an explicit `supervision` class in the catalog (or document its
   init-spawned policy here); T172 fails a service with no policy.
2. Keep the class consistent with `restart_on_exit`/`restart_limit`: a
   restartable service has a non-zero limit; a one-shot has zero.
3. A restarted instance is minted from the SAME `client_eps` (Fase 22) and the
   same device caps (Fase 23) — never widen authority to "help" a restart.
4. If the service registers dynamically, it must unregister on shutdown; svcmgr
   will not clean it up on death.
5. A critical service that cannot be restarted must have its unrecoverable state
   documented — never a silent implicit behaviour.

---

## Fase 28.1 addendum — pager restart and the grant session

A supervised pager's file authority is now a **VFS grant session**
(`file-grant-capability.md`).  The restart protocol gains one step: before
re-minting the session cap into a fresh pager instance, the supervisor issues
`GRANT_SESSION_RESET(session)` to the VFS, dropping every grant of that session.
A restarted pager therefore cannot reuse its predecessor's grants — the old
grant indices fail `NOT_FOUND` at the VFS (T235).  Combined with the epoch-
stamped generations that invalidate all grants across a *VFS* restart (T236),
neither a pager restart nor a VFS restart can revive stale file authority.  The
supervision boundary — a lost pager degrades paging for its targets but never
amplifies authority across a restart — now extends to file access.
