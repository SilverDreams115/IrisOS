# Fase 17 — scheduler / Scheduling-Context hardening + SMP-readiness

Status: ACCEPTED — implemented in this phase.  Companion to
`lifecycle-hardening.md` (Fase 16, exit/kill/reap) and
`ipc-stress-invariants.md` (A1.11, the deferred-reap slot-reuse fix).  Fase 17
audits the scheduler as the microkernel's source of truth about which task is
alive, runnable, blocked, dead or pending-reap, locks its run-queue and
Scheduling-Context invariants with runtime tests T119–T124, and writes down the
single-core assumptions a future SMP port must revisit.

**No new scheduler bug was found.**  The audit confirmed the run-queue,
IPC-blocking and Scheduling-Context paths are correct; this phase makes their
guarantees observable (additive `SYS_SCHED_INFO` instrumentation) and
regression-proof (T119–T124).  The runtime selftest suite rises from 114/114 to
120/120.

---

## Scheduler model

The scheduler lives in three translation units under
`kernel/core/scheduler/`:

- `scheduler.c` — the loop: `task_yield`, `scheduler_tick`, `scheduler_sleep_current`,
  idle-clock fast-forward, diagnostics accessors.
- `task_lifecycle.c` — task creation/teardown, the O(1) per-CPU run queue,
  `task_wakeup`/`task_suspend`, the deferred-reap queue, shared scheduler state.
- `kstack.c` — kernel-stack allocation.

Scheduling is **priority-preemptive, round-robin within a priority band, and
cooperative-dominant in practice**:

- Each task has an 8-bit `priority` (0 = idle, 128 = default user, 255 = max).
- The run queue is a 256-band structure: per-priority FIFO intrusive lists plus
  a 256-bit `mask` bitmap.  `rq_dequeue_best` picks the highest non-empty band
  in O(1) via `__builtin_clzll`.
- A running task holds the CPU until it yields, blocks, exits, exhausts its
  quantum (`TASK_DEFAULT_SLICE = 2` ticks) or a higher-priority task becomes
  runnable — at which point `scheduler_tick` sets `need_resched`.
- Under the QEMU TCG headless target no timer IRQs are delivered while a task
  spins in ring 0, so on that target forward progress between tasks is carried
  by explicit `task_yield`.  Preemption of ring-3 code by the PIT tick is real;
  preemption of a ring-0 spinner is not delivered by TCG.  (See **Limits**.)

The **idle task** is `tasks[0]` (= `task_list_head`), priority 0.  It is never
placed on a run queue; it runs only via the explicit fallback in `task_yield`
when `rq_dequeue_best()` returns NULL.  When idle is current and nothing is
runnable, `sched_handle_idle` fast-forwards `scheduler_ticks` to the nearest
sleeper/timed-block deadline so timed tasks wake even without IRQs.

### Task states

`task_state_t` (`kernel/include/iris/task.h`); a slot is in exactly one:

```text
TASK_READY            runnable, enqueued in a run queue
TASK_RUNNING          on-CPU (current_task)
TASK_BLOCKED_IPC      waiting on a KNotification / legacy timed block
TASK_BLOCKED_IRQ      waiting on a KNotification signal
TASK_SLEEPING         waiting until scheduler_ticks >= wake_tick
TASK_BLOCKED_FAULT    suspended pending fault-handler decision
TASK_BLOCKED_SEND     endpoint sender (or EP_CALL with no receiver yet)
TASK_BLOCKED_RECV     endpoint receiver
TASK_BUDGET_EXHAUSTED SchedContext budget spent; sleeping until refill tick
TASK_BLOCKED_REPLY    EP_CALL caller holding a KReply, awaiting sys_reply
TASK_SUSPENDED        explicitly parked (SYS_TCB_SUSPEND)
TASK_DEAD             free, OR dead-but-not-yet-reaped when awaiting_reap = 1
```

`task_is_runnable(s)` is true only for `TASK_READY` / `TASK_RUNNING`.

### Transition rules

```text
create           → TASK_READY        (rq_enqueue + live_count++)
dispatch         READY → RUNNING     (rq_dequeue_best in task_yield)
preempt/yield    RUNNING → READY     (re-enqueue if still RUNNING)
block (ipc)      RUNNING → BLOCKED_* (removed from rq by being dequeued)
wakeup           BLOCKED_* → READY   (task_wakeup → rq_enqueue)
sleep            RUNNING → SLEEPING  (wake_tick set; woken by tick/idle scan)
budget spent     RUNNING → BUDGET_EXHAUSTED (wake_tick = now + period)
budget refill    BUDGET_EXHAUSTED → READY   (remaining_budget reset)
self-exit        RUNNING → DEAD (awaiting_reap=1) → reaped off-CPU
external-kill    any(non-current) → DEAD, reaped inline
suspend          any → SUSPENDED     (rq_remove)
```

Invariant enforcement points:

- **`task_wakeup`** is the only READY-transition for blocked tasks; it refuses
  `TASK_DEAD` and never enqueues the idle head.
- **`rq_enqueue`** is the single guard for "a task is in the run queue at most
  once": it rejects a task whose `queued[idx]` bit is already set.
- **`task_yield`** re-enqueues the outgoing task only while its state is still
  `TASK_RUNNING`, so a task that blocked/slept/exited during its slice is never
  re-made-runnable by the switch itself.

---

## Run-queue invariants

The O(1) run queue (`struct CpuRunQueue`, one per CPU) maintains:

- `queued[TASK_MAX]` — the authoritative "is this task enqueued" bit; the
  duplicate-enqueue guard.
- `head[256]/tail[256]/next[TASK_MAX]` — per-priority FIFO links.
- `mask[4]` — 256-bit non-empty-band bitmap kept in lockstep with head/tail.

Every mutation runs under the run queue's `irq_spinlock`.  `task_reset_slot`
calls `rq_remove` **before** zeroing a slot, so a dead slot is never left linked.

---

## Relationship to IPC

Endpoint IPC (`syscall_endpoint.c`, `syscall_reply.c`, `kendpoint.c`) drives
task states directly:

- A receiver with no sender sets `TASK_BLOCKED_RECV`, links onto `ep->queue`,
  and `task_yield`s.  A sender with no receiver sets `TASK_BLOCKED_SEND`
  (EP_CALL uses the same queue with `ep_call_mode = 1`).
- Rendezvous wakes the peer via `task_wakeup` (→ `rq_enqueue`).  EP_CALL keeps
  the caller `TASK_BLOCKED_REPLY` until `sys_reply`.
- A blocked waiter holds a **refcount** ref on the endpoint but **not** an
  active-ref, so closing the last handle fires `kendpoint_obj_close`, which
  wakes every queued waiter with `ipc_ep_closed = 1` (→ `IRIS_ERR_CLOSED`) and
  empties the queue — no dead waiter is left schedulable.
- `kendpoint_cancel_waiter` (from `task_cancel_blocked_waits` on kill) unlinks a
  dying task from the endpoint queue and releases any staged cap **without
  consuming the source handle** (A1.10), leaving `t->state` for the reaper.

Endpoint blocks are indefinite: `wake_tick` is **not** set for
BLOCKED_RECV/SEND/REPLY, so the `scheduler_tick` / idle timeout scan (which only
touches SLEEPING, BUDGET_EXHAUSTED, BLOCKED_IPC, BLOCKED_IRQ) never spuriously
wakes an endpoint waiter.

---

## Relationship to lifecycle / reap

Unchanged from Fase 16 and re-audited here:

- **Self-exit** (`task_exit_current`): sets `awaiting_reap = 1` then
  `TASK_DEAD`, spins on `task_yield`.  The switch enqueues it on `reap_queue`;
  `reap_pending_dead_task` drains one per yield/tick off-CPU
  (`reap_dead_task_off_cpu` → `sched_live_count--`, `unlink_task`,
  `task_release_sched_ctx`, `task_reset_slot`, and `kprocess_free` for the last
  thread).
- **External kill** (`task_kill_external`): target is off-CPU, so the same
  cleanup — including **`task_release_sched_ctx`** — runs inline.
- `awaiting_reap` reserves the dead slot until the reaper zeroes it, closing the
  A1.11 slot-reuse window; the three `task_create*` scans skip it.

Both paths funnel the Scheduling-Context release through the same
`task_release_sched_ctx` helper, so SC teardown is identical for exit and kill.

---

## Relationship to Scheduling Context

A `KSchedContext` (`kschedctx.c`) carries `{budget_ticks, period_ticks,
remaining_budget}`.

- `SYS_SC_CREATE` allocates one and hands a handle (`RIGHT_READ|WRITE|DUP|
  TRANSFER`) to the caller's process.  `SYS_SC_CONFIGURE` validates
  `budget != 0 && period != 0 && budget < period` (else `IRIS_ERR_INVALID_ARG`)
  and requires `RIGHT_WRITE`.  `SYS_THREAD_SET_SC` binds/rebinds/unbinds the
  current task's `sched_ctx` (retained ref; `sc_h = 0` unbinds and releases).
- Enforcement lives in `scheduler_tick`: a `TASK_RUNNING` task with an SC
  decrements `remaining_budget`; at 0 it becomes `TASK_BUDGET_EXHAUSTED` with
  `wake_tick = now + period`, refilled by the tick/idle scan.
- **Lifetime:** the SC object is counted live from alloc to destroy.  Binding
  adds a task ref (not an object); the object is destroyed only when *every*
  ref drops — the bound task's ref (released at reap/kill via
  `task_release_sched_ctx`) **and** every handle.  So a leak, a double free, or
  a ref surviving a dead task is observable as drift in the live-SC count.

---

## Instrumentation (additive, ABI-safe)

Fase 17 adds a fourth `SYS_SCHED_INFO` tier, written only when the caller passes
`buf_size >= 112` (a caller passing 96..111 gets the exact historical 96-byte
snapshot — same additive rule as the Fase 16 / A1.7 tiers, no signature or
syscall-number change):

```text
offset  96: uint32_t run_queue_hwm            high-water of concurrently-enqueued tasks
offset 100: uint32_t duplicate_enqueue_count  times the S4 guard rejected a re-enqueue
offset 104: uint32_t sched_ctx_live           live KSchedContext objects
offset 108: uint32_t yield_count              monotonic task_yield() entries
```

None of it changes scheduling decisions.  `run_queue_hwm` / the enqueue live
count are maintained under the run-queue lock; the rest are relaxed atomics.
`duplicate_enqueue_count` is the counter behind invariant **S4**: the
`queued[]` guard is the *mechanism* that keeps a task out of the queue twice,
and this counts how often it engaged.  It is a benign, expected event (e.g. a
rendezvous racing a timeout wakeup), so tests assert it stays *bounded*, never
that it is zero.

---

## Invariants (S1–S16)

| # | Invariant | Enforced by | Locked by |
|---|-----------|-------------|-----------|
| S1 | No `TASK_DEAD` is runnable | `task_wakeup` refuses DEAD; reaper zeroes slot | T119 |
| S2 | No `TASK_BLOCKED_*` is in a run queue | block paths dequeue before yield | T119, T121 |
| S3 | No pending-reap slot is reused | `awaiting_reap` skipped by create scans | T119 |
| S4 | A task is in the run queue at most once | `rq_enqueue` `queued[]` guard | T120, T124 |
| S5 | Idle runs only when nothing real is runnable | `task_yield` fallback order | T119, T122 |
| S6 | `live_task_count` returns to baseline after churn | reap accounting | T119, T120, T124 |
| S7 | `live_process_count` returns to baseline after churn | `kprocess_free` on last ref | T119 |
| S8 | `sched_ctx` never stays bound to a dead task | `task_release_sched_ctx` at reap/kill | T123 |
| S9 | `sched_ctx` is never double-released | single release helper + refcount | T123 |
| S10 | `SC_CONFIGURE` rejects invalid budget/period | `kschedctx_configure` | T123 |
| S11 | `THREAD_SET_SC` leaves no stale ref | rebind releases old, rights/type checked | T123 |
| S12 | Yield never loses a runnable task | run-queue integrity | T120, T122 |
| S13 | Endpoint block/unblock never corrupts the run queue | rq under lock | T121 |
| S14 | Endpoint close/cancel leaves no schedulable dead waiter | `kendpoint_obj_close` / cancel | T121 |
| S15 | Kill/exit/reap keeps scheduler state balanced | Fase 16 lifecycle | T119, T121 |
| S16 | Single-core assumptions are written down for SMP | this document | T124 |

---

## Tests T119–T124

All run as ring-3 selftests in `services/iris_test/main.c`, observing the
scheduler only through `SYS_SCHED_INFO`.  In-process worker threads
(`SYS_THREAD_CREATE`) each leave one KTcb handle behind by design (Ph96), so
these tests assert TASK-live and PROCESS-live baselines, never handle-live.

| Test | Scenario | Invariants | Failure paths |
|------|----------|-----------|---------------|
| T119 | Worker threads cycle runnable→blocked(EP_RECV)→woken→dead; interleaved external-kill of a lifecycle_probe child; ×4 rounds | S1,S2,S3,S5,S6,S7,S15 | task/proc-live drift, reap backlog, SC drift, no yield progress |
| T120 | SH_NWORK workers run fixed-length yield loops (max run-queue churn), park in EP_RECV, then released to exit | S4,S6,S12 | lost worker, progress mismatch, rq-hwm implausible, duplicate-enqueue storm |
| T121 | A worker blocked in EP_RECV / EP_SEND / EP_CALL is woken by endpoint close (must be `CLOSED`); a child killed while blocked leaves no dead waiter | S2,S13,S14,S15 | wrong wake error, waiter not woken, dead waiter, ghost KReply |
| T122 | SH_NWORK equal-priority cooperative workers each yield a fixed quota; fairness = all complete | S5,S12 | starved worker, yield accounting |
| T123 | Full SC lifecycle: create/configure/bind/rebind/unbind + worker bind→self-exit→reap; then every handle closed → live-SC back to baseline | S8,S9,S10,S11 | budget/period invalid, wrong type, missing RIGHT_WRITE, SC leak/double-free |
| T124 | SMP-readiness tripwire: reap-queue bound, run-queue depth ≤ TASK_MAX, S4 counter wired, live counts consistent | S4,S6,S16 | reap-queue bound, rq depth, unwired counter |

---

## Limits (what the scheduler does NOT guarantee today)

- **No preemption of a ring-0 spinner** on the QEMU TCG target: no timer IRQ is
  delivered while a task spins in the kernel.  Cooperative `task_yield` (or a
  ring-3 quantum expiry) is required for progress there.
- **No fairness weighting** beyond round-robin within a priority band; a
  higher-priority runnable task always preempts a lower one — there is no aging
  or anti-starvation for a permanently-outranked task.
- **SchedContext budget is the only per-task CPU accounting.**  There is no
  global CPU-time ledger, no bandwidth reservation beyond budget/period, and
  budget is charged in whole ticks.
- **The `wake_tick` timeout scan is O(TASK_MAX) per tick** (documented in
  `scheduler_tick`); acceptable at `TASK_MAX = 256` / 100 Hz, a candidate for a
  timer wheel later.

---

## Single-core assumptions and SMP-readiness

The scheduler is correct for the BSP and structured for SMP (per-CPU run
queues, `home_cpu`, `cpu_local`, IPI hooks in `task_wakeup`) but **runs on one
CPU today**.  A future SMP port MUST revisit:

| Area | Current single-core assumption | Required before SMP |
|------|-------------------------------|---------------------|
| `current_task` | one global + `cpu_local[0].current_task` kept in sync | per-CPU current; no global reads on the hot path |
| Run queue | one wired queue (`cpu_local[0].rq`); `rq_dequeue_best` uses `cpu_self()->rq` | wire every AP's queue; cross-CPU enqueue already locks the *target* queue |
| Duplicate-enqueue guard (S4) | `queued[]` under the (single) run-queue lock | same guard under the *target* CPU's run-queue lock — the counter makes the guard's firing visible |
| Deferred reap | one death per yield interval; `reap_queue` (8) never nears full | per-CPU dead lists drained on each CPU's tick; cross-CPU reap via IPI/work-queue |
| `awaiting_reap` | reaper on the same CPU that ran the dying task | the reaping CPU must own the slot; publish with acquire/release |
| Endpoint wakeups | `task_wakeup` enqueues then IPIs a non-home CPU (hook present, APs down) | validate the IPI path once APs boot |
| Timeout scan | runs under CLI on the IRQ-handling CPU | per-CPU timer wheel to avoid cross-CPU wake IPIs |
| TLB | no cross-CPU shootdown (single address space active) | shootdown IPI on unmap/reap of shared mappings |
| Lock ordering | `ht->lock → ep->lock`; run-queue lock is a leaf | preserve; the reaper must not hold the run-queue lock across `kprocess_free` |

`kernel/core/scheduler/scheduler_priv.h` documents the `cpu_local` / GS_BASE
wiring; `scheduler.c`/`task_lifecycle.c` carry the per-loop SMP notes.  T124 is
the runtime tripwire that the single-core assumptions still hold.

---

## Remaining scheduler work

- Replace the O(TASK_MAX) `wake_tick` scan with a per-CPU timer wheel.
- Bring up APs and exercise the cross-CPU `task_wakeup` IPI + per-CPU reap.
- Optional anti-starvation / priority aging if a fairness policy is adopted.
- Widen SchedContext accounting (sub-tick budget, bandwidth groups) if real-time
  guarantees are pursued.

None of these is required for correctness on the current single-CPU target.
