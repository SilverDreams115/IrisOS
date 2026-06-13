# IRIS

IRIS is an `x86_64` operating system built around a **capability-based, seL4-inspired microkernel**. It runs real user-space services, enforces hardware and process isolation through an unforgeable capability system, and boots to an interactive shell entirely from ring-3 code after the first kernel handoff.

The kernel exposes a small set of typed kernel objects (endpoints, CNodes, reply objects, untyped memory, frames, TCBs, scheduling contexts, …). Everything above the kernel — service discovery, supervision, the filesystem, the keyboard driver, the console, and the shell — lives in ring 3 and talks over **synchronous endpoint IPC** with kernel-stamped sender identity.

## Boot Sequence

```
UEFI → BOOTX64.EFI → KERNEL.ELF
  kernel: PMM (buddy), 4-level paging + PCID, GDT/IDT/PIC/PIT, LAPIC,
          scheduler, syscall/sysret dispatch, capability enforcement, initrd catalog
    → userboot (ring-3 flat binary, injected KBootstrapCap)
      → init            (orchestration, CPtr-first mint handoff, boot self-tests)
        → fb            (framebuffer painter, fire-and-forget)
        → console       (serial output service, endpoint)
        → svcmgr        (service manager + supervisor, endpoint-first)
          → kbd         (PS/2 keyboard driver; endpoint + IRQ KNotification)
          → vfs         (boot-namespace filesystem; endpoint-only)
          → sh          (interactive shell; pure CPtr-first client)
```

Every service from `init` onward is a ring-3 ELF loaded by `svc_loader` using only kernel primitives. The kernel spawns nothing after the initial `userboot` task.

## Capability & Object Model

Capabilities are unforgeable references to typed kernel objects. Rights are stored **per capability**, not per object, and can only be *reduced* on copy/mint/transfer, never elevated.

| Object | Role |
|--------|------|
| `KOBJ_ENDPOINT` | Synchronous rendezvous IPC (seL4-style). Primary service transport. |
| `KOBJ_REPLY` | One-shot reply capability created by `EP_CALL`; consumed by `SYS_REPLY`. |
| `KOBJ_CNODE` | Capability storage node; a process's CSpace is a tree of CNodes. |
| `KOBJ_UNTYPED` | Untyped memory; retyped into other kernel objects (`SYS_UNTYPED_RETYPE`). |
| `KOBJ_TCB` | Thread control block (suspend/resume/priority/info). |
| `KOBJ_SCHED_CONTEXT` | Scheduling context (budget/period) bound to a TCB. |
| `KOBJ_FRAME` | Physical frame capability; mapped into a VSpace. |
| `KOBJ_VSPACE` | Address space object (CR3 + PCID). |
| `KOBJ_VMO` | Memory object; sparse (eagerly populated at map time) or MMIO wrap. |
| `KOBJ_NOTIFICATION` | Lightweight signal/wait; used for IRQ delivery. |
| `KOBJ_PROCESS` | Process container (address space + handle table + CSpace root). |
| `KOBJ_IRQ_CAP` / `KOBJ_IOPORT` | Capability-gated hardware access. |
| `KOBJ_BOOTSTRAP_CAP` | First-task authority with per-bit permission flags. |
| `KOBJ_INITRD_ENTRY` | Read-only handle to an initrd image slot. |
| `KOBJ_CHANNEL` | Legacy ring-buffer IPC; retained as a test/compat & bootstrap boundary. |

### Rights

`RIGHT_READ`, `RIGHT_WRITE`, `RIGHT_DUPLICATE`, `RIGHT_TRANSFER`, `RIGHT_ROUTE`, `RIGHT_MANAGE`. A dup/mint/transfer computes `effective = source_rights & requested`; rights that collapse to `RIGHT_NONE` are rejected. `ACCESS_DENIED` is a hard stop with no fallback.

### CPtr-first addressing & the namespace split

Services are handed their well-known capabilities **before they start**, minted directly into their root CNode (`SYS_PROC_CSPACE_MINT`), and invoke them by CPtr — e.g. `SYS_EP_CALL(IRIS_CPTR_SVCMGR_EP, &msg)` — with no handle transfer. CPtrs and handle IDs share one argument namespace, split and enforced by the kernel:

- **value `< 1024`** → resolved through the **CSpace only** (no handle-table fallback; a missing slot fails cleanly).
- **value `>= 1024`** → resolved through the **handle table only** (handle IDs are `slot | generation<<10`, generation ≥ 1).

Well-known child slots: `1` svcmgr EP, `2` vfs EP, `3` console EP, `4` kbd EP, `5` own EP (recv), `7` IRQ notification.

## Endpoint IPC

Synchronous rendezvous: a client `SYS_EP_CALL`s and blocks; a server loops on `SYS_EP_RECV` and answers with `SYS_REPLY`. `SYS_EP_SEND` / `SYS_EP_NB_SEND` / `SYS_EP_NB_RECV` cover the async and non-blocking variants. The wire message is `struct IrisMsg` (80 bytes): a label, four inline words, an optional bulk buffer, and **two capability slots**.

- **Reply capability** — `EP_CALL` creates a one-shot `KReply`; the server receives it in `attached_handle` and consumes it with `SYS_REPLY`. Replying twice fails.
- **Transferred capability** — the client may move/copy a capability to the server in `attached_cap` (+ `attached_cap_rights`). The kernel *stages* it (the caller must really hold it; rights reduced) and the server receives it as a real handle. The reply cap and the transferred cap never collide, so an `EP_CALL` can carry both.

A capability can never be forged from the payload: the kernel clears `attached_cap`/`sender_badge` written by the client and delivers only real, kernel-installed handles.

### Sender identity — badges

Every endpoint message carries a **kernel-stamped badge** (`IrisMsg.sender_badge`) identifying the *capability* the sender invoked — taken from the cap, never from the payload, so it cannot be spoofed. Badges are per-capability metadata assigned at mint time (`SYS_PROC_CSPACE_MINT` packs `rights | badge<<32`); a badged cap can **never be re-badged** (no identity forging). Two caps to the same endpoint can carry different badges. Replies force `sender_badge = 0`. Servers use the badge to authenticate clients (`iris_badge_is_supervisor()` gates privileged operations).

## Service Lifecycle

`svcmgr` supervises catalog services with real death/restart and a generation model:

- **Death → respawn**: `SYS_PROCESS_WATCH` reports a service exit; svcmgr respawns it (per-service restart budget) and **bumps its generation**. Endpoint masters persist across restart so client caps stay valid.
- **Liveness oracle**: `IRIS_SVCMGR_EP_STATUS` returns `{alive, generation}` — a non-blocking way for a client to poll a restart without hanging on a dead endpoint.
- **Client relookup**: on a generation change a client re-looks-up the service to obtain a fresh cap.
- **Logical revocation**: a cached generation older than the current one is *stale*; the supervisor's registry is the source of truth.

## `svcmgr` — endpoint-first API

The productive svcmgr API is entirely endpoint-based (`SYS_EP_CALL` to the svcmgr endpoint), badge-authenticated:

| Opcode | Operation |
|--------|-----------|
| `IRIS_SVCMGR_EP_LOOKUP_NAME` | Resolve `<name>` / `<name>.ep`; reply carries the endpoint cap. |
| `IRIS_SVCMGR_EP_REGISTER` | Cap-backed registration: client transfers an endpoint in `attached_cap`; owner = sender badge. |
| `IRIS_SVCMGR_EP_UNREGISTER` | Owner-badge (or supervisor) checked. |
| `IRIS_SVCMGR_EP_STATUS` | `{alive, generation}` for a service. |
| `IRIS_SVCMGR_EP_RESTART` | Supervisor-only kill + watch-driven respawn. |
| `IRIS_SVCMGR_EP_DIAG` | Snapshot: catalog count / ready / active dynamic / catalog version. |
| `IRIS_EP_OP_PING` | Health check; echoes the observed sender badge. |

Reserved names (`*.ep`, catalog names) are never runtime-registrable. `.ep` lookups grant `RIGHT_WRITE` to ordinary clients; `DUPLICATE`/`TRANSFER` is reserved for supervisor badges. Unknown opcodes fail with `INVALID_ARG` — there is no silent fallback to the legacy path.

The legacy `KChannel` `SVCMGR_MSG_*` loop survives only as a **compatibility/test boundary** (init bootstrap re-mint + boot self-tests); `SVCMGR_MSG_STATUS` and dead bootstrap branches were retired in Fase 12.

## Syscall Surface

105 syscall slots (0–104). Highlights by area:

- **Core / process / thread**: `EXIT`, `GETPID`, `YIELD`, `SLEEP`, `CLOCK_GET`, `CLOCK_NANOSLEEP`, `PROCESS_CREATE`, `PROCESS_WATCH`, `PROCESS_KILL`, `PROCESS_STATUS`, `PROCESS_EXIT_CODE`, `THREAD_CREATE/START/EXIT`, `FUTEX_WAIT/WAKE`.
- **Capabilities / CSpace**: `HANDLE_DUP`, `HANDLE_TRANSFER`, `HANDLE_TYPE`, `HANDLE_SAME_OBJECT`, `CNODE_CREATE/MINT/MOVE/FETCH/DELETE/SWAP`, `CSPACE_RESOLVE`, `PROC_CSPACE_MINT`, `CAP_DERIVE`, `CAP_REVOKE`.
- **Endpoint IPC**: `ENDPOINT_CREATE`, `EP_SEND`, `EP_RECV`, `EP_NB_SEND`, `EP_NB_RECV`, `EP_CALL`, `REPLY`.
- **Memory / untyped / frames**: `VMO_CREATE/MAP/MAP_INTO/UNMAP/SHARE/SIZE`, `UNTYPED_INFO/RETYPE/RESET`, `FRAME_MAP/UNMAP`.
- **Scheduling**: `SC_CREATE`, `SC_CONFIGURE`, `THREAD_SET_SC`, `TCB_SELF/SUSPEND/RESUME/SET_PRIORITY/EXIT/GET_INFO`, `SCHED_INFO`.
- **Hardware / bootstrap (cap-gated)**: `CAP_CREATE_IRQCAP`, `CAP_CREATE_IOPORT`, `IOPORT_IN/OUT/RESTRICT`, `IRQ_ROUTE_REGISTER`, `IRQ_ACK`, `BOOTCAP_RESTRICT`, `FRAMEBUFFER_VMO`, `INITRD_COUNT/VMO`, `POWEROFF`, `KLOG_DRAIN`.
- **Legacy IPC (compat/bootstrap)**: `CHAN_CREATE/SEND/RECV/RECV_NB/RECV_TIMEOUT/SEAL/CALL`, `WAIT_ANY`, `WAIT_ANY_TIMEOUT`, `NOTIFY_CREATE/SIGNAL/WAIT/WAIT_TIMEOUT`, `EXCEPTION_HANDLER/RESUME`.

Permanently retired / reserved (return `IRIS_ERR_NOT_SUPPORTED`): `SYS_WRITE(0)`, `SYS_BRK(7)`, `SYS_IPC_CREATE(9)`, `SYS_IPC_SEND(10)`, `SYS_IPC_RECV(11)`, `SYS_SPAWN(18)`, `SYS_NS_REGISTER(24)`, `SYS_NS_LOOKUP(25)`, `SYS_DIAG_SNAPSHOT(30)`, `SYS_SPAWN_SERVICE(31)`, `SYS_INITRD_LOOKUP(41)`, `SYS_SPAWN_ELF(42)` — the namespace and spawn primitives were superseded by CSpace/endpoint discovery and the composable `INITRD_VMO → PROCESS_CREATE → THREAD_START` flow.

## Services

### `svcmgr`
Endpoint-first service manager and supervisor (see API above). Receives hardware caps from `KBootstrapCap`, distributes per-service caps, pre-start-mints well-known slots into children, supervises restart with generation tracking, and narrows its own `KBootstrapCap` after acquiring hardware caps (strips `HW_ACCESS`/`FRAMEBUFFER`, keeps `SPAWN_SERVICE`/`KDEBUG`).

### `vfs`
Endpoint-only filesystem (no legacy channel pair). Boot files plus initrd-backed VMO-mapped files. Endpoint protocol: `READ_AT`, `LIST`, `STAT`, `STATUS` (plus the generic `PING`). Each request observes the caller's badge.

### `kbd`
PS/2 keyboard driver (assembly). IRQ 1 routed to a `KNotification` (WAIT side at the well-known IRQ slot); deferred ACK via `SYS_IRQ_ACK`. Serves key events over its endpoint (`kbd.ep`) with a parked one-shot `KReply` for blocking reads.

### `console`
Serial UART output service (0x3F8) exposed as an endpoint (`console.ep`).

### `sh`
Interactive shell — a **pure CPtr-first client**: everything it needs arrives as well-known CSpace slots; it never uses a legacy channel. Commands include `help`, `ls`, `cat <file>`, `clear`, `memory`, `uptime`.

### `fb`
Framebuffer painter; claims the framebuffer VMO via `SYS_FRAMEBUFFER_VMO` (one-shot); fire-and-forget.

## Memory & Hardware

- **Memory model**: sparse VMOs with eager map-time allocation — **no demand paging**. `KVmo.sparse`, per-process physical page quota, per-VMO page-shard locks. usercopy validates via the VMO mapping list and PTEs; it never allocates.
- **PCID**: CR4.PCIDE enabled when supported; each process gets a unique PCID (1–4094), kernel runs PCID 0, so context switches don't evict other processes' TLB entries.
- **SMP foundation**: per-CPU `iris_cpu_local` (GS-relative), LAPIC detected/software-enabled; PIC + PIT (100 Hz) remain the active interrupt source; AP bringup deferred.
- **IRQ delivery**: seL4-style deferred ACK — kernel masks + EOIs, signals a `KNotification`, the ring-3 handler reads hardware and calls `SYS_IRQ_ACK` to unmask.
- **Hardening**: `-fstack-protector-strong` with RDTSC-seeded per-service canary; per-service ASLR load bias; capability-gated IRQ/IO; framebuffer TOCTOU guard; PMM double-free panics.

## Limits

| Constant | Value |
|----------|-------|
| `TASK_MAX` | 256 |
| `KPROCESS_MAX_LIVE` | 64 |
| `HANDLE_TABLE_MAX` | 1024 |
| `KCNODE_DEFAULT_SLOTS` | 256 (root CNode) |
| `KCHAN_CAPACITY` | 128 messages |
| `KVMO_MAX_PAGES` | 16384 (64 MB per VMO) |
| `FUTEX_TABLE_SIZE` | 256 |
| CPtr namespace split | `<1024` CSpace, `>=1024` handle table |
| PCID range | 1–4094 per process; 0 = kernel |

## Testing

Two independently-gating layers, run on every change:

- **Host unit tests** — `make test-unit`: **10567 assertions** across ~21 suites that exercise the kernel objects directly (cspace, cnode, handle_table, kendpoint, kreply, knotification, kuntyped, kschedctx, kframe, rights, ipc_cspace, vfs_ep, …).
- **Runtime tests** — booted under QEMU headless: **68 tests (T001–T068)** covering KChannel/syscall basics (T001–T012), CPtr-first slots (T041–T046), badges & sender identity (T047–T053), service lifecycle / death-restart / relookup (T054–T062), endpoint cap-transfer & cap-backed REGISTER (T063–T066), and endpoint-first svcmgr DIAG / no-fallback (T067–T068).

```bash
make                                                       # zero-warning build
make test-unit                                             # host unit suites (10567)
make smoke-runtime                                         # headless runtime lane (SUITE PASS 68/68)
ENABLE_RUNTIME_SELFTESTS=1 make smoke-runtime-selftests    # + kernel self-tests
make run                                                   # interactive QEMU
```

Zero-warning policy: the build is treated as broken if `gcc -Wall -Wextra -Wshadow -Wundef` emits any diagnostic.

## What Does Not Exist Yet

IRIS is not a general-purpose OS. The current tree does not provide a persistent disk filesystem, networking, full SMP / AP bringup (foundation present, scheduling is single-CPU), userland ELF loading from external storage, a dynamic linker, a POSIX layer, or hardware support beyond QEMU x86-64. Recursive capability revocation, full `init` deconstruction, and total `KChannel` retirement are in-progress (see `docs/`).

## Positioning

IRIS is a serious experimental capability microkernel with a real ring-3 service boundary, headless-validated on every commit. The kernel owns boot, memory, paging/PCID, the scheduler, syscall dispatch, capability enforcement, IRQ routing, the typed object set, and first-task creation. It does **not** own VFS logic, keyboard handling, console output, service discovery, supervision, or shell behavior — all of that is ring-3 code talking over capability-secured endpoints.

## Documentation

Design notes live in `docs/`: `endpoint-protocol.md`, `badges-sender-identity.md`, `service-lifecycle.md`, `cptr-first-services.md`, `kchannel-migration.md`, `ipc.md`, `vmo-memory.md`, `memory-invariants.md`, and per-service contracts under `docs/contracts/`.
