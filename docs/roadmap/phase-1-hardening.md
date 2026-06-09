# Phase 1 Hardening Roadmap

## Purpose

This document turns the production-hardening discussion into an executable backlog for the current IRIS tree.

Phase 1 does not try to add networking, persistent storage, SMP, or a new userland model.
Its job is narrower:

- stabilize the current syscall and service contracts
- reduce silent corruption risk
- make lifecycle and IPC failure paths auditable
- improve diagnostics so regressions become easier to prove

If Phase 1 is done well, later phases can add features without building on ambiguous kernel behavior.

## Scope

In scope:

- syscall ABI hygiene
- service protocol contract hygiene
- `usercopy` and pointer/range validation
- lifecycle teardown correctness
- IPC hardening
- quota and limit visibility
- better diagnostics and validation coverage

Out of scope:

- new product features
- networking
- persistent storage
- SMP
- scheduler redesign
- wholesale capability model rewrite

## Success Criteria

Phase 1 is complete when all of the following are true:

- live syscall semantics are documented and consistent with code
- invalid user pointers and invalid ranges fail predictably
- task/process teardown does not leak obvious kernel objects or waiters
- IPC close/timeout/transfer semantics are explicit and regression-tested
- current limits are diagnosable rather than surprising
- the runtime validation lanes assert the critical healthy-path and hardening markers

## Work Order

Execute the backlog in this order:

1. ABI and contract freeze
2. `usercopy` and syscall argument hardening
3. lifecycle and teardown audit
4. IPC hardening
5. diagnostics and quota visibility
6. build/test hardening

That order matters. It avoids changing behavior before the current contract is explicit.

## Slice 1: ABI And Contract Freeze

### Goal

Make the live contract explicit before hardening internals.

### Primary files

- `kernel/include/iris/syscall.h`
- `kernel/core/syscall/syscall.c`
- `kernel/include/iris/svcmgr_proto.h`
- `kernel/include/iris/vfs_proto.h`
- `kernel/include/iris/kbd_proto.h`
- `kernel/include/iris/console_proto.h`
- `docs/contracts/*.md`

### Tasks

- classify every syscall as:
  - stable/live
  - transitional but supported
  - retired and intentionally `IRIS_ERR_NOT_SUPPORTED`
- normalize comments so return semantics match actual implementation
- document the canonical error model:
  - success is non-negative
  - failure is negative `iris_error_t`
- document service message ownership rules:
  - who owns attached handles
  - when a handle is consumed
  - required rights on send and receive paths

### Deliverables

- new ABI inventory doc or expanded contract docs
- removal of stale comments that still describe retired spawn or kernel-VFS behavior

### Validation

- `make`
- `make check`
- manual audit of the syscall table versus docs

## Slice 2: `usercopy` And Syscall Argument Hardening

### Goal

Eliminate ambiguous pointer validation and range handling on user-controlled syscall inputs.

### Primary files

- `kernel/core/usercopy.c`
- `kernel/include/iris/usercopy.h`
- `kernel/core/syscall/syscall.c`
- `kernel/arch/x86_64/paging.c`

### Tasks

- audit all syscall entry points that consume user pointers
- centralize range validation rules where possible
- verify overflow checks for:
  - `virt + size`
  - page rounding
  - user buffer length fields
  - attached message payload bounds
- ensure the same class of invalid pointer failure returns the same error where practical
- identify syscalls that still mix structural validation and copy failure handling in inconsistent ways

### High-risk areas

- channel message send/recv
- diagnostic snapshot write-back
- VMO map/unmap/map-into parameters
- thread start / boot argument setup
- wait-any arrays and timeout paths

### Deliverables

- tightened helpers in `usercopy`
- explicit syscall-side comments where helper use is non-obvious
- at least one selftest or runtime assertion for invalid pointer handling

### Validation

- `make`
- `make smoke-runtime`
- selftest lane if any new marker is added:
  - `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`

## Slice 3: Lifecycle And Teardown Audit

### Goal

Make task/process death, reaping, and cleanup predictable under normal exit, fault, and supervisor-driven kill.

### Primary files

- `kernel/core/scheduler/task_lifecycle.c`
- `kernel/core/scheduler/scheduler.c`
- `kernel/core/futex/futex.c`
- `kernel/new_core/src/kprocess.c`
- `kernel/new_core/src/kchannel.c`
- `kernel/new_core/src/knotification.c`
- `kernel/core/irq/irq_routing.c`

### Tasks

- audit the relationship between:
  - task death
  - process thread counts
  - process address-space reap
  - handle-table close-all
  - waiter cancellation
- verify that blocked tasks are removed from all relevant wait structures on kill/reap
- verify process-exit watches and exception channel cleanup under:
  - normal exit
  - fault
  - supervisor kill
  - service restart
- verify IRQ route cleanup remains process-scoped and leak-free

### Deliverables

- documented lifecycle invariants near the code that enforces them
- additional selftests for process exit and watcher cleanup if gaps are found

### Validation

- `make`
- `make smoke-runtime`
- `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`

## Slice 4: IPC Hardening

### Goal

Make `KChannel` semantics explicit and regression-resistant under close, seal, timeout, wakeup, and attached-handle transfer.

### Primary files

- `kernel/new_core/src/kchannel.c`
- `kernel/new_core/include/iris/nc/kchannel.h`
- `kernel/core/syscall/syscall.c`
- `kernel/core/phase3_selftest.c`

### Tasks

- audit send/recv behavior when:
  - the channel is empty
  - the channel is sealed
  - the channel is closed with buffered messages
  - a waiter times out
  - an attached handle install partially fails
- verify wake-one and wake-all paths do not leave stale waiters behind
- verify rights reduction and transfer semantics on attached handles match docs
- decide whether any current `IRIS_ERR_*` result is underspecified and document it

### Deliverables

- stronger comments in `kchannel.c`
- targeted selftests for:
  - seal semantics
  - timeout semantics
  - attached-handle reduction
  - stale waiter cleanup

### Validation

- `make`
- `make smoke-runtime`
- selftest lane must remain green

## Slice 5: Diagnostics And Quota Visibility

### Goal

Expose enough structured state to explain resource pressure and failure causes without reading arbitrary logs.

### Primary files

- `kernel/include/iris/diag.h`
- `kernel/core/syscall/syscall.c`
- `services/svcmgr/svcmgr.c`
- `services/vfs/vfs.c`
- `docs/contracts/diag.md`

### Tasks

- extend diagnostics only where the signal is high
- expose current pressure where useful:
  - live task count
  - process/object usage
  - channel pressure
  - quota exhaustion counts or last-failure markers if justified
- standardize boot and health markers that CI depends on
- ensure supervisor-side diagnostics differentiate:
  - service not found
  - bootstrap failure
  - restart exhaustion
  - dependency not ready

### Deliverables

- updated `diag` contract
- fewer ambiguous `WARN` logs in critical supervisor paths

### Validation

- `make smoke-runtime`
- `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`

## Slice 6: Build And Test Hardening

### Goal

Reduce false negatives and make validation reproducible.

### Primary files

- `Makefile`
- `scripts/run_qemu_headless.sh`
- `scripts/smoke_local.sh`
- `.github/workflows/ci.yml`
- `docs/testing.md`

### Tasks

- document and, if practical, reduce the shared-`build/` race surface
- separate validation intent more clearly:
  - compile validation
  - runtime smoke
  - selftest runtime
- add small host-side checks for pure helpers where QEMU is unnecessary
- keep all runtime markers machine-checkable

### Deliverables

- clearer validation matrix in docs and CI
- fewer accidental failures from concurrent local invocations

### Validation

- full local baseline:
  - `make`
  - `make check`
  - `make smoke`
  - `make smoke-runtime`
  - `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`

## Recommended Execution Units

Do not implement Phase 1 as one giant branch.

Use small sequential units:

1. ABI docs and comment cleanup only
2. `usercopy` helpers plus 1-2 syscall families
3. teardown invariants plus selftests
4. channel hardening plus selftests
5. diagnostics expansion
6. Make/CI/test improvements

Each unit should preserve a bootable system and keep the headless lanes green.

## Immediate First Task

Start with Slice 1 and the smallest useful artifact:

- create the live syscall and protocol inventory
- reconcile documentation with the current `0..64` syscall surface
- mark every retired path explicitly
- identify the top 5 syscall families with the highest hardening risk

That gives the next implementation step a stable contract to harden against.
