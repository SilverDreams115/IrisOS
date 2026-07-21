# IRIS — MCS Scheduling Model (Fase S2)

Canonical seL4-MCS-style SchedulingContext (SC): budget/period as a kernel
object retyped from Untyped, bound one-to-one to a TCB.

## SchedulingContext

- Storage: **exclusively Untyped** (`SYS_UNTYPED_RETYPE2`,
  `KOBJ_SCHED_CONTEXT`). `SYS_SC_CREATE` (83) and the kslab `kschedctx_alloc`
  are RETIRED (ledger).
- Initial state (B2): a freshly retyped SC is born **unconfigured and
  unbound** — `budget=period=remaining=0`, `configured=0`, `bound_task=NULL`.
  It cannot drive a thread until it is configured and bound.
- Fields: budget/period/remaining, `bound_task` (the inverse one-to-one
  binding), `configured`, the KObject header, a lock. No kslab sidecar.

## SC_CONFIGURE (84)

`SYS_SC_CONFIGURE(sc_cptr, budget, period)`: validates `budget>0`, `period>0`,
`budget<=period`; resets `remaining=budget`; marks `configured=1`. Atomic
under the SC's lock (S2.8). Requires RIGHT_WRITE.

## SC_BIND (113)

`SYS_SC_BIND(sc_cptr, tcb_cptr)`: explicitly binds an SC to a TCB, both by
CPtr, both live, one-to-one (S2.9). Contract:

- the SC must be configured;
- the TCB may not already have another SC (`IRIS_ERR_BUSY`);
- the SC may not be bound to another task (`IRIS_ERR_BUSY`);
- `tcb_cptr == 0` unbinds the SC from its current task;
- requires RIGHT_WRITE on both.

`SYS_THREAD_SET_SC(sc_cptr)` still exists as a **self-bind** (the calling
thread binds itself) and now also honors the one-to-one rule.

## Binding lifecycle

- explicit unbind: `SC_BIND(sc, 0)` or `THREAD_SET_SC(0)`.
- TCB death: `task_release_sched_ctx` unbinds (`kschedctx_unbind`) before
  dropping the ref → the SC keeps no stale `bound_task` (S2.11).
- SC death / last cap: destruction returns the payload to the Untyped region;
  the TCB drops its ref in its own teardown.
- budget exhaustion: unchanged from Ph75 (the tick decrements `remaining`;
  exhausted → `TASK_BUDGET_EXHAUSTED` until the refill).
- rebind: allowed after an unbind (T267).

## Instrumentation (SYS_UNTYPED_QUERY kind 4)

`sc_live / sc_hwm / sc_retyped / sc_destroyed` (+ the TCB equivalents and the
CDT counters). Diagnostics, never authority.

## Task-construction path (S2 target)

```
Untyped ── RETYPE2 → SC cap
SC_CONFIGURE(sc, budget, period)
… (TCB retype + TCB_CONFIGURE — increment 2)
SC_BIND(sc, tcb)
TCB_RESUME(tcb)
```

Increment 1 (this delivery) closes the SchedulingContext axis. The TCB axis
(retype + configure + resume from user space) and the spawner-supplied root
CNode remain for increment 2; see `sel4-convergence-ledger.md`.
