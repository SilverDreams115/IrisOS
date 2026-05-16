# IRIS

IRIS is an `x86_64` operating system project with a capability-oriented microkernel architecture, UEFI boot, and a strict privilege split between a small ring-0 kernel and ring-3 services.

Today, IRIS is no longer a kernel-with-demo-services. The healthy boot path is service-driven after the first bootstrap task:

1. UEFI loads `BOOTX64.EFI`.
2. The loader loads `KERNEL.ELF` and passes `boot_info`.
3. The kernel initializes memory, paging, interrupts, scheduler, syscalls, and the embedded initrd catalog.
4. The kernel spawns one minimal ring-3 `userboot` task and injects one `KBootstrapCap`.
5. `userboot` loads `init` using ring-3 loader primitives.
6. `init` spawns `console`, `fb`, and `svcmgr`.
7. `svcmgr` spawns and supervises `kbd`, `vfs`, and `sh`.
8. `init` validates discovery, diagnostics, VFS, keyboard subscription, timed IPC, and dynamic publication.

The current tree boots successfully in QEMU and reaches an interactive runtime.

## Current Positioning

IRIS is best described as:

- a capability-based microkernel-style OS
- with real user-space service bootstrap and user-space drivers/services
- still experimental, not production-ready

It is closer to a serious architectural prototype than to a toy kernel, but it is not a finished or pure microkernel in the strongest possible sense. The kernel still owns important bootstrap machinery, embeds service images in-kernel, and the overall system is intentionally narrow in scope.

## What Works

- UEFI boot through `BOOTX64.EFI`
- Higher-half `x86_64` kernel
- Physical memory manager and 4-level paging
- Per-process address spaces
- Ring-0 kernel / ring-3 services split
- Timer-driven round-robin scheduler at 100 Hz
- Syscall entry via `syscall/sysret`
- Capability handles with per-handle rights reduction
- `KChannel` IPC with attached-handle transfer
- `KNotification` signal/wait
- `KProcess` lifecycle operations and exit watches
- `KVmo` memory objects with demand paging
- Ring-3 ELF service loading through `svc_loader`
- Embedded initrd image catalog exposed by index only
- User-space service manager (`svcmgr`)
- User-space VFS service (`vfs`)
- User-space keyboard service (`kbd`) with IRQ routing
- User-space serial console service (`console`)
- User-space shell service (`sh`)
- User-space framebuffer painter service (`fb`)
- Diagnostics path through `SYS_DIAG_SNAPSHOT` and service `STATUS` / `DIAG`
- Headless runtime validation in normal and selftest configurations

## What Does Not Exist Yet

IRIS is not a general-purpose operating system yet. The current tree does not provide:

- persistent disk filesystem
- networking stack
- SMP / multicore support
- userland process loading from external storage
- dynamic linker or shared libraries
- POSIX compatibility
- preemptive priority scheduling
- process isolation hardening beyond the current project scope
- mature crash recovery or production observability

Several legacy syscall paths are intentionally retired and return `IRIS_ERR_NOT_SUPPORTED`, including:

- `SYS_WRITE`
- `SYS_OPEN`, `SYS_READ`, `SYS_CLOSE`
- `SYS_BRK`
- `SYS_SPAWN`, `SYS_SPAWN_SERVICE`
- `SYS_INITRD_LOOKUP`, `SYS_SPAWN_ELF`

Those old kernel-side convenience paths were replaced by service protocols and lower-level process/VMO primitives.

## Architecture

### Kernel responsibilities

The kernel currently owns:

- boot protocol validation
- PMM and paging
- GDT/IDT/PIC/PIT setup
- scheduler and context switching
- syscall dispatch
- core kernel objects
- task/thread/process primitives
- capability enforcement
- IRQ routing mechanism
- initrd image catalog
- first ring-3 bootstrap task creation

The kernel intentionally does not own the healthy-path VFS, keyboard service logic, runtime service discovery, or shell behavior.

### User-space services

Current runtime services:

- `userboot`: first bootstrap task; loads `init`
- `init`: orchestration and runtime validation
- `svcmgr`: service manager, service lookup, supervision, hardware-cap distribution
- `kbd`: PS/2 keyboard service
- `vfs`: boot-namespace VFS service
- `console`: serial console writer
- `fb`: framebuffer painter
- `sh`: interactive shell

### Capability model

Core kernel object types in active use:

- `KOBJ_CHANNEL`
- `KOBJ_VMO`
- `KOBJ_NOTIFICATION`
- `KOBJ_PROCESS`
- `KOBJ_IRQ_CAP`
- `KOBJ_IOPORT`
- `KOBJ_BOOTSTRAP_CAP`
- `KOBJ_INITRD_ENTRY`

Important properties:

- rights are attached to handles, not globally to objects
- rights can only be reduced when duplicated or transferred
- hardware access is capability-gated
- service loading is performed in ring 3

Bootstrap capability bits currently used:

- `IRIS_BOOTCAP_SPAWN_SERVICE`
- `IRIS_BOOTCAP_HW_ACCESS`
- `IRIS_BOOTCAP_KDEBUG`
- `IRIS_BOOTCAP_FRAMEBUFFER`

## Memory, Scheduling, And IPC

- Scheduling is simple round-robin with a default 20 ms quantum at 100 Hz.
- `TASK_MAX` is currently `64`.
- The kernel still uses several fixed structural limits.
- `KChannel` objects are `kpage`-allocated, but each channel queue is bounded.
- Per-process VMO mappings are tracked through a `kpage`-backed linked list.
- Demand-paged VMOs are populated on fault.

Selected current limits:

- `TASK_MAX = 64`
- `HANDLE_TABLE_MAX = 1024`
- `KCHAN_CAPACITY = 128`
- `WAIT_ANY_MAX_CHANNELS = 64`
- `KPROCESS_EXIT_WATCH_MAX = 4`
- `KNOTIF_WAITERS_MAX = 4`
- `VFS_SERVICE_OPEN_FILES = 32`
- `FUTEX_TABLE_SIZE = 256`

These limits are acceptable for an experimental system, but they are part of why IRIS should still be considered non-production.

## Service Protocols

### `svcmgr`

- service bootstrap handle delivery
- endpoint lookup by numeric endpoint or published name
- dynamic runtime publication and withdrawal
- diagnostics aggregation
- supervised restart for catalog services

### `vfs`

- `OPEN`
- `READ`
- `CLOSE`
- `LIST`
- `STATUS`

### `kbd`

- `HELLO`
- `STATUS`
- `SUBSCRIBE`

### `console`

- `CONSOLE_MSG_WRITE`

### `sh`

Current shell commands include:

- `help`
- `ver`
- `ls`
- `cat <file>`
- `clear`

## Build

Requirements:

- `gcc`, `ld`, `objcopy`, `readelf`
- `gnu-efi`
- `qemu-system-x86_64`
- OVMF firmware

Build commands:

```bash
make
make check
```

Interactive run:

```bash
make run
```

Headless run:

```bash
make run-headless
```

## Validation

Validated paths available in-tree:

```bash
make smoke
make smoke-runtime
make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests
```

The selftest runtime path currently expects markers such as:

- `[IRIS][P3] handle/lifecycle selftests OK`
- `[IRIS][P41] rights selftests OK`
- `[SVCMGR] ready`
- `[USER][INIT][BOOT] healthy path OK`
- `[USER][INIT][TIMED] recv timeout OK`

Important build note:

- the `Makefile` uses a shared `build/` directory
- do not run multiple `make` targets in parallel against the same tree if you want reproducible validation results

## Runtime Snapshot

A healthy headless boot currently shows the following high-level behavior:

- kernel enters Phase 50 boot
- first user bootstrap task is created
- `svcmgr` spawns `kbd`, `vfs`, and `sh`
- `vfs` exports at least two boot files
- `init` validates service lookup, diagnostics, dynamic registry, VFS read path, keyboard subscription, and timed IPC
- the shell starts and presents `IRIS shell (Phase 45)`

## Is It A Pure 100% Microkernel?

Not in the strict doctrinal sense.

Why it is microkernel-shaped:

- the healthy-path VFS is in user space
- keyboard handling is in user space
- console output is moved to a ring-3 service
- service spawn policy lives outside the kernel
- runtime discovery and supervision live in `svcmgr`
- the kernel exposes mostly mechanisms and capability checks

Why it is not a strict pure microkernel:

- the kernel still embeds service images directly into the kernel image
- the kernel directly creates and seeds the first user-space task
- `userboot` is a raw binary copied and mapped by kernel-side bootstrap logic
- several platform-specific mechanisms remain tightly coupled to current boot assumptions
- the system is still designed around one fixed built-in runtime image set

The honest description is: IRIS is a capability-oriented microkernel project with a meaningful user-space service boundary, but it is not a fully purified, policy-minimized, production-hardened microkernel system.

## Maturity Assessment

IRIS is more serious than a classroom kernel because it already has:

- real service bootstrap
- user-space drivers/services
- capability transfer
- address-space separation
- diagnostics
- restart/supervision logic
- repeatable headless validation

IRIS is still experimental because it still lacks:

- broad hardware support
- persistent storage
- networking
- robustness under adversarial workloads
- larger-scale testing
- compatibility targets beyond its own ABI

The right framing today is:

- serious experimental OS work
- not production-ready
- architecturally coherent
- worth treating as a real systems project, not as a finished operating system
