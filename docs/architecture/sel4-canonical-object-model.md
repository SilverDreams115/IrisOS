# IRIS — Canonical Kernel Object Model (Phase S1, normative)

## Ultimate objective

> IRIS must converge toward an architecturally seL4-pure microkernel: every
> dynamic kernel object is born from explicit Untyped memory, all persistent
> authority lives in CSpace, and processes, memory objects, loaders, pagers
> and resource policy are built in user space.

This document is **normative**: it defines the final set of kernel objects,
their sizes, their lifecycle and the bootstrap exceptions. No new mechanism may
introduce an object type, an allocator, or a creation path that is not recorded
here and in
[`sel4-convergence-ledger.md`](sel4-convergence-ledger.md).

**Assurance note**: IRIS does NOT claim assurance equivalence with seL4. There
is no mechanized formal verification; the convergence is architectural.

## Final canonical set

```
Untyped            (KUntyped     — canonical)
CNode              (KCNode       — canonical)
TCB                (task         — canonical; RETYPE2 and the EXECUTION path both, since Stage 7)
SchedulingContext  (KSchedContext— canonical; refill depth chosen at retype and sizing the object)
Endpoint           (KEndpoint    — canonical, Untyped-only since S1)
Notification       (KNotification— canonical, Untyped-only since S1)
Reply              (KReply       — canonical, Untyped-only + explicit since S1)
Frame              (KFrame       — canonical physical region, Untyped-backed)
VSpaceRoot         (KVSpace      — canonical; retyped by its HOLDER since Stage 6-pure)
PageTable          (KPageTable   — canonical since Stage 6-pure: an explicit object the
                                   holder retypes and installs, not something the kernel makes)
ASIDPool           (KAsidPool    — canonical since ledger A-21: carved from an Untyped by a
                                   holder of ASIDControl, and the source of the identifier
                                   without which no thread can be bound to an address space)
IRQHandler         (KIrqCap      — canonical, Untyped-backed)
IOPort             (KIoPort      — canonical arch object, Untyped-backed)
```

Two capability types have no backing object in seL4 and do in IRIS —
`KInitrdEntry` (a boot image) and `KBootstrapCap` (a boot authority).  Neither
costs kernel memory and neither is how anything is reached; seL4 expresses both
as capability TYPES with no object, which is a difference in how a CNode slot
is represented rather than in what the system can do.  `ASIDControl` and
`SchedControl` ARE that shape already: boot capabilities with no object behind
them.

No additional strictly-mechanical object was identified that must live in the
kernel: I/O ports (KIoPort) are device authority (equivalent to the
seL4/x86 IO-port-control model) and stay; everything else is policy and is
composed in user space.

## Classification of the current objects

| Current object | Final status | Canonical replacement | Migration phase | Reason |
|---|---|---|---|---|
| KUntyped | CANONICAL | — | S1 (done) | allocation substrate |
| KCNode | CANONICAL | — | S1 + Stage 6-pure (a child's root CNode is retyped by its spawner; only the root task's comes from the slab) | CSpace |
| KEndpoint | CANONICAL | — | S1 (done) | synchronous IPC |
| KNotification | CANONICAL | — | S1 (done) | asynchronous signals |
| KReply | CANONICAL | — | S1 (done, explicit MCS style) | reply authority |
| KSchedContext | CANONICAL | — | S2+ (storage already Untyped via retype; legacy SYS_SC_CREATE to retire) | time |
| task (TCB) | CANONICAL (TCB) | TCB from Untyped | S2 + Stage 7 (`RETYPE2(KOBJ_TCB)` and the EXECUTION path both done; `SYS_THREAD_START` retired, `task_thread_create` deleted).  Since Stage 7 the TCB also carries its own CSpace root, address space, fault record, death and exit code | thread |
| KFrame | CANONICAL (Frame) | header inside Untyped | frame/page-table phase | physical memory |
| KVSpace | CANONICAL (VSpaceRoot) | done — retyped by its HOLDER (Stage 6-pure), and NAMED from an ASIDPool (A-21) before a thread can be bound to it | done | address space |
| KPageTable | CANONICAL (PageTable) | done — Stage 6-pure Step 1: an explicit object the holder retypes and installs; a map whose walk is incomplete says `IRIS_ERR_MISSING_TABLE` instead of quietly spending a budget | done | paging level |
| KAsidPool | CANONICAL (ASIDPool) | done — ledger A-21 | done | address-space identity is a grant, not a kernel bitmap |
| KIrqCap | CANONICAL (IRQHandler) | storage from Untyped | done | IRQ routing |
| KIoPort | CANONICAL (arch) | storage from Untyped | done | port authority |
| KProcess | **REMOVED (Stage 7-proc)** | nothing — a process IS threads configured with the same CSpace and the same VSpace | done | process = policy |
| KChannel | **REMOVED (Phase 13)** | endpoints and notifications | done | a third IPC mechanism |
| KVMO | **REMOVED (ledger D-5)** | a grant is a run of FRAME capabilities, one per page — which page a pager may install is which capability it holds | done | the last object whose existence meant the kernel owned memory for somebody |
| handle table / handles | **REMOVED (Stage 4)** | CSpace-only invocation | done | second namespace |
| per-process quota domains (VMO/page) | **REMOVED (Stage 7 / 7-mem)** | Untyped as the budget | done | quota ≠ explicit memory |
| notification quota | REMOVED (S1) | Untyped | S1 | retired |
| KBootstrapCap | BOOTSTRAP_EXCEPTION | structured BootInfo | root-task phase | bootstrap authority |
| KInitrdEntry | BOOTSTRAP_EXCEPTION | a capability type with no object, as in seL4 | Stage 10 (platform) | boot image; costs no kernel memory and is not how anything is reached |
| process metadata / parent-child / supervision | USERLAND_POLICY | svcmgr/init | already in user space | policy |
| file-backed regions / page cache / private-shared | USERLAND_POLICY | pager+VFS | already in user space (Phase 28) | policy |
| loader metadata | USERLAND_POLICY | svc_loader | already in user space | policy |
| kslab (for dynamic objects) | **REMOVED as a runtime allocator (ledger A-16)** | Untyped retype | done | it is a BOOT ARENA now, SEALED at the end of boot: allocating from it afterwards panics, no syscall handler can reach it through any chain of calls (purity gate, transitive closure, zero exemptions), and T318 reads the seal from ring 3 |

`NOT_AN_OBJECT`: scheduler queues, IRQ paths, klog buffers — internal kernel
state, not authority. `UNJUSTIFIED`: none found in the S1 audit, and none in
the A-26 re-read either.

**The count, as of ledger A-26.**  Eleven RETYPEABLE types (Untyped, CNode,
TCB, Endpoint, Notification, Reply, SchedContext, Frame, PageTable, VSpace,
ASIDPool), every one of them seL4's and every one born only through
`SYS_UNTYPED_RETYPE2`.  Four more that are not retypeable: `KIrqCap` and
`KIoPort` come from a budget through `SYS_CAP_CREATE_IRQCAP`/`IOPORT`, which is
seL4's arrangement rather than a divergence (`IRQControl_Get` is not a retype
either); `KInitrdEntry` and `KBootstrapCap` are IRIS's, and seL4 would express
both as capability types with no backing object.  Three enumerators reserved
and dead: `KOBJ_PROCESS`, `KOBJ_VMO`, `KOBJ_CHANNEL`.

## Central rule (S1)

For any migrated object:

```
the retyped memory IS the kernel object's storage
```

- The header (`struct KObject`: type, refcounts, lock, ops) is the first field
  of the payload and lives INSIDE the retyped region (asserts in
  `syscall_untyped.c`).
- There is no dynamic sidecar metadata in kslab for migrated objects.
- The retyped block is `KUNTYPED_ALIGN` (64 B, back-pointer to the parent) +
  `align64(sizeof(object))`; on destruction the block is zero-filled and
  decrements the source Untyped's `child_count`.

## Sizes and states (S1)

Size contract: `KUNTYPED_ALIGN = 64 B` granularity (an explicit contract
equivalent to seL4's size_bits; the exact sizes are `sizeof(struct K*)`, fixed
by compile-time asserts and visible in the table below as the consumed block =
64 + align64(sizeof)).

| Object | Payload | Alignment | Retype source | Initial state | Destruction precondition |
|---|---|---|---|---|---|
| Endpoint | sizeof(KEndpoint) | ≤64 | normal Untyped | IDLE, empty queues | refcount 0 (all caps + kernel refs released) |
| Notification | sizeof(KNotification) | ≤64 | normal Untyped | bits=0, no waiters | refcount 0 |
| Reply | sizeof(KReply) | ≤64 | normal Untyped | free (caller=NULL, staged=0) | refcount 0 |
| CNode(n) | KCNODE_ALLOC_SIZE(n), n power of 2 ≤4096 | ≤64 | normal Untyped | empty slots | refcount 0 (close releases the slots) |
| SchedContext | sizeof(KSchedContext) | ≤64 | normal Untyped | default budget | refcount 0 |
| Sub-Untyped | arg bytes (≥4096, page multiple) | page | normal/device Untyped | used=0, gen=0 | refcount 0 and no children |
| Frame | arg bytes (≥4096, page multiple) | page | normal/device Untyped | unmapped | refcount 0, mapped_count 0 |

`maximum count per retype`: 32 objects and 128 KiB per batch
(`KUNTYPED_RETYPE_MAX_COUNT/MAX_BYTES`); UNTYPED/FRAME always count=1 in S1.
`zeroing`: every normal-Untyped block is zero-filled on carve and on destroy.
`device`: a device Untyped produces only UNTYPED/FRAME (U11/U12).

## Authority

- Creation: holding an Untyped cap with RIGHT_WRITE + empty destination CSpace
  slots. **Never** a numeric quota, a handle, or `RIGHT_MANAGE` on a process
  (S19/S20).
- Every capability created by retype appears directly in CSpace (S21);
  `SYS_CSPACE_RESOLVE` is the only sanctioned CSpace→handle bridge (ephemeral
  materialization, A1 contract).
- Default rights at birth: EP `R|W|DUP|XFER`; Notification
  `R|W|WAIT|DUP|XFER`; Reply `R|W|XFER|DUP` (DUP only so the supervisor can
  mint it into the child and then DROP its copy); CNode/SC/Untyped/Frame
  `R|W|DUP|XFER`.

## Lifecycle (delete / revoke / reuse)

See [`kernel-object-lifetime.md`](kernel-object-lifetime.md). Summary:
- deleting a cap = freeing that slot/handle; the object lives while caps or
  kernel refs remain (S10).
- the last cap triggers `close` (wakes waiters with CLOSED — S25/S26/S27) and,
  with no kernel refs, `destroy` (the block returns, zero-filled, to the
  region).
- `SYS_UNTYPED_RESET` reclaims the region only with `child_count == 0` (S13)
  and bumps `generation` (reuse witness, S12/S28).
- transitive revoke: there is exactly ONE derivation tree, the native CSpace
  CDT/MDB (`SYS_CSPACE_REVOKE`).  The parallel handle tree and its
  `SYS_CAP_DERIVE`/`SYS_CAP_REVOKE` were deleted in Phase S4 / Stage 3.

## Bootstrap exceptions (enumerated, static, non-allocator)

1. Kernel image, initial stacks, boot metadata (static).
2. The ROOT TASK's root CNode and address space: fabricated from kslab,
   because they are built before any Untyped exists that could pay for them.
   Bounded at exactly one, and it no longer grows with the number of children —
   since Stage 6-pure a child's root CNode, VSpace and TCB are retyped by the
   spawner out of a budget it holds.  This is the permanent boot-path
   exception; seL4 has the same one.
3. Kernel PMM reserve (`IRIS_PMM_KERNEL_RUNTIME_RESERVE`): page tables, kernel
   stacks, PML4, KVMO metadata — legacy internal allocators, recorded in the
   ledger; not available for creating canonical objects.
4. Selftest fixtures (phase3, host tests): static blocks with an untyped-child
   header and a NULL parent; test builds only.

No exception may be used to create objects after bootstrap or act as an
alternative allocator for the new model.
