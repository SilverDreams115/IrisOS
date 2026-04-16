# Boot contract

## Purpose

Defines the current healthy-path boot sequence from UEFI handoff to the first userland validation loop.

## Entry contract

- The UEFI loader transfers control to `iris_kernel_main(struct iris_boot_info *boot_info)`.
- `boot_info` must be non-null and `boot_info->magic` must equal `IRIS_BOOTINFO_MAGIC`.
- The kernel expects `IRIS_BOOTINFO_VERSION == 2`.
- The boot info contains:
  - framebuffer base/size/geometry
  - bounded memory map (`IRIS_MMAP_MAX_ENTRIES`)

## Kernel boot stages

Current stage order in `kernel/kernel_main.c`:

1. Serial init and banner.
2. Boot info validation and local copy into `saved_boot_info`.
3. PMM initialization from the firmware memory map.
4. Paging initialization, including framebuffer mapping.
5. GDT, PIC/PIT, and IDT initialization.
6. Framebuffer driver init and visual paint.
7. Syscall layer init.
8. IRQ routing table init.
9. Optional runtime selftests when `ENABLE_RUNTIME_SELFTESTS=1`.
10. Scheduler init.
11. `svcmgr` bootstrap.
12. `init` bootstrap from the embedded initrd.
13. `sti`, scheduler start, and initial cooperative yields.

## Embedded initrd contract

The healthy boot path requires these embedded ELF images to exist in the kernel initrd:

- `svcmgr`
- `kbd`
- `vfs`
- `init`

The initrd is a static, read-only registry compiled into the kernel image via `objcopy -I binary`.

## Ring-3 bootstrap register contract

For spawned ring-3 tasks:

- bootstrap `arg0` is delivered in `RBX` on first user entry
- the same value is also mirrored at `USER_STACK_TOP - 8` as a transitional compatibility path

Service entrypoints currently consume `RBX` by moving it to `RDI` before calling their C entry function.

## Healthy-path boot invariants

- Healthy boot does not require kernel nameserver lookup for `svcmgr`.
- The kernel boots only the supervisor path explicitly:
  - spawn `svcmgr`
  - attach narrow bootstrap client handle to `init`
- `svcmgr` autostarts `kbd` and `vfs` from the declarative service catalog.
- `init` validates service reachability and health before entering its interactive loop.

## Current healthy-path runtime signature

The current healthy boot path is considered valid when all of these hold:

- `svcmgr` starts and logs ready
- `init` successfully looks up `kbd`, `kbd.reply`, `vfs`, and `vfs.reply`
- `init` receives a successful `KBD_MSG_HELLO_REPLY`
- `init` receives a successful `SVCMGR_MSG_DIAG_REPLY` and validates its fields
- `init` completes VFS `LIST`, `OPEN`, `READ`, and `CLOSE` checks
- `init` subscribes to keyboard events and enters the echo loop

Current log signature to search for:

- kernel handoff start:
  - `[IRIS][BOOT] handoff: kernel -> svcmgr/init`
- first-wave scheduling:
  - `[IRIS][BOOT] waiting for first userland wave`
- final healthy-path confirmation from `init`:
  - `[USER][INIT][BOOT] healthy path OK`

## Non-goals of this contract

- This document does not define a stable boot ABI for third-party loaders.
- This document does not claim CI-grade runtime determinism yet.
- This document does not freeze internal log strings as public API.
