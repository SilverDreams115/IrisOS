# IRIS OS

IRIS is a custom x86_64 operating system built from scratch with UEFI boot, ring 0 / ring 3 separation, a capability-based kernel object model, and a microkernel architecture in active progress.

As of Phase 21, IRIS is a pure microkernel in the meaningful sense: **zero userland code is compiled into the kernel binary**. Every user process — init, svcmgr, kbd, vfs — is a standalone ELF service loaded from an embedded initrd at boot.

---

## Current Status

Phase 21 complete. The system boots cleanly and exits with code 0.

What is real today:

- ring 3 user tasks with per-process address spaces
- capability-based handle tables per process (`KProcess`)
- `KChannel` IPC with attached-handle transfer and explicit rights capping
- `KNotification`, `KVmo`, `KIrqCap`, `KIoPort` kernel objects
- a fully userland service manager (`svcmgr`) written in C, running as ring-3 ELF
- explicit bootstrap-handle delivery to first clients of `svcmgr`
- userland-authoritative service lookup via IPC handle transfer
- a userland keyboard service (`kbd`) owning PS/2 I/O via a `KIoPort` capability
- a userland VFS service (`vfs`) owning the full `OPEN/READ/CLOSE` path, session state, dead-client reclaim, and an enumerable boot namespace
- shared protocol headers for `svcmgr`, `vfs`, and `kbd`
- compact subsystem-owned status summaries and a single global diagnostics aggregation point via `svcmgr`
- IRQ routing with process-scoped ownership and automatic cleanup on exit
- header-change dependency tracking in the build system (`-MMD -MP`)
- `init` spawned as an ELF service from the initrd — the last piece of userland code removed from the kernel binary

---

## Architecture Snapshot

- **Target**: x86_64
- **Boot**: UEFI (`BOOTX64.EFI`) via a custom UEFI loader
- **Execution model**: kernel in ring 0, all user processes in ring 3
- **Memory model**: 4-level paging, per-process user mappings, higher-half kernel
- **Scheduling**: timer-driven round-robin; explicit blocking for IPC, notifications, IRQ, and sleep
- **IPC**: `KChannel` objects accessed through per-process handle tables
- **Handle transfer**: attached handles move across IPC with explicit rights capping
- **Process model**: `KProcess` owns address space and handle table
- **Task model**: `struct task` owns CPU context, kernel stack, and scheduler state
- **Capability objects**: `KObject`, `KChannel`, `KVmo`, `KNotification`, `KIrqCap`, `KIoPort`, `KProcess`, `KBootstrapCap`

Authority split today:

| Responsibility | Owner |
|---|---|
| Bootstrap discovery of `svcmgr` | Kernel (explicit bootstrap handle) |
| Service spawn authority | `KBootstrapCap` delivered to `svcmgr` |
| Live service discovery | `svcmgr` |
| Autostart service recovery | `svcmgr` with declarative restart limits |
| Keyboard I/O policy and lifecycle | `svcmgr` + `kbd` |
| PS/2 port access | `KIoPort` capability (delivered to `kbd` by kernel) |
| IRQ delivery | `KIrqCap` → `KChannel` routing (delivered to `svcmgr` by kernel) |
| VFS session state and content | `vfs` service |
| Global diagnostics entry point | `svcmgr` aggregation over `SYS_DIAG_SNAPSHOT` + subsystem `STATUS` |

---

## Implemented Pieces

| Area | Status | Notes |
|------|--------|-------|
| UEFI boot | Working | Loads ELF kernel, passes boot info |
| PMM + paging | Working | Per-process user mappings, kernel/user split |
| GDT / TSS / IDT | Working | Ring transitions, exceptions, IRQ dispatch |
| Scheduler | Working | Round-robin with explicit blocking/wakeup |
| Capability core | Working | `KObject`, `KChannel`, `KProcess`, `KNotification`, `KVmo`, `KIrqCap`, `KIoPort`, `KBootstrapCap` |
| Handle tables | Working | Per-process, 1024 slots, generation-tagged |
| Syscalls | Working | 39 live syscalls (0–38); retired numbers return `ERR_NOT_SUPPORTED` |
| Service manager | Working | Ring-3 ELF; owns live lookup; restarts autostart services with bounded retry |
| Bootstrap authority | Working | `svcmgr` receives explicit `KBootstrapCap`; `init` receives svcmgr bootstrap handle |
| Keyboard service | Working | Ring-3 ELF; owns PS/2 I/O via `KIoPort`; IRQ1 routed via `KIrqCap` |
| VFS service | Working | Ring-3 ELF; owns all client-visible file state, offsets, stale-id rejection, dead-client reclaim |
| IRQ routing | Working | `KIrqCap`-gated; route ownership tied to process; auto-cleared on exit |
| I/O port access | Working | `KIoPort`-gated; `SYS_IOPORT_IN` / `SYS_IOPORT_OUT` check capability |
| Service protocols | Working | Shared headers, versioned message layouts for `svcmgr`, `vfs`, `kbd` |
| Diagnostics | Working | `SYS_DIAG_SNAPSHOT` + `svcmgr` aggregation |
| ELF loader | Working | Loads multi-segment ELF, creates page table, maps stack |
| initrd | Working | Four embedded ELF services: `svcmgr`, `kbd`, `vfs`, `init` |
| Pure kernel binary | Working | Zero userland code in kernel ELF for non-selftest builds (Phase 21) |
| Header dependency tracking | Working | `-MMD -MP` in Makefile; stale-`.o` rebuilds are automatic |

---

## Boot Sequence

```
1. UEFI loader → loads kernel ELF, passes boot_info
2. Kernel → PMM, paging, GDT, IDT, PIC/PIT, framebuffer, syscalls, IRQ routing, scheduler
3. Kernel → loads svcmgr from initrd (elf_loader), task_spawn_elf
4. Kernel → delivers KBootstrapCap + KIrqCap(IRQ1) + KIoPort(0x60-0x64) to svcmgr
5. Kernel → loads init from initrd (elf_loader), task_spawn_elf
6. Kernel → delivers svcmgr bootstrap channel handle to init (in RBX)
7. Scheduler starts

User-space boot:
8.  svcmgr → receives capabilities, spawns kbd and vfs from initrd
9.  kbd    → receives KIoPort cap, registers IRQ1 route, flushes PS/2, signals ready
10. vfs    → seeds boot exports (iris.txt, services.txt), signals ready
11. svcmgr → signals ready
12. init   → looks up kbd and vfs via svcmgr, validates all service paths, exits 0
```

Healthy boot no longer depends on any kernel nameserver path. All service discovery is IPC handle transfer through `svcmgr`.

## Technical Contracts

Current executable contracts are documented here:

- [docs/contracts/boot.md](docs/contracts/boot.md)
- [docs/contracts/bootstrap.md](docs/contracts/bootstrap.md)
- [docs/contracts/svcmgr.md](docs/contracts/svcmgr.md)
- [docs/contracts/vfs.md](docs/contracts/vfs.md)
- [docs/contracts/kbd.md](docs/contracts/kbd.md)
- [docs/contracts/diag.md](docs/contracts/diag.md)
- [docs/contracts/hardening.md](docs/contracts/hardening.md)

---

## Syscall Surface

Full list (kernel/include/iris/syscall.h):

| Number | Name | Status |
|--------|------|--------|
| 0 | `SYS_WRITE` | Live |
| 1 | `SYS_EXIT` | Live |
| 2 | `SYS_GETPID` | Live |
| 3 | `SYS_YIELD` | Live |
| 4 | `SYS_OPEN` | Retired → `ERR_NOT_SUPPORTED` |
| 5 | `SYS_READ` | Retired → `ERR_NOT_SUPPORTED` |
| 6 | `SYS_CLOSE` | Retired → `ERR_NOT_SUPPORTED` |
| 7 | `SYS_BRK` | Retired → `ERR_NOT_SUPPORTED` (Phase 20) |
| 8 | `SYS_SLEEP` | Live |
| 12 | `SYS_CHAN_CREATE` | Live |
| 13 | `SYS_CHAN_SEND` | Live |
| 14 | `SYS_CHAN_RECV` | Live |
| 15 | `SYS_HANDLE_CLOSE` | Live |
| 16 | `SYS_VMO_CREATE` | Live |
| 17 | `SYS_VMO_MAP` | Live |
| 18 | `SYS_SPAWN` | Retired → `ERR_NOT_SUPPORTED` (Phase 19) |
| 19 | `SYS_NOTIFY_CREATE` | Live |
| 20 | `SYS_NOTIFY_SIGNAL` | Live |
| 21 | `SYS_NOTIFY_WAIT` | Live |
| 22 | `SYS_HANDLE_DUP` | Live |
| 23 | `SYS_HANDLE_TRANSFER` | Live |
| 24 | `SYS_NS_REGISTER` | Retired → `ERR_NOT_SUPPORTED` |
| 25 | `SYS_NS_LOOKUP` | Retired → `ERR_NOT_SUPPORTED` |
| 26 | `SYS_PROCESS_STATUS` | Live |
| 27 | `SYS_IRQ_ROUTE_REGISTER` | Live (requires `KIrqCap`) |
| 28 | `SYS_PROCESS_SELF` | Live |
| 29 | `SYS_PROCESS_WATCH` | Live |
| 30 | `SYS_DIAG_SNAPSHOT` | Live |
| 31 | `SYS_SPAWN_SERVICE` | Live (requires `KBootstrapCap`) |
| 32 | `SYS_IOPORT_IN` | Live (requires `KIoPort` cap) |
| 33 | `SYS_IOPORT_OUT` | Live (requires `KIoPort` cap) |
| 34 | `SYS_CHAN_RECV_NB` | Live (non-blocking recv) |
| 35 | `SYS_PROCESS_KILL` | Live (requires `RIGHT_MANAGE`) |
| 36 | `SYS_VMO_UNMAP` | Live |
| 37 | `SYS_CHAN_SEAL` | Live |
| 38 | `SYS_CHAN_CALL` | Live (synchronous send+recv RPC) |

Heap allocation is exclusively via `SYS_VMO_CREATE` + `SYS_VMO_MAP`. `SYS_BRK` is permanently retired.

---

## Capability Object Model

Every kernel resource that a user process can act on is a `KObject`. Access is mediated by handles in the process's `KHandleTable`.

| Type | Purpose |
|------|---------|
| `KOBJ_CHANNEL` | Bidirectional IPC channel; supports attached-handle transfer |
| `KOBJ_VMO` | Virtual memory object; mapped into process address spaces |
| `KOBJ_NOTIFICATION` | Lightweight event primitive; signal + wait |
| `KOBJ_PROCESS` | KProcess handle; grants access to `SYS_PROCESS_WATCH`, `SYS_PROCESS_KILL`, `SYS_PROCESS_STATUS` |
| `KOBJ_IRQ_CAP` | IRQ routing capability; required to call `SYS_IRQ_ROUTE_REGISTER` |
| `KOBJ_IOPORT` | I/O port range capability; required to call `SYS_IOPORT_IN` / `SYS_IOPORT_OUT` |
| `KOBJ_BOOTSTRAP_CAP` | Spawn authority; required to call `SYS_SPAWN_SERVICE` |

Rights are per-handle: `RIGHT_READ`, `RIGHT_WRITE`, `RIGHT_MANAGE`, `RIGHT_TRANSFER`, `RIGHT_ROUTE`.

---

## Service Protocols

### svcmgr (kernel/include/iris/svcmgr_proto.h)

Versioned by `SVCMGR_PROTO_VERSION`. Covers:

- bootstrap-handle delivery (kernel → svcmgr, kernel → init)
- service lookup with IPC handle transfer
- `SYS_PROCESS_WATCH`-based lifecycle events
- `STATUS` reply: catalog entry count, ready service count, active supervision slots, catalog version
- `DIAG` reply: compact global summary (kernel snapshot + svcmgr + vfs + kbd)

### vfs (kernel/include/iris/vfs_proto.h)

Versioned by `VFS_PROTO_VERSION`. Covers:

- `OPEN` (attaches caller KProcess handle for watch-based dead-client reclaim)
- `READ`, `CLOSE`
- `LIST` (enumerable boot namespace)
- `STATUS`: ready export count, active open count, capacity, exported byte total

### kbd (kernel/include/iris/kbd_proto.h)

Versioned by `KBD_PROTO_VERSION`. Covers:

- `HELLO` (liveness probe)
- `STATUS`: keyboard flags (`KBD_STATUS_READY`, `KBD_STATUS_PS2_OK`)
- Routed IRQ scancode delivery over `KChannel`

### Kernel diagnostics (kernel/include/iris/diag.h)

`SYS_DIAG_SNAPSHOT` fills a compact struct with kernel-owned pool counters:

- live scheduler tasks, live KProcess slots
- live KChannel / KNotification / KVmo slots
- active IRQ routes, scheduler tick count

---

## Pool Capacities (healthy build)

| Resource | Capacity |
|----------|----------|
| `TASK_MAX` | 16 |
| `KPROCESS_POOL_SIZE` | 32 |
| `KCHANNEL_POOL_SIZE` | 64 |
| `KCHAN_CAPACITY` | 32 messages |
| `KNOTIF_POOL_SIZE` | 32 |
| `KVMO_POOL_SIZE` | 32 |
| `HANDLE_TABLE_MAX` | 1024 slots per process |
| `VFS_SERVICE_OPEN_FILES` | 32 |

---

## Build And Run

```bash
make clean && make   # full build (header deps auto-tracked)
make run             # launch in QEMU with OVMF
make run-headless    # headless QEMU run with serial log capture
make smoke           # local smoke: default build + selftest-enabled build
make smoke-runtime   # headless runtime smoke with healthy-boot assertion
```

With selftests:

```bash
make clean
make ENABLE_RUNTIME_SELFTESTS=1
make run
```

CI currently validates `make clean`, `make`, `make check`, and a headless runtime smoke that asserts the healthy boot signature from serial logs. See [docs/testing.md](docs/testing.md).

Selftest mode additionally verifies:

- VFS stale-id rejection
- VFS dead-client reclaim and post-reclaim invalidation
- extra spawn / handle / VMO regression checks

---

## What Normal Boot Verifies

```
[IRIS][USER] init task created (ELF), id=2
[USER] [SVCMGR] ready
[USER] init bootstrap start
[USER] kbd hello reply OK
[USER] vfs list reply OK
[USER] vfs open reply OK
[USER] vfs read reply OK
[USER] vfs close reply OK
[SYSCALL] exit code=0
```

---

## Repository Map

```
boot/uefi/boot.c                          UEFI loader
kernel/kernel_main.c                      Kernel bootstrap and subsystem bring-up
kernel/core/syscall/syscall.c             Syscall dispatcher and all handlers
kernel/core/scheduler/scheduler.c         Scheduler and task lifecycle
kernel/core/init/svcmgr_bootstrap.c       Kernel-side bootstrap: delivers caps to svcmgr and init
kernel/core/initrd/initrd.c               Embedded ELF registry (svcmgr, kbd, vfs, init)
kernel/core/irq/irq_routing.c             IRQ routing with process-scoped ownership
kernel/arch/x86_64/idt.c                  ISR stubs, preemptive scheduler tick
kernel/arch/x86_64/paging.c               Per-process page table management
kernel/arch/x86_64/user_init.S            Selftest-only ring-3 code (ENABLE_RUNTIME_SELFTESTS=1)
kernel/new_core/src/                       Capability object implementations
kernel/new_core/include/iris/nc/           Capability object headers
kernel/include/iris/syscall.h              Complete syscall ABI (0–38)
kernel/include/iris/svcmgr_proto.h         svcmgr protocol
kernel/include/iris/vfs_proto.h            vfs protocol
kernel/include/iris/kbd_proto.h            kbd protocol
kernel/include/iris/diag.h                 Kernel diagnostics snapshot contract
kernel/include/iris/service_catalog.h      Declarative service catalog
services/svcmgr/svcmgr.c + entry.S        Ring-3 service manager
services/vfs/vfs.c + entry.S               Ring-3 VFS service
services/kbd/main.S                        Ring-3 keyboard service
services/init/main.c + entry.S             Ring-3 init process (ELF, spawned from initrd)
services/link_service.ld                   Shared linker script for all service ELFs
```

---

## Branches

| Branch | Purpose |
|--------|---------|
| `main` | Promoted stable line |
| `silver` | Active development |
| `staging` | Integration and verification |
| `collab` | Collaboration branch |

Governance references:

- [CONTRIBUTING.md](CONTRIBUTING.md)
- [docs/branching.md](docs/branching.md)
- [docs/roadmap/initial-backlog.md](docs/roadmap/initial-backlog.md)
