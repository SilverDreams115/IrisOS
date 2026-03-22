# IRIS OS

IRIS is a custom x86_64 operating system built from scratch with UEFI boot, explicit kernel/user separation, a syscall boundary, ring 3 execution, and an in-progress transition toward a capability-based microkernel architecture.

The project is no longer just a "kernel base" experiment. It now has enough integrated pieces to boot, create user processes, expose kernel objects through handles, publish bootstrap services, and consume a first service from userland.

---

## Current Status

**Hybrid transition toward microkernel**

IRIS currently boots a monolithic kernel core with several services still inside the kernel, but it already includes:

- ring 3 user tasks
- per-process address spaces
- process-owned private user mappings
- a capability-oriented object model
- handle tables
- channel-based IPC
- notification objects
- VMOs with user mappings
- process objects
- nameserver bootstrap
- a first user-space keyboard server
- userland lookup of the `kbd` service through the nameserver

This means the project is in a **hybrid integration phase**:

- the low-level kernel base is already established
- the microkernel core is already partially real
- the main work now is stabilization, integration, and gradual service extraction

---

## Implemented Subsystems

| Area | Status | Notes |
|------|--------|-------|
| UEFI boot | Working | `BOOTX64.EFI` loads the ELF kernel and passes `iris_boot_info` |
| PMM | Working | Bitmap allocator over the firmware memory map |
| Paging | Working | 4-level x86_64 paging with user/kernel split |
| GDT/TSS/IDT | Working | Ring transitions, exceptions, IRQ dispatch |
| PIT + scheduling | Working | Timer-driven round-robin with explicit blocking |
| Legacy IPC | Working | Producer/consumer demo still present for kernel validation |
| Framebuffer | Working | GOP framebuffer fill + rectangle drawing |
| VFS + ramfs | Working | Still kernel-resident; not yet externalized |
| PCI | Working | Config-space scan and device classification |
| Keyboard IRQ path | Working | IRQ1 routed into a `KChannel` for a user-space server |
| Ring 3 userland | Working | `user_init` runs in user mode |
| Syscalls | Working | File, memory, IPC, notification, process, handle, nameserver paths present with stricter validation in core paths |
| Capability core | Working | `KObject`, `KChannel`, `KProcess`, `KNotification`, `KVmo`, `HandleTable`, rights |
| Nameserver bootstrap | Working | Kernel initializes nameserver and registers `kbd` |
| First userland service path | Working | `user_init` does `NS_LOOKUP("kbd")` and sends a handshake |

---

## Microkernel State

IRIS is best described today as:

- **not** a pure monolithic kernel anymore
- **not yet** a fully service-oriented microkernel system
- **already** a capability-based hybrid core with real user-space service bootstrap

What is already true:

- process ownership is separated from thread execution state
- process-scoped handles live in `KProcess`
- process teardown and final destroy are split explicitly
- exited user processes release handle tables, user stack, heap pages, CR3, and private page tables
- threads/tasks carry scheduler state and CPU context
- channels are kernel objects referenced through handles
- notifications use an explicit single-waiter policy
- channels and notifications now expose observable close semantics for blocked waiters
- `sys_spawn` rolls back partial children instead of publishing ambiguous state
- `sys_vmo_map` and `sys_brk` use observable mapping failure paths in critical syscall code
- user/kernel pointer validation now checks `PAGE_USER` and `PAGE_WRITABLE` per page where applicable
- nameserver lookup inserts fresh handles into the caller's table
- bootstrap services can be published and discovered by name

What is still pending:

- broader migration of services out of the kernel
- a cleaner root-handle/capability structure beyond the current bootstrap arg0 path
- more formal service protocols beyond the first minimal request/response contracts
- continued hardening of syscall/error coherence outside the already-fixed hot paths

---

## Architecture

- **Target:** x86_64
- **Boot:** UEFI (`BOOTX64.EFI`)
- **Execution model:** kernel in ring 0, user processes in ring 3
- **Memory model:** 4-level paging with separate user CR3 per process
- **Kernel style:** hybrid kernel transitioning toward capability-based microkernel
- **Scheduling:** timer-driven round-robin with blocking for IPC, notifications, and sleep
- **IPC:** `KChannel` objects accessed through handle tables
- **Process model:** `KProcess` owns address space, heap break, and handle table
- **Thread/task model:** `struct task` owns execution context, stack, and scheduler state
- **Bootstrap discovery:** kernel nameserver publishes initial services such as `kbd`

### Userland Bootstrap Handle Contract

The current ring-3 bootstrap contract for an initial handle/arg0 is:

- primary delivery path: `arg0` enters the task in `%rbx` on first user entry
- compatibility path: the same value is mirrored at `USER_STACK_TOP-8`
- installation path: the kernel uses `task_set_bootstrap_arg0(...)` to set both forms together

This is intentional:

- `%rbx` is the authoritative bootstrap path for current user services such as `kbd_server`
- the stack mirror remains only as compatibility during transition
- kernel code should not patch bootstrap registers/stacks manually in ad hoc call sites

---

## Virtual Address Space Layout

```text
0x0000000000000000 - 0x0000000003FFFFFF  shared low kernel window
0x0000008000200000                    user text base
0x0000008000400000                    user heap base
0x0000008040000000                    user heap max
0x0000008050000000                    user VMO mapping window start
0x00000080FFFF7000                    user stack base
0x00000080FFFFF000                    user stack top

0xFFFF800000000000                    kernel physmap base
0xFFFFFFFF80200000                    kernel text base
0xFFFFFFFF80205000                    kernel data/bss area
```

Notes:

- the lower shared window is kernel-only and shared across CR3s
- user text, heap, VMOs, and stack live in the process-private window
- user stacks are mapped in the process page table, not borrowed from kernel BSS
- kernel mappings are not exported to ring 3 with `PAGE_USER`
- syscall-side user pointer validation rejects merely-present kernel-only mappings
- framebuffer/MMIO remain kernel-only

---

## Repository Structure

```text
boot/uefi/boot.c                          UEFI loader
kernel/kernel_main.c                      Kernel bootstrap and subsystem bring-up
kernel/arch/x86_64/                       x86_64 boot, paging, traps, syscall entry
kernel/core/scheduler/scheduler.c         Timer-driven scheduler and sleep wakeups
kernel/core/syscall/syscall.c             Syscall dispatcher and syscall implementations
kernel/core/irq/irq_routing.c             IRQ -> KChannel routing
kernel/core/nameserver/nameserver.c       Bootstrap nameserver
kernel/arch/x86_64/kbd_server.S           First user-space service
kernel/arch/x86_64/user_init.S            First user-space init path
kernel/new_core/include/iris/nc/          Capability core headers
kernel/new_core/src/                      KObject/KChannel/KProcess/HandleTable implementations
kernel/fs/ramfs/                          Current in-kernel filesystem implementation
kernel/drivers/                           Serial, framebuffer, PCI, keyboard drivers
```

---

## Build & Run

```bash
make clean
make
make check
make run
```

`make run` launches IRIS in QEMU with OVMF.

---

## Syscall Surface

Current syscall numbers exposed in `kernel/include/iris/syscall.h`:

| Number | Name | Description |
|--------|------|-------------|
| 0 | `SYS_WRITE` | Print a user string to serial |
| 1 | `SYS_EXIT` | Terminate current task |
| 2 | `SYS_GETPID` | Return current task id |
| 3 | `SYS_YIELD` | Yield CPU |
| 4 | `SYS_OPEN` | VFS open |
| 5 | `SYS_READ` | VFS read |
| 6 | `SYS_CLOSE` | VFS close |
| 7 | `SYS_BRK` | Heap break management |
| 8 | `SYS_SLEEP` | Sleep by scheduler ticks |
| 12 | `SYS_CHAN_CREATE` | Create channel object |
| 13 | `SYS_CHAN_SEND` | Send `KChanMsg` through a channel handle |
| 14 | `SYS_CHAN_RECV` | Receive `KChanMsg` through a channel handle |
| 15 | `SYS_HANDLE_CLOSE` | Close a handle |
| 16 | `SYS_VMO_CREATE` | Create VMO |
| 17 | `SYS_VMO_MAP` | Map VMO into caller address space with range/rights validation |
| 18 | `SYS_SPAWN` | Spawn user process with rollback-safe bootstrap channel setup |
| 19 | `SYS_NOTIFY_CREATE` | Create notification object |
| 20 | `SYS_NOTIFY_SIGNAL` | Signal notification |
| 21 | `SYS_NOTIFY_WAIT` | Wait on notification with explicit single-waiter behavior and `RIGHT_WAIT` |
| 22 | `SYS_HANDLE_DUP` | Duplicate handle with reduced rights |
| 23 | `SYS_HANDLE_TRANSFER` | Move handle into another process |
| 24 | `SYS_NS_REGISTER` | Register named service |
| 25 | `SYS_NS_LOOKUP` | Look up named service and receive handle |

---

## Runtime Behavior Verified

Recent validation on the current tree confirms:

- the kernel boots to the Stage 13 banner
- nameserver initializes during bootstrap
- the `kbd` service is registered in the nameserver
- the `kbd.reply` service is registered in the nameserver
- `user_init` runs in ring 3
- `user_init` successfully looks up `kbd`
- `user_init` successfully looks up `kbd.reply`
- `kbd_server` receives `KBD_OP_HELLO` and replies correctly
- `kbd_server` receives `KBD_OP_GET_STATUS` and replies correctly
- `sys_exit` follows the common exit path and completes process teardown
- blocked channel/notification waiters can now observe remote close with `IRIS_ERR_CLOSED`
- producer/consumer IPC continues running while userland and services are alive

Representative serial output:

```text
[IRIS][NS] initializing...
[IRIS][NS] bootstrap registry ready
[IRIS][KBD-SRV] keyboard server spawned, id=3, handle=1024
[IRIS][NS] registered service 'kbd'
[IRIS][NS] registered service 'kbd.reply'
[IRIS][USER] init task created, id=4
[USER] KBD start
[USER] KBD reply OK
[USER] init bootstrap start
[USER] kbd lookup OK
[USER] kbd.reply lookup OK
[USER] KBD recv
[USER] KBD bootstrap OK
[USER] kbd hello reply OK
[USER] KBD recv
[USER] KBD status OK
[USER] kbd status reply OK
```

---

## Roadmap

Short-term priorities:

1. formalize the first service protocol beyond a bootstrap handshake
2. keep stabilizing process/thread lifecycle and resource cleanup
3. improve bootstrap root capability structure beyond the current arg0 contract
4. migrate additional small services out of the kernel
5. leave VFS migration for a later, cleaner phase

Non-goals for the immediate next step:

- no cosmetic "microkernelization"
- no large scheduler rewrite
- no premature VFS externalization

---

## Branch Strategy

| Branch | Purpose |
|--------|---------|
| `silver` | Active development |
| `staging` | Integration and verification |
| `main` | Promoted stable line |
| `collab` | Collaboration branch |
