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
- IRQ routing with process-scoped ownership and automatic cleanup
- dead-client reclaim for migrated VFS entries

What remains transitional:

- the kernel bootstrap nameserver still exists as a narrow compatibility surface
- kernel ramfs still provides backend storage for the current VFS path
- legacy VFS syscalls still exist as backend/compatibility support

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
- live service discovery: `svcmgr`
- keyboard service policy/lifecycle: `svcmgr` + `kbd_server`
- migrated VFS file/session state: `vfs` service
- VFS backend storage: kernel ramfs for now

---

## Implemented Pieces

| Area | Status | Notes |
|------|--------|-------|
| UEFI boot | Working | Loads the ELF kernel and passes boot info |
| PMM + paging | Working | Per-process user mappings with kernel/user split |
| GDT/TSS/IDT | Working | Ring transitions, exceptions, IRQ dispatch |
| Scheduler | Working | Round-robin with explicit blocking/wakeup |
| Capability core | Working | `KObject`, `KChannel`, `KProcess`, `KNotification`, `KVmo`, handle table |
| Syscalls | Working | Process, memory, IPC, handle, IRQ-route, and transitional bootstrap lookup surfaces |
| Service manager | Working | `svcmgr` runs in ring 3 and owns live lookup for migrated services |
| Keyboard path | Working | `kbd` + `kbd.reply` booted by `svcmgr` |
| VFS migrated path | Working | `vfs` owns client-visible `file_id`, offsets, close semantics, stale-id rejection, and dead-client reclaim for the accepted subset |
| IRQ ownership | Working | Route ownership follows the service `KProcess`; cleanup happens on exit |
| Bootstrap nameserver | Transitional | Still present, but no longer the normal path for bootstrapping `svcmgr` |

---

## Boot And Discovery Model

Healthy boot now works like this:

1. The kernel boots core subsystems and spawns `svcmgr`.
2. The first control-plane connection to `svcmgr` is delivered explicitly as a bootstrap handle.
3. `svcmgr` boots `kbd` and `vfs`, retains their master public handles, and owns lookup policy.
4. `user_init` reaches `svcmgr` through the bootstrap handle, then looks up `kbd`/`vfs` over IPC.
5. Clients receive service handles through IPC handle transfer, not through normal kernel lookup.

This means `SYS_NS_LOOKUP("svcmgr")` is no longer the healthy bootstrap path.

---

## VFS Status

The VFS transition is real but partial.

What has moved into userland:

- client-visible `OPEN/READ/CLOSE` for the migrated path
- service-owned `file_id` namespace
- per-open offset/state
- stale-id invalidation
- dead-client reclaim keyed to owner process death

What remains in the kernel:

- ramfs namespace and storage backend
- legacy `SYS_OPEN` / `SYS_READ` / `SYS_CLOSE` compatibility/backend surface

So the kernel is no longer the sole authority for the migrated client-visible path, but VFS extraction is not complete yet.

---

## Runtime Shape

Default boot keeps a small healthy smoke path:

- kernel boot progression
- `svcmgr` startup
- explicit bootstrap-handle handoff to `user_init`
- `kbd` lookup and reply path
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
- `SYS_NOTIFY_CREATE`, `SYS_NOTIFY_SIGNAL`, `SYS_NOTIFY_WAIT`
- `SYS_VMO_CREATE`, `SYS_VMO_MAP`
- `SYS_PROCESS_STATUS`, `SYS_PROCESS_SELF`
- `SYS_IRQ_ROUTE_REGISTER`
- `SYS_NS_REGISTER`, `SYS_NS_LOOKUP`

Current status of the nameserver syscalls:

- `SYS_NS_REGISTER` is restricted to `svcmgr`
- `SYS_NS_LOOKUP` is now transitional bootstrap compatibility, not the intended steady-state discovery path

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
kernel/core/nameserver/nameserver.c  Transitional bootstrap nameserver
kernel/arch/x86_64/user_init.S       Default init + small smoke path
kernel/arch/x86_64/kbd_server.S      Keyboard service
kernel/arch/x86_64/svcmgr.S          Tiny svcmgr entry shim
kernel/arch/x86_64/vfs_server.S      Tiny vfs entry shim
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
3. improve the bootstrap root-capability structure beyond the current `arg0` contract
4. migrate additional compiled-in services into the same userland service model

Non-goals for the immediate next step:

- no large scheduler rewrite
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
