# IRIS

IRIS is an `x86_64` operating system built around a capability-based microkernel. It runs real user-space services, enforces hardware isolation through a capability system, and boots to an interactive shell entirely from ring-3 code after the first kernel handoff.

## Boot Sequence

```
UEFI → BOOTX64.EFI → KERNEL.ELF
  kernel: PMM, paging, PCID, GDT/IDT/PIC/PIT, LAPIC, scheduler, syscalls, initrd catalog
    → userboot (ring-3 flat binary, injected KBootstrapCap)
      → init
        → fb          (framebuffer painter, fire-and-forget)
        → console     (serial output service)
        → svcmgr      (service manager and supervisor)
          → kbd       (PS/2 keyboard driver)
          → vfs       (boot-namespace filesystem)
          → sh        (interactive shell)
```

Every service from `init` onward is a ring-3 ELF binary loaded by `svc_loader` using only kernel primitives. The kernel itself spawns nothing after the initial `userboot` task.

## What Is Running Today

The system reaches a fully interactive runtime with all of the following active:

- **Kernel**: PMM, 4-level paging with PCID (CR4.PCIDE), per-process address spaces, GDT/IDT, 8259A PIC, PIT at 100 Hz, LAPIC detected and software-enabled, round-robin + preemptive scheduler, `syscall`/`sysret` dispatch, capability enforcement
- **SMP foundation**: per-CPU `iris_cpu_local` struct (GS-relative via IA32_GS_BASE), LAPIC ID stored per-CPU, context-switch and idle-tick counters; AP bringup deferred
- **IPC**: `KChannel` ring-buffer IPC (capacity 128), attached-handle transfer with rights reduction, `KNotification` signal/wait, `SYS_CHAN_CALL` synchronous IPC, `SYS_CHAN_RECV_TIMEOUT`, `SYS_WAIT_ANY` (up to 64 channels)
- **Memory**: sparse VMOs with eager map-time allocation (no demand paging), `SYS_VMO_MAP`, `SYS_VMO_UNMAP`, `SYS_VMO_SHARE`, `SYS_VMO_SIZE`, per-process 8 MB physical page quota, VMO page-shard locks (16 shards)
- **Process and thread management**: `SYS_PROCESS_CREATE`, `SYS_THREAD_CREATE`, `SYS_THREAD_START`, `SYS_THREAD_EXIT`, `SYS_PROCESS_WATCH`, `SYS_PROCESS_KILL`, `SYS_PROCESS_STATUS`, automatic teardown on last-thread exit
- **Capability system**: per-handle rights (READ, WRITE, DUPLICATE, TRANSFER, ROUTE, MANAGE), rights reduction on dup/transfer, generation-stamped handle IDs, stale-handle detection
- **Hardware capabilities**: `KIrqCap` (IRQ routing, capability-gated), `KIoPort` (I/O port range, whitelist-validated), `KBootstrapCap` (per-bit permission flags, one-shot framebuffer claim)
- **Futex**: 256-entry futex table with per-process ownership, `SYS_FUTEX_WAIT` / `SYS_FUTEX_WAKE`
- **Fault delivery**: ring-3 faults delivered as `FAULT_MSG_NOTIFY` on a registered exception channel; supervisor resumes or kills via `SYS_EXCEPTION_RESUME`
- **Clock**: `SYS_CLOCK_GET` returns monotonic nanoseconds derived from TSC (when calibrated) or 100 Hz wall ticks (fallback)
- **Scheduler diagnostics**: `SYS_SCHED_INFO` returns a 40-byte snapshot (ticks, wall\_ticks, context\_switches, idle\_ticks, live\_task\_count); requires `IRIS_BOOTCAP_KDEBUG`
- **ASLR**: per-service load bias via RDTSC + xorshift64; R_X86_64_RELATIVE relocations applied at load time
- **Stack protection**: `-fstack-protector-strong -mstack-protector-guard=global`; RDTSC-seeded canary per service
- **VFS**: 4 static boot files + 8 initrd-backed VMO-mapped files = 12 exports at runtime
- **Diagnostics**: `SYS_KLOG_DRAIN` (KDEBUG-gated); `SVCMGR_MSG_DIAG` aggregates service status

## Syscall Surface

70 syscall slots (0–69). Active:

| Number | Name | Notes |
|--------|------|-------|
| 1 | `SYS_EXIT` | |
| 2 | `SYS_GETPID` | |
| 3 | `SYS_YIELD` | |
| 8 | `SYS_SLEEP` | ticks |
| 12 | `SYS_CHAN_CREATE` | |
| 13 | `SYS_CHAN_SEND` | |
| 14 | `SYS_CHAN_RECV` | |
| 15 | `SYS_HANDLE_CLOSE` | |
| 16 | `SYS_VMO_CREATE` | |
| 17 | `SYS_VMO_MAP` | |
| 19 | `SYS_NOTIFY_CREATE` | |
| 20 | `SYS_NOTIFY_SIGNAL` | |
| 21 | `SYS_NOTIFY_WAIT` | |
| 22 | `SYS_HANDLE_DUP` | |
| 23 | `SYS_HANDLE_TRANSFER` | |
| 26 | `SYS_PROCESS_STATUS` | |
| 27 | `SYS_IRQ_ROUTE_REGISTER` | requires `KIrqCap` + `RIGHT_ROUTE` |
| 28 | `SYS_PROCESS_SELF` | |
| 29 | `SYS_PROCESS_WATCH` | up to 8 watchers per process |
| 32 | `SYS_IOPORT_IN` | requires `KIoPort` |
| 33 | `SYS_IOPORT_OUT` | requires `KIoPort` |
| 34 | `SYS_CHAN_RECV_NB` | non-blocking |
| 35 | `SYS_PROCESS_KILL` | |
| 36 | `SYS_VMO_UNMAP` | |
| 37 | `SYS_CHAN_SEAL` | drain-then-close semantics |
| 38 | `SYS_CHAN_CALL` | synchronous send+recv |
| 39 | `SYS_CAP_CREATE_IRQCAP` | requires `IRIS_BOOTCAP_HW_ACCESS` |
| 40 | `SYS_CAP_CREATE_IOPORT` | requires `IRIS_BOOTCAP_HW_ACCESS` |
| 43 | `SYS_IOPORT_RESTRICT` | narrow port range |
| 44 | `SYS_WAIT_ANY` | up to 64 channels |
| 45 | `SYS_BOOTCAP_RESTRICT` | one-way narrow |
| 46 | `SYS_VMO_SHARE` | |
| 47 | `SYS_EXCEPTION_HANDLER` | register fault channel |
| 48 | `SYS_THREAD_CREATE` | |
| 49 | `SYS_THREAD_EXIT` | |
| 50 | `SYS_FUTEX_WAIT` | |
| 51 | `SYS_FUTEX_WAKE` | |
| 52 | `SYS_HANDLE_TYPE` | |
| 53 | `SYS_HANDLE_SAME_OBJECT` | |
| 54 | `SYS_POWEROFF` | requires `IRIS_BOOTCAP_KDEBUG` |
| 55 | `SYS_INITRD_VMO` | requires `IRIS_BOOTCAP_SPAWN_SERVICE` |
| 56 | `SYS_PROCESS_CREATE` | requires `IRIS_BOOTCAP_SPAWN_SERVICE` |
| 57 | `SYS_VMO_MAP_INTO` | |
| 58 | `SYS_THREAD_START` | |
| 59 | `SYS_HANDLE_INSERT` | |
| 60 | `SYS_FRAMEBUFFER_VMO` | one-shot; requires `IRIS_BOOTCAP_FRAMEBUFFER` |
| 61 | `SYS_INITRD_COUNT` | requires `IRIS_BOOTCAP_SPAWN_SERVICE` |
| 62 | `SYS_CLOCK_GET` | monotonic ns; TSC or wall-tick fallback |
| 63 | `SYS_CHAN_RECV_TIMEOUT` | ns deadline |
| 64 | `SYS_NOTIFY_WAIT_TIMEOUT` | ns deadline |
| 65 | `SYS_KLOG_DRAIN` | requires `IRIS_BOOTCAP_KDEBUG` |
| 66 | `SYS_EXCEPTION_RESUME` | action=0 resume, action=1 kill |
| 67 | `SYS_VMO_SIZE` | returns VMO byte size |
| 68 | `SYS_IRQ_ACK` | unmask IRQ line after handler consumes event |
| 69 | `SYS_SCHED_INFO` | 40-byte scheduler snapshot; requires `IRIS_BOOTCAP_KDEBUG` |

Retired (return `IRIS_ERR_NOT_SUPPORTED`): `SYS_WRITE(0)`, `SYS_BRK(7)`, `SYS_SPAWN(18)`, `SYS_NS_REGISTER(24)`, `SYS_NS_LOOKUP(25)`, `SYS_DIAG_SNAPSHOT(30)`, `SYS_SPAWN_SERVICE(31)`, `SYS_INITRD_LOOKUP(41)`, `SYS_SPAWN_ELF(42)`.

## Architecture

### Kernel Objects

| Type | Description |
|------|-------------|
| `KOBJ_CHANNEL` | Ring-buffer IPC channel; capacity 128 messages |
| `KOBJ_VMO` | Memory object; sparse (eagerly populated at map time) or contiguous/MMIO wrap, up to 16384 pages |
| `KOBJ_NOTIFICATION` | Lightweight signal/wait |
| `KOBJ_PROCESS` | Process container; owns address space (CR3 + PCID) and handle table |
| `KOBJ_IRQ_CAP` | Hardware IRQ routing capability |
| `KOBJ_IOPORT` | I/O port range capability (whitelist-validated) |
| `KOBJ_BOOTSTRAP_CAP` | First-task authority cap with per-bit permission flags |
| `KOBJ_INITRD_ENTRY` | Read-only handle to an initrd image slot |

### Capability Rights

Rights are stored per-handle, not per-object. When a handle is duplicated or transferred, rights can only be reduced, never elevated. Rights that collapse to `RIGHT_NONE` are rejected at dup and send.

- `RIGHT_READ` — read, receive
- `RIGHT_WRITE` — write, send
- `RIGHT_DUPLICATE` — dup the handle
- `RIGHT_TRANSFER` — attach to a channel message
- `RIGHT_ROUTE` — register an IRQ route
- `RIGHT_MANAGE` — manage a target process

### Bootstrap Capability Bits

`KBootstrapCap` delivered to `userboot` with flags:

- `IRIS_BOOTCAP_SPAWN_SERVICE` — authorizes `SYS_INITRD_COUNT`, `SYS_INITRD_VMO`, `SYS_PROCESS_CREATE`
- `IRIS_BOOTCAP_HW_ACCESS` — authorizes `SYS_CAP_CREATE_IRQCAP`, `SYS_CAP_CREATE_IOPORT`; `svcmgr` strips this via `SYS_BOOTCAP_RESTRICT` after claiming all hardware caps
- `IRIS_BOOTCAP_KDEBUG` — authorizes `SYS_POWEROFF`, `SYS_KLOG_DRAIN`, `SYS_SCHED_INFO`
- `IRIS_BOOTCAP_FRAMEBUFFER` — authorizes `SYS_FRAMEBUFFER_VMO` (one-shot; kernel clears flag after first claim)

### PCID — Process Context Identifiers

When the CPU supports CPUID.01H:ECX[17], the kernel enables CR4.PCIDE at boot. Each `KProcess` is assigned a unique PCID (range 1–4094) from an atomic counter; the kernel itself always runs with PCID 0. The scheduler includes the PCID in every CR3 load, so TLB entries for other live processes are not evicted on a context switch. CR3 is loaded with bit 63 = 0 (flush) on every switch; the no-flush optimization is deferred to a later phase. On CPUs without PCID the kernel falls back to full TLB flushes.

### SMP Foundation

`struct iris_cpu_local` is stored at GS:0 on the boot CPU (IA32_GS_BASE = `&cpu_local[0]`). It tracks the current task, LAPIC ID, context-switch count, and idle-tick count per CPU. The LAPIC is probed via CPUID and IA32_APIC_BASE, mapped through the physmap window, and software-enabled after `idt_init()`. The PIC and PIT remain the active interrupt source; AP bringup and the LAPIC timer are deferred.

`cpu_self()` (GS-relative read) is safe from task context. It is not used from the bare IRQ handler stack to avoid a crash on the boot CPU's entry stack where GS semantics are not yet fully controlled.

### Process Lifecycle

Process teardown is ordered and automatic:

1. Last thread calls `task_exit_current`
2. `kprocess_teardown`: emit exit watches → clear exception channel → clear VMO mappings → unregister IRQ routes → close handle table
3. `kprocess_reap_address_space`: destroy page tables
4. `kprocess_free`: release kernel object

Exit watches (`SYS_PROCESS_WATCH`) fire before the handle table is closed, so watchers receive a handle ID that is still live at notification time. Up to 8 watchers per process.

### IRQ Delivery — Deferred ACK Model

IRQ delivery follows a seL4-style deferred ACK contract:

1. Kernel masks the IRQ line and sends EOI to clear the PIC ISR bit
2. Kernel signals the registered `KChannel` (via `irq_routing_signal`)
3. Ring-3 handler reads hardware registers (e.g. port 0x60 via `SYS_IOPORT_IN`)
4. Ring-3 calls `SYS_IRQ_ACK` to unmask the line, re-enabling subsequent interrupts

If the handler never calls `SYS_IRQ_ACK` the IRQ line stays masked permanently, giving the handler full control over delivery rate.

### IPC Invariants

- A sealed channel (`SYS_CHAN_SEAL`) rejects new sends but allows receivers to drain buffered messages before returning `IRIS_ERR_CLOSED`
- Attached handles are moved (not copied): the sender's slot is consumed on a successful send; the kernel installs the object into the receiver's handle table on recv
- Attached rights are reduced: `effective = source_handle_rights & attached_rights`; the receiver cannot exercise rights the sender did not hold or did not grant
- `SYS_WAIT_ANY` sets task state atomically with waiter enqueue to prevent preemption-window stalls on a 100 Hz scheduler
- Timeout receivers remove themselves from waiter lists in task context after wakeup (`kchannel_cancel_waiter`)

### Hardening Invariants

- `SYS_IRQ_ROUTE_REGISTER` requires `KIrqCap` with `RIGHT_ROUTE`, a channel with `RIGHT_READ | RIGHT_WRITE`, and a process handle with `RIGHT_ROUTE`; the caller never chooses the IRQ number directly
- IRQ route ownership is process-scoped: `kprocess_teardown` always calls `irq_routing_unregister_owner`, which only masks the PIC line if a route was actually registered to that process
- `SYS_FRAMEBUFFER_VMO` clears the valid flag before copying the params struct to prevent TOCTOU double-claim
- usercopy validates pages through the current process VMO mapping list and PTEs; it never allocates pages (demand paging was removed in Fase 6.1)
- PMM double-free calls `iris_panic`; `irq_spinlock_t` guards all PMM, futex, and klog operations
- Stack RSP for bootstrap tasks is randomized by 0–240 bytes (16-byte aligned, RDTSC entropy) to reduce stack-layout predictability

## Services

### `svcmgr`

- Receives hardware caps from `KBootstrapCap` and distributes them to catalog services
- Bootstraps catalog services (`kbd`, `vfs`, `sh`) and delivers endpoint, reply, console, IRQ, and I/O port handles
- Handles lookup by numeric endpoint (`SVCMGR_MSG_LOOKUP`) and published name (`SVCMGR_MSG_LOOKUP_NAME`)
- Dynamic publish (`SVCMGR_MSG_REGISTER`) and withdraw (`SVCMGR_MSG_UNREGISTER`) for runtime services
- Supervised restart with per-service budget and counter
- Diagnostics aggregation (`SVCMGR_MSG_DIAG`): queries VFS and kbd status directly, returns composite reply
- Narrows `KBootstrapCap` after hardware cap acquisition: strips `HW_ACCESS` and `FRAMEBUFFER`, keeps `SPAWN_SERVICE` and `KDEBUG`

### `vfs`

- 4 static boot files: `iris.txt`, `services.txt`, `readme.txt`, `catalog.txt`
- 8 initrd-backed files mapped via `SYS_INITRD_VMO` + `SYS_VMO_MAP`; demand-faulted transparently
- 12 total exports at runtime
- Protocols: `VFS_MSG_OPEN`, `VFS_MSG_READ`, `VFS_MSG_CLOSE`, `VFS_MSG_LIST`, `VFS_MSG_STATUS`
- Open file limit: 32 concurrent file handles

### `kbd`

- PS/2 keyboard driver; IRQ 1 routed via `KIrqCap`; deferred ACK via `SYS_IRQ_ACK`
- Protocols: `KBD_MSG_HELLO`, `KBD_MSG_STATUS`, `KBD_MSG_SUBSCRIBE`
- Scancode events forwarded to all subscribers via `KBD_MSG_SCANCODE_EVENT`

### `console`

- Serial UART output service (0x3F8, 8 registers)
- Single message type: `CONSOLE_MSG_WRITE`

### `fb`

- Framebuffer painter; claims framebuffer VMO via `SYS_FRAMEBUFFER_VMO`
- Fire-and-forget; exits after painting

### `sh`

- Interactive shell receiving scancode events from `kbd`
- Commands: `help`, `ver`, `ls`, `cat <file>`, `clear`

## Limits

| Constant | Value |
|----------|-------|
| `TASK_MAX` | 256 |
| `KPROCESS_MAX_LIVE` | 64 |
| `HANDLE_TABLE_MAX` | 1024 |
| `KCHAN_CAPACITY` | 128 messages |
| `KCHANNEL_WAITERS_MAX` | 8 per channel |
| `KPROCESS_EXIT_WATCH_MAX` | 8 per process |
| `KPROCESS_PHYS_PAGES_LIMIT` | 2048 pages (8 MB per process) |
| `KVMO_MAX_PAGES` | 16384 pages (64 MB per VMO) |
| `KVMO_PAGE_SHARDS` | 16 shard locks per VMO |
| `FUTEX_TABLE_SIZE` | 256 |
| `VFS_SERVICE_EXPORTS` | 16 slots |
| `VFS_SERVICE_OPEN_FILES` | 32 |
| `WAIT_ANY_MAX_CHANNELS` | 64 |
| `IRQ_ROUTE_MAX` | 16 |
| `PCID range` | 1–4094 per process; 0 = kernel |

## Key Address Space Constants

| Constant | Value |
|----------|-------|
| `USER_TEXT_BASE` | `0x8000200000` |
| `USER_STACK_SIZE` | 32 KB (28 KB usable; 1 guard page) |
| `USER_VMO_BASE` | `0x8050000000` |
| `KERNEL_VIRT_BASE` | `0xFFFFFFFF80000000` |
| `PHYS_TO_VIRT(p)` | `p + 0xFFFF800000000000` |
| `KSTACK_VIRT_BASE` | `0xFFFF800100000000` |
| Kernel stack slot | 12288 bytes (1 guard + 2 data pages × 256 tasks = 3 MB virtual) |

## Build

**Requirements:**

- `gcc`, `ld`, `objcopy`
- `gnu-efi`
- `qemu-system-x86_64`
- OVMF firmware

**Full rebuild:**

```bash
make clean && make
```

Zero warnings policy: the build is treated as broken if `gcc -Wall -Wextra -Wshadow -Wundef` produces any diagnostic.

**Run interactively:**

```bash
make run
```

**Headless CI lanes:**

```bash
make smoke-runtime                      # 25 s default lane
ENABLE_RUNTIME_SELFTESTS=1 make smoke-runtime-selftests   # 35 s selftest lane
make smoke-full                         # 90 s extended lane
```

## Runtime Validation

The default CI lane (`smoke-runtime`, `smoke-full`) validates the following markers on every run:

- `[IRIS][SCHED] running` — scheduler reached
- `[SVCMGR] ready` — svcmgr bootstrapped all catalog services
- `[USER][INIT][BOOT] healthy path OK` — init completed all staged health checks
- `[USER][INIT][DIAG] reply` — diagnostics round-trip completed
- `[USER][INIT][TIMED] recv timeout OK` — timed IPC selftest
- `[USER][INIT][S8] exception delivery OK` — fault delivery and exception resume
- `[USER][INIT][S9] channel seal OK` — seal semantics (drain-then-close)
- `[USER][INIT][S10] rights reduction OK` — attached-handle rights reduction enforced

The selftest lane additionally requires:

- `[IRIS][P3] handle/lifecycle selftests OK`
- `[IRIS][P41] rights selftests OK`
- `[SVCMGR][DIAG] kbd status OK`

## What Does Not Exist Yet

IRIS is not a general-purpose OS. The current tree does not provide:

- persistent disk filesystem
- networking stack
- full SMP / AP bringup (infrastructure present; scheduler and IRQ delivery are single-CPU)
- userland ELF loading from external storage
- dynamic linker or shared libraries
- POSIX compatibility layer
- production crash recovery or observability tooling
- broad hardware platform support beyond QEMU x86-64

## Positioning

IRIS is a capability-based microkernel project with a real user-space service boundary. It is not a toy kernel and it is not production software. It is a serious experimental system: architecturally coherent, headless-validated on every commit, and built to be a real platform for systems research.

The kernel owns: boot, PMM, 4-level paging with PCID, GDT/IDT, PIC/PIT, LAPIC detection, scheduler with preemption, syscall dispatch, capability enforcement, IRQ routing, initrd catalog, and first-task creation.

The kernel does not own: VFS logic, keyboard handling, serial console output, runtime service discovery, service supervision, or shell behavior. All of that lives in ring 3.
