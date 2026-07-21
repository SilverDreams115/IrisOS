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
| handle table / dual resolution | second authority namespace | every dual-resolver syscall; A1 materialization | CSpace-only invocation | CSpace-only ABI | new handle-first paths for canonical objects: FORBIDDEN (guard: T251/T260; review) | FROZEN for new producers (A1 list closed) |
| `SYS_ENDPOINT_CREATE` (73) | global fabrication without Untyped | — | RETYPE2 | S1 | — | RETIRED |
| `SYS_NOTIFY_CREATE` (19) | same + quota + handle | — | RETYPE2 | S1 | — | RETIRED |
| `SYS_CNODE_CREATE` (80) | same | — | RETYPE2 | S1 | — | RETIRED |
| implicit reply allocation (kreply in EP_CALL) | the kernel fabricated authority per call | — | explicit reply objects (recv arg2) | S1 | — | REMOVED |
| `SYS_UNTYPED_RETYPE` (87) handle-publishing | publishes authority as a handle | tests/authority suite (UNTYPED/FRAME/SC) | RETYPE2 | CSpace-only ABI | for migrated types: already rejected | MIGRATING |
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
| handle-tree derivation for migrated types (EP/Notif/Reply/CNode/TCB/SC) | parallel derivation tree hidden in the handle table | `SYS_CAP_DERIVE`/`SYS_CAP_REVOKE` (handle-only) | per-slot derivation via the native CDT (Fase S3) | Stage 3 | yes — no new producer; use `SYS_CSPACE_MINT` | ACTIVE_LEGACY (parallel to the CDT; the `legacy_handle_derivation_migrated` counter must → 0 when the handle-only syscalls are retired) |
| MDB LEGACY roots (`MDB_FLAG_LEGACY_ROOT`) | caps with no provable CSpace ancestor (handle/bootstrap/IPC-delivery origin) | bootstrap (kernel_main), legacy `kcnode_mint*`, IPC receive-slot delivery | a real CSpace origin (retype/derive by CPtr) | Stages 2/4/5 | yes — closed allowlist, observable `mdb_legacy_roots` counter | ACTIVE_LEGACY (counted debt; must → 0) |
| IPC cap-transfer with a handle source | a transfer's source is resolved by handle, not by CPtr | EP_SEND/NB_SEND/CALL/REPLY (`syscall_ipc_stage_cap_peek_badged`) | CPtr source + CSpace-only receive slot | Stage 2 | yes | ACTIVE_LEGACY (delivery already installs a LEGACY_ROOT MDB node; `cdt_ipc_transfer` counter) |
| root CNode at `kprocess_alloc` (kslab) | runtime CNode outside Untyped | every spawn | the spawner supplies a retyped CNode (process-server) | process-server | yes | ACTIVE_LEGACY |
| root CNode reachable only via `cspace_root_h` (handle) | the CSpace ROOT is located through the handle table | every resolver + Fase S3: `cspace_resolve_slot`, `cspace_own_root`, `SYS_CSPACE_MINT_INTO` (allowlist grown +3 citing charter §3 in the same commit) | BootInfo/root-task delivers the root CNode as a structural cap | Stage 5 | yes — root reads for resolution only | ACTIVE_LEGACY |
| implicit page-table allocation (PMM reserve on map) | kernel memory hidden by mapping | paging | PageTable objects from Untyped | frame/page-table | yes | ACTIVE_LEGACY |
| KFrame header sidecar (kslab) | metadata outside the region | frame retype | header inside the Untyped | frame/page-table | yes | ACTIVE_LEGACY |
| process-level fault record (one per process) | belongs on the TCB | fault delivery (Fase 20/25) | per-TCB fault / fault EP | process-server | yes | ACTIVE_LEGACY |
| `SYS_PROCESS_VSPACE` (107) | process authority → VSpace by handle | supervisors/pager tests | CSpace mint of the VSpace cap | process-server | yes | ACTIVE_LEGACY |
| `KBootstrapCap` | monolithic bootstrap authority | userboot/init/svcmgr/tests | structured BootInfo + fine-grained caps | root-task/BootInfo | yes | ACTIVE_LEGACY |
| `KInitrdEntry` + `SYS_INITRD_*` | filesystem-aware kernel state | loader | user-space VFS/loader | process-server | yes | ACTIVE_LEGACY |
| kernel stacks / PML4 from the PMM reserve | allocation outside Untyped | task/process create | TCB/VSpace from Untyped | process/frame phases | yes | ACTIVE_LEGACY |
| `KChannel` | — | — | endpoints | Fase 13 | — | REMOVED |
| hardcoded ioport whitelist (`kioport_whitelist`, syscall_priv.h) | device policy in the kernel | kbd/console/fb/userboot via svcmgr | fine-grained ioport caps issued by the root task (BootInfo) | Stage 5 | yes — no new entry without a citation to charter §2.6/P3 | ACTIVE_LEGACY (temporary bootstrap) |
| TOCTOU receive-slot→handle fallback (`syscall_ipc_deliver_cap_routed`) | CSpace-to-handle delivery degradation | IPC delivery with a declared slot that loses the race | CSpace-only install with the CDT (the race resolves in the tree) | Stage 2 | yes — counted (`iris_ipc_stat_toctou_fallbacks`), never a pattern | ACTIVE_LEGACY (the only permitted degradation, doomed) |

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

## Non-regression guard

- T251 pins the closed manifest of RETYPE2-creatable types.
- T260 pins the retirement of the create syscalls and their no-effect.
- T125/T126 pin the rejection of the migrated family on the legacy retype.
- The `IRIS_KOBJ_* == KOBJ_*` asserts pin the type ABI.
- Review: any PR that adds `kslab_alloc` for a canonical type, a new
  `SYS_*_CREATE`, or a new handle-first resolver for canonical objects must be
  rejected citing this ledger.
