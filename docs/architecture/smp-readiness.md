# IRIS — SMP Readiness & Scheduler Indirection (Fase S2)

State of the scheduler's indirection away from the static task pool and its
implications for SMP and for moving `struct task` to Untyped.

## Current scheduler model

- `struct task tasks[TASK_MAX]` (TASK_MAX=256): a static pool that TODAY backs
  the real state of each runtime TCB (registers, kstack ptr, scheduler
  linkage, blocking state). It is neither kslab nor a dynamic allocator, but it
  IS the execution object's storage → classified ACTIVE_LEGACY / REMOVE in the
  ledger.
- Per-CPU run queue (`CpuRunQueue`): O(1), per-priority FIFO lists represented
  with **indices** (`next[TASK_MAX]`, `queued[TASK_MAX]`, `head/tail[256]`),
  and `rq_*` derives the index with `(t - tasks)`.
- `current_task`, the reap queue, the wait queues (EP/notif/reply/fault): all
  pointers to `struct task` — already pointer indirection, array-agnostic.

## Fase S2 increment 2 — indirection achieved

1. **`task_rsp[TASK_MAX]` REMOVED** (inc.2 step 1). The kernel RSP lived in an
   index-keyed parallel array; it now lives in `struct task.saved_krsp`. The
   context switch no longer derives `old_idx = old - tasks` or indexes
   `task_rsp[]`.
2. **Run queue 100% pointer-based** (inc.2B Block A). `CpuRunQueue.head/tail`
   move from indices to `struct task *`; the parallel arrays
   `next[TASK_MAX]`/`queued[TASK_MAX]` are retired and their data live in the
   TCB (`t->rq_next`, `t->rq_queued`). `rq_enqueue/remove/dequeue` no longer
   use `(t - tasks)` or `&tasks[idx]`: they operate on TCB pointers. No
   run-queue identity derives from an array position (I2B.1 closed).
3. **Canonical SC binding** (`SYS_SC_BIND`) and `SYS_THREAD_SET_SC` frozen.

After Block A, the only uses of `tasks[]` are: (a) the per-tick timeout scans
(`scheduler_tick`, `sched_handle_idle`) — iteration over the backing, not
identity; (b) the free-slot search in task allocation; (c) `idle = tasks[0]`.
None is run-queue identity.

## What blocks moving `struct task` to Untyped

It is no longer the run queue. It is the **storage source**: moving
`struct task`/KTCB to a non-contiguous Untyped region requires (Blocks C/D/H):

1. Turning `tasks[TASK_MAX]` into a **pointer + generation registry**
   (`{KTcb *tcb; uint32_t generation; bool occupied}`), with no payload;
   converting the timeout scans to registry iteration.
2. Sourcing the TCB storage from **Untyped**: trivial for the canonical path
   (user space retypes the TCB), but the legacy productive path (svc_loader →
   THREAD_START creates the TCB in the kernel) has no Untyped at hand → it
   requires migrating that path to user-space construction + TCB_CONFIGURE +
   a kernel-stack decision. Boot-critical, staged in phases with a boot test
   at each one.
3. Idle task: a static bootstrap exception, outside Retype (ledger).

## SMP

`saved_krsp` in the TCB (instead of a global array) is also pro-SMP: each CPU
saves/restores from its own TCB with no contention on a shared array. The rest
of the SMP contract (per-CPU run queues, wakeup IPIs) is the earlier one.
