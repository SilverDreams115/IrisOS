# IRIS OS

IRIS is an `x86_64` operating system with a capability-based microkernel design, UEFI boot, and a strict ring split: kernel in ring 0, services in ring 3.

The current healthy path is fully service-driven after the first bootstrap task:

- the kernel boots one opaque initrd image
- that image is currently `init`
- `init` spawns `svcmgr`
- `svcmgr` spawns and supervises `kbd` and `vfs`

No userland service code is linked into the kernel binary.

## Current Status

The current tree boots to a healthy interactive runtime and passes both default and selftest-enabled headless runtime validation.

Healthy-path runtime markers:

- `[SVCMGR] ready`
- `[USER][INIT][DIAG] reply`
- `[USER][INIT][BOOT] healthy path OK`

Selftest-enabled runtime additionally reaches:

- `[IRIS][P3] handle/lifecycle selftests OK`
- `[SVCMGR][DIAG] kbd status OK`

## What Works

- UEFI loader plus higher-half kernel bootstrap
- Per-process address spaces with isolated user mappings
- Capability handle tables with per-handle rights reduction
- Demand-paged VMOs with lazy backing on first fault
- `KChannel` IPC with attached-handle transfer
- `KNotification` wait/signal primitives
- `SYS_THREAD_CREATE` / `SYS_THREAD_EXIT`
- `SYS_FUTEX_WAIT` / `SYS_FUTEX_WAKE`
- Userland service bootstrap through `SYS_INITRD_LOOKUP` + `SYS_SPAWN_ELF`
- Userland service supervision and live lookup through `svcmgr`
- PS/2 keyboard service with IRQ routing delegated from `svcmgr`
- VFS service with boot exports, client tracking, and dead-client reclaim
- Consolidated diagnostics through `SYS_DIAG_SNAPSHOT` plus service `STATUS`/`DIAG`
- Headless runtime smoke in both default and selftest-enabled configurations

## Architecture

| Area | Current design |
|---|---|
| Target | `x86_64` |
| Boot | UEFI via `BOOTX64.EFI` |
| Kernel boundary | Ring 0 kernel, ring 3 services |
| Bootstrap policy | Kernel boots one opaque initrd image only |
| Current bootstrap image | `init` |
| Service manager | `svcmgr` |
| Runtime services | `init`, `svcmgr`, `kbd`, `vfs` |
| Memory model | 4-level paging, higher-half kernel, per-process user mappings |
| IPC | `KChannel` ring-buffer IPC |
| Scheduling | Timer-driven round-robin at 100 Hz |
| Fault handling | Userland faults notify optional exception handler, then kill faulting task |

Authority split:

| Responsibility | Owner |
|---|---|
| First bootstrap task selection | Kernel |
| Bootstrap cap injection | Kernel |
| Service spawn policy | `svcmgr` |
| Hardware cap creation policy | `svcmgr` |
| Service discovery | `svcmgr` |
| IRQ ownership policy | `svcmgr` |
| VFS namespace and session state | `vfs` |
| Keyboard I/O | `kbd` |

## Boot Flow

1. UEFI loads the kernel ELF and passes `boot_info`.
2. The kernel initializes PMM, paging, descriptors, interrupts, scheduler, syscalls, and initrd metadata.
3. The kernel resolves one opaque bootstrap image from the embedded initrd and spawns it.
4. The kernel injects one `KBootstrapCap` into that first task.
5. `init` resolves and spawns `svcmgr`.
6. `init` transfers bootstrap authority to `svcmgr`.
7. `svcmgr` requests hardware capabilities, spawns `kbd` and `vfs`, and supervises them.
8. `init` validates discovery, diagnostics, dynamic registry, VFS, and keyboard subscription before entering the echo loop.

## Capability Model

Core object types:

- `KOBJ_CHANNEL`: IPC endpoint with attached-handle transfer
- `KOBJ_VMO`: virtual memory object
- `KOBJ_NOTIFICATION`: signal/wait object
- `KOBJ_PROCESS`: process handle for lifecycle operations
- `KOBJ_IRQ_CAP`: IRQ routing authority
- `KOBJ_IOPORT`: I/O port authority
- `KOBJ_BOOTSTRAP_CAP`: bootstrap authority for spawn, hardware-cap creation, and privileged serial debug
- `KOBJ_INITRD_ENTRY`: immutable reference to an embedded ELF image

Important properties:

- rights are per-handle, not per-object
- `SYS_BOOTCAP_RESTRICT` narrows only the calling handle's bootstrap-cap authority
- attached handles crossing IPC are explicitly rights-reduced
- hardware access remains capability-gated in userland

Bootstrap-cap permission bits currently in use:

- `IRIS_BOOTCAP_SPAWN_SERVICE`
- `IRIS_BOOTCAP_HW_ACCESS`
- `IRIS_BOOTCAP_KDEBUG`

## Syscall Surface

The live syscall surface currently spans `0..51`.

Key groups:

- base task/syscall layer: `SYS_WRITE`, `SYS_EXIT`, `SYS_YIELD`, `SYS_SLEEP`
- IPC and handles: `SYS_CHAN_CREATE`, `SYS_CHAN_SEND`, `SYS_CHAN_RECV`, `SYS_CHAN_CALL`, `SYS_HANDLE_DUP`, `SYS_HANDLE_TRANSFER`, `SYS_HANDLE_CLOSE`
- memory objects: `SYS_VMO_CREATE`, `SYS_VMO_MAP`, `SYS_VMO_UNMAP`, `SYS_VMO_SHARE`
- notifications: `SYS_NOTIFY_CREATE`, `SYS_NOTIFY_SIGNAL`, `SYS_NOTIFY_WAIT`
- lifecycle: `SYS_PROCESS_SELF`, `SYS_PROCESS_STATUS`, `SYS_PROCESS_WATCH`, `SYS_PROCESS_KILL`
- threads and futexes: `SYS_THREAD_CREATE`, `SYS_THREAD_EXIT`, `SYS_FUTEX_WAIT`, `SYS_FUTEX_WAKE`
- bootstrap and hardware: `SYS_INITRD_LOOKUP`, `SYS_SPAWN_ELF`, `SYS_CAP_CREATE_IRQCAP`, `SYS_CAP_CREATE_IOPORT`, `SYS_IOPORT_RESTRICT`
- diagnostics: `SYS_DIAG_SNAPSHOT`, `SYS_EXCEPTION_HANDLER`, `SYS_WAIT_ANY`

Retired paths now intentionally return not-supported and are kept only for ABI continuity:

- `SYS_OPEN`, `SYS_READ`, `SYS_CLOSE`
- `SYS_BRK`
- `SYS_SPAWN`, `SYS_SPAWN_SERVICE`
- `SYS_NS_REGISTER`, `SYS_NS_LOOKUP`

See [kernel/include/iris/syscall.h](/home/silver/projects/IRIS/kernel/include/iris/syscall.h) for the canonical ABI.

## Service Protocols

`svcmgr`

- bootstrap handle delivery to child services
- endpoint lookup by numeric endpoint or published name
- runtime service publication and withdrawal
- status and aggregated diagnostics
- supervised restart for catalog services

`vfs`

- `OPEN`, `READ`, `CLOSE`
- `LIST` over the boot namespace
- `STATUS` for exports, open-file pressure, and bytes exported

`kbd`

- `HELLO` liveness probe
- `STATUS` health flags
- `SUBSCRIBE` for scancode event delivery

## Allocation And Limits

IRIS now mixes fixed structural limits with `kpage`-backed object allocation.

Still fixed by design:

- `TASK_MAX = 64`
- `HANDLE_TABLE_MAX = 1024`
- `KCHAN_CAPACITY = 128`
- `WAIT_ANY_MAX_CHANNELS = 64`
- `KPROCESS_EXIT_WATCH_MAX = 4`
- `KNOTIF_WAITERS_MAX = 4`
- `VFS_SERVICE_OPEN_FILES = 32`
- `FUTEX_TABLE_SIZE = 256`

No longer using static allocator ceilings:

- `KProcess`
- `KVmo`
- `KNotification`
- `KIrqCap`
- `KIoPort`
- `KBootstrapCap`
- `KInitrdEntry`

Notable recent structural changes:

- per-process VMO mappings are now a `kpage`-backed linked list, not a fixed `KPROCESS_VMO_MAP_MAX` array
- `kpage` records allocation metadata and rejects mismatched frees with a fatal diagnostic
- `svcmgr` runtime publication capacity is now bounded by the service-manager handle table, not by a local 16-entry registry cap

## Build And Validation

Build:

```bash
make
make check
```

Default runtime validation:

```bash
make smoke-runtime
```

Selftest-enabled runtime validation:

```bash
make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests
```

Interactive runtime:

```bash
make run
make run-headless
```

Local build smoke:

```bash
make smoke
```

## CI Lanes

Current CI covers:

- default build and ELF inspection
- default + selftest-enabled build smoke
- default headless runtime smoke
- selftest-enabled headless runtime smoke

The selftest runtime lane asserts both the final healthy-path marker and intermediate markers for the `DIAG` path:

- `[IRIS][P3] handle/lifecycle selftests OK`
- `[SVCMGR][DIAG] kbd status OK`
- `[USER][INIT][DIAG] reply`

## Repository Map

| Path | Role |
|---|---|
| `boot/uefi/boot.c` | UEFI loader |
| `kernel/kernel_main.c` | Kernel bootstrap and first-user-task spawn |
| `kernel/core/syscall/syscall.c` | Syscall dispatcher |
| `kernel/core/scheduler/` | Scheduler and task lifecycle |
| `kernel/core/irq/irq_routing.c` | IRQ routing mechanism |
| `kernel/core/initrd/` | Embedded initrd catalog |
| `kernel/core/loader/elf_loader.c` | ELF loading into new processes |
| `kernel/mm/kpage/` | `kpage` allocator |
| `kernel/new_core/` | Kernel object implementations |
| `services/init/` | Bootstrap validation service |
| `services/svcmgr/` | Userland service manager |
| `services/kbd/` | Keyboard service |
| `services/vfs/` | VFS service |
| `docs/contracts/` | Boot, service, and protocol contracts |

## Branches

| Branch | Role |
|---|---|
| `main` | promoted stable line |
| `silver` | active development |
| `staging` | integration and verification |

References:

- [docs/architecture/iris-v0.1.md](/home/silver/projects/IRIS/docs/architecture/iris-v0.1.md)
- [docs/testing.md](/home/silver/projects/IRIS/docs/testing.md)
- [docs/contracts/boot.md](/home/silver/projects/IRIS/docs/contracts/boot.md)
