# IRIS OS

IRIS is a custom x86_64 operating system built from scratch with UEFI boot, ring 0 / ring 3 separation, and a pure capability-based microkernel architecture.

Zero userland code is compiled into the kernel binary. Every user process — `init`, `svcmgr`, `kbd`, `vfs` — is a standalone ELF service loaded from an embedded initrd at boot. The kernel spawns only `init`; everything else is spawned from userland. Hardware resource policy (which service owns which IRQ or I/O port range) lives entirely in `svcmgr`, not in the kernel.

---

## Current Status

The system boots cleanly to a healthy interactive state. All subsystems run in ring 3.

**What works today:**

- Pure microkernel boot: kernel spawns only `init`; `init` spawns `svcmgr`; `svcmgr` spawns all other services
- Ring-3 user tasks with per-process isolated address spaces and capability handle tables
- Multi-threading: `SYS_THREAD_CREATE` / `SYS_THREAD_EXIT`; `KProcess.thread_count` tracks live threads; process teardown fires only on last thread exit
- Futex: `SYS_FUTEX_WAIT` / `SYS_FUTEX_WAKE`; 64-entry table keyed by `(virtual_address, owning_process)` — cross-process safe
- Demand paging: VMO pages are allocated lazily on first access; PTEs installed on fault; zero-cost for unaccessed regions
- `KChannel` IPC with attached-handle transfer and explicit rights capping
- `KNotification` event objects: 64-bit signal bits, up to 4 concurrent waiters, wake-one on signal
- `KVmo` virtual memory objects: `SYS_VMO_CREATE` → `SYS_VMO_MAP` → `SYS_VMO_UNMAP` lifecycle with lazy backing
- `KIrqCap` / `KIoPort` capabilities gate all hardware access from ring 3
- `KInitrdEntry` capability: decouples initrd lookup from process spawn
- `KBootstrapCap`: two-permission authority token (`SPAWN_SERVICE` + `HW_ACCESS`)
- A fully userland `svcmgr` (ring-3 C ELF) that owns service spawn policy, hardware cap requests, live lookup, and bounded autostart restart
- `kbd` service owns PS/2 I/O exclusively via a `KIoPort` delegated from `svcmgr`
- `vfs` service owns all client-visible file state, session offsets, dead-client reclaim, and an enumerable boot namespace
- Userland fault isolation: ring-3 exceptions (page fault, GP, etc.) deliver a `FAULT_MSG_NOTIFY` to any registered exception handler, then kill the faulting task; NMI / #DF / MCE remain fatal unconditionally
- IRQ routing owned by `svcmgr`: routes are process-scoped and auto-cleared on process exit
- `SYS_WAIT_ANY`: block until any one of up to **64** channels has a message (multiplexed recv)
- `SYS_PROCESS_WATCH`: up to **4** independent exit-watch subscribers per process
- `SYS_IOPORT_RESTRICT`: delegate a narrower I/O port sub-range from an existing `KIoPort`
- `SYS_VMO_SHARE`: non-destructive VMO dup into another process's handle table
- `SYS_BOOTCAP_RESTRICT`: narrow a `KBootstrapCap`'s permission set at runtime
- `SYS_CHAN_CALL`: synchronous send+recv RPC in one syscall
- Header dependency tracking (`-MMD -MP`); stale `.o` rebuilds are automatic

---

## Architecture Snapshot

| | |
|---|---|
| **Target** | x86_64 |
| **Boot** | UEFI (`BOOTX64.EFI`) via a custom UEFI loader |
| **Execution model** | Kernel in ring 0; all user processes in ring 3 |
| **Memory model** | 4-level paging, per-process user mappings, higher-half kernel, demand-paged VMOs |
| **Scheduling** | Timer-driven round-robin (100 Hz) with explicit blocking for IPC, notifications, IRQ, futex, and sleep |
| **IPC** | `KChannel` ring-buffer, up to 32 messages, wake-one on send |
| **Handle transfer** | Attached handles move across IPC with explicit rights reduction |
| **Process model** | `KProcess` owns address space and handle table; `struct task` owns CPU context and scheduler state; `thread_count` tracks live threads |
| **Capability objects** | `KObject`, `KChannel`, `KVmo`, `KNotification`, `KIrqCap`, `KIoPort`, `KProcess`, `KBootstrapCap`, `KInitrdEntry` |

**Authority split:**

| Responsibility | Owner |
|---|---|
| Bootstrap cap delivery to `init` | Kernel (injected into init's handle table at spawn) |
| `svcmgr` spawn | `init` (via `SYS_INITRD_LOOKUP` + `SYS_SPAWN_ELF`) |
| Service spawn authority | `KBootstrapCap` (SPAWN_SERVICE) held by `svcmgr` |
| Hardware cap creation policy | `svcmgr` via `SYS_CAP_CREATE_IRQCAP` / `SYS_CAP_CREATE_IOPORT` |
| Live service discovery | `svcmgr` IPC handle transfer |
| Autostart service restart | `svcmgr` with declarative restart limits |
| Keyboard I/O and lifecycle | `svcmgr` + `kbd` |
| PS/2 port access | `KIoPort` cap (requested by `svcmgr`, delegated to `kbd`) |
| IRQ delivery | `KIrqCap` → `KChannel` route (requested by `svcmgr`, registered by `kbd`) |
| VFS session state and content | `vfs` service |
| Userland fault handling | Kernel (kills faulting task; optional `FAULT_MSG_NOTIFY` to exception handler channel) |
| Global diagnostics aggregation | `svcmgr` over `SYS_DIAG_SNAPSHOT` + subsystem `STATUS` channels |

---

## Boot Sequence

```
Kernel:
  1. UEFI loader → loads kernel ELF, passes boot_info
  2. Kernel → PMM, paging, GDT, IDT, PIC/PIT, framebuffer, syscalls, IRQ routing, scheduler
  3. Kernel → loads init from initrd, task_spawn_elf
  4. Kernel → creates KBootstrapCap (SPAWN_SERVICE | HW_ACCESS), inserts into init's handle table
  5. Scheduler starts

User-space:
  6.  init   → SYS_INITRD_LOOKUP("svcmgr") → SYS_SPAWN_ELF → spawns svcmgr
  7.  init   → sends KBootstrapCap to svcmgr over spawn channel
  8.  svcmgr → receives KBootstrapCap, requests KIrqCap(IRQ1) + KIoPort(0x60–0x64)
  9.  svcmgr → SYS_INITRD_LOOKUP("kbd") → SYS_SPAWN_ELF → spawns kbd
  10. svcmgr → SYS_INITRD_LOOKUP("vfs") → SYS_SPAWN_ELF → spawns vfs
  11. svcmgr → delivers KIoPort + KIrqCap to kbd over bootstrap channel
  12. kbd    → flushes PS/2, registers IRQ1 route, signals ready
  13. vfs    → seeds boot exports (iris.txt, services.txt), signals ready
  14. svcmgr → signals ready
  15. init   → looks up kbd and vfs via svcmgr, exercises all service paths
```

---

## Capability Object Model

| Type | Purpose |
|------|---------|
| `KOBJ_CHANNEL` | Bidirectional IPC ring-buffer; supports attached-handle transfer |
| `KOBJ_VMO` | Virtual memory object; mapped with demand-paged backing |
| `KOBJ_NOTIFICATION` | Lightweight event primitive; 64-bit signal bits, up to 4 concurrent waiters |
| `KOBJ_PROCESS` | Process handle; grants `SYS_PROCESS_WATCH`, `SYS_PROCESS_KILL`, `SYS_PROCESS_STATUS` |
| `KOBJ_IRQ_CAP` | IRQ routing capability; required for `SYS_IRQ_ROUTE_REGISTER` |
| `KOBJ_IOPORT` | I/O port range capability; required for `SYS_IOPORT_IN` / `SYS_IOPORT_OUT`; sub-rangeable via `SYS_IOPORT_RESTRICT` |
| `KOBJ_BOOTSTRAP_CAP` | Authority token for `SYS_CAP_CREATE_IRQCAP`, `SYS_CAP_CREATE_IOPORT`, `SYS_INITRD_LOOKUP`; permissions narrowable via `SYS_BOOTCAP_RESTRICT` |
| `KOBJ_INITRD_ENTRY` | Immutable reference to a named ELF image in the initrd; required for `SYS_SPAWN_ELF` |

Rights are per-handle: `RIGHT_READ`, `RIGHT_WRITE`, `RIGHT_MANAGE`, `RIGHT_TRANSFER`, `RIGHT_DUPLICATE`, `RIGHT_ROUTE`.

---

## Syscall Surface

| Number | Name | Status | Notes |
|--------|------|--------|-------|
| 0 | `SYS_WRITE` | Live | Write to serial |
| 1 | `SYS_EXIT` | Live | Terminate calling process |
| 2 | `SYS_GETPID` | Live | Return task ID |
| 3 | `SYS_YIELD` | Live | Cooperative yield |
| 4 | `SYS_OPEN` | Retired | → `ERR_NOT_SUPPORTED`; use VFS service |
| 5 | `SYS_READ` | Retired | → `ERR_NOT_SUPPORTED`; use VFS service |
| 6 | `SYS_CLOSE` | Retired | → `ERR_NOT_SUPPORTED`; use VFS service |
| 7 | `SYS_BRK` | Retired | → `ERR_NOT_SUPPORTED`; use `SYS_VMO_CREATE` + `SYS_VMO_MAP` |
| 8 | `SYS_SLEEP` | Live | Sleep N scheduler ticks |
| 12 | `SYS_CHAN_CREATE` | Live | Allocate a `KChannel` |
| 13 | `SYS_CHAN_SEND` | Live | Send message (may attach a handle) |
| 14 | `SYS_CHAN_RECV` | Live | Blocking receive |
| 15 | `SYS_HANDLE_CLOSE` | Live | Drop a handle |
| 16 | `SYS_VMO_CREATE` | Live | Allocate a `KVmo` |
| 17 | `SYS_VMO_MAP` | Live | Map VMO pages into address space (demand-paged) |
| 18 | `SYS_SPAWN` | Retired | → `ERR_NOT_SUPPORTED`; use `SYS_INITRD_LOOKUP` + `SYS_SPAWN_ELF` |
| 19 | `SYS_NOTIFY_CREATE` | Live | Allocate a `KNotification` |
| 20 | `SYS_NOTIFY_SIGNAL` | Live | Set signal bits; wake one waiter |
| 21 | `SYS_NOTIFY_WAIT` | Live | Block until bits != 0; atomically clear and return |
| 22 | `SYS_HANDLE_DUP` | Live | Duplicate handle with reduced rights |
| 23 | `SYS_HANDLE_TRANSFER` | Live | Move handle to another process |
| 24 | `SYS_NS_REGISTER` | Retired | → `ERR_NOT_SUPPORTED`; discovery via `svcmgr` IPC |
| 25 | `SYS_NS_LOOKUP` | Retired | → `ERR_NOT_SUPPORTED`; discovery via `svcmgr` IPC |
| 26 | `SYS_PROCESS_STATUS` | Live | Non-blocking liveness query |
| 27 | `SYS_IRQ_ROUTE_REGISTER` | Live | Requires `KIrqCap` with `RIGHT_ROUTE` |
| 28 | `SYS_PROCESS_SELF` | Live | Return handle to caller's own `KProcess` |
| 29 | `SYS_PROCESS_WATCH` | Live | Register process-exit watch; up to 4 subscribers per process |
| 30 | `SYS_DIAG_SNAPSHOT` | Live | Compact kernel-state snapshot into user buffer |
| 31 | `SYS_SPAWN_SERVICE` | Retired | → `ERR_NOT_SUPPORTED`; use `SYS_INITRD_LOOKUP` + `SYS_SPAWN_ELF` |
| 32 | `SYS_IOPORT_IN` | Live | Requires `KIoPort` with `RIGHT_READ` |
| 33 | `SYS_IOPORT_OUT` | Live | Requires `KIoPort` with `RIGHT_WRITE` |
| 34 | `SYS_CHAN_RECV_NB` | Live | Non-blocking recv; returns `ERR_WOULD_BLOCK` if empty |
| 35 | `SYS_PROCESS_KILL` | Live | Requires `RIGHT_MANAGE`; idempotent; kills all threads |
| 36 | `SYS_VMO_UNMAP` | Live | Remove virtual mappings (does not free physical pages) |
| 37 | `SYS_CHAN_SEAL` | Live | Explicitly close channel; wake all blocked receivers |
| 38 | `SYS_CHAN_CALL` | Live | Synchronous send+recv RPC in one syscall |
| 39 | `SYS_CAP_CREATE_IRQCAP` | Live | Requires `KBootstrapCap` with `HW_ACCESS`; returns `KIrqCap` |
| 40 | `SYS_CAP_CREATE_IOPORT` | Live | Requires `KBootstrapCap` with `HW_ACCESS`; returns `KIoPort` |
| 41 | `SYS_INITRD_LOOKUP` | Live | Requires `KBootstrapCap` with `SPAWN_SERVICE`; returns `KInitrdEntry` |
| 42 | `SYS_SPAWN_ELF` | Live | Requires `KInitrdEntry` with `RIGHT_READ`; spawns process |
| 43 | `SYS_IOPORT_RESTRICT` | Live | Requires `KIoPort` + `RIGHT_READ\|RIGHT_DUPLICATE`; returns narrower `KIoPort` |
| 44 | `SYS_WAIT_ANY` | Live | Block until any of up to **64** channels has a message |
| 45 | `SYS_BOOTCAP_RESTRICT` | Live | AND-reduce `KBootstrapCap` permissions; used by `svcmgr` to drop `HW_ACCESS` post-bootstrap |
| 46 | `SYS_VMO_SHARE` | Live | Non-destructive VMO dup into another process's handle table |
| 47 | `SYS_EXCEPTION_HANDLER` | Live | Register a `KChannel` to receive `FAULT_MSG_NOTIFY` for a process |
| 48 | `SYS_THREAD_CREATE` | Live | Spawn a new thread in an existing process; caller supplies stack pointer and entry |
| 49 | `SYS_THREAD_EXIT` | Live | Exit calling thread; tears down process on last thread |
| 50 | `SYS_FUTEX_WAIT` | Live | Block if `*uaddr == expected`; keyed by `(uaddr, process)` |
| 51 | `SYS_FUTEX_WAKE` | Live | Wake up to N waiters on `uaddr` within the same address space |

---

## Service Protocols

### svcmgr (`kernel/include/iris/svcmgr_proto.h`)

- Bootstrap handle delivery (`init` → `svcmgr` over spawn channel, `svcmgr` → child services)
- Service lookup with IPC handle transfer and rights reduction
- `SYS_PROCESS_WATCH`-based lifecycle events (supports multiple watchers per process)
- `STATUS` reply: catalog count, ready service count, active supervision slots, catalog version
- `DIAG` reply: global summary (kernel snapshot + svcmgr + vfs + kbd)

### vfs (`kernel/include/iris/vfs_proto.h`)

- `OPEN` (attaches caller `KProcess` handle for watch-based dead-client reclaim)
- `READ`, `CLOSE`
- `LIST` (enumerable boot namespace)
- `STATUS`: ready export count, active open count, capacity, exported byte total

### kbd (`kernel/include/iris/kbd_proto.h`)

- `HELLO` (liveness probe)
- `SUBSCRIBE`: client supplies a write-end channel; kbd forwards `SCANCODE_EVENT` on every IRQ
- `STATUS`: keyboard flags (`KBD_STATUS_READY`, `KBD_STATUS_PS2_OK`)

---

## Pool Capacities

| Resource | Capacity |
|----------|----------|
| `TASK_MAX` | 64 |
| `KPROCESS_POOL_SIZE` | 32 |
| `KPROCESS_EXIT_WATCH_MAX` | 4 subscribers per process |
| `KCHANNEL_POOL_SIZE` | 64 |
| `KCHAN_CAPACITY` | 32 messages per channel |
| `KCHANNEL_WAITERS_MAX` | 64 (= TASK_MAX) |
| `KNOTIF_POOL_SIZE` | 64 |
| `KNOTIF_WAITERS_MAX` | 4 concurrent waiters per notification |
| `KVMO_POOL_SIZE` | 32 |
| `KIRQCAP_POOL_SIZE` | 16 |
| `KIOPORT_POOL_SIZE` | 16 |
| `KINITRDENTRY_POOL_SIZE` | 8 |
| `KBOOTCAP_POOL_SIZE` | 4 |
| `HANDLE_TABLE_MAX` | 1024 slots per process |
| `FUTEX_TABLE_SIZE` | 64 entries |
| `WAIT_ANY_MAX_CHANNELS` | 64 channels per call |
| `VFS_SERVICE_OPEN_FILES` | 32 |

---

## Build And Run

```bash
make clean && make       # full build (header deps auto-tracked via -MMD -MP)
make run                 # launch in QEMU with OVMF
make run-headless        # headless QEMU run; serial log captured to build/qemu-headless.log
make smoke               # local smoke: default build + selftest-enabled build
make smoke-runtime       # headless runtime smoke with healthy-boot assertion
```

With selftests enabled (phase 3 handle/channel/notification probes + VFS edge cases):

```bash
make clean && make ENABLE_RUNTIME_SELFTESTS=1 && make run
```

---

## Healthy Boot Output

A successful boot produces this on the serial log:

```
[IRIS][USER] init task created (ELF), id=1
[IRIS][SCHED] running
[SVCMGR] started
[SVCMGR] child bootstrap OK     ← kbd
[SVCMGR] service spawned
[SVCMGR] child bootstrap OK     ← vfs
[SVCMGR] service spawned
[SVCMGR] ready
KBD ready
VFS ready
[USER][INIT][BOOT] healthy path OK
[USER] init echo loop start
```

---

## Repository Map

```
boot/uefi/boot.c                         UEFI loader
kernel/kernel_main.c                     Kernel bootstrap — spawns init, injects KBootstrapCap
kernel/core/syscall/syscall.c            Syscall dispatcher (syscalls 0–51)
kernel/core/scheduler/scheduler.c        Scheduler, task lifecycle, multi-thread support
kernel/core/futex/futex.c               Futex table (64 entries, per-process keyed)
kernel/core/initrd/initrd.c              Embedded ELF registry (svcmgr, kbd, vfs, init)
kernel/core/irq/irq_routing.c            IRQ routing with process-scoped ownership
kernel/core/loader/elf_loader.c          ELF loader: segments, page table, stack
kernel/arch/x86_64/idt.c                 ISR stubs, exception delegation, preemptive scheduler tick
kernel/arch/x86_64/paging.c              Per-process page table management
kernel/new_core/src/                      KObject implementations (kchannel, knotification, kvmo,
                                          kirqcap, kioport, kbootcap, kprocess, kinitrdentry, ...)
kernel/new_core/include/iris/nc/          KObject headers
kernel/include/iris/syscall.h            Syscall ABI (0–51)
kernel/include/iris/svcmgr_proto.h       svcmgr protocol
kernel/include/iris/vfs_proto.h          VFS protocol
kernel/include/iris/kbd_proto.h          kbd protocol
kernel/include/iris/fault_proto.h        Fault notification protocol
kernel/include/iris/diag.h               Kernel diagnostics snapshot contract
services/svcmgr/svcmgr.c + entry.S      Ring-3 service manager
services/svcmgr/service_catalog.h        Declarative service catalog (autostart, restart limits)
services/vfs/vfs.c + entry.S             Ring-3 VFS service
services/kbd/main.S                       Ring-3 keyboard service
services/init/main.c + entry.S           Ring-3 init process (spawns svcmgr)
services/link_service.ld                  Shared linker script for all service ELFs
```

---

## Branches

| Branch | Purpose |
|--------|---------|
| `main` | Promoted stable line |
| `silver` | Active development |
| `staging` | Integration and verification |

References: [CONTRIBUTING.md](CONTRIBUTING.md) · [docs/branching.md](docs/branching.md) · [docs/roadmap/initial-backlog.md](docs/roadmap/initial-backlog.md)
