# Fase 16 — lifecycle / process hardening

Status: ACCEPTED — implemented in this phase.  Companion to
`ipc-stress-invariants.md` (A1.11, which found and fixed the deferred-reap
slot-reuse leak) and `handle-table-freeze.md` (A1.7 counters).  Fase 16
audits the task/process lifecycle end to end and locks its contracts with
runtime tests T113–T118.  No new lifecycle bug was found — the A1.11 fix
(`task.awaiting_reap`) had already closed the only one; this phase makes the
surviving guarantees regression-proof and adds the instrumentation to observe
them.

## Task and process states

A task slot (`struct task`, `TASK_MAX = 256`) is in exactly one of:

```text
TASK_DEAD       free, OR dead-but-not-yet-reaped when awaiting_reap = 1
TASK_READY      runnable, on a run queue
TASK_RUNNING    on-CPU
TASK_BLOCKED_*  RECV / SEND / REPLY / notification / futex / sleep
TASK_SUSPENDED  parked
```

A `KProcess` is *alive* iff `thread_count > 0` (`kprocess_is_alive`).  It is
freed (its object destroyed, `kprocess_live--`) only when **every** reference
drops: the kernel creation ref AND every handle any process holds to it.  So a
parent that holds a child's process handle keeps the child's `KProcess` object
counted-live until the parent closes that handle — even after the child is
dead.  Tests must close child handles before asserting a live-process
baseline; `it_quiesce_reaper()` covers the deferred-reap tail (below).

## Self-exit vs external-kill — the core asymmetry

| | Self-exit (`task_exit_current`) | External kill (`task_kill_external`) |
|---|---|---|
| Trigger | `SYS_EXIT` / `SYS_THREAD_EXIT` on current task | `SYS_PROCESS_KILL` → `task_kill_process` on each non-current task |
| Address-space reap | **deferred** to `reap_dead_task_off_cpu` (task still runs on its own CR3/kstack) | **inline** (target is off-CPU; caller's CR3 differs) |
| `kprocess_free` (creation ref) | deferred (reaper) | inline |
| Slot release | deferred; `awaiting_reap = 1` reserves it until the reaper runs | inline (`task_reset_slot`) |
| `sched_ctx` / `unlink` / live-count | deferred | inline |
| `ktcb` release | inline (in `task_exit_current`) | inline |

The **deferred** side is why A1.11's bug existed: before the fix, a slot that
was `TASK_DEAD` but not yet reaped looked free to the allocators, so an
immediate respawn recycled it and the reaper then skipped the stale entry.
`task.awaiting_reap`, set right before `TASK_DEAD` in `task_exit_current` and
cleared by the reaper's `task_reset_slot`, closes that window; the three
slot-allocation scans skip `awaiting_reap` slots.

## Deferred reap queue

`reap_queue` (size 8, power-of-two ring) holds self-exited tasks awaiting
off-CPU cleanup.  A dying task enqueues itself once when it context-switches
away (`scheduler.c`, `old->state == TASK_DEAD`).  `reap_pending_dead_task`
drains **one** entry per `task_yield` / `scheduler_tick`; if the head is still
`current_task` it re-enqueues (the task hasn't switched off yet).  On
single-CPU only one task dies per yield interval, so the queue never nears 8 —
`reap_queue_hwm` (exposed via SYS_SCHED_INFO) proves this empirically under
the T114/T118 churn.

Consequence for tests: a batch of self-exited children releases their
`KProcess` creation refs only across the next handful of scheduler ticks.
`it_quiesce_reaper()` (200 yields) drains that backlog before a
live-process snapshot so the baseline is stable.

## Cleanup chains

- **Blocked waits** — `task_cancel_blocked_waits`: notification, futex and
  endpoint waiter cancel, plus `pending_kreply` → `kreply_cancel_caller`
  (clears `r->caller`, releases the caller's own KReply ref).
- **Endpoint waiters** — `kendpoint_cancel_waiter`: removes the task from the
  endpoint queue, resets `ep_state` to IDLE when it empties, and releases any
  staged cap **without consuming the source** (A1.10).
- **Reply caps** — a killed caller's KReply survives via the server's handle
  ref; with `r->caller == 0` the server's `SYS_REPLY` returns NOT_FOUND and
  keeps its attached cap (two-phase staging).  `kreply_obj_close` also wakes a
  still-blocked caller with CLOSED if the server drops the reply cap unused.
- **CSpace** — the process root CNode lives in the process's own handle table
  (`cspace_root_h`).  `kprocess_teardown` → `handle_table_close_all` releases
  it → `kcnode_obj_close` releases every slot cap (active + lifecycle).
- **VSpace / mappings** — `kprocess_reap_address_space` → `kvspace_invalidate`
  tears down every mapping and drops bootstrap KFrame refs, then destroys the
  page tables.  Idempotent via `aspace_reaped`.
- **Death notification** — `kprocess_emit_exit_watch` fires each armed watch's
  KNotification signal exactly once; `teardown_complete` guards re-entry.  A
  watch armed on an already-dead process emits immediately.

Both `kprocess_teardown` (`teardown_complete`) and
`kprocess_reap_address_space` (`aspace_reaped`) are idempotent, so the
self-exit (teardown inline, reap deferred) and kill (both inline) orderings
converge without double-free.

## Audit matrix

| Area | Behavior | Cleanup path | Tests | Risk |
|---|---|---|---|---|
| Self-exit (last thread) | teardown inline, aspace+free deferred | `task_exit_current` → reaper | T112, T114, T118 | slot reuse before reap — **closed** by `awaiting_reap` |
| Self-exit (non-last thread) | dec `thread_count`, slot deferred | `task_exit_current` → reaper | T118 (threads) | KTcb handle persists (+1, Ph96) — by design |
| External kill | everything inline | `task_kill_external` | T113, T114-T117 | none observed |
| Kill multi-thread proc | per-task kill; last triggers teardown | `task_kill_process` | (indirect) | current-task skip — SYS_PROCESS_KILL forbids suicide |
| Child respawn | immediate reuse of freed slot | allocator skips `awaiting_reap` | T112, T114 | reuse-before-reap — **closed** |
| Reap-queue pressure | 1 drain / yield, ring of 8 | `reap_pending_dead_task` | T114, T118 (`reap_queue_hwm`) | single-CPU bound holds |
| Watcher notification | one signal per armed watch | `kprocess_emit_exit_watch` | T117 | dup/lost — guarded by `teardown_complete` |
| EP_RECV waiter death | dequeued, ep IDLE | `kendpoint_cancel_waiter` | T101, T115 | dead waiter — none |
| EP_SEND waiter death | dequeued, staged cap released | `kendpoint_cancel_waiter` | T115 | source-cap consume — none (A1.10) |
| EP_CALL waiter death (pre-rendezvous) | dequeued as BLOCKED_SEND | `kendpoint_cancel_waiter` | T115 | ghost KReply — none (minted only at rendezvous) |
| EP_CALL caller death (BLOCKED_REPLY) | KReply caller cleared | `kreply_cancel_caller` | T113 | dangling reply / server cap loss — none |
| CSpace caps on death | root CNode release cascades | `handle_table_close_all` → `kcnode_obj_close` | T116 | cap leak / stale ref — none |
| VMO cap shared to child | released with child handle table | `handle_table_close_all` | T116 | shared-object destroy — none (parent keeps its ref) |
| VMO mapping in child aspace | torn down by kvspace_invalidate | `kprocess_reap_address_space` | every kill/exit test (child stack/text) | `mapped_count` not ring-3 observable — **documented gap** |
| sched live count | inc on create, dec once on death | inline or reaper | T114, T118 (`it_task_live`) | double-dec / zombie-counted — none |
| Thread create/exit | KTcb handle to process; slot reaped | `task_thread_create` / exit | T118 | KTcb +1 persistent — by design (Ph96) |

## Tests T113–T118

| Test | Scenario | Invariants | Failure paths |
|---|---|---|---|
| T113 | caller death mid-EP_CALL with a live reply cap | I5-I10, I15, I16 | reply→NOT_FOUND, 2nd reply with cap→NOT_FOUND (server cap kept), exactly 1 KReply, no dangling waiter |
| T114 | 4 concurrent children, 40 churn rounds (alt exit/kill), immediate respawn | I15-I17 | spawn NO_MEMORY, exit-code drift, task/proc-live drift, reap backlog |
| T115 | child killed while blocked in EP_RECV / EP_SEND / EP_CALL | I6, I14-I16 | dead waiter (NB probe), ghost KReply on send/call |
| T116 | child killed holding CSpace endpoint+notification + shared VMO cap | I1, I15, I16 | shared object destroyed, handle/proc leak |
| T117 | 3 deaths (exit / kill / block-then-kill) watched on one notification | I15 + death-notify exactly-once | missing/dup death bit, wrong STATUS/EXIT_CODE, non-idempotent kill, silent late watch |
| T118 | interleaved proc self-exit / proc kill / thread self-exit churn | I15-I17 | task-live drift, proc-live drift, reap backlog |

(Invariant numbers I1–I18 are defined in `ipc-stress-invariants.md`.)

## Instrumentation

SYS_SCHED_INFO grew 88 → 96 bytes (additive; a legacy 88-byte reader is
unaffected):

- offset 84 `live_process_count` (`kprocess_live_count`) — exact
  A1.11-class leak detector.
- offset 88 `reap_queue_hwm` (`sched_reap_queue_hwm`) — deferred-reap depth
  high-water.

The base-frame `live_task_count` (offset 32) is read via `it_task_live()`.
No new syscall, no layout break.

## Remaining gaps

- **`mapped_count` is not ring-3 observable.**  T116 proves shared objects
  survive a holder's death and the books balance, but cannot directly assert
  the child's page-table mapping refcount hit zero.  A debug-only
  `SYS_SCHED_INFO` mapping-count field, or a KDEBUG VMO-introspection call,
  would close this.
- **Multi-threaded process kill** is exercised only indirectly; a test with a
  child that spawns its own threads and is then killed mid-flight would lock
  the per-task teardown ordering explicitly.
- **Reap-queue saturation** cannot be provoked on single-CPU (one death per
  yield).  Under SMP the queue, the `awaiting_reap` handshake and the
  `reap_dead_task_off_cpu` off-CPU assumption all need revisiting — the reap
  queue already carries an explicit SMP TODO.
- **Thread external-kill**: there is no userland `SYS_THREAD_KILL`; a
  non-current thread only dies with its process.  Caller-death mid-call is
  therefore tested at process granularity (T113), not thread granularity.
- **KReply transfer** (RIGHT_TRANSFER on a reply handle, handing the reply
  cap to a third party) is outside the lifecycle tests.

## How to extend without flaking

1. Close every child process handle **before** asserting a live-process
   baseline — a parent handle keeps a dead child's `KProcess` counted-live.
2. Call `it_quiesce_reaper()` before both the baseline and the final snapshot
   when the test self-exits children; deferred reaps land on later ticks.
3. Prefer exact counters that return to baseline — live tasks, live
   processes, handle live, reply caps — over `pmm_free_pages` (moved by lazy
   allocation and background services; use it only for manual leak hunts).
4. External kill reaps inline; self-exit reaps deferred.  A test that mixes
   them must account for the timing difference, not assume symmetry.
5. Each helper thread leaves a documented +1 KTcb handle (Ph96); assert
   task-live, not handle-live, when a test creates threads.
