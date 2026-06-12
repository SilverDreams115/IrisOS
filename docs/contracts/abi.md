# Syscall ABI Contract

## Purpose

Defines the current syscall compatibility surface implemented by the live IRIS tree.

This document is descriptive, not aspirational. If code and docs disagree, code wins until the docs are corrected.

## Error Model

The current syscall ABI target is:

- every syscall returns a signed long in the architectural ABI sense
- success is non-negative
- failure is a negative `iris_error_t`

Kernel implementation note:

- the dispatcher still moves return values through `uint64_t` internally
- that does not change the external contract; failure values must still encode as negative `iris_error_t`

## Surface Summary

Current exported syscall number surface: `0..67`.

Classification used here:

- live/conforming: current supported surface on the v1 error model
- live/transitional: current supported surface with compatibility notes
- retired: permanently reserved and returns `IRIS_ERR_NOT_SUPPORTED`

## Retired Numbers

The following syscall numbers remain reserved for ABI continuity and are intentionally retired:

- `0` `SYS_WRITE`
- `4` `SYS_OPEN`
- `5` `SYS_READ`
- `6` `SYS_CLOSE`
- `7` `SYS_BRK`
- `18` `SYS_SPAWN`
- `24` `SYS_NS_REGISTER`
- `25` `SYS_NS_LOOKUP`
- `30` `SYS_DIAG_SNAPSHOT` (retired Phase 51)
- `31` `SYS_SPAWN_SERVICE`
- `41` `SYS_INITRD_LOOKUP`
- `42` `SYS_SPAWN_ELF`

## Live Surface By Area

### Base task control

- `1` `SYS_EXIT`
- `2` `SYS_GETPID`
- `3` `SYS_YIELD`
- `8` `SYS_SLEEP`
- `62` `SYS_CLOCK_GET`

### Channels and handle movement

- `12` `SYS_CHAN_CREATE`
- `13` `SYS_CHAN_SEND`
- `14` `SYS_CHAN_RECV`
- `15` `SYS_HANDLE_CLOSE`
- `22` `SYS_HANDLE_DUP`
- `23` `SYS_HANDLE_TRANSFER`
- `34` `SYS_CHAN_RECV_NB`
- `37` `SYS_CHAN_SEAL`
- `38` `SYS_CHAN_CALL`
- `44` `SYS_WAIT_ANY`
- `52` `SYS_HANDLE_TYPE`
- `53` `SYS_HANDLE_SAME_OBJECT`
- `63` `SYS_CHAN_RECV_TIMEOUT`

### Memory objects and address-space operations

- `16` `SYS_VMO_CREATE`
- `17` `SYS_VMO_MAP`
- `36` `SYS_VMO_UNMAP`
- `46` `SYS_VMO_SHARE`
- `55` `SYS_INITRD_VMO`
- `57` `SYS_VMO_MAP_INTO`
- `60` `SYS_FRAMEBUFFER_VMO`
- `61` `SYS_INITRD_COUNT`
- `67` `SYS_VMO_SIZE`

### Notifications and futexes

- `19` `SYS_NOTIFY_CREATE`
- `20` `SYS_NOTIFY_SIGNAL`
- `21` `SYS_NOTIFY_WAIT`
- `50` `SYS_FUTEX_WAIT`
- `51` `SYS_FUTEX_WAKE`
- `64` `SYS_NOTIFY_WAIT_TIMEOUT`

### Process, threads, and lifecycle

- `26` `SYS_PROCESS_STATUS`
- `27` `SYS_IRQ_ROUTE_REGISTER`
- `28` `SYS_PROCESS_SELF`
- `29` `SYS_PROCESS_WATCH`
- `35` `SYS_PROCESS_KILL`
- `47` `SYS_EXCEPTION_HANDLER`
- `48` `SYS_THREAD_CREATE`
- `49` `SYS_THREAD_EXIT`
- `56` `SYS_PROCESS_CREATE`
- `58` `SYS_THREAD_START`
- `59` `SYS_HANDLE_INSERT`
- `66` `SYS_EXCEPTION_RESUME`

### Bootstrap and hardware capabilities

- `32` `SYS_IOPORT_IN`
- `33` `SYS_IOPORT_OUT`
- `39` `SYS_CAP_CREATE_IRQCAP`
- `40` `SYS_CAP_CREATE_IOPORT`
- `43` `SYS_IOPORT_RESTRICT`
- `45` `SYS_BOOTCAP_RESTRICT`
- `54` `SYS_POWEROFF`

### Diagnostics

- `65` `SYS_KLOG_DRAIN`

## Current Architectural Reading

The live syscall surface reflects the current architecture:

- file I/O is not a kernel syscall surface anymore
- service discovery is not a kernel namespace syscall surface anymore
- ELF loading is not a kernel spawn syscall surface anymore
- process construction is exposed as composable primitives for ring-3 loaders
- hardware access remains capability-gated

## Top Hardening-Risk Families

These syscall families carry the highest near-term hardening risk and should be audited first:

1. channel receive/send/call/timeout paths
2. VMO map/unmap/map-into paths
3. process/thread start and handle insertion paths
4. notification wait and timed wait paths
5. `SYS_KLOG_DRAIN` and other user-buffer write-back paths

## Canonical Sources

- `kernel/include/iris/syscall.h`
- `kernel/core/syscall/syscall.c`
- `kernel/include/iris/svcmgr_proto.h`
- `kernel/include/iris/vfs_ep_proto.h` (replaced `vfs_proto.h`, removed in Fase 7.5)
- `kernel/include/iris/kbd_proto.h` (legacy probes) + `kbd_ep_proto.h` (event ABI, Fase 7.4)
- `kernel/include/iris/console_proto.h` (legacy writer) + `console_ep_proto.h` (EP ABI, Fase 7.3)
- `kernel/include/iris/endpoint_proto.h` (endpoint/bootstrap-kind/CPtr ABI)
