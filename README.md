# IRIS OS

IRIS is a custom x86_64 operating system built from scratch with UEFI boot, ring 0 / ring 3 separation, a syscall boundary, capability-style kernel objects, and an active transition toward a more serious microkernel-like architecture.

Today it is no longer just a kernel skeleton. It boots, runs user processes, moves handles over IPC, boots early userland services, and exercises real service-managed paths such as keyboard I/O and a migrated subset of VFS.

---

## Current Status

IRIS is currently a hybrid system:

- the low-level kernel base is already established
- key control-plane responsibilities have moved into userland
- some major subsystems still retain kernel-resident backend pieces

What is already real:

- ring 3 user tasks
- per-process address spaces
- `KProcess`-owned handle tables
- `KChannel` IPC
- attached-handle transfer over IPC
- `KNotification` and `KVmo`
- a userland service manager (`svcmgr`) implemented in C
- explicit bootstrap-handle delivery for the first connection to `svcmgr`
- userland-authoritative lookup for migrated live services
- a userland keyboard service
- a userland VFS service with real ownership of the migrated `OPEN/READ/CLOSE` path
- a userland VFS namespace with enumerable boot exports
- explicit shared protocols for the live `svcmgr`, `vfs`, and `kbd` service paths
- compact subsystem-owned status summaries for `svcmgr`, `vfs`, and `kbd`
- a compact kernel diagnostics snapshot via `SYS_DIAG_SNAPSHOT`
- one consolidated diagnostics flow through `svcmgr` that gathers kernel + `svcmgr` + `vfs` + `kbd` health
- IRQ routing with process-scoped ownership and automatic cleanup
- dead-client reclaim for migrated VFS entries

What remains transitional:

- retired legacy VFS syscall numbers still exist as compatibility stubs that return failure

---

## Architecture Snapshot

- Target: x86_64
- Boot: UEFI (`BOOTX64.EFI`)
- Execution model: kernel in ring 0, user processes in ring 3
- Memory model: 4-level paging with per-process user mappings
- Scheduling: timer-driven round-robin with blocking for IPC, notifications, and sleep
- IPC: `KChannel` objects accessed through process handle tables
- Handle transfer: attached handles move across IPC with explicit rights capping
- Process model: `KProcess` owns address space, heap break, and handle table
- Task model: `struct task` owns CPU context, kernel stack, and scheduler state

Current authority split:

- bootstrap discovery of `svcmgr`: explicit bootstrap handle from kernel
- bundled service spawn authority: explicit bootstrap capability delivered to `svcmgr`
- live service discovery: `svcmgr`
- autostart service recovery: `svcmgr` with declarative restart limits
- keyboard service policy/lifecycle: `svcmgr` + `kbd`
- migrated VFS file/session state: `vfs` service
- default migrated VFS content: service-owned
- legacy kernel VFS backend: retired from the default kernel image
- service/runtime status ownership: subsystem-local `STATUS` replies from `svcmgr`, `vfs`, and `kbd`
- global diagnostics entry point: `svcmgr` aggregation over `SYS_DIAG_SNAPSHOT` + subsystem `STATUS`

---

## Implemented Pieces

| Area | Status | Notes |
|------|--------|-------|
| UEFI boot | Working | Loads the ELF kernel and passes boot info |
| PMM + paging | Working | Per-process user mappings with kernel/user split |
| GDT/TSS/IDT | Working | Ring transitions, exceptions, IRQ dispatch |
| Scheduler | Working | Round-robin with explicit blocking/wakeup |
| Capability core | Working | `KObject`, `KChannel`, `KProcess`, `KNotification`, `KVmo`, handle table |
| Syscalls | Working | Process, memory, IPC, handle, IRQ-route, and retired compatibility syscall numbers |
| Service manager | Working | `svcmgr` runs in ring 3, owns live lookup, and can restart declarative autostart services with bounded retry policy |
| Bootstrap authority | Working | `svcmgr` receives an explicit spawn capability; IRQ route install is authorized by target `proc_handle` rights |
| Keyboard path | Working | `kbd` + `kbd.reply` booted by `svcmgr` |
| VFS migrated path | Working | `vfs` owns client-visible `file_id`, offsets, stale-id rejection, dead-client reclaim, and a small enumerable boot namespace |
| Service protocols | Working | `svcmgr`, `vfs`, and `kbd` use shared protocol headers with explicit message layouts and protocol versions |
| Diagnostics | Working | `SYS_DIAG_SNAPSHOT` plus `svcmgr` aggregation expose compact global health without turning boot into a dump |
| IRQ ownership | Working | Route ownership follows the service `KProcess`; cleanup happens on exit |
---

## Boot And Discovery Model

Healthy boot now works like this:

1. The kernel boots core subsystems and spawns `svcmgr`.
2. The kernel delivers a bootstrap spawn capability to `svcmgr` over its private bootstrap channel.
3. The first control-plane connection to `svcmgr` is delivered explicitly as a bootstrap handle.
4. `svcmgr` boots `kbd` and `vfs`, retains their master public handles, owns lookup policy, and applies declarative restart policy if they exit.
5. `user_init` reaches `svcmgr` through the bootstrap handle, then looks up `kbd`/`vfs` over IPC.
6. Clients receive service handles through IPC handle transfer, not through normal kernel lookup.

This means healthy boot no longer depends on any kernel nameserver path.

---

## VFS Status

The VFS transition is real but partial.

What has moved into userland:

- client-visible `OPEN/READ/CLOSE` for the migrated path
- enumerable exported namespace via `VFS_MSG_LIST`
- service-owned `file_id` namespace
- per-open offset/state
- stale-id invalidation
- dead-client reclaim keyed to owner process death
- default boot export content for the migrated path

What remains in the kernel:

- retired syscall numbers for legacy `SYS_OPEN` / `SYS_READ` / `SYS_CLOSE`

So the healthy-path client-visible VFS authority is no longer split with the kernel. Remaining work toward a purer microkernel is now in other areas such as naming policy and hardware-driver extraction.

---

## Runtime Shape

Default boot keeps a small healthy smoke path:

- kernel boot progression
- `svcmgr` startup
- explicit bootstrap-handle handoff to `user_init`
- `kbd` lookup and reply path
- one compact global diagnostics query through `svcmgr`
- migrated `vfs` lookup and `OPEN/READ/CLOSE` path

Heavier proof probes are available, but not enabled by default.

Enable them with:

```bash
make clean
make ENABLE_RUNTIME_SELFTESTS=1
make check
make run
```

That selftest mode re-enables heavier runtime validation such as:

- phase-3 supervision probe
- VFS stale-id proof
- VFS abnormal-client-death reclaim proof
- extra spawn/handle/VMO smoke checks

---

## Build And Run

Default path:

```bash
make clean
make
make check
make run
```

`make run` launches IRIS in QEMU with OVMF.

---

## Important Syscalls

The current syscall surface lives in [kernel/include/iris/syscall.h](/home/silver/projects/IRIS/kernel/include/iris/syscall.h). The most important live interfaces are:

- `SYS_CHAN_CREATE`, `SYS_CHAN_SEND`, `SYS_CHAN_RECV`
- `SYS_HANDLE_CLOSE`, `SYS_HANDLE_DUP`, `SYS_HANDLE_TRANSFER`
- `SYS_SPAWN`
- `SYS_PROCESS_WATCH`
- `SYS_NOTIFY_CREATE`, `SYS_NOTIFY_SIGNAL`, `SYS_NOTIFY_WAIT`
- `SYS_VMO_CREATE`, `SYS_VMO_MAP`
- `SYS_PROCESS_STATUS`, `SYS_PROCESS_SELF`
- `SYS_IRQ_ROUTE_REGISTER`
- `SYS_DIAG_SNAPSHOT`
- `SYS_SPAWN_SERVICE`

Retired compatibility syscall numbers:

- `SYS_NS_REGISTER`, `SYS_NS_LOOKUP`
- `SYS_OPEN`, `SYS_READ`, `SYS_CLOSE`

Current lifecycle note:

- healthy-path `svcmgr` supervision is no longer pure polling; service exit is watched via `SYS_PROCESS_WATCH`
- `SYS_PROCESS_STATUS` still exists as a compatibility query, but the healthy service lifecycle path now uses `SYS_PROCESS_WATCH`
- `SYS_DIAG_SNAPSHOT` now exposes bounded kernel pool pressure for channels, notifications, and VMOs
- `SYS_SPAWN_SERVICE` now requires an explicit bootstrap capability handle, not a special process identity
- `SYS_IRQ_ROUTE_REGISTER` is authorized by the target `proc_handle` carrying `RIGHT_ROUTE`

---

## Protocols And Diagnostics

The current live protocols are intentionally small, but they are no longer ad hoc byte soup.

`svcmgr` protocol:

- versioned by `SVCMGR_PROTO_VERSION`
- shared definition in [kernel/include/iris/svcmgr_proto.h](/home/silver/projects/IRIS/kernel/include/iris/svcmgr_proto.h)
- covers bootstrap-handle delivery, service lookup, process-exit watch events, `STATUS`, and consolidated `DIAG`
- runtime policy comes from a shared declarative service catalog rather than a private hardcoded manifest inside `svcmgr`
- `STATUS` reports:
  - catalog entry count
  - ready service count
  - active supervision slot count
  - service catalog version
- `DIAG` reports one compact global summary containing:
  - kernel snapshot counts from `SYS_DIAG_SNAPSHOT`
  - `svcmgr` supervision summary
  - `vfs` export/open summary
  - `kbd` status flags

`vfs` protocol:

- versioned by `VFS_PROTO_VERSION`
- shared definition in [kernel/include/iris/vfs_proto.h](/home/silver/projects/IRIS/kernel/include/iris/vfs_proto.h)
- covers `OPEN`, `READ`, `CLOSE`, `LIST`, reclaim probe, and `STATUS`
- `STATUS` reports:
  - ready export count
  - active open-file count
  - open-file capacity
  - exported byte total

`kbd` protocol:

- versioned by `KBD_PROTO_VERSION`
- shared definition in [kernel/include/iris/kbd_proto.h](/home/silver/projects/IRIS/kernel/include/iris/kbd_proto.h)
- covers `HELLO`, `STATUS`, and routed IRQ scancode delivery
- `STATUS` reports keyboard service flags such as `KBD_STATUS_READY` and `KBD_STATUS_PS2_OK`

Kernel diagnostics snapshot:

- shared definition in [kernel/include/iris/diag.h](/home/silver/projects/IRIS/kernel/include/iris/diag.h)
- exposed through `SYS_DIAG_SNAPSHOT`
- reports compact kernel-owned counts:
  - live scheduler tasks
  - live `KProcess` slots
  - live `KChannel` slots
  - live `KNotification` slots
  - live `KVmo` slots
  - active IRQ routes
  - scheduler ticks

Current bounded capacities in the healthy build:

- `TASK_MAX = 32`
- `KPROCESS_POOL_SIZE = 32`
- `KCHANNEL_POOL_SIZE = 64`
- `KCHAN_CAPACITY = 32`
- `KNOTIF_POOL_SIZE = 64`
- `KVMO_POOL_SIZE = 32`
- `VFS_SERVICE_OPEN_FILES = 32`
- `KChannel` recv now supports a bounded multi-waiter set instead of rejecting the second waiter with `IRIS_ERR_BUSY`

Together these surfaces give one clean answer to “how do I ask IRIS for global health?”:

- ask `svcmgr` for `DIAG` if you want the compact global view
- use subsystem-local `STATUS` only when you need the owner-specific view directly

---

## Repository Map

```text
boot/uefi/boot.c                     UEFI loader
kernel/kernel_main.c                 Kernel bootstrap and subsystem bring-up
kernel/core/syscall/syscall.c        Syscall dispatcher and implementations
kernel/core/scheduler/scheduler.c    Scheduler and task lifecycle
kernel/core/svcmgr.c                 Ring-3 service manager logic
kernel/core/vfs_service.c            Ring-3 VFS service logic
kernel/core/init/svcmgr_bootstrap.c  Kernel-side svcmgr bootstrap
kernel/core/irq/irq_routing.c        IRQ routing with owner-based cleanup
kernel/arch/x86_64/user_init.S       Default init + gated selftest entrypoints
kernel/arch/x86_64/kbd_server.S      Keyboard service
kernel/arch/x86_64/svcmgr.S          Tiny svcmgr entry shim
kernel/arch/x86_64/vfs_server.S      Tiny vfs entry shim
kernel/include/iris/diag.h           Kernel diagnostics snapshot contract
kernel/include/iris/kbd_proto.h      kbd protocol
kernel/include/iris/svcmgr_proto.h   svcmgr protocol
kernel/include/iris/vfs_proto.h      vfs protocol
kernel/new_core/src/                 Capability-object implementation
kernel/fs/ramfs/                     Current in-kernel storage backend
```

---

## What Is Verified In Runtime

On the current tree, normal boot verifies:

- Stage 13 kernel boot
- `svcmgr` spawn and readiness
- explicit `svcmgr` bootstrap handle handoff
- `kbd` request/reply success
- one consolidated diagnostics query covering kernel + `svcmgr` + `vfs` + `kbd`
- migrated `vfs` open/read/close success

With `ENABLE_RUNTIME_SELFTESTS=1`, the tree also verifies:

- heavier `svcmgr` supervision path
- VFS stale-id rejection
- VFS dead-client reclaim and post-reclaim invalidation

---

## Short-Term Debt

The biggest remaining architectural debt is:

1. retire more of the transitional kernel bootstrap nameserver surface
2. continue extracting VFS authority beyond the current migrated subset
3. make the consolidated diagnostics consumer less tightly coupled to exact current runtime values
4. reduce polling-based lifecycle on paths that still have not moved to watch/event-style cleanup
5. improve service coverage and reduce remaining compatibility-only syscall surfaces

Non-goals for the immediate next step:

- no large scheduler rewrite
- no SMP bring-up in this phase
- no broad one-shot filesystem rewrite
- no unrelated subsystem churn

---

## Branches

| Branch | Purpose |
|--------|---------|
| `silver` | Active development |
| `staging` | Integration and verification |
| `main` | Promoted stable line |
| `collab` | Collaboration branch |
