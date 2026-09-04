# Syscall ABI Contract

## Purpose

Defines the current syscall compatibility surface implemented by the live IRIS tree.

This document is descriptive, not aspirational. If code and docs disagree, code wins until the docs are corrected.

## Register convention

Arguments travel in RDI, RSI, RDX and R10 (arg0..arg3); the syscall number is
in RAX.

A caller that passes fewer than four arguments **must still present a defined
R10 — zero.**  A syscall that later grows a fourth argument reads that
register, and what the compiler left in it is not zero.  This is not a
theoretical rule: it is how `SYS_INITRD_VMO`'s budget argument (Stage 6) broke
every three-argument caller until their stubs were fixed.  Zero has a defined
meaning wherever an argument has been added so far ("no destination", "my own
budget"), so a stub that zeroes R10 degrades to the previous behaviour instead
of resolving garbage.

## Memory budgets (Stage 6)

Two syscalls take a **budget**: a `KUntyped` CPtr, with `RIGHT_WRITE`, that the
memory they allocate is carved from.

| Syscall | Argument | Required? | What it pays for |
|---|---|---|---|
| `SYS_VMO_CREATE` | arg1 | no (0 = own budget) | the VMO's pages, page-address array and header |
| `SYS_INITRD_VMO` | arg3 | no (0 = own budget) | the private copy of the boot image |

`SYS_PROCESS_CREATE` used to be a third, and is not any more: Stage 6-pure made
the caller RETYPE what it used to buy, and Stage 7-proc retired the syscall
outright.  Everything else that allocates — a TCB, a CNode, a VSpace, a page
table — comes through `SYS_UNTYPED_RETYPE2`, which names its budget by
construction.

## Composed objects (Stage 6-pure)

The kernel does not create an address space, a paging level or a CSpace.  A
holder retypes each from its own Untyped and passes it in.

| Syscall | Arguments | What the caller supplies |
|---|---|---|
| `SYS_TCB_CONFIGURE` (120) | tcb, cspace, vspace | the CSpace root and the address space the thread runs in, both retyped by the caller.  This is `seL4_TCB_Configure`: since Stage 7-proc there is no identity check against a third object, and threads sharing a CSpace and a VSpace is what a "process" IS |
| `SYS_VSPACE_MAP_TABLE` (122) | pt, vspace, vaddr | an `IRIS_KOBJ_PAGE_TABLE` to fill the first level missing for `vaddr` |

A map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE` and names
nothing else — the holder supplies the level and retries.  A task that maps
therefore needs a budget to retype levels from, which is why `svc_load_minted_ws`
takes the slot to mint the child its own.

Everything carved this way is a **child** of that Untyped, so
`SYS_UNTYPED_RESET` refuses while it lives and reclaims the whole region once
it does not.  A bump allocator does not rewind: reclamation is by RESET, which
is why spawners recycle budgets rather than sizing them for a whole run.

## Error Model

The current syscall ABI target is:

- every syscall returns a signed long in the architectural ABI sense
- success is non-negative
- failure is a negative `iris_error_t`

Kernel implementation note:

- the dispatcher still moves return values through `uint64_t` internally
- that does not change the external contract; failure values must still encode as negative `iris_error_t`

## Surface Summary

Exported syscall number surface: **`0..127`** — 128 numbers, of which **69 are
live**, 56 are named in `syscall.h` and retired, and `9`–`11` were never
assigned.

Classification used here:

- **live**: dispatched to a real implementation
- **retired**: permanently reserved; the dispatcher either has no case or the
  implementation is a stub, and either way the caller gets
  `IRIS_ERR_NOT_SUPPORTED`.  **A retired number is never reused.**

The retirements are the convergence history in ABI form: the handle namespace
(Stage 4), the fabricating creators superseded by `SYS_UNTYPED_RETYPE2`
(Phases S1–S2), the KChannel family (Phase 13), the process surface (Stage 7),
and the early Unix-shaped calls that predate the capability model.

## Live Surface By Area

### Core, time and futex

`1` `SYS_EXIT`, `2` `SYS_GETPID`, `3` `SYS_YIELD`, `8` `SYS_SLEEP`, `49` `SYS_THREAD_EXIT`, `50` `SYS_FUTEX_WAIT`, `51` `SYS_FUTEX_WAKE`, `62` `SYS_CLOCK_GET`, `70` `SYS_CLOCK_NANOSLEEP`

### Endpoint IPC

`74` `SYS_EP_SEND`, `75` `SYS_EP_RECV`, `76` `SYS_EP_NB_SEND`, `77` `SYS_EP_NB_RECV`, `93` `SYS_EP_CALL`, `94` `SYS_REPLY`

### Notifications and faults

`20` `SYS_NOTIFY_SIGNAL`, `21` `SYS_NOTIFY_WAIT`, `64` `SYS_NOTIFY_WAIT_TIMEOUT`, `66` `SYS_EXCEPTION_RESUME`

### Capabilities and CSpace

`91` `SYS_CNODE_DELETE`, `92` `SYS_CNODE_SWAP`, `114` `SYS_CSPACE_MINT`, `115` `SYS_CSPACE_REVOKE`, `117` `SYS_CAP_IDENTIFY`, `118` `SYS_CAP_SAME_OBJECT`, `119` `SYS_CSPACE_SELF`, `127` `SYS_CSPACE_SET_GUARD`

### Untyped, memory and address spaces

`16` `SYS_VMO_CREATE`, `17` `SYS_VMO_MAP`, `36` `SYS_VMO_UNMAP`, `55` `SYS_INITRD_VMO`, `57` `SYS_VMO_MAP_INTO`, `67` `SYS_VMO_SIZE`, `86` `SYS_UNTYPED_INFO`, `88` `SYS_UNTYPED_RESET`, `102` `SYS_FRAME_MAP`, `103` `SYS_FRAME_UNMAP`, `106` `SYS_VSPACE_SELF`, `108` `SYS_VMO_MAP_PAGE`, `111` `SYS_UNTYPED_RETYPE2`, `112` `SYS_UNTYPED_QUERY`, `122` `SYS_VSPACE_MAP_TABLE`

### Threads (the whole of what a process used to be)

`82` `SYS_THREAD_PRIORITY`, `85` `SYS_THREAD_SET_SC`, `96` `SYS_TCB_SELF`, `97` `SYS_TCB_SUSPEND`, `98` `SYS_TCB_RESUME`, `99` `SYS_TCB_SET_PRIORITY`, `100` `SYS_TCB_EXIT`, `101` `SYS_TCB_GET_INFO`, `120` `SYS_TCB_CONFIGURE`, `121` `SYS_TCB_WRITE_REGS`, `123` `SYS_TCB_FAULT_INFO`, `124` `SYS_TCB_WATCH`, `125` `SYS_TCB_EXIT_CODE`, `126` `SYS_TCB_SET_FAULT_HANDLER`

### Scheduling

`69` `SYS_SCHED_INFO`, `84` `SYS_SC_CONFIGURE`, `113` `SYS_SC_BIND`

### Hardware and bootstrap (capability-gated)

`27` `SYS_IRQ_ROUTE_REGISTER`, `32` `SYS_IOPORT_IN`, `33` `SYS_IOPORT_OUT`, `39` `SYS_CAP_CREATE_IRQCAP`, `40` `SYS_CAP_CREATE_IOPORT`, `54` `SYS_POWEROFF`, `60` `SYS_FRAMEBUFFER_VMO`, `61` `SYS_INITRD_COUNT`, `65` `SYS_KLOG_DRAIN`, `68` `SYS_IRQ_ACK`

### Retired numbers

Never reused, always `IRIS_ERR_NOT_SUPPORTED`:

`0`, `4`, `5`, `6`, `7`, `12`, `13`, `14`, `15`, `18`, `19`, `22`, `23`, `24`, `25`, `26`, `28`, `29`, `30`, `31`, `34`, `35`, `37`, `38`, `41`, `42`, `43`, `44`, `45`, `46`, `47`, `48`, `52`, `53`, `56`, `58`, `59`, `63`, `71`, `72`, `73`, `78`, `79`, `80`, `81`, `83`, `87`, `89`, `90`, `95`, `104`, `105`, `107`, `109`, `110`, `116`

`9`, `10` and `11` were never assigned.

## Current Architectural Reading

The live syscall surface reflects the current architecture:

- file I/O is not a kernel syscall surface anymore
- service discovery is not a kernel namespace syscall surface anymore
- ELF loading is not a kernel spawn syscall surface anymore
- **process construction is not a syscall at all**: a ring-3 loader retypes a
  VSpace, a root CNode and a TCB from a budget it holds, configures the thread
  with the first two, writes its registers and resumes it
- accounting is not a syscall about a process either — a budget answers for
  itself (`SYS_UNTYPED_INFO` / `SYS_UNTYPED_QUERY`)
- hardware access remains capability-gated

## Top Hardening-Risk Families

These syscall families carry the highest near-term hardening risk and should be audited first:

1. endpoint send/recv/call/reply paths, including staged capability transfer
2. VMO map/unmap/map-into/map-page paths
3. retype, configure and resume — the thread-construction path
4. notification wait and timed wait paths
5. `SYS_KLOG_DRAIN`, `SYS_UNTYPED_QUERY` and other user-buffer write-back paths

## Canonical Sources

- `kernel/include/iris/syscall.h` (numbers, contracts and retirement notes)
- `kernel/core/syscall/syscall_dispatch.c` (what is actually dispatched)
- `kernel/include/iris/svcmgr_proto.h`
- `kernel/include/iris/vfs_ep_proto.h` (replaced `vfs_proto.h`, removed in Phase 7.5)
- `kernel/include/iris/kbd_proto.h` (legacy probes) + `kbd_ep_proto.h` (event ABI, Phase 7.4)
- `kernel/include/iris/console_proto.h` (legacy writer) + `console_ep_proto.h` (EP ABI, Phase 7.3)
- `kernel/include/iris/endpoint_proto.h` (endpoint/bootstrap-kind/CPtr ABI)
