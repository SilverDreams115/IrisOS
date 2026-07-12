# Fase 20 — Fault endpoint / exception delivery model

Status: ACCEPTED — implemented in this phase.  Companion to
`vspace-frame-hardening.md` (Fase 19), `lifecycle-hardening.md` (Fase 16) and
`scheduler-hardening.md` (Fase 17).  Fase 20 closes the gap Fase 19 left
explicit: ring 3 can now observe and drive write-protection / NX / invalid-VA
page faults through an explicit, capability-mediated delivery model instead of
"the kernel kills the task".  Locked with runtime tests T140–T147.

**One real kernel bug was found and fixed** (shared-IST frame clobbering, see
below), plus two hardening defects in the registration/teardown border.  The
runtime selftest suite rises from 135/135 to 143/143.

---

## The model in one paragraph

A user fault is an authority event.  A supervisor that holds `RIGHT_MANAGE`
over a process registers a `KNotification` as that process's fault endpoint.
When a task of the process faults in ring 3, the kernel records the fault in
the `KProcess`, suspends the task in `TASK_BLOCKED_FAULT`, and signals the
notification.  The supervisor reads the record (`RIGHT_READ`), decides, and
resolves it (`RIGHT_MANAGE`): resume re-executes the faulting instruction;
kill reaps the task.  Nothing about the fault is ambient: registration,
observation and resolution each require an explicit capability over the
target process, and the fault record cannot be fabricated from userland.

## Syscall surface (unchanged numbers, no new syscalls)

```text
SYS_EXCEPTION_HANDLER(proc_h, notif_h, signal_bits)   47
SYS_EXCEPTION_RESUME (proc_h, task_id, action)        66
SYS_PROCESS_FAULT_INFO(proc_h, out_uptr)             105
```

- `SYS_EXCEPTION_HANDLER` — `proc_h`: `KOBJ_PROCESS` with `RIGHT_MANAGE`
  (or `HANDLE_INVALID` = self); `notif_h`: `KOBJ_NOTIFICATION` with
  `RIGHT_WRITE`; `signal_bits != 0`.  Re-registration replaces the handler
  (last registration wins).  Registering on a torn-down process fails
  `NOT_FOUND` (Fase 20 hardening — see bugs).  Registering on a created but
  not-yet-started process is legitimate and is the race-free supervisor
  order: wire the handler first, start the child second.
- `SYS_PROCESS_FAULT_INFO` — `RIGHT_READ`; fills the 32-byte record of
  `iris/fault_proto.h` (vector, task_id, rip, error code, cr2).  Returns
  `WOULD_BLOCK` when no fault is pending — including after resolution and
  after process death (the record never outlives the fault).
- `SYS_EXCEPTION_RESUME` — `RIGHT_MANAGE`; `action 0` wakes the task at the
  faulting rip (the instruction re-executes; if the condition persists a NEW
  fault is generated), `action 1` kills the task.  The target must be a task
  of that process currently in `TASK_BLOCKED_FAULT`, else `NOT_FOUND`; both
  actions clear the pending record.

## Delivery path

```text
ring-3 exception (vector < 32, CS.RPL == 3, not NMI/#DF/#MC)
  → isr_common: #GP/#PF only — migrate the 22-qword frame off IST1 onto the
    task's own kernel stack (%gs:48 = cpu_local.syscall_kstack)
  → isr_handler: kprocess_notify_fault(task, vector, err, rip, cr2)
      handler registered → record fault in KProcess (fault_valid = 1),
                           signal the notification,
                           task->state = TASK_BLOCKED_FAULT, task_yield()
      no handler         → log one diagnostic line, task_exit_current()
kernel exception (CS.RPL == 0) or NMI/#DF/#MC → panic dump + halt, always
```

The faulting task does not execute a single further user instruction while
the fault is pending: the fault is raised by the CPU before the instruction
retires, and the task blocks inside the ISR.  A read of the record while
pending is stable and repeatable; delivery signals the notification exactly
once per fault.

### Why kernel faults are never delivered

A ring-0 fault means the kernel itself is broken; no userland component can
be trusted to arbitrate that, and any delivery path would hand a corrupted
kernel's state to a process.  NMI, #DF and #MC are fatal regardless of CPL
(hardware/kernel integrity, not process behaviour).  This is a hard
invariant (F2), not a policy default.

### The shared-IST bug (found and fixed this phase)

`#GP`/`#PF` enter through IST1 — a fixed 16 KB per-CPU stack that the CPU
rewinds to the top on every entry.  Since Fase 13, a delivered fault blocks
the task *inside the ISR*, leaving its live context (interrupt frame, C
frames, `context_switch` state) inside IST1.  The next `#GP`/`#PF` from any
other task would start pushing at the same IST1 top and destroy the
suspended task's context — resume would then pop garbage and iretq to a
corrupted frame.  The bug was unreachable before fault delivery existed
(the faulting task was killed and never resumed); Fase 20 makes suspension
routine, so it had to be fixed at the root:

- For vectors 13/14 originating in ring 3, `isr_common` now copies the full
  22-qword frame onto the task's own kernel stack (guaranteed empty on any
  ring-3 entry; located via `%gs:48`) and continues there.  Blocking,
  resume and the final `iretq` all run on per-task memory.
- Ring-0 `#GP`/`#PF` stay on IST1: that path is fatal and never returns, and
  IST1 remains necessary for the SYSCALL entry window where CPL is 0 but
  RSP is still user-controlled.
- Vectors without IST (e.g. #UD, #DE) already enter via TSS.RSP0 (per-task)
  and were never affected.

T147 keeps two children suspended in-fault simultaneously every round as the
permanent regression fixture.

## Fault record and wire format

`iris/fault_proto.h`, 32 bytes, written only by the kernel:

```text
offset  0: uint32_t vector      x86 exception vector
offset  4: uint32_t task_id     faulting task
offset  8: uint64_t rip         user rip at fault
offset 16: uint32_t error_code  CPU error code (0 if N/A)
offset 24: uint64_t cr2         #PF address (vector 14 only)
```

Information exposure: rip and cr2 are the faulting process's own user-space
values; the error code is architectural.  No kernel address, kernel stack
value or other process's state appears in the record (F8).  The #PF error
code preserves the architectural bits, so write faults (`W`), user faults
(`U`) and instruction-fetch/NX faults (`I`) are distinguishable (F21):

```text
invalid-VA user read   err = 0x04        (U)
invalid-VA user write  err = 0x06        (W|U)
RO-page user write     err = 0x07        (P|W|U)
NX user fetch          err = 0x15        (P|U|I), cr2 == rip
```

Spoofing (F9): the record lives in kernel memory and is written exclusively
by `kprocess_notify_fault`.  A process holding the handler notification can
signal it by hand, but `SYS_PROCESS_FAULT_INFO` then reports `WOULD_BLOCK` —
a signal without a kernel-written record carries no authority and no
information.  T140 proves this.

## Task and record lifecycle

```text
        fault                    resume(0)              re-fault
RUNNING ─────→ TASK_BLOCKED_FAULT ─────→ READY → RUNNING ─────→ BLOCKED_FAULT
                     │ resume(1) / SYS_PROCESS_KILL
                     ▼
                 killed → teardown → reap
```

- The record (`fault_valid` in `KProcess`) is set at delivery and cleared by
  exactly three events: `SYS_EXCEPTION_RESUME` action 0, action 1, and
  `kprocess_teardown` (F15).  Each clear increments the cleanup counter.
- A resumed-and-refaulted task produces a fresh record and a fresh signal —
  never a reuse of the resolved one (F16; T142/T144 assert rip/cr2/error
  equality across the re-fault to prove it is the same instruction, and the
  counters to prove it is a new delivery).
- One record per process (last-writer-wins).  See "Current limits".

## Interaction with the rest of the kernel

- **IPC** — none.  Delivery is a `KNotification` signal; no KChannel, no
  KReply, no endpoint queue is involved.  A fault-blocked task is not on any
  wait queue, so `task_cancel_blocked_waits` is a no-op for it.  T141/T145/
  T146/T147 assert zero KReply drift.
- **Scheduler** — `TASK_BLOCKED_FAULT` is out of the run queue; resume is a
  plain `task_wakeup`, kill a plain `task_kill_external`.  Live counts are
  asserted back to baseline in every test.
- **Lifecycle/reap** — killing a fault-blocked task follows the standard
  teardown: cancel waits (no-op), free stacks, teardown at last thread, reap
  off-CPU.  Teardown clears the fault record and (twice, see below) the
  registered notification pin.
- **VSpace/VM** — the fault path reads cr2 and touches no mapping state;
  process death sweeps mappings exactly as in Fase 19 (`mapped_count`
  baseline asserted per test).  The write-protection and NX faults are the
  hardware-level proof of the Fase 19 rights model: PTE.W and PTE.NX now
  observably enforce what the authority layer promised (T138 gap closed).
- **CSpace/capabilities** — registration/observation/resolution resolve
  through the standard dual resolver with `RIGHT_MANAGE`/`RIGHT_READ`
  checks and no fallback.  Delivery transfers no capability and creates no
  handle in any table (F17/F18): the handler learns facts, not authority.

## Contracts

- **Registration**: `RIGHT_MANAGE` over the target + `RIGHT_WRITE` over the
  notification; wrong types `WRONG_TYPE`; missing rights `ACCESS_DENIED`
  with no fallback and no partial installation; `signal_bits == 0`
  `INVALID_ARG`; torn-down target `NOT_FOUND`.  Replacement is atomic:
  after re-registration only the new notification fires.
- **Delivery**: user faults only; exactly one signal per fault; faulting
  task suspended before the signal is observable; record readable and
  stable while pending.
- **Resume**: exact (process, task, state) match or `NOT_FOUND`; `action >
  1` `INVALID_ARG`; unauthorized `ACCESS_DENIED`; both actions clear the
  record; resume without a pending fault fails clean with no side effect.
- **Kill/cleanup**: killing the process with a fault pending clears the
  record, releases the notification pin, cancels nothing stale, and leaves
  every book (tasks, handles, notification/endpoint objects, mappings,
  KReply) at baseline.  A late handler response after the kill is a clean
  `NOT_FOUND` / `WOULD_BLOCK`.
- **Handler death / endpoint close**: the notification HANDLE is not the
  resolution authority — the process cap is.  Registration pins the
  notification object, so the handler closing (or dying and thus closing)
  its handle changes nothing about the pending fault: the faulted task
  stays suspended, and any `RIGHT_MANAGE` holder can still resolve it.
  Handler death deliberately does NOT auto-kill the faulted process: the
  kernel cannot know which supervisor policy is right, so it keeps the
  smallest honest contract — state is preserved until an authority acts.
  The cost (a suspended task pinned until its supervisor tree acts or the
  process is killed) is bounded by the same authority that created the
  process.

## Instrumentation (additive, silent)

`SYS_SCHED_INFO` grows an ext5 tier (buffer ≥ 184 bytes; smaller requests
get the exact previous tiers — same additive rule as ext1–ext4):

```text
offset 160: fault_delivery_count   faults handed to a registered handler
offset 164: fault_nohandler_count  faults with no handler (task killed)
offset 168: fault_resume_count     RESUME action 0
offset 172: fault_kill_count       RESUME action 1
offset 176: fault_cleanup_count    pending records cleared (resolve/teardown)
offset 180: (pad)
```

Zero cost on the fault-free path beyond one relaxed atomic per event; no
boot noise; no timing dependence.

## Tests (runtime, deterministic)

```text
T140  registration authority: wrong types, reduced rights (no fallback), zero
      bits, empty slot, no-partial-install (fault after failed registrations
      takes the kill path), replacement contract, spoof check, dead-process
      registration.
T141  invalid-VA delivery: exactly-once, honest info (vector/cr2/rip/err),
      suspension while pending, record stability, kill-resolution, books.
T142  write-protection on the child's own r-x text: read completes, write
      faults 0x07, store never retires, resume re-faults at the same rip/cr2,
      closes the Fase 19 T138 gap.
T143  NX instruction fetch from the child's stack: err 0x15, cr2 == rip.
T144  resume semantics: unauthorized denied, bogus id NOT_FOUND, bad action
      INVALID_ARG, resume→new fault, kill→no stale record, late resume clean.
T145  handler drop mid-fault: close the notification handle, resolve via the
      proc cap (resume-kill and PROCESS_KILL variants); no zombies; object
      books at baseline.
T146  process kill while pending: teardown clears the record; late responses
      fail clean; task/handle/KReply/mapping books at baseline.
T147  seeded stress: mixed fault kinds (invalid VA / RO write / kernel-range /
      NX) × mixed resolutions (resume-refault-kill / resume-kill / kill /
      close-then-kill), two concurrent suspended faults every round (shared-
      IST regression), non-faulting children interleaved; all books and
      counters at baseline; prints seed/iteration only on failure.
```

`lifecycle_probe` gained three opt-in fault-trigger commands
(`LP_CMD_FAULT_READ/WRITE/EXEC`, words[0] = target VA, 0 = the child's own
ASLR-biased code address — the parent cannot know it, so the child resolves
it).  The init S8 selftest (intra-process, TCB-thread fault) remains as
smoke-level coverage.

## Current limits / remaining gaps

- **One fault record per process** (last-writer-wins).  Two tasks of the same
  multi-threaded process faulting concurrently overwrite the record; both
  tasks block and both remain individually resumable by task_id, but the
  info of the first becomes unreadable.  Honest next step: per-TCB fault
  records, or a small per-process fault queue.
- **No fault redirection to a third-party pager**: resume re-executes; there
  is no "fix the mapping on the child's behalf" primitive yet (a supervisor
  cannot map into another process's VSpace — Fase 19 V20 isolation).  This
  is the groundwork boundary for a user pager (VM policy) phase.
- **No per-vector filtering**: one endpoint receives every deliverable
  vector.  Cheap to add later via signal_bits conventions if a real consumer
  appears; deliberately not invented now.
- **Handler death is not auto-detected**: a faulted child whose supervisor
  died without resolving stays suspended until some other authority acts.
  Documented contract, revisit only with a real supervision-tree design.
