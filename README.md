# IRIS

IRIS is an `x86_64` operating system built around a **capability-based,
seL4-inspired microkernel**. It runs real user-space services, enforces hardware
and process isolation through an unforgeable capability system, and boots to an
interactive shell entirely from ring-3 code after the first kernel handoff.

The kernel exposes a small set of typed kernel objects (endpoints, CNodes, reply
objects, untyped memory, frames, VMOs, VSpaces, notifications, TCBs, scheduling
contexts, …). Everything above the kernel — service discovery, supervision, the
filesystem, the keyboard driver, the console, a user-space pager, and the shell
— lives in ring 3 and talks over **synchronous endpoint IPC** with
kernel-stamped sender identity.

## Boot sequence

```
UEFI → BOOTX64.EFI → KERNEL.ELF
  kernel: PMM (buddy), 16 MB kernel-object slab, 4-level paging + PCID,
          GDT/IDT/PIC/PIT, LAPIC, scheduler, syscall/sysret dispatch,
          capability enforcement, initrd catalog
    → userboot (ring-3 flat binary, injected KBootstrapCap)
      → init            (orchestration, CPtr-first mint handoff, boot self-tests)
        → fb            (framebuffer painter, fire-and-forget)
        → console       (serial output service, endpoint)
        → svcmgr        (service manager + supervisor, endpoint-first)
          → kbd         (PS/2 keyboard driver; endpoint + IRQ notification)
          → vfs         (boot-namespace filesystem + file grants; endpoint-only)
          → sh          (interactive shell; pure CPtr-first client)
```

Every service from `init` onward is a ring-3 ELF loaded by `svc_loader` using
only kernel primitives (`INITRD_VMO → PROCESS_CREATE → VMO map → THREAD_START`).
The kernel spawns nothing after the initial `userboot` task; the initrd may grow
past the named catalog and extra images are loaded by index.

## Capability & object model

Capabilities are unforgeable references to typed kernel objects. Rights are
stored **per capability**, not per object, and can only be *reduced* on
copy/mint/transfer, never elevated. Since Fase S3 every CSpace capability
also carries a native **CDT/MDB** derivation node (parent / children /
siblings): copy and mint record a derivation edge, and `CSPACE_REVOKE`
recursively destroys a capability's entire descendance across CNodes and
processes while the invoked capability and its siblings survive — delegation
is no longer "give away forever". See
`docs/architecture/cspace-cdt-mdb.md` and the
[seL4 purity charter](docs/architecture/iris-sel4-purity-charter.md).

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
| `KOBJ_VMO` | Memory object; sparse (populated at map time), page-granular map, or MMIO wrap. |
| `KOBJ_NOTIFICATION` | Lightweight signal/wait; used for IRQ delivery, process-exit watch, and fault delivery. |
| `KOBJ_PROCESS` | Process container (address space + handle table + CSpace root). |
| `KOBJ_IRQ_CAP` / `KOBJ_IOPORT` | Capability-gated hardware access. |
| `KOBJ_BOOTSTRAP_CAP` | First-task authority with per-bit permission flags. |
| `KOBJ_INITRD_ENTRY` | Read-only handle to an initrd image slot. |
| `KOBJ_CHANNEL` | Removed (Fase 13). The enum value is reserved; all `CHAN_*` syscalls return `NOT_SUPPORTED`. |

### Rights

`RIGHT_READ`, `RIGHT_WRITE`, `RIGHT_DUPLICATE`, `RIGHT_TRANSFER`, `RIGHT_ROUTE`,
`RIGHT_MANAGE`, `RIGHT_WAIT`. A dup/mint/transfer computes
`effective = source_rights & requested`; rights that collapse to `RIGHT_NONE`
are rejected. `ACCESS_DENIED` is a hard stop with no fallback.

### CPtr-first addressing & the namespace split

Services are handed their well-known capabilities **before they start**, minted
directly into their root CNode (`SYS_PROC_CSPACE_MINT`), and invoke them by CPtr
— e.g. `SYS_EP_CALL(IRIS_CPTR_SVCMGR_EP, &msg)` — with no handle transfer. CPtrs
and handle IDs share one argument namespace, split and enforced by the kernel:

- **value `< 1024`** → resolved through the **CSpace only** (no handle-table
  fallback; a missing slot fails cleanly).
- **value `>= 1024`** → resolved through the **handle table only** (handle IDs
  are `slot | generation<<10`, generation ≥ 1).

Well-known child slots: `1` svcmgr EP, `2` vfs EP, `3` console EP, `4` kbd EP,
`5` own EP (recv), `6` spawn/authority cap, `7` IRQ notification.

## Endpoint IPC

Synchronous rendezvous: a client `SYS_EP_CALL`s and blocks; a server loops on
`SYS_EP_RECV` and answers with `SYS_REPLY`. `SYS_EP_SEND` / `SYS_EP_NB_SEND` /
`SYS_EP_NB_RECV` cover the async and non-blocking variants. The wire message is
`struct IrisMsg`: a label, four inline words, an optional bulk buffer, and **two
capability slots**.

- **Reply capability** — `EP_CALL` creates a one-shot `KReply`; the server
  receives it in `attached_handle` and consumes it with `SYS_REPLY`. Replying
  twice fails.
- **Transferred capability** — the client may move/copy a capability to the
  server in `attached_cap` (+ `attached_cap_rights`). The kernel *stages* it
  (the caller must really hold it; rights reduced) and the server receives it as
  a real handle. The reply cap and the transferred cap never collide, so an
  `EP_CALL` can carry both.

A capability can never be forged from the payload: the kernel clears
`attached_cap`/`sender_badge` written by the client and delivers only real,
kernel-installed handles.

### Sender identity — badges

Every endpoint message carries a **kernel-stamped badge**
(`IrisMsg.sender_badge`) identifying the *capability* the sender invoked — taken
from the cap, never from the payload, so it cannot be spoofed. Badges are
per-capability metadata assigned at mint time (`SYS_PROC_CSPACE_MINT` packs
`rights | badge<<32`); a badged cap can **never be re-badged** (no identity
forging). Two caps to the same endpoint can carry different badges. Replies
force `sender_badge = 0`. Servers use the badge to authenticate clients — from
`iris_badge_is_supervisor()` gating privileged lifecycle operations to the VFS
classifying file-grant admins, grant sessions, and ordinary clients.

## Service lifecycle & supervision

`svcmgr` supervises catalog services with real death/restart and a generation
model:

- **Death → respawn**: `SYS_PROCESS_WATCH` reports a service exit (a
  `KNotification` signal); svcmgr respawns it under a per-service restart budget
  and **bumps its generation**. Endpoint masters persist across restart so
  client caps stay valid.
- **Liveness oracle**: `IRIS_SVCMGR_EP_STATUS` returns `{alive, generation}` and
  an explicit supervision-policy classification — a non-blocking way to poll a
  restart without hanging on a dead endpoint.
- **Explicit supervision policy**: each catalog service declares a criticality
  class (critical-restart / optional-restart / optional-no-restart); a service
  that exceeds its restart budget is left **degraded**, never silently
  restarted forever.
- **Logical revocation**: a cached generation older than the current one is
  *stale*; the supervisor's registry is the source of truth.

## `svcmgr` — endpoint-first API

The productive svcmgr API is entirely endpoint-based (`SYS_EP_CALL` to the
svcmgr endpoint), badge-authenticated:

| Opcode | Operation |
|--------|-----------|
| `IRIS_SVCMGR_EP_LOOKUP_NAME` | Resolve `<name>` / `<name>.ep`; reply carries the endpoint cap. |
| `IRIS_SVCMGR_EP_REGISTER` | Cap-backed registration: client transfers an endpoint in `attached_cap`; owner = sender badge. |
| `IRIS_SVCMGR_EP_UNREGISTER` | Owner-badge (or supervisor) checked. |
| `IRIS_SVCMGR_EP_STATUS` | `{alive, generation}` + supervision policy for a service. |
| `IRIS_SVCMGR_EP_RESTART` | Supervisor-only kill + watch-driven respawn. |
| `IRIS_SVCMGR_EP_DIAG` | Snapshot: catalog count / ready / active dynamic / catalog version. |
| `IRIS_EP_OP_PING` | Health check; echoes the observed sender badge. |

Reserved names (`*.ep`, catalog names) are never runtime-registrable. `.ep`
lookups grant `RIGHT_WRITE` to ordinary clients; `DUPLICATE`/`TRANSFER` is
reserved for supervisor badges. Unknown opcodes fail with `INVALID_ARG` — there
is no silent fallback to the legacy path.

## User-space pager & file-backed memory

Demand fault resolution runs entirely in ring 3. A supervised **pager** service
resolves page faults for the processes it is granted, backing them from files
served by the VFS — with **no new kernel syscall**; it composes from
`SYS_VMO_MAP_PAGE`, `SYS_PROCESS_VSPACE`, fault generations, seq-checked resume,
and the VFS grant protocol.

- **Fault delivery**: a faulting process's exception is delivered as a
  `KNotification` signal (`SYS_EXCEPTION_HANDLER`); the pager reads the fault via
  `SYS_PROCESS_FAULT_INFO`, maps a page, and resumes the target with a
  seq-checked `SYS_EXCEPTION_RESUME`.
- **File-backed regions**: read-only shared (a bounded, evicting page cache),
  private-writable (copy-at-fill), exact EOF / zero-fill, and W^X-checked
  segment shapes (RX code / R rodata / RW data / BSS) as ELF-loading groundwork.
  Shared-writable is deliberately unsupported.
- **Least authority**: the pager's entire authority is a pre-start manifest —
  a control endpoint, per-target process/address-space grants, VMO grants for
  its cache and private pool, one shared fault notification, and a VFS grant
  session. It holds no spawn cap, no untyped, no device caps, and no global VFS
  access. Its compromise is bounded by its manifest; its death is survivable;
  its restart regains exactly the declaration.

### VFS-enforced file grants

File access is a **VFS-issued, VFS-validated capability**, not a pathname. The
supervisor asks the VFS to open a grant on a named file for a pager session; the
VFS returns an opaque backing identity (`backing_id`, `generation`) and hands
the pager a session-badged, write-only `vfs.ep` cap. From then on:

- the pager reads by **grant index**, never by name — the VFS denies a session
  badge every name-based operation (no read-by-name, no directory listing);
- the VFS validates badge + session + grant + rights + generation on **every**
  read, so a compromised pager can only read the backings it was explicitly
  granted;
- grant rights (stat / read / duplicate / revoke) reduce monotonically;
- **revocation is enforced at the VFS**: revoking a backing bumps its
  generation, so a stale grant fails closed even if the pager keeps the index,
  the name, or an old message; a VFS restart re-seeds under a fresh instance
  epoch, so grants never survive it.

## Resource ownership & accounting

Every kernel object is charged to the process that logically **owns** it (its
payer / resource domain), selected by explicit capability authority at creation
— not to whoever ran the syscall. A `KProcess` *is* a resource domain.

- `SYS_VMO_CREATE(size)` charges the caller; `SYS_VMO_CREATE_FOR(size, target)`
  charges a process the caller holds `RIGHT_MANAGE` on. A loader charges each
  child's image VMOs to the **child**, so a supervisor can launch many children
  without accumulating their memory against its own quota.
- Sparse VMO pages are charged **once to the VMO owner** and released at
  destroy; a shared VMO's pages are paid once, and mapping it into more targets
  does not re-charge.
- Per-domain quotas (`KPROCESS_VMO_QUOTA` = 32, `KPROCESS_NOTIFICATION_QUOTA` =
  16, `KPROCESS_PHYS_PAGES_LIMIT` = 2048) carry monotonic high-water marks;
  exhaustion is atomic (clean `NO_MEMORY`, no partial object, a global
  failed-charge counter advances).
- `SYS_RESOURCE_INFO(proc, out)` is a read-only, versioned snapshot of a
  domain's usage / limit / high-water plus system-wide failed-charge / rollback
  / kslab gauges.

The kernel object slab (16 MB) is **global implementation capacity**,
deliberately distinct from per-domain quota; its exhaustion returns `NULL` →
`IRIS_ERR_NO_MEMORY` with no corruption. See
`docs/architecture/resource-ownership-accounting.md` and
`kernel-capacity-limits.md`.

## Syscall surface

~117 syscall slots (0–116; several early numbers permanently retired).
Highlights by area:

- **Core / process / thread**: `EXIT`, `GETPID`, `YIELD`, `SLEEP`, `CLOCK_GET`,
  `CLOCK_NANOSLEEP`, `PROCESS_CREATE`, `PROCESS_WATCH`, `PROCESS_KILL`,
  `PROCESS_STATUS`, `PROCESS_EXIT_CODE`, `PROCESS_VSPACE`, `PROCESS_FAULT_INFO`,
  `THREAD_CREATE/START/EXIT`, `FUTEX_WAIT/WAKE`.
- **Capabilities / CSpace**: `HANDLE_DUP`, `HANDLE_TYPE`,
  `HANDLE_SAME_OBJECT`, `CNODE_MINT/MOVE/FETCH/DELETE/SWAP`,
  `CSPACE_RESOLVE`, `PROC_CSPACE_MINT`. Native **CDT/MDB** derivation
  (Fase S3): `CSPACE_MINT` (copy/mint slot→slot), `CSPACE_MINT_INTO`
  (cross-process mint), `CSPACE_REVOKE` (recursive, cross-process). The
  handle-tree `CAP_DERIVE`/`CAP_REVOKE` are legacy, frozen, and slated for
  retirement. `HANDLE_TRANSFER`, `CNODE_CREATE`, `ENDPOINT/NOTIFY/CNODE/SC_CREATE`
  are retired (`NOT_SUPPORTED`).
- **Endpoint IPC**: `ENDPOINT_CREATE`, `EP_SEND`, `EP_RECV`, `EP_NB_SEND`,
  `EP_NB_RECV`, `EP_CALL`, `REPLY`.
- **Memory / untyped / frames**: `VMO_CREATE/CREATE_FOR/MAP/MAP_INTO/MAP_PAGE/UNMAP/SHARE/SIZE`,
  `VSPACE_SELF`, `RESOURCE_INFO`, `UNTYPED_INFO/RETYPE/RESET`, `FRAME_MAP/UNMAP`.
- **Faults / notifications**: `NOTIFY_CREATE/SIGNAL/WAIT/WAIT_TIMEOUT`,
  `EXCEPTION_HANDLER`, `EXCEPTION_RESUME`.
- **Scheduling**: `SC_CREATE`, `SC_CONFIGURE`, `THREAD_SET_SC`,
  `TCB_SELF/SUSPEND/RESUME/SET_PRIORITY/EXIT/GET_INFO`, `SCHED_INFO`.
- **Hardware / bootstrap (cap-gated)**: `CAP_CREATE_IRQCAP`, `CAP_CREATE_IOPORT`,
  `IOPORT_IN/OUT/RESTRICT`, `IRQ_ROUTE_REGISTER`, `IRQ_ACK`, `BOOTCAP_RESTRICT`,
  `FRAMEBUFFER_VMO`, `INITRD_COUNT/VMO`, `POWEROFF`, `KLOG_DRAIN`.
- **CSpace derivation (Fase S3)**: `CSPACE_MINT`, `CSPACE_REVOKE`,
  `CSPACE_MINT_INTO` — native MDB/CDT, CSpace-only source, cross-process
  recursive revoke.

Several early syscalls (`SYS_WRITE`, `SYS_BRK`, `SYS_SPAWN`, `SYS_NS_REGISTER`,
`SYS_NS_LOOKUP`, `SYS_SPAWN_ELF`, …) are permanently retired and return
`IRIS_ERR_NOT_SUPPORTED`; the namespace and spawn primitives were superseded by
CSpace/endpoint discovery and the composable `INITRD_VMO → PROCESS_CREATE →
THREAD_START` flow.

## Services

- **`svcmgr`** — endpoint-first service manager and supervisor (API above).
  Receives hardware caps from `KBootstrapCap`, distributes per-service caps,
  pre-start-mints well-known slots into children, supervises restart with
  generation tracking and explicit policy, and narrows its own `KBootstrapCap`
  after acquiring hardware caps.
- **`vfs`** — endpoint-only filesystem. Boot files plus initrd-backed
  VMO-mapped files, served by name (`READ_AT` / `LIST` / `STAT` / `STATUS`) to
  ordinary clients, and by unforgeable **file grant** to pager sessions. Every
  request observes the caller's badge.
- **`pager`** — user-space page-fault handler + file-backed memory subsystem
  (above). Its own supervised binary.
- **`kbd`** — PS/2 keyboard driver. IRQ 1 routed to a `KNotification`; deferred
  ACK via `SYS_IRQ_ACK`. Serves key events over `kbd.ep` with a parked one-shot
  `KReply` for blocking reads.
- **`console`** — serial UART output service (`0x3F8`) exposed as `console.ep`.
- **`sh`** — interactive shell, a pure CPtr-first client. Commands include
  `help`, `ls`, `cat <file>`, `clear`, `memory`, `uptime`.
- **`fb`** — framebuffer painter; claims the framebuffer VMO via
  `SYS_FRAMEBUFFER_VMO` (one-shot); fire-and-forget.

## Memory & hardware

- **Memory model**: sparse VMOs with map-time allocation plus page-granular
  mapping (`SYS_VMO_MAP_PAGE`) for the user pager — **no kernel demand paging**;
  page faults are resolved in ring 3. Per-process physical page quota; per-VMO
  page-shard locks. usercopy validates via the VMO mapping list and PTEs; it
  never allocates.
- **Kernel object slab**: typed kernel object headers (KProcess, KVSpace, root
  KCNode, page tables, endpoints, …) are allocated from a dedicated 16 MB slab,
  leaving the rest of the PMM for userspace as untyped/frame capabilities.
- **PCID**: CR4.PCIDE enabled when supported; each process gets a unique PCID
  (1–4094), kernel runs PCID 0, so context switches don't evict other processes'
  TLB entries.
- **SMP foundation**: per-CPU `iris_cpu_local` (GS-relative), LAPIC
  detected/software-enabled; PIC + PIT (100 Hz) remain the active interrupt
  source; AP bringup deferred (scheduling is single-CPU).
- **IRQ delivery**: seL4-style deferred ACK — kernel masks + EOIs, signals a
  `KNotification`, the ring-3 handler reads hardware and calls `SYS_IRQ_ACK`.
- **Hardening**: `-fstack-protector-strong` with RDTSC-seeded per-service
  canary; per-service ASLR load bias; capability-gated IRQ/IO; framebuffer
  TOCTOU guard; PMM double-free panics; W^X on user mappings.

## Limits

| Constant | Value |
|----------|-------|
| `TASK_MAX` | 256 |
| `KPROCESS_MAX_LIVE` | 64 |
| `HANDLE_TABLE_MAX` | 256 |
| `KCNODE_DEFAULT_SLOTS` | 256 (root CNode) |
| `KVMO_MAX_PAGES` | 16384 (64 MB per VMO) |
| `KPROCESS_NOTIFICATION_QUOTA` | 16 per domain |
| `KPROCESS_VMO_QUOTA` | 32 per domain |
| `KPROCESS_PHYS_PAGES_LIMIT` | 2048 (8 MB) per domain |
| kernel object slab | 16 MB (global capacity) |
| CPtr namespace split | `<1024` CSpace, `>=1024` handle table |
| PCID range | 1–4094 per process; 0 = kernel |

## Testing

Two independently-gating layers, run on every change:

- **Host unit tests** — `make test-unit`: **10410 assertions** across ~22 suites
  that exercise the kernel objects and pure logic directly (cspace, cnode,
  handle_table, kendpoint, kreply, knotification, kuntyped, kschedctx, kframe,
  the MDB/CDT (structural + model-based fuzzing), rights, ipc_cspace, vfs_ep
  including the file-grant layer, …).
- **Runtime tests** — booted under QEMU headless: **267 tests** covering IPC and
  syscall basics, CPtr-first slots, badges & sender identity, service lifecycle /
  death-restart / relookup, endpoint cap-transfer, device/driver isolation,
  service supervision, the user pager and fault model, file-backed memory,
  VFS-enforced file grants, multi-target paging, resource ownership / quota
  accounting, the canonical Untyped-born TCB lifecycle (T284–T287), and the
  native MDB/CDT with cross-process revocation (T288–T290).
- **Purity gate** — `make check-purity`: the frozen legacy-consumer allowlist
  (handle table / kslab). It can only shrink.

```bash
make                                                       # zero-warning build
make check-purity                                          # seL4 purity allowlist
make test-unit                                             # host unit suites (10410)
make smoke-runtime                                         # headless runtime lane
ENABLE_RUNTIME_SELFTESTS=1 make smoke-runtime-selftests    # + full self-test suite (267/267)
make run                                                   # interactive QEMU
```

Zero-warning policy: the build is treated as broken if
`gcc -Wall -Wextra -Wshadow -Wundef` emits any diagnostic.

## What does not exist yet

IRIS is not a general-purpose OS. The current tree does not provide a persistent
disk filesystem, a mutable filesystem or writeback, networking, full SMP / AP
bringup (foundation present, scheduling is single-CPU), a global page cache,
copy-on-write, full ELF demand paging (the pager has the groundwork), a dynamic
linker, a POSIX layer, or hardware support beyond QEMU x86-64.

## Positioning

IRIS is a serious experimental capability microkernel with a real ring-3 service
boundary, headless-validated on every change. The kernel owns boot, memory,
paging/PCID, the scheduler, syscall dispatch, capability enforcement, IRQ
routing, fault delivery, the typed object set, and first-task creation. It does
**not** own VFS logic, keyboard handling, console output, service discovery,
supervision, page-fault resolution, file-backed memory, or shell behavior — all
of that is ring-3 code talking over capability-secured endpoints.

## Documentation

Design notes live in `docs/`, including `endpoint-protocol.md`,
`badges-sender-identity.md`, `service-lifecycle.md`, `cptr-first-services.md`,
`vmo-memory.md`, `memory-invariants.md`, per-service contracts under
`docs/contracts/`, and deeper design records under `docs/architecture/`
(the user pager, file-backed memory, file-grant capability, service
supervision, device isolation, and more).
