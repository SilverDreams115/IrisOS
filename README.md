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
    → userboot (ring-3 flat binary; reads a structured BootInfo describing
                every capability the kernel installed in its CSpace, and
                validates it against that CSpace before delegating anything)
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
Each of those steps names the **budget** it spends (Stage 6): the image copy, the
child's address space, its process state and its segment and stack VMOs all come
out of an `Untyped` the spawner chose, and the spawner recycles one budget per
live child so cost is bounded by what is running rather than by what has run.
The kernel spawns nothing after the initial `userboot` task; the initrd may grow
past the named catalog and extra images are loaded by index.

## Capability & object model

Capabilities are unforgeable references to typed kernel objects. Rights are
stored **per capability**, not per object, and can only be *reduced* on
copy/mint/transfer, never elevated. Since Phase S3 every CSpace capability
also carries a native **CDT/MDB** derivation node (parent / children /
siblings): copy and mint record a derivation edge, and `CSPACE_REVOKE`
recursively destroys a capability's entire descendance across CNodes and
processes while the invoked capability and its siblings survive — delegation
is no longer "give away forever". Since Phase S4 this is the **only**
derivation tree: an IPC transfer parents the delivered cap to the sender's
source slot, and a device capability is parented to the bootstrap cap that
authorised it, so both are revocable by their grantor. See
`docs/architecture/cspace-cdt-mdb.md` and the
[seL4 purity charter](docs/architecture/iris-sel4-purity-charter.md).

| Object | Role |
|--------|------|
| `KOBJ_ENDPOINT` | Synchronous rendezvous IPC (seL4-style). Primary service transport. |
| `KOBJ_REPLY` | One-shot reply capability created by `EP_CALL`; consumed by `SYS_REPLY`. |
| `KOBJ_CNODE` | Capability storage node; a process's CSpace is a tree of CNodes. |
| `KOBJ_UNTYPED` | Untyped memory; retyped into other kernel objects (`SYS_UNTYPED_RETYPE2`), and the **budget** every allocation names since Stage 6. Carves from both ends: page-aligned regions from the bottom, object headers from the top. A task that maps anything needs one, because paging levels are retyped from it. |
| `KOBJ_TCB` | Thread control block (suspend/resume/priority/info). |
| `KOBJ_SCHED_CONTEXT` | Scheduling context (budget/period) bound to a TCB. |
| `KOBJ_FRAME` | Physical frame capability; mapped into a VSpace. |
| `KOBJ_VSPACE` | Address space (CR3 + PCID). **Retyped by its holder** since Stage 6-pure: its 4 KiB region IS the PML4, and `SYS_PROCESS_CREATE` takes one rather than building it. Binds to exactly one process. |
| `KOBJ_PAGE_TABLE` | A paging level, retyped like any other object and installed with `SYS_VSPACE_MAP_TABLE` (seL4's `PageTable_Map`). The kernel creates none: a map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE` and the holder supplies the level. |
| `KOBJ_VMO` | Memory object; sparse (populated at map time), page-granular map, or MMIO wrap. |
| `KOBJ_NOTIFICATION` | Lightweight signal/wait; used for IRQ delivery, process-exit watch, and fault delivery. |
| `KOBJ_PROCESS` | Process container, **composed** from a VSpace and a root CNode its creator retyped — seL4's `TCB_Configure` shape. Frozen: it retires whole with the user-space process server (Stage 7). |
| `KOBJ_IRQ_CAP` / `KOBJ_IOPORT` | Capability-gated hardware access. |
| `KOBJ_BOOTSTRAP_CAP` | Boot authority, **one capability per authority** since Stage 5 — process, initrd, IRQ control, ioport control, debug, framebuffer. Matched by exact equality; a capability carrying two of them cannot be constructed. |
| `KOBJ_INITRD_ENTRY` | Read-only handle to an initrd image slot. |
| `KOBJ_CHANNEL` | Removed (Phase 13). The enum value is reserved; all `CHAN_*` syscalls return `NOT_SUPPORTED`. |

### Rights

`RIGHT_READ`, `RIGHT_WRITE`, `RIGHT_DUPLICATE`, `RIGHT_TRANSFER`, `RIGHT_ROUTE`,
`RIGHT_MANAGE`, `RIGHT_WAIT`. A dup/mint/transfer computes
`effective = source_rights & requested`; rights that collapse to `RIGHT_NONE`
are rejected. `ACCESS_DENIED` is a hard stop with no fallback.

### Introspection

`SYS_CAP_IDENTIFY(cptr)` returns the type of the capability in a slot the
caller names, and `SYS_CAP_SAME_OBJECT(a, b)` says whether two slots name the
same kernel object (identity only — rights and badge are not compared). Both
are CPtr-only, take no right, produce no capability and retain nothing past the
call; a handle value is `INVALID_ARG` with no fallback. They exist because a
supervisor must narrow its protocol on the type of a cap it was just handed,
and because proving that a transferred capability is the *same* object the
sender held is an authority property, not a debugging convenience.

### CPtr-first addressing

Services are handed their well-known capabilities **before they start**, minted
directly into their root CNode (`SYS_PROC_CSPACE_MINT`), and invoke them by CPtr
— e.g. `SYS_EP_CALL(IRIS_CPTR_SVCMGR_EP, &msg)` — with no handle transfer. CPtrs
and handle IDs share one argument namespace, split and enforced by the kernel:

There is **one** authority namespace. Stage 4 deleted the handle table
outright — `HandleTable`, `KProcess.handle_table` and the twelve syscalls that
spoke that language are gone, their numbers permanently reserved. A syscall
argument is a CPtr or it is `INVALID_ARG`; there is nowhere else to look.

A CPtr is walked radix-by-radix through the CNode tree and addresses **exactly
one** capability: leftover bits at a non-CNode terminal are `INVALID_ARG`, not
a silent alias of a shallower slot. Receive slots are full CPtrs too, so a
process whose root CNode is full can still be handed a capability.

Every capability is created *into* a slot. `SYS_VMO_CREATE`,
`SYS_PROCESS_CREATE`, `SYS_TCB_SELF`, `SYS_VSPACE_SELF` and the rest take a
destination and refuse to run without one.

Well-known child slots: `1` svcmgr EP, `2` vfs EP, `3` console EP, `4` kbd EP,
`5` own EP (recv), `6` process control, `7` IRQ notification, `8` initrd
control, `9` debug control.

Boot authority is **one capability per authority** (Stage 5): process, initrd,
IRQ control, ioport control, debug and framebuffer, each matched by exact
equality — a capability carrying two of them cannot be constructed.  The root
task learns what it holds from a structured **BootInfo** region the kernel maps
read-only into it, instead of agreeing with the kernel on constants; it also
holds capabilities to its own root CNode and its own thread.

## Endpoint IPC

Synchronous rendezvous: a client `SYS_EP_CALL`s and blocks; a server loops on
`SYS_EP_RECV` and answers with `SYS_REPLY`. `SYS_EP_SEND` / `SYS_EP_NB_SEND` /
`SYS_EP_NB_RECV` cover the async and non-blocking variants. The wire message is
`struct IrisMsg`: a label, four inline words, an optional bulk buffer, and **two
capability slots**.

- **Reply capability** — `EP_CALL` creates a one-shot `KReply`; the server
  receives it in `attached_handle` and consumes it with `SYS_REPLY`. Replying
  twice fails.
- **Transferred capability** — the client may move a capability to the server in
  `attached_cap` (+ `attached_cap_rights`). Since Phase S4 the **source is a
  CPtr resolved to its CSpace slot**, never a handle (a handle value is
  `INVALID_ARG`, with no fallback), and the delivered cap is installed as an
  **MDB child of the sender's source slot** — so an IPC delegation is
  revocable from the sender or any of its ancestors. The kernel *stages* it
  (the caller must really hold it, with `RIGHT_TRANSFER`; rights reduced),
  delivers, and only then consumes the source slot. A capability revoked while
  staged is never delivered. The reply cap and the transferred cap never
  collide, so an `EP_CALL` can carry both.

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
resolves page faults for the threads it is granted, backing them from files
served by the VFS — with **no new kernel syscall**; it composes from
`SYS_VMO_MAP_PAGE`, the target's address-space capability (handed over by the
spawner that retyped it), fault generations, seq-checked resume, and the VFS
grant protocol.

- **Fault delivery**: a faulting THREAD's exception is delivered as a
  `KNotification` signal, armed on that thread with
  `SYS_TCB_SET_FAULT_HANDLER`, which also names the mailbox the faulting
  thread's capability lands in.  The pager reads the fault off that capability
  with `SYS_TCB_FAULT_INFO`, maps a page, and resumes the target with a
  seq-checked `SYS_EXCEPTION_RESUME` — every step naming the execution, never
  an id.
- **File-backed regions**: read-only shared (a bounded, evicting page cache),
  private-writable (copy-at-fill), exact EOF / zero-fill, and W^X-checked
  segment shapes (RX code / R rodata / RW data / BSS) as ELF-loading groundwork.
  Shared-writable is deliberately unsupported.
- **Least authority**: the pager's entire authority is a pre-start manifest —
  a control endpoint, per-target process/address-space grants, VMO grants for
  its cache and private pool, one shared fault notification, and a VFS grant
  session. It holds no process-control capability, no untyped, no device caps,
  and no global VFS
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

- `SYS_VMO_CREATE(size, budget, dest)` charges the caller;
  `SYS_VMO_CREATE_FOR(size, target, dest)` charges a process the caller holds
  `RIGHT_MANAGE` on. A loader charges each child's image VMOs to the **child**,
  so a supervisor can launch many children without accumulating their memory
  against its own quota.
- **Since Stage 6 the charge is not a number, it is memory.** A VMO's pages,
  its metadata and its header are carved from an `Untyped` the caller names —
  as are page tables, PML4s, `KVSpace`/`KProcess` headers, root CNodes, mapping
  records and device capabilities. Each carve is a *child* of that Untyped, so
  `SYS_UNTYPED_RESET` refuses while it lives and reclaims the whole region once
  it does not.
- Sparse VMO pages are charged **once to the VMO owner** and released at
  destroy; a shared VMO's pages are paid once, and mapping it into more targets
  does not re-charge.
- Per-domain quotas (`KPROCESS_VMO_QUOTA` = 32, `KPROCESS_PHYS_PAGES_LIMIT` =
  2048) carry monotonic high-water marks; exhaustion is atomic (clean
  `NO_MEMORY`, no partial object, a global failed-charge counter advances).
  The notification quota was **retired in Phase S1** — the capacity to create a
  notification is holding Untyped memory plus a CSpace slot, never a numeric
  kernel quota; `SYS_RESOURCE_INFO` reports `notifs_limit = 0`.
- `SYS_RESOURCE_INFO(proc, out)` is a read-only, versioned snapshot of a
  domain's usage / limit / high-water plus system-wide failed-charge / rollback
  / kslab gauges.

The kernel object slab (16 MB) is **global implementation capacity**,
deliberately distinct from per-domain quota; its exhaustion returns `NULL` →
`IRIS_ERR_NO_MEMORY` with no corruption. After Stage 6 it serves only the boot
path — the root task's own objects and the boot Untypeds, all created before
the first Untyped exists — so a running process cannot grow it. See
`docs/architecture/stage6-memory-from-untyped.md`,
`docs/architecture/resource-ownership-accounting.md` and
`kernel-capacity-limits.md`.

**Reclamation is by RESET, so budgets are recycled, not sized for a run.** A
bump allocator never rewinds: charging memory without a way to get it back
would make every spawn a permanent cost. The loader therefore keeps one budget
per *live* child (reset when the child dies) and one scratch budget for boot
image copies, which bounds consumption by what is alive rather than by what has
ever run — seL4's "revoke the Untyped you used", in the form IRIS has.

## Syscall surface

~123 syscall slots (0–122; several numbers permanently retired and reserved).
Highlights by area:

- **Core / process / thread**: `EXIT`, `GETPID`, `YIELD`, `SLEEP`, `CLOCK_GET`,
  `CLOCK_NANOSLEEP`, `PROCESS_CREATE`, `PROCESS_WATCH`, `PROCESS_KILL`,
  `PROCESS_STATUS`, `PROCESS_EXIT_CODE`, `PROCESS_VSPACE`, `PROCESS_FAULT_INFO`,
  `FUTEX_WAIT/WAKE`.  `THREAD_CREATE` (Stage 5) and `THREAD_START` (Stage 7)
  are retired: a thread is retyped, configured, pointed and resumed — see
  below.
- **Capabilities / CSpace**: `HANDLE_DUP`, `HANDLE_TYPE`,
  `HANDLE_SAME_OBJECT`, `CNODE_MINT/MOVE/FETCH/DELETE/SWAP`,
  `CSPACE_RESOLVE`, `PROC_CSPACE_MINT`. Native **CDT/MDB** derivation
  (Phase S3): `CSPACE_MINT` (copy/mint slot→slot), `CSPACE_MINT_INTO`
  (cross-process mint), `CSPACE_REVOKE` (recursive, cross-process). The
  handle-tree `CAP_DERIVE`/`CAP_REVOKE` are **retired** (Phase S4) — there is
  exactly one derivation tree in the system. `HANDLE_TRANSFER`, `CNODE_CREATE`, `ENDPOINT/NOTIFY/CNODE/SC_CREATE`
  are retired (`NOT_SUPPORTED`).
- **Endpoint IPC**: `ENDPOINT_CREATE`, `EP_SEND`, `EP_RECV`, `EP_NB_SEND`,
  `EP_NB_RECV`, `EP_CALL`, `REPLY`.
- **Memory / untyped / frames**: `VMO_CREATE/CREATE_FOR/MAP/MAP_INTO/MAP_PAGE/UNMAP/SHARE/SIZE`,
  `VSPACE_SELF`, `RESOURCE_INFO`, `UNTYPED_INFO/RETYPE/RESET`, `FRAME_MAP/UNMAP`.
- **Faults / notifications**: `NOTIFY_CREATE/SIGNAL/WAIT/WAIT_TIMEOUT`,
  `EXCEPTION_HANDLER`, `EXCEPTION_RESUME`.
- **Scheduling**: `SC_CREATE`, `SC_CONFIGURE`, `THREAD_SET_SC`,
  `TCB_SELF/SUSPEND/RESUME/SET_PRIORITY/EXIT/GET_INFO`, `SCHED_INFO`.
- **Hardware / bootstrap (cap-gated)**: `CAP_CREATE_IRQCAP`, `CAP_CREATE_IOPORT`
  (each requires ITS OWN control capability — Stage 5's one-capability-one-
  authority split — and publishes the new cap into a caller-named CSpace slot
  as an MDB child of the authorising slot, so revoking the control capability
  revokes what it authorised), `IOPORT_IN/OUT`, `IRQ_ROUTE_REGISTER`, `IRQ_ACK`,
  `FRAMEBUFFER_VMO`, `INITRD_COUNT/VMO`, `POWEROFF`, `KLOG_DRAIN`.
  `BOOTCAP_RESTRICT` is retired with the monolithic boot capability.
- **CSpace derivation (Phase S3)**: `CSPACE_MINT`, `CSPACE_REVOKE`,
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
  page faults are resolved in ring 3. usercopy validates via the VMO mapping
  list and PTEs; it never allocates.
- **Every allocation names its budget** (Stage 6). An address space's page
  tables and PML4, the `KVSpace`/`KProcess` headers, the child's root CNode, a
  VMO's pages and metadata, per-mapping records and device capabilities are all
  carved from an `Untyped` the caller named at creation, and counted as its
  children. A spawn without a budget is refused rather than funded by the
  kernel; a budget cannot be reset while its objects live; and when they die
  the whole region is reusable.
- **Kernel object slab**: 16 MB, now serving only the **boot path** — the root
  task's KProcess, root CNode, KVSpace, PML4 and mapping records (its address
  space is built before the first Untyped exists), the boot Untypeds, the boot
  control capabilities and the initrd catalog. Bounded, does not grow with
  load, and retires with user-space process creation (Stage 7).
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
| `KPROCESS_MAX_LIVE` | retired (Stage 7) — a KProcess comes out of an Untyped, so memory bounds how many exist |
| `KCNODE_DEFAULT_SLOTS` | 256 (root CNode) |
| `KVMO_MAX_PAGES` | 16384 (64 MB per VMO) |
| `KPROCESS_VMO_QUOTA` | 32 per domain |
| `KPROCESS_PHYS_PAGES_LIMIT` | retired (Stage 7) — reports 0; the Untyped a VMO names is the limit |
| kernel object slab | 16 MB (boot path only, since Stage 6) |
| CPtr range | the low 31 bits; a CPtr addresses exactly one capability |
| default per-child budget | 1 MiB, chosen by the spawner |
| PCID range | 1–4094 per process; 0 = kernel |

## Testing

Three independently-gating layers, run on every change:

- **Host unit tests** — `make test-unit`: **18720 assertions** across 21 suites
  that exercise the kernel objects and pure logic directly (cspace, cnode,
  kendpoint, kreply, knotification, kuntyped including its two-ended carve,
  kschedctx, kframe, the MDB/CDT (structural + model-based fuzzing), rights,
  ipc_cspace, the root-task BootInfo builder, vfs_ep including the file-grant
  layer, …). They cover what a successful boot cannot show: buffer bounds on a
  page about to be mapped into ring 3, a CSpace that names itself, and an
  allocator's two ends meeting exactly once.
- **Runtime tests** — booted under QEMU headless: **276 tests** covering IPC and
  syscall basics, CPtr-first slots, badges & sender identity, service lifecycle /
  death-restart / relookup, endpoint cap-transfer, device/driver isolation,
  service supervision, the user pager and fault model, file-backed memory,
  VFS-enforced file grants, multi-target paging, resource ownership accounting,
  the canonical Untyped-born TCB lifecycle (T284–T287), the native MDB/CDT with
  cross-process revocation (T288–T290), one-capability-one-authority (T296),
  a retyped TCB executing (T297), the Stage 6 budget invariants (T298–T300),
  a refused address-space retype leaving its budget untouched (T301), the page
  table as a capability (T302), and a running thread outliving every capability
  to it (T303).
- **Purity gate** — `make check-purity`: the frozen legacy-consumer allowlist.
  Nothing handle-table-shaped is left in it (Stage 4 deleted the namespace); what
  it holds is the kslab inventory, which Stage 6 reduced to the boot path. It
  can only shrink, and refusing a change that MOVES a use from one file to
  another is part of how it does that.

```bash
make                                                       # zero-warning build
make check-purity                                          # seL4 purity allowlist
make test-unit                                             # host unit suites (18720)
make smoke-runtime                                         # headless runtime lane
ENABLE_RUNTIME_SELFTESTS=1 make smoke-runtime-selftests    # + full self-test suite (276/276)
make run                                                   # interactive QEMU
```

Zero-warning policy: the build is treated as broken if
`gcc -Wall -Wextra -Wshadow -Wundef` emits any diagnostic.

## Convergence status

The kernel is measured against seL4 by a [roadmap](docs/architecture/sel4-convergence-roadmap.md)
of numbered stages and a [ledger](docs/architecture/sel4-convergence-ledger.md)
of every mechanism that is not yet seL4's. Where it stands:

| Stage | State |
|---|---|
| 0–4 — TCB consolidation, CDT/MDB, CSpace-only transfer/derive, dual-namespace retirement | closed; the handle namespace is **deleted**, not deprecated |
| 5 — seL4-like bootstrap | closed: the root task is *told* what it holds (BootInfo), one capability per authority, and a retyped TCB executes |
| 6 — remaining memory and objects | closed: no kernel object and no page of user-visible memory is created from kernel-private storage after boot |
| **6-pure — the user retypes what the kernel charged** | **closed**: every object that constitutes an address space or a CSpace is retyped by its holder and handed over |
| **7 — KProcess retirement** | **in progress**: the last pool-born thread and one invented ceiling are gone; `KProcess` itself needs the user-space process server |
| 8–10 — MCS, SMP, platform | not started, by sequencing |

**How close is this to seL4?** Two answers, and they are far apart: the
authority model is close, the kernel architecture is not. The roadmap's
[How close is this to seL4](docs/architecture/sel4-convergence-roadmap.md#how-close-is-this-to-sel4)
grades each dimension against seL4 rather than against the roadmap's own
progress, and names the two things no further stage closes — threads that block
inside the kernel (ledger D-1), and an ABI that numbers its syscalls where seL4
invokes capabilities (charter §6, permanent by decision).

**What Stage 6-pure changed.** Stage 6 answered *who pays* for memory: the
kernel creates the object and charges it to an Untyped the caller named. That
is not seL4's answer, and the ledger recorded the gap as D-5. Stage 6-pure
gives seL4's answer for address spaces — the **holder** retypes the object and
hands it over:

- a **page table** is retyped (`IRIS_KOBJ_PAGE_TABLE`) and installed
  explicitly (`SYS_VSPACE_MAP_TABLE`); the kernel creates none, and a map whose
  walk is incomplete answers `IRIS_ERR_MISSING_TABLE` rather than quietly
  spending somebody's budget;
- an **address space** is retyped (`IRIS_KOBJ_VSPACE`, its region *is* the
  PML4) and passed to `SYS_PROCESS_CREATE`, together with a root **CNode** the
  spawner also retyped — which is `seL4_TCB_Configure`'s shape;
- the **bootstrap exception ends**: the root task's pre-run maps are the only
  kernel-funded ones, and they stop the moment it can speak for itself. Six
  levels, measured. After that no address space is implicitly funded while
  anybody is running.

A consequence worth stating because it is a real authority change: **a task
that maps anything must hold a budget**, since retyping a level needs untyped
memory. The spawner decides — `vfs` and the pager get one, `console` and `kbd`
do not, and `lifecycle_probe` gets one only in the spawn where it acts as a
pager. Same image, two roles, different authority.

**What Stage 7 has done, and what it needs.** `SYS_THREAD_START` — the last
path by which a thread existed because the kernel had a free slot — is retired
and `task_thread_create` is deleted; a child's first thread is retyped,
configured, pointed and resumed like any other. The per-process page quota is
retired too: it was a ceiling nobody granted, contradicting the budget that
replaced it. What remains is `KProcess` itself, and that cannot land without
the thing that replaces the policy it carries — a user-space process server —
so the stage is open and the roadmap says why.

## What does not exist yet

IRIS is not a general-purpose OS yet — by sequencing, not by ambition: the
platform work is Stage 10 of the roadmap and lands only on a consolidated
microkernel (charter §5).  The current tree does not provide a persistent
disk filesystem, a mutable filesystem or writeback, networking, full SMP / AP
bringup (foundation present, scheduling is single-CPU), a global page cache,
copy-on-write, full ELF demand paging (the pager has the groundwork), a dynamic
linker, a POSIX layer, or hardware support beyond QEMU x86-64.

## Positioning

IRIS is a **pure capability-based microkernel of its own implementation, in
semantic convergence toward seL4/MCS**, with a real ring-3 service boundary and
headless validation on every change. The kernel owns boot, memory, paging/PCID,
the scheduler, syscall dispatch, capability enforcement, IRQ routing, fault
delivery, the typed object set, and first-task creation. It does **not** own VFS
logic, keyboard handling, console output, service discovery, supervision,
page-fault resolution, file-backed memory, or shell behavior — all of that is
ring-3 code talking over capability-secured endpoints.

The authority model is not aspirational. Derivation, delegation and revocation
are the native CSpace CDT/MDB and nothing else: one derivation tree, revoke that
is recursive across CNodes and processes, and every delegation — IPC transfer,
device capability, pre-start grant to a child — parented to the slot that
granted it, so it stays revocable by its grantor. There are no CPtr-to-handle
fallbacks. What remains transitional is recorded, dated to a stage and gated by
`make check-purity`, whose allowlist can only shrink — and did, in Stage 6, for
the first time since Stage 4: **32 of the 36 charter invariants are met**, with
A5 (ambient authority), O1 and M1 (every object from Untyped) and P2
(mechanism, not policy) PARTIAL and nothing PENDING, the remainder scoped to
Stage 7 of the
[convergence roadmap](docs/architecture/sel4-convergence-roadmap.md). Stages 0
through 6 are closed: bootstrap is fine-grained, a thread is a retyped object
configured through capabilities, and **no kernel object or page of user memory
is created from kernel-private storage after boot** — page tables, address
spaces, process state, VMO pages and device capabilities all come out of an
Untyped somebody named and can be reclaimed by resetting it.

Convergence is measured against seL4's *authority model*, which is where IRIS is
closest. It is not an architectural replica: the kernel still CHARGES memory
objects to a budget where seL4 has the user retype them, composes threads
through a `KProcess` object, and gives every thread its own kernel stack to
block on. All three are recorded — the first two as staged debt, the last as an
unscheduled structural divergence — in the
[ledger](docs/architecture/sel4-convergence-ledger.md).

Scope is deliberate, not provisional: IRIS targets QEMU x86-64 and grows
new capability — drivers, storage, networking, an optional POSIX personality —
**exclusively in user space**, without re-contaminating the kernel. It does not
claim formal verification; its invariants are proven by construction plus
adversarial tests, and that divergence is registered as permanent in the
[purity charter](docs/architecture/iris-sel4-purity-charter.md).

## Documentation

Three documents are normative and outrank the rest when they disagree:

| Document | Answers |
|---|---|
| [purity charter](docs/architecture/iris-sel4-purity-charter.md) | what IRIS may never do — the 36 invariants, the permanent prohibitions, and the registered deliberate divergences |
| [convergence roadmap](docs/architecture/sel4-convergence-roadmap.md) | what order the remaining work happens in, and what each stage must demonstrate to close |
| [convergence ledger](docs/architecture/sel4-convergence-ledger.md) | every non-seL4 mechanism still alive, who uses it, and when it retires — including the structural divergences that have **no** retirement stage (D-1..D-5) |

Per-stage design records: `stage5-root-task-bootinfo.md` (fine-grained boot
authority, BootInfo, executable retyped TCBs) and
`stage6-memory-from-untyped.md` (who pays for memory — closed, and superseded
in part by Stage 6-pure, which changed the answer from *charged* to *retyped*
for address spaces; the roadmap carries that record).

Design notes live in `docs/`, including `endpoint-protocol.md`,
`badges-sender-identity.md`, `service-lifecycle.md`, `cptr-first-services.md`,
`vmo-memory.md`, `memory-invariants.md` (safety **and** budget invariants),
per-service contracts under `docs/contracts/`, and deeper design records under
`docs/architecture/` (the user pager, file-backed memory, file-grant
capability, service supervision, device isolation, and more).
