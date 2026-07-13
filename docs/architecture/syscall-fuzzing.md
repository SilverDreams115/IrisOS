# Fase 21 — Cross-syscall fuzzing / hostile argument surface

Status: ACCEPTED — implemented in this phase.  Companion to every prior
hardening doc (`fault-endpoint-model.md`, `vspace-frame-hardening.md`,
`untyped-retype-revoke-hardening.md`, `lifecycle-hardening.md`,
`scheduler-hardening.md`, `ipc-stress-invariants.md`).  The earlier phases
hardened each subsystem by family; Fase 21 subjects the WHOLE syscall surface
to deterministic adversarial pressure and proves it fails clean.  Locked with
runtime tests T148–T155.

**No new kernel bug was found.**  The audit confirmed that unknown/retired
numbers, wrong-type caps, stale/out-of-range handles, hostile user pointers,
reduced rights and forced mid-syscall failures all fail clean with no drift in
any live gauge — the invariants the previous phases established hold under
combination, not only in isolation.  This phase adds no kernel behaviour: it is
tests, a snapshot harness reusing existing instrumentation, and this document.
The runtime selftest suite rises from 143/143 to 151/151.

The bar for this phase is explicit.  Not accepted: *"the fuzz did not crash."*
Accepted: *"the syscall surface fails clean under deliberate hostility."*

---

## The syscall surface

IRIS routes syscalls through an exact-match switch (`syscall_dispatch.c`) over
numbers 0..106; every unrouted number returns `IRIS_ERR_NOT_SUPPORTED`.  There
is no number-range table and no computed dispatch, so an unknown or aliased
number cannot fall through to a live handler.

| Family | Syscalls | Authority | Fuzz priority |
|---|---|---|---|
| Process lifecycle | SELF, STATUS, WATCH, KILL, CREATE, EXIT_CODE | proc cap RIGHT_MANAGE/READ, or self | high |
| TCB / thread | TCB_SELF/SUSPEND/RESUME/SET_PRIORITY/EXIT/GET_INFO, THREAD_* | TCB/self | high |
| Scheduling context | SC_CREATE, SC_CONFIGURE, THREAD_SET_SC | SC cap | high |
| IPC endpoint | ENDPOINT_CREATE, EP_SEND/RECV/NB_SEND/NB_RECV/CALL, REPLY | endpoint cap RIGHT_WRITE/READ | high |
| Notification | NOTIFY_CREATE/SIGNAL/WAIT/WAIT_TIMEOUT | notif cap RIGHT_WRITE/WAIT | high |
| Capability / CSpace | CAP_DERIVE/REVOKE, CNODE_*, CSPACE_RESOLVE, PROC_CSPACE_MINT, HANDLE_DUP/INSERT/TYPE/SAME_OBJECT/CLOSE | cap rights + CNode | high |
| Untyped memory | UNTYPED_INFO/RETYPE/RESET | untyped cap RIGHT_READ/WRITE | high |
| Frame / VSpace | FRAME_MAP/UNMAP, VSPACE_SELF, VMO_* | frame+VSpace cap rights | high |
| Exception / fault | EXCEPTION_HANDLER/RESUME, PROCESS_FAULT_INFO | proc cap RIGHT_MANAGE/READ | high |
| Device / IRQ | IOPORT_IN/OUT, IRQ_*, CAP_CREATE_IOPORT/IRQCAP, IOPORT_RESTRICT | device caps | medium |
| Diagnostic | SCHED_INFO, KLOG_DRAIN, UNTYPED_INFO | KDEBUG bootstrap cap | medium |
| Clock / misc | CLOCK_GET/NANOSLEEP, SLEEP, YIELD, GETPID, POWEROFF | none / self | low |
| Retired / reserved | WRITE(0), OPEN..BRK(4–7), CHAN_*(12–14,34,37,38,63), SPAWN(18), HANDLE_TRANSFER(23), NS_*(24,25), DIAG_SNAPSHOT(30), SPAWN_SERVICE(31), INITRD_LOOKUP/SPAWN_ELF(41,42), WAIT_ANY(44,72) | — | table only |

### Two namespaces, one resolver

Most cap-taking syscalls accept EITHER a handle-table id OR a CSpace CPtr,
resolved by `cspace_or_handle_resolve_*`.  This is load-bearing for fuzzing:

- A `handle_id_t` encodes `slot` in bits[9:0] and `generation` in bits[31:10];
  generation 0 is forbidden and `HANDLE_TABLE_MAX` is 256, so any value with
  slot ≥ 256 (e.g. `0x7FFFFFFF`) is invalid as a handle.
- A CPtr is valid in 0..1023 (`IRIS_CPTR_LIMIT` = 1024); CPtrs 1..11 name real
  boot capabilities (svcmgr/vfs/console/kbd/spawn/…).

Therefore a *small* integer is NOT a "bad handle" — as a CPtr it may name a
live capability, and honouring it is correct.  A value safe to feed to a
mutating syscall as a hostile token must be invalid in BOTH namespaces: slot ≥
256 AND > 1023.  The fuzz uses `4095, 0x1FFFF, 0x7FFFFFFF, 0xFFFFFFFF`, plus
freshly-closed (stale-generation) handles.  Feeding CPtr 1 to `EP_SEND` and
expecting failure would be a test bug, not a kernel bug — this is documented so
future fuzz authors do not "fix" it wrongly.

## Invariants X1–X24

```text
X1  Unknown/retired/reserved number → clean error, no side effect.
X2  Wrong-type cap/handle → fail, no fallback.
X3  ACCESS_DENIED has no fallback.
X4  Empty slot → fail, no side effect.
X5  Dead/reaped object → fail clean.
X6  Invalid user pointer → controlled error/fault, never kernel corruption.
X7  Invalid size → no overflow.
X8  Invalid rights → no authority amplification.
X9  Repeating a failed syscall does not change state.
X10 Partially-failed syscall leaves no ref.
X11 Cancelled blocking syscall leaves no dead waiter.
X12 CSpace keeps no ghost slot.
X13 VSpace keeps no ghost PTE.
X14 IPC neither loses nor duplicates caps.
X15 Scheduler live counts return to baseline.
X16 Process live counts return to baseline.
X17 Fuzzer faults are observable and cleanable.
X18 No kernel fault is inducible from userland arguments.
X19 Handle-table HWM stays within bound.
X20 No KReply leak.
X21 No notification/endpoint ref leak.
X22 No mapped_count drift.
X23 No untyped/frame/cnode live-count drift.
X24 No syscall crosses authority between processes without an explicit cap.
```

## The deterministic fuzz model

Each test seeds `fz_rand` (xorshift32) with a fixed constant and, on failure
only, prints `FZ <test> seed=<seed> iter=<i> op=<op>` — enough to replay the
exact path.  Synchronisation is always a control-endpoint rendezvous or a
bounded `NOTIFY_WAIT_TIMEOUT`; there are no long sleeps and no timing races.

The anchor is a full-surface **snapshot** (`it_snap`) built from the existing
`SYS_SCHED_INFO` tiers plus the task-live word — every live-object gauge the
kernel exposes: task, process, handle, untyped/frame/endpoint/notification/
cnode objects, VSpace, mapping nodes, the KReply balance and the pending-fault
counters.  A hostile op that leaks a ref, plants a ghost slot/PTE or strands a
waiter moves at least one gauge, so equality-to-baseline after the churn is a
strong, cheap invariant.

Two comparators exist by design:

- `it_snap_baseline` — full equality, for single-process tests (T148–T152,
  T154).  Every gauge including per-type object counts must match.
- `it_snap_baseline_live` — task/process/handle/KReply/VSpace/mapping only, for
  tests that SPAWN child processes (T153, T155).  Loading a service via
  `svc_load` creates transient child-owned objects (bootstrap endpoint,
  segment frames, CNode) whose reaping is deferred, so the per-type object
  counts are not a reliable per-test balance across a child spawn — the
  canonical cross-process churn test (T114) omits them for the same reason.
  The gauges that DO return exactly to baseline after child teardown are the
  ones this comparator checks, and the fault suite (T145–T147) already proved
  them reliable across kill.

### What counts as an allowed side effect

Only **cumulative** counters may move across a fuzz run: `SYS_SCHED_INFO` map/
unmap/TLB-invalidate totals, the fault delivery/resume/kill/cleanup counters,
context-switch and yield counters, and monotonic high-water marks.  These are
checked as directional deltas ONLY where a test deliberately triggers the
event.  Every **live** gauge must return exactly to baseline.  The global
handle high-water rule (`ghwm*4 ≤ max`) is a monotonic gauge asserted by
T095/T112 in their own contexts, not a per-test balance, so the fuzz baseline
excludes it (it would fire on HWM inherited from the fault-stress tests).

### How expected faults are cleaned

A fuzzer child that faults on purpose (invalid-VA/RO-write/NX/kernel-range) is
handled through the Fase 20 fault-endpoint model: a handler notification is
registered on the child, the fault is delivered and read, and the child is
resolved by `SYS_EXCEPTION_RESUME` (kill) or `SYS_PROCESS_KILL`.  The fault is
therefore an observable, cleanable authority event, never a silent process
death that would strand the suite — X17.  A child fuzzing a hostile *pointer*
(T150) never faults at all: the kernel validates every user range by
page-table walk before touching it, so the syscall returns `INVALID_ARG` — X6,
X18.

## Tests

```text
T148  syscall table / retired fuzz: every hole in 0..106, the high range
      107..400, and numbers whose low 32 bits alias a live handler — all must
      return NOT_SUPPORTED with zero drift.  No live number is ever fuzzed
      (SYS_EXIT etc. would self-destruct — the tables are holes only).
T149  CPtr/handle/wrong-type fuzz: notification↔endpoint↔process wrong-type
      crossings, both-namespace-invalid boundary handles across a spread of
      mutating families, and a stale (closed) handle.  Every one fails, none
      mutates, none amplifies.
T150  user pointer/buffer fuzz: null / kernel-half / unmapped / below-min
      pointers to SCHED_INFO, PROCESS_FAULT_INFO, UNTYPED_INFO,
      NOTIFY_WAIT_TIMEOUT and EP_SEND → INVALID_ARG, no kernel fault, no
      waiter created; size 0 / below-base / huge (clamped) on SCHED_INFO; a
      valid call still works afterwards.  (Null out-params on UNTYPED_INFO are
      LEGAL — optional fields — and are asserted to succeed.)
T151  cross-family hostile sequence: seeded mix of malformed and well-formed
      ops across untyped/frame/cap/notify/endpoint/fault against a small
      liveness model; malformed ops never advance the model, the full snapshot
      unwinds to baseline every batch.
T152  failure atomicity: occupied-VA BUSY, missing-rights ACCESS_DENIED,
      wrong-type, invalid VA/size/pointer, resume mismatch — each all-or-
      nothing (snapshot before == after), and a following valid op works.
T153  blocking cancellation: a child parked in EP_RECV / EP_SEND / EP_CALL /
      fault-pending, torn down by process kill / endpoint close / handler drop
      in a seeded mix; books balance (lifecycle-reliable gauges) every round.
T154  rights monotonicity: a RIGHT_READ notification dup cannot signal and
      cannot re-derive RIGHT_WRITE; a RIGHT_READ frame cap maps read-only but
      is denied writable; the full cap still works — no amplification, no
      corruption, across CPtr and handle paths.
T155  full syscall stress: seeded rounds combining object churn, map/unmap,
      cap derive/revoke, notify rendezvous and child spawn/kill/fault within
      fixed budgets and aggressive per-round cleanup; every gauge returns to a
      per-round and a final baseline.
```

## Adding a syscall without losing coverage

1. Route it in `syscall_dispatch.c`; unrouted numbers already fail X1.
2. If it takes a cap/CPtr, resolve through `cspace_or_handle_resolve_*` with an
   explicit rights check and no fallback (X2/X3/X8), and add it to a wrong-type
   / boundary-handle sweep in T149.
3. If it takes a user pointer, validate the FULL range with `user_range_*`
   BEFORE any state change or block (X6/X10/X11), and add a hostile-pointer
   case to T150.
4. If it creates/destroys an object, blocks, or mutates CSpace/VSpace, add it
   to the T151/T155 mixed streams so its live gauge is balanced under churn.
5. If it can fault a caller, ensure the fault is deliverable/cleanable (Fase 20)
   and cover it in T153.

## Remaining fuzzing gaps

- **Device/IRQ family** (IOPORT/IRQ caps) is exercised only lightly — those
  caps are held by specific drivers, not the test process; a driver-isolation
  phase is the right place to fuzz them under authority.
- **TCB/SC family** hostile combinations are covered by the Fase 17 tests
  (T119–T124) rather than re-fuzzed here; a future pass could fold them into
  the T151 stream.
- **Multi-threaded intra-process races** are out of scope on single-CPU with
  cooperative-plus-timer scheduling; real concurrency fuzzing belongs to an
  SMP-groundwork phase.
- The fuzz asserts CSpace has no ghost slot indirectly (via object liveness and
  RESET gating); a direct CNode-slot enumerator from ring 3 does not exist by
  design, so ghost-slot detection stays indirect.
