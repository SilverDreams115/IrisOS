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
| payer selection (`SYS_VMO_CREATE_FOR`) | per-payer accounting | svc_loader | Untyped delegation | with KVMO | yes | ACTIVE_LEGACY |
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
| `struct task tasks[TASK_MAX]` (static pool) | backing for kstack + arch-context + scheduler linkage | scheduler, thread create | TCB payload from Untyped; array → pointer/generation registry | S2 (run-queue index→pointer + productive-path Untyped source) | yes — no new consumers outside the scheduler | ACTIVE_LEGACY (bounded static pool, NOT kslab; runtime TCB storage — REMOVE pending) |
| `task_rsp[TASK_MAX]` (index-keyed RSP array) | per-slot kernel RSP, parallel to the array | scheduler context switch | `struct task.saved_krsp` | S2 inc.2 | — | REMOVED (Phase S2 inc.2 — the scheduler's first indirection) |
| run-queue `next[TASK_MAX]`/`queued[TASK_MAX]` + `(t - tasks)`/`&tasks[idx]` | run-queue identity by array index | rq_enqueue/remove/dequeue | intrusive pointer lists (`t->rq_next`/`rq_queued`) | S2 inc.2B | — | REMOVED (Phase S2 inc.2B Block A — run queue 100% pointer-based) |
| `tasks[j]` timeout scans (tick/idle) + slot allocation | iteration over the backing array | scheduler_tick / sched_handle_idle / task alloc | iteration over `ktcb_registry[]` (pointers+generation) | S2 inc.2 Stage C | — | REMOVED as identity (Phase S2 Stage C — everything goes through `ktcb_registry[i].tcb`) |
| `KTcbRegistrySlot ktcb_registry[TASK_MAX]` | reference registry (tcb*/generation/occupied/bootstrap), NOT payload | scheduler/alloc/lookup | same registry; transitional capacity | — | transitional TASK_MAX limit | TRANSITIONAL_IMPLEMENTATION_CAPACITY (Stage C) |
| `struct task tasks[TASK_MAX]` (static payload) | real TCB backing (registers/kstack ptr/scheduler state) pointed to by `registry[i].tcb` | registry (scaffolding) | canonical KTCB in Untyped (Stage D) | S2 inc.2 Stage D | yes — scaffolding, no new consumers | ACTIVE_LEGACY (scaffolding; REMOVE in Stage D, except the idle bootstrap) |
| `SYS_THREAD_SET_SC` (85) | SC self-bind | existing scheduler code | `SYS_SC_BIND(sc,tcb)` by CPtr | — | yes — frozen | FROZEN (Phase S2 inc.1) |
| `struct KTcb` wrapper (kslab) | cap-visible TCB object in the heap, separate from the task | — | `struct task` IS the KTCB (KObject at offset 0) | S2 inc.2 | — | REMOVED (Phase S2 inc.2 — one structure, one identity) |
| executable thread-create via pool (`SYS_THREAD_CREATE`) | a thread was carved from the kernel's static task pool and identified by a global id: no capability authorised it and no Untyped paid for its storage | — | `RETYPE2(KOBJ_TCB)` + `SYS_TCB_CONFIGURE` (CSpace/VSpace caps) + `SYS_TCB_WRITE_REGS` + `SYS_TCB_RESUME` | Stage 5 | n/a | **RETIRED (Stage 5 Step 4)** — number 48 reserved, `NOT_SUPPORTED`, zero in-tree callers.  Every thread in the suite and init's exception test is retyped from an Untyped and configured with capabilities; creation returns a CAPABILITY, not an index into a kernel array (charter §3.4/§3.5).  T297 pins the gate, T148 the number |
| first-thread start for a spawned process (`SYS_THREAD_START` 58 → `task_thread_create`) | the child's first thread still comes from the static pool, because the spawner cannot name the child's CSpace/VSpace | none | the spawner retypes and configures the child's initial TCB, once it holds caps to the child's CSpace and VSpace | Stage 7 (done) | n/a | **RETIRED (Stage 7)** — 58 answers `NOT_SUPPORTED`, number reserved, and `task_thread_create` is DELETED.  svc_loader composes the child's first thread the way any thread is composed: `RETYPE2(KOBJ_TCB)` from the child's budget, `SYS_TCB_CONFIGURE` with the child's CSpace and VSpace (which it retyped and holds since Stage 6-pure Step 4/5), `SYS_TCB_WRITE_REGS`, `SYS_TCB_RESUME`.  No path remains by which a thread exists because the kernel had a free slot; what still comes from the pool is the root task and the idle task, both built before any Untyped exists |
| idle task (static backing, registry slot 0) | bootstrap TCB outside Untyped, with no cap-visible object | scheduler | root-task TCB from BootInfo (Stage 5) | Stage 5 | yes — isolated bootstrap exception, never retyped or reused | BOOTSTRAP_EXCEPTION |
| native CDT/MDB in CNode slots | — (it is the correct seL4 mechanism) | `SYS_CSPACE_MINT`/`MINT_INTO`/`REVOKE`, retype2, teardown, receive-slot | — | — | n/a | IMPLEMENTED (Phase S3 — recursive cross-process revoke; validator + fuzzing) |
| handle-tree derivation (all types) | parallel derivation tree hidden in the handle table | — | per-slot derivation via the native CDT | Stage 3 | n/a | **REMOVED (Phase S4)** — `SYS_CAP_DERIVE`(78)/`SYS_CAP_REVOKE`(79) retired to `NOT_SUPPORTED` (numbers reserved); the table's derived-insert, revoke-children and parent-array machinery deleted.  `legacy_handle_derivation_migrated` has ZERO callers and is a structural 0 — the retirement witness |
| MDB LEGACY roots (`MDB_FLAG_LEGACY_ROOT`) | caps with no provable CSpace ancestor (handle/bootstrap/IPC-delivery origin) | bootstrap (kernel_main), legacy `kcnode_mint*`, IPC receive-slot delivery | a real CSpace origin (retype/derive by CPtr) | Stages 2/4/5 | yes — closed allowlist, observable `mdb_legacy_roots` counter | ACTIVE_LEGACY (counted debt; must → 0) |
| IPC cap-transfer with a handle source | a transfer's source was resolved by handle, not by CPtr | — | CPtr source + CSpace receive slot | Stage 2 | n/a | **REMOVED (Phase S4)** — `syscall_ipc_stage_cap_peek_badged` resolves the source through `cspace_resolve_slot`; a handle value is `INVALID_ARG`. Delivery installs the cap as an MDB CHILD of the source slot, so `cdt_ipc_transfer` deliveries are no longer LEGACY_ROOTs |
| root CNode from the slab | runtime CNode outside Untyped | the ROOT TASK only | the spawner retypes the CNode and names it in `SYS_TCB_CONFIGURE` | permanent for the boot path | yes | **CLOSED except for the root task (Stage 6-pure Step 5).**  Every child's root CNode is retyped by its spawner out of a budget the spawner holds, and named at configure time.  What remains on the slab is the ROOT TASK's, built before any Untyped exists — the same permanent boot-path exception seL4 has, and the reason this row cannot reach zero |
| root CNode reachable only via `cspace_root_h` (handle) | the CSpace ROOT is located through the handle table | — | `KProcess.cspace_root`: a structural back-reference holding one lifecycle + one active ref, released in `kprocess_teardown` | Stage 4 | n/a | **REMOVED (Phase S4, Step 4)** — resolving a CPtr no longer touches a handle table, and the cross-process paths (`SYS_CSPACE_MINT_INTO`, `SYS_PROC_CSPACE_MINT`, retype2, IPC receive-slot delivery) no longer read ANOTHER process's handle table to find its root.  Allowlist: `handle_table_get_object` 52 → 40 (14 files → 11), `handle_table_insert` 42 → 41 |
| implicit page-table allocation (PMM reserve on map) | kernel memory hidden by mapping | the KERNEL address space (kstack region, physmap) and the root task's maps made BEFORE it runs — six levels, ended by `kvspace_end_bootstrap` | page tables carved from an Untyped the address space names | Stage 6 (the bootstrap exception retires with Stage 7's user-space VSpace composition) | yes | **CLOSED for userland (Stage 6-pure Step 2/3)** — the kernel does not create page tables.  A map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE`; the holder retypes a `KOBJ_PAGE_TABLE` and installs it (`SYS_VSPACE_MAP_TABLE`).  The root task's pre-run maps are the only exception and it ENDS when the root task can speak for itself, after which no address space is implicitly funded while anybody is running (measured: six carves, all before the root task runs).  What stays kernel-funded is the KERNEL's own address space, which has no holder to ask |
| KFrame header sidecar (kslab) | metadata outside the region | VMO page frames and a spawning process's bootstrap frames (`kframe_alloc`) | header carved from the Untyped | Stage 6 (Etapas 3/5, with the paths that create those frames) | yes | **MIGRATING (Stage 6 Step 1)** — a frame RETYPED from an Untyped now carries its header as a child block of that same Untyped, carved from the top so it cannot displace the page-aligned carve nor land inside the page that is mapped into ring 3.  What still uses kslab are the frames with no Untyped to charge: VMO pages (`kframe_alloc_vmo_page`) and bootstrap frames (`bootstrap_kframe_map`) |
| process-level fault record (one per process) | belongs on the TCB | none | the record lives on the thread that takes the fault; the handler is armed on that thread (`SYS_TCB_SET_FAULT_HANDLER`) and read off it (`SYS_TCB_FAULT_INFO`) | Stage 7 Steps 6/8/12 (done) | n/a | **REMOVED** |
| `SYS_PROCESS_VSPACE` (107) | process authority → VSpace by handle | none | the spawner RETYPED the child's address space, so it already holds the capability and mints it where it is needed | Stage 7 Step 15 (done) | n/a | **RETIRED** — 107 answers `NOT_SUPPORTED`, number reserved |
| `SYS_BOOTCAP_RESTRICT` (dual-namespace split brain) | `arg0` is resolved with `cspace_or_handle_resolve_obj` (CPtr **or** handle), but the restricted clone is published with `handle_table_replace(ht, (handle_id_t)arg0, …)` — the two halves disagree about which namespace `arg0` is in | init (fb spawn cap), svcmgr (post-bootstrap strip) — **both pass handles**, so no live defect | publish the clone into a CSpace destination slot as an MDB child of the source slot, the way retype2/mint already do | Step 4 | yes | ACTIVE_LEGACY — **blocks the spawn-cap CPtr migration**.  A CPtr has generation 0 and every live handle slot has generation ≥ 1, so `handle_table_replace` rejects it with `BAD_HANDLE`: no corruption, but the syscall silently cannot succeed by CPtr.  Migrating `spawn_cap_h` to a CPtr before fixing this turns a working restriction into a no-op error path — i.e. a capability that was supposed to be narrowed stays wide |
| CSpace cycles below the root CNode | a CNode reachable from itself keeps its own references alive, so nothing frees it | any task minting a second-level CNode into itself | seL4-style recursive delete with zombie capabilities | unscheduled — the self-cycle is closed and nothing in tree builds the general case | n/a | **SELF-CYCLE CLOSED (Stage 7-proc); the general case remains.**  A slot naming its OWN CNode no longer takes an ACTIVE reference: an object reachable only from itself is reachable by nobody, and the count now says so.  The lifecycle reference stays, so close fires when the last EXTERNAL holder goes, empties the slots, and that release lets destroy run — which is what removed KProcess's reason to empty a root CSpace pre-emptively at a moment it knew because it counted threads.  BC-13 was the negative control for the old behaviour and is now the positive case.  A cycle THROUGH another CNode (A names B, B names A) is still uncollectable and still wants seL4's recursive delete with zombie capabilities.  Previously:   It was recorded as known and unexercised; it is now the last thing standing between IRIS and a kernel with no process object.  `kprocess_teardown` breaks the root-CSpace cycle pre-emptively, at a moment it knows because it counts threads, and deleting `KProcess` without recursive delete would leak the root task's CSpace.  Stage 5 Step 3 fixes the case it INTRODUCES (the root task's capability to its own root CNode) by emptying the root's slots at `kprocess_teardown` before dropping its refs; a cycle deeper in the tree is still uncollectable.  No in-tree code builds one; BC-11..BC-13 pin the fixed case |
| `KBootstrapCap` (monolith) | one object carried spawn, hardware, debug and framebuffer authority at once | — | one capability per authority + structured BootInfo | Stage 5 | n/a | **REMOVED (Stage 5 Step 2)** — the object type survives as the carrier of a SINGLE authority (`kind`), and `kbootcap_alloc` refuses a zero or multi-bit kind, so a monolithic boot capability cannot be constructed.  Six capabilities (process, initrd, IRQ, ioport, debug, framebuffer) are published one per slot and described in BootInfo v4.  `SYS_BOOTCAP_RESTRICT` (45) is RETIRED with its number reserved; `kbootcap_allows` and `kbootcap_clone_restricted` are deleted; `BOOT_CPTR_BOOTSTRAP_CAP` (slot 1) is permanently empty.  Least-authority result: vfs, a file server, no longer holds the authority to create processes.  T296 pins the split; T291 died with the mechanism.  History: Step 1-2b — Step 2a/2b SPLIT device and debug authority out: `IRIS_BOOTCAP_HW_ACCESS` (one bit for both IRQ and ioport creation) is replaced by two capabilities matched EXACTLY by the kernel, published one per slot and recorded in BootInfo v2.  svcmgr renounces hardware authority by DELETING those slots instead of cloning a narrowed monolith.  Step 2b did the same for `IRIS_BOOTCAP_KDEBUG` (kernel log, scheduler statistics, poweroff), published at `BOOT_CPTR_DEBUG_CONTROL` and delivered to children in the retired `IRIS_CPTR_SVC_REPLY` slot.  Step 2c split the rest; T296 pins the split, T291 keeps `SYS_BOOTCAP_RESTRICT` honest until it retires.  Step 1 — the structured BootInfo EXISTS (`struct iris_root_bootinfo`): the root task is told its initial caps by CPtr, the shape of its root CNode and every boot Untyped with its physical region, instead of agreeing with the kernel on constants and probing slots until one answered `NOT_FOUND`.  The object itself is unchanged and still carries the four-bit permission mask; splitting it into fine-grained caps and retiring `SYS_BOOTCAP_RESTRICT` is Step 2 |
| `KInitrdEntry` + `SYS_INITRD_*` | filesystem-aware kernel state | loader | user-space VFS/loader | Stage 10 (platform) — it no longer has a process server to retire with | yes | ACTIVE_LEGACY |
| kernel stacks / PML4 from the PMM reserve | allocation outside Untyped | task create; the ROOT TASK's PML4 only (a spawned process's PML4 is a page child of its budget since Stage 6 Step 3) | TCB/VSpace from Untyped | process/frame phases | yes | ACTIVE_LEGACY — this row is about WHERE the memory comes from, which is staged.  That every thread HAS a kernel stack to block on is a separate, unstaged divergence: ledger D-1 |
| `KChannel` | — | — | endpoints | Phase 13 | — | REMOVED |
| hardcoded ioport whitelist (`kioport_whitelist`, syscall_priv.h) | device policy in the kernel | kbd/console/fb/userboot via svcmgr | fine-grained ioport caps issued by the root task (BootInfo) | Stage 5 | yes — no new entry without a citation to charter §2.6/P3 | ACTIVE_LEGACY (temporary bootstrap) |
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

## Structural divergences from seL4 — recorded, not staged

The table above tracks mechanisms with a retirement stage.  These four are
different: they are ways IRIS's KERNEL is built that differ from seL4's, found
by comparing the two architectures rather than by auditing a migration.  None
is a purity violation under the charter's invariants — they are not ambient
authority, not a second namespace, not policy in the kernel — but the charter
(§5) requires every semantic divergence from seL4 to be documented and marked,
and these were not.  They are recorded here with the state they are actually
in, which for three of them is "no stage assigned".

Two of the four are DELIBERATE and therefore also appear in the charter's §6
divergence register; the other two are consequences of how IRIS was built and
could be revisited.

| # | Divergence | IRIS | seL4 | Consequence | State |
|---|---|---|---|---|---|
| D-1 | **Per-thread kernel stack; threads block IN the kernel** (the memory SOURCE is the staged row "kernel stacks / PML4 from the PMM reserve" above; this entry is about the architecture, which is not staged) | Every thread gets `TASK_STACK_SIZE` (8 KiB) of kernel stack plus a guard page, allocated with `pmm_alloc_pages(2)` from the kernel's PMM reserve — including a TCB retyped from an Untyped, whose *payload* the user paid for but whose kernel stack the kernel still supplies.  A blocking syscall parks the thread with its kernel state live on that stack (`saved_krsp`, `TASK_BLOCKED_*`) | Event-based kernel: ONE kernel stack per core, no thread ever blocks in the kernel.  A long operation returns to a preemption point and the syscall is restarted | Cost per thread the user did not pay for; kernel memory that scales with thread count; and it is the structural reason seL4 can bound in-kernel latency (and be verified) while IRIS cannot claim either | **SCHEDULED — Stage 9-evt.**  It carried "no stage assigned" from Stage 5 until the Stage 7 audit; assigning it a stage is not a claim that it is cheap.  Converting IRIS to a single-stack event kernel is a rewrite of every blocking path (IPC, futex, notification wait, reply), not an increment, and it must land BEFORE Stage 9 (SMP) so the atomicity properties are re-derived once rather than twice.  Recorded so the claim "seL4-like" is never read as covering it until then |
| D-2 | **CNodes have no guard** | Resolution is a pure radix walk: each level consumes `ctz(slot_count)` bits and indexes directly (`cspace.c`).  A CPtr's meaning is fixed by the CNode sizes along the path | CPtr resolution uses guard bits + radix + an explicit depth argument, so a CSpace can be sparse and levels can be skipped | IRIS's CSpace is a strict subset of seL4's: no sparse layouts, no depth-limited lookups, and a two-level CSpace costs the full radix of each level.  Nothing currently needs guards, which is why it has never bitten | **SCHEDULED — Stage 8-cap.**  Additive (a guard field per CNode + resolution change) with Stage 4 Step 6b's injectivity rule re-derived.  "Nothing currently needs guards" stopped being true in Stage 7: leaf exhaustion, the "a mint source must be a root CPtr" constraint and a whole extra CNode for a supervisor's child table are all costs of not having them |
| D-3 | **Different rights set** | `READ / WRITE / DUPLICATE / TRANSFER / WAIT / ROUTE / MANAGE`.  `RIGHT_DUPLICATE` gates minting a copy and `RIGHT_TRANSFER` gates sending one over IPC | `Read / Write / Grant / GrantReply`.  Copyability is NOT a right: whether a capability can be copied or minted follows from the derivation tree and the cap's type | A holder in IRIS can be given a capability it may invoke but not copy — expressible in seL4 only through the MDB, not through rights.  The two models are not translatable one-to-one, so "the same rights as seL4" is never a correct statement about IRIS | **DELIBERATE, revisable** (charter §6).  Load-bearing: `RIGHT_DUPLICATE` is what makes a delegation non-re-delegable today |
| D-5 | **Memory objects are CHARGED to an Untyped, not RETYPED by the user** (Stage 6; PAGE TABLES and the ADDRESS SPACE converted in Stage 6-pure) | The kernel carves VMO pages and mapping records out of an Untyped the caller NAMED, and accounts each as a child of it | The user retypes each object explicitly and maps it (`seL4_X86_PageTable_Map`); frames, IRQ handlers and I/O ports have no kernel object at all | Accounting and revocation are equivalent — nothing is created without a budget, and a budget cannot be reset while its objects live — but the user does not choose WHEN or WHERE each level exists, and cannot hold a capability to an individual page table | **CLOSED for address spaces, CSpaces and threads (Stage 6-pure + Stage 7); what remains is the KVMO, and retires with the memory server.**  A PAGE TABLE is retyped by the holder (`IRIS_KOBJ_PAGE_TABLE`) and installed by an explicit invocation (`SYS_VSPACE_MAP_TABLE`, seL4's `seL4_X86_PageTable_Map`); the kernel creates none, and a map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE`.  The ADDRESS SPACE itself is retyped too (`IRIS_KOBJ_VSPACE`, PML4 + header from one region), as is the root CNode, and a thread is CONFIGURED with both — `SYS_TCB_CONFIGURE(tcb, cnode, vspace)`, which is `seL4_TCB_Configure` outright since Stage 7-proc retired `SYS_PROCESS_CREATE` and the identity check that used to make the pair agree with a third object.  The address is validated as authority (a kernel-half install is refused).  The exclusive binding went with the per-process teardown that required it: threads sharing a CSpace and a VSpace is not a hazard to refuse, it is the definition of a process.  T301/T302 and host PT-1..PT-11 pin it.  What is still CHARGED rather than retyped: VMO pages and mapping records.  That frames, IRQ handlers and I/O ports have a kernel object at all is the deeper half of this row — a change to what a capability IS, not to who pays — and retires with the memory server |
| D-6 | **The MDB has unparented capabilities (LEGACY_ROOTs)** | 43 live roots of 335 MDB nodes, measured by T305 at Stage 7 close.  A LEGACY_ROOT sits in a CSpace with no parent, so `SYS_CSPACE_REVOKE` — which walks descendants — can never reach it | Every capability is a node in the derivation tree; the only roots are the initial capabilities the kernel installs at boot | Three classes.  BOOT PATH: legitimate and permanent (seL4's BootInfo capabilities are roots too).  KVmo PUBLISHES: a VMO is fabricated rather than retyped, so it has no ancestor to name.  FAULT DELIVERY: the faulting thread's capability is published into a mailbox unparented, so revoking the supervisor's thread capability does not reach the kernel's copy — this one is a defect | **FAULT-DELIVERY CLASS CLOSED; KVmo class scheduled with Stage 7-mem.**  The delivered capability is a child of the TCB slot the registrant named when it armed the handler — the same rule Stage 2 set for IPC — recorded at registration (`task.fault_src_cn/idx`, CNode retained) and verified by IDENTITY at delivery (`kcnode_slot_holds`), because a slot is a reusable location and between arming and faulting it can hold something that never authorised anything.  A registration whose source slot no longer holds the thread still delivers (a handler with a signal and no thread to answer with is a deadlock dressed as a working handler) but as a root, and the gauge counts it.  43 → 39.  T305 pins the count against growth and asserts that a live fault delivery adds no root |
| D-4 | **No per-thread IPC buffer object** | A message carries four inline words plus an optional bulk payload staged through `task.ipc_kbuf` (256 B inside the TCB) and copied to/from a user pointer named per call; one capability per message | Each TCB has an IPC buffer FRAME the user registers (`seL4_TCB_SetIPCBuffer`), holding the message registers beyond the physical ones and the extra-capability slots | The bulk-payload path is kernel-staged rather than user-provided, so its size is a kernel constant instead of a frame the user chose and paid for | **SCHEDULED — Stage 8-cap.**  Its trigger (frames retyped from Untyped) FIRED at Stage 6 and nobody returned to this row — the process failure charter §5.1 now forbids.  Until it lands, every TCB carries 256 B of kernel staging the user did not choose and did not pay for |

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

## Non-regression guard

- T251 pins the closed manifest of RETYPE2-creatable types.
- T260 pins the retirement of the create syscalls and their no-effect.
- T125/T126 pin the rejection of the migrated family on the legacy retype.
- The `IRIS_KOBJ_* == KOBJ_*` asserts pin the type ABI.
- Review: any PR that adds `kslab_alloc` for a canonical type, a new
  `SYS_*_CREATE`, or a new handle-first resolver for canonical objects must be
  rejected citing this ledger.
