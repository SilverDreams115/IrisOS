# Fase 22 — Service authority minimization

Status: ACCEPTED — implemented in this phase.  Companion to
`syscall-fuzzing.md` (Fase 21), `fault-endpoint-model.md` (Fase 20) and the
earlier subsystem-hardening docs.  Fase 21 proved the kernel MECHANISM fails
clean under hostility; the next risk is a service holding authority it does not
need.  This phase audits the initial authority of every in-tree service,
reduces the authority that is provably unused, and locks the least-authority
contract with runtime tests T156–T163.

**One real over-authority was found and reduced** (catalog children received
every core client-endpoint cap regardless of use), plus a full audit that
classified every remaining grant.  No kernel bug.  The runtime selftest suite
rises from 151/151 to 159/159.

The bar for this phase is explicit.  Not accepted: *"the services boot with
these caps."*  Accepted: *"each service receives only the authority it can
defend."*

---

## Philosophy

In a capability microkernel, security does not end at the kernel validating
caps correctly (Fases 18–21).  It also matters WHO receives WHICH caps at
startup.  A compromised service must not hold authority it never uses, because
that authority is exactly what an attacker inherits.  Every cap a service holds
at boot is answerable to one question: *if this service is fully compromised,
what can it reach with this cap?*

Least authority is enforced at two moments, kept distinct throughout:

- **bootstrap authority** — what a service needs to come up (spawn cap to load
  its image, an endpoint to serve on);
- **runtime authority** — what it needs to operate afterward.

A cap needed only for bootstrap should not linger as runtime authority; a cap
never used at all should not be granted.

## Service matrix

| Service | Role | Spawned by | Blast radius if compromised |
|---|---|---|---|
| init | root task | kernel (userboot) | total — holds the boot cap; audited, not minimized (T160) |
| svcmgr | service registry / loader | init | can spawn catalog images, hold device caps; is a REGISTRY, not a dominator of what it registers (T157) |
| vfs | initrd filesystem server | svcmgr | can read initrd, log to console; no peer authority |
| console | serial console driver | init | owns the 0x3F8 UART port only |
| kbd | PS/2 keyboard driver | svcmgr | owns the 0x60 port + IRQ1; no peer authority |
| fb | framebuffer painter | init | owns the framebuffer cap only |
| sh | shell | svcmgr | drives all four core services (legitimate client) |
| iris_test | ring-3 test suite | init | privileged TEST child — holds untyped/proc/spawn; contained to itself (T160/T162) |
| lifecycle_probe | test child | iris_test | holds only what its parent mints; the least-authority probe (T156/T162) |

## Capability matrix (per service, after Fase 22)

Classification: **RR** = REQUIRED_RUNTIME, **RB** = REQUIRED_BOOTSTRAP_ONLY,
**T** = TEST_ONLY, **L** = LEGACY_COMPAT, **X** = SHOULD_REMOVE.

| Service | Cap | Rights | Source | Class | Used for |
|---|---|---|---|---|---|
| svcmgr | console.ep (slot 3) | WRITE\|DUP\|XFER | init mint | RR | log + re-mint to children |
| svcmgr | svcmgr.ep recv (slot 5) | READ\|WRITE\|DUP\|XFER | init mint | RR | serve discovery, re-mint per child |
| svcmgr | spawn cap (slot 6) | READ\|DUP\|XFER | init mint | RR | load catalog images, create ioport/irq caps |
| svcmgr | KDEBUG (in spawn cap) | — | init | RR | SYS_KLOG_DRAIN (boot log), SYS_POWEROFF |
| vfs | console.ep (slot 3) | WRITE | svcmgr mint | RR | logging |
| vfs | own ep (slot 5) | READ | svcmgr mint | RR | serve filesystem |
| vfs | spawn cap (slot 6) | READ | svcmgr mint | RR | SYS_INITRD_VMO (read initrd files) |
| console | own ep (slot 5) | READ | init mint | RR | serve console |
| console | UART ioport (slot 10) | READ\|WRITE | init mint | RR | 0x3F8 serial I/O |
| kbd | own ep (slot 5) | READ | svcmgr mint | RR | serve kbd.ep |
| kbd | IRQ notif (slot 7) | WAIT | svcmgr mint | RR | wait for IRQ1 |
| kbd | 0x60 ioport (slot 10) | READ | svcmgr mint | RR | read scancodes |
| kbd | IRQ cap (slot 11) | ROUTE | svcmgr mint | RR | SYS_IRQ_ACK |
| fb | framebuffer cap (slot 6) | READ | init mint | RR | map + paint the framebuffer |
| sh | svcmgr/vfs/console/kbd ep (slots 1–4) | WRITE | svcmgr mint | RR | the shell drives all four |
| iris_test | core service eps (slots 1–4) | WRITE | init mint | T | the test suite exercises every service |
| iris_test | spawn cap (slots 6, 26) | READ | init mint | T | spawn lifecycle_probe children |
| iris_test | self proc (slot 25) | WRITE | init mint | T | self-CSpace mint tests |
| iris_test | boot untyped (slot 55) | R\|W\|D\|X | init mint | T | retype/revoke authority suite |
| iris_test | supervisor svcmgr cap (slot 27) | WRITE, badge INIT | init mint | T | privileged RESTART path tests |

## Reductions applied

| Service | Removed authority | Why safe | Test |
|---|---|---|---|
| kbd | svcmgr.ep, vfs.ep, console.ep, kbd.ep (all 4 client caps at slots 1–4) | kbd/main.S calls NO peer — it is a pure endpoint server + IRQ handler; verified by source and by the full smoke (kbd still serves sh) | T156/T162 + smoke |
| vfs | svcmgr.ep, vfs.ep, kbd.ep (slots 1,2,4) | vfs only logs to console.ep and serves its own endpoint; it never calls svcmgr, itself, or kbd; verified by source (only CONSOLE_EP/OWN_EP/SPAWN_CAP referenced) | T156/T162 + smoke |

Before Fase 22, `svcmgr_build_core_mints` minted ALL FOUR core client-endpoint
caps into EVERY catalog child unconditionally — authority "just in case."  A
compromised kbd held WRITE caps to svcmgr, vfs and console; a compromised vfs
held a WRITE cap to kbd.  The reduction makes the grant manifest-driven: each
catalog entry declares a `client_eps` bitmask of the peers it actually calls,
and only those slots are minted.  kbd now receives none; vfs only console; sh
(the shell, which legitimately drives all four) keeps all four.

## Reductions considered and rejected

- **vfs spawn cap (slot 6)** — kept: vfs needs `SYS_INITRD_VMO` to read initrd
  files it serves.  Restricting the cap to SPAWN_SERVICE-only (dropping KDEBUG)
  is already the case; the cap carries no KDEBUG for vfs.
- **svcmgr KDEBUG** — kept: svcmgr uses `SYS_KLOG_DRAIN` to flush the boot log
  to console and `SYS_POWEROFF`.  Both are supervisor duties.  Documented, not
  removed.
- **iris_test's broad bag** — kept: it is a TEST_ONLY child, not a productive
  service, and T160/T162 prove that authority is contained to iris_test and
  absent from any ordinary service.  Removing it would gut the test suite.
- **init's retained caps** — audited (T160), not yet minimized: init is the
  root task and legitimately holds the boot cap.  A minimal-root-task redesign
  is deferred (see remaining work) to avoid mixing a large refactor with this
  phase's reductions.

## Invariants A1–A16

```text
A1  Each service receives only justified caps.
A2  Bootstrap-only caps are closed/isolated after use where the model allows.
A3  No service receives untyped without explicit need.
A4  No service receives KDEBUG without justified test/diagnostic need.
A5  No service receives another VSpace without explicit authority.
A6  No service receives another process's MANAGE cap without explicit need.
A7  svcmgr cannot dominate services it should only register/discover.
A8  vfs holds no console/kbd/fb authority beyond an explicit contract.
A9  console/kbd/fb hold no authority over arbitrary processes.
A10 iris_test/lifecycle_probe do not contaminate productive authority.
A11 Service lookup delivers only expected caps with expected rights.
A12 Service death/unregister leaves no orphan caps.
A13 Receive-slot / mint delivery does not amplify rights.
A14 CSpace mint toward services respects rights monotonicity.
A15 Removing an unused cap does not change expected behaviour.
A16 A compromised service is contained by its real authority.
```

## The least-authority observability model

There is no syscall to enumerate another process's CSpace (by design — the A1
authority-namespace endgame).  So the delivery contract is verified through a
self-report: a `lifecycle_probe` child resolves its well-known CPtr slots and
exits with a bitmask (bits 0..15 = slots 0..15; bit 16 = slot 25 proc, 17 =
slot 55 untyped, 18 = slot 56 vspace).  The parent mints a KNOWN set and
asserts the reported bitmask equals exactly that set — no phantom cap, and
removing a cap removes it from the child.  Because every service is delivered
its caps through the same `svc_load_minted` / `SYS_PROC_CSPACE_MINT` path the
probe uses, locking the mechanism locks all of them.

svcmgr's authority is observed through its real endpoint API: `LOOKUP_NAME`
returns a cap whose `attached_rights` is checked exactly, and `DIAG` reports the
ready-service count (which includes dynamic registrations) as the registry
gauge.  The full-surface snapshot from Fase 21 (`it_snap`) anchors the
no-drift checks; its cumulative `reply-caps-created` counter is NOT a per-test
balance and is excluded (a genuine KReply leak surfaces as a handle-live drift,
which IS checked).

## Tests

```text
T156  manifest delivery: a probe minted {cmd@3, notif@5, ep@7} reports exactly
      those slots; minted {cmd@3} reports only slot 3.  No phantom cap; removing
      a cap removes it (A1/A11/A13/A15).
T157  svcmgr grant tightening: an ordinary-badge LOOKUP_NAME of a ".ep" name
      returns WRITE-only (DUPLICATE/TRANSFER stripped); a supervisor badge keeps
      DUPLICATE; a missing name is NOT_FOUND.  svcmgr is a registry, not a
      dominator (A7/A11/A14).
T158  vfs boundary: vfs.ep answers PING and its own ops but not a foreign
      registry op; an ordinary client's vfs.ep cap is call-only (not
      re-mintable) (A8/A11/A16).
T159  console/kbd boundary: both answer PING and their own ops, reject a foreign
      registry op, and hand out only call-only client caps (A9/A11/A16).
T160  init handoff audit: iris_test's declared TEST authority (spawn/untyped/
      proc) resolves here — exactly what T162 proves absent from an ordinary
      service (A4/A6/A10).
T161  death containment: a dynamic registration resolves, bumps the registry
      gauge, then unregister removes it, drops the gauge to baseline, and the
      name no longer resolves — no ghost (A12/A16).
T162  least-authority lock (teeth): a minimal probe holds NO high authority
      (no spawn/proc/untyped/vspace, no peer client eps); minting one extra cap
      makes exactly that slot appear, proving the guard has teeth (A1/A3–A6/A10).
T163  authority stress: seeded register/lookup/unregister churn interleaved with
      malformed ops (stale unregister, missing lookup, reserved-name register);
      no amplification, no ghost, registry gauge and snapshot back to baseline.
```

## Adding a new service without widening authority

1. Add a catalog entry (or an init mint table) with the SMALLEST cap set the
   service needs; set `client_eps` to only the peers it actually calls.
2. Never mint a cap "just in case" — if the service does not reference a slot in
   its source, do not grant it (T156/T162 will not catch an unused-but-granted
   cap automatically, but a code reviewer checking `client_eps` against the
   source will; keep them in sync).
3. Never grant untyped, another process's MANAGE, another VSpace, or a
   KDEBUG-bearing cap to a productive service — T162 asserts an ordinary probe
   holds none of these; a new service must stay on that side of the line.
4. If the service registers with svcmgr, its clients receive call-only caps by
   default (grant tightening) — do not widen client rights without a documented
   reason.

## Remaining service-authority work

- **init minimal-root-task**: init still holds broad boot authority after
  handoff.  Auditing is done (T160); the reduction (closing bootstrap-only caps,
  handing the residual to a dedicated supervisor) is deferred to avoid mixing a
  large refactor with this phase.
- **Bootstrap-only cap closing (A2)**: some caps used only during boot are still
  held for the process lifetime; a pass to close them post-boot would tighten
  runtime authority further.
- **Device-cap fuzzing**: the IOPORT/IRQ caps held by console/kbd are exercised
  functionally but not fuzzed under authority — a driver-isolation phase should
  fuzz them from a compromised-driver posture.
- **Static manifest cross-check**: the `client_eps` manifest is kept in sync
  with service source by review; a build-time assertion (host test) that a
  service's referenced slots ⊆ its granted slots would make it mechanical.
