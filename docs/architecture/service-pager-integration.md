# Fase 27 — Service pager integration

Status: ACCEPTED — implemented in this phase.  Companion to
`user-pager-vm-policy.md` (Fase 25), `memory-object-vmo-policy.md` (Fase 26),
`service-supervision-model.md` (Fase 24) and `service-authority-minimization.md`
(Fase 22).  Fase 25/26 made the pager a MODEL and a page SOURCE; Fase 27 makes
it a supervised system SERVICE.  Locked with runtime tests T201–T210.

---

## Philosophy

A user pager is only real when it lives in the system as a service: a named,
supervised, restartable component with an explicit authority manifest and a
recovery story — not a one-off probe that "fixes a page".  In a real operating
system the pager is a daemon that other components can look up, that a
supervisor restarts under a policy, and whose authority is exactly the set of
processes it may page and the objects it may page from — nothing global.

The one-line model: **the pager is a supervised service with least authority,
explicit target and VMO grants, and demonstrable recovery.**

Crucially, the pager is NOT a global memory manager.  It holds no untyped, no
device caps, no KDEBUG, no spawn cap, and no authority over any process outside
its declared grants.  Its compromise is bounded by its manifest; its death is
survivable; its restart regains exactly the declaration.

## Roles

```text
pager service   a supervised userland service.  Loops on a control endpoint,
                serving fault-resolution requests inside a manifest of target
                grants (proc READ|MANAGE, vspace WRITE, fault-notif WAIT) and
                VMO grants (READ [+WRITE]).  Acquires nothing at runtime.

supervisor      creates targets and VMOs, mints the pager's manifest, spawns
                and registers the service, watches it, and restarts it under
                an explicit policy.  Retains its own caps: it can always
                resolve or kill directly.

target          a process the pager is authorized to page; receives
                VMO-backed mappings, gains no authority over the pager or the
                VMOs.

svcmgr          the registry: publishes the pager under a name so lookups
                reach the CURRENT endpoint; a restarted pager re-registers and
                the stale name no longer resolves.
```

## The service, concretely

The pager service is its **own supervised initrd binary** (`services/pager/`,
initrd index 10, named "pager") — distinct from `iris_test` (the supervisor)
and from `lifecycle_probe`.  The supervisor spawns it via `svc_load` with its
whole authority minted before start; it enters its serve loop immediately (a
dedicated pager binary IS the service — no mode-entry message).

> **History.**  Fase 27 first shipped the pager as a persistent mode of the
> already-embedded `lifecycle_probe` image, because adding a tenth initrd blob
> appeared to wedge early boot.  Fase 28 Bloque A root-caused that "wedge": it
> was an over-strict `userboot` assertion (`initrd_count == SL_CATALOG_COUNT`)
> that exited before loading init on any image-count change — not a memory or
> layout bug (`__data_start`/`__kernel_end` were byte-identical with a 256-byte
> extra blob).  With the assertion relaxed to `>=` and a boot diagnostic added
> (see `boot-image-growth.md`), the pager was promoted to its own binary
> (T215), keeping the exact same authority manifest, control protocol, and
> supervision/restart mechanics.

### Manifest (minted before start; the whole of the pager's authority)

```text
slot 3            control endpoint (served in a loop; READ to receive)
target i (0,1):   proc  slot 8 + i*3   RIGHT_READ | RIGHT_MANAGE
                  vspace slot 9 + i*3  RIGHT_WRITE            (map-into authority)
                  notif  slot 10 + i*3 RIGHT_WAIT             (fault delivery)
VMO j (0,1):      slot 16 + j          RIGHT_READ [+ RIGHT_WRITE]
```

No spawn cap (slot 6), no core client endpoints (slots 1/2/4), no untyped
(slot 55), no self-vspace (slot 56), no device caps, no KDEBUG.  A `REPORT`
op returns the exact resolving-slot bitmask so the supervisor asserts the
manifest (T201).

### Control protocol (EP_CALL → SYS_REPLY)

```text
words[0] = op | (target_idx << 8) | (vmo_idx << 16) | (flags << 24)
words[1] = VMO byte offset (page-aligned)
words[2] = expected cr2 (0 = skip)

op 1 PING         health check → reply 0
op 2 REPORT       manifest oracle → reply resolving-slot bitmask
op 3 MAP_RESUME   wait target[t] fault; read info; map vmo[v]@offset at cr2 in
                  target[t]'s VSpace; seq-resume → reply 0 / -err
op 4 KILL         wait target[t] fault; seq-kill → reply 0 / -err
op 5 SHUTDOWN     reply 0, then exit cleanly
```

Reply `words[0]` is `0` on success, else `(uint32_t)(-err)` or a private
validation marker.  Every `MAP_RESUME`/`KILL` step is a capability check: fault
info through the target proc cap (READ), map through the VMO cap (READ[/WRITE])
+ the VSpace cap (WRITE), resume through the proc cap (MANAGE).  Fault delivery
itself grants the pager nothing (F17/F18, inherited).

## Supervision policy

```text
class          OPTIONAL_RESTART   a lost pager degrades paging for its targets
                                  but is not fatal to the system
restart_limit  3                  respawn up to the limit, then degraded
generation     monotonic          each (re)start is a new generation
```

The supervisor (`iris_test`, the Fase 24 probe-supervisor model over a real
named service) applies this policy: it spawns, registers `pager.svc`, watches,
and — on repeated death — respawns up to the limit and then stops, leaving the
service degraded (T206).  A degraded pager does not strand its targets: a
pending fault stays observable and the supervisor resolves it with its own
authority.

## Contracts

- **Startup** — the supervisor mints the manifest, spawns the service, sends
  `LP_CMD_PAGER_SERVICE` to enter the loop, and registers `pager.svc` (a
  non-`.ep` dynamic name; `.ep` is reserved for catalog services).  The service
  reports exactly its manifest and answers a lookup PING (T201).
- **Resolution** — trigger the target's fault, then `EP_CALL` the pager to map
  a VMO page at the fault VA and seq-resume; the target continues.  Delivery is
  exactly-once; no implicit caps; PTE rights are the meet of the VMO cap and
  the request (T202).
- **Unauthorized target** — a pager granted target A cannot touch target B: A's
  caps do not resolve or map into B, B's fault stays intact and is resolved
  only by B's own authority (T203).
- **Unauthorized VMO** — a read-only VMO grant cannot install a writable PTE; a
  bad offset / kernel VA is refused; no partial PTE, no VMO ref leak (T204).
- **Restart** — a killed pager is respawned with the same manifest and a new
  generation; it re-registers so `pager.svc` serves the CURRENT endpoint (the
  stale name no longer resolves); it reports exactly the manifest (no
  accumulated authority) and resolves a fresh fault (T205).
- **Crash-loop** — bounded by the restart limit, then degraded; the pending
  fault survives and is resolvable by the supervisor (T206).
- **Multiple targets** — grants never mix: each target's fault info and
  mappings are per-target; one target's death does not disturb another
  (T207).
- **Multiple VMOs** — grants never mix: region A is backed by VMO0, region B by
  VMO1, and the stores land in the right object; PTE rights are per-VMO (T208).
- **Pager death with pending fault** — the target is not a zombie: it stays
  suspended-alive with record and generation intact, resolvable by a restarted
  pager or the supervisor; the dead pager's control endpoint has no phantom
  receiver (T209).
- **Target death during resolution** — the pager's late map is `BAD_HANDLE`,
  late resume `NOT_FOUND`; the fault record is gone; the VMO survives (T210
  op2).

## Interaction with the rest of the system

- **Supervision (Fase 24)** — the pager is a supervised service like any other:
  watch, restart-to-limit, generation, degraded.  The policy is declared and
  applied by the supervisor; the mechanism is the kernel's `SYS_PROCESS_WATCH`
  + `SYS_PROCESS_KILL` + reap.
- **Service authority (Fase 22)** — the manifest is delivered by the same
  pre-start `SYS_PROC_CSPACE_MINT` mechanism; the pager adds a manifest *shape*,
  not a new mechanism.  Least authority is asserted by the `REPORT` oracle.
- **svcmgr registry** — the pager registers a dynamic name (`pager.svc`);
  lookups return a fresh WRITE cap to the current control endpoint.  A restarted
  pager re-registers; the stale registration is unregistered on death, so the
  name resolves `NOT_FOUND` between generations (T205).
- **User pager / VMO policy (Fase 25/26)** — unchanged and reused verbatim.
  Resolution is `SYS_VMO_MAP_PAGE` (Fase 26) into a `SYS_PROCESS_VSPACE` cap
  (Fase 25), with fault generations and seq-checked resume.  The service is the
  integration layer on top; it adds no kernel surface.
- **Fault endpoint model (Fase 20)** — the pager waits on the target's fault
  notification (WAIT cap) and resolves through the proc cap; delivery and the
  record lifecycle are unchanged.

## ABI / kernel impact

None.  Fase 27 adds no syscall and no kernel change.  The pager service is
pure userland (a `lifecycle_probe` mode) plus supervisor test code.  The only
non-test source change outside `iris_test` is the pager-service loop in
`lifecycle_probe/main.c`.

## Instrumentation

None added.  The Fase 20 fault counters, the Fase 18/19 live gauges
(frame/mapping/VSpace/endpoint/notification/handle/task/process), the Fase 26
VMO-live gauge, the svcmgr registry gauges and the per-process fault record
cover every observable this phase needs.

## Tests (runtime, deterministic)

```text
T201  manifest & startup: the service comes up with EXACTLY its declared
      manifest (control ep + one target grant + one VMO grant), reports it,
      registers pager.svc, and answers a lookup PING; no spawn/device/untyped/
      KDEBUG/peer authority.
T202  resolves one target fault: supervisor fills a VMO page, triggers the
      target's read fault, calls the pager to map + seq-resume; the target
      reads the pattern; exactly-once delivery; books at baseline.
T203  unauthorized target denial: a pager granted target A cannot resolve or
      map into target B; B's fault stays intact, resolved only by B's authority.
T204  unauthorized VMO denial: RO VMO grant cannot map writable; bad offset /
      kernel VA refused; no partial PTE; a valid RO resolution still works.
T205  restart preserves authority: gen1 resolves, is killed, unregistered; the
      stale pager.svc no longer resolves; gen2 restarts with the same manifest
      (new generation, re-registered), reports exactly the manifest, resolves
      a fresh fault.
T206  crash-loop containment: respawn exactly `limit` times, then degraded; the
      pending fault survives and the supervisor resolves it.
T207  multiple targets: one pager, two target grants; both resolve into the
      right VSpace by index; A's death does not disturb B.
T208  multiple VMOs: one pager, two VMO grants (RO + writable); region A from
      VMO0, region B into VMO1; backings never mix.
T209  pager death with pending fault: the target is suspended-alive with record
      intact, resolvable by a restarted pager; no phantom receiver.
T210  seeded stress: mixed rounds — VMO-backed resolve, pager death + supervisor
      takeover, target death mid-fault + BAD_HANDLE, unauthorized VMO denial —
      with all live books at baseline every round.
```

## Security notes

- **Authority boundaries** — the pager holds exactly its manifest: proc caps
  (READ|MANAGE) and VSpace caps (WRITE) for its declared targets, VMO caps for
  its declared page sources, one control endpoint, and the fault notifications.
  Nothing global.
- **No implicit caps from faults** — fault delivery transfers no capability and
  creates no handle; the pager learns facts, not authority.
- **Pager compromise blast radius** — bounded by the manifest: a compromised
  pager can page ITS targets from ITS VMOs and nothing else.  It cannot forge
  device or spawn authority (asserted by the manifest oracle), cannot touch
  unrelated processes (T203), and cannot exceed VMO/VSpace rights (T204).
- **Restart least-authority** — a restarted pager gets exactly the re-minted
  manifest; the report oracle proves no accumulation across generations (T205).
- **Stale endpoint prevention** — registry names are unregistered on death, so
  a stale `pager.svc` resolves `NOT_FOUND`; the restarted service re-registers
  the current endpoint (T205).
- **Remaining over-privilege** — none detected within the manifest model.

## Remaining gaps

- **Boot-size fragility — FIXED in Fase 28.**  The "wedge on the 10th image"
  was an over-strict `userboot` count assertion, not a kernel memory bug; it is
  root-caused, fixed, and locked with tests T211–T216.  New service images can
  now be added freely, and the pager is its own binary.  See
  `boot-image-growth.md`.
- **Not boot-autostarted.**  The pager is spawned and supervised by a
  test-controlled supervisor, not the boot catalog — legitimate, because a
  pager's grants are inherently runtime (per target/VMO), not boot-time, but a
  productive deployment would want a catalog entry once the boot-size fragility
  is fixed and a runtime grant-delivery protocol (receive-slots) is added.
- **One control request at a time.**  The service resolves one fault per
  request (no `wait-any`); a multi-target autonomous pager would need per-target
  threads or a poll primitive.
- **No backing store.**  VMO pages are still eager/anonymous (no swap, no
  filesystem-backed paging, no COW) — the next memory-policy phases.

## Fase 28 Bloque B — file-backed extension (implemented)

The Fase 27 raw-VMO pager grew a file-backed subsystem (see
`file-backed-memory.md`).  The same manifest model gains a VFS endpoint cap
(`PGR_SLOT_VFS_EP`, slot 4) as the file-read source and two VMO grants — an RO
page cache (slot 16) and a private-writable pool (slot 17).  New control ops
(`PGR_OP_REGISTER_BACKING/REGISTER_REGION/MAP_REGION/UNREGISTER_REGION/REVOKE_BACKING/DIAG`)
drive backing identity+generation, validated file regions, RO-shared and
private-writable resolution, and observability counters (`struct pgr_diag`).
The pager still holds NO global VFS access: it reads only names the supervisor
registered as backings.

Authority note: a file-backed pager's supervisor needs a **duplicable** VFS
endpoint cap to mint the read source into the pager.  svcmgr strips
`RIGHT_DUPLICATE` from ordinary (non-supervisor) client lookups, so init — the
supervisor — pre-mints iris_test a duplicable vfs cap at
`IRIS_CPTR_TEST_VFS_DUP`; iris_test materializes it to a handle with
`SYS_CSPACE_RESOLVE` (rights preserved) before re-minting `RIGHT_WRITE` into the
pager.  This mirrors the existing supervisor-badged svcmgr cap
(`IRIS_CPTR_TEST_SUPER`) that iris_test holds for the privileged RESTART path.

---

## Fase 28.1 addendum — manifest tightening + multi-target

The Fase 27/28 manifest is tightened in two ways (`file-grant-capability.md`):

- **No generic VFS cap.** Slot 4 is now a *session-badged, WRITE-only* `vfs.ep`
  cap (`IRIS_BADGE_FILEGRANT_S(session)`).  The VFS confines a session badge to
  the session-scoped grant ops and denies every name-based op, so the slot
  carries exactly "the backings my supervisor granted me" — even if the pager
  is hostile.  The supervisor's grant-admin authority lives on a *separate*
  cap (`IRIS_CPTR_TEST_VFS_DUP`, badge `IRIS_BADGE_FILEGRANT_ADMIN`); the
  session-cap mint source is a third, unbadged cap
  (`IRIS_CPTR_TEST_VFS_MINT`).  A badged cap can never be re-badged, so these
  three roles are necessarily distinct caps.
- **One shared fault notification (slot 5) for all targets.** Per-target
  notification columns are gone; the supervisor registers every target's
  exception handler on the one notification with bit `(1 << tidx)`, and the
  pager wait-any's over a pending-bits accumulator.  `PGR_MAX_TARGETS = 16`;
  targets are at `20 + i·2` (proc/vs).  N concurrent targets cost the
  supervisor ONE notification, not N (T237/T238).
