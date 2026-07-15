# Fase 25 — VM policy / user pager groundwork

Status: ACCEPTED — implemented in this phase.  Companion to
`fault-endpoint-model.md` (Fase 20), `vspace-frame-hardening.md` (Fase 19)
and `service-supervision-model.md` (Fase 24).  Fase 25 closes the gap Fase 20
left explicit: *"no fault redirection to a third-party pager — a supervisor
cannot map into another process's VSpace"*.  Locked with runtime tests
T181–T190.

---

## Philosophy

In a real operating system, virtual memory policy is not
"the kernel maps pages eagerly and kills on failure".  Policy — which page
backs which fault, when, from what source — belongs in userland, with
authority, because policy is exactly the thing different systems legitimately
disagree about.  The kernel's job is smaller and harder: keep the *mechanism*
sound.  It validates authority, preserves fault state, installs and removes
PTEs, and cleans up when either side dies.  It never decides.

The kernel therefore must not be the global pager, and no userland pager may
become one by accident.  A pager in IRIS is an ordinary process whose entire
power is a handful of explicitly minted capabilities over ONE target.  Its
compromise is bounded by that manifest; its death is survivable; its restart
regains exactly the declaration and nothing more.

The model in one paragraph: **an authorized pager can resolve memory faults
of a specific process, with explicit authority, without becoming a global
owner of the system.**

## Roles

```text
faulting process   generates the page fault; suspended in TASK_BLOCKED_FAULT
                   until an authority acts (Fase 20 semantics, unchanged).

pager              authorized to receive the fault and resolve it.  Holds,
                   by supervisor mint: target proc cap (READ|MANAGE), target
                   VSpace cap (WRITE), the frame(s) it may install, and the
                   fault notification (WAIT).

supervisor         creates the target, registers the fault endpoint
                   (RIGHT_MANAGE), takes the target VSpace cap
                   (SYS_PROCESS_VSPACE) and mints the pager's manifest.
                   Retains its own caps: it can always resolve or kill.

kernel             validates every capability on every step, preserves fault
                   state with a per-process generation, applies map/resume,
                   sweeps state on either party's death.  No policy.
```

## Minimal policy (normative)

- A pager resolves faults only of processes for which it received explicit
  authority (P1, P4).
- Resolving a fault requires, each as a separate capability check:
  - authority over the target process (READ to observe, MANAGE to resume);
  - authority over the target VSpace (WRITE) or an equivalent map-into cap;
  - authority over the frame/VMO being installed (READ, +WRITE if writable);
  - resume authority (MANAGE, seq-checked form available).
- Fault info carries no capabilities; fault delivery grants no authority
  (P2, F17/F18 inherited from Fase 20).
- Every map goes through the standard CSpace/rights checks; there is no
  pager-privileged path (P5–P8).

## Authority surface

### SYS_PROCESS_VSPACE (107) — the map-into-target capability (NEW, additive)

`SYS_PROCESS_VSPACE(proc_h) → vspace_handle`.  Requires `RIGHT_MANAGE` on a
process capability (dual resolver, no fallback; `HANDLE_INVALID` = self,
where it degenerates to `SYS_VSPACE_SELF`).  Returns the target's `KVSpace`
cap with `READ|WRITE|DUPLICATE` (no TRANSFER — same shape as VSPACE_SELF).

This grants nothing MANAGE did not already imply — `SYS_VMO_MAP_INTO` has
always let a MANAGE holder install pages into a child — but it converts that
process-cap side effect into a first-class, delegable, **attenuable** object
capability: the supervisor mints it down to `RIGHT_WRITE` into the pager's
CSpace, so the pager can drive `SYS_FRAME_MAP`/`SYS_FRAME_UNMAP` on the
target *without* holding the process cap that kills, mints or re-registers.
Errors: `ACCESS_DENIED` (no MANAGE), `WRONG_TYPE`, `BAD_HANDLE` (torn-down
target), `INVALID_ARG` (no address space), `NO_MEMORY`.

There remains **no ambient route** to a foreign VSpace: the only path is an
explicit process capability with MANAGE (P24).

### Fault generations (NEW, additive)

Every delivered fault gets a per-process, 1-based, monotonic generation:

- `FAULT_OFF_SEQ` (offset 20 of the 32-byte record — previously `_pad`,
  always written 0, so a 0 means "pre-Fase-25 kernel").  Readable by any
  READ holder together with the rest of the record.
- The blocked task keeps its own copy (`task->fault_seq`): the per-process
  record is last-writer-wins (Fase 20 limit, unchanged), but each suspended
  task stays resolvable by ITS generation.

### Seq-checked resolution (NEW, additive)

`SYS_EXCEPTION_RESUME` actions `2` (resume) and `3` (kill) carry the expected
generation in bits [63:32] of the action argument.  Generation 0 is
`INVALID_ARG`; a mismatch — the task refaulted since, or the caller replays a
record it never matched — is `NOT_FOUND` with **no side effect** (P13).
Actions 0/1 keep the exact Fase 20 semantics; values 2/3 were `INVALID_ARG`
before this phase, so no existing caller changes behaviour.  Actions > 3
remain `INVALID_ARG`.

### FRAME_MAP/UNMAP VSpace argument: dual resolver (hazard closed)

`SYS_FRAME_MAP`/`SYS_FRAME_UNMAP` resolved their VSpace argument through the
raw radix walk only — a handle (≥ 1024) fed there was masked into low root
slots, the exact aliasing hazard class the Fase 8 namespace split closed for
every other capability argument (and Fase 21 documented as residual).  Fase
25 completes the A1 migration for these two syscalls: the VSpace argument now
uses the standard dual resolver (CPtr < 1024 → CSpace only; ≥ 1024 → handle
table only, `WRONG_TYPE`/`ACCESS_DENIED` with no fallback).  Consequence: a
supervisor or pager passes its `SYS_PROCESS_VSPACE` handle directly, with no
permanent CSpace-slot pin (ring 3 has no slot delete).  The CPtr namespace is
byte-for-byte unchanged.

## Contracts

- **Registration** — unchanged Fase 20: `RIGHT_MANAGE` on the target +
  `RIGHT_WRITE` on the notification; the supervisor registers, then mints the
  notification to the pager with `RIGHT_WAIT` only.  Registration is the
  supervisor's act; the pager cannot re-point delivery (it holds no WRITE on
  the notification and its MANAGE is scoped to resolution).
- **Delivery** — unchanged Fase 20: exactly one signal per fault, task
  suspended before the signal is observable, record stable while pending,
  no capability transfer, no handle created in any table.  New: the record
  carries the generation.
- **Observation** — `RIGHT_READ` on the target process cap is the information
  authority, and ONLY information: a READ holder cannot resume, kill or
  re-register (T184).  A process holding no cap for the target cannot even
  name it.
- **Map-into-target** — `SYS_FRAME_MAP(frame, target_vspace, va, flags)` with
  the pager's minted VSpace cap.  All Fase 19 hardening applies unchanged:
  page-aligned user-window VA only (kernel range `INVALID_ARG`), W^X,
  occupied VA `BUSY`, writable map requires frame `WRITE`, any install
  requires VSpace `WRITE`, denial installs nothing (no partial PTE).  The
  PTE carries the MAPPING's rights, never the cap's ceiling: a page mapped
  read-only through a fully-writable frame cap still write-protection-faults
  the target's store (T188).
- **Resume** — exact (process, task, BLOCKED_FAULT[, generation]) match or
  `NOT_FOUND`.  Resume re-executes the faulting instruction: if the pager
  mapped the page, the access retires; if not, a NEW fault with a NEW
  generation is delivered (clean refault, P10 — same rip/cr2, fresh seq,
  fresh signal).
- **Stale fault** — a resolution carrying an old generation can never touch a
  newer fault of the same task; late resolutions after resolve/death are
  `NOT_FOUND`; the record never outlives the fault (`WOULD_BLOCK` after).
  Cleanup happens exactly once per record (Fase 20 F15 + P12).
- **Pager death** — deliberately inherits the Fase 20 handler-death contract:
  the kernel does NOT auto-kill or auto-reassign.  The faulted target stays
  suspended with record, generation and pending delivery signal intact —
  observable state, not a zombie (P15).  Any proper authority (the
  supervisor, or a restarted pager re-minted the manifest) completes the
  resolution; the pending notification signal survives the pager because
  registration pins the notification object.  Crash-looping pagers are
  contained by the Fase 24 supervision policy (restart limit → degraded),
  and the supervisor — who never delegated away its own caps — resolves or
  kills (P17, T189).
- **Target death** — teardown clears the fault record, invalidates the
  VSpace and sweeps every mapping, including pager-installed ones (P14,
  P18).  Late pager steps fail clean: map/unmap `BAD_HANDLE`, resume
  `NOT_FOUND`, info `WOULD_BLOCK`.  The pager's frame survives (its caps are
  its own), with `mapped_count` back at baseline — reusable, contents
  intact.

## Interaction with the rest of the system

- **Supervision (Fase 24)** — a pager is just a supervised service whose
  manifest happens to include another process's caps.  Restart gives a fresh
  process with the declared mints and nothing else (T189: post-crash
  generations report exactly the manifest); generation/limit/degraded
  bookkeeping is the supervisor's, as for any service.
- **Service authority (Fase 22)** — the manifest mechanism (pre-start
  `SYS_PROC_CSPACE_MINT`) and the slot-report oracle are reused verbatim;
  the pager adds a new manifest *shape*, not a new mechanism.
- **VSpace/frame hardening (Fase 19)** — `SYS_FRAME_MAP/UNMAP` semantics are
  untouched; Fase 25 only adds a legitimate way to HOLD a foreign VSpace cap.
  The V20 isolation statement is refined, not weakened: cross-process
  address-space authority still requires an explicit MANAGE process cap —
  now optionally reified as an attenuable VSpace cap.
- **Fault endpoint model (Fase 20)** — delivery, registration, spoofing,
  record lifecycle: all unchanged.  Fase 25 adds the generation field in the
  record pad, the seq-checked resume actions, and the userland protocol on
  top.
- **IPC / scheduler / lifecycle** — no semantic change.  Pager traffic is
  notification signals + ordinary syscalls; KReply and live books stay at
  baseline in every test.

## Instrumentation

None added.  The Fase 20 ext5 fault counters (delivery/nohandler/resume/
kill/cleanup), the Fase 18/19 live gauges (frame/mapping/VSpace/endpoint/
notification/handle/task/process) and the per-process record itself cover
every observable this phase needs; the tests assert against them.  (A new
SYS_SCHED_INFO tier was considered and rejected: the gauge set is
sufficient, and Fase 23 documented the risk of growing that surface without
need.)

## Tests (runtime, deterministic)

```text
T181  pager authority manifest: a pager instance reports exactly the declared
      slots (cmd/tproc/tvs/frame/notif) — no spawn, device, untyped, KDEBUG,
      peer or self-authority slots; SYS_PROCESS_VSPACE is MANAGE-gated (READ
      denied, wrong type rejected, self allowed).
T182  external delivery: pager wakes on WAIT cap, validates honest info
      (cr2), seq-kills; exactly-once (no residual signal or record); books.
T183  map into target: RO map + pattern read (data flows supervisor→frame→
      target) and W map + store retires (target→frame→supervisor); target
      death sweeps the pager mapping; frame reusable; counters exact.
T184  unauthorized pager: full battery against a victim (READ-only proc/
      vspace caps, foreign task ids, unrelated target cap, device forgery)
      — zero breaches, record undisturbed, READ = info only.
T185  stale generation: gen1 → refault → gen2 (same rip/cr2); every gen1
      replay NOT_FOUND; gen 0 INVALID_ARG; proper map+resume on gen2; even
      the correct generation is late after resolution.
T186  pager death while pending: target suspended-alive with record+signal
      intact (not a zombie), no phantom receiver on the dead pager's
      endpoint; restarted pager with the same manifest completes the job.
T187  target death during resolution: late map BAD_HANDLE, late resumes
      NOT_FOUND, record gone; frame never installed stays clean; books.
T188  rights and PTE policy: RO-frame/RO-vspace denials, W^X, kernel/low/
      unaligned VA, no partial PTE, occupied-VA BUSY, r-x allowed; RO PTE
      refuses the store at hardware level (err P|W|U); late unmap after
      death BAD_HANDLE.
T189  restart least authority: crashing pager supervised under limit 2 →
      degraded, loop stops; fault survives every generation; post-crash
      instance reports exactly the manifest; serving generation resolves.
      (The Fase 24 ↔ Fase 25 junction test.)
T190  seeded stress (seed printed only on failure): 6 rounds × 2 concurrent
      pending faults, resolution path chosen by PRNG among pager map+resume,
      pager kill, pager death + supervisor takeover, target death + refault
      + stale rejection, unauthorized denials, occupied-VA + RO-PTE; per
      round: no pending fault, no zombie, live books at baseline.
```

`lifecycle_probe` gained two opt-in pager modes: `LP_CMD_PAGER_SERVE`
(wait → read info → validate → [map] → seq-resolve, N times) and
`LP_CMD_PAGER_XPROBE` (the unauthorized battery, exit = breach bitmask).
The pager slots are all < 16, so `LP_CMD_REPORT_SLOTS` doubles as the
manifest oracle.

## Deliberate limits (not gaps: scope)

- **No swap, no filesystem-backed paging, no copy-on-write.**  This phase
  fixes the authority contract; backing-store policy comes later and will
  sit entirely in userland on top of exactly these primitives.
- **No demand-paging by default.**  Eager mapping remains the normal path;
  the pager route is opt-in per process via fault-endpoint registration.
- **Pager ≠ driver.**  The pager model shares the manifest mechanism with
  drivers but holds no device authority (asserted in T181/T184).

## Remaining gaps (honest)

- One fault record per process (Fase 20 limit, unchanged).  Two tasks of one
  process faulting concurrently: both suspend, both stay resolvable by
  (task_id, generation) — `task->fault_seq` keeps resolution exact — but the
  info of the earlier one becomes unreadable.  Next honest step remains
  per-TCB records or a small queue.
- No per-vector filtering on the fault endpoint (unchanged; add via
  signal_bits conventions when a real consumer appears).
- The frame source in this phase is supervisor-minted frames.  A VMO-backed
  pager (`SYS_VMO_MAP_INTO` held by the pager, or per-page VMO grants) works
  today via MANAGE but has no attenuated form; that is the "memory object /
  VMO policy expansion" follow-up.
- Ring 3 still has no CSpace slot-delete: caps delivered into CSpace slots
  (receive-slot protocol, pre-start mints) occupy them for the process's
  lifetime.  The dual-resolver migration removed the pressure for pagers
  (handles suffice), but a real init/root-task design should revisit CSpace
  slot lifecycle as first-class.
