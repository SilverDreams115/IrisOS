# A1 Design — Authority Namespace Endgame

Status: **IMPLEMENTED — resolution endgame reached** (A1 closeout).
Originally accepted as architectural direction; increments 1, 1b, 2a and 2b
landed the full dual-resolution matrix (commits `5c92039`, `3442e9b`,
`0d1b185`, `a4eb78e` + runtime tests T079–T083).

Decision:

```text
CSpace-canónico + capa de handles efímera, acotada y formalizada
```

- CSpace is the canonical model for persistent, delegable authority.
- The handle table remains as a permitted **ephemeral/bounded** layer:
  - ephemeral reply caps,
  - `SYS_CSPACE_RESOLVE` materializations,
  - per-process temporary working set.
- Reply caps do NOT migrate to CSpace.
- No big-bang rewrite. A1 advances in small increments, each gated by the
  full runtime suite.

## Executive recommendation

Converge every syscall that names a kernel object on the existing dual
resolvers (`kernel/new_core/src/cspace.c`), making the CPtr namespace
(`< 1024`) work everywhere a handle (`>= 1024`) works today. The handle
table is not deleted; it is *demoted* to an ephemeral working-set cache
whose only producers are a short, closed list (reply-cap delivery, IPC
cap delivery, `SYS_CSPACE_RESOLVE`, object-creation returns). Persistent
authority — what a process *is allowed to touch across its lifetime* —
lives in its root CNode and is delegated with `SYS_CNODE_MINT` /
`SYS_PROC_CSPACE_MINT` / `SYS_CAP_DERIVE`, never by long-lived raw
handles.

## Current model

Two authority namespaces coexist in one argument space, enforced since
Fase 8 (`CSPACE_DIRECT_CPTR_LIMIT` in `kernel/new_core/src/cspace.c`):

- **CPtr namespace (`value < 1024`)** — resolved by walking the process
  root CNode (`proc->cspace_root_h`, created in `kprocess_create`,
  `kernel/new_core/src/kprocess.c`). CSpace-only: a missing slot fails
  cleanly, `ACCESS_DENIED` is a hard stop, no handle-table fallback.
- **Handle namespace (`value >= 1024`)** — `slot | generation << 10`
  with generation ≥ 1, resolved through `HandleTable`
  (`kernel/new_core/include/iris/nc/handle_table.h`). Handle-table-only:
  never walks the CSpace, so populated low slots cannot be aliased by
  handle bit patterns.

The bridge between them already exists and is one-directional by design:
`SYS_CSPACE_RESOLVE` (`sys_cspace_resolve`,
`kernel/core/syscall/syscall_cspace.c`) materializes a CSpace slot into
a handle, preserving rights and badge; `SYS_CNODE_MOVE` /
`SYS_CNODE_MINT` carry a handle into a slot.

Well-known slots (`kernel/include/iris/endpoint_proto.h`,
`IRIS_CPTR_*`) are pre-start-minted by the spawner via
`SYS_PROC_CSPACE_MINT`; services are CPtr-first since Fase 8
(`docs/cptr-first-services.md`).

## Problem

Which namespace a syscall accepts is inconsistent — it reflects the
order in which subsystems were migrated, not a design rule:

- `SYS_EP_CALL` accepts a CPtr (`cspace_or_handle_resolve_endpoint_badged`),
  but `SYS_VMO_MAP` did not: it called `handle_table_get_object`
  directly (`sys_vmo_map`, `kernel/core/syscall/syscall_vm.c`), so a
  VMO minted into a CSpace slot was unusable without first
  materializing a handle.
- A process cannot hold its full persistent authority in its CSpace:
  any VMO/Process/TCB/SchedContext capability it needs to invoke must
  also exist as a raw handle, which defeats CSpace-based delegation,
  revocation (`SYS_CAP_REVOKE` trees), and auditing.
- Every object family that stays handle-only keeps the handle table
  load-bearing for *persistent* authority, blocking the endgame where
  the handle table can be frozen, bounded and reasoned about as a pure
  working-set cache.

## Per-object matrix (state after A1 closeout)

| Object | Syscalls | Resolution | Landed in |
|---|---|---|---|
| KEndpoint | `SYS_EP_SEND/RECV/CALL/NB_*` | dual, badged (`cspace_or_handle_resolve_endpoint[_badged]`) | pre-A1 (Fase 8/9) |
| KNotification | `SYS_NOTIFY_SIGNAL/WAIT[_TIMEOUT]` | dual (`syscall_ipc.c`); three secondary-arg sites still handle-only (see audit) | pre-A1 (Fase 13) |
| KCNode | `SYS_CNODE_MINT/MOVE/FETCH/DELETE/SWAP` | dual (`cspace_or_handle_resolve_cnode`) | pre-A1 |
| KUntyped | `SYS_UNTYPED_INFO/RETYPE/RESET` | dual (`cspace_or_handle_resolve_untyped`) | pre-A1 |
| KFrame | `SYS_FRAME_MAP/UNMAP` | dual (`cspace_or_handle_resolve_frame`) | pre-A1 (Fase 5) |
| KVSpace | `SYS_FRAME_MAP` target | CSpace-only (`cspace_resolve_vspace`) | pre-A1 |
| KIoPort / KIrqCap / KBootstrapCap | `SYS_IOPORT_*`, `SYS_IRQ_*`, `SYS_INITRD_*`, `SYS_FRAMEBUFFER_VMO`, `SYS_PROCESS_CREATE` auth | dual, generic (`cspace_or_handle_resolve_obj`) | pre-A1 (Fase 13) |
| KReply | `SYS_REPLY` | delivered as a fresh handle by `EP_RECV`; stays ephemeral **by design** (see below) | n/a |
| KVmo | `SYS_VMO_MAP/SIZE/MAP_INTO/SHARE` (vmo arg) | **dual — done** (T079, T080) | Inc 1 `5c92039`, Inc 1b `3442e9b` |
| KProcess | `SYS_PROCESS_STATUS/WATCH/KILL/EXIT_CODE/FAULT_INFO`, `SYS_THREAD_START`, proc args of `VMO_MAP_INTO/SHARE`, `SYS_PROC_CSPACE_MINT`, `SYS_HANDLE_TRANSFER/INSERT` dest, `SYS_IRQ_ROUTE_REGISTER` owner, `SYS_EXCEPTION_HANDLER/RESUME` (14 sites) | **dual — done** (T081, T082) | Inc 2a `0d1b185` |
| KTcb | `SYS_TCB_SUSPEND/RESUME/SET_PRIORITY/EXIT/GET_INFO` | **dual — done** (T083) | Inc 2b `a4eb78e` |
| KSchedContext | `SYS_SC_CONFIGURE`, `SYS_THREAD_SET_SC` | **dual — done** (T083) | Inc 2b `a4eb78e` |

Handle-table-intrinsic syscalls (`SYS_HANDLE_CLOSE/DUP/TRANSFER/INSERT/
TYPE/SAME_OBJECT`, `SYS_CAP_DERIVE/REVOKE`, and the src args of
`SYS_CNODE_MINT/MOVE`, `SYS_PROC_CSPACE_MINT`, IPC cap staging) operate
*on the handle layer itself* and stay handle-native; they are part of
the formalized ephemeral layer, not migration targets.

`SYS_VMO_UNMAP` takes `(vaddr, size)` — no capability argument, so A1
does not apply to it.

## Implementation nuances (discovered during increments)

- **Error-code preservation**: the TCB and SchedContext families
  historically return `IRIS_ERR_INVALID_ARG` (not `WRONG_TYPE`) on a
  type mismatch.  The migrated sites remap `WRONG_TYPE → INVALID_ARG`
  after the resolver so the handle path stays bit-for-bit compatible
  (and the CPtr path follows the same per-family convention).
- **`SYS_THREAD_SET_SC` takes no TCB argument**: it always binds the
  *calling* thread; its single cap argument is the SchedContext (now
  dual).  `0` remains the unbind path (`HANDLE_INVALID == CPTR_NULL ==
  0`, so it never reaches a resolver).  It performs no rights check on
  the SC — pre-existing semantics, deliberately not changed by A1.
- **Self-path guards stay ahead of the resolver**: `SYS_PROCESS_FAULT_INFO`,
  `SYS_EXCEPTION_HANDLER` and `SYS_EXCEPTION_RESUME` treat `arg0 == 0`
  as "self"; since 0 is also `CPTR_NULL`, the dual resolvers never see
  it and the special case is unambiguous.
- **The ref contract made the swaps mechanical**: every migrated site
  used `handle_table_get_object` (lifecycle-only ref) and
  `cspace_or_handle_resolve_obj` intentionally matches that contract,
  so no `kobject_release` path changed — including the ownership
  transfer in `SYS_THREAD_SET_SC` where the resolved ref becomes
  `t->sched_ctx`.
- **Test-slot scarcity**: a process cannot write (or delete) slots in
  its own root CNode without external help, so runtime tests self-mint
  through an init-provided fixture (`IRIS_CPTR_TEST_PROC`, slot 25 — the
  suite's own process cap with `RIGHT_WRITE`) and slots are mint-once.
  The 16..29 dynamic pool was exhausted by T079–T082; T083+ uses 32..39
  (documented in `endpoint_proto.h`).

## Reply caps — dedicated decision

**Reply caps stay in the handle table. They never migrate to CSpace.**

Rationale:

- A reply cap is one-shot and bound to a single in-flight `SYS_EP_CALL`
  (`kernel/core/syscall/syscall_reply.c`; lifecycle covered by T074,
  cross-process death cleanup by T078). It is the *definition* of
  ephemeral authority — the opposite of what a CNode slot models.
- Delivery is synchronous and per-message: `EP_RECV` materializes the
  reply cap directly into the receiver's handle table
  (`syscall_ipc_deliver_cap*`). The A1.5 receive-slot protocol
  (`a1-5-ipc-receive-slot.md`) deliberately excludes it: routing a
  reply cap through CSpace would buy zero delegation benefit — a reply
  cap must not be delegable or persistent (verified at runtime by
  T087).
- Kernel-side cleanup on process death walks the handle table today;
  moving reply caps would split that invariant across two structures.

This is the anchor case for the "ephemeral layer" being *permitted and
formalized* rather than tolerated: reply caps, `SYS_CSPACE_RESOLVE`
materializations, and IPC-delivered caps are the working set; the
CSpace is the estate.

## Migration plan — COMPLETED

Each increment shipped as one resolver swap family + one runtime test,
all gates green (`make`, `make test-unit`, `make smoke-runtime`,
`ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`), zero syscall
signature or number changes.

1. **VMO family** — done.  1a: `SYS_VMO_MAP` (`5c92039`, T079).
   1b: `SYS_VMO_SIZE` / `SYS_VMO_SHARE` / `SYS_VMO_MAP_INTO` vmo args
   (`3442e9b`, T080).
2. **Process family** — done (`0d1b185`, T081/T082): 14 call-sites
   across `syscall_proc.c`, `syscall_vm.c`, `syscall_cspace.c`,
   `syscall_cap.c`, `syscall_irq.c`.
3. **TCB + SchedContext** — done (`a4eb78e`, T083): 7 call-sites in
   `syscall_tcb.c` + `syscall_sched.c`.
4. **Handle-layer formalization** — this closeout: the closed producer
   list below, the namespace-contract comments in
   `kernel/new_core/src/cspace.c` and
   `kernel/new_core/include/iris/nc/handle_table.h`, and the audit
   verdict.  No debug assert was added: every producer call-site is on
   the closed list already, so an insert-path assert would only be able
   to re-check what the audit verified statically, while running on hot
   paths (IPC delivery) — cost without detection value.

## Handle producers — the closed list

After A1, a handle (`>= 1024`) can only come into existence through
these producers.  "Ephemeral" means the handle is working-set by
nature; "compat" means it carries authority that a future phase should
deliver via CSpace instead.

| Producer | File/function | Object types | Why allowed | Persistent authority? | Future direction |
|---|---|---|---|---|---|
| Object-creation returns | `SYS_VMO_CREATE`, `SYS_ENDPOINT_CREATE`, `SYS_NOTIFY_CREATE`, `SYS_SC_CREATE`, `SYS_CNODE_CREATE`, `SYS_PROCESS_CREATE`, `SYS_UNTYPED_RETYPE`, `SYS_CAP_CREATE_*`, `SYS_INITRD_VMO`, `SYS_FRAMEBUFFER_VMO` | all | creator needs an initial reference; CSpace placement is a separate deliberate act (`SYS_CNODE_MINT` / `SYS_PROC_CSPACE_MINT`) | transitional working set — holder decides | keep; creation-into-slot could be added later but is not required |
| Self-references | `SYS_PROCESS_SELF`, `SYS_TCB_SELF` | KProcess, KTcb | self-introspection; no delegation implied | ephemeral | keep |
| Handle-layer ops | `SYS_HANDLE_DUP`, `SYS_CAP_DERIVE` (`handle_table_insert_derived`) | all | operate on the handle layer by definition | mirrors source | keep |
| Cross-process placement | `SYS_HANDLE_TRANSFER` / `SYS_HANDLE_INSERT` dest, `SYS_VMO_SHARE` dest | all / KVmo | legacy delegation path; dest process cap now resolves dual | **compat — persistent authority delivered as a handle** | prefer `SYS_PROC_CSPACE_MINT`; candidates to retire once no service depends on them |
| IPC cap delivery (no slot declared) | `syscall_ipc_deliver_cap[_badged]` (`syscall_endpoint.c`) — EP_SEND/REPLY attached caps when the receiver declared no receive-slot, or as TOCTOU fallback when the declared slot was filled between declaration and delivery | transferable types | legacy compatibility; A1.5 receive-slot (`a1-5-ipc-receive-slot.md`) is the CSpace landing zone when declared | **compat — persistent authority delivered as a handle, receiver's choice** | receivers migrate to declaring receive-slots; producer shrinks as services adopt them |
| Reply-cap delivery | `EP_RECV`/`EP_CALL` path (`syscall_endpoint.c`, `syscall_reply.c`) | KReply | one-shot, bound to an in-flight call — the anchor ephemeral case | ephemeral **by design** | permanent; never migrates |
| CSpace materialization | `SYS_CSPACE_RESOLVE` (`syscall_cspace.c`), `SYS_CNODE_FETCH` (`syscall_cnode_ops.c`) | all | the sanctioned CSpace→handle bridge for APIs that want a working handle | ephemeral (authority already lives in the slot) | keep |
| Kernel bootstrap | `kernel_main.c` (init's KBootstrapCap), `kprocess_create` (root CNode handle), `task_thread_create`/`task_create` (KTcb auto-insert) | KBootstrapCap, KCNode, KTcb | pre-userland injection; notably the CSpace itself is *rooted* in a handle (`proc->cspace_root_h`) | infrastructure | keep; root-CNode-as-handle is an implementation detail invisible to userland |
| Kernel selftests | `phase3_selftest.c` | various | debug-gated kernel selftest | test-only | keep |

## Final audit — remaining handle-only call-sites

Scan basis: every `handle_table_get_object` call in `kernel/core/` at
closeout.  Classification:

- **Handle-layer intrinsic (allowed)**: `SYS_HANDLE_CLOSE/DUP/TYPE/
  SAME_OBJECT`, `SYS_CAP_DERIVE/REVOKE`, src args of `SYS_HANDLE_
  TRANSFER/INSERT`, `SYS_CNODE_MINT/MOVE` src, `SYS_PROC_CSPACE_MINT`
  src, IPC cap staging (`syscall_ipc_stage_cap*`).  These *are* the
  ephemeral layer's API.
- **Kernel-internal (allowed)**: the handle legs inside the dual
  resolvers (`kernel/new_core/src/cspace.c`), `sys_proc_cspace_mint`'s
  fetch of the child's root CNode from the *child's* table,
  `handle_table.c` internals, `phase3_selftest.c`.
- **Transitional gap (documented, small)**: exactly three secondary
  `KNotification` arguments are still handle-only —
  `SYS_PROCESS_WATCH` arg1, `SYS_IRQ_ROUTE_REGISTER` arg1,
  `SYS_EXCEPTION_HANDLER` arg1.  KNotification is dual-capable in its
  own family, and a CSpace-held notification reaches these three sites
  via `SYS_CSPACE_RESOLVE`, so no authority is trapped in the handle
  namespace; they are a mechanical one-increment follow-up, out of
  scope for closeout (kernel code freeze except comments).
- **Bugs/regressions found**: none.

**Verdict: no persistent authority is invocable only by handle**, except
the documented compat producers (cross-process placement, and IPC cap
delivery when the receiver declares no receive-slot — since A1.5 the
receiver can opt into direct CSpace delivery instead), and the three
notification secondary args above which have a sanctioned CSpace path.

## Invariants

- **I1 — namespace split**: `< 1024` resolves through CSpace only;
  `>= 1024` through the handle table only. No fallback in either
  direction (Fase 8 rule, `cspace_value_is_cptr`).
- **I2 — ABI stability**: no syscall number, signature, or register
  convention changes. Migration is argument-interpretation only, and
  only for the previously-rejected `< 1024` range — every value a
  program could legally pass before keeps its exact behavior.
- **I3 — rights are checked where they were checked**: resolver swaps
  pass `RIGHT_NONE` and leave each syscall's existing rights logic
  untouched (the `cspace_or_handle_resolve_obj` contract).
- **I4 — refcount contract preserved**: swaps use the lifecycle-only
  generic resolver so existing `kobject_release` paths do not change.
- **I5 — reply caps are handle-table-only**, one-shot, never minted
  into a CNode.
- **I6 — ephemeral layer is bounded**: the handle table only grows from
  the closed producer list (object creation returns, IPC cap delivery
  without a declared receive-slot, reply-cap delivery,
  `SYS_CSPACE_RESOLVE`).
- **I7 — every increment lands with a runtime test** proving the new
  CPtr path AND leaving all prior handle-path tests green.
- **I8 — closed producer list** (closeout): a handle may only be
  created/materialized by the producers table above.  New code that
  inserts into a handle table outside those categories is a design
  regression, not a convenience.
- **I9 — per-family error codes survive migration**: where a family
  used `INVALID_ARG` for type mismatch (TCB, SchedContext), the dual
  sites remap `WRONG_TYPE → INVALID_ARG`; nothing observable changed on
  the handle path.

All of I1–I7 held through increments 1–2b with zero exceptions; I8/I9
were added at closeout from implementation experience.

## Status and remaining post-A1 work

The resolution endgame is reached: every object with persistent,
delegable authority is CSpace-invocable, and the handle table is an
ephemeral/bounded working set with a closed producer list.  Runtime
coverage: T079–T083 (CPtr paths + authority-not-relaxed + failure
paths), T084–T088 (A1.5 receive-slot delivery, rights, atomicity,
call/reply separation, death cleanup), T001–T078 (handle paths
unchanged), 84/84 green.

**A1.5 shipped** (`a1-5-ipc-receive-slot.md`): the receiver can declare
a per-receive CSpace slot so IPC-transferred caps land as CPtrs instead
of handles — zero ABI change (two previously-dead input fields), legacy
path bit-for-bit intact when no slot is declared, reply caps untouched
(I5 holds, T087).  This turns the "IPC cap delivery" compat producer
into an opt-out: it only fires when the receiver declines (or on the
documented TOCTOU fallback).

**A1.6 shipped** (see `a1-5-ipc-receive-slot.md` § in-tree adoption):
svcmgr stores REGISTER caps CSpace-canonically via declared
receive-slots (pool 64..255, released on UNREGISTER), init's boot VFS
session lands as a CPtr in its own CSpace, and T089–T092 cover the
service flows plus legacy compat.  Suite: 88/88.

Remaining follow-ups:

1. **Cross-process placement retirement** — revisit the
   `SYS_HANDLE_TRANSFER`/`INSERT` dest producer, whose CSpace
   replacement is `SYS_PROC_CSPACE_MINT`; and consider a sanctioned
   own-root-CNode accessor so services need not type-probe for it.
2. **Notification secondary args** — make the three remaining
   `KNotification` handle-only sites dual (one mechanical increment).
3. **Handle-table shrink/freeze** — with services adopted, measure the
   live working set and revisit `HANDLE_TABLE_MAX` sizing.
