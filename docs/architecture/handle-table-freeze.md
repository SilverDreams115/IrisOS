# Handle-table freeze (A1.7)

Status: ACTIVE POLICY.  Closes the A1 arc (A1 dual resolution → A1.5
receive-slot → A1.6 service adoption → this freeze): the handle table is
now a **measured, bounded, frozen ephemeral layer**, not a second
authority namespace.  Companion docs:
`a1-authority-namespace-endgame.md`, `a1-5-ipc-receive-slot.md`.

## Measurements (Fase 1/2 instrumentation, 2026-07-05)

Source: per-table live/hwm/insert/remove counters + kernel-global
atomic-max hwm + IPC delivery counters, exposed through the
`sys_sched_info` additive extension (buf_size ≥ 88; KDEBUG-gated; legacy
40-byte callers bit-for-bit unaffected).  Workload: full boot plus the
complete 92-test runtime suite — every producer class fires (spawns,
deaths, IPC transfers in all three delivery modes, reply storms,
resolves, revokes).

| Metric | Value | Notes |
|---|---|---|
| Busiest table peak (global hwm) | **33** | iris_test itself; fixture excluded |
| iris_test live at suite end | 28 | books balance: inserts − removes = live (asserted, T095) |
| IPC caps delivered into receive-slots | 39 | A1.5/A1.6 path |
| IPC caps delivered as handles (no slot) | 24 | legacy compat, opt-out |
| TOCTOU slot-race fallbacks | 1 | forced by T094; documented degradation |
| Reply caps created | 250 | all one-shot; ephemeral by design |
| `SYS_CSPACE_RESOLVE` materializations | 55 | sanctioned bridge |
| Legacy pressure leak check (T096) | 0 leaked | 32 lookup+close cycles, live before == after |

The deliberate full-table boundary fixture in `phase3_selftest.c` fills
a local table to the ceiling; it snapshots/restores the global hwm so
the sizing datum measures real processes only.

## Shrink decision — APPLIED

```text
HANDLE_TABLE_MAX: 1024 → 256
```

- Measured peak 33 → 256 keeps a ~7.8× margin.
- The handle-id encoding (`HANDLE_SLOT_BITS = 10`, ids ≥ 1024) and the
  CPtr boundary (`CSPACE_DIRECT_CPTR_LIMIT = 1 << HANDLE_GEN_SHIFT`)
  derive from the ENCODING, not the ceiling — the Fase 8 namespace
  split is untouched and no legal handle value changes meaning.
- `SVCMGR_DYNAMIC_SERVICE_CAP` mirrors the new value; the CSpace
  registration pool (slots 64..255) carries the first 192 dynamic
  services and was never bounded by the handle table.
- Gates after shrink: `make`, `make test-unit` (10196), boot smoke,
  selftests 92/92.
- **Raising it back requires new high-water evidence, not aesthetics.**

## Producer audit — the frozen closed list

Census basis: every `handle_table_insert*` call site at freeze time.

| Producer | File/function | Object types | Category | Future direction |
|---|---|---|---|---|
| Object-creation returns | `SYS_VMO_CREATE`/`SYS_ENDPOINT_CREATE`/`SYS_NOTIFY_CREATE`/`SYS_SC_CREATE`/`SYS_CNODE_CREATE`/`SYS_PROCESS_CREATE`/`SYS_UNTYPED_RETYPE`/`SYS_CAP_CREATE_*`/`SYS_BOOTCAP_RESTRICT`/`SYS_INITRD_VMO`/`SYS_FRAMEBUFFER_VMO` (`syscall_vm.c`, `syscall_endpoint.c:250`, `syscall_ipc.c:23`, `syscall_sched.c:27`, `syscall_cspace.c:74`, `syscall_proc.c:247`, `syscall_untyped.c:179`, `syscall_cap.c`) | all | WORKING_SET_OK | keep; creation-into-slot possible later, not required |
| Self-references | `SYS_PROCESS_SELF` (`syscall_proc.c:34`), `SYS_TCB_SELF` | KProcess, KTcb | EPHEMERAL_OK | keep |
| Handle-layer ops | `SYS_HANDLE_DUP` (`syscall_cap.c:48`), `SYS_CAP_DERIVE` (`syscall_cspace.c:32`, insert_derived) | all | WORKING_SET_OK | keep — they ARE the layer's API |
| Cross-process placement | `SYS_HANDLE_TRANSFER`/`SYS_HANDLE_INSERT` dest (`syscall_cap.c:109,255`), `SYS_VMO_SHARE` dest (`syscall_vm.c:551`) | all / KVmo | LEGACY_COMPAT | retire once no service depends on them; replacement is `SYS_PROC_CSPACE_MINT` |
| IPC cap delivery, slotless / TOCTOU | `syscall_ipc_deliver_cap_badged` (`syscall_endpoint.c`) — only reachable through `_routed` | transferable types | LEGACY_COMPAT (opt-out since A1.6) | shrinks as clients adopt receive-slots; measured 24 + 1 |
| Reply-cap delivery | `syscall_endpoint.c:499,765`, `syscall_reply.c:174` | KReply | EPHEMERAL_OK **by design** (invariant I5) | permanent; never migrates |
| CSpace materialization | `SYS_CSPACE_RESOLVE` (`syscall_cspace.c`), `SYS_CNODE_FETCH` (`syscall_cnode_ops.c:93`) | all | EPHEMERAL_OK | keep — the sanctioned CSpace→handle bridge |
| Kernel bootstrap | `kernel_main.c:173,312` (init bootcap, untyped seed), `kprocess.c:169` (root CNode), `task_lifecycle.c:578,689` (KTcb auto-insert) | KBootstrapCap, KUntyped, KCNode, KTcb | BOOTSTRAP_OK | keep; root-CNode-as-first-handle is what the svcmgr A1.6 probe relies on |
| Kernel selftests | `phase3_selftest.c` (local tables) | various | TEST_ONLY | keep; excluded from global hwm |

No `SHOULD_REMOVE` and no `BUG` entries: the one leak found in the A1
arc (caps attached to non-REGISTER svcmgr opcodes) was fixed in A1.6.

## Freeze policy

**Allowed:** exactly the producers above, in their categories.  All of
them flow through `handle_table_insert` / `_badged` / `_derived` — the
only insertion API — so every future producer is automatically counted
by the A1.7 instrumentation.

**Forbidden:**
- new persistent, delegable authority reachable ONLY by handle (design
  regression — new object types get a dual resolver and live in CSpace);
- new `handle_table_insert*` call sites outside the table above;
- raising `HANDLE_TABLE_MAX` without new high-water evidence;
- silent overwrite semantics anywhere in the handle layer.

**New-handle review rule:** a change that adds a handle producer must
(1) fit one of the categories above, (2) show up in the counters
(T095-style evidence), and (3) update this document's table.  If the
capability is persistent, the answer is a CSpace slot (pre-start mint or
receive-slot), not a handle.

**Receive-slot criterion:** a service that receives persistent caps by
IPC declares a receive-slot (A1.6 helpers in `iris/ipc_recv_slot.h`);
handles remain correct for mint/DUP sources, self-references, and
transient materializations.

**Reply caps:** the permanent ephemeral exception (I5).  One-shot,
handle-table only, never minted into a CNode — verified by T074/T087.

**Legacy compat:** slotless clients keep receiving handles ≥ 1024
indefinitely; the path is opt-out, measured, and covered by T092/T096.
It is not deprecated by this freeze.

## Remaining candidates (beyond this freeze)

1. Cross-process placement retirement (`SYS_HANDLE_TRANSFER`/`INSERT`,
   `SYS_VMO_SHARE` dest) once in-tree users migrate to
   `SYS_PROC_CSPACE_MINT`.
2. A sanctioned own-root-CNode accessor, replacing the svcmgr
   handle-type probe.
3. Notification secondary args (three handle-only sites) → dual.
4. Multi-child receive-slot stress (cross-process, not same-process
   threads).
