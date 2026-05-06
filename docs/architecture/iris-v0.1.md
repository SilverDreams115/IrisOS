# IRIS architecture baseline

This document tracks the current architectural baseline implemented in-tree.
It is intentionally concise and must stay aligned with the live code.

## System shape

- Target: `x86_64`
- Boot: `UEFI`
- Privilege split:
  - kernel in ring 0
  - all services in ring 3
- Image model:
  - the kernel embeds service ELFs in the initrd
  - the kernel boots one opaque bootstrap image
  - the current healthy path uses `init` as that bootstrap image

## Boot flow

1. The UEFI loader loads the kernel ELF and passes `boot_info`.
2. The kernel initializes memory management, interrupts, scheduling, syscalls, and initrd metadata.
3. The kernel resolves the bootstrap image from the initrd catalog and spawns it as the first user task.
4. The kernel injects one `KBootstrapCap` with:
   - handle rights: `RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER`
   - permission bits: `IRIS_BOOTCAP_SPAWN_SERVICE | IRIS_BOOTCAP_HW_ACCESS | IRIS_BOOTCAP_KDEBUG`
5. `init` looks up and spawns `svcmgr`.
6. `init` transfers a bootstrap-cap handle to `svcmgr`.
7. `svcmgr` creates hardware capabilities, spawns `kbd` and `vfs`, and then narrows its own bootstrap-cap handle to drop `HW_ACCESS`.

## Authority model

- The kernel retains mechanism, not service policy.
- `svcmgr` owns:
  - built-in service spawn policy
  - runtime service publication and lookup
  - hardware-cap distribution to child services
  - supervised restart for catalog services
- Rights are per-handle, not per-object.
- `SYS_BOOTCAP_RESTRICT` now narrows only the calling handle's bootstrap authority; other aliases are unaffected.

## Core kernel objects

- `KChannel`: bounded IPC queue with attached-handle transfer
- `KVmo`: demand-paged virtual memory object
- `KNotification`: event object with signal bits and waiter slots
- `KProcess`: address space, handle table, and thread ownership
- `KIrqCap`: IRQ routing authority
- `KIoPort`: I/O port authority, further narrowable with `SYS_IOPORT_RESTRICT`
- `KBootstrapCap`: bootstrap authority for spawn, hardware-cap creation, and privileged serial debug
- `KInitrdEntry`: immutable reference to a boot image in the initrd catalog

## Allocation model

- `KChannel` remains pool-backed.
- `KProcess`, `KVmo`, `KNotification`, `KIrqCap`, `KIoPort`, `KBootstrapCap`, and `KInitrdEntry` now allocate from `kpage`.
- `kpage` records allocation metadata and rejects mismatched frees with a fatal diagnostic instead of silent undefined behaviour.
- Per-process VMO mappings now use a `kpage`-backed linked list instead of a fixed `KPROCESS_VMO_MAP_MAX` array.

## Observable health path

A healthy runtime should eventually emit:

- `[SVCMGR] ready`
- `[USER][INIT][BOOT] healthy path OK`

The kernel portion of the health snapshot is exposed through `SYS_DIAG_SNAPSHOT`.
Service-local health remains exposed through service `STATUS`/`DIAG` protocols.
