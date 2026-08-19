# IRIS — seL4 Convergence Ledger (normative)

Record of hybrid debt: every non-seL4 mechanism still alive, who uses it, its
replacement and its retirement phase.

**Normative frame**: this ledger implements the
[seL4 purity charter](iris-sel4-purity-charter.md) (constitutional) and the
[convergence roadmap](sel4-convergence-roadmap.md) (dependency ordering). The
table's "removal phases" are read against the roadmap's Stages. The executable
guard `make check-purity` freezes the legacy handle-table / kslab consumers:
the allowlist only shrinks.

**Rule**: `no mechanism marked FROZEN may take new consumers`. Adding a
consumer to a FROZEN/ACTIVE_LEGACY entry is a review defect. The states are:
`ACTIVE_LEGACY` (in use, no migration underway) · `MIGRATING` (partial
migration) · `FROZEN` (new uses forbidden) · `RETIRED` (number/symbol
reserved, no functionality) · `REMOVED` (deleted).

| Legacy mechanism | Why non-seL4 | Current users | Replacement | Removal phase | New uses forbidden | State |
|---|---|---|---|---|---|---|
| `KProcess` | process as a kernel object = policy in the kernel | spawn/loader, supervision, fault info, accounting | user-space process server (TCB+CNode+VSpace+Untyped) | process-server | yes | FROZEN |
| `KVMO` (+`SYS_VMO_CREATE`/`SYS_VMO_CREATE_FOR`/`SYS_VMO_MAP*`) | memory object with policy (owner, quota, file-backing) | loader, pager, tests | memory server (Frames + pager) | memory-server | yes | FROZEN |
| `kslab` for dynamic objects | hidden global heap | KProcess, KVMO, KFrame header, KVSpace, KTcb, KIrqCap, KIoPort, KBootstrapCap, KInitrdEntry, KUntyped header, root CNode, handle table | Untyped retype | per family (see rows) | yes — no new canonical object may be born from kslab | MIGRATING |
| kslab for runtime KEndpoint/KNotification/KReply/CNode | same | — | RETYPE2 | S1 | — | REMOVED |
| notification owner quota (`KPROCESS_NOTIFICATION_QUOTA`) | numeric quota as creation source | — | Untyped is the budget | S1 | — | REMOVED |
| per-process VMO/page quotas (Fase 29) | resource domain parallel to explicit memory | KVMO/paging | Untyped | with KProcess/KVMO | yes | LEGACY_FOR_KPROCESS_KVMO (ACTIVE_LEGACY) |
| payer selection (`SYS_VMO_CREATE_FOR`) | per-payer accounting | svc_loader | Untyped delegation | with KVMO | yes | ACTIVE_LEGACY |
| `SYS_RESOURCE_INFO` notifs_* fields | mirror of a retired quota | tests (read 0) | — (additive, frozen at 0) | with KProcess | yes | TRANSITIONAL_DIAGNOSTICS |
| handle table / dual resolution | second authority namespace | — | CSpace | Stage 4 | n/a | **REMOVED (Stage 4)** — `HandleTable`, `KProcess.handle_table`, `handle_table.c/.h` and `test_handle_table.c` are DELETED, and `cspace_or_handle_resolve_*` is renamed `cspace_resolve_only_*`.  Twelve syscalls retired to `NOT_SUPPORTED` with their numbers reserved (15, 22, 43, 46, 52, 53, 59, 81, 87, 89, 90, 95).  Permanent gate: T095 pins handle-live, handle-delivery and TOCTOU at structural zero |
| `SYS_VMO_SHARE` (46) | placed a VMO capability in ANOTHER process's HANDLE TABLE: a cross-process handle producer.  The receiver could not name the grant in its CSpace, and it had no MDB edge to the sender's cap, so the grantor could not revoke it | — (tests only, since Fase 8) | `SYS_PROC_CSPACE_MINT` / `SYS_CSPACE_MINT_INTO` — installs into the target's root CNode as an MDB child of the caller's source slot | Stage 4 | — | **RETIRED (Stage 4)** |
| `SYS_HANDLE_INSERT` (59) | same defect, for any object type: inserted a capability directly into another process's handle table | — (tests only, since A1.8) | same | Stage 4 | — | **RETIRED (Stage 4)** |
| `SYS_IOPORT_RESTRICT` (43) | narrowed a KIoPort by fabricating a NEW KIoPort from kslab and publishing it as a HANDLE — device authority with no capability ancestor, so untraceable to its grantor and unrevocable by one | — (never called in-tree, not even by a test) | `SYS_CAP_CREATE_IOPORT` publishing into a CSpace slot as an MDB child of the authorising bootstrap cap | Stage 4 | — | **RETIRED (Stage 4)** |
| `SYS_CNODE_FETCH` (90) | copied a CNode slot's capability into a HANDLE — a slot→handle copy with no MDB edge back to the slot, so revoking the slot left the copy alive | — | `SYS_CSPACE_MINT` (slot→slot, records the derivation) | Stage 4 | — | **RETIRED (Stage 4)** |
| `SYS_ENDPOINT_CREATE` (73) | global fabrication without Untyped | — | RETYPE2 | S1 | — | RETIRED |
| `SYS_NOTIFY_CREATE` (19) | same + quota + handle | — | RETYPE2 | S1 | — | RETIRED |
| `SYS_CNODE_CREATE` (80) | same | — | RETYPE2 | S1 | — | RETIRED |
| implicit reply allocation (kreply in EP_CALL) | the kernel fabricated authority per call | — | explicit reply objects (recv arg2) | S1 | — | REMOVED |
| `SYS_UNTYPED_RETYPE` (87) handle-publishing | publishes authority as a handle | — | RETYPE2 | Stage 4 | — | **RETIRED (Stage 4)** — Fase S1 already refused the migrated family; Stage 4 refuses KUntyped / KFrame / KSchedContext too, since RETYPE2 accepts all three into a CSpace slot.  There is exactly ONE way to create an object from an Untyped, and it publishes into CSpace |
| `SYS_SC_CREATE` (83) | global SC create | none | RETYPE2 + SC_CONFIGURE + SC_BIND | S2 | — | RETIRED (Fase S2) |
| `kschedctx_alloc` (kslab SC) | SC payload in the global heap | none | RETYPE2 (`kschedctx_alloc_at`) | S2 | yes | REMOVED (Fase S2) |
| `struct task tasks[TASK_MAX]` (static pool) | backing for kstack + arch-context + scheduler linkage | scheduler, thread create | TCB payload from Untyped; array → pointer/generation registry | S2 (run-queue index→pointer + productive-path Untyped source) | yes — no new consumers outside the scheduler | ACTIVE_LEGACY (bounded static pool, NOT kslab; runtime TCB storage — REMOVE pending) |
| `task_rsp[TASK_MAX]` (index-keyed RSP array) | per-slot kernel RSP, parallel to the array | scheduler context switch | `struct task.saved_krsp` | S2 inc.2 | — | REMOVED (Fase S2 inc.2 — the scheduler's first indirection) |
| run-queue `next[TASK_MAX]`/`queued[TASK_MAX]` + `(t - tasks)`/`&tasks[idx]` | run-queue identity by array index | rq_enqueue/remove/dequeue | intrusive pointer lists (`t->rq_next`/`rq_queued`) | S2 inc.2B | — | REMOVED (Fase S2 inc.2B Block A — run queue 100% pointer-based) |
| `tasks[j]` timeout scans (tick/idle) + slot allocation | iteration over the backing array | scheduler_tick / sched_handle_idle / task alloc | iteration over `ktcb_registry[]` (pointers+generation) | S2 inc.2 Stage C | — | REMOVED as identity (Fase S2 Stage C — everything goes through `ktcb_registry[i].tcb`) |
| `KTcbRegistrySlot ktcb_registry[TASK_MAX]` | reference registry (tcb*/generation/occupied/bootstrap), NOT payload | scheduler/alloc/lookup | same registry; transitional capacity | — | transitional TASK_MAX limit | TRANSITIONAL_IMPLEMENTATION_CAPACITY (Stage C) |
| `struct task tasks[TASK_MAX]` (static payload) | real TCB backing (registers/kstack ptr/scheduler state) pointed to by `registry[i].tcb` | registry (scaffolding) | canonical KTCB in Untyped (Stage D) | S2 inc.2 Stage D | yes — scaffolding, no new consumers | ACTIVE_LEGACY (scaffolding; REMOVE in Stage D, except the idle bootstrap) |
| `SYS_THREAD_SET_SC` (85) | SC self-bind | existing scheduler code | `SYS_SC_BIND(sc,tcb)` by CPtr | — | yes — frozen | FROZEN (Fase S2 inc.1) |
| `struct KTcb` wrapper (kslab) | cap-visible TCB object in the heap, separate from the task | — | `struct task` IS the KTCB (KObject at offset 0) | S2 inc.2 | — | REMOVED (Fase S2 inc.2 — one structure, one identity) |
| executable thread-create via pool + handle (`SYS_THREAD_CREATE`/`task_create_user_impl`) | the thread EXECUTION path is born from the static pool and publishes a handle in the process table | spawn/loader, iris_test threads | retyped TCB (`RETYPE2(KOBJ_TCB)`, already present) + `TCB_CONFIGURE` with CSpace/VSpace caps (Stage 5/6, post-CDT) | Stage 5/6 | yes — no new thread-creation path | ACTIVE_LEGACY (the only executable path; the retyped TCB is cap-complete but inactive until TCB_CONFIGURE) |
| idle task (static backing, registry slot 0) | bootstrap TCB outside Untyped, with no cap-visible object | scheduler | root-task TCB from BootInfo (Stage 5) | Stage 5 | yes — isolated bootstrap exception, never retyped or reused | BOOTSTRAP_EXCEPTION |
| native CDT/MDB in CNode slots | — (it is the correct seL4 mechanism) | `SYS_CSPACE_MINT`/`MINT_INTO`/`REVOKE`, retype2, teardown, receive-slot | — | — | n/a | IMPLEMENTED (Fase S3 — recursive cross-process revoke; validator + fuzzing) |
| handle-tree derivation (all types) | parallel derivation tree hidden in the handle table | — | per-slot derivation via the native CDT | Stage 3 | n/a | **REMOVED (Fase S4)** — `SYS_CAP_DERIVE`(78)/`SYS_CAP_REVOKE`(79) retired to `NOT_SUPPORTED` (numbers reserved); the table's derived-insert, revoke-children and parent-array machinery deleted.  `legacy_handle_derivation_migrated` has ZERO callers and is a structural 0 — the retirement witness |
| MDB LEGACY roots (`MDB_FLAG_LEGACY_ROOT`) | caps with no provable CSpace ancestor (handle/bootstrap/IPC-delivery origin) | bootstrap (kernel_main), legacy `kcnode_mint*`, IPC receive-slot delivery | a real CSpace origin (retype/derive by CPtr) | Stages 2/4/5 | yes — closed allowlist, observable `mdb_legacy_roots` counter | ACTIVE_LEGACY (counted debt; must → 0) |
| IPC cap-transfer with a handle source | a transfer's source was resolved by handle, not by CPtr | — | CPtr source + CSpace receive slot | Stage 2 | n/a | **REMOVED (Fase S4)** — `syscall_ipc_stage_cap_peek_badged` resolves the source through `cspace_resolve_slot`; a handle value is `INVALID_ARG`. Delivery installs the cap as an MDB CHILD of the source slot, so `cdt_ipc_transfer` deliveries are no longer LEGACY_ROOTs |
| root CNode at `kprocess_alloc` (kslab) | runtime CNode outside Untyped | every spawn | the spawner supplies a retyped CNode (process-server) | process-server | yes | ACTIVE_LEGACY |
| root CNode reachable only via `cspace_root_h` (handle) | the CSpace ROOT is located through the handle table | — | `KProcess.cspace_root`: a structural back-reference holding one lifecycle + one active ref, released in `kprocess_teardown` | Stage 4 | n/a | **REMOVED (Fase S4, Etapa 4)** — resolving a CPtr no longer touches a handle table, and the cross-process paths (`SYS_CSPACE_MINT_INTO`, `SYS_PROC_CSPACE_MINT`, retype2, IPC receive-slot delivery) no longer read ANOTHER process's handle table to find its root.  Allowlist: `handle_table_get_object` 52 → 40 (14 files → 11), `handle_table_insert` 42 → 41 |
| implicit page-table allocation (PMM reserve on map) | kernel memory hidden by mapping | paging | PageTable objects from Untyped | frame/page-table | yes | ACTIVE_LEGACY |
| KFrame header sidecar (kslab) | metadata outside the region | frame retype | header inside the Untyped | frame/page-table | yes | ACTIVE_LEGACY |
| process-level fault record (one per process) | belongs on the TCB | fault delivery (Fase 20/25) | per-TCB fault / fault EP | process-server | yes | ACTIVE_LEGACY |
| `SYS_PROCESS_VSPACE` (107) | process authority → VSpace by handle | supervisors/pager tests | CSpace mint of the VSpace cap | process-server | yes | ACTIVE_LEGACY |
| `SYS_BOOTCAP_RESTRICT` (dual-namespace split brain) | `arg0` is resolved with `cspace_or_handle_resolve_obj` (CPtr **or** handle), but the restricted clone is published with `handle_table_replace(ht, (handle_id_t)arg0, …)` — the two halves disagree about which namespace `arg0` is in | init (fb spawn cap), svcmgr (post-bootstrap strip) — **both pass handles**, so no live defect | publish the clone into a CSpace destination slot as an MDB child of the source slot, the way retype2/mint already do | Etapa 4 | yes | ACTIVE_LEGACY — **blocks the spawn-cap CPtr migration**.  A CPtr has generation 0 and every live handle slot has generation ≥ 1, so `handle_table_replace` rejects it with `BAD_HANDLE`: no corruption, but the syscall silently cannot succeed by CPtr.  Migrating `spawn_cap_h` to a CPtr before fixing this turns a working restriction into a no-op error path — i.e. a capability that was supposed to be narrowed stays wide |
| `KBootstrapCap` (monolith) | one object carried spawn, hardware, debug and framebuffer authority at once | — | one capability per authority + structured BootInfo | Stage 5 | n/a | **REMOVED (Stage 5 Etapa 2)** — the object type survives as the carrier of a SINGLE authority (`kind`), and `kbootcap_alloc` refuses a zero or multi-bit kind, so a monolithic boot capability cannot be constructed.  Six capabilities (process, initrd, IRQ, ioport, debug, framebuffer) are published one per slot and described in BootInfo v4.  `SYS_BOOTCAP_RESTRICT` (45) is RETIRED with its number reserved; `kbootcap_allows` and `kbootcap_clone_restricted` are deleted; `BOOT_CPTR_BOOTSTRAP_CAP` (slot 1) is permanently empty.  Least-authority result: vfs, a file server, no longer holds the authority to create processes.  T296 pins the split; T291 died with the mechanism.  History: Etapa 1-2b — Etapa 2a/2b SPLIT device and debug authority out: `IRIS_BOOTCAP_HW_ACCESS` (one bit for both IRQ and ioport creation) is replaced by two capabilities matched EXACTLY by the kernel, published one per slot and recorded in BootInfo v2.  svcmgr renounces hardware authority by DELETING those slots instead of cloning a narrowed monolith.  Etapa 2b did the same for `IRIS_BOOTCAP_KDEBUG` (kernel log, scheduler statistics, poweroff), published at `BOOT_CPTR_DEBUG_CONTROL` and delivered to children in the retired `IRIS_CPTR_SVC_REPLY` slot.  Etapa 2c split the rest; T296 pins the split, T291 keeps `SYS_BOOTCAP_RESTRICT` honest until it retires.  Etapa 1 — the structured BootInfo EXISTS (`struct iris_root_bootinfo`): the root task is told its initial caps by CPtr, the shape of its root CNode and every boot Untyped with its physical region, instead of agreeing with the kernel on constants and probing slots until one answered `NOT_FOUND`.  The object itself is unchanged and still carries the four-bit permission mask; splitting it into fine-grained caps and retiring `SYS_BOOTCAP_RESTRICT` is Etapa 2 |
| `KInitrdEntry` + `SYS_INITRD_*` | filesystem-aware kernel state | loader | user-space VFS/loader | process-server | yes | ACTIVE_LEGACY |
| kernel stacks / PML4 from the PMM reserve | allocation outside Untyped | task/process create | TCB/VSpace from Untyped | process/frame phases | yes | ACTIVE_LEGACY |
| `KChannel` | — | — | endpoints | Fase 13 | — | REMOVED |
| hardcoded ioport whitelist (`kioport_whitelist`, syscall_priv.h) | device policy in the kernel | kbd/console/fb/userboot via svcmgr | fine-grained ioport caps issued by the root task (BootInfo) | Stage 5 | yes — no new entry without a citation to charter §2.6/P3 | ACTIVE_LEGACY (temporary bootstrap) |
| `SYS_CAP_CREATE_IRQCAP`/`_IOPORT` as handle producers | device authority existed ONLY as a handle, leaving the legacy handle tree as its sole derive/revoke mechanism | — | slot publication (arg3) parented to the authorising bootstrap-cap slot | Stage 3 prep | n/a | **REMOVED (Fase S4)** — both publish into CSpace as MDB children of the bootstrap cap; device caps now derive/revoke through the native CDT |
| `svc_mint.src_h` (handle-sourced pre-start delegation) | the loader mints a child's caps from the supervisor's handle table | all non-device mints in svcmgr/init/userboot | `svc_mint.src_cptr` + `SYS_CSPACE_MINT_INTO` | Stage 4 | yes for device caps (already migrated) | MIGRATING (device caps done; endpoints/untyped/reply still handle-sourced) |
| IPC delivery into the receiver's handle table (`syscall_ipc_deliver_cap_badged`) | a capability entered a process through the handle namespace because the receiver declared no destination — not a choice either side made | — | the receiver declares a receive slot; an undeclared receive gets the message WITHOUT the capability, and the sender's source slot is untouched | Stage 4 | n/a | **RETIRED (Stage 4)** — the destination half of charter I1.  `iris_ipc_stat_handle_deliveries` is a structural 0 (T095 pins it, T096 proves 32 consecutive deliveries all land in slots) |
| TOCTOU receive-slot→handle fallback (`syscall_ipc_deliver_cap_routed`) | CSpace-to-handle delivery degradation | — | fail closed: no cap delivered, source slot untouched | Stage 2 | n/a | **REMOVED (Fase S4)** — the last permitted degradation is gone; `iris_ipc_stat_toctou_fallbacks` is a structural 0 pinned by T094 (forces the race) and T095 (asserts the counter never moves) |

### A-4 — CSpace-native introspection replaces the CPtr→handle bridge

**Change**: two new CSpace-only syscalls, `SYS_CAP_IDENTIFY` (117) and
`SYS_CAP_SAME_OBJECT` (118).  `SYS_HANDLE_TYPE` (52) and
`SYS_HANDLE_SAME_OBJECT` (53) become legacy-only and retire with the handle
namespace.

**Justification**: charter §3 forbids new handle producers and consumers, not
new CPtr-only surface, and these are the mechanism by which three productive
services LEFT the handle namespace.  `SYS_CSPACE_RESOLVE` was being used to
answer two questions that are not about handles at all — "what type is the cap
in this slot" (svcmgr's delivered-cap dispatch) and "is this slot occupied"
(the `pager` / `lifecycle_probe` manifest oracles).  Answering them cost a
real handle-table entry per call, i.e. the probe took AUTHORITY to learn a
FACT, and the pager's version had no ceiling — one leaked entry per occupied
slot, per request, against a 256-entry table.

The replacements are strictly weaker than what they retire: they return a
scalar, produce no capability, retain no reference past the call, and resolve
only against the invoker's own CSpace root.  They take a CPtr and nothing
else — a handle value is `INVALID_ARG` with no fallback, so they are not dual
resolvers (§3.6) and add no degradation path (§3.7).

**Scope**: no invariant changes state.  The allowlist SHRINKS: three services
stop calling the bridge, and `handle_table_insert` loses its
`task_lifecycle.c` entry.  A6/A3 gain their last productive-path evidence
before Stage 4 closes.  Tests: T292, T293; T148 moves its unassigned-number
floor to 119.

### A-5 — CPtr resolution is injective; receive slots are full CPtrs

**Change**: `cspace_resolve_cap_badged`, `cspace_resolve_slot` and the new
`cspace_resolve_dest_slot` reject a CPtr with leftover bits at a non-CNode
terminal (`IRIS_ERR_INVALID_ARG`).  The IPC receive slot is a full CPtr rather
than a direct root index.  `IRIS_CPTR_LIMIT` becomes `HANDLE_TAG`.

**Justification**: charter A3 says the CPtr is the capability identifier.  An
identifier that is not injective does not identify: resolution discarded the
bits it had not consumed, so in a 256-slot root every capability answered to
`k`, `k+256`, `k+512`, … — about 2^23 addresses each.  The suite's own
"invalid in both namespaces" fuzz constant 4095 aliased root slot 255, the
serial `KIoPort` it prints through.  seL4 rejects the same shape as a depth
mismatch.

The receive-slot restriction was the other half: a slot had to be a direct
index into the root CNode, so a process with a full root could not receive a
capability at all.  That is Stage 4's stated second-order benefit made
concrete — multi-level CSpace is unusable if the one operation that *installs*
a capability into your CSpace cannot address past the first level.

**Scope**: no invariant changes state; A3's "identifier" reading is now
literally true.  No allowlist movement.  Tests: T294, T295, and host cases in
`tests/kernel/test_cspace.c`.

### A-6 — the root task's BootInfo region

**Change**: the kernel builds a structured `struct iris_root_bootinfo` and maps
it read-only / non-executable into the root task before it starts; its address
travels in RBX.  `userboot` validates it and delegates the CPtrs it names
instead of the constants it used to assume.

**Justification**: charter §4 requires "bootstrap with fine-grained
capabilities (structured BootInfo; no monolithic `KBootstrapCap`)", and this is
its first half.  The mechanism it retires is a compile-time convention plus
probing: the root task learned its own CSpace from constants the kernel happened
to share, and counted its untypeds by invoking slots until one failed.  That is
not a contract — nothing detects the moment the two sides disagree — and
Stage 4's own experience (three bring-up failures from slot collisions) is what
it costs.

**Not a new authority path**: the region is read-only, confers nothing, and
every CPtr in it names a slot the kernel had already populated.  §3.5 forbids an
address substituting for a capability; a description of capabilities is not a
substitute for one, and no syscall accepts "BootInfo said so".  The converse
rule is enforced instead: a capability that cannot be described is not granted,
so the untyped drain is bounded by the description.

**Scope**: no invariant changes state; `KBootstrapCap` moves ACTIVE_LEGACY →
MIGRATING.  No allowlist movement.  Tests: RBI-1..RBI-10
(`tests/kernel/test_root_bootinfo.c`); the boot is the runtime witness, since an
unreadable or untrue BootInfo halts userboot with a serial diagnostic.

### A-8 — `SYS_BOOTCAP_RESTRICT` retires; a monolithic boot capability cannot exist

**Change**: number 45 answers `NOT_SUPPORTED` and stays reserved.
`kbootcap_alloc` refuses a kind that is zero or has more than one bit set;
`kbootcap_allows` and `kbootcap_clone_restricted` are deleted.  The loader API
takes a process capability and an initrd capability instead of one "spawn cap".

**Justification**: charter §4 requires bootstrap with fine-grained capabilities
and no monolithic `KBootstrapCap`.  Narrowing by cloning was the mechanism that
made a monolith survivable; removing the monolith removes its reason to exist,
and leaving it live would leave a supported way to build the thing that was
retired.  The split of `SPAWN_SERVICE` into process and initrd authority is the
measurable part: vfs held process-creation authority solely because reading a
boot image required the same bit.

**Scope**: `KBootstrapCap` (monolith) moves MIGRATING → REMOVED.  No allowlist
movement (the type is still one kslab consumer, Stage 6's inventory).  Tests:
T296 extended, T148 pins 45, T291 retired, T134's empty-slot probe moved to a
self-deleted scratch slot.

### A-7 — one capability, one authority (boot control capabilities)

**Change**: `IRIS_BOOTCAP_HW_ACCESS` is deleted and replaced by
`IRIS_BOOTCAP_IRQ_CONTROL` and `IRIS_BOOTCAP_IOPORT_CONTROL`, each carried by
its own capability, each matched by EXACT equality (`kbootcap_is`) rather than
the subset test used for the remaining mask bits.

**Justification**: charter §4 requires bootstrap with fine-grained
capabilities, and A5 (no ambient authority) is only as strong as what a
capability *bundles*.  One bit authorised both interrupt-line and I/O-port
creation, on an object that also carried spawn, debug and framebuffer
authority, so a service that needed a serial port was handed the authority to
claim any IRQ and power the machine off.  Exact matching is what makes the
split irreversible: a capability that merely contains the bit is refused, so
nothing can drift back to authorising by subset.

**Not a new mechanism**: no syscall number, signature or resolver changes;
`SYS_CAP_CREATE_IRQCAP` / `SYS_CAP_CREATE_IOPORT` take the same argument and
publish into the same destination slot as MDB children of the authorising slot.
What changed is WHICH capability is accepted.

**Scope**: `KBootstrapCap` stays MIGRATING; no allowlist movement.  svcmgr's
hardware renunciation stops going through `SYS_BOOTCAP_RESTRICT` and becomes a
slot delete, which removes one of that syscall's two remaining callers.  Tests:
T296 (new), T069 and T291 re-anchored.

## Checkpoint C.1 — Versioned user-buffer ABI (Fase S2)

`SYS_UNTYPED_QUERY` (arg0 = kind|version<<16|size<<32) and `SYS_RESOURCE_INFO`
(arg2 = user_size) know the caller-declared size and write at most
`min(user_size, kernel_size)` (prefix-compatible): an older/smaller caller
cannot overflow. Minimum header (8 B) and an unsupported version →
`IRIS_ERR_INVALID_ARG` without writing. Helper `copy_versioned_to_user`.
Audit of versioned queries:

| Query | Version | Size field | Copy bound | Prefix-compat | Action |
|---|---|---|---|---|---|
| SYS_UNTYPED_QUERY (1..4) | arg0 bits16-31 | arg0 high32 | min(user,kernel) | yes | HARDENED |
| SYS_RESOURCE_INFO | struct.version | arg2 | min(user,kernel) | yes | HARDENED |
| SYS_TCB_GET_INFO (iris_tcb_info) | — | fixed | fixed sizeof | n/a | FIXED-SIZE (stable, does not grow) |
| SYS_PROCESS_FAULT_INFO | — | fixed FAULT_MSG_LEN | fixed | n/a | FIXED-SIZE |
| SYS_SCHED_INFO ext tiers | tier-gated | bounded `want` | bounded | partial | REVIEWED (bounded per tier) |

Test: T283 (QABI1–10 + guard canaries). Future new fields in a query struct
can no longer overflow a caller that declares its size.

### A-3 — ambient KDEBUG authority removed

**Change**: charter §2.1, invariant A5 — record that the KDEBUG handle-table
scan is deleted.

**Justification**: `SYS_KLOG_DRAIN`, `SYS_SCHED_INFO` and `SYS_POWEROFF` gated
on `task_has_kdebug_cap`, which scanned the caller's entire handle table for
any `KBootstrapCap` carrying `IRIS_BOOTCAP_KDEBUG`.  The caller passed no
capability and proved nothing: authority came from *possession somewhere*, not
from invocation — the definition of ambient authority.  It also could not see a
bootstrap capability held in CSpace, so migrating a service off handles
silently revoked its KDEBUG (this is how it was found: svcmgr's boot-log drain
went dead and the headless gate caught the missing scheduler marker).

All three now take the authorising capability as a CPtr in a previously unused
argument.  Every in-tree caller names it, so the scan is deleted outright
rather than left as a zero-argument fallback.

**Scope**: A5 stays PARTIAL — the ioport whitelist and the per-process kernel
quotas are still ambient and are Stage 6/7 work.  One of its three named
sources is gone.

## Charter amendments

The [purity charter](iris-sel4-purity-charter.md) may only be amended in a
change that cites it explicitly and records the amendment here.

### A-1 — invariant O1 pointed at a closed stage

**Change**: charter §2.2, invariant O1 — the pending-work reference
`(Stages 0/6)` becomes `(Stages 5/6)`.

**Justification**: Stage 0 (TCB consolidation) closed in Fase S2 inc.2, so O1
named an already-closed stage as the home of work that is still open.  The
replacement path for the residue — `TCB_CONFIGURE` over a retyped TCB, whose
arguments (CSpace root, VSpace, fault EP) only exist as caps after the
seL4-like bootstrap — is assigned to Stage 5/6 by both the roadmap (Stage 0,
"Recorded debt") and this ledger (row: *executable thread-create via pool +
handle*).  The charter was the only document out of sync.

**Scope**: editorial.  No invariant changes state, no allowlist entry moves,
no prohibition is added or lifted.  O1 remains PARTIAL and the count of met
invariants remains 29 of 36.

### A-2 — allowlist growth: two half-migrated notification arguments

**Change**: `scripts/purity_allowlist.txt` — `cspace_or_handle_resolve_` grows
by 1 in `syscall_proc.c` (8 -> 9) and 1 in `syscall_irq.c` (7 -> 8).
`handle_table_get_object` shrinks by 1 in each (`syscall_proc.c` 1 -> 0, the
file leaves that list entirely; `syscall_irq.c` 2 -> 1).

**Justification**: `SYS_PROCESS_WATCH` and `SYS_EXCEPTION_HANDLER` resolved
their *process* argument through the dual resolver while their *notification*
argument stayed handle-only.  A caller holding its notification in CSpace could
neither arm a watch nor register a fault handler — the half-migration was
itself the barrier to migrating anything else.  The growth is a strict trade of
a handle-namespace-only consumer for a dual one, on the same argument; no new
authority path appears, and the dual resolver's handle leg is deleted wholesale
when the namespace retires.

**Scope**: the allowlist's net movement is -2 handle-table consumers, +2 dual
resolvers.  No invariant changes state, no prohibition is added or lifted.
Precedent: Fase S3 grew the same list by 3 under this clause.

**Extended**: `SYS_IRQ_ROUTE_REGISTER` is the third occurrence of the identical
shape — irqcap (arg0) and owning process (arg2) resolved either way, the
destination notification (arg1) did not — so a service holding its IRQ
notification in CSpace could not register a route.  Same trade:
`syscall_irq.c` `handle_table_get_object` 2 -> 0 (the file leaves that list
entirely), `cspace_or_handle_resolve_` 7 -> 9.

**Pattern worth naming**: all three were syscalls whose *object* arguments were
migrated while their *notification* argument was left behind.  Any syscall
taking a notification alongside an already-dual argument should be assumed to
have it until checked.

## Non-regression guard

- T251 pins the closed manifest of RETYPE2-creatable types.
- T260 pins the retirement of the create syscalls and their no-effect.
- T125/T126 pin the rejection of the migrated family on the legacy retype.
- The `IRIS_KOBJ_* == KOBJ_*` asserts pin the type ABI.
- Review: any PR that adds `kslab_alloc` for a canonical type, a new
  `SYS_*_CREATE`, or a new handle-first resolver for canonical objects must be
  rejected citing this ledger.
