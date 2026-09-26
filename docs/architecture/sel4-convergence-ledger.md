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
| `KProcess` | process as a kernel object = policy in the kernel | none — the object does not exist | nothing: a process IS threads configured with the same CSpace and the same VSpace | Stage 7-proc (done) | n/a | **REMOVED (Stage 7-proc).**  `struct KProcess` is deleted; `SYS_PROCESS_CREATE` answers `NOT_SUPPORTED` and `KOBJ_PROCESS` is a reserved enumerator no live capability carries.  The **user-space process server this row named as the replacement was never built and is not scheduled** — there was no policy left to replace once every piece went to the object it concerned, and a supervisor holding its children's threads (and their address spaces when it maps into them) IS the child table that server would have kept, in the place seL4 puts it.  The history, kept because the steps are the argument: **Stage 7 SUBSTITUTED one consumer, it did not add one.**  `SYS_TCB_CONFIGURE` gained a process argument (arg3; 0 still means "my own") in the same change that retired `SYS_THREAD_START`, which consumed `KProcess` for the same authority.  The surface shrank by a syscall and `task_thread_create` went with it.  Recorded because the frozen rule reads "no new consumers", and a reviewer running it against this diff should see the trade rather than have to reconstruct it.  The page quota and the live-process ceiling it carried are retired (Stage 7 Steps 2-3).  **Stage 7 Steps 4-7 took the hot path off it entirely**: a thread holds its own CSpace root and its own VSpace — the two capabilities `SYS_TCB_CONFIGURE` already named — so resolving a CPtr and switching CR3 no longer read `KProcess`; the fault record moved to the thread that takes it; and a fault is ANSWERED by naming that thread's capability, delivered into a mailbox the registrant named, rather than by a global task id — and READ off that thread too (Step 8), which left the pager holding no authority over the processes it serves.  `user_cr3` and `pcid` are gone (the tag belongs to the walk); `cr3` survives as a declared cache for teardown.  Steps 9-10 took the rest of the reachable surface: a supervisor names the CSpace or address space it means (`SYS_CSPACE_MINT` with a destination CNode, `SYS_VMO_MAP_INTO` with a VSpace) instead of the process holding it, and a death is observed on the THREAD that dies (`SYS_TCB_WATCH` / `SYS_TCB_EXIT_CODE`).  Step 11 moved the address-space reclamation off the death event and into the VSpace's own destructor, so a walk comes down when its last capability does; Step 12 armed faults on the EXECUTION that takes them (`SYS_TCB_SET_FAULT_HANDLER`), which retired the process-scoped registration AND the process-scoped read that Step 8 had deliberately kept — a spawner that supervises keeps its child's first thread, so the supervisor that had only a process capability holds a thread capability.  Steps 13-15 finished the authority side: killing a child is stopping the EXECUTION a supervisor holds (SYS_TCB_EXIT), a never-started process is reclaimed by deleting the last capability to it (there is no creation reference any more — a thread that joins takes a real one), the DEFAULT BUDGET is gone (every allocating syscall names the Untyped that pays, so `mem_pool` is `storage_pool`: an anchor for the object's own block and nothing else), and a child's ADDRESS SPACE is handed over by the spawner that retyped it rather than read out of a KProcess.  Eleven syscalls retired outright — 26, 28, 29, 35, 47, 58, 71, 104, 105, 107, 116 — plus the fault selector in Step 7.  What was LEFT after Step 15 was not authority but IDENTITY, and **Stage 7-mem and 7-proc took that too**: the KVmo owner relation and its quota went to the Untyped a VMO is carved from, the IRQ route owner to the notification the route is bound to, `SYS_RESOURCE_INFO` to `SYS_UNTYPED_INFO`/`QUERY`, and `SYS_TCB_CONFIGURE`'s identity check went outright — the CSpace and VSpace you name no longer have to agree with a third object, which is `seL4_TCB_Configure`.  Three bugs had to be fixed first, each real: a thread held no ACTIVE reference on its address space (so a spawner deleting its own capability invalidated the space its child was about to run in); a slot naming its own CNode took an active reference, which is a reference-counting lie that kept a self-naming CSpace from ever emptying; and 53 syscall guards asked whether a task HAD A PROCESS when they meant whether it could NAME anything |
| `KVMO` (+`SYS_VMO_CREATE`/`SYS_VMO_CREATE_FOR`/`SYS_VMO_MAP*`) | memory object with policy (owner, quota, file-backing) | loader, pager, tests | memory server (Frames + pager) | Stage 7 (memory server) | yes | **CONVERTED (Stage 6 Step 5), still FROZEN as an object** — a VMO's pages, page-address array and header are carved from an Untyped, and `SYS_VMO_CREATE` / `SYS_INITRD_VMO` take the budget as a CPtr so the caller says WHICH of its budgets pays.  What remains non-seL4 is the OBJECT: a VMO is still a kernel-side memory abstraction with an owner and a quota where seL4 has only Frames.  That retires with the memory server |
| `kslab` for dynamic objects | hidden global heap | KVMO, KFrame header, KVSpace, KIrqCap, KIoPort, KBootstrapCap, KInitrdEntry, KUntyped header, root CNode — all of it now the BOOT PATH only (KProcess and the handle table are deleted; KTcb is retyped) | Untyped retype | per family (see rows) | yes — no new canonical object may be born from kslab | MIGRATING — 17 permitted occurrences across 14 files, gated by `make check-purity` |
| kslab for runtime KEndpoint/KNotification/KReply/CNode | same | — | RETYPE2 | S1 | — | REMOVED |
| notification owner quota (`KPROCESS_NOTIFICATION_QUOTA`) | numeric quota as creation source | — | Untyped is the budget | S1 | — | REMOVED |
| per-process VMO/page quotas (Phase 29) | resource domain parallel to explicit memory | none | Untyped | Stage 7 (done) | n/a | **BOTH GONE.**  The VMO-COUNT quota is **DELETED (Stage 7-mem)** with the owner relation it counted against — a VMO's accounting is the Untyped it was carved from, which `SYS_UNTYPED_QUERY` reports to whoever holds that budget.  Previously, on the page quota: **RETIRED (Stage 7)** — since Stage 6-pure a VMO's pages come from an Untyped the caller NAMED, so `phys_pages_limit` was a second ceiling the kernel invented, contradicting the model rather than reinforcing it: a holder with a large delegated budget still stopped at 8 MB nobody granted.  It reports 0 ("no kernel ceiling"), the way the notification quota did in Phase S1; the counters stay as instrumentation.  The VMO-COUNT quota is still ACTIVE_LEGACY and retires with the KVMO object (memory server) |
| payer selection (`SYS_VMO_CREATE_FOR`) | per-payer accounting | svc_loader | Untyped delegation | with KVMO | yes | **RETIRED (Stage 10-abi walk).**  KVMO is gone (D-5) and so is the payer argument; nothing in the kernel names either.  The row said "with KVMO" as its retirement stage and KVMO retired without anybody coming back to it — which is the failure charter §5.1 exists to stop, found by the §5.1 walk itself |
| `SYS_RESOURCE_INFO` | per-process resource domain in the ABI | none | `SYS_UNTYPED_INFO` (one budget) and `SYS_UNTYPED_QUERY` kind GLOBAL (the three fields that were never per-process: kslab occupancy, failed charges, rollbacks) | Stage 7-mem (done) | n/a | **REMOVED (Stage 7-mem)** — the syscall answers `NOT_SUPPORTED`, number reserved.  Its per-process half went with the domain; its global half moved where the rest of the global instrumentation already lived, so the drift tests that end most of the suite lost nothing |
| handle table / dual resolution | second authority namespace | — | CSpace | Stage 4 | n/a | **REMOVED (Stage 4)** — `HandleTable`, `KProcess.handle_table`, `handle_table.c/.h` and `test_handle_table.c` are DELETED, and `cspace_or_handle_resolve_*` is renamed `cspace_resolve_only_*`.  Twelve syscalls retired to `NOT_SUPPORTED` with their numbers reserved (15, 22, 43, 46, 52, 53, 59, 81, 87, 89, 90, 95).  Permanent gate: T095 pins handle-live, handle-delivery and TOCTOU at structural zero |
| `SYS_VMO_SHARE` (46) | placed a VMO capability in ANOTHER process's HANDLE TABLE: a cross-process handle producer.  The receiver could not name the grant in its CSpace, and it had no MDB edge to the sender's cap, so the grantor could not revoke it | — (tests only, since Phase 8) | `SYS_PROC_CSPACE_MINT` / `SYS_CSPACE_MINT_INTO` — installs into the target's root CNode as an MDB child of the caller's source slot | Stage 4 | — | **RETIRED (Stage 4)** |
| `SYS_HANDLE_INSERT` (59) | same defect, for any object type: inserted a capability directly into another process's handle table | — (tests only, since A1.8) | same | Stage 4 | — | **RETIRED (Stage 4)** |
| `SYS_IOPORT_RESTRICT` (43) | narrowed a KIoPort by fabricating a NEW KIoPort from kslab and publishing it as a HANDLE — device authority with no capability ancestor, so untraceable to its grantor and unrevocable by one | — (never called in-tree, not even by a test) | `SYS_CAP_CREATE_IOPORT` publishing into a CSpace slot as an MDB child of the authorising bootstrap cap | Stage 4 | — | **RETIRED (Stage 4)** |
| `SYS_CNODE_FETCH` (90) | copied a CNode slot's capability into a HANDLE — a slot→handle copy with no MDB edge back to the slot, so revoking the slot left the copy alive | — | `SYS_CSPACE_MINT` (slot→slot, records the derivation) | Stage 4 | — | **RETIRED (Stage 4)** |
| `SYS_ENDPOINT_CREATE` (73) | global fabrication without Untyped | — | RETYPE2 | S1 | — | RETIRED |
| `SYS_NOTIFY_CREATE` (19) | same + quota + handle | — | RETYPE2 | S1 | — | RETIRED |
| `SYS_CNODE_CREATE` (80) | same | — | RETYPE2 | S1 | — | RETIRED |
| implicit reply allocation (kreply in EP_CALL) | the kernel fabricated authority per call | — | explicit reply objects (recv arg2) | S1 | — | REMOVED |
| `SYS_UNTYPED_RETYPE` (87) handle-publishing | publishes authority as a handle | — | RETYPE2 | Stage 4 | — | **RETIRED (Stage 4)** — Phase S1 already refused the migrated family; Stage 4 refuses KUntyped / KFrame / KSchedContext too, since RETYPE2 accepts all three into a CSpace slot.  There is exactly ONE way to create an object from an Untyped, and it publishes into CSpace |
| `SYS_SC_CREATE` (83) | global SC create | none | RETYPE2 + SC_CONFIGURE + SC_BIND | S2 | — | RETIRED (Phase S2) |
| `kschedctx_alloc` (kslab SC) | SC payload in the global heap | none | RETYPE2 (`kschedctx_alloc_at`) | S2 | yes | REMOVED (Phase S2) |
| `struct task tasks[TASK_MAX]` (static pool) | backing for kstack + arch-context + scheduler linkage | scheduler, thread create | TCB payload from Untyped; array → pointer/generation registry | S2 (run-queue index→pointer + productive-path Untyped source) | yes — no new consumers outside the scheduler | **RETIRED (Stage 10-abi walk).**  There is no `TASK_MAX` and no `tasks[]`: ledger A-19 replaced the array with an intrusive list, and the static pool is two entries — the idle thread and the root task — which is the same bootstrap exception seL4's root task is |
| `task_rsp[TASK_MAX]` (index-keyed RSP array) | per-slot kernel RSP, parallel to the array | scheduler context switch | `struct task.saved_krsp` | S2 inc.2 | — | REMOVED (Phase S2 inc.2 — the scheduler's first indirection) |
| run-queue `next[TASK_MAX]`/`queued[TASK_MAX]` + `(t - tasks)`/`&tasks[idx]` | run-queue identity by array index | rq_enqueue/remove/dequeue | intrusive pointer lists (`t->rq_next`/`rq_queued`) | S2 inc.2B | — | REMOVED (Phase S2 inc.2B Block A — run queue 100% pointer-based) |
| `tasks[j]` timeout scans (tick/idle) + slot allocation | iteration over the backing array | scheduler_tick / sched_handle_idle / task alloc | iteration over `ktcb_registry[]` (pointers+generation) | S2 inc.2 Stage C | — | REMOVED as identity (Phase S2 Stage C — everything goes through `ktcb_registry[i].tcb`) |
| `KTcbRegistrySlot ktcb_registry[TASK_MAX]` | reference registry (tcb*/generation/occupied/bootstrap), NOT payload | scheduler/alloc/lookup | same registry; transitional capacity | — | transitional TASK_MAX limit | TRANSITIONAL_IMPLEMENTATION_CAPACITY (Stage C) |
| `struct task tasks[TASK_MAX]` (static payload) | real TCB backing (registers/kstack ptr/scheduler state) pointed to by `registry[i].tcb` | registry (scaffolding) | canonical KTCB in Untyped (Stage D) | S2 inc.2 Stage D | yes — scaffolding, no new consumers | **RETIRED (Stage 10-abi walk).**  Same as the row above: the payload array is gone with the pool |
| `SYS_THREAD_SET_SC` (85) | SC self-bind | existing scheduler code | `SYS_SC_BIND(sc,tcb)` by CPtr | — | yes — frozen | FROZEN (Phase S2 inc.1) |
| `struct KTcb` wrapper (kslab) | cap-visible TCB object in the heap, separate from the task | — | `struct task` IS the KTCB (KObject at offset 0) | S2 inc.2 | — | REMOVED (Phase S2 inc.2 — one structure, one identity) |
| executable thread-create via pool (`SYS_THREAD_CREATE`) | a thread was carved from the kernel's static task pool and identified by a global id: no capability authorised it and no Untyped paid for its storage | — | `RETYPE2(KOBJ_TCB)` + `SYS_TCB_CONFIGURE` (CSpace/VSpace caps) + `SYS_TCB_WRITE_REGS` + `SYS_TCB_RESUME` | Stage 5 | n/a | **RETIRED (Stage 5 Step 4)** — number 48 reserved, `NOT_SUPPORTED`, zero in-tree callers.  Every thread in the suite and init's exception test is retyped from an Untyped and configured with capabilities; creation returns a CAPABILITY, not an index into a kernel array (charter §3.4/§3.5).  T297 pins the gate, T148 the number |
| first-thread start for a spawned process (`SYS_THREAD_START` 58 → `task_thread_create`) | the child's first thread still comes from the static pool, because the spawner cannot name the child's CSpace/VSpace | none | the spawner retypes and configures the child's initial TCB, once it holds caps to the child's CSpace and VSpace | Stage 7 (done) | n/a | **RETIRED (Stage 7)** — 58 answers `NOT_SUPPORTED`, number reserved, and `task_thread_create` is DELETED.  svc_loader composes the child's first thread the way any thread is composed: `RETYPE2(KOBJ_TCB)` from the child's budget, `SYS_TCB_CONFIGURE` with the child's CSpace and VSpace (which it retyped and holds since Stage 6-pure Step 4/5), `SYS_TCB_WRITE_REGS`, `SYS_TCB_RESUME`.  No path remains by which a thread exists because the kernel had a free slot; what still comes from the pool is the root task and the idle task, both built before any Untyped exists |
| idle task (static backing, registry slot 0) | bootstrap TCB outside Untyped, with no cap-visible object | scheduler | root-task TCB from BootInfo (Stage 5) | Stage 5 | yes — isolated bootstrap exception, never retyped or reused | BOOTSTRAP_EXCEPTION |
| native CDT/MDB in CNode slots | — (it is the correct seL4 mechanism) | `SYS_CSPACE_MINT`/`MINT_INTO`/`REVOKE`, retype2, teardown, receive-slot | — | — | n/a | IMPLEMENTED (Phase S3 — recursive cross-process revoke; validator + fuzzing) |
| handle-tree derivation (all types) | parallel derivation tree hidden in the handle table | — | per-slot derivation via the native CDT | Stage 3 | n/a | **REMOVED (Phase S4)** — `SYS_CAP_DERIVE`(78)/`SYS_CAP_REVOKE`(79) retired to `NOT_SUPPORTED` (numbers reserved); the table's derived-insert, revoke-children and parent-array machinery deleted.  `legacy_handle_derivation_migrated` has ZERO callers and is a structural 0 — the retirement witness |
| MDB LEGACY roots (`MDB_FLAG_LEGACY_ROOT`) | caps with no provable CSpace ancestor (handle/bootstrap/IPC-delivery origin) | bootstrap (kernel_main), legacy `kcnode_mint*`, IPC receive-slot delivery | a real CSpace origin (retype/derive by CPtr) | Stages 2/4/5 | yes — closed allowlist, observable `mdb_legacy_roots` counter | ACTIVE_LEGACY, and CLOSED TO THE BOOT PATH.  "must → 0" was the wrong target and is corrected here: seL4's BootInfo capabilities are unparented too, because they exist before there is anything to be a child of.  What is enforced is a CEILING (T305), which is 32 on this machine — the boot authorities, the boot Untypeds, and the five firmware regions Stage 10 publishes so ring 3 can read ACPI.  A root appearing anywhere else is a defect and the ceiling is what makes it visible |
| IPC cap-transfer with a handle source | a transfer's source was resolved by handle, not by CPtr | — | CPtr source + CSpace receive slot | Stage 2 | n/a | **REMOVED (Phase S4)** — `syscall_ipc_stage_cap_peek_badged` resolves the source through `cspace_resolve_slot`; a handle value is `INVALID_ARG`. Delivery installs the cap as an MDB CHILD of the source slot, so `cdt_ipc_transfer` deliveries are no longer LEGACY_ROOTs |
| root CNode from the slab | runtime CNode outside Untyped | the ROOT TASK only | the spawner retypes the CNode and names it in `SYS_TCB_CONFIGURE` | permanent for the boot path | yes | **CLOSED except for the root task (Stage 6-pure Step 5).**  Every child's root CNode is retyped by its spawner out of a budget the spawner holds, and named at configure time.  What remains on the slab is the ROOT TASK's, built before any Untyped exists — the same permanent boot-path exception seL4 has, and the reason this row cannot reach zero |
| root CNode reachable only via `cspace_root_h` (handle) | the CSpace ROOT is located through the handle table | — | `KProcess.cspace_root`: a structural back-reference holding one lifecycle + one active ref, released in `kprocess_teardown` | Stage 4 | n/a | **REMOVED (Phase S4, Step 4)** — resolving a CPtr no longer touches a handle table, and the cross-process paths (`SYS_CSPACE_MINT_INTO`, `SYS_PROC_CSPACE_MINT`, retype2, IPC receive-slot delivery) no longer read ANOTHER process's handle table to find its root.  Allowlist: `handle_table_get_object` 52 → 40 (14 files → 11), `handle_table_insert` 42 → 41 |
| implicit page-table allocation (PMM reserve on map) | kernel memory hidden by mapping | the KERNEL address space (kstack region, physmap) and the root task's maps made BEFORE it runs — six levels, ended by `kvspace_end_bootstrap` | page tables carved from an Untyped the address space names | Stage 6 (the bootstrap exception retires with Stage 7's user-space VSpace composition) | yes | **CLOSED for userland (Stage 6-pure Step 2/3)** — the kernel does not create page tables.  A map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE`; the holder retypes a `KOBJ_PAGE_TABLE` and installs it (`SYS_VSPACE_MAP_TABLE`).  The root task's pre-run maps are the only exception and it ENDS when the root task can speak for itself, after which no address space is implicitly funded while anybody is running (measured: six carves, all before the root task runs).  What stays kernel-funded is the KERNEL's own address space, which has no holder to ask |
| KFrame header sidecar (kslab) | metadata outside the region | VMO page frames and a spawning process's bootstrap frames (`kframe_alloc`) | header carved from the Untyped | Stage 6 (Etapas 3/5, with the paths that create those frames) | n/a | **CLOSED for everything ring 3 can reach (Stage 9-evt).**  Every frame a syscall can cause — retyped, VMO-paged, or mapped through a VMO — carves its header from a budget somebody named.  `kframe_alloc`'s slab form is the BOOT PATH's alone: bootstrap mappings made before any Untyped capability exists.  The three VMO mapping loops in `syscall_vm.c` were the last holdouts and were the worst of them, being one allocation per mapped page under a ring-3 caller's control.  The purity gate's reachability check now runs with NO exemptions: no `kslab_alloc` is named by any syscall handler |
| process-level fault record (one per process) | belongs on the TCB | none | the record lives on the thread that takes the fault; the handler is armed on that thread (`SYS_TCB_SET_FAULT_HANDLER`) and read off it (`SYS_TCB_FAULT_INFO`) | Stage 7 Steps 6/8/12 (done) | n/a | **REMOVED** |
| `SYS_PROCESS_VSPACE` (107) | process authority → VSpace by handle | none | the spawner RETYPED the child's address space, so it already holds the capability and mints it where it is needed | Stage 7 Step 15 (done) | n/a | **RETIRED** — 107 answers `NOT_SUPPORTED`, number reserved |
| `SYS_BOOTCAP_RESTRICT` (dual-namespace split brain) | `arg0` is resolved with `cspace_or_handle_resolve_obj` (CPtr **or** handle), but the restricted clone is published with `handle_table_replace(ht, (handle_id_t)arg0, …)` — the two halves disagree about which namespace `arg0` is in | init (fb spawn cap), svcmgr (post-bootstrap strip) — **both pass handles**, so no live defect | publish the clone into a CSpace destination slot as an MDB child of the source slot, the way retype2/mint already do | Step 4 | yes | **RETIRED (Stage 10-abi walk).**  `SYS_BOOTCAP_RESTRICT` and `kbootcap_clone_restricted` were both removed in Stage 5, which split the monolith into one capability per authority and made narrowing-by-cloning unnecessary.  The row's warning about blocking the spawn-cap CPtr migration outlived the thing it warned about |
| CSpace cycles below the root CNode | a CNode reachable from itself keeps its own references alive, so nothing frees it | any task minting a second-level CNode into itself | seL4-style recursive delete with zombie capabilities | unscheduled — the self-cycle is closed and nothing in tree builds the general case | n/a | **SELF-CYCLE CLOSED (Stage 7-proc); the general case remains.**  A slot naming its OWN CNode no longer takes an ACTIVE reference: an object reachable only from itself is reachable by nobody, and the count now says so.  The lifecycle reference stays, so close fires when the last EXTERNAL holder goes, empties the slots, and that release lets destroy run — which is what removed KProcess's reason to empty a root CSpace pre-emptively at a moment it knew because it counted threads.  BC-13 was the negative control for the old behaviour and is now the positive case.  A cycle THROUGH another CNode (A names B, B names A) is still uncollectable and still wants seL4's recursive delete with zombie capabilities.  Previously:   It was recorded as known and unexercised; it is now the last thing standing between IRIS and a kernel with no process object.  `kprocess_teardown` breaks the root-CSpace cycle pre-emptively, at a moment it knows because it counts threads, and deleting `KProcess` without recursive delete would leak the root task's CSpace.  Stage 5 Step 3 fixes the case it INTRODUCES (the root task's capability to its own root CNode) by emptying the root's slots at `kprocess_teardown` before dropping its refs; a cycle deeper in the tree is still uncollectable.  No in-tree code builds one; BC-11..BC-13 pin the fixed case |
| `KBootstrapCap` (monolith) | one object carried spawn, hardware, debug and framebuffer authority at once | — | one capability per authority + structured BootInfo | Stage 5 | n/a | **REMOVED (Stage 5 Step 2)** — the object type survives as the carrier of a SINGLE authority (`kind`), and `kbootcap_alloc` refuses a zero or multi-bit kind, so a monolithic boot capability cannot be constructed.  Six capabilities (process, initrd, IRQ, ioport, debug, framebuffer) are published one per slot and described in BootInfo v4.  `SYS_BOOTCAP_RESTRICT` (45) is RETIRED with its number reserved; `kbootcap_allows` and `kbootcap_clone_restricted` are deleted; `BOOT_CPTR_BOOTSTRAP_CAP` (slot 1) is permanently empty.  Least-authority result: vfs, a file server, no longer holds the authority to create processes.  T296 pins the split; T291 died with the mechanism.  History: Step 1-2b — Step 2a/2b SPLIT device and debug authority out: `IRIS_BOOTCAP_HW_ACCESS` (one bit for both IRQ and ioport creation) is replaced by two capabilities matched EXACTLY by the kernel, published one per slot and recorded in BootInfo v2.  svcmgr renounces hardware authority by DELETING those slots instead of cloning a narrowed monolith.  Step 2b did the same for `IRIS_BOOTCAP_KDEBUG` (kernel log, scheduler statistics, poweroff), published at `BOOT_CPTR_DEBUG_CONTROL` and delivered to children in the retired `IRIS_CPTR_SVC_REPLY` slot.  Step 2c split the rest; T296 pins the split, T291 keeps `SYS_BOOTCAP_RESTRICT` honest until it retires.  Step 1 — the structured BootInfo EXISTS (`struct iris_root_bootinfo`): the root task is told its initial caps by CPtr, the shape of its root CNode and every boot Untyped with its physical region, instead of agreeing with the kernel on constants and probing slots until one answered `NOT_FOUND`.  The object itself is unchanged and still carries the four-bit permission mask; splitting it into fine-grained caps and retiring `SYS_BOOTCAP_RESTRICT` is Step 2 |
| `KInitrdEntry` + `SYS_INITRD_*` | filesystem-aware kernel state | loader | user-space VFS/loader | Stage 10 (platform) — it no longer has a process server to retire with | yes | ACTIVE_LEGACY, with the target CORRECTED.  The row said "filesystem-aware kernel state", and that has not been true since the catalog lost its names: the kernel exposes images BY INDEX and knows nothing about files or formats.  What remains is a kernel that holds the boot images at all, where seL4 puts them inside the root task.  Stage 10 did not close it, and says so rather than quietly redefining it: retiring it means the kernel loading ONE image and the rest living in a ring-3 archive, which changes the image source of every service and is a boot-path change that deserves its own stage rather than being done in the margin of another |
| kernel stacks / PML4 from the PMM reserve | allocation outside Untyped | task create; the ROOT TASK's PML4 only (a spawned process's PML4 is a page child of its budget since Stage 6 Step 3) | TCB/VSpace from Untyped | process/frame phases | yes | **RETIRED (Stage 10-abi walk).**  D-1 closed in Stage 9-evt: there is one kernel stack per CORE, `kstack_alloc` is deleted, and a thread no longer blocks in the kernel.  The PML4 question closed with it — a spawned process's address space has been a retyped object since Stage 6-pure |
| `KChannel` | — | — | endpoints | Phase 13 | — | REMOVED |
| hardcoded ioport whitelist (`kioport_whitelist`, syscall_priv.h) | device policy in the kernel | kbd/console/fb/userboot via svcmgr | fine-grained ioport caps issued by the root task (BootInfo) | Stage 5 | n/a — the table is gone | **REMOVED (Stage 5).**  The range a holder may claim travels ON the `IOPORT_CONTROL` capability: boot issues the root task one over the whole port space, `SYS_IOPORT_CONTROL_NARROW` derives a sub-range for a delegate (an MDB child of the authorising slot, so revoking the parent reaches it), and `SYS_CAP_CREATE_IOPORT` checks a request against the authority the caller actually holds.  The table was wrong twice over: the kernel had no basis for the list — which ports exist is a fact about a machine and who may claim them is a fact about who is trusted, and neither is the kernel's to know — and it applied to every holder equally, so it could not express the only useful restriction, that init may claim a serial port and svcmgr may not.  The order of the checks changed with it: authority FIRST, so a caller learns it has no authority rather than learning something about the port map.  `RIGHT_DUPLICATE` gates the narrowing, because it creates a second capability carrying the same authority — a delegate handed one without it may use its range and may not subdivide it.  The narrowed object is carved from an Untyped the caller NAMES: the first version of this took it from the kernel slab, which is correct for the boot path and would have been a charter M3 hole anywhere else — a syscall that let ring 3 spend the kernel's memory, opened by the very change that closed a policy one.  Caught before it shipped by asking what the new syscall made reachable from ring 3, not by a gate: the purity allowlist counts occurrences, not reachability, and it stayed green throughout.  T164 narrows the suite's own control capability and then discovers it is narrowed (in-range allowed, CMOS denied, a spill denied, and a widening refused); T171 fuzzes against a narrowed authority.  seL4's `IOPortControl` is unranged and confines by who holds it at all; this is that, plus the ability to hand out a piece |
| `SYS_CAP_CREATE_IRQCAP`/`_IOPORT` as handle producers | device authority existed ONLY as a handle, leaving the legacy handle tree as its sole derive/revoke mechanism | — | slot publication (arg3) parented to the authorising bootstrap-cap slot | Stage 3 prep | n/a | **REMOVED (Phase S4)** — both publish into CSpace as MDB children of the bootstrap cap; device caps now derive/revoke through the native CDT |
| `svc_mint.src_h` (handle-sourced pre-start delegation) | the loader mints a child's caps from the supervisor's handle table | all non-device mints in svcmgr/init/userboot | `svc_mint.src_cptr` + `SYS_CSPACE_MINT_INTO` | Stage 4 | yes for device caps (already migrated) | MIGRATING (device caps done; endpoints/untyped/reply still handle-sourced) |
| IPC delivery into the receiver's handle table (`syscall_ipc_deliver_cap_badged`) | a capability entered a process through the handle namespace because the receiver declared no destination — not a choice either side made | — | the receiver declares a receive slot; an undeclared receive gets the message WITHOUT the capability, and the sender's source slot is untouched | Stage 4 | n/a | **RETIRED (Stage 4)** — the destination half of charter I1.  `iris_ipc_stat_handle_deliveries` is a structural 0 (T095 pins it, T096 proves 32 consecutive deliveries all land in slots) |
| TOCTOU receive-slot→handle fallback (`syscall_ipc_deliver_cap_routed`) | CSpace-to-handle delivery degradation | — | fail closed: no cap delivered, source slot untouched | Stage 2 | n/a | **REMOVED (Phase S4)** — the last permitted degradation is gone; `iris_ipc_stat_toctou_fallbacks` is a structural 0 pinned by T094 (forces the race) and T095 (asserts the counter never moves) |

### A-12 — user memory is charged to a named Untyped

**Change**: a VMO's pages, metadata and header come from an Untyped.
`SYS_VMO_CREATE`'s unused first argument and a new fourth argument of
`SYS_INITRD_VMO` name WHICH budget pays; zero means the budget the caller's
address space was built from.  The loader recycles a per-child budget and a
scratch budget for image copies.

**Justification**: charter §2.5 M3 and M1.  Anonymous memory was the last
allocation a process could obtain without a capability standing behind it —
bounded by `KPROCESS_PHYS_PAGES_LIMIT`, a number the kernel chose, rather than
by an Untyped somebody delegated.  Naming the budget is not new surface for its
own sake: a process holds several budgets, and charging a service's data to the
small pool that funds its address space would be the wrong answer, silently.

**Reclamation is part of the change, not a follow-up**: a bump allocator does
not rewind, so charging alone makes consumption monotonic.  Budgets are
recycled — per live child, and per image copy — which bounds cost by what is
ALIVE rather than by what has ever run.  `SYS_CAP_IDENTIFY` is what lets the
loader ask whether a leaf is free before recycling its budget.

**Scope**: `KVMO` moves FROZEN → CONVERTED (still frozen as an object; the
object retires with the memory server, Stage 7).  No allowlist movement:
`kvmo.c` still has the kernel-funded path for wrapped device regions and the
root task.  A pre-existing defect fixed on the way: three-argument syscall
stubs left `r10` undefined, which became visible the moment a syscall grew a
fourth argument.  Tests: T300.

### A-11 — page tables are charged to a named Untyped

**Change**: `SYS_PROCESS_CREATE` gains a required page-table budget argument (a
KUntyped CPtr with `RIGHT_WRITE`), retained by the new VSpace.
`paging_map_checked_in_from` carves intermediate levels from it and fails when
it cannot.  Each table takes a `child_count` entry on that Untyped
(`kuntyped_alloc_page_child`), returned when the address space is destroyed.

**Justification**: charter §2.5 M3 — "the kernel does not implicitly allocate
user memory".  A page table that maps user memory IS user memory, and it was
being taken from the kernel's PMM reserve on every map that needed a level:
unbudgeted, unauthorised, and drivable from ring 3 by mapping at scattered
addresses.  M1 ("frames, page tables, VSpace converge to creation from
Untyped") moves from PENDING toward MET with this.

**Not a new namespace or fallback**: the budget is named as a capability and
checked like one; a spawn without it is `INVALID_ARG`, not a spawn the kernel
funds.  The bootstrap exception (kernel mappings, root task) is bounded and
stated, in the same category as the idle task's static backing.

**Two pre-existing defects fixed in the same change** because this step made
them reachable: page-aligned carves aligned the offset instead of the absolute
address (so a frame retyped from a sub-untyped got a paddr the mapper masked
DOWN, overlapping earlier carves), and a process created but never started
could not be reclaimed at all (kill found no threads and dropped nothing).

**A third, found later, in the same family**: a sub-untyped's SIZE was required
to be a page multiple but its BASE was carved at 64-byte granularity, so the
region agreed with nothing inside it.  The first frame, page table or VSpace
retyped out of a fresh sub-untyped had to re-align upward and silently burned
up to a page that the child's `available` had already counted — a caller who
bought 256 KiB and spent it on frames simply got one fewer than arithmetic
said.  The waste MOVED whenever an unrelated kernel struct changed size,
because that shifts the parent's bump pointer, which is how it surfaced: adding
48 bytes to `struct task` for D-1's register save changed the alignment T298
happened to be sitting on.  A sub-untyped is now carved page-aligned.  seL4 has
no such case at all — an untyped object there is always naturally aligned to
its own size, which is what makes its retype arithmetic exact; page
granularity is the weaker property IRIS needs and can afford.

**Scope**: the `implicit page-table allocation` row moves ACTIVE_LEGACY →
MIGRATING.  No allowlist movement.  Tests: T299, plus the whole suite, whose
every spawn now runs on a budgeted address space.

### A-10 — the Untyped carves from both ends

**Change**: `KUntyped` gains a second bump pointer (`used_top`) growing down
from the end of the region, and `kuntyped_alloc_child_top`.  Object headers for
objects that also own a page-aligned region are carved there; a retyped
`KFrame`'s header is the first user.

**Justification**: charter §2.5 M3 ("the kernel does not implicitly allocate
user memory") and §3.3 (no canonical object from kslab).  A frame's page was
paid for by its Untyped and its header was not — kernel memory that nothing
accounted and no capability authorised, which is the same defect as the
implicit page tables one level down.  The second pointer exists because a
header carved from the bottom would round the next page carve up and cost a
whole page per frame; the alternative (accept the waste) would have made
paying for headers a memory regression, and the alternative to that (leave
headers in the heap) is the thing being fixed.

**Not a new mechanism**: no syscall signature changes; `child_count` semantics
are unchanged (one child per frame); `SYS_UNTYPED_RESET` reclaims both ends and
still refuses while children are live.  A refused carve moves neither pointer.

**Scope**: the `KFrame header sidecar` row moves ACTIVE_LEGACY → MIGRATING; no
allowlist movement yet, because `kframe.c` still allocates headers for frames
that have no Untyped to charge (VMO pages, bootstrap frames).  Tests:
UT-TOP-1..5 and T298.

## Structural divergences from seL4

### A-44 — an endpoint wait queue named threads it did not hold  ✅ CLOSED

**Found by extending A-43.  Fixed on the second attempt; the first was
reverted, and why it failed is the useful part.**

`KEndpoint`'s wait queue is an intrusive list of raw `struct task *` through
`t->ep_next`, and nothing on it held a reference.  The core that takes a
waiter off the queue keeps walking it after `ep->lock` is dropped: it wakes
it, and on several paths writes to it first.

**Why that is reachable.**  A blocked thread is off-CPU, and `sys_tcb_exit` on
an off-CPU target does NOT defer to the reaper — it falls through to
`task_execution_teardown_off_cpu(t)` and runs the whole teardown
**synchronously on the killer's core**.  So a supervisor on one core frees a
waiter's storage back to its Untyped while another core, mid-rendezvous, still
holds the pointer.  `task_wakeup` refuses a `terminal` or `TASK_DEAD` task,
which guards against enqueueing the dead — not against a slot already retyped
into a different live thread.

`kendpoint_close` had it too, and worse: it calls `task_kill_external(t)` while
holding `ep->lock`, and clears `t->blocking_ep` first — so a concurrent
`kendpoint_cancel_waiter(t)` reads that field BEFORE taking the lock, sees 0,
returns without ever blocking, and teardown proceeds.

**The repair**: being queued is being held.  Four enqueues take a reference and
every removal gives it back after its last touch — the two fastpaths, the
fault-call delivery's two branches, the notification-to-blocked-receiver path,
both `ep_recv` rendezvous, the reply path's own rendezvous,
`sys_ep_cancel_badged_sends`, `kendpoint_close`, and `kendpoint_cancel_waiter`,
which releases only when it is the caller that actually dequeued.  A call-mode
sender that stays blocked is released here too: the REPLY binding holds it from
then on (A-43).

**The first attempt, and the lesson.**  It enumerated the queue's enqueues and
removals **by grepping one file**, `syscall_endpoint.c`.  `syscall_reply.c`
manipulates the same queue — it enqueues a caller at its own park and dequeues
a receiver at its own rendezvous — and was missed entirely.  A waiter queued
there and dequeued in the other file had a reference released that nobody ever
took, so a live thread's refcount reached zero and it was destroyed with
`terminal == 0`.  The ring-3 run panicked with `sched_resume: kernel resume
with no entry`: a zeroed slot, woken and then dispatched.

It was NOT localised by bisection, which cost three runs and pointed nowhere.
It was localised by instrumenting: one probe at the assert printing the broken
task (`id=0 state=1 resume=0 ref=0 type=0` — `type=0` being the same
zero-filled signature T350 recorded), and one in the TCB destructor printing
`term=0`, which said the destruction was not a teardown at all but an
over-release.  A `grep` for queue writes across the WHOLE tree then named the
missing file in one line.

**Two things that wasted runs and are worth not repeating.**  The first probes
used `klog_write`, which does not reach the serial log on that path — the
panic's own messages use `serial_write`, and so must anything meant to be read
beside them.  And "revert and register" was the right call at the point it was
made, but the row it produced blamed an undocumented task reference model; the
actual defect was an enumeration that stopped at a file boundary.  **A
grep-derived surface is only as complete as its path argument.**

**The NOTIFICATION queue had the same defect and is fixed with it.**  Applying
the lesson rather than the patch: its surface was enumerated across the whole
tree first, which showed it is entirely contained in `knotification.c` -- one
enqueue and three removals (signal-one, cancel, and the wake-all on close) --
while the two `notif_next` writes in `syscall_endpoint.c` only borrow the link
field for a local list and are not that queue at all.  One retain, three
releases.  The enqueue's two "already queued" early returns take nothing,
because a task already on the queue is already held and the second reference
would never come back.

**A queue that holds references makes hand-installed waiters illegal.**  Two
places install a waiter directly instead of calling the enqueue -- the ring-0
notification self-check and a host test for close-with-a-waiter -- and both
now initialise the fake waiter as a real KObject, because the wake-all they
are testing gives a reference back for every waiter it empties.

**Tests**: `tests/kernel/` initialises its `struct task` fixtures as real
KObjects (`test_task_object_init`), because a queue that holds references
makes a bare `struct task { 0 }` underflow on the first release.  The host
suite caught exactly that, three times, which is how every fixture gap
surfaced.

### A-43 — a reply binding named a thread it did not hold  ✅ CLOSED

**Found by the same method as A-41 and A-42: reading a fix already in the tree
and looking for the site that never got it.**

`sys_tcb_exit` holds its resolve reference across the kill, and the comment
saying why is explicit about what happens otherwise: "another core killing the
same thread completes the teardown and drops the CSpace slot's reference in
that window, and this core then walks a TCB whose storage has already gone
back to its Untyped, zero-filled."  T350 is its test, and the symptom it
records is `kobject_retain: resurrect from refcount 0` on an object whose type
field read 0.

`KReply::caller` had the identical shape and no reference.  `kreply_bind_caller`
stored a raw `struct task *`; `sys_reply` took it out under `r->lock`, dropped
the lock, and then wrote the reply into the caller's staging, resolved its
fault record and woke it.  The comment on the delivery said "caller is blocked
— safe".  **Blocked is not alive.**  A supervisor holding the caller's TCB
capability runs `TCB_EXIT` on another core, teardown claims `terminal`,
`task_cancel_blocked_waits` finds the binding already taken, and nothing then
stops the storage going back to its Untyped while this core is still walking
it.  `task_wakeup` refuses a `terminal` or `TASK_DEAD` task, which is a guard
against enqueueing the dead — not against a slot that has since been retyped
into a different live thread.

**The repair**: the binding takes a reference, and whoever takes the caller out
of the reply object inherits it and releases after the last touch.  Three
consumers, three releases — `sys_reply` on both of its delivering paths (the
one that loses the one-shot race never holds it), `kreply_cancel_caller`, and
`kreply_obj_close`.  `r->caller != NULL` now means "this task is still there",
which is what every reader of it already assumed.

**Tests**: `tests/kernel/test_kreply.c`.  The suite caught the change
immediately and correctly — its callers were bare `struct task { 0 }` on the
stack, never `kobject_init`-ed, so the first retain tripped `resurrect from
refcount 0`.  They are objects now, and the test asserts the reference lands
on bind and is given back by both cancel and close, which is the accounting
the fix depends on and which nothing checked before.

### A-42 — the entry frame was frozen by a flag nobody held  ✅ CLOSED

**Found by looking for A-41's siblings: other once-only transitions tested and
set without a claim.**

A thread's entry frame stops being writable once it has been runnable, and
`started` is the flag that says so.  `ktcb_write_regs` tested it; `sys_tcb_resume`
set it; nothing held anything across either.

Two cores pass each other.  WRITE_REGS reads `started == 0` and
`state == TASK_SUSPENDED`; RESUME sets `started` and wakes the thread; WRITE_REGS
then runs `task_set_first_user_entry` on a thread that is RUNNING.  That call
zeroes `user_ctx`, republishes `resume_user` as `TASK_RESUME_USER_FIRST`, and
clears `kentry`.

`kentry` is the continuation witness — it is what RESUME itself checks to
decide whether a thread can be resumed in the kernel.  Clearing it under a
thread the kernel believes is mid-syscall hands that thread back to user at a
fresh entry with its continuation dropped.  **A thread abandoned that way while
queued on an endpoint leaves the queue still naming it**, and the next sender
to rendezvous with it is a different principal, which is what makes this more
than a caller mangling a thread it already owns.

The comment on the old gate said writing a running thread's registers "would
corrupt the kernel stack it is standing on".  That is no longer where the
damage is — Stage 9-evt step 3 moved the frame into the TCB — and the comment
had not moved with it.  The hazard did.

**The repair**: the test and the write are one critical section under the
thread's own `obj_lock`, and RESUME publishes `started` under the same lock.
Rank 6, nothing taken beneath it, so the ordering table is unchanged.

**Also closed here**: `task_registry_alloc` tested `reg_slot >= 0` OUTSIDE the
`sched_list_lock` it then links under, so two callers could both pass and both
splice one thread into `sched_thread_list`.  A-41's claim is what keeps that
unreachable today — the test belongs under the lock that does the linking
whether or not a caller happens to serialise it.

### A-41 — teardown was claimed against concurrent callers; construction was not  ✅ CLOSED

**Found by audit, by reading the fix that was already there and looking for
its mirror.**

`terminal` is `_Atomic uint8_t` and teardown claims it with an exchange, and
the comment on it says exactly why: it used to be a plain byte tested at the
top of teardown, "an unlocked read that four cores calling Exit on one thread
all pass, so all four tore the same thread down."

`ktcb_configure` had the identical hole at the other end of the lifecycle and
no claim at all.  It opened with `if (t->configured || t->terminal) return
ALREADY_EXISTS;` and set `t->configured = 1` ninety lines later, with no lock
anywhere in between.  Two cores holding `RIGHT_WRITE` on the same unconfigured
TCB — which is what a spawner with two worker threads has — both pass the
test and both build the thread:

- `t->cspace_root` and `t->vspace` are each assigned and then retained, so the
  second call overwrites the first's pointers.  **The first CSpace and VSpace
  pair is leaked past any reach**: nothing holds a pointer to it, so teardown
  cannot release it and the Untyped that paid for it can never be reset.
- `kobject_retain(&t->base)` — the EXECUTION reference — is taken twice and
  dropped once, so the TCB is never destroyed.
- `sched_live_count` is incremented twice and decremented once.  The comment
  three lines above it warns that a thread which skipped the increment would
  underflow the counter; the inverse breaks it just as thoroughly.
- `task_registry_alloc` runs twice, which can put one thread twice into the
  list the tick walks.
- And the two calls interleave field by field, so the thread can end with the
  CSpace of one and the VSpace of the other — **a pairing neither caller
  asked for**.

None of it needs two principals.  One principal racing two of its own threads
on one TCB it owns leaks a CNode, a VSpace and a TCB permanently, repeatably,
and nothing — not revoke, not `SYS_UNTYPED_RESET` — reclaims any of it.

**The repair** is the precedent, applied at the other end: `configuring`, an
atomic claim taken with an exchange at the top.  It is a separate flag rather
than `configured` itself, because `configured` is the execution gate that
seven other places read as "this thread may be written to and run" — claiming
it early would open a half-built thread to `TCB_WRITE_REGS` and `TCB_RESUME`.
The claim is released again on the two failure paths that run before any state
is touched, so a retry after `NO_MEMORY` still works.

**No targeted regression test.**  `task_lifecycle.c` is not in the host suite
and pulling it in would drag the scheduler with it, and a race is not
something a single-threaded test can express in any case.  What is asserted is
the shape: the claim is initialised where `configured` is, the four lanes are
green, and the reasoning is the one already written down for `terminal`.

### A-40 — a derivation named its parent by location, and a slot is reusable  ✅ CLOSED

**Found by audit while checking the CDT for use-after-free, fixed and gated in
the same session.**

Every path that installs a capability as the child of another one does the
same three things: read the parent slot, decide what it is allowed to do, then
install.  `kcnode_slot_install_linked` checked the parent slot for OCCUPANCY —
`if (!parent->object || parent == s)` — and nothing more.

A slot is a reusable location, and the installing thread is not the only
thread in its process.  On SMP a sibling can empty that slot and mint
something unrelated into it between the read and the install.  The new
capability is then linked as a child of whatever now sits there: revoking the
true ancestor does not reach it, and revoking the impostor destroys a
capability that has no relation to it.  **A capability surviving a revoke is
the one outcome the derivation tree exists to prevent** — charter A9, in both
directions.

The authority itself was never bypassed; the caller really did observe what it
read while it was there.  What breaks is revocability, which is the property
that makes granting a capability a decision a principal can take back.

**Five paths had it**, all the same shape:

- `kcnode_slot_derive` — `CNODE_COPY`/`MINT`.  The most reachable: two threads
  racing on one slot, no rendezvous to time, retried until it lands.
- IPC capability delivery (`syscall_endpoint.c`).  It already called
  `kcnode_slot_holds` for exactly this reason, but under `src_cn->lock`, which
  it then dropped — the window was narrowed, not closed.
- `SYS_UNTYPED_RETYPE2` — the parent is the Untyped's slot.
- `dev_cap_publish` (IRQ, ioport, ioport-narrow) — the parent is the
  authorising bootstrap capability's slot.
- `syscall_publish_slot` (initrd frame) — likewise.

**The repair.**  A caller that names a parent says what it expects to find
there, and the comparison happens under the same `mdb_lock` hold that installs
the link.  `NULL` still means occupancy only, for a caller that genuinely has
no expectation.

Three of the five had to start HOLDING the authority they name, because a
pointer whose object may already have been freed and its address reused is not
an expectation.  `dev_cap_auth_ranged` used to release the bootstrap
capability the moment it had finished checking its kind; it now hands it back
to the caller and `dev_cap_auth_release` drops it after the publish.  The same
in `sys_ioport_control_narrow` and in the initrd path.

**Why not check identity always, with no parameter.**  A derivation's parent
holds the same object it does, so `obj` would serve — but retype and the
device-capability paths legitimately parent a NEW object under an authority
that is a different object entirely.  There is no universal rule; there is
only what the caller expects, so that is what the caller passes.

**Tests**: `tests/kernel/test_parent_identity.c`.  Matching expectation
installs; mismatched is refused with the slot still empty and no reference
kept — the refusal path takes a retain and an active_retain at the top of the
function and has to undo both.  An absent expectation still refuses an empty
parent.  Verified to FAIL on four assertions against the occupancy-only
version.

### A-39 — CNode teardown recursed as deep as ring 3 nested capabilities  ✅ CLOSED

**Found by audit, measured on the real object code, fixed and gated in the
same session.**

Emptying a CNode slot that names another CNode ends that CNode's last active
reference, which runs its close hook, which empties ITS slots.  Written as
plain recursion, the stack depth is the depth of the capability chain.

Ring 3 picked that depth.  `CSPACE_MAX_DEPTH` is 8, but it bounds WALKING a
CPtr, not nesting one CNode inside another: a chain is built bottom-up with
every link sitting at depth 1 in the builder's own CSpace, so no walk ever
goes past one level while the object graph goes as deep as untyped memory
allows.  A 1-slot CNode costs about 144 bytes, so a few megabytes of untyped
buys thousands of links.  Bringing the whole chain down takes one syscall:
delete the single capability naming its head.

A core has ONE 4 KiB stack (`CORE_STACK_BYTES`) and the stacks are a plain
`.bss` array, so there is nothing mapped below one.  The overflow does not
fault — core N writes down into core N-1's stack, at its live end.

**The numbers.**  The recursion cycle is `kcnode_obj_close` (48 bytes) ->
`kcnode_slot_delete` (64) -> `kcnode_slot_drop_old` (32) ->
`kobject_active_release` (48), read off the disassembly of the object built
with the kernel's own flags, which carry no `-O`.  Measured end to end by
painting a stack and running the teardown in it: **177 bytes per link, 23
links to the bottom of a core stack.**  Link 24 is in the next core's.

**The repair.**  Close does not recurse.  The first CNode to start a cascade
owns it and runs a loop; every close firing underneath hands its CNode to an
intrusive list (`KObject::destroy_next`) and returns.  The list holds a
reference on what it carries, so nothing on it dies before the loop arrives,
and the link doubles as the "already queued" marker — close can fire twice on
one object when a core installs a fresh capability while another tears the
last one down, and without that marker the second enqueue would cut the list
in half.  The flag is global, not per-core, so a second core entering teardown
hands its work over rather than opening a second stack's worth of depth; the
loop re-checks the list under the same hold that clears the flag.

`kcnode_teardown_slots` stays inline instead of deferring, because its
contract is that the CNode is empty when it returns.  It is bounded anyway —
every nested CNode freed underneath it goes through the cascade.

`kcnode_obj_destroy` empties directly for a different reason: the drain
reaches a queued object by releasing it, and releasing an object whose
refcount already reached zero is where destroy came from.

Re-measured after: 8 links and 2000 links consume the SAME stack.  The cost
per link is zero.

**What is still true, and the second half of the fix.**  A 4 KiB stack with
nothing below it absorbs the next bug of this shape just as silently.  The
kernel image is mapped with 2 MiB pages, so an unmapped guard would mean
splitting one — more surgery than this warranted with the hole already shut.
Instead the lowest eight bytes of every core stack hold a known value and the
dispatcher checks its own before anything else.  The core that overflows
writes through its OWN canary on the way out of its region, so the panic names
the core that did it rather than the victim.  That is the "find out why"
`core_dispatch.c` already asked for: it does not widen the stack and it does
not make an overflow survivable, it makes one announce itself.

**Tests**: `tests/kernel/test_cnode_depth.c` tears down a 4-link and a
200-link chain and compares the stack frame the deepest destructor runs in.
Verified to FAIL (drift 34 KB) against the recursive version and pass at a
drift under 512 bytes against this one, and to leave `kcnode_live_count()`
where it started.

### A-38 — the message carries four words whatever the count says

**Found while hardening, measured, and left open deliberately.**

`ipc_msg_load` copies all four message registers and `ipc_msg_store_ext`
returns all four, whatever `word_count` declared.  The comment above them said
the count decides which words are part of the message, the way seL4's `length`
does.  It does not.

That was established by changing it.  Carrying only the declared words failed
nineteen tests immediately, in BOTH directions: clients that fill `words[0]`
and never set the count, and servers that answer with an error code in
`words[0]` and never declare it.  Ring 3 writes `words[]` 379 times and
`word_count` 107.

So the protocol is four words, always.  What that costs is a channel: the
words a sender did not fill carry whatever its `struct iris_msg` held, and
that reaches the receiver.  The exposure is bounded by what senders leave
there -- every one in this tree zeroes the struct first, through
`iris_msg_zero` and its siblings -- so nothing is known to leak today.

It is a boundary held by CONVENTION rather than by the kernel, which is
exactly the shape this tree has spent a sweep removing elsewhere.  Closing it
means making every request and every reply declare its count, across the whole
userland; that is its own change with its own gate, and pretending otherwise
by half-doing it here would have left a kernel whose comment and behaviour
disagree in a new way instead of the old one.  The comment now states what is
true.

### A-37 — the kernel halts on a ring-0 fault, and one path could take one  ✅ CLOSED

**Found by audit rather than by a test, registered, and then closed in the
same session.**

`idt.c` routes every exception that did not come from ring 3 to
`[IRIS][EXCEPTION] halting`.  There is no fixup table: the kernel has no way to
survive a fault of its own, which is a deliberate and reasonable posture for a
kernel that intends never to take one.

seL4 intends the same thing and can PROVE it.  IRIS cannot, and there is at
least one path where the proof would fail.  `copy_to_user_checked` walks the
destination range, then writes it.  Nothing serialises that against a
concurrent `FRAME_UNMAP` on the same address space, and a spawner holds VSpace
capabilities for its children — so on SMP a racing unmap can retire the PTE and
shoot down the TLB entry between the walk and the store, and the store then
faults at CPL 0 and halts the machine.  That is a denial of service available
to ring 3.

What is NOT wrong is `usercopy.c`'s own header claim.  "There is no TOCTOU
window on any input the kernel acts on" is true: the read path was deleted in
A-33 and a message is registers now.  This is a different property — fault
SAFETY on the write-back that remains — and the file does not claim it.

**The repair, and why it is shaped this way.**

An exception table maps a FAULTING INSTRUCTION to the one that should run
instead.  That is a statement about a specific machine instruction, so the
instruction cannot be left to a compiler: a C loop of byte stores may be
vectorised, unrolled, or emitted at several addresses, and a table with one
entry would then cover one of them.  The store is therefore one `rep movsb` in
`usercopy_asm.S` — one instruction, the whole copy, and on a fault the hardware
leaves `%rcx` holding exactly how many bytes were not transferred, so the
landing pad needs no bookkeeping of its own.

The table lives in `.ex_table`, collected into rodata between
`__ex_table_start` and `__ex_table_end` by the linker script, so adding an
entry is one `.quad` pair in the file that needs it and nothing anywhere else.
`idt.c` consults it before it decides to halt, for `#PF` and `#GP` only: the
mapping going away, and the same access under SMAP or at a non-canonical
address.  Any other exception on that instruction is not the case this is for
and still halts.

**The range check is NOT removed, and the table is not a substitute for it.**
The check is what refuses a bad address; the table is what survives a good
address that stopped being one. A fault there means "this call failed", not
"this caller was malicious", so `copy_to_user_checked` returns failure and a
partial write is reported as a failure too — the caller is not handed a
half-filled buffer it has no way to measure.

**How it is tested, given that the path it protects is a race.**  A race is not
something a test can schedule.  So the kernel fires the protected instruction
at a NON-CANONICAL address at boot — a shape the hardware rejects on every
x86-64, which nothing can accidentally map — and checks three things: that the
store wrote no bytes, that the fixup counter moved by exactly one, and,
implicitly, that there is a line after the call at all.  That last one is the
real assertion: before the table, this printed nothing, because the machine
stopped inside the store.  The headless gate requires the marker.

What this does NOT buy is seL4's guarantee.  seL4 proves its kernel never
faults; IRIS now survives the one fault it knew it could take.  The difference
is that the proof covers instructions nobody thought about, and this table
covers the one somebody did.
 — recorded, not staged

The table above tracks mechanisms with a retirement stage.  These seven are
different: they are ways IRIS's KERNEL is built that differ from seL4's, found
by comparing the two architectures rather than by auditing a migration.  None
is a purity violation under the charter's invariants — they are not ambient
authority, not a second namespace, not policy in the kernel — but the charter
(§5) requires every semantic divergence from seL4 to be documented and marked,
and these were not.  They are recorded here with the state they are actually
in.

Some are DELIBERATE and therefore also appear in the charter's §6 divergence
register; the rest are consequences of how IRIS was built and could be
revisited.

**Read D-7 first if you are comparing the two kernels.**  It is the one that
explains the others: object lifetime by reference counting rather than by the
derivation tree is upstream of the CSpace-cycle row in the table above, of the
self-naming-slot defect Stage 7-proc fixed, and of the unbounded revoke D-8
records.  It was missing from this table until the Stage 7 code audit.

| # | Divergence | IRIS | seL4 | Consequence | State |
|---|---|---|---|---|---|
| D-1 | **Per-thread kernel stack; threads block IN the kernel** (the memory SOURCE is the staged row "kernel stacks / PML4 from the PMM reserve" above; this entry is about the architecture, which is not staged) | Every thread gets `TASK_STACK_SIZE` (8 KiB) of kernel stack plus a guard page, allocated with `pmm_alloc_pages(2)` from the kernel's PMM reserve — including a TCB retyped from an Untyped, whose *payload* the user paid for but whose kernel stack the kernel still supplies.  A blocking syscall parks the thread with its kernel state live on that stack (`saved_krsp`, `TASK_BLOCKED_*`) | Event-based kernel: ONE kernel stack per core, no thread ever blocks in the kernel.  A long operation returns to a preemption point and the syscall is restarted | Cost per thread the user did not pay for; kernel memory that scales with thread count; and it is the structural reason seL4 can bound in-kernel latency (and be verified) while IRIS cannot claim either | **CLOSED (Stage 9-evt).**  Converting IRIS to a single-stack event kernel is a rewrite of every blocking path, not an increment, and it must land BEFORE Stage 9 (SMP) so the atomicity properties are re-derived once rather than twice.  It decomposes into three steps whose ORDER is forced, not preferred: **(1) make blocking handlers RESTART-SAFE** — hold no live state across the block, express the continuation in thread state, and prove it by actually re-executing them; **(2) abandon the syscall frame** instead of parking it, so the stack is dead while blocked rather than live; **(3) one kernel stack per CORE**.  Step 2 is mechanical once step 1 holds everywhere and unsafe before it, because a handler keeping a local across a block needs the frame step 2 throws away.  **ALL THREE STEPS ARE COMPLETE.**  IRIS has ONE kernel stack per core and no thread blocks inside the kernel.

Step 2 landed: a syscall that parks ABANDONS its frame.  `task_park_restart`
points the outgoing thread at `syscall_restart_trampoline` with `saved_krsp`
reset to the top of its kernel stack, and throws the integer context the switch
would have saved into a discard buffer — so a blocked thread's kernel stack
holds nothing.  The FPU state is still saved into the real buffer, because the
thread parked with its user's SSE registers live and the frame is disposable
while those are not.  It resumes at the trampoline on a fresh stack, re-runs
the syscall from thread state, and returns to ring 3 through an iretq built
from `sc_user_rip/rflags/rsp` — saved into the TCB on every syscall entry,
since the frame that used to hold them is what step 2 throws away.

**And from `sc_user_regs[6]`, which the first design missed.**  A procedural
kernel preserves the caller's `rbx`, `rbp` and `r12`-`r15` for free: it obeys
the C ABI, so those values either stay in registers or get spilled onto the
syscall's own frame, and the normal exit finds them intact.  Abandoning the
frame throws the spills away.  The failure was not subtle to observe and was
very easy to misread — a service resumed from a blocking syscall and took a
page fault writing to `0xFFFF8001...`, a kernel stack address, at a user RIP
that was correct.  It looked like a wild pointer; it was `rbp` still holding
what the kernel had left in it.  So the six callee-saved registers are pushed
at syscall entry alongside everything else and restored on the way out.  That
is six pushes on the syscall path, and it is the same bill seL4 pays: an event
kernel has no frame to leave a user context on, so it saves the whole context
into the TCB on every entry.  There is no cheaper version of this.

`syscall_return_to_user` takes the user context in REGISTERS and the saved
block by POINTER rather than reading either GS-relative, so no `struct task`
offset is hard-coded in assembly and a field added in C cannot silently
corrupt a register restore.

There is a fallback and it is honest: when nobody else can run, the park
declines and `syscall_run` yields through its own frame exactly as step 1 did.
Abandoning is worth nothing when the alternative is idling on the same stack.
A separate gauge (`syscall_abandons`, distinct from `syscall_restarts`) counts
only the real abandonment, because the two are indistinguishable from ring 3
and only one of them is what D-1 is about; T310 asserts it advances.

**What step 3 still needs**, and the first analysis missed it: it is not enough for syscalls to stop keeping state on the
stack.  A timer interrupt fires while a task runs in USER mode and lands on the
kernel stack named by TSS.RSP0; if that stack is per-core and the ISR then
preempts to another task, the outgoing task's interrupt frame is on a stack the
incoming task is about to use.  So step 3 additionally requires the IRQ path to
save the full user context into the TCB rather than leave it on a kernel stack
— which is a second conversion, of the preemption path, comparable in size to
the first.

**Half of that is now done.**  `struct iris_user_ctx` is the 22-word frame
`isr_common` builds, and it is a field of `struct task`: every ring-3 kernel
entry copies the thread's whole register state into its own TCB, and every
ring-3 exit rebuilds the frame from the TCB of whatever thread is current by
then.  One definition is shared by the assembly, the C handler and the TCB,
because three copies of a 22-field layout is three chances to transpose two of
them.

It is deliberately a no-op today, and that is the point.  Kernel stacks are
still per-thread, so a handler that switches away switches stacks too and comes
back to its own frame; the save and the restore are an identity that costs two
word-copies per interrupt.  Doing the move WHILE the frame is still
authoritative is what makes the remaining half a change to where execution
resumes rather than a change to what gets restored — the two would otherwise
land in the same commit, and a wrong restore would present as a service
faulting on a wild pointer with nothing to bisect.

A path whose effect is currently invisible is a path that rots, so it is
measured: `irq_ctx_saves` in `SYS_UNTYPED_QUERY`, and T314, which spins in ring
3 with known values in rbx and r12-r15 across thousands of preemptions and
checks every one of them.  That test cannot fail today.  It is written now so
that when the frame stops being the authority, a restore that drops or
transposes a register fails THERE.

**Step 3 is closed, and the shape it took is worth recording.**

`TSS.RSP0` and the syscall stack pointer are set ONCE, at `core_dispatch_init`,
and never change: every entry from ring 3 lands on the core's stack, whichever
thread it belongs to.  What made that possible is that nothing ties a thread to
a stack any more.  Its ring-3 context went into the TCB in step 3's first half;
its FIRST entry frame — initial rip, rsp, flags and argument, previously pushed
onto its kernel stack at creation under `user_entry_trampoline`, which is why
every thread needed a stack before it had ever run — went there too.

The DISPATCHER replaced the switch.  It runs on the core's stack, resets that
stack every time it is entered (which is what makes "abandon the frame" and
"give the stack back" the same act rather than two that have to agree), and
resumes a thread in one of three shapes: an iretq from its TCB, a first entry
from its TCB, or a CALL — a parked syscall's restart trampoline, on the stack
the dispatcher is already standing on.  **`context_switch` is deleted**; it is
on no thread's path, and neither is any per-thread kernel stack.

Every path that used to hand the CPU away through its own frame became a mark
or a park.  The timer tick MARKS and the ISR's exit path dispatches, because a
handler that switched would hand the CPU away with its interrupt frame live on
a stack the incoming thread is about to use.  A userland fault marks the thread
blocked.  `SYS_YIELD`, self-suspend and `SYS_CLOCK_NANOSLEEP` park.  A dying
thread leaves through the dispatcher, which is what lets the reaper free its
storage: nothing is standing on it by then.  `task_yield`, `task_yield_impl`
and `scheduler_sleep_current` are deleted.

**The idle task is gone.**  It was the boot thread yielding for ever, and it is
why per-thread stacks could not go — idle was a thread with a stack like any
other, and every "nobody else can run" answer had to be a switch to it.  The
answer is a `hlt` on the core's own stack.

**What it bought, measured**: `kstack_alloc` is deleted.  A thread owned two
pages of kernel stack plus a guard page, taken from the kernel's physical
reserve at creation — including a TCB retyped from an Untyped, whose payload
its payer bought and whose kernel stack the kernel supplied anyway.  That was
the largest standing exception to charter M3, and it was invisible from ring 3
until this stage added the gauge that makes T318 possible: create eight threads
and watch the kernel's reserve not move.  The old behaviour would show as two
pages per thread, exactly.

**One thing broke on the way and is worth the space.**  `SYS_TCB_RESUME`
refused a thread that had never been told where to start, and its witness was
`saved_krsp` being non-zero — true precisely because the entry frame lived on
the thread's own stack.  Moving the frame left the witness answering about the
wrong thing, and the boot died with NOT_SUPPORTED on the first spawn.  A stale
witness does not go quiet; it answers.  Syscalls themselves are not the obstacle: `SFMASK` clears IF, so no
syscall is ever preempted mid-flight and step 1's restart points are the only
places a thread gives up the CPU inside the kernel.  Sequenced accordingly, and
not started.  **Steps 1 and 2 have already paid for themselves**: step 1 is
what let D-8 close, and step 2 is what makes "no thread blocks in the kernel"
a claim about STACKS rather than about handlers.  Details of step 1:  Every blocking syscall in the kernel is restart-safe: `SLEEP`, `NOTIFY_WAIT`, `NOTIFY_WAIT_TIMEOUT`, `FUTEX_WAIT`, `EP_SEND`, `EP_RECV` and `EP_CALL`.  No syscall handler calls `task_yield` any more — the two that remain are `SYS_YIELD` and suspending yourself, which are reschedules rather than blocks with a continuation.  Each converted path expresses its continuation as state on the OBJECT and the THREAD; what the parked frames were actually holding, once the state was examined, was almost always a single REFERENCE rather than information, because the waker writes into the sleeper's TCB (a sender writes the message into the receiver's `ipc_msg`, a server writes its answer into the caller's).  That reference moved to `task.sc_held`.  Previously: **four paths in**: the dispatcher has a restart loop (`syscall_request_restart`, saved arguments and a re-entry flag on the thread), and `SYS_SLEEP`, `SYS_NOTIFY_WAIT`, `SYS_NOTIFY_WAIT_TIMEOUT` and `SYS_FUTEX_WAIT` are converted.  Each expresses its continuation as state on the OBJECT and the THREAD — a deadline, a waiter registration, a futex bucket entry — instead of as the rest of a C function on a kernel stack.  T310 pins it on the kernel's restart gauge, because from ring 3 a restartable wait and a parked one are indistinguishable.  Two lessons the conversion taught, both now structural: a handler cannot infer "resuming" from the state it parked on, because the WAKER clears that state (hence `sc_reentry`, owned by the dispatcher); and a wake reason that only the parked form could see must be recorded on the thread instead (a notification that closes marks its waiters, so a re-executed wait still reports `CLOSED` rather than `NOT_FOUND` for a slot the close emptied).  **What remains for step 1: `EP_SEND`, `EP_RECV`, `EP_CALL`** — the three that park holding staged capabilities, a partially built delivery and a reply binding, which is why they are last |
| D-2 | **CNodes have no guard** — CLOSED (Stage 8-cap) | Resolution is a pure radix walk: each level consumes `ctz(slot_count)` bits and indexes directly (`cspace.c`).  A CPtr's meaning is fixed by the CNode sizes along the path | CPtr resolution uses guard bits + radix + an explicit depth argument, so a CSpace can be sparse and levels can be skipped | IRIS's CSpace is a strict subset of seL4's: no sparse layouts, no depth-limited lookups, and a two-level CSpace costs the full radix of each level.  Nothing currently needs guards, which is why it has never bitten | **HALF CLOSED — Stage 8-cap.**  Guards exist on CNode CAPABILITIES: `SYS_CSPACE_SET_GUARD` (127) installs one, `KCSlot` carries `guard`/`guard_bits`, and the walk consumes and checks them.  Additive by construction — `guard_bits == 0` is every slot's initial state and resolves exactly as the pre-guard kernel did, which is what let 273 runtime tests and 18738 host assertions pass unchanged on the landing commit.  The guard is CAPABILITY-local, not object-local, which is the property that makes it seL4's guard rather than a lookalike: two capabilities to one CNode can be guarded differently and address the same object differently (host G-7).  Bit order within a level is [guard][index] MSB..LSB, matching seL4's resolution order, and Stage 4 Step 6b's injectivity rule is re-derived on top (host G-6).  Pinned by T306 (ring 3) and `test_cnode_guard` G-1..G-8 (host).  **The ROOT guard landed too, and D-2 is CLOSED.**  A thread reaches its root CNode through a structural pointer rather than a slot, so its guard had nowhere to live but the thread — installed by `SYS_TCB_CONFIGURE`'s arg3, which is seL4's `cspace_root_data`: the same argument, in the same position of the same operation, meaning the same thing.  arg3 had been reserved and ignored since Stage 7-proc retired the process argument, so this is additive for every existing caller, including the ones still passing a stale process capability whose low bits become a guard of zero width.  The resolver applies it only when the CNode it is walking IS the running thread's root, because that is the only capability the guard belongs to: a walk rooted at a destination the caller named, or at a child's root a spawner mints into, is a different capability and must not inherit it.  T312 asserts the property that would be lost by putting the guard on the KCNode instead — a parent and a child sharing ONE root CNode object address it differently, because a guard belongs to a capability and not to what it names, which is the same thing host G-7 asserts for the levels below |
| D-3 | **Different rights set** | `READ / WRITE / DUPLICATE / TRANSFER / WAIT / ROUTE / MANAGE`.  `RIGHT_DUPLICATE` gates minting a copy and `RIGHT_TRANSFER` gates sending one over IPC | `Read / Write / Grant / GrantReply`.  Copyability is NOT a right: whether a capability can be copied or minted follows from the derivation tree and the cap's type | A holder in IRIS can be given a capability it may invoke but not copy — expressible in seL4 only through the MDB, not through rights.  The two models are not translatable one-to-one, so "the same rights as seL4" is never a correct statement about IRIS | **DELIBERATE AND PERMANENT** (charter §6).  The decision this row was left open for, taken at Stage 8-cap, and the reasoning rather than the conclusion is the point.

**The two sets are not a superset and a subset; they cut differently.**  Mapping what is common: IRIS's `READ`/`WRITE` are seL4's `Read`/`Write`.  seL4's `Grant` — may capabilities travel through this endpoint — is IRIS's `RIGHT_TRANSFER`, moved from the ENDPOINT to the CAPABILITY BEING SENT, which is a narrower statement: seL4 says "this channel may carry capabilities", IRIS says "this capability may be carried".  seL4's `GrantReply` has no rights equivalent in IRIS and does not need one: the reply capability is a separate OBJECT (`KReply`, Stage 8-mcs), so "you may send me a reply capability and nothing else" is expressed by what the caller holds rather than by a bit on the channel.  `WAIT`, `ROUTE` and `MANAGE` are IRIS's own, and each names an authority seL4 splits across separate capabilities instead.

**What IRIS can express that seL4 cannot**, and the reason for keeping it: `RIGHT_DUPLICATE` makes a delegation NON-RE-DELEGABLE.  A holder can be given a capability it may invoke and may not copy.  In seL4 any holder may `seL4_CNode_Copy` anything it holds — copying grants no authority the holder did not already have, so seL4 treats it as harmless, and for AUTHORITY it is.  For CONFINEMENT it is not: a child that can copy its endpoint capability can seed a third party with it, and the supervisor's only recourse in seL4 is to revoke the whole subtree.  IRIS refuses the copy at the door.  Removing `RIGHT_DUPLICATE` would not relocate that property into the derivation tree — the tree records what was derived, it does not prevent deriving — it would DELETE it.

**Cost of the divergence, stated plainly**: the two models are not translatable one-to-one, so **"IRIS uses seL4's rights" is never a correct sentence**, and no document may write it.  Every rights argument in this codebase is IRIS's own set, and a reader coming from seL4 has to read this row before reading any of them.

**Pinned, not merely written down**: host RG-1..RG-5 assert the bit values, their distinctness, that `rights_check` requires ALL requested bits rather than any (an `&`-and-compare, not a non-zero test — the difference between "has some of this authority" and "has this authority"), that `RIGHT_NONE` never satisfies a check, and that `rights_reduce` cannot elevate.  A set declared permanent has to be a set that cannot drift |
| D-5 | **Memory objects are CHARGED to an Untyped, not RETYPED by the user** (Stage 6; PAGE TABLES and the ADDRESS SPACE converted in Stage 6-pure) | The kernel carves VMO pages and mapping records out of an Untyped the caller NAMED, and accounts each as a child of it | The user retypes each object explicitly and maps it (`seL4_X86_PageTable_Map`); frames, IRQ handlers and I/O ports have no kernel object at all | Accounting and revocation are equivalent — nothing is created without a budget, and a budget cannot be reset while its objects live — but the user does not choose WHEN or WHERE each level exists, and cannot hold a capability to an individual page table | **CLOSED (ledger D-5, final).  There is no KVmo.**  `kvmo.c` and `kvmo.h` are deleted, `KOBJ_VMO` is reserved the way `KOBJ_PROCESS` is — the enumerator's VALUE is the wire type `SYS_CAP_IDENTIFY` reports — and seven syscall numbers answer NOT_SUPPORTED permanently.

The object was a region the KERNEL owned on a holder's behalf: it allocated the pages lazily on a schedule nobody chose, kept a page-address array in kernel memory, and range-checked an offset into it.  That is a memory manager, and a microkernel does not have one — which is what every one of these calls was really asking it to be.  `SYS_VMO_CREATE` became `SYS_UNTYPED_RETYPE2` with `IRIS_KOBJ_FRAME`; `SYS_VMO_MAP` and `SYS_VMO_MAP_INTO` became `SYS_FRAME_MAP`, which already took the same four arguments in the same order (MAP_INTO was that call under another name, and MAP was it with the address space left implicit); `SYS_VMO_UNMAP` became `SYS_FRAME_UNMAP`, and the difference is the point — it took an address RANGE and no capability at all, "remove whatever is here"; `SYS_VMO_SIZE` became `SYS_FRAME_SIZE`, the same number (67) asking the same question of the object that answers it now.

The last one was the reason this row stayed open, and it is worth stating properly.  `SYS_VMO_MAP_PAGE` let a pager map page N of a region a client granted, at an offset the client named — the one shape a frame could not express, because a frame maps as a whole (D-10).  **A GRANT IS A RUN OF FRAME CAPABILITIES, ONE PER PAGE.**  Then which pages a pager may install is the set of capabilities it holds, whether it may install one writable is `RIGHT_WRITE` on that page, revoking one page is deleting one slot, and "offset past the end of the region" is an empty CSpace slot.  Every rule the VMO enforced survives, stated where seL4 states it, and the offset argument disappears because the question it asked is answered by which capability you were given.

Retiring it removed more than it replaced.  Subaction 4 of the pager probe went — it mapped a VMO page at an offset, and with the offset gone it IS subaction 1.  The probe's whole persistent PAGER SERVICE mode went, dead since Phase 28 gave the pager its own binary and still compiling because nothing sent it the command: **a dead code path is exactly where a retired object survives unnoticed.**  And the file-backed pager's cache and private-pool GRANTS went — the pager has retyped both pools from its own budget since its pages became frames, and nothing had read the grants since.  Authority nothing uses is what the manifest oracle exists to catch, and it was listing it as expected.

`kframe` loses `vmo_owner` and `kframe_alloc_vmo_page` with it: a frame could point at a VMO that owned its physical page and retain it, so physical memory had two owners and destroying the frame only delayed the other one.  It has one owner now — the Untyped it was retyped from.  The PMM's kernel reserve loses two of its four entries for the same reason.

Four things the conversion exposed, which is the argument for doing it rather than describing it.  (1) `SYS_VMO_MAP_INTO` flattened a wrong-type VSpace argument to INVALID_ARG — a resolver that knows exactly what the caller named, reporting only that something was wrong.  (2) `IRIS_CPTR_TEST_FIX_A` carried `RIGHT_WRITE` alone, chosen when its only probe was `EP_CALL`; its own comment says it exists so probes fail on TYPE, and under a call that also needs READ it was answering ACCESS_DENIED and passing a "wrong type" assertion for the wrong reason.  (3) **T196 never tested what it claimed** — it closed the address space BEFORE the memory object, so "a live mapping keeps its object alive" was measured with nothing mapped; it now releases the whole grant while a page is mapped, and three pages go while the mapped one stays.  (4) The page-table fixup scratch sat on object-CNode leaves 201/202, which are `IT_FAULT_LEAF(0)`/`(1)` — the mailbox a fault delivers a TCB into — and the fixup DELETES its slot before retyping a level into it. |

And the PAGER owns frames: its page cache and its private-writable pool are one capability per page, retyped on first use from its own budget, where they used to be two VMOs somebody granted it.  A pager holding a budget has no need of the kernel allocating for it on a schedule it did not choose.

**What is left is ONE path**: `PGR_OP_MAP_RESUME`, which maps a page of a region the CLIENT granted, at an offset the client names.  That page-at-a-time shape over somebody else's multi-page region is the one thing a VMO does that a frame does not.

**The replacement was built and measured, and it works.**  A grant becomes one FRAME CAPABILITY PER PAGE: the pager takes `PGR_PSLOT(grant, page)` and maps it, the rights on that capability decide whether the map may be writable (the same check, in the kernel rather than in a VMO's bookkeeping), and the test fixture becomes a run of one-page frames whose signatures are unchanged so the tests keep meaning what they say.  With it, **T201–T209 and T215 — the pager's core resolve, denial, multi-target and generation tests — all pass**.  It is not in the tree, and what stopped it is worth naming exactly rather than calling it "a project":

  1. **Ten tests (T191–T200) test KVMO AS AN OBJECT** — its size contract, its rights gating, its live count.  They fail by construction against a frame and must be retired WITH the object, after checking each property they assert is covered by the frame tests (T132/T133/T317) rather than lost with them.

  2. **A CSpace interaction that appears only once the fixtures are real frames.**  From T248 onward every test fails resolving `IRIS_CPTR_TEST_UNTYPED` or the object CNode with NOT_FOUND, while `SYS_UNTYPED_INFO` reports 98 MiB free — so it is not memory.  T245, the last test that passes, is the one that exercises `SYS_UNTYPED_RESET` on live pools.  That is a specific, findable thing and it has to be found BEFORE the change lands, not diagnosed afterwards from a suite that fails in sixty places at once.

Four slot collisions were fixed on the way, and they are the same mistake four times: a range read as free because nobody had named it.  The pager's frame slots landed on the page-table scratch the MISSING_TABLE fixup deletes and re-retypes; the fixture pool's Untyped sat in the ROTATING object pool, so it vanished when the counter wrapped and every fixture after that failed on a capability that had been fine an hour earlier — and that one HID the interaction above, because a pool that silently disappears makes the fixtures fail quietly instead of consuming anything.  **A slot map is not documentation; it is state, and the only honest way to read it is to check what writes to it.**

Two slot collisions on the way, both the same mistake as the earlier ones.  The pager's frame slots first landed on 54..69, which contains the page-table scratch (62) that the MISSING_TABLE fixup deletes and re-retypes — so the fixup turned a private page into a page table under a pager that still believed it held a frame, every fault stopped resolving, and no test could say why.  **A slot range is not free because nobody named it**, and that is now the third time this project has learned it.  A PAGE TABLE is retyped by the holder (`IRIS_KOBJ_PAGE_TABLE`) and installed by an explicit invocation (`SYS_VSPACE_MAP_TABLE`, seL4's `seL4_X86_PageTable_Map`); the kernel creates none, and a map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE`.  The ADDRESS SPACE itself is retyped too (`IRIS_KOBJ_VSPACE`, PML4 + header from one region), as is the root CNode, and a thread is CONFIGURED with both — `SYS_TCB_CONFIGURE(tcb, cnode, vspace)`, which is `seL4_TCB_Configure` outright since Stage 7-proc retired `SYS_PROCESS_CREATE` and the identity check that used to make the pair agree with a third object.  The address is validated as authority (a kernel-half install is refused).  The exclusive binding went with the per-process teardown that required it: threads sharing a CSpace and a VSpace is not a hazard to refuse, it is the definition of a process.  T301/T302 and host PT-1..PT-11 pin it.  What is still CHARGED rather than retyped: VMO pages and mapping records.  That frames, IRQ handlers and I/O ports have a kernel object at all is the deeper half of this row — a change to what a capability IS, not to who pays — and retires with the memory server |
| D-6 | **The MDB has unparented capabilities (LEGACY_ROOTs)** | 43 live roots of 335 MDB nodes, measured by T305 at Stage 7 close.  A LEGACY_ROOT sits in a CSpace with no parent, so `SYS_CSPACE_REVOKE` — which walks descendants — can never reach it | Every capability is a node in the derivation tree; the only roots are the initial capabilities the kernel installs at boot | Three classes.  BOOT PATH: legitimate and permanent (seL4's BootInfo capabilities are roots too).  KVmo PUBLISHES: a VMO was fabricated rather than retyped, so it had no ancestor to name — **this class is GONE with the object (D-5)**: memory is retyped from an Untyped, and a retyped capability has a parent by construction.  FAULT DELIVERY: the faulting thread's capability is published into a mailbox unparented, so revoking the supervisor's thread capability does not reach the kernel's copy — this one is a defect | **FAULT-DELIVERY CLASS CLOSED; SELF-CAPABILITY CLASS CLOSED; KVmo CLASS CLOSED — it retired with the object (D-5), which is the cleanest way a divergence can close: not fixed, but no longer constructible.**  A fourth class turned up while migrating services to their own IPC buffers, and it was self-inflicted — and it is CLOSED TOO: the SELF syscalls are RETIRED (A-18), so the class is no longer constructible either.  `SYS_VSPACE_SELF` and `SYS_TCB_SELF` published LEGACY_ROOTS, and said so at the publish site — "the caller's own address space is an attribute of being a process, not something another slot granted", which is true of the OBJECT and false of the CAPABILITY.  seL4 has no such syscall: a thread is given its VSpace and its TCB by whoever configured it, which is what makes them revocable by that creator.  The loader retyped both and holds both through the spawn, so it now MINTS them into the child at `IRIS_CPTR_OWN_VSPACE`/`IRIS_CPTR_OWN_TCB` — gated on the child having been given a budget, because the rule states itself: a service given memory to create objects from is given the address space to map them into.  No new authority (a thread could always name both), an ancestor for the capability that says so.  Measured: the six-service migration pushed the inventory to 47 roots through the SELF syscalls and delegation brought it back to 43; **A-14 took it to 32, and A-18 to 23** by giving every object retyped from a SECOND-LEVEL Untyped the MDB parent an open-coded `< 1024` had been denying it, and T305 now asserts that as a CEILING rather than only checking for growth — a producer that was there at boot is invisible to a no-growth check, which is how A-14 survived a test that printed its number every run.  The pager's manifest oracle was re-stated deliberately rather than loosened — it now counts slots 18 and 19, because a capability sitting in a slot is authority whoever reads that oracle has to account for.  The delivered capability is a child of the TCB slot the registrant named when it armed the handler — the same rule Stage 2 set for IPC — recorded at registration (`task.fault_src_cn/idx`, CNode retained) and verified by IDENTITY at delivery (`kcnode_slot_holds`), because a slot is a reusable location and between arming and faulting it can hold something that never authorised anything.  A registration whose source slot no longer holds the thread still delivers (a handler with a signal and no thread to answer with is a deadlock dressed as a working handler) but as a root, and the gauge counts it.  43 → 39.  T305 pins the count against growth and asserts that a live fault delivery adds no root |
| D-4 | **No per-thread IPC buffer object** | A message carries four inline words plus an optional bulk payload staged through `task.ipc_kbuf` (256 B inside the TCB) and copied to/from a user pointer named per call; one capability per message | Each TCB has an IPC buffer FRAME the user registers (`seL4_TCB_SetIPCBuffer`), holding the message registers beyond the physical ones and the extra-capability slots | The bulk-payload path is kernel-staged rather than user-provided, so its size is a kernel constant instead of a frame the user chose and paid for | **CLOSED (Stage 8-cap).**  `SYS_TCB_SET_IPC_BUFFER` (130) is seL4's `seL4_TCB_SetIPCBuffer`: a thread registers a FRAME it retyped and mapped, and the kernel transfers payloads between the two ends' frames through its own physical window.  Three properties change at once and each is asserted by T313 — the SIZE is the frame's (the test moves 600 bytes, which the 256-byte staging could not carry), NO USER POINTER is named on either side (so there is no address to validate and none for a second thread to invalidate between the check and the copy), and the buffer IS a capability, refused like one when it is not a writable frame (host TB-9).  It is also strictly less work: the staging path copies user→kernel, kernel→kernel, kernel→user and validates a pointer on two of those; two registered ends exchange ONE copy.  **Adoption has started and the first blocker is gone.**  Every service now holds a capability to the Untyped its address space was already charged to (`IRIS_CPTR_OWN_UNTYPED`; kbd, sh and console were the three that did not), so a service can retype a frame at all — it could not before, and that, not the kernel, was what stood in the way.  `services/common/iris_ipc_buffer.h` does the whole registration, including retyping and installing the paging levels the window needs, because since Stage 6-pure the kernel creates none and an IPC buffer lands where nothing has been mapped: on a fresh address space all three levels under the PML4 are missing.  **EVERY SERVICE THAT SENDS A BULK PAYLOAD IS MIGRATED** — init, console, svcmgr, vfs, sh and the pager.  `fb` and `kbd` are not, and the distinction is worth stating rather than rounding to "every service": neither sends a payload at all (kbd is assembly and speaks in registers), so neither needs a buffer, and there is no staging path left for them to be quietly using.  The fact is now MEASURED rather than trusted: `ipc_buffers` in `SYS_UNTYPED_QUERY` is a LIVE count of threads holding a registered buffer, and T313 asserts both that it moves when this thread registers and that at least three services already hold one.  That instrument exists because the migration fails silently by construction — a service whose registration is refused keeps working on the staging path and the whole suite passes either way, which is exactly what happened on the first service tried (the frame mapped into a window with no paging levels under it, and the kernel creates none).  **What keeps this MIGRATING**: the 256-byte `ipc_kbuf` is still in `struct task`, because every other service still takes that path.  vfs was the one that was not mechanical, and the fix is worth recording because it is the general answer: an IPC buffer is ONE page and the kernel reads a send from offset 0 of it, ignoring `buf_uptr`, so a server that composes its reply while the REQUEST is still live cannot point both at the same page and hope.  vfs COPIES the request out the moment it arrives and composes the reply in the buffer itself — one 256-byte copy per request, buying a property that holds no matter what a handler does, rather than the property "every handler happens to finish with the path before it starts writing", which nobody declared and nothing checks.  The pager needed the same answer for the same reason, being a server on its control endpoint and a client of vfs on one thread.

**The sharp edge this exposed, and what was done about it.**  A thread with a registered buffer sends FROM it, so the kernel was ignoring `msg.buf_uptr` — and the shared console client marshals into a buffer its CALLER passes.  Five migrated services passed their own static array, and the kernel dutifully sent whatever was at offset 0 of their IPC buffer instead: every service log line came out as the last reply payload that service had composed.  A whole boot of corrupted console output, and **not one test failed**, because nothing asserts on log text.  The fix is not the five call sites: a send that names an address other than the registered buffer is now REFUSED (`INVALID_ARG`), so a marshalling mistake fails at the call site that made it rather than silently transmitting something else.  seL4 has no `buf_uptr` at all — there is one IPC buffer and that is where a message is marshalled; IRIS keeps the field for unregistered threads and makes disagreement an error for the rest.  T313's fourth leg asserts both halves.

**`ipc_kbuf` IS DELETED**, and with it the last of what this row was about: 256 bytes inside every TCB, carried whether the thread sent a payload or not.  A send with no registered buffer is now an error — seL4's answer, where the message registers travel in registers and anything longer needs somewhere that somebody owns.

The test suite was the last holder, and converting it took one change rather than forty: `SYS_TCB_SET_IPC_BUFFER` takes a TCB capability, so the CREATOR registers a buffer for each thread it makes, between WRITE_REGS and RESUME — while the thread is configured and not yet running, because a thread that started first would have a window in which it could send a payload with nothing to hold it.  The main thread's buffer goes through the same helper, so no root slot is consumed: the first attempt took two and broke four tests that measure when the root CSpace fills, which is an assertion this had no business changing.

**Three things the migration found, none of them in the kernel's IPC path.**  The suite's per-thread frames first came from its rotating object pool, which DELETES the slot it is about to reuse, and from the Untyped several tests REVOKE — either takes a live buffer's capability away from a running thread, and the kernel panicked on an active-reference underflow within three tests.  They have a pool and a CNode of their own now, which is the right shape regardless: an IPC buffer outlives every operation in a test, so borrowing a slot with a shorter life than the thing put in it was the mistake.  Slot 0 of a CNode is the null slot and refuses publication.  And T313's gauge leg compared against a sample taken at the top of the test, which made it an assertion about how many other threads happened to be alive; it is measured around the act now.  T313's fourth leg asserts the fallback still behaves (an oversized payload clamps to the kernel constant), so the old path cannot rot unobserved while it survives |
| D-7 | **Object lifetime is REFERENCE COUNTED, not derivation-driven** | Every `KObject` carries two atomic counters — `refcount` (lifecycle) and `active_refs` (published/CSpace) — and a `close`/`destroy` pair fired when each reaches zero (`kobject.c`).  An object dies when the last reference goes, wherever that reference lived | seL4 has NO per-object reference count.  An object exists while a capability to it exists, and dies when its Untyped is revoked or its last cap is deleted; `cteDelete`/`finaliseCap` walk the derivation tree, and ZOMBIE capabilities make a long deletion preemptible | Refcounting is a strictly weaker collector than the derivation tree, and the difference is not theoretical — it is the CAUSE of three entries this ledger already records as separate incidents.  **(1)** The CSpace-cycle row above: a CNode reachable only through another CNode keeps its own count above zero and is uncollectable.  seL4 does not have this problem because it does not count.  **(2)** The self-naming-slot defect Stage 7-proc fixed — a slot naming its own CNode took an ACTIVE reference, so the count never fell and the root CSpace never emptied — was a refcount lie, and the fix (take no active ref on a self-naming slot) is a special case bolted onto the counter rather than a property of the model.  **(3)** It is why `KProcess` had to break the root-CSpace cycle pre-emptively, at a moment it knew because it counted threads.  It also costs 8 bytes of atomics per object and an acquire fence per release | **SEMANTICS MET AND MEASURED; the gap is MECHANISM, and the one disagreement it produced is fixed.**

Four tests, and they were written in the order that made each one answerable.

**T322 — the rule itself, for every type RETYPE2 can make.**  Retype into slot A, copy into B, delete A: the region must still refuse to RESET.  Delete B: it must reset.  That is seL4's lifetime rule stated so it can be checked — an object exists exactly while a capability names it — and all ten types pass, including the two expected to be exceptions (a TCB and a VSpace).  A counter that over-counts fails the second half; one that under-counts fails the first.

**T323 — the same rule over shapes a counter could get wrong.**  The three incidents this row lists were not the two-capability shape: they were a slot naming its own CNode, a cycle through another, and a process object emptying a CSpace pre-emptively.  So a seeded generator builds chains of copies derived from randomly chosen earlier ones — real MDB depth and branching — and deletes them shuffled, checking BOTH directions after every single delete.

**T321 — reclamation, which turned out not to be the gap at all.**  An unreachable CSpace cycle is not collected: two CNodes naming each other keep each other's counts up, the budget reports BUSY, the memory is spent.  The refcount statement holds exactly as written.  But revoking the Untyped reclaims it anyway, because a capability's MDB parent is where it was DERIVED, not where it is STORED — a cycle in CSpace is not a cycle in the derivation tree — and the region then resets.  **That is seL4's guarantee too**, and being exact matters: seL4 has no idle collector either.  What seL4 has is that the derivation tree IS the lifetime rule, so there is no second structure to disagree with it.

**T324 — and there was a disagreement.**  Reading a capability retains what it names, so a slot that outlived its OBJECT panics the kernel; no test had ever read an idle slot, so no test could see one.  This one scans the whole pool, and it found a scheduling context at refcount 0 with a CSpace slot still naming it.  The cause is in the MCS donation return: a donation moves `sched_ctx` from lender to borrower without touching the refcount, which is correct exactly while the pointer and the reference stay together — and they come apart when the BORROWER DIES FIRST.  Its teardown released the reference and cleared its pointer, and `kreply_return_donation` handed the pointer back to the lender anyway.  The lender's own teardown released it a second time.  Fixed: the borrower's pointer is the proof the loan is outstanding, so no pointer means no loan and nothing to return.

And the corner that repair left open is closed: a loan with nowhere to go — the
lender gone, or holding a scheduling context of its own by the time it waited —
now RELEASES the reference instead of leaking it.  That was blocked by the host
suite, whose fixture modelled a scheduling context held only by the donation.
The kernel cannot construct one: a retyped object always has the slot that made
it.  So the fixture models the slot, and the rule can be enforced — a test suite
that breaks the contract it exists to check is a reason the contract cannot be
enforced, not an argument that it should not be.

What is left is the mechanism: two counters where seL4 reads the tree, 8 bytes and a fence per object, and the standing possibility that they disagree again.  That possibility now has a test that reads every slot in the suite's pool every run, which is what turns "we have not seen it" into "we looked" | **NO STAGE ASSIGNED — deliberate for now, and the honest reason is cost.**  Converting to derivation-driven lifetime means implementing seL4's `finaliseCap`/zombie protocol and making every destroy path reachable from a capability walk instead of from a counter reaching zero.  That is the same class of work as D-1 (a rewrite of a cross-cutting mechanism, not an increment) and it interacts with D-1: a preemptible delete needs a place to park its continuation, which an event kernel has and a per-thread-stack kernel does not.  Sequenced AFTER Stage 9-evt for that reason.  Until then: **"IRIS uses seL4's capability model" is true of derivation, delegation and revocation, and NOT true of object lifetime**, and no document may state it unqualified |
| D-8 | **`SYS_CSPACE_REVOKE` is not preemptible and not bounded** | `kcnode_slot_revoke` loops until the invoked capability's subtree is exhausted, re-taking the global `mdb_lock` (IRQ-off) each iteration.  Each iteration is short and lifecycle effects are correctly deferred outside the lock, but the SYSCALL does not return until the whole subtree is gone, and nothing bounds the subtree | `cteRevoke` is preemptible: it converts a capability under deletion into a ZOMBIE, returns to a preemption point, and the operation is restarted.  In-kernel latency stays bounded no matter how large the derivation subtree is | A ring-3 principal that can build a wide derivation tree can hold the CPU for as long as that tree is large.  Today the whole system runs 43 MDB roots over 335 nodes at depth 6 (T305), so it has never been reachable — but it is a latency bound the kernel does not have, in a kernel whose sibling property (D-1) is the other reason it cannot claim one | **CLOSED (Stage 9-evt).**  It was recorded as blocked on D-1, because a preemptible delete needs somewhere to park a continuation — and D-1's step 1 built exactly that.  `SYS_CSPACE_REVOKE` now revokes a bounded slice (`IRIS_REVOKE_SLICE` = 16) and asks to be re-executed while descendants remain; the dispatcher reschedules in between, which is the preemption point.  It needs no cursor and no zombie: every slice DESTROYS what it revoked, so the subtree is strictly smaller on re-entry and the same arguments mean less work each time.  The only thing carried across slices is the running total, because the caller asked once and expects one answer — T311 asserts both halves, that the restart counter advances (it really preempted) and that the reported count is the whole job rather than the last slice (the accounting mistake a sliced operation invites).  The kernel-internal callers pass `budget == 0` and stay unbounded: they run during teardown, with nowhere to return to.  **"On nobody's behalf" was too strong** and is corrected by D-12 — a ring-3 DELETE of a capability naming a large CNode tree reaches exactly this unbounded path, so the bound D-8 put on `SYS_CSPACE_REVOKE` has a sibling door it does not cover.  **Note what this did NOT need: zombie capabilities.**  seL4 needs them because its delete must be resumable mid-object; a revoke that destroys whole capabilities per slice leaves no half-deleted state to name |
| D-9 | **A device Untyped's object headers had nowhere to come from** | Boot now publishes the framebuffer's MMIO region as a DEVICE Untyped, minted into the root task's CSpace and described in BootInfo with `is_device = 1` — the flag that had been in the ABI since v1 and had only ever been written as 0.  Before that, `kuntyped_create` was called from exactly two places (boot, always RAM, and the device branch of retype, which already required a device Untyped), so **no device Untyped could exist** and invariants U11/U12 described an object the system could not construct | seL4's BootInfo lists device Untypeds alongside RAM ones; that is how a driver is handed an MMIO region as a capability and retypes frames from it | A device region is MMIO, so it cannot hold the HEADERS of the objects carved from it: a `struct KFrame` written into a framebuffer is pixels, and read back it is whatever the display controller left there.  The old code took those headers from `kslab` — the kernel allocating on somebody's behalf, charter M3's exact prohibition — and the only reason it was not a live hole is that nothing could reach it | **CLOSED (Stage 6).**  `SYS_UNTYPED_SET_DEVICE_BUDGET` pairs a device Untyped with a RAM one that pays for its headers, and an UNPAIRED device Untyped refuses to retype rather than falling back to kernel memory: the kernel does not know whose memory to spend and will not guess.  The pairing is set ONCE and retains the RAM Untyped, which makes two properties fall out of one retain — the budget's own RESET already refuses while any header carved from it is alive, and the pairing cannot move, so no header can be stranded in a region that is then reset.  T316 measures BOTH halves of a retype (header charged to RAM, page charged to the device region), because charging the wrong one to the wrong region is the mistake that would pass every other test.  **This unblocks D-5's remainder**: the framebuffer is now reachable as an Untyped a driver can retype frames from, which is the path that retires `SYS_FRAMEBUFFER_VMO` and with it the last kernel-fabricated memory object.  Multi-page frames landed with it (D-10), and **the migration is DONE**: `SYS_FRAMEBUFFER_INFO` reports the geometry and creates nothing, `fb` retypes one frame covering the whole region out of the device Untyped and maps it, and `SYS_FRAMEBUFFER_VMO` is retired — its number reserved, its code and `kvmo_wrap` DELETED rather than left undispatched, because a second way to reach the framebuffer that nothing tests and nothing revokes is worse than none.  T316 proves it end to end from ring 3 by observing that the region is CARVED and carries a child: a migration that had quietly fallen back would leave it untouched and every other test would still pass, because the screen is painted either way |
| D-10 | **A frame mapped only its first page** | `SYS_UNTYPED_RETYPE2` accepted any page multiple for a `KOBJ_FRAME` and `SYS_FRAME_MAP` installed exactly ONE PTE — `kframe_map_page` mapped `f->paddr` and never read `f->size`.  A caller who bought a 64 KiB frame spent 64 KiB of its Untyped and could reach 4 KiB of it, with no error anywhere: the other fifteen pages were charged, owned, and unreachable | seL4 has frame SIZES (4K, 2M, 1G on x86-64) and a map covers the whole frame; large frames are how a driver maps an MMIO region without thousands of invocations | Nothing had ever created a multi-page frame, which is why it went unnoticed rather than why it was acceptable.  It was found while working out what `fb` would need to retype the framebuffer out of the device Untyped D-9 published | **CLOSED (Stage 6).**  A map installs every page of the frame, an unmap removes every page, and the THREE bulk teardown paths walk the frame rather than the mapping record's first address — a cleanup that removed one page of sixteen would leave PTEs outliving the object that justified them, which is the one thing an unmap exists to prevent, so all four share `kframe_unmap_all`.  Two properties are deliberate.  The occupancy check runs over EVERY page before any PTE is installed, so a failed map changes nothing — checking as it went would leave six pages of somebody's frame in an address space whose owner was told the map failed.  And a partial install unwinds exactly what went in, because a holder that has to retype a page table and retry must find the address space as it left it, or the retry hits BUSY on its own leftovers.  T317 asserts the charge, that every page is distinct (a map that aliased them all onto the first would pass a single-page test), that one unmap removes all of it, and that an OVERLAPPING map is refused while changing nothing.  **First recorded as "refused, not fixed"** — the refusal shipped, and the fix followed once the shape of it was clear |
| D-12 | **CNode teardown is bounded by memory, not by a budget** (first written as D-11, which Stage 10-dma had already taken for the device-refusal witness) | Deleting the one capability that names a CNode empties its slots, which ends the last active reference of every CNode below it, and so on to the bottom of the tree.  Since A-39 the STACK cost of that is constant, but the syscall still does not return until the whole tree is gone.  `kcnode_slot_revoke_bounded` takes a budget; `kcnode_slot_delete` does not, and teardown calls the unbounded form | `cteDelete` is preemptible through zombie capabilities: a delete that cannot finish in one slice names its own unfinished state and is restarted, so in-kernel latency is bounded whatever the object graph looks like | Reachable from ring 3 by one syscall, which is why it is a row and not a note.  What limits it is the principal's own Untyped: a 1-slot CNode costs about 144 bytes, so the work is proportional to memory the principal was legitimately granted, never more.  That is the charter's own bound — what bounds an allocation is the budget it was given — applied to time instead of space, which is weaker than D-8's explicit slice and weaker than seL4's | **OPEN.**  Registered rather than fixed: the hole A-39 closed was ring-0 memory corruption, this one is latency, and the repairs are not the same shape.  D-8's trick does not transfer — it works because every slice DESTROYS what it revoked, whereas a half-emptied CNode must not become reachable again, which is the case seL4 answers with zombies.  The cascade list A-39 introduced is the natural place to park a continuation, so the eventual fix has somewhere to stand |

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

### A-9 — execution for a retyped TCB (`SYS_CSPACE_SELF`, `SYS_TCB_CONFIGURE`, `SYS_TCB_WRITE_REGS`)

**Change**: three new CPtr-only syscalls (119, 120, 121) and the retirement of
`SYS_THREAD_CREATE` (48).  A thread is created by retyping a TCB from an
Untyped, configuring it with capabilities to the CSpace and VSpace it runs in,
writing its entry registers and resuming it.

**Justification**: charter §2.2 O1 (every canonical object born from Untyped)
and §3.4/§3.5 (no global identifiers conferring authority, no index standing in
for a capability).  `RETYPE2(KOBJ_TCB)` produced inactive TCBs from Phase S2
onward and the roadmap parked their activation here because its ARGUMENTS are
capabilities that only existed after Stages 3–5.  `SYS_CSPACE_SELF` is the
enabling piece: `SYS_TCB_CONFIGURE`'s signature says capability, and every
process except the root task could only have offered a convention.

**Not new authority**: all three are CPtr-only, produce no handle, and name
nothing outside the caller's own CSpace.  `SYS_CSPACE_SELF` returns a
capability to the CNode the caller already resolves every CPtr through;
`SYS_TCB_CONFIGURE` refuses any CSpace or VSpace that is not the caller's own.

**Scope**: the ledger's "executable thread-create via pool" entry moves
ACTIVE_LEGACY → RETIRED; a new entry records `SYS_THREAD_START` as the last
pool-born execution path (Stage 7).  No allowlist movement.  Tests: T297 (new),
T148 pins 48, T083/T285/T287 re-anchored onto object-reported identity.

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

## Checkpoint C.1 — Versioned user-buffer ABI (Phase S2)

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

### A-13 — a leaf index is not a root slot, and a slot map is state

Found while closing D-5, and recorded because the finding is not about D-5.

The suite fabricates from a rotating pool of leaves in a second-level CNode,
and a two-level CPtr is `(leaf << 8) | root_slot`.  Six call sites undid that
arithmetic before releasing a pool capability — `cptr >> 8` — and handed the
LEAF index to a delete that reads a bare number as a ROOT slot.  Leaves run
4..199; the root slots the suite depends on live at 1..15, 55, 66, 80, 99.  So
every run silently deleted a handful of root capabilities, and the run survived
only because the rotating counter happened to miss the fatal ones.

It stopped missing them the moment the pager fixtures became real frames: the
extra allocations shifted the counter, a delete landed on slot 80 — the CNode
every fabrication goes through — and sixty tests failed at once resolving a
capability that had been fine an hour earlier, with 98 MiB free, so it never
once looked like memory.  **The change did not cause it; the change moved a
counter.**

Two things follow, and they are the reason this is a ledger entry.

**A comment is not a guard.**  The comment directly above `it_slot_delete`
already warned about exactly this failure, in these words — "getting this wrong
is silent and severe" — and six call sites did it anyway.  The arithmetic is
gone; in its place the helper REFUSES a root-level delete of a load-bearing
slot and COUNTS it, and T319 asserts the count is zero.  The class is a test
failure now, not a later NOT_FOUND with no author.

**A slot map is state, and the only honest way to read it is to check what
writes to it.**  This is the fifth slot collision this convergence has cost, and
the fourth found by reading "reserved" or "unassigned" as "free": the page-table
fixup scratch sat on object-CNode leaves 201/202, which are `IT_FAULT_LEAF(0)`
and `(1)` — the mailbox a fault delivers a faulting TCB into — and the fixup
DELETES its slot before retyping a level into it.  A map that says a range is
scratch is a claim somebody made once, not a fact about the system.

### A-14 — an open-coded `1024` made every sub-budget's children unrevocable

`cspace.h` says it in words: the CPtr/handle split has exactly one definition,
every caller tests it through `cspace_value_is_cptr`, and **nobody open-codes
`< 1024`**.  `SYS_UNTYPED_RETYPE2` open-coded it.

The consequence is not the one the number suggests.  A second-level CPtr is
`(leaf << 8) | root_slot`, which is `>= 1024` for every leaf above 3 — so the
guard was not distinguishing a CPtr from a handle, it was rejecting every
Untyped held BELOW the root CNode.  Those retypes fell through to the branch
labelled "a handle-named source has no CSpace ancestor" and published their
objects as LEGACY ROOTS: no MDB parent, unreachable by any revoke, and in a
region whose `SYS_UNTYPED_RESET` refuses while its children live — so the
memory was reclaimable by nobody.

The suite holds every budget it carves in a second-level CNode, which is most
of what the system retypes at runtime.  Measured: `mdb_legacy_roots` 43 → 32,
`mdb_max_depth` 6 → 7.  The tree got deeper because a whole layer had been
falling out of it.

Found by building a CSpace cycle and asking whether the memory comes back
(T321) — which was a question about D-7 and turned out to be a question about
this.

### A-15 — D-7's severity, measured

The D-7 row says refcounting is a strictly weaker collector than a derivation
tree and names the CSpace cycle as the case that shows it.  That was recorded
as known and unexercised.  T321 exercises it, and the answer changes what the
row should claim:

1. **An unreachable cycle is NOT collected.**  Two CNodes naming each other
   keep each other's counts above zero; with no external capability left, the
   budget still reports BUSY and the memory is still spent.  The refcount
   statement holds exactly as written.
2. **Revoking the Untyped reclaims it anyway**, because a capability's MDB
   parent is where it was DERIVED, not where it is STORED — a cycle in CSpace
   is not a cycle in the derivation tree.
3. **So does the region reset afterwards**: the memory is genuinely back.

That is also seL4's guarantee, and it is worth being exact about which one.
seL4 has no idle collector either: an unreferenced object there is not freed
until its Untyped is revoked or reset.  What seL4 has that IRIS does not is
that the derivation tree IS the lifetime rule, so there is no counter to
disagree with it — and the three incidents D-7 lists are all disagreements.

**The gap is mechanism, not reclamation**, and D-7 should say so.  Nothing in
IRIS is unreclaimable; what is different is that reclaiming it goes through a
second structure that can be wrong, and has been wrong three times.

### A-16 — the purity gate, tested, and what testing it found

The gate's own comment said its reachability check was an approximation: "a
deeper chain slips through, and the answer to that is to keep the chains
shallow rather than to build a call-graph analyser in bash."  Keeping chains
shallow was a hope with nothing enforcing it, on charter M3 — the invariant the
whole purity model rests on.

So the gate was tested the way anything is tested: by giving it a violation and
watching.  One hop it caught.  Two hops — a syscall handler calling a helper
that calls `kcnode_alloc` — it passed, silently, with no new occurrence of the
token anywhere so the count check stayed quiet as well.

The set is CLOSED now: seed it with every function whose body names the
allocator, then repeatedly add every function whose body names something
already in the set.  It converges in two rounds at 11 functions — no explosion,
which was the reason not to try — and the round it stops at is printed, because
an approximation that says how far it went beats one that does not.

Three things that closure and its test found, and none of them was the thing
being looked for:

**Two real M3 holes.**  `kioport_alloc_from` and `kirqcap_alloc_from` fell back
to the kernel slab when the caller named no budget — the kernel spending its
own memory because the caller did not say whose to spend.  That is the same
hole D-9 closed for device Untypeds, left open here because nothing passes a
NULL pool: "unreachable today" is exactly how the last one was described.  Both
now refuse, and `kioport_alloc`/`kirqcap_alloc` are DELETED because the
fallback was their only caller.  Two allowlist entries go with them.

**The gate could not tell code from prose.**  A grep for an allocator's name
hit the comments that explain it, and a gate that cries wolf gets an exemption
written for it rather than a fix.  Comments are stripped now.

**The closure had a blind spot the size of a one-line function.**  Its scanner
recorded a definition line and skipped to the next — so a function whose whole
body is on that line was invisible, and the probe that motivated the check went
on passing after the check was written.  Found by re-running the probe against
the fix instead of assuming the fix worked.

### A-17 — a capability belongs to the pool that matches its LIFE

T324 counted 52 capabilities left in the suite's rotating object pool, and the
histogram named the biggest holder immediately: 22 TCBs.  `it_thread_create`
retyped each thread's TCB into a rotating leaf and returned the CPtr — and of
its 47 call sites, most discard it (`if (it_thread_create(...) < 0)`).  A slot
nobody keeps is a slot nobody can release, so the contract could not be fixed
where it was broken.

Threads have their own CNode now, and a leaf there is reused only when the
thread it names is PROVABLY gone — asked with `SYS_TCB_GET_INFO`, not assumed,
because "the test that made it has finished" and "the thread has exited" are
different statements and the rotating pool's whole defect was treating them as
one.  52 → 27, with T308 and T309 also releasing the endpoints, replies,
notifications and scheduling contexts they had been abandoning.

The instructive part is the repair that did NOT work, and why.

Moving `it_tcb_self_slot` to the same CNode looked identical — a capability the
suite holds, out of a recycling pool — and it added **36 unrevocable MDB roots**.
`SYS_TCB_SELF` publishes a LEGACY ROOT every call (D-6: no ancestor, no revoke
reaches it), eleven call sites use it, two inside loops, and tests take it, use
it and abandon it.  In the rotating pool those are recycled and cost nothing;
in a stable CNode they accumulate forever.  T305's ceiling caught it in one run.

Memoising it — publish once, hand the CPtr back — was the other obvious repair
and it hung the suite: T083 asks for a capability it can close and revoke on its
own account, and a shared one makes that the suite's.

So the rule is not "stable slots are better".  **A capability belongs in the
rotating pool when its life is a TEST's, and in a dedicated CNode when its life
is a THREAD's or the run's.**  Getting that backwards trades a capability that
gets deleted too early for one that can never be revoked at all.

**And the debt at rest was the wrong number to chase.**  27 capabilities left
in the pool are harmless until the allocator comes round to them, so the
allocator was consolidated — five copies of the same five lines — and made to
COUNT the coming round: how many times a leaf was recycled while it still held
something.  35 a run.  Thirty-three of those were `SYS_TCB_SELF` publications
made inside loops, one abandoned per iteration; the calls hoisted out and
released took it to 5, and the at-rest count to 26.

Both are asserted ceilings now, so both can only go down.  Not all five
remaining evictions are defects — recycling what a finished test abandoned is
what the pool is FOR — and the allocator cannot tell the two apart from the
inside.  That is precisely why the number is capped rather than explained: the
ones that are defects are indistinguishable until they cost something, and four
slot collisions in this convergence are what they cost.

### A-18 — the last ambient authority, named and mostly removed

`SYS_VSPACE_SELF`, `SYS_CSPACE_SELF` and `SYS_TCB_SELF` handed a thread
capabilities to its own address space, CSpace and thread **on request, asking
for no capability at all**.  That is ambient authority — charter A5's subject —
and seL4 has none of it: a thread is given those by whoever configured it, and
the root task finds them in BootInfo.  They also published MDB LEGACY ROOTS, so
D-6 and A5 were the same defect seen from two sides.

**Two of the three are RETIRED.**  An address space and a CSpace are facts about
the PROCESS, so one delegation covers every thread in it: the loader mints
`IRIS_CPTR_OWN_VSPACE`, `IRIS_CPTR_OWN_TCB` and now `IRIS_CPTR_OWN_CSPACE`
before the child's first instruction, and the root task uses the BootInfo slots
it already had.  Every caller — the loader itself, fb, vfs, the pager, init's
selftest and the whole suite — now derives from the delegated capability, which
makes each of them a child in the derivation tree instead of a root.

Removing them exposed three things that had been hiding behind them.

**The delegation was gated on the wrong question.**  It read `own_budget_slot`
— the slot THIS loader mints its pool into — so a child handed a budget by any
other route counted as one that creates nothing and was denied its own address
space, thread and CSpace.  `iris_test` is exactly that child: it is given
`IRIS_CPTR_TEST_UNTYPED` by init and creates thousands of objects from it.  The
gate now asks the question it meant to ask — is any capability this child gets
an Untyped — of the capabilities the child actually gets.

**The root task's own BootInfo slot was under-powered.**  `BOOT_CPTR_VSPACE`
carried `READ | DUPLICATE | TRANSFER` and no `RIGHT_WRITE`, so the root task
could not map into the address space its own BootInfo handed it — and reached
it through the ambient syscall instead.  An ambient call was covering for a
capability that did not work.  seL4's `seL4_CapInitThreadVSpace` is a full
capability, and so is this one now.

**A slot's emptiness expires.**  T079 named slot 18 as "never minted — must not
resolve"; 18 became `IRIS_CPTR_OWN_VSPACE`, and the test kept passing because a
VSpace answers WRONG_TYPE, which is also negative.  T080 named 19, which became
`IRIS_CPTR_OWN_TCB`, and there the exclusive mint failed loudly.  One of the two
told us; the other did not.

**`SYS_TCB_SELF` went too, and the thing that had blocked it was one register.**
`IRIS_CPTR_OWN_TCB` names the thread the spawner configured — the process's
first one — so deriving from it answers a different question than a HELPER
THREAD asking about itself, which five of the suite's callers do.  Delegation
could not reach them: the loader never saw those threads, and the entry
argument arrived only in `rbx`, which a C entry point cannot read.

So the trampoline delivers it in `rdi` as well — the System V first argument —
and a thread written in C reads its own TCB capability as a parameter.  That is
the one per-thread channel a freshly started thread has, and with it, whoever
creates a thread tells it which thread it is.  All three syscalls are retired,
their numbers permanently reserved.

Measured: `mdb_legacy_roots` 32 → **23**.  Nine unparented capabilities a run
were these three calls.

### A-19 — P2 audited: what the kernel was deciding for somebody else

The charter row for P2 (the kernel implements mechanism, not product policy)
said PARTIAL and named the ioport whitelist, which Stage 5 had removed.  Nobody
had re-audited it since.  This is the audit: every fixed limit, default and
subsystem in `kernel/` read against the question "who decided this, and should
they have".

**Removed — the kernel was deciding.**

*A futex.*  `SYS_FUTEX_WAIT`/`SYS_FUTEX_WAKE` and 176 lines of hash table:
32 buckets by 8 slots, a ceiling of 256 waiters the kernel invented, and a
blocking wait keyed on a raw user ADDRESS rather than on a capability.  A futex
is a synchronization PRODUCT.  seL4 has none: one is built in user space from a
shared frame for the word and a notification for the sleep, both of which this
kernel already provides, and the notification wait has taken a timeout since
Phase 13.  Nothing in the system used it — the only caller was one test whose
subject was the futex itself, retired with it.

*A waiter ceiling.*  `KNOTIF_WAITERS_MAX` was 4, a fixed array inside the
object, and a fifth waiter got BUSY.  The kernel was deciding how many threads
may wait on a notification.  The unbounded mechanism was in the file next door:
`KEndpoint` has queued its waiters intrusively through the TCB since Phase 9.
The same kernel was answering the same question two ways.  T325 blocks six
threads on one notification — two past the old limit, so it fails against the
old code rather than merely passing against the new.

*A CSpace size.*  Retyping a CNode with `obj_arg == 0` meant 256 slots, and the
memory for them, chosen for a caller who had not said.  `seL4_Untyped_Retype`
takes `size_bits` and has no default.

*Two dead constants.*  `KSCHEDCTX_DEFAULT_BUDGET` and `_PERIOD` had no users at
all — policy the kernel carried without applying.

**Classified as mechanism — the kernel is not deciding for anybody.**

`CSPACE_MAX_DEPTH` and `KCNODE_MAX_SLOTS` bound a resolver walk and one
object's size; seL4 bounds both.  `KUNTYPED_RETYPE_MAX_COUNT`/`_BYTES` bound
one IRQ-off window, which is a latency property, and seL4 bounds retype by
preemption for the same reason.  `IRQ_ROUTE_MAX` is 16 because the legacy PIC
has 16 lines — a hardware fact.  `TASK_PRIORITY_MAX` (255) and a round-robin
time slice are seL4's arrangement too.  `MAX_CPUS`, `MAX_ORDER` and
`PMM_MAX_PAGES` are build and hardware bounds.  The kernel's `0x3F8` writes are
its panic serial, which seL4 also compiles in.

**What is left, named.**  `TASK_MAX` is 256 and it is a REAL ceiling: the
scheduler keeps an index-keyed registry (`ktcb_registry[]`) for thread
identity, and `task_registry_alloc` returns NO_MEMORY when it is full.  seL4
has no thread limit — a TCB exists because somebody retyped one, and the
scheduler reaches it through the capability.  The run queue is already
intrusive (the index-keyed `next[TASK_MAX]` arrays went in Phase S2), so what
remains is the identity registry alone: a pointer, a generation and an occupied
flag per slot, used to validate a thread pointer against a stale id.  Whether
the generation check survives at all is the real question — a TCB is a
capability now, and a capability's own lifetime already answers "is this thread
still the one you meant".

**And then that one was removed too.**

`ktcb_registry[TASK_MAX]` is gone.  Everything that read it was WALKING it —
three sweeps looking for a thread whose sleep or replenishment is due — so it is
an intrusive list through the TCB now (`sched_thread_list`), removal is O(1),
and `task_registry_alloc` cannot fail.  The tick's scan costs what the system
actually has rather than a fixed 256; the shape that removes the scan entirely
is seL4's release queue, ordered by wake time, and that is a separate change
with a separate reason.

The `generation` field went with the array.  It existed so a stale INDEX could
be detected, and there are no indices; a TCB is a capability, and a capability's
own lifetime already answers "is this still the thread you meant".

`task_create` and `scheduler_add_task` went too — a kernel thread built from the
static pool, with no callers, and the last way to make a thread that was not
retyped from an Untyped somebody holds.  With them gone the static pool shrinks
from 256 entries to **two**: the idle thread and the root task, built by boot
code out of memory no Untyped exists for yet, which is exactly the exception
seL4's root task is.

T326 asserts it the only way that is honest.  Not "count to 257" — that would
need 256 live threads with a stack each and would measure the suite's budget
rather than the kernel's rule.  It asserts that the EXHAUSTION counter is zero
and stays zero, because the only code that could increment it was the refusal;
a non-zero reading means somebody put a ceiling back.

**P2 is MET.**  36 of 36.

### A-20 — a file-by-file audit against seL4, and what it found

The convergence rows had been closed one at a time, each against the mechanism
it named.  This is the other direction: read the whole kernel and the whole
test suite against seL4's actual API and ask what is missing, rather than
asking whether each recorded item is done.

It found six things no row had named, and two of them contradict claims this
repo was making.

**FIXED — all three authority holes are closed; the rest of this row records
what the audit found and what it cost.**  `IRIS_BOOTCAP_SCHED_CONTROL` is a
boot control capability like the IRQ and ioport ones, published at
`BOOT_CPTR_SCHED_CONTROL` and carried in BootInfo (v6) exactly as seL4 carries
`seL4_CapSchedControl`; `SYS_SC_CONFIGURE` takes it as a fourth argument and
refuses without it.  `SYS_TCB_SET_PRIORITY` takes an AUTHORITY and refuses a
priority above that authority's ceiling, and a thread inherits the ceiling of
whoever configured it, so a bound travels with delegation instead of being a
number the kernel hands out.  `SYS_THREAD_PRIORITY` is retired.  T327 is the
gauge for all three, and it proves the priority bound with an authority whose
ceiling is ZERO — a retyped but unconfigured TCB — because a bound tested only
with values that happen to fit is not tested.

**No `SchedControl` — time is not a delegated authority.**  seL4 hands the root
task one `SchedControl` capability per core, and `seL4_SchedControl_Configure`
is how a budget and period get onto a scheduling context: the authority over
CPU TIME is a capability you are given.  IRIS's `SYS_SC_CONFIGURE` requires
only `RIGHT_WRITE` on the scheduling context itself, so anyone who can retype
an SC out of an Untyped they hold can grant themselves any budget over any
period.  The roadmap said of MCS "nothing in this dimension is still not
seL4's"; that was wrong, and the row now says what is.

**No maximum controlled priority.**  `seL4_TCB_SetPriority(tcb, authority,
prio)` bounds the new priority by the AUTHORITY thread's MCP — you cannot grant
a priority above the one you were granted.  IRIS's takes no authority argument
and no bound.

**`SYS_THREAD_PRIORITY` is ambient authority.**  It sets the CALLER's own
priority, up to 255, taking no capability at all — exactly the shape A-18 spent
its length removing from the CSpace side, sitting on the scheduler.  Priority
255 starves everything below it.  `SYS_TCB_SET_PRIORITY` is the capability-based
call that already does this correctly, so this one is a capability-free
duplicate.  Charter A5 goes back to PARTIAL for it.

**No ASID capability model.**  seL4 has `ASIDControl` and `ASIDPool`: an address
space must be assigned an ASID out of a pool somebody holds, which makes
creating one an authority and bounds how many can exist.  IRIS enables PCID in
the kernel and a retyped VSpace simply works.  Address-space identity is
kernel-managed, not capability-managed.

**Faults are not IPC.**  In seL4 a fault is an IPC MESSAGE on the faulting
thread's fault endpoint: the handler `Recv`s it, gets an implicit reply
capability, and REPLYING resumes the thread — a fault handler is just a server,
and the authority to resume is the reply capability.  IRIS signals a
notification, publishes the faulting TCB into a mailbox CNode, and the handler
reads the record with `SYS_TCB_FAULT_INFO` and resumes with
`SYS_EXCEPTION_RESUME` and a sequence number.  Equivalent in what it can
express, and a different structure: three mechanisms where seL4 reuses one.

**Two smaller absences.**  seL4 binds a notification to a TCB
(`seL4_TCB_BindNotification`) so a passive server blocked on an endpoint can
still take signals; IRIS cannot.  And seL4 has
`seL4_CNode_CancelBadgedSends`, which cancels the IN-FLIGHT sends of one badge
after revoking a delegation; IRIS revokes the capability and leaves whatever is
already queued.

**A fourth flattening.**  *(The counting stopped here and should not have:
A-30 found eighteen more and removed the convention outright.)*
`tcb_resolve` turned WRONG_TYPE into INVALID_ARG —
"something about your argument is wrong", from a resolver that had just
identified the capability exactly.  The typed resolvers had it until A-20's
type-before-rights fix and `dev_cap_budget` had it until D-5; this is the third
place and it was found by a test asserting the new behaviour and being told the
old one.  Six assertions across both suites had pinned the flattening.

**And the timed calls.**  `SYS_SLEEP`, `SYS_CLOCK_NANOSLEEP` and
`SYS_NOTIFY_WAIT_TIMEOUT` block on TIME inside the kernel.  seL4 has no timed
blocking at all: a timer driver holds the hardware and a client waits on a
notification the driver signals.  This is deliberate in IRIS and it is a real
divergence, because a kernel that can block on time owns a policy about time.


### A-21 — address-space identity becomes a capability (ASIDControl / ASIDPool)

A-20 found it and did not fix it: *"IRIS enables PCID in the kernel and a
retyped VSpace simply works.  Address-space identity is kernel-managed, not
capability-managed."*  This closes it.

**What was there.**  `kvspace_alloc` called `kvspace_tag`, which took the next
free bit out of a kernel-global PCID bitmap and wrote it into the VSpace's
CR3.  Three consequences, none of them visible from ring 3:

  - Creating an address space required no authority beyond the memory it was
    carved from.  The identifier arrived with the object.
  - How many address spaces the system could hold was a constant compiled into
    the kernel.  When the bitmap ran out, the kernel stopped being able to
    build address spaces, and nothing in ring 3 could see it coming, account
    for it, or be told it was their turn to stop.
  - There was nothing to revoke.  A namespace nobody holds is a namespace
    nobody can take away.

**What is there now**, which is seL4's arrangement:

  - `IRIS_BOOTCAP_ASID_CONTROL` is a boot control capability like the IRQ,
    ioport, debug and (A-20) SchedControl ones — published at
    `BOOT_CPTR_ASID_CONTROL` and carried in BootInfo, now v7.  It authorises
    exactly one thing: carving pools.  This is `seL4_CapASIDControl`.
  - `KOBJ_ASID_POOL` is a real object, retyped from an Untyped somebody holds,
    that owns a contiguous range of `IRIS_ASID_POOL_SIZE` identifiers.  The
    retype refuses without ASIDControl — holding the memory is not the
    authority.  This is `seL4_X86_ASIDControl_MakePool`.
  - `SYS_ASID_POOL_ASSIGN(pool, vspace)` issues one identifier from a pool the
    caller holds into an address space the caller holds.  This is
    `seL4_X86_ASIDPool_Assign`.
  - `ktcb_configure` refuses a VSpace that has not been assigned one.  This is
    the step that makes the rest mean anything: an unnamed address space is
    memory, not somewhere a thread can run.

**Two design points worth stating, because both could have gone the easy way.**

*The rule does not depend on the hardware.*  `kvspace_has_asid` deliberately
does not consult `iris_pcid_enabled`.  A machine with no PCID allocates the
same identifiers from the same pools and merely drops them on the way to CR3,
so the rule a program has to obey is the same everywhere.  The alternative —
skip the check where the tag register does not exist — would have made the
whole model decorative on exactly the configurations where it is cheapest to
be wrong about it.

*Identifiers are carved forward and never rewound.*  A destroyed pool does not
return its range to the global counter, because identifiers it handed out may
still be cached in a TLB under a walk that is gone.  Individual identifiers DO
come back to their own pool when the address space they named is destroyed, and
the pool outlives every space it named (the space holds a reference to it) —
so the recycling is bounded by the pool's own lifetime and never crosses it.

**The bootstrap exception, named.**  The root task's address space is built
before any Untyped exists, so there is no pool to assign from and nobody to
hold one.  It takes identifier `KASID_BOOTSTRAP` (1), stamped by boot, and
pools carve from 2 upward so no pool can ever issue it.  This is the same
exception the root task's CNode and TCB already are, and it is one line in one
place rather than a fallback path.

**The delegation chain.**  userboot receives ASIDControl in BootInfo, carves
ONE pool from its boot Untyped into `IRIS_CPTR_ASID_POOL`, and passes init both
halves.  init passes the POOL — never the control — to svcmgr and iris_test,
the two tasks below it that load children.  Leaf services get neither: a
service that never builds an address space for anybody else has no use for the
authority to name one.  `svc_loader` assigns an identifier to every child
VSpace it retypes, in the one place it retypes them.

**Gauges.**  T328 is the new one and its first claim is the load-bearing one:
a freshly retyped VSpace is refused by `TCB_CONFIGURE`.  If that ever passes by
accident the model is decorative.  It also proves the identifiers come back, by
building and destroying `IRIS_ASID_POOL_SIZE + 8` address spaces one at a time
through a single pool — a loop that can only finish if every name was returned.
T251's manifest grew an eleventh canonical type and, with it, the distinction
this row is about: a registered type you may not create is ACCESS_DENIED, an
unregistered one is NOT_SUPPORTED, and collapsing those two would let a retired
type code come back disguised as an authority error.  T305's legacy-root
ceiling goes 24 -> 25 for ASIDControl; the pool itself is NOT a root, because it
is retyped from an Untyped and parented there, which is the whole point of the
split.

**The recurring lesson, again.**  Nothing here broke a test.  295 runtime tests
and 19k host assertions passed with a kernel-global bitmap handing out
address-space identity to anyone who could retype a page.  Every property in
this row exists because an assertion was deliberately written for it.



### A-22 — a fault becomes an IPC message on an endpoint

A-20 found it and A-21's neighbour in the same audit: *"In seL4 a fault is an
IPC MESSAGE on the faulting thread's fault endpoint... IRIS signals a
notification, publishes the faulting TCB into a mailbox CNode, and the handler
reads the record with `SYS_TCB_FAULT_INFO` and resumes with
`SYS_EXCEPTION_RESUME` and a sequence number.  Equivalent in what it can
express, and a different structure: three mechanisms where seL4 reuses one."*
This closes it.

**What the three mechanisms each were, and what each of them was re-inventing.**

  - *The notification* carried the fact and nothing else, which is why a second
    mechanism was needed to carry the content.
  - *The mailbox* was a hand-rolled capability delivery: a CNode slot the
    registrant declared, that the kernel minted the faulting thread's
    capability into on every fault.  It needed its own parent tracking
    (`fault_src_cn`/`fault_src_idx`, verified by identity at delivery) so that
    revoking the supervisor's thread capability would reach the copies the
    kernel had handed out — a rule IPC already had, rebuilt for one caller.
    And it meant a pager held a TCB capability for every target it served,
    which authorises everything a thread can be made to do, in order to do the
    one thing it needed.
  - *The generation number* was a hand-rolled one-shot token.  "May resume this
    thread" was `RIGHT_WRITE` on a TCB capability — permanent, copyable, and
    just as valid for the next fault as for the one it was handed for — so a
    counter had to be bolted on and echoed back to tell a current answer from a
    stale one.

**What is there now.**  The faulting thread performs a CALL on an endpoint.
`SYS_TCB_SET_FAULT_HANDLER(tcb, ep)` points its faults there and captures the
BADGE on the capability used; every fault message carries that badge, which is
how a handler serving many clients on one endpoint knows whose fault it is.
The handler receives it like any other request, gets a reply capability with
it, and REPLYING resumes the thread.  `SYS_EXCEPTION_RESUME` and
`SYS_TCB_FAULT_INFO` are retired.  The wire layout of the record is unchanged
(`fault_proto.h`): it travels in the message registers instead of sitting in
the kernel waiting to be fetched.

**Three consequences worth stating, because each replaced something.**

*Nothing is minted into anybody's CSpace when a thread faults.*  The badge does
what the mailbox did, and it rides on the message.  `kfault_deliver`'s
capability-publishing half is gone, with its parent tracking, its identity
check and the two CNode references every armed thread used to hold.

*The one-shot is structural.*  A reply capability answers the one call bound to
it; a second use is NOT_FOUND because there is nothing left to answer.  T185
now proves this the hard way: it keeps a COPY of a reply capability while it is
still live, spends the original, lets the thread refault, and shows the copy
answers neither fault.  The generation number survives in the record for a
handler correlating logs, and gates nothing.

*A pager holds strictly less.*  It receives on an endpoint and answers with a
reply; it holds no capability to any thread it serves.  "Kill the faulting
thread", which used to be action 1 of `SYS_EXCEPTION_RESUME`, is now the
ABSENCE of an answer — a handler that drops the reply object leaves the fault
unanswerable, and the kernel destroys a thread nobody will answer, counting it
as a kill.  That choice is IRIS's and is stated here rather than assumed: seL4
would leave such a thread blocked forever.  Waking it instead would resume it
at the instruction that faulted, which faults again — a livelock nobody can
see — so the alternatives were a livelock, a permanent block, or the honest
report that the fault ended without being served.

**One property was genuinely lost, and it is the point.**  A supervisor used to
be able to LOOK at a fault it intended somebody else to serve, by polling
`SYS_TCB_FAULT_INFO` without consuming the notification.  Observation and
delivery were separate because they were separate mechanisms.  They are one
mechanism now, so receiving a fault IS taking delivery of it, and a fault taken
by the supervisor is one the pager will never see.  Six tests were built on the
peek and are rebuilt on what a supervisor can honestly do: watch the kernel's
delivery counter, and check the thread.

**Gauges.**  T329 is the new one, and its third claim is the one the mailbox
existed for: two threads armed on ONE endpoint through differently badged
copies produce distinguishable faults.  It also pins that `RIGHT_READ` on the
endpoint is what takes delivery — a write-only copy can ARM a thread's faults
and never see one — and that both retired numbers answer NOT_SUPPORTED to a
caller holding every capability there is to hold about the thread.  T140 keeps
the registration authority, T144 moves the resume semantics onto the reply
capability, T145/T146/T147 keep the teardown and churn contracts, T184 keeps
pager containment, and T307/T308 carry the timeout fault across unchanged.

**And a latent bug this shook out.**  T311 fanned 24 capabilities into leaves
100..123 of the suite's object CNode — inside the ROTATING pool it also draws
its source endpoint from.  Once the rotation reached leaf 117 the test deleted
its own source half way through the fan-out.  It had been correct only for as
long as nobody changed how many objects earlier tests allocate.  The codebase
had already named this hazard once, for T307's mailbox slot; this is the second
occurrence, and both are now above the pool.

**The recurring lesson, again.**  Nothing here broke a test either.  295
runtime tests and 27k host assertions passed with faults delivered by three
mechanisms, a TCB capability minted into a mailbox on every fault, and a
sequence number standing in for a one-shot capability.



### A-23 — a bound notification, so one thread can be a driver

A-20's audit listed this as one of "two smaller absences": *"seL4 binds a
notification to a TCB (`seL4_TCB_BindNotification`) so a passive server blocked
on an endpoint can still take signals; IRIS cannot."*  It is not small.

**What the absence cost.**  A thread blocked receiving on an endpoint is in
that endpoint's queue, and nothing else can reach it.  Every server that needs
BOTH an interrupt and a request queue — which is what a DRIVER is — therefore
had to choose one to block on.  Both drivers in this system chose the same way
out, and both paid for it with a timeout:

  - `kbd` drained its endpoint non-blockingly and then slept 10 ms on its IRQ
    notification, so the drain kept running with no key traffic.  A hundred
    wakeups a second, to find nothing, forever.
  - `svcmgr` did exactly the same with its service-death notification.

Both were busy-waits with a kernel timeout standing in for a thing they could
not say.  Neither is a timeout in any real sense: nothing was being given a
deadline, and nobody wanted to know that time had passed.

**What is there now.**  `SYS_TCB_BIND_NOTIFICATION(tcb, ntfn)` binds one
notification to one thread.  A signal that finds no waiter is delivered to the
bound thread even while it is blocked on an endpoint: it is dequeued from the
endpoint and given a message labelled `IRIS_MSG_LABEL_NOTIFICATION` with the
bits in `words[0]`.  A pending signal is also consulted on the way INTO a
receive, so a signal that arrives before the thread blocks is not lost.  One
notification per thread and one thread per notification, because "which thread
does a signal wake" must have exactly one answer; a second bind either way is
ALREADY_EXISTS.

**One design point.**  The binding takes a LIFECYCLE reference on the
notification and not an ACTIVE one.  An active reference is what keeps an
object open, so taking one here would mean a notification could never be closed
while a thread was bound to it — the binding keeping alive the very thing it
points at.  The lifecycle reference keeps the storage valid, `close` still
fires when the last capability goes, and `close` breaks the binding.

**Gauges.**  T330, five claims, of which the third is the one the absence was
about: a signal that arrives BEFORE the receive is consulted on the way in.
`kbd` and `svcmgr` are the two real users, and both lost their timeout with the
bind — the second-order proof that this was the missing piece rather than a
convenience.

### A-24 — waiting becomes a service, and the kernel forgets how

A-20 found it and named it precisely: *"`SYS_SLEEP`, `SYS_CLOCK_NANOSLEEP` and
`SYS_NOTIFY_WAIT_TIMEOUT` block on TIME inside the kernel.  seL4 has no timed
blocking at all... This is deliberate in IRIS and it is a real divergence,
because a kernel that can block on time owns a policy about time."*  It is no
longer deliberate.

**What the kernel was deciding.**  Three syscalls parked a thread with a
deadline and had the scheduler wake it.  That is a set of policies, none of
which a microkernel should hold: how long a thread may wait, whose waiting is
worth a kernel data structure, what "the deadline passed" means, and what
happens to a deadline nobody wants any more.  Every thread carried a
`wake_tick`; the tick swept the whole thread list for expiries; there was a
`TASK_SLEEPING` state and a `timed_out` flag and a fast-forward path in the
idle loop to make sleepers wake on a guest that delivers no interrupts.

**What is there now.**  A TIMER SERVICE, in ring 3.  It holds the timer
interrupt, takes "signal this notification in N nanoseconds" over an endpoint,
and signals.  A client waits the way it waits for anything else.  What used to
be a syscall number every task could reach for is a capability somebody granted
— delegable, revocable, replaceable, and refusable.

The kernel keeps the timer interrupt for preemption and MCS accounting, which
is what seL4's kernel does with its own; what it stopped doing is deciding who
waits.  The tick is OFFERED to ring 3 through the ordinary IRQ routing, so a
holder of an IRQ capability for line 0 gets a time base.  The kernel does not
mask that line for its holder, because it needs the interrupt itself: a handler
that never acknowledges slows nothing down, it simply stops being told.

**What the kernel lost, concretely.**  `TASK_SLEEPING`, `timed_out`,
`timeout_ns_to_deadline_ticks`, `knotification_wait_timeout`,
`knotification_wait_timeout_step`, the expiry sweep in `scheduler_tick`, and
the sleeper half of the idle fast-forward.  `wake_tick` survives with one
meaning instead of three: when a scheduling context's budget comes back, which
is a fact about the budget.

**The service is single-threaded, and could not have been before A-23.**  A
driver has to take both an interrupt and a request queue.  The timer service is
the first thing in this system that could not be written at all without the
bound notification, which is why the two rows are adjacent.

**Two additions the retirement required, both seL4's.**  `SYS_NOTIFY_POLL` is
`seL4_Poll`: a caller that used a zero timeout was asking "is anything there"
and having it answered by the timed-block machinery — the machinery went, the
question did not.  And `TMR_OP_CANCEL`, because a bounded wait that ends early
leaves a timer armed and a service never told holds one table entry and one
capability per abandoned wait.  Whose timer it is, is decided by the BADGE on
the capability the request arrived through, not by the token: a token is a
number, and a number is not authority.

**Three things a caller now sees that the kernel used to hide.**

  - *A stale timeout still fires.*  An armed timer nobody wants signals anyway
    unless it is cancelled, so `IRIS_TIMER_BIT` is reserved and callers mask it
    out.  The kernel used to cancel the deadline as it woke the thread, which
    is exactly the bookkeeping about somebody else's waiting it should not have
    been doing.
  - *A notification handed to the service is GIVEN AWAY.*  A caller derives a
    fresh copy per arm.  The service can signal what it was handed and nothing
    else, and the grant ends when the timer fires and the service deletes its
    copy.  (*Amended by A-29*: this said "transfer in IRIS is a move, so a
    caller derives a fresh copy per arm."  Transfer is a copy now, and the
    caller deletes its own slot after the arm — the same two steps, with the
    second one written down.)
  - *Waiting can be REFUSED.*  A task with no timer capability cannot wait on
    time.  That was never true of a syscall number.

**And a distinction the conversion forced.**  The suite's 71 `SYS_SLEEP` calls
turned out to be two different things wearing one syscall.  "Let the child
reach its blocking syscall" is a SCHEDULING request and became a yield; only
"fail instead of hanging if this never comes" was about time.  The kernel's
timed block had been standing in for a yield in most of its uses, which is the
kind of thing you only find out by taking it away.

**Gauges.**  T331 proves the service fires, that the authority is the endpoint,
that an arm without a notification is refused, and that all three numbers
answer NOT_SUPPORTED.  T310's subject moved from `SYS_SLEEP` to
`SYS_NOTIFY_WAIT` — the restartable-syscall claim is unchanged and is now made
about a mechanism that will still be here.  T150's hostile-pointer battery
moved to `SYS_NOTIFY_POLL` and `SYS_NOTIFY_WAIT`, the only kernel writers left.

**A bug this shook out.**  The timer service's first token encoding was the raw
slot index, so arming into slot 0 with generation 0 produced token 0 — which
the cleanup path reads as "nothing was armed" and used to delete the client's
notification out from under a timer that then fired into an empty slot.  The
first bounded wait in the suite hung, which is the correct amount of noticing
for a bug that silent.



### A-25 — revocation without a tail (CancelBadgedSends)

The second of A-20's "two smaller absences", and the last item that audit
found: *"seL4 has `seL4_CNode_CancelBadgedSends`, which cancels the IN-FLIGHT
sends of one badge after revoking a delegation; IRIS revokes the capability and
leaves whatever is already queued."*

**Why it is not small either.**  Revoking a badged capability stops a client
sending anything NEW.  It does nothing about what is already in the endpoint's
send queue — a message sent a moment before the revoke is delivered afterwards,
to a server that has just been told this client no longer exists.  A supervisor
that revokes and then assumes silence is wrong, and nothing in the system told
it so.  Revocation with a tail is not revocation.

`SYS_EP_CANCEL_BADGED_SENDS(ep, badge)` dequeues every waiting sender carrying
that badge and wakes it with CLOSED — which is what actually happened from the
sender's side: the endpoint stopped existing for it.  A staged capability is
released without consuming its source slot, the rule cancellation follows
everywhere else: nothing was delivered, so nothing is spent.  A blocked CALLER
is cancelled too; it is waiting for a reply its badge is no longer entitled to
ask for.

**The capability must be UNBADGED**, and that is the whole authority argument.
A badged capability names one client; cancelling by badge through it would let
that client silence any other by naming their number — the same reason a badge
can never be re-badged (A8).  Being able to say "everything from THAT client"
is a property of holding the endpoint itself.

**It returns a COUNT**, which is what makes it observable: a supervisor can
tell a revoke that had a tail from one that did not, instead of assuming.

**Gauge.**  T332, four claims — the count, that a different badge is untouched
(one client is silenced, not the endpoint), that a cancelled sender learns its
send did not happen rather than believing it was delivered, and that a badged
capability is refused.

**With this row, every item A-20's file-by-file audit found is closed.**  Three
authority holes (A-20), address-space identity (A-21), faults as IPC (A-22), the
bound notification (A-23), timed blocking (A-24), and this.  What remains from
that audit is the ABI SHAPE, which the charter has always carried as a
permanent deliberate divergence and now says so with the other three settled.



### A-26 — the review after the form divergences, file by file

A-20 read the system against seL4 and found six things.  All six are closed
(A-20 itself, A-21, A-22, A-23, A-24, A-25).  This is the re-read afterwards,
asking the same question of a system that has changed a great deal since: what
is still not seL4's, and what does the documentation now say that is no longer
true.

**The documentation was the biggest finding.**  The roadmap's "How close is
this to seL4" section contradicted its own status table.  It said D-1 — the
event-kernel rewrite — was one of "the two that no further stage closes", and
described converting to it as a rewrite of every blocking path; the table three
screens above said Stage 9-evt was CLOSED, which it was, several stages
earlier.  It also carried "roughly 75% on capability semantics, 25% on kernel
architecture" and a remaining-work list whose four items were all done.  A
roadmap's stalest paragraph is usually the one that was written most
confidently, and both of those were.

**Five things the code review found.**  None is a hole in the authority model;
all are recorded so the next reader does not have to find them again.

1. *IPC capability transfer is a MOVE.*  **— CLOSED by A-29, which also
   reversed the "permanent, deliberate" call this finding led to.**
   `syscall_ipc_stage_cap_commit` deletes
   the sender's source slot at the delivery point.  seL4 COPIES — the sender
   keeps its capability, gated by the Grant right.  Both are coherent, and
   IRIS's is strictly the more conservative of the two: nothing can be
   delegated without the delegator giving it up.  But it is a real difference
   in what an endpoint IS, it was never written down, and the way it surfaced
   is the reason it belongs here — the timer client (A-24) handed the service a
   notification and then could not signal its own object.  A caller that wants
   to keep what it sends derives a copy per send.

2. *`SYS_GETPID` is an ambient read.*  It hands a thread its own task id for
   the asking.  Information rather than authority, so A5 is not violated and
   the number confers nothing — but seL4 has no equivalent, in a capability
   system a task's identity is what OTHERS hold about it, and the only caller
   is the test that tests it.

3. *`SYS_THREAD_EXIT` duplicates `SYS_EXIT`.*  Two entry points that end a
   thread, one of which also records the exit code.  The duplicate is used only
   by test threads that have no code to record.

4. *`SYS_CLOCK_GET` is an ambient read of the clock.*  Every task can read the
   time holding nothing.  seL4 has no such call: a timer driver reads its own
   hardware, and everybody else asks the driver.  A-24 built the driver; the
   syscall it reads the time with is the last piece of that arrangement still
   in the kernel, and the timer service is its only productive caller.

5. *Four seL4 invocations have no equivalent.*
   `seL4_TCB_ReadRegisters`/`CopyRegisters` — a supervisor can WRITE a thread's
   registers (`SYS_TCB_WRITE_REGS`) and not read them, which is the asymmetry a
   debugger would notice first.  `seL4_SchedContext_YieldTo`/`Consumed`.
   `seL4_IRQHandler_Clear`.  And cross-CNode `seL4_CNode_Move`: IRIS moves
   within a CNode (`SYS_CNODE_SWAP` against an empty slot) and across CNodes
   only as mint-then-delete, which reaches the same place with a different
   derivation shape.

**And one cosmetic.** *(Closed: the file is `kfault.c`.)*
`kernel/new_core/src/kprocess.c` holds fault delivery
and its counters; `struct KProcess` was deleted in Stage 7-proc.  The file
name is the last thing in the tree still asserting that a process object
exists.

**What the review measured**, counted rather than recalled:

  - **62 live syscalls out of 94 dispatched numbers.**  The other 32 are
    retirement stubs answering NOT_SUPPORTED, which is how this system records
    that a number is SPENT rather than reusing it.
  - **11 retypeable object types** — Untyped, CNode, TCB, Endpoint,
    Notification, Reply, SchedContext, Frame, PageTable, VSpace, ASIDPool —
    every one of which is seL4's, and all 11 born only through
    `SYS_UNTYPED_RETYPE2`.
  - **4 more object types that are not retypeable**, and two of those are
    seL4's arrangement rather than a divergence: `KIrqCap` and `KIoPort` come
    from a budget through `SYS_CAP_CREATE_IRQCAP`/`IOPORT`, which is how seL4
    makes an IRQHandler (`IRQControl_Get`, not a retype).  The two that ARE
    IRIS's are `KInitrdEntry` and `KBootstrapCap`, which seL4 would express as
    capability types with no backing object.
  - **3 enumerators reserved and dead**: `KOBJ_PROCESS` (Stage 7-proc),
    `KOBJ_VMO` (D-5), `KOBJ_CHANNEL` (Phase 13).  No live capability carries
    any of them.
  - **299 runtime tests, 27 host suites, 27417 host assertions, 36 of 36
    charter invariants MET**, and the purity gate clean over the transitive
    closure with zero exemptions.

**The honest summary.**  IRIS has seL4's authority model, seL4's object model
and seL4's execution model.  It has its own ABI, its own rights set (D-3), a
refcount where seL4 walks the derivation tree (D-7), and a move where seL4
copies.  Each of those four is written down with the reasoning rather than the
conclusion.  It does not have the proof, and the proof is seL4's identity —
which is the one sentence about this system that no amount of further work
changes.



### A-27 — two ambient answers retired, and one that could not be

A-26's re-read found three syscalls that answered a question from NOTHING.
Two are gone; the third stayed, and the reason it stayed is the more useful
half of this row.

**`SYS_GETPID` — RETIRED.**  It handed a thread its own task id for the asking.
Not authority: the number selected no object and conferred no right.  But in a
capability system a task's identity is what OTHERS hold about it, and a call
that answers "who am I" from nothing is the shape A-18 spent its length
removing from the CSpace — only handing out a number instead of a capability.
seL4 has no equivalent, and nothing in the tree used it but the test that
tested it.

**`SYS_THREAD_EXIT` — RETIRED.**  It ended the calling thread, which is what
`SYS_EXIT` does — and `SYS_EXIT` also RECORDS the exit code, so the two were one
operation with one of them throwing information away.  It dated from when a
thread and a process were different things to end; since Stage 7-proc they are
not, and a thread that exits without saying why is a thread whose supervisor
learns nothing.  41 call sites in the suite moved to `SYS_EXIT`.

**`SYS_CLOCK_GET` — KEPT, and A-26 overstated the finding.**  It looked like the
last ungated read in the kernel: any task gets a monotonic timestamp holding
nothing, and seL4 has no such call because a timer driver reads its hardware
and everybody else asks the driver.  IRIS has that driver since A-24, so
retiring it looked free.

It is not gateable.  On x86 `rdtsc` is an UNPRIVILEGED instruction: any task
can read a monotonic counter with no capability and no syscall whatsoever.
Retiring the syscall would have moved the same ungated read into an instruction
and bought nothing — the appearance of a capability check over something the
hardware hands out for free.  What CAN be gated is WAITING — how long a thread
is kept off the CPU, and who decides — and A-24 gated it.  **Reading a counter
is not authority; blocking on one is.**

The timer service keeps `TMR_OP_UPTIME` anyway, because a client that was
granted a clock should be able to ask its owner rather than reaching around it,
and `sh` asks it that way now.  `IRIS_TICK_HZ` became an ABI fact shared by the
kernel that programs the PIT and the service that reads the line, where it had
been `pit_init(100)` in one file and `10000000` in another.

**And the attempt that failed, because it is the interesting part.**  The first
implementation had the timer service count the ticks it received instead of
reading a clock — a driver that receives every tick, the reasoning went, does
not need to be told the time.  It does.  A notification carries BITS, not a
count: ticks arriving while the service is not scheduled COALESCE into one
wake-up, so counting wake-ups is a clock that runs slow exactly when the system
is busy, which is when a deadline matters most.  The suite went from 6.3 s to
over 120 s and the cause was three layers down from the symptom.

**What the same change shook out.**  Converting the suite's thread exits, the
first pass matched `it_sys1(SYS_THREAD_EXIT, 0)` and missed
`it_sys0(SYS_THREAD_EXIT)` — 21 sites, all of them helper threads whose next
statement is `for (;;) {}`.  Those threads took NOT_SUPPORTED and span forever.
Nothing FAILED: every test still passed, and the suite simply got slower and
slower as each leaked spinner competed with the next test's yield loop.  It was
found by printing the live thread count next to each test's elapsed time and
watching it climb 10 → 16 and never come down.

That is the third time in this convergence that a defect surfaced as a
performance number rather than a failure, and the lesson is the same one A-21
and A-22 recorded: **a property nothing asserts is a property that degrades
silently.**  A thread that will not exit is not caught by any test that does not
count threads.



### A-28 — the five invocations seL4 has and IRIS could not express

A-26's re-read listed four operations with no equivalent here and called none
of them load-bearing.  That was true and it is the wrong test: an API gap only
hurts when somebody reaches for it, and nobody had.  Each of these is something
a supervisor should be able to SAY, and the fact that no code in the tree said
it is a statement about the tree, not about the gap.

**`SYS_TCB_READ_REGS` (139) — `seL4_TCB_ReadRegisters`.**  A supervisor could
point a thread anywhere it liked and never ask where it was.  A fault handler
gets the rip and the faulting address in the message (A-22); WHICH REGISTER held
the bad pointer was unreachable from ring 3 by any means at all.

Three things it gets right that a flag on the write would not.  `RIGHT_READ`,
because observing a thread is not changing it and a supervisor that may only
watch should be expressible — which is why seL4 makes this its own invocation.
`IRIS_ERR_BUSY` for a RUNNING thread, because since D-1 step 3 a running
thread's registers are in the CPU and the TCB holds whatever it looked like
when it last left a core; handing that back as current state is a lie a
debugger acts on.  And the same refusal for reading YOURSELF, where the frame
you would read is the one the syscall entered on.

**`SYS_CSPACE_MOVE` (140) — `seL4_CNode_Move`, across CSpaces.**  A capability
could move WITHIN a CNode (`SYS_CNODE_SWAP` against an empty slot) and not
between them.  Across CNodes the only route was mint-then-delete, and that is
not the same operation: a copy is a CHILD of its source, so for as long as the
two calls take, the derivation tree records a delegation that never happened —
and a revoke arriving in that window reaches something the mover meant to keep.

`kcnode_slot_move` has relocated MDB nodes since Phase S3, with host coverage
and no way for ring 3 to reach it.  The tree after a move is the tree before
with one slot renamed.  The BADGE travels, which is the sharper reason this
cannot be a mint: a badged capability can never be re-badged (A8), so a move is
the only way to relocate one and keep one history instead of two.

**`SYS_SC_CONSUMED` (141) — `seL4_SchedContext_Consumed`.**  MCS gave IRIS the
machinery to say what a scheduling context is OWED — the refill queue — and
nothing at all to say what it SPENT.  A temporal supervisor deciding whether a
server deserves its budget was holding the wrong half of the ledger.  The read
zeroes the counter, because "since I last looked" is the question and a
monotonic total only moves the subtraction into every caller.

**`SYS_SC_YIELD_TO` (142) — `seL4_SchedContext_YieldTo`.**  Give the rest of
this turn to the thread bound to a context.  Not donation: an endpoint Call
donates a scheduling context for the length of a request (T308) and this does
not — the caller keeps its budget and stops running first.  Bounded by the
caller's MCP for the reason every priority operation is: a thread that could not
RAISE another to a priority must not be able to schedule one already at it on
demand, or the ceiling is a number rather than a rule.

**`SYS_IRQ_CLEAR` (143) — `seL4_IRQHandler_Clear`.**  A route could be installed
and taken back only by DESTROYING the notification it pointed at, because the
binding is the notification's.  A driver handing a line on, or one that wants to
keep its notification for something else, had no way to say so.  Same authority
as installing a route, because taking one back is the same power over the same
line; and the line is masked on the way out, since a line with no route
delivers to nobody and an unmasked one would spin the kernel on an interrupt it
then drops.

**Gauge.**  T333, one claim per invocation, and the two that assert a REFUSAL
are the ones worth reading: a write-only capability cannot read registers, and
a task holding no IRQ capability cannot clear a route — which is this suite's
situation and therefore an assertion it can actually make.

**A note on the cost.**  T333 first spent eight rotating pool leaves on
rights-reduced copies and pushed T324's eviction count from 4 to 7 against a
ceiling of 6 — a new test spending a budget that exists to measure something
else.  It uses fixed root scratch slots now.  The gauge caught it on the first
run, which is what it is for.


## Charter amendments

The [purity charter](iris-sel4-purity-charter.md) may only be amended in a
change that cites it explicitly and records the amendment here.

### A-1 — invariant O1 pointed at a closed stage

**Change**: charter §2.2, invariant O1 — the pending-work reference
`(Stages 0/6)` becomes `(Stages 5/6)`.

**Justification**: Stage 0 (TCB consolidation) closed in Phase S2 inc.2, so O1
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
Precedent: Phase S3 grew the same list by 3 under this clause.

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


### A-3 — A1, A5 and O1 restated for the ASID capability model

**Change**: charter §2.1 A1 and A5, and §2.2 O1.  A1 gains the two operations
that did not require a capability until A-20 and A-21 (configuring a budget,
naming an address space) and now do.  A5 records that the kernel-global PCID
bitmap was an ambient RESOURCE and is gone.  O1 gains the eleventh canonical
type, `ASIDPool`, and extends "born from Untyped" to cover an address space's
NAME as well as its storage.

**Justification**: ledger A-21.  A1 read "MET" flatly while `SYS_SC_CONFIGURE`
took no authority and every retyped VSpace was named for free by the kernel;
both are now capability-gated and refused without the grant, so the invariant's
"Today" column has to say which operations were the last to get there and what
they take.  A5's list of ambient authorities was complete for SYSCALLS and
silent about RESOURCES — a namespace the kernel hands out to anyone who asks is
ambient authority whether or not a syscall names it.

**Scope**: three "Today" cells restated to match shipped mechanism.  No
invariant changes state — A1, A5 and O1 were all MET and all remain MET — no
allowlist entry moves, no prohibition is added or lifted.



### A-4 — A9 and I-invariants restated for fault IPC

**Change**: charter §2.1 A9 — the FAULT DELIVERY entry in the LEGACY_ROOT
history is restated: the class is not merely fixed but GONE, because a fault no
longer delivers a capability at all.

**Justification**: ledger A-22.  A9's "Today" column described fault delivery
as a legacy-root producer that had been fixed by parenting the published
capability to the registrant's slot.  There is nothing to parent: the badge
identifies the faulting client and the reply capability authorises resuming it,
so no capability is published on a fault.  Leaving the old wording would credit
a fix for a mechanism that no longer exists.

**Scope**: one "Today" cell restated to match shipped mechanism.  A9 was MET
and remains MET; no allowlist entry moves, no prohibition is added or lifted.



### A-5 — P1 and P2 restated for retired timed blocking

**Change**: charter §2.6 P1/P2 (mechanism, not policy) — the "Today" columns
gain the timed-blocking retirement: the kernel no longer holds a deadline on
any thread's behalf, and waiting is a capability to a service.

**Justification**: ledger A-24.  P2 says the kernel decides no policy that a
holder could decide.  Three syscalls decided how long a thread may wait, whose
waiting is worth a kernel data structure, and what happens to a deadline nobody
wants — all of them policy, all of them now in a ring-3 service a task either
holds a capability to or does not.

**Scope**: two "Today" cells restated to match shipped mechanism.  No invariant
changes state, no allowlist entry moves, no prohibition is added or lifted.



### A-6 — the ABI-shape divergence restated as the last one open

**Change**: charter §4, the "Own ABI (not seL4)" row — restated to say that it
is the last of the four form divergences A-20's audit named and the only one
still open, and to give the actual trade rather than only the fact.

**Justification**: ledger A-21, A-22, A-24 closed the other three.  The row
described a shape difference in the abstract; with its three neighbours settled
it is the whole of what "IRIS is not seL4 in form" now means, and a charter that
carries one permanent divergence should say what it buys and what it costs
rather than only that it exists.

**Scope**: one divergence row restated.  Nothing changes state; the divergence
was permanent and deliberate before and remains so.



### A-7 — A9, A10 and a divergence row, restated after the review

**Change**: charter §2.1 A9 (the LEGACY_ROOT count, 43 → 23 becomes 43 → 25),
A10 (gains `CancelBadgedSends` and what it covers that the MET claim did not),
and §4 gains one divergence row: IPC capability transfer is a MOVE.

**Justification**: ledger A-25 and A-26.  The A9 figure was measured before
`SchedControl` (A-20) and `ASIDControl` (A-21) each added one permanent
boot-path root, so the number in the charter was two behind the number T305
prints every run.  A10 claimed revocation was complete while a revoked client's
queued sends were still delivered afterwards.  And the transfer-is-a-move
difference had been the behaviour since Stage 2 without being written anywhere
— found by A-26's re-read, and by the timer client running into it.

**Scope**: two "Today" cells restated to match measured mechanism, one
divergence recorded that already existed.  No invariant changes state — A9 and
A10 were MET and remain MET — no allowlist entry moves, no prohibition is added
or lifted.


## A-29 — the tree was right and the operation was wrong

**Before**: sending a capability over an endpoint EMPTIED the sender's slot.
`syscall_ipc_stage_cap_commit` called `kcnode_slot_delete` on the source at the
delivery point, on all four transfer paths (`EP_SEND`, `EP_NB_SEND`, `EP_CALL`,
`SYS_REPLY`).  A-26 found this, wrote it down, and A-7 registered it in the
charter as a permanent, deliberate divergence: seL4 COPIES, IRIS moves, both
are coherent and IRIS's is strictly the more conservative of the two.

**After**: transfer is a COPY.  The sender keeps its capability whether the
message landed or not, and the receiver's capability is a derivation CHILD of
the sender's slot.  A sender that means to give a capability away derives a
copy, sends it, and deletes its own slot — two steps that both belong to it.

**Why the "permanent, deliberate" call was wrong.**  Not because seL4 does it
differently.  Because the kernel already disagreed with itself.
`syscall_ipc_deliver_cap_routed` installs the delivered capability with
`kcnode_slot_install_linked(..., src_cn, src_idx, ...)` — linked to the
sender's slot, an MDB parent-child edge, which is a statement that only means
something if the parent continues to exist.  The delivery recorded that
relationship and the commit immediately deleted the parent, reparenting the
child onto whatever happened to be above it.  The move and the ancestry were
two different theories of the same operation, shipped together.  One of them
had to go, and it was not going to be the tree: the ancestry is what makes an
IPC delegation revocable from the delegator, which is the property the whole
CDT exists to provide.

The conservatism argument does not survive either.  "Nothing can be delegated
without the delegator giving it up" was never enforced — a sender derives a
copy first and gives THAT up, which is what every caller in the system already
did.  What the move actually bought was one less `CNode_Delete` in clients that
wanted move semantics, at the cost of a lie in the derivation tree.

**Scope**:
- `syscall_ipc_stage_cap_commit` and `syscall_ipc_stage_cap_abort` collapse
  into one `syscall_ipc_stage_cap_release(src_cn)`.  They had become identical
  bodies, and keeping two names for them would have preserved a distinction the
  kernel no longer makes: there is no longer anything for a delivered exit and
  a non-delivered exit to disagree about.  Nine call sites lose their
  `if (new_h != IRIS_MSG_NO_CAP)` branch.
- The DELIVER-first ordering rule (Phase S4 Step 2) is unchanged and is now the
  whole story rather than half of it: parenting needs the source slot occupied
  at delivery time, and it stays occupied afterwards.
- `services/common/iris_timer.h`, `services/svcmgr/svcmgr.c` and
  `services/init/init_launch.c` are the productive callers.  svcmgr now deletes
  its lookup scratch slot after replying; the timer client documents the
  send-then-delete pair; init keeps its slot deliberately, because holding the
  parent of a grant is how it could revoke the timer's reach.
- Charter §6 loses the divergence row (retired, not restated).

**What it cost to find, and what that says.**  Thirteen tests failed on the
first build, and not one of them was asserting move semantics on purpose.  Two
said "dup not consumed" — a genuine assertion of the old rule, inverted here.
The other eleven were arithmetic: object counts that no longer balanced because
senders kept what they used to lose, and a rotating slot pool that began
evicting live capabilities mid-test.  A behaviour with two direct assertions in
a 301-test suite, and both of them written as an afterthought inside a fuzzer.

**T334 is the test that would have caught it.**  Three claims, each of which
fails under a different mutation of the kernel: the sender still holds what it
sent (fails if the commit deletes the slot — verified), the receiver's
capability is a child of the sender's slot (fails if the delivery installs a
LEGACY_ROOT instead — verified), and deleting the sender's slot leaves the
receiver's copy alive while revoking it does not.  That last pair is the
difference between delegation and disposal, and nothing in the suite had ever
asked for it.

**This is the third defect in this convergence that surfaced as something other
than a failure** — after the timer token that read as a hang and the retired
`SYS_THREAD_EXIT` that read as a slowdown.  The pattern is now explicit enough
to state as a rule: *when a behaviour is chosen deliberately, the same commit
adds the test that fails if it is chosen differently.*  A divergence row in a
charter is documentation; a test is the only thing that keeps the row true.


## A-30 — a wrong type, answered as a wrong type

**Before**: twenty-two resolver results were rewritten on their way out of the
syscall layer.  Sixteen `WRONG_TYPE → INVALID_ARG`, three
`WRONG_TYPE → ACCESS_DENIED`, and three ternaries that mapped `WRONG_TYPE` to
itself — dead code, and the clearest evidence of what this was: a conversion
done three separate times and finished none of them.

**After**: the resolvers' answer travels.  One rule, both halves of it:

> a capability of the WRONG TYPE is `WRONG_TYPE`;
> a capability of the RIGHT type without the authority is `ACCESS_DENIED`.

**Why it was wrong, and it is not the seL4 comparison.**  Three things.

*It was already the position here, applied piecemeal.*  A-20 fixed the typed
resolvers to check type before rights.  D-5 fixed `dev_cap_budget`, with the
comment "a caller that named an endpoint where a budget goes is told so".  The
TCB family fixed `tcb_resolve` at Step 4, and its comment counted itself as the
"third instance of the same defect".  Three fixes, three write-ups, and the
other twenty-two sites untouched — because each time the fix was made where
somebody was looking rather than where the pattern was.

*It was inconsistent inside a single syscall.*  `SYS_TCB_SET_IPC_BUFFER`
answered `WRONG_TYPE` for a non-TCB in arg0 and `INVALID_ARG` for a non-frame
in arg1.  One call, one kind of caller mistake, two different answers,
asserted both ways in T313.

*It protected nothing.*  The confidentiality argument for flattening is that
naming a type discloses something.  It does not: `SYS_CAP_IDENTIFY` takes
`RIGHT_NONE`, costs no capability, and reports the type of any slot the caller
holds — and every one of these twenty-two resolutions runs against the
CALLER'S OWN CSpace, never a foreign one.  So "that is a notification, not a
frame" tells a caller something it can read for itself in one syscall, while
`INVALID_ARG` told it strictly less than the kernel knew and left it unable to
tell a malformed number from a well-formed capability of the wrong kind.  Two
different bugs with two different fixes, given one answer.

**The three `ACCESS_DENIED` sites deserved their own look**, because there the
flattening had a real argument: arg0 is an authority (`KOBJ_BOOTSTRAP_CAP`),
and the next line answers `ACCESS_DENIED` when it IS a bootstrap capability of
the wrong flavour.  Making both answers the same hides which one happened.  But
that argument gives the caller less than `CAP_IDENTIFY` already does, and it
costs the distinction that matters: presenting a notification where an
authority belongs is not a failed authority check — the check never ran.  The
flavour stays hidden either way, and that is the part that is actually secret.

**Scope**: 22 sites across `syscall_cspace.c`, `syscall_cnode_ops.c`,
`syscall_vm.c`, `syscall_tcb.c`, `syscall_untyped.c`, `syscall_sched.c`.  No
resolver changes; no rights check changes; no authority moves.  Eight runtime
assertions and two host assertions changed the code they expect — every one of
them a test that had written down the flattening as though it were the
contract, which is how a convention becomes an invariant nobody chose.

**T335 pins it**, both halves: fifteen wrong-type invocations across the six
families that answer `WRONG_TYPE`, and the boundary that keeps the rule from
degenerating into "say `WRONG_TYPE` more often" — a read-only SchedContext
still answers `ACCESS_DENIED` to `SC_CONFIGURE`, and a real framebuffer-control
capability still answers `ACCESS_DENIED` to `SYS_INITRD_COUNT`.  It ends by
asking `SYS_CAP_IDENTIFY` for the type it was just told, because that is the
whole argument in one line.


## A-31 — the suite gets an interface to itself

**Before**: `services/iris_test/main.c`, 26,572 lines, one translation unit,
802 `static` declarations, 303 tests.  Every helper, every piece of per-test
state and every worker-thread entry point in one namespace, in the order they
were written.

**After**: twelve files.  `it_priv.h` is the interface; `it_base.c` holds what
every test is built out of (serial, accounting, the slot helpers, the rotating
object pool, object fabrication, the bounded wait that asks the timer service);
`main.c` is the entry point and the running order; and the tests are in ten
files of ~2,400 lines each, named for the range of test numbers they hold, so a
`[IRIS][TEST] T157 FAIL` line names its own file.

**How, and why it is checkable.**  The split was done by a program, not by
hand, and the program asserts the thing that matters: **every line of the
original file lands in exactly one output file.**  26,572 in, 26,541 out, 31
dropped — and the 31 are forward declarations of functions the header now
declares, which is the only category allowed to disappear.  A hand split of a
26,000-line file is a diff nobody can read; a mechanical one with a
conservation law is a diff nobody has to.

**What is exported, and what is not.**  Only symbols actually referenced
outside the file that defines them: 465 declarations out of 802 statics.  The
other 337 stayed `static`, which is the part of this that is worth something —
per-test state that used to be visible to 302 other tests is now visible to the
handful in its own file.  Three cases the analysis had to get right, each found
by the compiler rather than guessed at:

- a symbol named only by a MACRO in the shared header (`#define IT_UT
  (it_auth_ut())`) is reachable from every file, so a mention from the header
  counts as a mention from everywhere;
- `extern const long x[];` is an incomplete type and `IT_FZ_BAD_H_N` takes its
  `sizeof`, so an unsized array's bound is counted from its initializer and put
  back;
- a `;`-terminated chunk is a forward declaration when it is a function and a
  DEFINITION when it is a variable — dropping the second kind deletes the
  object.

**The boundaries are by line count, not by test count.**  T083–T119 is 37 tests
and 4,000 lines where T001–T044 is 37 tests and 1,500; what makes a file hard
to read is its length.  Boundaries are then nudged until adjacent files' number
ranges stop overlapping, which works everywhere except one file: T295, T296,
T319 and T324 sit physically at the end of the suite, hundreds of tests after
their numeric neighbours, because that is where they were written.  That file's
name overlaps its neighbours' and its header says so.

**What this does not do.**  It does not rename a single test, change a single
assertion, or reorder anything: the running order in `main.c` is the order it
was, and the suite passes 303/303 before and after.  Narrowing `it_priv.h` from
465 declarations to the ones that deserve to be an interface is a separate
change — one that can now be made a piece at a time, because there is an
interface to narrow.


## A-32 — a method you cannot name without naming what it acts on

**Before**: 93 syscall numbers dispatched, 62 of them live.  A number selected
a method; the capability it acted on was its first argument, like any other
argument.  Charter §6 recorded this as "Own ABI (not seL4)" — permanent and
deliberate, on the grounds that the authority semantics were equivalent and
that a conversion would rewrite every caller in the system to gain nothing the
charter measures.

**After**: one invocation door.  `SYS_INVOKE(cptr, label, a1, a2, a3)` names a
capability and a method, and the method cannot be named without it.  Three
numbers survive — `SYS_EXIT`, `SYS_YIELD`, `SYS_CLOCK_GET` — each because it
invokes nothing, which is the same reason seL4 keeps `seL4_Yield`.

**What the conversion was actually worth.**  The charter row was right that the
authority semantics do not change: nothing was reachable without a capability
before and nothing is now, and not one of the 36 invariants moved.  What it
undervalued is the difference between a property that holds and a property the
shape enforces.  Under numbers, "a syscall selects a method and never an
object" was a rule the kernel obeyed and nothing checked.  It is now the only
thing the ABI can express.

**The five stages, and what each cost.**

*A — the door.*  A second entry point, resolving the capability to learn its
type and dispatching on (type, label), with every number still working.  The
syscall entry grew a fifth argument register: an invocation needs a capability,
a label and three method arguments where the numbered ABI needed four
arguments, and three arguments is what `Untyped_Retype`, `TCB_Configure` and
`Frame_Map` each take.  `r8` was the scratch holding the user stack pointer for
three instructions after SWAPGS; it waits in a per-CPU slot now and is pushed
straight out of memory, so it occupies no register at all.

*B — the labels went flat, and the plan was wrong twice.*  The first cut scoped
labels to the invoked type, so `TCB_Suspend` and `EP_Send` were both 1.  That
is not seL4's arrangement — `enum invocation_label` is one flat list — and it
is not free: a scoped space forces the door to learn the type before it can
pick the method, which is a CSpace walk the method then repeats.  Two walks per
invocation, forever, for a disambiguation seL4 does not need.

The plan said the repeat would go the other way: hoist the resolve out of sixty
function bodies and hand each the object it needs.  **The code refused, for an
architectural reason.**  Under the event kernel (D-1), WHEN a method resolves is
part of its contract.  `sys_notify_wait` checks for a notification that closed
under it BEFORE resolving, because the close is usually the last capability
going away and a resolve would report NOT_FOUND for something that actually
closed.  `sys_tcb_suspend` returns on re-entry before resolving, because a
restarted suspend that resolved and suspended again would put the thread back
to sleep the instant it was resumed.  Six of the seven methods that act before
resolving are the blocking ones — `ep_send`, `ep_recv`, `ep_call`,
`reply_recv` among them.  Hoisting would have broken exactly the hot path.  It
is refused rather than scheduled, and this is the record of why.

So the type is checked where it always was: inside the method, by the resolver
that asks for what it needs.  A label sent to the wrong kind of capability
answers `IRIS_ERR_WRONG_TYPE`, which names what is wrong — a better answer than
seL4's `IllegalOperation`, and one the kernel only became able to give
consistently at A-30, three commits earlier.

*C — ring 3.*  Every service, the loader, the shared headers and the assembly
driver.  `iris_vspace`'s map fixup took a syscall number and dispatched on it;
it takes a label.  `iris_ipc_buffer` and `svc_loader` dropped the arity wrappers
they carried only so they could name numbers.  kbd is where the change is
visible as what it is: a syscall was `rax=number, rdi=capability`, an invocation
is `rax=SYS_INVOKE, rdi=capability, rsi=method`, so every site shifts one
register and `EP_RECV`'s reply object moves into the fifth.  That also forced
the labels out of an enum and into defines — a label is ABI, and the assembler
has to read it.

*D — the suite.*  1,139 call sites across eleven files.

*E — the door closed.*  The switch and thirty-two functions whose entire body
was a refusal: the residue of retirements that kept a stub so the number would
answer NOT_SUPPORTED.  A number that names nothing is refused by the dispatcher
having no case for it.  737 lines deleted, 28 added.

**The instrument, and the three times it earned its place.**  The migration
fails silently by construction — a caller that is never converted keeps
working, and every test passes.  D-4's IPC-buffer migration had exactly this
shape and the first service tried was quietly not migrated.  So a counter of
calls that still named a method by number went in with the first commit, and
`SYS_UNTYPED_QUERY` reports it.

It read **438,901**, which was not a stalled migration: it was `SYS_YIELD`,
which the settle loops spin on and which is never going anywhere.  A number
that cannot reach zero is not a progress bar, so the three permanent syscalls
stopped being counted.  It read **399**, and the rest was T148 fuzzing every
hole in the table on purpose; a number that names nothing reaches no method, so
the default branch takes its increment back.  It read **59**, which was the
retirement assertions and T337's own probes.  It reads **0**.

**The one caller the mechanical pass missed**, and how.  lifecycle_probe chose
between `EP_CALL` and `EP_SEND` with a ternary, so the first argument was not a
literal and the converter could not see it.  Closing the door turned it into
NOT_SUPPORTED, the child never blocked, and T113 hung.  It hung rather than
failing quietly, which is the only reason it cost minutes — and the gauge would
have said so too, if it had been read before the door was closed rather than
after.

**What did NOT change, and is worth saying because the row claimed it would.**
Not one of the 36 invariants moved.  No authority is reachable that was not, and
none is unreachable that was.  Every method resolves its own capability and
checks its own rights, exactly as it did when a number selected it.

**Two divergences from seL4 that this created or kept, recorded rather than
rounded away.**

1. *IRIS folds seL4's IPC syscalls into the invocation door too.*  seL4 keeps
   `Send`, `Recv`, `Call`, `Reply`, `ReplyRecv`, `NBSend` and `NBRecv` as real
   syscalls, because `msgInfo`'s label is application data and the syscall
   number is what says which IPC verb was meant.  IRIS's message carries its own
   label, so the number is not needed for that, and `EP_Send` is a method like
   any other.  One entry point rather than eight.  More uniform than seL4, and
   different from it either way.
2. *The slot methods hang off the slot.*  `Mint`, `Move`, `Revoke`,
   `SetGuard`, `Identify` and `SameObject` act on a slot rather than on the
   object in it.  seL4 expresses them as CNode invocations, with the CNode as
   the object and (index, depth) as arguments; IRIS invokes them on the slot
   directly.  A difference about which object a method hangs off, not about
   whether a method needs one.

**Still open, named rather than dropped: `struct IrisMsg` is the message ABI.**
A message is an 80-byte struct in user memory named by a pointer, where seL4's
is a `MessageInfo` word plus message registers plus the IPC buffer.  It is a
divergence about how a MESSAGE is carried, not about how a METHOD is named, and
the charter records the buffer half of it separately as D-4 (closed at Stage
8-cap; every service that sends a bulk payload is migrated).  Retiring the
struct is 339 ring-3 sites and 130 kernel references — the same size as this
whole conversion — and it buys nothing this charter measures beyond what D-4
already bought.  **Not scheduled, and that is a decision rather than an
oversight.**


## A-33 — a message is registers

**Before**: `struct IrisMsg` was the message ABI.  Eighty bytes in user memory,
named by a pointer; the kernel validated the range and copied the struct in,
and on a receive copied it back out.  A-32 recorded it as a permanent
divergence with the numbers attached — 339 ring-3 sites, 130 kernel references,
"the same size as the whole invocation conversion, and it buys nothing this
charter measures beyond what D-4 already bought."

**After**: a message is a MessageInfo word and message registers, and anything
longer lives in the page the sending thread registered.  `struct IrisMsg` does
not exist; what survives is `struct ipc_stage`, the place a message waits
INSIDE the kernel between a sender being queued and a receiver taking it, which
is what it always really was.

**What the row got wrong, which is the same thing A-32's row got wrong.**  It
priced the work correctly and the gain not at all.  Three things change, and
none of them is tidiness:

1. *There is no address on the message path.*  Nothing to validate, nothing for
   a second thread to unmap between the check and the copy.  `user_range_readable`
   is gone from every send path in the kernel, and the host suite's IP-2 —
   "the message pointer is required" — is retired because its subject is gone.
2. *A short message never touches memory* at either end.  Two words used to
   cost an eighty-byte copy each way.
3. *The shape of a message is a register map, not a struct layout.*  `kbd` is a
   driver written in assembly and it used to know field offsets; it reads a
   message the same way C does now, which is why `iris/ipc_msg.h` is shared by
   the kernel, the C services and the assembler.

**`buf_uptr` is deleted rather than carried.**  D-4 had already made it
vestigial: with a registered buffer it could only hold one value and the kernel
refused every other, which is what turned a boot's worth of silently corrupted
console output into an error at the call site that caused it.  A-33 removes the
question.  A message carries a LENGTH; the bytes are in the page the thread
registered, because there is nowhere else they could be.

**The register map**, and why it is shaped this way.  The entry carries nine
user words and the exit hands seven back.  `r15`, `r14` and `r13` cost nothing
to take: the event kernel has pushed them on every entry since it started
saving the full user context (abandoning a frame throws away the spills a C-ABI
kernel would rely on), so carrying them as arguments is the same three stores
with a different name.  seL4 uses `r15` as a message register on x86-64 for
exactly this reason.  A receive returns in the SAME registers the message
registers went out in, which is seL4's arrangement and is what makes a server's
reply loop free of shuffling.

**Two things the conversion had to DECIDE rather than translate.**

*A receiver must be able to tell whether a capability arrived.*  seL4 answers
with `extraCaps`, a count.  IRIS answers with the RIGHTS the capability landed
with, and zero is unambiguous because a capability with no rights cannot be
transferred at all — the staging refuses `RIGHT_NONE`.  That is more than seL4
reports, and the reason is worth stating: a receiver that has to ask a second
time about a capability it was just handed is a receiver that can be told a
different answer in between.

*A receive of a Call is handed TWO capabilities* — the caller's gift and the
reply object it is now owed.  They used to share `attached_handle`, told apart
by which half of the conversation was looking at it.  The gift lands in the
slot the receiver DECLARED, which it knows without being told; `got_cap` is the
reply object.  One field per question.

**Six defects, each of which passed a build.**  They are listed because the
shape of them is the lesson, not because the list is interesting:

- *A register variable must not live across a function call.*  `iris_mi` and
  `iris_capw` were not inlined, so the syscall left carrying whatever those
  calls had spilled into r10, r8 and r9.  Every wrapper computes into ordinary
  locals now and assigns the register variables last.
- *`sys_ep_recv` still validated `arg1` as a user pointer* after `arg1` became
  the receive slot.  Every receive answered INVALID_ARG and no server ever
  received anything.
- *`ipc_msg_store_call` was inverted.*  It gave a Call's completion the
  capability from `attached_cap`, which is where a SERVER finds a caller's
  gift; a CLIENT finds what the reply transferred in `attached_handle`.  It
  returned NO_CAP for every lookup in the system — and a lookup that gets no
  capability RETRIES, so the symptom was a receive slot that was already full
  rather than a capability that was missing.
- *`ReplyRecv` took its receive slot from the wrong word.*  It is a send
  followed by a receive, so its words are laid out like a send's — and a plain
  receive's slot occupies the same argument index as a send's MessageInfo.
- *A failed receive was unpacked anyway.*  The kernel writes no return words on
  an error path, so the wrapper handed back the arguments it had sent: a
  refused receive came back carrying its own MessageInfo and looking like a
  delivered capability.
- *`kbd`'s object had no header dependency in the Makefile.*  A MessageInfo
  layout change left a stale assembly driver in the build, and six tests with
  nothing to do with kbd failed — reply-object accounting, because a driver
  replying with a malformed MessageInfo leaks reply objects system-wide.

And two in the assembly written for this row: the exit dropped the alignment
pad before reading the frame, so `sysretq` took RFLAGS for RIP; and the abandon
path used `r9` as its CR3 scratch, which A-33 had just made the pointer to the
return message — it overwrote it with a page-table root and dereferenced that.

**What a payload now requires, said plainly.**  Both ends must own a registered
IPC buffer, because a payload lives in the sender's and is copied to the
receiver's.  Two threads of one process each need their own; T022 found this
the hard way, its server having written into the page the MAIN thread
registered.  The old ABI told a receiver where its buffer was, in `buf_uptr`.
A thread that registered one already knows.

**T338 pins it**, and each claim fails under its own mutation of the kernel:
the MessageInfo round-trips and its fields do not bleed into each other at
their limits; a hostile address in a message word is a WORD and is not refused,
because nothing dereferences it; a delivered capability's rights come back, and
zero comes back when nothing was delivered; and a failed receive delivers no
message at all.


## A-34 — the five the audit named

**Before**: the file-by-file audit (`sel4-purity-audit.md`) ended by saying the
remaining distance to seL4 was work not done rather than shape got wrong, and
listed what that work was.  Five items were small: four generic invocations
IRIS did not answer, and the domain scheduler.

**After**: all five exist, each with a runtime test.  What each is FOR, since
"seL4 has it" is not a reason to add anything:

- **`Frame_GetAddress`** — a holder programming a device needs the PHYSICAL
  address of the memory it points that device at, and nothing else can tell
  it.  Without this a driver is handed its address out of band by whoever
  retyped the frame: a fact travelling outside the capability that carries the
  authority.  `RIGHT_READ` gates it, because "which physical page is this" is
  the question that turns an opaque capability into something correlatable.

- **`TCB_SetMCPriority`** — A-20 made the MCP a ceiling on what a thread may
  grant and shipped with it only INHERITABLE, which cannot express LOWERING
  one.  A supervisor wanting to hand a subtree less authority than it holds
  had to have been configured with less, deciding the whole hierarchy before
  building any of it.  A running priority above the new ceiling comes down
  with it: otherwise the ceiling would bind future grants only, and the thread
  would keep running at an authority just taken away.

- **`PageTable_Unmap`** — `PageTable_Map` had no counterpart, so rearranging
  an address space meant destroying it.  It REFUSES while the subtree is live,
  which is a DELIBERATE difference: seL4 unmaps the table and invalidates the
  mappings under it, IRIS answers BUSY because a detached level whose PTEs the
  VSpace still describes leaves the bookkeeping asserting mappings the hardware
  cannot reach.  Same rule `Untyped_Reset` has.  It also pays the debt
  `paging_detach_table_in` recorded against itself: an interior entry removed
  from a LIVE walk owes a paging-structure flush, and one INVLPG is enough
  precisely because the unmap refuses a non-empty subtree.

- **`CSpace_Rotate`** — three slots, two moves, one critical section.  The
  two-call version needs a FOURTH slot to park the displaced capability in,
  and a CSpace full enough to need rearranging is the one without a spare;
  between the calls the capability is also somewhere neither the holder nor a
  revoke expects.  `dest == src` is the swap, the case that cannot be done
  with relocations because both slots are occupied.  BADGES TRAVEL rather than
  being arguments — seL4 takes a new badge per destination, and charter A8
  says a badged capability is never re-badged, so such an argument would exist
  to be rejected.

- **Scheduling domains** — the top-level time partition, and a new charter
  invariant (S6).  A domain is not a priority: priority orders threads that
  COMPETE and leaks through when they get to run, while a partition does not,
  because the boundary is a SCHEDULE rather than a comparison.  Run queues are
  per (domain, priority), so dispatch never reads another domain's threads —
  which is why it stays O(1) and why the cost of running a domain does not
  depend on what the others hold.  The schedule is FIXED as seL4's is: one
  somebody can influence is one that carries information.  `Domain_Set` takes
  its own boot authority, because ordering a thread within the time you were
  given and moving it into somebody else's are different questions.

**What implementing them turned up**, which is the part worth keeping:

`root_bootinfo_set_control_cap` had no case for a new authority kind, so it
answered INVALID_ARG, the boot path turned that into FATAL, and the root task
was aborted before the scheduler ran.  Its host test had never covered
SchedControl or ASIDControl either — all three are covered now, so the next
authority is caught at build time rather than by a silent boot.

`iris_test` receives the domain authority at a different slot from every other
task.  97 is free everywhere except the suite, where 88..97 is the fixed
reply-object range and T113 deletes 97 on its way out — so the capability was
delivered, survived most of the run, and was gone by the time a late test
looked.  The suite's own documented free slot, 99, turned out to be
`IRIS_CPTR_FB_CONTROL`, which init also mints, so the exclusive mint refused
and the child started without the authority.

**That second one is the part that got fixed rather than worked around.**  The
loader's pre-start mints were non-fatal AND silent, on the stated argument that
"consumers gate loudly in smoke" — which holds only for capabilities some
marker happens to cover.  The domain authority had none, so a refused mint
produced a child missing an authority with nothing anywhere saying which, and
the first symptom was a test three hundred cases later getting ACCESS_DENIED.

Now: `struct svc_mint` carries a `result` the loader writes for every entry,
`init` prints any failure with the destination slot, and the headless gate
fails the build on that line — BEFORE it checks the suite result, because a
missing capability is the cause and a failing suite is the symptom.  A
collision is a build failure that names the slot.

The comment listing the suite's free slots is gone with it.  It was derived by
enumerating what the suite NAMES, and 99 is something the suite is GIVEN —
different lists, and the enumeration could only ever check one of them.  A
mechanical check replaced it.

**Pinned by**: T340 (GetAddress: answers, survives a map, refused without
READ, WRONG_TYPE on a notification), T341 (Unmap: comes out, capability
reusable, BUSY with a live mapping under it and the mapping still works after
the refusal, NOT_FOUND when not installed here), T342 (Rotate: the rotation,
the swap, occupied dest refused whole, empty src/pivot NOT_FOUND, and the
derivation tree travelling — revoke after two rotations still reaches the
child), T343 (Domains: a thread in an unscheduled domain stops completely and
resumes when moved back; a domain outside the set and a non-domain capability
are both refused).

**T305's LEGACY_ROOT ceiling: 25 → 26.**  DomainControl is a boot authority
like SchedControl (A-20) and ASIDControl (A-21) — minted once, in BootInfo,
unparented because nothing is above it to be a child of, every delegation
downward a child.  The note at the ceiling now states what makes those three
acceptable and why a root anywhere else is not.

**What is left after this**: formal verification, and the platform work of
Stage 10.  SMP and the IOMMU were the other two on this list and are built.
None of the three was shape IRIS got wrong.

**SMP is done** (roadmap §9.1–§9.3, all five steps).  Four processors dispatch
threads on a four-CPU machine and the full suite passes there and on one.  What
it cost is the entry worth keeping, because the pattern will repeat for the
IOMMU: **not one of the six kernel defects was in the code written for SMP.**
Every one was an existing correctness argument with "there is one processor"
inside it, unstated — a per-CPU value stored globally (`current_task`, and the
syscall path's shadow copies of the kernel stack and the user CR3), a
link-time constant where a per-CPU read belonged (`&cpu_local[0]` written into
`KERNEL_GS_BASE` by both ring-3 entry paths), a register the boot processor
configured and an arriving one never did (CR4: SSE, PCIDE, SMEP, SMAP), an
ownership handover with no flag to mark it (`task->on_cpu`), an operation that
returned success without doing anything to the core concerned (`Suspend` and
the external kill), and a list maintained only in order to be maintained
(`task_list_head`'s ring — deleted rather than locked).

Three more were in the TEST SUITE, and they are a different kind of mistake
worth separating: every one was a wait that had quietly stopped waiting.
`it_settle`, `it_quiesce_reaper` and `it_fault_wait_ep` were bounded in yields,
which is a real wait only while every yield is a dispatch that hands the CPU to
the thread being waited for.  Two assertions read a GLOBAL counter to make a
claim about the CALLING thread.  **T346** pins what step 4 actually delivers —
`online=N dispatching=N`, which step 3 could have satisfied with
`online=4 dispatching=1`.

**And the adversarial phase found four more** (§9.3 step 5), which is the part
worth keeping because it changes what "the suite passes on four processors"
was worth.  It was worth less than it sounded: the suite's threads happened to
be spread across cores, so it exercised whatever interleavings fell out.  Four
tests that AIM four processors at one object found, in one afternoon:

- a RETYPE rollback that un-bumped a carve window read with two separate lock
  holds — on four cores that window covers another core's allocation, so the
  rollback handed a live block back to the allocator and two cores built
  objects in one block;
- a release-then-use: `sys_tcb_exit` dropped the resolve's reference on the
  line above the call that dereferenced the pointer;
- a teardown gate that was not a gate: `task->terminal` was a plain byte tested
  by an unlocked read, so four cores calling Exit on one thread all entered its
  teardown and released one reference four times;
- a dispatch that overwrote a `Suspend`: a thread already dequeued is in no
  queue, so the suspend could not take it out of one, and the dispatcher then
  marked it RUNNING.  The caller was told its thread had stopped while it went
  on running — an API that lies, which is the same class as the `Suspend` that
  did not stall (step 4) one layer down.  **The fix's first version was too
  broad and is worth recording as such**: it dropped any dispatch choice that
  was not READY, on the reasoning that a queued thread is a runnable thread.
  That reasoning is false here — a thread is put in a queue and its state
  written by two different pieces of code, so one can legitimately be queued
  while BLOCKED_REPLY — and dropping a single such thread wedged the system.
  A rule that is true of a kernel in general is not automatically true of
  THIS kernel.

Three of the four presented identically — a refcount assert on an object whose `type`
field read 0, a type nothing creates, which is the signature of a header
already zero-filled by the free.  The asserts NAME the object now, on the
panic's own channel, because `klog` is a ring that ring 3 drains and the
machine halts first.  That one line is what turned "something is wrong" into a
specific path.  The fourth was found by a test that had passed for a year:
T333 suspends a thread and reads its registers, and the kernel refuses that for
a RUNNING one.

What remains is not mechanism: the model-based fuzzer is not yet aimed at N
cores, and §9.4's limit stands — TCG interleaves, it does not reorder, so this
method finds logic races and not a wrong `memory_order`.

---

**The IOMMU is built** (Stage 10-dma, all six steps), and it closes the one
place where IRIS's central claim was fiction.  A driver holds an I/O port
capability and an IRQ capability and nothing else, and what goes with that is
containment — which against a DMA-capable device meant nothing at all: the
driver writes a physical address into the device and the device writes there,
past every check the kernel makes.

A device's reach is a capability now: the frames somebody mapped for it,
nameable, delegatable, revocable by the ordinary CDT.  Translation is enabled
at boot with every device BLOCKED, so the default is refusal rather than
identity or "unconfigured".

**One prediction in this roadmap was wrong and is worth recording as wrong.**
It said the stage needed PCI enumeration first.  It does not: installing a
translation needs the device's source-id, and the kernel does not need to have
DISCOVERED it — the source-id travels ON the capability, exactly as an I/O port
range travels on an IOPORT_CONTROL derivation.  seL4's kernel enumerates no PCI
either.  Enumeration is what ring 3 needs to make this USEFUL; it is not what
the kernel needs to make it SAFE, and a bus scanner in the kernel would have
bent charter P1/P2 for nothing.

**The limit that used to go in the same breath as the claim is gone.**  It read:
nothing in the test environment watches a device be refused, there is no DMA
engine under IRIS's control, so every assertion is about what the kernel
accepted, refused and wrote into the translation tables.  That was true and it
mattered, because all of it is consistent with hardware that ignored the
kernel completely.

**D-11 — a device is watched being refused (Stage 10-dma §10.2 step 6).**
QEMU's `edu` DMA engine is on the command line of every headless run and
**T353** is a ring-3 driver for it: an I/O-port capability for 0xCF8/0xCFC, a
bus scan that finds 1234:11e8, a frame retyped over the window the firmware
assigned its BAR out of the PCI-hole device Untyped and mapped uncached, and a
DMA target frame retyped from the suite's own Untyped.  One frame, two halves:
the processor writes a pattern into the first, the device is told to copy it
through its internal buffer into the second, and what the second half holds
afterwards is the answer.  With a unit and no mapping the sentinel stands and
the unit's fault record names source-id 0x10; with the frame mapped the pattern
arrives; with the mapping revoked the sentinel stands again; with no unit at
all the pattern arrives with nobody having granted anything.  The middle cases
are what make the first mean something — an untouched sentinel is also what a
DMA that was never issued looks like.

**What the driver found, which is the reason to write drivers.**  Three
defects, all pre-existing, none reachable from steps 1–5:

- `paging_virt_to_phys` returned `entry & ~0xFFF`, which strips a page-table
  entry's LOW flags and keeps every high one — so every physical address it
  produced for a non-executable page carried **NX at bit 63**.  Harmless for
  every caller that compared it against zero or fed it into another walk;
  fatal the first time such an address was written into a VT-d root entry,
  whose bits 63:39 are reserved.  The unit answered fault reason 10 and refused
  every device on the bus.  `PAGE_PA_MASK` / `PAGE_PA_MASK_2M` now name the
  address field, and the AP trampoline's CR3 was carrying the same passenger.
- The I/O-port ABI had **only byte-wide** `IN`/`OUT`, and PCI configuration
  space is reached through a 32-bit index register that discards anything
  narrower — so the kernel could not host a PCI driver at all.
  `INV_IOPORT_IN16/OUT16/IN32/OUT32` are seL4's `seL4_X86_IOPort_In8/16/32`
  family; the range check became `offset + width <= count`, because a dword
  read at the last byte of a range reads three bytes the capability does not
  cover.
- Every frame mapping was write-back.  `SYS_FRAME_MAP` flag bit 2 now asks for
  an uncached one (`PAGE_PCD | PAGE_PWT`), which is a property of the MAPPING
  and not of the frame.

`INV_IOSPACE_FAULT` is new and is not a defect: it reports the unit's fault
RECORD — source-id, address, reason, direction — on the IOSPACE_CONTROL
authority, because "a fault happened" and "MY device was refused" are different
claims and only the second is worth anything to a driver.

**And the limit that remains**: the device is emulated.  What T353 establishes
is that IRIS programs a VT-d unit correctly enough that a bus master behind it
reaches exactly the frames somebody mapped, as QEMU implements VT-d.  Real
errata, and devices behind bridges whose source-id is not their devfn, are not
exercised.  The bus scan is bus 0 function 0 because that is the machine.

## A-35 — the ABI is frozen at 1.0 (Stage 10-abi)

**Change**: what IRIS offers ring 3 is now a contract in one file
(`iris/abi.h`) and a test that checks it, where it was a description spread
across three headers and a hundred and forty comments.

Four syscall numbers — `SYS_INVOKE`, and `SYS_EXIT`/`SYS_YIELD`/`SYS_CLOCK_GET`
because they name no capability.  Seventy-seven invocation labels, 0 through
76, CONTIGUOUS.  One hundred and thirty-six reserved numbers that answer
`NOT_SUPPORTED` for ever.  `tests/kernel/test_abi.c` asserts all of it over
every number the dispatcher can see and every label in the declared range,
because a surface nothing checks is a surface that will be wrong.

**Versioning** rides in BootInfo (`abi_major`/`abi_minor`, v9) rather than
behind an invocation, because it is a fact about the KERNEL and there is no
capability to invoke it on — a syscall for it would have been a fifth numbered
door in all but name.  The root task refuses to boot on a major it was not
built against, which is the only way a caller can find out that a method it
depends on is gone other than one `NOT_SUPPORTED` at a time.

**Three rules make growth safe**, and each already had a precedent that this
promotes to a rule: a versioned struct is a PREFIX and fields are only
appended; an unknown flag bit is REFUSED rather than ignored, which is what
lets a later minor version define one; and an error code is part of the
contract, so changing which of WRONG_TYPE / ACCESS_DENIED / NOT_SUPPORTED a
case produces is a major change even though the call still fails.

**What the freeze found.**  `handle_id_t` and `HANDLE_INVALID` survived in 831
and 510 places as naming residue from a namespace Stage 4 deleted, and one of
those places was not cosmetic: `sys_sc_configure` read
`handle_id_t sc_h = (handle_id_t)arg0`, truncating a 64-bit capability argument
to 32 bits and widening it again at the resolver.  A value ABOVE the
CPtr/handle boundary — exactly the bit pattern that boundary exists to refuse —
was folded back INSIDE the valid range instead of being rejected.  The type is
`iris_cptr_t` everywhere now, `nc/handle.h` is `nc/cptr.h`, and `CPTR_NULL` and
`IRIS_CPTR_NULL` are one name instead of two.

**Scope**: no invariant changes state.  Six ledger rows move to RETIRED in the
charter §5.1 walk the stage required — `SYS_VMO_CREATE_FOR`, both
`tasks[TASK_MAX]` rows, `SYS_BOOTCAP_RESTRICT`, and the kernel-stack/PML4 row —
each of which named a retirement stage that closed without anyone returning to
it, which is the exact failure §5.1 was written to stop.

## A-36 — the platform is services, not kernel (Stage 10)

**Change**: three things a general-purpose system needs — a bus, the firmware's
description of the machine, and a disk — became reachable from ring 3 without
the kernel learning anything new about any of them.

**`pci` — the bus as a service.**  PCI configuration space is one pair of I/O
ports through which any device on the machine can be reprogrammed, so a
capability for it is a capability over the bus.  Giving that to each driver
would have undone Stage 10-dma one port range at a time: contain a device's
DMA, then hand out the config space that programs the DMA.  One task holds
those ports; init derives them once, hands them over, and deletes its own copy.
It also owns the PCI-hole device Untyped, which is what makes the restriction
enforceable rather than conventional — a driver holds no device Untyped, so
there is no window it could retype a frame over.

It carves its frames at STARTUP and in ADDRESS ORDER, which is a property of an
Untyped rather than a choice: the region is a watermark and does not go
backwards, so carving on demand would satisfy the first driver to ask and then
be unable to satisfy one whose device sits below it — a failure that depends on
the order clients happen to start in.

**ACPI, reachable.**  The tables live in memory the firmware marked
RECLAIMABLE or NVS: not usable RAM, so in no RAM Untyped; not unmapped address
space, so not in the PCI hole.  No capability named those bytes, so the
kernel's refusal to interpret them was not a delegation but a gap — anything
ring 3 wanted to know about the machine it could only learn by the kernel
having already decided to tell it.  Those regions are device Untypeds now, and
BootInfo carries the RSDP because a region is not a starting point.  **T354**
finds the root pointer the way every firmware reader does, by searching for the
signature, and validates the checksum.

**`blk` — an AHCI driver in ring 3.**  It asks `pci` for a controller by CLASS
code, gets a frame over BAR5, builds command structures in memory it owns, and
issues `READ DMA EXT`.  It is the first driver in IRIS that is useful rather
than illustrative, and it is the one that most needs Stage 10-dma: AHCI takes
physical addresses FROM ITS DRIVER, so a driver that lied would have the
controller write wherever it liked.  On a machine with a remapping unit it
binds an IOSpace to its controller's source-id and maps only its own two
buffers; on a machine without one the controller reaches all of memory, and it
reports which of the two it is rather than pretending.  **T355** checks the
last link — that the bytes are the bytes on the medium — with the FAT boot
signature, because "the command completed" and "the data arrived" are very
different claims about a bus master.

**The buffer's ownership** is the part worth copying.  A read replies with a
READ-ONLY frame capability over the data, and the service revokes what it
handed out before each read: a client still holding the previous capability
loses it rather than watching its data change underneath.  Revoke-before-reuse
is why the buffer can be one frame instead of one per request, and T355 asserts
it by trying to use the stale one.

**`net` — an e1000 driver in ring 3.**  Added after the disk, and the thing
worth recording is that it needed the SAME FIVE capabilities: an endpoint, a
reply object, an endpoint to the bus service, `IOSpaceControl`, and memory.
Two drivers for completely unrelated hardware, one manifest — because "drive a
PCI device with DMA" turned out to be a single shape once the parts had names.

It is the clearest case in the tree for containment.  A disk controller reads a
command table when it is told to; a NIC reads a RING of physical addresses
CONTINUOUSLY and nothing tells it to stop, so a driver that got those addresses
wrong would have the card scribbling asynchronously with no call to attribute
it to.  The driver binds its IOSpace before it writes a single ring address.

It parses nothing — no ARP, no IP, no checksums.  The stack above it builds the
ARP request and reads the reply, because a driver that understood ARP would be
policy inside a driver.  The round trip is the gate: a transmit-only check
proves nothing, since the card reports a descriptor done whether or not
anything was listening.

**The ARP wait was bounded wrongly twice, and the second way is worth keeping.**
First by a poll count of twenty thousand, which was generous on the success
path and pushed the boot past the gate's deadline on the failure path — the one
that runs on a machine with no network.  Then by two hundred and fifty, which
fixed that and started missing real replies under an IOMMU, where every round
trip costs more.  A COUNT cannot be both, because what it buys depends on how
fast the machine is.  A TIME bound can, and means the same thing on a fast
machine and a slow one.

**And the legacy-root ceiling moved for a reason nobody changed.**  It was
raised to 32 with a caveat that it is a fact about the MACHINE as well as the
kernel; attaching a network card made it 33, because the firmware describes one
more device and therefore publishes one more table region.  Nothing about the
kernel changed.  T305 caught it, which is why it refuses rather than reports.

**`ip` — ARP, IPv4 and UDP, as a service and not as part of the driver.**  It
holds one capability that reaches hardware at all: an endpoint to `net`.  No
ports, no device Untyped, no `IOSpaceControl`, no DMA authority.  That is the
payoff for a driver that parses nothing — the protocols can be replaced without
reimplementing an e1000, and a bug in a checksum cannot reach a bus master.

Its gate is a TFTP read against the server QEMU's userspace network carries at
the gateway.  It was chosen because every part of it is something only a real
peer can confirm: a server answers only a stack that got the ARP reply, the
IPv4 header checksum and the UDP checksum over its pseudo-header all right, and
gets any one of them wrong in silence otherwise.  The reply is matched to the
EPHEMERAL port the request went out from, because a TFTP server answers from a
port of its own — a stack that ignored ports would read back whatever arrived
first and call it the answer.

**Three things this cost, and each was a wrong assumption rather than a typo.**

The driver's receive buffers were 256 bytes, which is smaller than a datagram.
The first real reply arrived split across four descriptors, and nothing
correct above that layer could have reassembled it — the buffers are 1024 bytes
now, and the ring spans two frames because 8 × 1024 no longer fits in one.

Raising the size did not close the case, and the leftover is the more
interesting half.  A frame can still exceed 1024 bytes, the card still splits
it, and dropping the pieces that cannot be placed is NOT enough: the last
piece carries the card's end-of-packet bit and a plausible length, so it was
handed up as a frame — a tail with no Ethernet header, which the layer above
cannot recognise as a fragment because nothing in it says so.  A split frame is
now dropped whole.  Losing a frame is a fact a caller can act on; being given
part of one as though it were all of it is not.

The poll loop had to become a DISPATCHER rather than a filter.  A loop that
dropped every frame except the one it was waiting for makes the host
unreachable: an ARP request for our address arriving mid-wait has to be
ANSWERED, or the peer never learns where we are and the datagram never comes.

And what is being waited for needs a KIND, not just a port.  With only a port
number, "waiting for an ARP reply" was encoded as port zero, so a stray ARP
reply for some other host ended a wait for a datagram and the caller read back
a length of zero as if the request had timed out.  The two are different
questions and the dispatcher now asks which one is open.

**The screen became a diagnostic surface, because the serial port is a QEMU
assumption.**  Every gate in this tree reads `-serial file:`, and every
diagnostic this kernel emits before ring 3 exists goes to port 0x3F8.  Both are
fine under emulation and both are nothing on the hardware this system is meant
to reach, where most machines have no serial port: a boot that died before
userspace left a black screen and no record.

`fbcon` paints the kernel log onto the framebuffer.  It is deliberately not a
driver in the sense the charter uses — it claims nothing, allocates nothing and
holds no capability, it writes pixels to an address the firmware chose and
`paging_init` already identity maps, which is why one pointer is valid both
before paging exists and after.  It is the same exception the kernel's serial
path already was, on a second device, and for the same reason: a diagnostic
that needs a working userspace cannot report a userspace that never started.

The screen has one owner, so the kernel goes quiet when ring 3 asks where the
framebuffer is, and panic takes it back — a kernel that is stopping has no
userspace left to be polite to.  Three things fell out of building it.  White
on black is not an aesthetic choice: the pixel FORMAT is recorded nowhere in
this boot protocol, which carries only geometry, and white and black are the
two values that mean the same thing whether the firmware reports RGBA or BGRA.
Logged NUMBERS were invisible at first, because `klog_write_dec` pushes bytes
into the ring directly rather than through `klog_write` — `free RAM:  MB` is a
line that passes a banner check and says nothing.  And the mirror had to move
INSIDE the klog lock, because more than one core logs and fbcon has a cursor;
it owns no lock of its own so that it cannot add a rank to an order this tree
checks.

The gate is not a screenshot somebody looks at.  The console draws an 8x8
bitmap font, so `scripts/fbcon_ocr.py` decodes the screen back to text using
the font parsed out of the kernel's own source — there is no second copy to
drift from, and the check cannot pass against a screen that says something
else.

**And a service was one boot away from destroying somebody's disk.**  `blk`
took a client's LBA and put it straight into a `WRITE DMA EXT` — absolute, with
no offset and no bound — and `fs` wrote its superblock to LBA 0.  On a raw test
image that is the start of the image.  On a real drive it is the PARTITION
TABLE of the whole disk, and overwriting it destroys the addressing for every
partition on it, including the ones holding data.  No amount of care in the
filesystem above could have prevented that, because the filesystem was not the
thing choosing the address.

The comment beside the code gave the reason and, read on a machine, gave the
bug with it: *this disk is IRIS's own, because the block service numbers the
boot disk 0 and this one 1*.  True of the test runner, which makes the image.
False of hardware, where disk 1 is whatever SATA device enumerates second.

Two things changed, and the ORDER of them is the point.

Every LBA in the block protocol is now RELATIVE to a window, and the window is
the GPT partition typed `IRISFS-PARTITION`.  There is no way left to express an
absolute address, so containment is by construction rather than by checking: a
client that wants to write outside its partition has no word to put the address
in.

Reads and writes are deliberately NOT symmetric, and the asymmetry is the
honest part.  A write only ever happens inside an IRIS partition — no disk and
no argument relaxes that, because that is the property whose absence destroys
somebody's data.  A read on a disk with no IRIS partition is allowed and
absolute, because it has to be: finding the partition means reading the GPT,
and proving a port works at all means reading a sector off it.  A driver that
cannot read an unknown disk cannot discover anything about one.

So the claim is exactly this, and no more: on a machine whose disks hold
somebody else's data, IRIS can READ sectors of a disk it has no partition on.
It cannot write one.  It is worth stating rather than rounding, because the
first version of this bounded both and broke the two tests that read the boot
disk to prove the driver works — which is how the distinction got noticed.

Formatting is separately an AUTHORITY, granted like every other one here: a
token at a fixed offset in the partition's first sector, written by whoever
prepares a disk they are willing to lose.

The type is the sixteen bytes of ASCII `IRISFS-PARTITION` rather than a
generated GUID, deliberately: a real type GUID is never printable ASCII, so it
cannot collide with one, and it is legible in a hex dump — which is what
matters when the question is whether this system touched somebody's drive.

**The test had to change before it could test anything.**  A raw image cannot
check "stay inside your partition", because there is nowhere else to go.  The
runner builds a GPT disk with a DECOY partition full of recognisable bytes, and
`check_persistence.sh` compares the result against a pristine copy built the
same way: the filesystem must be in the IRIS partition, and every byte outside
it — the GPT, its backup, the decoy — must be identical.

Two things fell out of building it.  `fdisk` validates the generated image,
which is worth more than any assertion here could be: an independent
implementation agrees the table is well formed.  And the window was first
returned as a fifth word on `BLK_OP_INFO`, which wrote past `words[4]` — an
IPC message carries exactly `IRIS_MSG_WORDS` of them and INFO already used all
four.  The out-of-bounds write was caught only because the value came back as 4
instead of 8159; it has its own operation now.

This one is worth keeping for what it says about the rest of the tree: it
passed every gate for as long as it existed, because every gate ran on a
machine where the assumption happened to hold.  A test environment that is
uniform is a test environment that cannot tell you which of your reasons are
reasons and which are coincidences.

**Position is not identity, and a hardcoded disk index proved it.**  `fs`
addressed `FS_DISK_PORT 1` — correct on the machine the tests build, where disk
0 is the boot image and disk 1 is the one the runner made, and a coin flip on
real hardware, where the index is whatever order the AHCI ports enumerate in.
A machine with two SATA drives would have had `fs` inspecting whichever one
came second, and the right behaviour there (refuse everything) is
indistinguishable from a broken system.

`blk` answers `BLK_OP_HOME` — which disk carries an IRIS partition — and `fs`
asks before it reads anything.  It is the rule the driver below it already
follows: `blk` finds its controller by CLASS CODE rather than by
vendor:device, because a driver that matched an identifier would drive one
machine.

The index is printed on the `blk:` line even though under QEMU the lookup and
the old constant always agree.  That is the point: a mechanism whose right
answer is indistinguishable from its fallback is one nobody can tell has
stopped working.

**`fs` — a filesystem that survives the power going off.**  It holds the least
of any service here: an endpoint, a reply object, an endpoint to the block
service, and memory.  No disk, no controller, no device Untyped, no DMA
authority.  The thing that owns your data holds no hardware, which is the
arrangement the whole stack was built to make possible.

Its format is a superblock, a fixed directory and one sector per file.  That is
not the filesystem a system should ship and it is the smallest thing that makes
"persistent" a TESTABLE claim rather than a plan: `scripts/check_persistence.sh`
boots twice over one image, requires the first to format and report generation
1 and the second to find it and report 2, and then reads the image from the
HOST — which does not depend on IRIS being self-consistent about anything.

**And it found the defect underneath, which is the one worth recording.**  The
disk driver reported writes complete that were still in a CACHE.  An emulated
disk accepts a write and reports success without the host file changing, so the
first version of this passed every check IRIS could make about itself and left
nothing on the medium.  `blk` now issues `FLUSH CACHE EXT` after every write —
from the driver rather than as an operation clients must remember, because a
client that has to remember to flush is a client that will forget.

**Two harness defects surfaced with it, and both were mis-reporting rather than
breaking.**  The runner ran qemu under `timeout` and waited on the WRAPPER, so
`wait` returned before qemu had exited: the disk image was not flushed yet, and
qemu's exclusive lock on it made the NEXT boot produce an empty log — which
arrived at the bottom of the script as "missing scheduler running marker", a
kernel that booted and said nothing.  It waits on qemu's own pid now, and the
exit-code check moved ABOVE the marker checks so a qemu that never started says
so instead of being reported as a silent kernel.

**T356 — the system has numbers.**  An invocation that resolves a capability
and refuses, an IPC round trip to a real server, and a disk read: the floor of
every operation, what a service call costs, and a whole subsystem.  The
ceilings are order-of-magnitude guards and deliberately generous, because this
runs under TCG on a machine nobody controls; what a generous bound still
catches is an operation that became ten times more expensive because somebody
added a lock, a copy or a walk.  The NUMBERS are printed whether or not they
pass, because the number is the point.

**A defect class made impossible.**  Three slot collisions were found by
running the system during this stage, one of which minted an endpoint over
init's loader workspace and made every subsequent service load fail with
nothing naming the slot.  `services/init/init.h` now static-asserts every init
slot against every service-wide `IRIS_CPTR_*`, so a fourth is a compile error
that names the line.

**Scope**: no invariant changes state.  T305's legacy-root ceiling moves 27 →
32 for the five firmware regions, with the reason recorded on the constant.

## Non-regression guard

- T251 pins the closed manifest of RETYPE2-creatable types, and the boundary
  between a type you may not create (ACCESS_DENIED) and one that does not
  exist (NOT_SUPPORTED).
- T328 pins the ASID model: an unnamed address space cannot be entered, and
  identifiers return to the pool that issued them.
- T329 pins fault IPC: a fault is a badged message on an endpoint, the reply
  capability is the only authority that resumes, and the two retired syscalls
  answer NOT_SUPPORTED.
- T330 pins the bound notification: a signal reaches a thread blocked on an
  endpoint, and a pending one is consulted on the way in.
- T331 pins that waiting is a service: the timer service fires, the authority
  is its endpoint, and the three timed syscalls answer NOT_SUPPORTED.
- T332 pins revocation without a tail: queued sends of one badge are
  cancelled, another badge's are not, and a badged capability cannot do it.
- T001 pins that `SYS_GETPID` and `SYS_THREAD_EXIT` answer NOT_SUPPORTED;
  T002 that a granted clock can be asked of its owner and that it advances.
- T333 pins the five invocations of A-28, including the two refusals: a
  write-only capability cannot read registers, and a task with no IRQ
  capability cannot clear a route.
- T334 pins that IPC capability transfer is a COPY (A-29): the sender keeps
  what it sent, the receiver's capability is a revocable derivation child of
  the sender's slot, and deleting that slot is not revoking it.
- T338 pins the message ABI (A-33): the MessageInfo round-trips and its fields
  do not overlap at their limits, a hostile address in a message word is a word
  and not an address, a delivered capability's RIGHTS come back with it and
  zero comes back when nothing did, and a failed receive delivers nothing.
- T337 pins the invocation ABI (A-32): every method that had a syscall number
  answers NOT_SUPPORTED when called by one, the three calls that invoke nothing
  still work, a label sent to the wrong kind of capability is refused by type,
  a label that names no method is refused, the fifth argument register arrives,
  and the numbered-door gauge is a structural zero.  `test_syscall_dispatch`
  makes the stronger version from inside the kernel: every number from 0 to 400
  answers NOT_SUPPORTED except four.
- T335 pins the error-code rule of A-30: a capability of the wrong type is
  WRONG_TYPE, a capability of the right type without the authority is
  ACCESS_DENIED, and nothing about either is a secret from the caller.
- T336 pins `SYS_CNODE_SWAP`, which had no ring-3 coverage at all: two slots
  exchange occupants by identity, a swap against an empty slot is a move, and
  the derivation edges travel with the capability — including a parent swapped
  with its own child, the case the implementation's stack temporary exists
  for.  A naive content-only swap fails it.
- T353 pins that DMA containment is real and not merely programmed: the same
  driver, the same device and the same frame produce an untouched sentinel with
  no IOSpace mapping and the transferred pattern with one, and the machine with
  no remapping unit shows what that costs.  The headless gate requires the
  device to have been FOUND on every selftest run, because a `-device edu`
  dropped from the command line would otherwise read as a green run.
- `test_abi.c` pins the 1.0 surface (A-35): exactly four syscall numbers over
  every value the dispatcher can see, a label space with no holes that ends
  where it says, and a BootInfo that names the ABI it was built for.  Adding a
  label without declaring it fails the build's test rather than shipping.
- T354 pins that ring 3 can read the firmware's own description: the region is
  device memory, one frame covers it, and the root pointer inside it
  checksums.
- T355 pins the storage subsystem end to end (A-36): a ring-3 AHCI driver
  reads sector zero and the FAT boot signature is there, the controller's DMA
  is contained exactly when the machine has a unit to contain it with, and a
  buffer capability from the previous read no longer works.
- T260 pins the retirement of the create syscalls and their no-effect.
- T125/T126 pin the rejection of the migrated family on the legacy retype.
- The `IRIS_KOBJ_* == KOBJ_*` asserts pin the type ABI.
- Review: any PR that adds `kslab_alloc` for a canonical type, a new
  `SYS_*_CREATE`, or a new handle-first resolver for canonical objects must be
  rejected citing this ledger.
