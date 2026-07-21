# IRIS — Canonical Kernel Object Model (Fase S1, normative)

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
Untyped            (KUntyped     — implemented, canonical)
CNode              (KCNode       — implemented, canonical)
TCB                (task         — canonical since S2; execution path still uses the static pool → migration pending)
SchedulingContext  (KSchedContext— canonical; storage from Untyped, legacy retype ABI pending removal)
Endpoint           (KEndpoint    — implemented, canonical, Untyped-only since S1)
Notification       (KNotification— implemented, canonical, Untyped-only since S1)
Reply              (KReply       — implemented, canonical, Untyped-only + explicit since S1)
Frame              (KFrame       — canonical physical region; header sidecar still kslab)
VSpaceRoot         (KVSpace      — present; storage still kslab → migration pending)
PageTable          (implicit in paging — explicit object pending)
IRQControl         (implicit in spawn-cap/IRQ setup — explicit object pending)
IRQHandler         (KIrqCap      — present; storage still kslab)
```

No additional strictly-mechanical object was identified that must live in the
kernel: I/O ports (KIoPort) are device authority (equivalent to the
seL4/x86 IO-port-control model) and stay; everything else is policy and is
composed in user space.

## Classification of the current objects

| Current object | Final status | Canonical replacement | Migration phase | Reason |
|---|---|---|---|---|
| KUntyped | CANONICAL | — | S1 (done) | allocation substrate |
| KCNode | CANONICAL | — | S1 (runtime creation done; root-CNode-at-spawn pending) | CSpace |
| KEndpoint | CANONICAL | — | S1 (done) | synchronous IPC |
| KNotification | CANONICAL | — | S1 (done) | asynchronous signals |
| KReply | CANONICAL | — | S1 (done, explicit MCS style) | reply authority |
| KSchedContext | CANONICAL | — | S2+ (storage already Untyped via retype; legacy SYS_SC_CREATE to retire) | time |
| task (TCB) | CANONICAL (TCB) | TCB from Untyped | S2 (RETYPE2(KOBJ_TCB) done; execution path pending) | thread |
| KFrame | CANONICAL (Frame) | header inside Untyped | frame/page-table phase | physical memory |
| KVSpace | CANONICAL (VSpaceRoot) | storage from Untyped | frame/page-table phase | address space |
| KIrqCap | CANONICAL (IRQHandler) | storage from Untyped | device phase | IRQ routing |
| KIoPort | CANONICAL (arch) | storage from Untyped | device phase | port authority |
| KProcess | LEGACY_TO_REMOVE | user-space process server (TCB+CNode+VSpace) | process-server phase | process = policy |
| KVMO | LEGACY_TO_REMOVE | user-space memory server (Frames+pager) | memory-server phase | memory object = policy |
| handle table / handles | LEGACY_TO_REMOVE | CSpace-only invocation | CSpace-only phase | second namespace |
| per-process quota domains (VMO/page) | LEGACY_TO_REMOVE | Untyped as the budget | with KProcess/KVMO | quota ≠ explicit memory |
| notification quota | REMOVED (S1) | Untyped | S1 | retired |
| KBootstrapCap | BOOTSTRAP_EXCEPTION | structured BootInfo | root-task phase | bootstrap authority |
| KInitrdEntry | USERLAND_POLICY | user-space VFS/loader | with KProcess | filesystem-aware state |
| process metadata / parent-child / supervision | USERLAND_POLICY | svcmgr/init | already in user space | policy |
| file-backed regions / page cache / private-shared | USERLAND_POLICY | pager+VFS | already in user space (Fase 28) | policy |
| loader metadata | USERLAND_POLICY | svc_loader | already in user space | policy |
| kslab (for dynamic objects) | LEGACY_TO_REMOVE | Untyped retype | per family (ledger) | hidden allocator |

`NOT_AN_OBJECT`: scheduler queues, IRQ paths, klog buffers — internal kernel
state, not authority. `UNJUSTIFIED`: none found in the S1 audit.

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
- transitive revoke: the derivation tree lives TODAY in the handle table
  (`SYS_CAP_REVOKE`) in parallel with the native CSpace CDT (Fase S3,
  `SYS_CSPACE_REVOKE`; ledger).

## Bootstrap exceptions (enumerated, static, non-allocator)

1. Kernel image, initial stacks, boot metadata (static).
2. Per-process root CNode: fabricated by `kprocess_alloc` from kslab. Bounded
   (1 per process), but grows with processes → classified ACTIVE_LEGACY tied to
   KProcess; the first target of the process-server phase.
3. Kernel PMM reserve (`IRIS_PMM_KERNEL_RUNTIME_RESERVE`): page tables, kernel
   stacks, PML4, KVMO metadata — legacy internal allocators, recorded in the
   ledger; not available for creating canonical objects.
4. Selftest fixtures (phase3, host tests): static blocks with an untyped-child
   header and a NULL parent; test builds only.

No exception may be used to create objects after bootstrap or act as an
alternative allocator for the new model.
