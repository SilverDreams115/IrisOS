# A1 Design — Authority Namespace Endgame

Status: **ACCEPTED** as architectural direction.

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

## Per-object matrix (state at time of acceptance)

| Object | Syscalls | Resolution today | A1 target |
|---|---|---|---|
| KEndpoint | `SYS_EP_SEND/RECV/CALL/NB_*` | dual, badged (`cspace_or_handle_resolve_endpoint[_badged]`) | done |
| KNotification | `SYS_NOTIFY_SIGNAL/WAIT[_TIMEOUT]` | dual (`syscall_ipc.c`) | done |
| KCNode | `SYS_CNODE_MINT/MOVE/FETCH/DELETE/SWAP` | dual (`cspace_or_handle_resolve_cnode`) | done |
| KUntyped | `SYS_UNTYPED_INFO/RETYPE/RESET` | dual (`cspace_or_handle_resolve_untyped`) | done |
| KFrame | `SYS_FRAME_MAP/UNMAP` | dual (`cspace_or_handle_resolve_frame`) | done |
| KVSpace | `SYS_FRAME_MAP` target | CSpace-only (`cspace_resolve_vspace`) | done |
| KIoPort / KIrqCap / KBootstrapCap | `SYS_IOPORT_*`, `SYS_IRQ_*`, `SYS_INITRD_*`, `SYS_FRAMEBUFFER_VMO`, `SYS_PROCESS_CREATE` auth | dual, generic (`cspace_or_handle_resolve_obj`, Fase 13) | done |
| KReply | `SYS_REPLY` | dual exists, but the cap is *delivered* as a fresh handle by `EP_RECV` | stays ephemeral (see below) |
| **KVmo** | `SYS_VMO_MAP/UNMAP/SIZE/MAP_INTO/SHARE` | **handle-only** (`handle_table_get_object`) | **migrate — Increment 1 starts here** |
| **KProcess** | `SYS_PROCESS_STATUS/WATCH/KILL/...`, proc args of `VMO_MAP_INTO/SHARE`, `PROC_CSPACE_MINT` | **handle-only** | migrate (later increment) |
| **KTcb** | `SYS_TCB_SUSPEND/RESUME/...` | **handle-only** (`syscall_tcb.c`) | migrate (later increment) |
| **KSchedContext** | `SYS_SC_CONFIGURE`, `SYS_THREAD_SET_SC` | **handle-only** (`syscall_sched.c`) | migrate (later increment) |

Handle-table-intrinsic syscalls (`SYS_HANDLE_CLOSE/DUP/TRANSFER/INSERT/
TYPE/SAME_OBJECT`, `SYS_CAP_DERIVE/REVOKE`) operate *on the handle layer
itself* and stay handle-native; they are part of the formalized
ephemeral layer, not migration targets.

## Reply caps — dedicated decision

**Reply caps stay in the handle table. They never migrate to CSpace.**

Rationale:

- A reply cap is one-shot and bound to a single in-flight `SYS_EP_CALL`
  (`kernel/core/syscall/syscall_reply.c`; lifecycle covered by T074,
  cross-process death cleanup by T078). It is the *definition* of
  ephemeral authority — the opposite of what a CNode slot models.
- Delivery is synchronous and per-message: `EP_RECV` materializes the
  reply cap directly into the receiver's handle table
  (`syscall_ipc_deliver_cap*`). Routing it through CSpace would require
  a receive-slot protocol (slot allocation, collision policy, cleanup
  on failed delivery) for zero delegation benefit — a reply cap must
  not be delegable or persistent.
- Kernel-side cleanup on process death walks the handle table today;
  moving reply caps would split that invariant across two structures.

This is the anchor case for the "ephemeral layer" being *permitted and
formalized* rather than tolerated: reply caps, `SYS_CSPACE_RESOLVE`
materializations, and IPC-delivered caps are the working set; the
CSpace is the estate.

## Migration plan (small increments, no big-bang)

Each increment: one resolver swap family + one runtime test, all gates
green (`make`, `make test-unit`, `make smoke-runtime`,
`ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`), no syscall
signature or number changes ever.

1. **VMO family** — swap `handle_table_get_object` →
   `cspace_or_handle_resolve_obj(..., KOBJ_VMO, ...)`:
   1a. `SYS_VMO_MAP` (this increment, T079);
   1b. `SYS_VMO_SIZE`, `SYS_VMO_SHARE` (vmo arg), `SYS_VMO_MAP_INTO`
       (vmo arg). `SYS_VMO_UNMAP` takes `(vaddr, size)` — no cap
       argument, nothing to migrate.
2. **Process family** — proc args of `SYS_VMO_MAP_INTO` / `SYS_VMO_SHARE`
   / `SYS_PROC_CSPACE_MINT`, then `SYS_PROCESS_*` lifecycle syscalls.
3. **TCB + SchedContext** — `syscall_tcb.c`, `syscall_sched.c`
   (typed resolvers `cspace_resolve_tcb` / `cspace_resolve_schedctx`
   already exist for the CSpace leg).
4. **Handle-layer formalization** — enumerate and document the closed
   list of handle producers; add a debug-build assertion/audit that no
   other path inserts long-lived handles; freeze `HANDLE_TABLE_MAX`
   sizing rationale as working-set-only.

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
  the closed producer list (object creation returns, IPC cap delivery,
  reply-cap delivery, `SYS_CSPACE_RESOLVE`).
- **I7 — every increment lands with a runtime test** proving the new
  CPtr path AND leaving all prior handle-path tests green.

## First recommended increment

`SYS_VMO_MAP` dual resolver (family 1a): smallest possible slice — one
call-site swap in `sys_vmo_map` (`kernel/core/syscall/syscall_vm.c`),
`RIGHT_NONE` + `KOBJ_VMO` through `cspace_or_handle_resolve_obj`,
existing `RIGHT_READ`/`RIGHT_WRITE` checks and mapping semantics
untouched. Runtime test T079: create a VMO, mint it into an own-CSpace
slot, `SYS_VMO_MAP` by CPtr, write/read the mapping, and verify the
failure paths (empty slot, wrong type, insufficient rights) plus the
unchanged handle path (T008).
