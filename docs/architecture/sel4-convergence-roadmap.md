# IRIS — seL4 Convergence Roadmap (normative, by dependencies, no dates)

Orders the stages toward the [purity charter](iris-sel4-purity-charter.md).
Each stage declares its **technical precondition** (what must be closed
first) and its **closing criterion** (what must be demonstrable when it ends).
The [ledger](sel4-convergence-ledger.md) maps every transitional mechanism to
its retirement stage. No stage may be declared closed while its productive
path still depends on the mechanism it retires (charter §3.10).

## Status

| Stage | State |
|---|---|
| 0 — TCB consolidation | ✅ CLOSED (Phase S2 inc.2) |
| 1 — CDT/MDB | ✅ CLOSED (Phase S3) |
| 2 — CSpace-only cap transfer | ✅ CLOSED (Phase S4) |
| 3 — CSpace-only derive and revoke | ✅ CLOSED (Phase S4) |
| 4 — Dual namespace retirement | ✅ CLOSED |
| 5 — seL4-like bootstrap | ✅ CLOSED |
| 6 — Remaining memory and objects | ✅ CLOSED |
| 6-pure — the user retypes what the kernel charged | ✅ CLOSED (5 steps) |
| 7 — KProcess retirement | ✅ CLOSED (15 steps + 7-mem + 7-proc) |
| 8-mcs — Full MCS scheduling | ✅ CLOSED |
| 8-cap — the capability model's last gaps | ✅ CLOSED — D-2 (CNode guards, root included), D-8 (preemptible revoke) and D-4 (per-thread IPC buffer, every service migrated) CLOSED; D-3 decided and registered as a permanent divergence |
| 9-evt — the event kernel (D-1) | ✅ CLOSED — one kernel stack per core, no thread blocks in the kernel, `context_switch`/`task_yield`/`kstack_alloc` deleted |
| 10-mem — the memory server (D-5) | ✅ CLOSED — there is no KVmo.  A grant is a run of frame capabilities, one per page |
| 12-pol — mechanism, not policy (P2) | ✅ CLOSED — the kernel futex, the notification waiter ceiling, the default CSpace size and the THREAD ceiling are gone; what is left is classified as mechanism with a reason each (A-19) |
| 11-life — object lifetime (D-7) | ✅ SEMANTICS CLOSED — an object exists exactly while a capability names it, measured for every type (T322), over generated MDB shapes (T323) and through a CSpace cycle (T321).  The MECHANISM stays a refcount, registered as a permanent divergence; the one disagreement it produced (a donated scheduling context released twice) is fixed and T324 reads every pool slot each run to catch the next |
| 9 — SMP | pending |
| 10 — General-purpose platform | pending |

Charter invariants closed so far by this roadmap: **A2, A3, A4, A6, A7, A8,
A9, A10** (authority); **O2–O6** (objects); **I1–I7** (IPC); **S1–S5**
(scheduling); **M1–M5** (memory); **P1, P3** (policy); **S2** with the process
object itself (Stage 7-proc); **O1** (object *form*) with `KVmo` — the last
object that was fabricated rather than retyped — deleted (D-5); **A5** (no
ambient authority) with the three SELF syscalls retired (A-18); **P2**
(mechanism, not policy) with the audit that closed it (A-19).

**P2** (mechanism, not policy) was AUDITED (ledger A-19) rather than assumed,
and closed.  Five things the kernel was deciding for somebody else are gone: a
futex, a notification waiter ceiling, a default CSpace size, two dead scheduling
defaults, and the THREAD CEILING — `ktcb_registry[TASK_MAX]` refused a thread
when its array filled, and everything that read it was walking it, so it is an
intrusive list now.  The static task pool shrank from 256 entries to two: the
idle thread and the root task, which is the same bootstrap exception seL4's root
task is.  The rest is classified as mechanism with the reason for each.

**36 of the 36 charter invariants are MET.**

### Where the line is now

Stage 6 answered *who pays* for memory.  Stage 6-pure answered *who creates
it*, which is the question seL4 answers and the one ledger D-5 recorded as
open.  The two are not the same claim and the difference is the whole point:
a kernel that charges you for a page table it made on your behalf accounts
honestly and still decides for you.

After Stage 6-pure the kernel creates no page table, no PML4 and no CNode.  A
map whose walk is incomplete says so (`IRIS_ERR_MISSING_TABLE`) instead of
quietly spending a budget, and `SYS_PROCESS_CREATE` composes a process from a
VSpace and a CNode its creator retyped — `seL4_TCB_Configure`'s shape.  What
the kernel still funds is bounded to things with no holder to ask: its own
address space, and the root task's maps made before the root task exists.

After Stage 7, no thread exists because the kernel had a free slot, no memory
ceiling exists that a capability did not set, and there is no process object to
hold either — a "process" is threads configured with the same CSpace and the
same VSpace.

## How close is this to seL4

Measured against seL4's model rather than against this roadmap's own progress,
because a roadmap that grades itself is not evidence.  The honest answer has
two halves and they are far apart:

**The authority model is done.  So, now, is the kernel architecture.**

Of the eight dimensions below, five are met or as close as they will get:
capabilities (no open gap — one registered permanent divergence, D-3, decided),
MCS scheduling (nothing left that is not seL4's), no ambient authority (the
kernel decides no device policy at all), the ABI shape (far, by a recorded
decision), and — as of Stage 9-evt — the KERNEL ARCHITECTURE: one kernel stack
per core, and no thread blocks inside the kernel.

That last one was the item the whole roadmap was sequenced behind, and the only
one that was a rewrite rather than an increment.  With it closed, IRIS can
state the two things a blocking multi-stack kernel cannot: kernel memory does
not scale with thread count (T318 measures it), and the longest a thread can be
kept out of the CPU is the longest kernel path between preemption points, not
"however long the longest kernel path takes".

Seven of eight.  IPC joined them — `ipc_kbuf` is deleted, every thread sends
from a frame it registered, and a payload with no buffer is an error — and so
did the kernel heap: the slab is a boot arena, sealed at the end of boot, with
no syscall handler able to reach it and the seal readable from ring 3.

The object model closed with them.

`KVMO` left every production path in stages — the framebuffer became a device
Untyped a driver retypes from (D-9), boot images became frames
(`SYS_INITRD_FRAME`), a spawned image's segments and stack became frames
retyped from the child's budget, vfs served from frames, and the pager's own
page cache and private pool became one capability per page out of its own
budget — and the LAST path was `PGR_OP_MAP_RESUME`: mapping a page of a region
the CLIENT granted, at an offset the client names.

That was the one thing a VMO did that a frame did not, and replacing it meant
redefining what a grant is.  **A grant is a run of frame capabilities, one per
page.**  Then which pages a pager may install is the set of capabilities it
holds, whether it may install one writable is `RIGHT_WRITE` on that page,
revoking one page is deleting one slot, and "an offset past the end of the
region" is an empty CSpace slot.  The offset argument disappears because the
question it asked is answered by which capability you were given.

`KInitrdEntry` and `KBootstrapCap` remain as objects, but neither costs the
kernel memory any more and neither is how anything is reached: they are boot
capabilities that seL4 would express as capability TYPES with no backing
object, which is a change to how a CNode slot is represented rather than to
what the system can do.

`KVmo` is gone (D-5).  It was the last object whose existence meant the KERNEL
owned memory for somebody — allocating its pages on a schedule the holder did
not choose, keeping a page-address array, and range-checking an offset into a
region it managed.  A client that wants a pager to map page N of its memory
grants the FRAME for page N.

| Dimension | State | Evidence |
|---|---|---|
| Object model and creation | **met** | every canonical object is retyped from Untyped; address spaces and CSpaces are retyped by their HOLDER (Stage 6-pure).  `KProcess` — the largest of the four object types seL4 has no equivalent for — is DELETED (Stage 7-proc), and `KVmo` is DELETED with it (D-5): a grant is a run of frame capabilities, one per page, so which page a pager may install is which capability it holds and the kernel owns no memory on anybody's behalf.  MMIO is handed over as a DEVICE Untyped the way seL4's BootInfo does it (D-9, D-10, T316/T317).  What remains is `KInitrdEntry` and `KBootstrapCap`: neither costs kernel memory, neither is how anything is reached, and seL4 would express both as capability TYPES with no backing object — a change to how a CNode slot is represented, not to what the system can do |
| Capabilities (CSpace, CDT, revoke) | **close** | native CDT/MDB, recursive cross-process revoke, one namespace, and CNode GUARDS on the capability rather than the object — the root CSpace included, which is where a guard is load-bearing and where it was missed first (D-2, closed).  Revoke is preemptible (D-8, closed).  The rights set is different from seL4's and now permanently so (D-3, decided): `RIGHT_DUPLICATE` makes a delegation non-re-delegable, which seL4 cannot express — its derivation tree records what was derived, it does not prevent deriving.  Pinned by host RG-1..RG-5.  **No open gap in this dimension**, only a registered permanent divergence |
| IPC | **met** | endpoints, badges, reply objects, receive slots, no handle fallback, and `SYS_REPLY_RECV` — seL4's combined `ReplyRecv`, which a passive server needs so it never crosses the gap between returning its donated time and blocking again (Stage 8-mcs, T309).  D-4 is CLOSED: `SYS_TCB_SET_IPC_BUFFER` is seL4's `seL4_TCB_SetIPCBuffer`, `ipc_kbuf` is deleted, and a payload with no registered buffer is an error. |
| No ambient authority | **met** | boot authority is one capability per authority, every per-process quota is gone (Stage 7), and the kernel's hardcoded ioport whitelist is REMOVED (Stage 5): the range a holder may claim travels on the `IOPORT_CONTROL` capability, narrowed by derivation (`SYS_IOPORT_CONTROL_NARROW`, T164/T171).  The kernel decides no device policy at all.  A-18 removed the LAST ambient authority: `SYS_VSPACE_SELF`, `SYS_CSPACE_SELF` and `SYS_TCB_SELF` handed a thread capabilities to its own address space, CSpace and thread asking for no capability at all.  All three are RETIRED — delegated at `IRIS_CPTR_OWN_VSPACE`/`OWN_CSPACE`/`OWN_TCB` for services, in BootInfo for the root task (which is seL4's arrangement), and for a thread the loader never saw, in the ENTRY REGISTER: the trampoline delivers the thread argument in `rdi` as well as `rbx`, so a thread written in C reads its own TCB capability as a parameter.  `mdb_legacy_roots` 32 → 23 |
| No kernel heap | **met** | The kernel's slab is a BOOT ARENA and it is SEALED at the end of boot: allocating from it afterwards panics.  seL4 has no kernel heap because its boot code carves the root task's initial objects from a statically-known region and describes everything else as Untyped — which is exactly what this is, now that the door shuts behind it.  The purity gate's reachability check runs with ZERO exemptions and over the TRANSITIVE closure (A-16): no syscall handler can reach the allocator through any chain of calls, not merely by naming its caller, and T318 reads the seal from ring 3 so the property cannot stop being true unobserved |
| MCS scheduling | **close** | all four pillars are in as of Stage 8-mcs.  Budget and period are enforced; **sporadic replenishment** returns every tick consumed exactly one period later, so a thread can never spend more than its budget in any window of its period (host R-1..R-8); **timeout faults** make an overrun a policy decision a temporal supervisor takes rather than an invisible stall (`SYS_TCB_SET_TIMEOUT_HANDLER`, T307); and **SC donation** lends a client's scheduling context to a PASSIVE server for the duration of a Call, so an SC-less thread runs on the requester's time instead of — as it did before — running unbudgeted (T308).  `SYS_REPLY_RECV` closes the last of them (T309): without it a passive server is, between reply and receive, runnable with no scheduling context — and an SC-less thread is not charged, so it runs unbudgeted for exactly as long as the second syscall takes.  `refill_max` is now the SC's own, chosen at RETYPE and sizing the object (T315): a passive server woken per request needs a deep replenishment queue and a periodic task needs two, and the memory is charged to whoever asked for the depth instead of every SC paying for the worst case out of the kernel.  **Nothing in this dimension is still not seL4's** |
| ABI shape | **far, by decision** | 68 live numbered syscalls of 127 numbers, each taking CPtrs and checking rights itself, where seL4 has a handful and expresses every other operation as an INVOCATION on a capability.  Registered permanent divergence (charter §6) |
| Object lifetime | **close** | seL4 has no per-object reference count: an object exists while a capability to it exists, and `cteDelete`/`finaliseCap` walk the derivation tree.  IRIS reaches the same ANSWER through two counters, and that is now measured rather than asserted — T322 checks the rule for every retypeable type, T323 over generated derivation shapes, T321 through a CSpace cycle (where IRIS and seL4 behave identically: neither collects it idly, both reclaim it when the Untyped is revoked).  The mechanism difference is registered and permanent (D-7).  What it cost is recorded too: a donated scheduling context was released twice because the loan moved a pointer and not a reference, and the object hit refcount 0 with a slot still naming it — found by T324, which reads every pool slot because nothing else ever reads an idle one |
| Kernel architecture | **met** | D-1, the only one of these that was a rewrite rather than an increment, is CLOSED.  IRIS has ONE kernel stack per core and no thread blocks inside the kernel.  No blocking syscall keeps live state across its block (step 1); a parked one abandons its frame (step 2, T310); the whole ring-3 register context lives in the TCB (step 3, T314); and `TSS.RSP0` is set once and never changes, because a DISPATCHER on the core's stack replaced `context_switch` — which is deleted, along with `task_yield`, `scheduler_sleep_current`, the idle task and `kstack_alloc`.  T318 measures the consequence from ring 3: eight threads, and the kernel's physical reserve does not move, where the old per-thread stacks would have cost two pages each |

### The two that no further stage closes

**D-1 is the structural one.**  seL4 is an event kernel: one stack per core,
no thread ever blocks in the kernel, a long operation returns to a preemption
point and the syscall restarts.  IRIS gives every thread 8 KiB of kernel stack
and parks it there with live state.  That is the reason seL4 can bound in-kernel
latency and be verified, and converting to it is a rewrite of every blocking
path (IPC, futex, notification wait, reply) — not an increment.  The ledger
assigns it no stage deliberately.

**The ABI shape is permanent by choice.**  In seL4 there is one way to exercise
authority: invoke a capability.  In IRIS there are 68 entry points, each
validating its own arguments.  The authority SEMANTICS are equivalent — nothing
is reachable without naming a capability — but the verification surface is not,
and no convergence work changes that.

### If it needs a number

**Roughly 75% on capability semantics, 25% on kernel architecture.**  Stages
5 through 7 moved the first a long way and the second not at all, because the
second does not move in stages.  Anyone quoting a single figure for "how seL4
is IRIS" is averaging two things that should not be averaged.

### Remaining work, by real size

1. **MCS proper** — donation, timeout faults, refills (Stage 8).  The largest
   piece of genuinely stage-able convergence left.
2. **`KProcess` and `KVMO` retirement** with the process and memory servers
   (Stage 7) — closes the rest of D-5 and the last kernel-side policy.
3. **CNode guards** (D-2) — additive, and nothing currently needs them.
4. **D-1** — a separate project, not a stage.

## Stage 0 — TCB consolidation  ✅ CLOSED (Phase S2 inc.2)

- The open increment is closed and committed; the working tree is clean.
- Canonical KTCB: `struct task` IS the object (KObject at offset 0); the
  wrapper is removed; five separated lifetimes (cap / object / execution /
  registry / storage) with no ambiguous refcount.
- Stable lifecycle: TERMINATED ≠ destroyed; the destructor is the sole
  storage releaser; pointer-based run queues; registry with generation.
- Retypable storage: `RETYPE2(KOBJ_TCB)` creates canonical TCBs (inactive,
  `configured=0`) with storage inside the Untyped and the cap directly in
  CSpace; the migrated family is now {EP, Notif, Reply, CNode, SC, TCB}.
- No new handle: RETYPE2 creation publishes no handles; the `make
  check-purity` guard freezes the existing consumers.
- Recorded debt: the thread EXECUTION path (SYS_THREAD_CREATE) still comes
  from the static pool + handle; its replacement (TCB_CONFIGURE over a
  retyped TCB) requires CSpace/VSpace caps as arguments and is defined in
  Stage 5/6 (post-CDT). The idle task is an isolated bootstrap exception
  (registry slot 0, never retyped or reused).

## Stage 1 — CDT/MDB  ✅ CLOSED (Phase S3)

Precondition: Stage 0 (closed).
Design: `docs/architecture/cspace-cdt-mdb.md`.

- Intrusive per-slot derivation metadata (not in handles): parent /
  first-child / doubly-linked siblings.
- Global parent/child relationships (cross-CNode, cross-process) — the links
  are slot pointers, agnostic of the owning KProcess.
- Single canonical primitives (`kcnode_slot_install_linked/derive/move/
  delete/revoke`); no TU mutates `cn->slots[]` directly.
- Recursive cross-process revoke (`SYS_CSPACE_REVOKE`) with deterministic
  order (deepest-leftmost post-order) and lifecycle effects outside the lock;
  proven by T288-T290 (runtime, real processes) + model-based fuzzing
  (5 seeds × 4000 ops, parent-vector comparison).
- Exact rollback (retype2 publishes via the primitive; a failure uninstalls
  the leaves and undoes the carve). delete ≠ revoke; intermediate delete
  reparents to the grandparent.
- Untyped as the MDB ancestor of its retyped objects (D.1/D.2/D.3).
- Locking: global `mdb_lock` → `cn->lock`; releases outside the lock.

Debt that stays live (does NOT block, retired in later stages):
`legacy_handle_derivation_migrated` (parallel handle-tree, `SYS_CAP_DERIVE`)
→ Stage 3; `mdb_legacy_roots` (non-CSpace origins) → Stages 2/4/5;
`cdt_ipc_transfer` (IPC delivery = LEGACY_ROOT) → Stage 2.

## Stage 2 — CSpace-only cap transfer  ✅ CLOSED (Phase S4)

Precondition: Stage 1 (closed).

- CPtr source: `syscall_ipc_stage_cap_peek_badged` resolves the source
  through `cspace_resolve_slot` and carries the slot identity
  (`task.ep_cap_src_cn`/`ep_cap_src_idx`) across a blocking send.  A handle
  value is `INVALID_ARG` — no fallback (invariant A6).
- The delivered cap is installed with `kcnode_slot_install_linked` as an MDB
  **child of the source slot**, not a LEGACY_ROOT: an IPC delegation is now
  revocable from the sender or any of its ancestors.
- Order: DELIVER, then commit.  MDB parenting needs the source slot occupied,
  so the source is consumed only after the child exists; move semantics are
  preserved because `kcnode_slot_delete` reparents the delivered cap to the
  grandparent.  A cap revoked while staged is never delivered (entry
  invariant 4).
- The TOCTOU slot→handle degradation is REMOVED — the last CPtr→handle
  fallback in the kernel.  A raced/occupied destination fails closed: the
  message arrives with no capability and the source slot is untouched.
  `iris_ipc_stat_toctou_fallbacks` is a structural 0 (T094 forces the race,
  T095 pins the counter).
- Endpoint close leaves the source-slot refs for the woken sender to drop:
  releasing the last ref on a CNode runs a destructor that tears down every
  slot, which must not happen under `ep->lock`.

Debt that stays live (does NOT block): delivery into the receiver's handle
table when it declares NO receive slot (legacy receivers) → Stage 4 with the
dual namespace; `SYS_CNODE_MINT` still marks its slot a LEGACY_ROOT → Stages
3/4.

## Stage 3 — CSpace-only derive and revoke  ✅ CLOSED (Phase S4)

Precondition: Stages 1–2 (both closed).

- `SYS_CAP_DERIVE` (78) and `SYS_CAP_REVOKE` (79) are RETIRED: the numbers
  stay permanently reserved and answer `NOT_SUPPORTED`.
- The handle table's parallel derivation tree is DELETED — its derived-insert
  and revoke-children entry points and the per-slot parent array are gone.
  The handle table is now a flat reference table with no derivation semantics.
- Every productive and test path derives with `SYS_CSPACE_MINT` (slot→slot,
  installing a real MDB child) and revokes with `SYS_CSPACE_REVOKE`
  (recursive, cross-CNode, cross-process).
- Unblocked earlier in Phase S4 by giving `KIoPort`/`KIrqCap` a CSpace-native
  origin (see the Stage 3 prep note in the ledger).
- `legacy_handle_derivation_migrated` has zero callers: a structural 0, kept
  as the retirement witness in the `UNTYPED_QUERY` layout.

Result: there is exactly ONE derivation tree in the system.  Charter A9 and
A10 move to MET.

## Stage 4 — Dual namespace retirement  ✅ CLOSED

**Closing criterion met: there is one authority namespace.**  The handle table
is not reduced to zero consumers — `HandleTable`, `KProcess.handle_table`, the
implementation and its unit suite are DELETED, and the purity allowlist has no
`handle_table_*` or `cspace_or_handle_resolve_` entries left because those
identifiers no longer exist.  What the allowlist still holds is Stage 6's
inventory: the object families born from the kslab heap.

Retired to `NOT_SUPPORTED`, numbers permanently reserved: `SYS_HANDLE_CLOSE`
(15), `SYS_HANDLE_DUP` (22), `SYS_IOPORT_RESTRICT` (43), `SYS_VMO_SHARE` (46),
`SYS_HANDLE_INSERT` (59), `SYS_HANDLE_TYPE` (52), `SYS_HANDLE_SAME_OBJECT`
(53), `SYS_CNODE_MINT` (81), `SYS_UNTYPED_RETYPE` (87), `SYS_CNODE_MOVE` (89),
`SYS_CNODE_FETCH` (90), `SYS_CSPACE_RESOLVE` (95).  Added: `SYS_CAP_IDENTIFY`
(117) and `SYS_CAP_SAME_OBJECT` (118), CSpace-native and strictly weaker than
what they replace.

Three structural zeros are the permanent gate (T095): handle-live,
handle-delivery and TOCTOU.  Any of them moving means a second namespace came
back.

Precondition: Stages 2–3 (both closed — no authority lives handle-only anymore).

- ~~Remove the value-range discrimination (<1024 / ≥1024).~~  ✅ the boundary
  is the handle TAG BIT, defined once in `nc/handle.h`; CPtrs own the low 31
  bits and a CPtr addresses exactly one capability (Step 6b).
- ~~Remove the bootstrap's handle producers (kernel_main dual insert).~~  ✅
  the bootstrap capability and every boot Untyped are published into CSpace
  ONLY; RBX carries 0 and a failed publish is fatal rather than "non-fatal
  because the legacy handle still works".
- Remove handle resolution from every dual resolver.
- Remove `SYS_CSPACE_RESOLVE` and `SYS_HANDLE_DUP` — the last two producers,
  both with no consumer outside `iris_test`.
- Remove the handle table when it has zero consumers; the `check_purity`
  allowlist must reach empty.

**Retired in Stage 4 so far** (numbers permanently reserved, `NOT_SUPPORTED`):
`SYS_IOPORT_RESTRICT` (43), `SYS_VMO_SHARE` (46), `SYS_HANDLE_INSERT` (59),
`SYS_UNTYPED_RETYPE` (87), `SYS_CNODE_FETCH` (90), plus the handle leg of IPC
delivery (`syscall_ipc_deliver_cap_badged`).  Every one was a handle PRODUCER
whose CSpace form already existed.

Measured surface (from `scripts/purity_allowlist.txt`, which is the executable
inventory — it only shrinks):

| Frozen consumer | Close of Stage 3 | Now | Files |
|---|---|---|---|
| `cspace_or_handle_resolve_` | 104 | 107 | 17 |
| `handle_table_get_object` | 52 | 37 | 14 → 9 |
| `handle_table_insert` | 42 | 41 | 14 → 13 |

`cspace_or_handle_resolve_` grew by 3 under charter §3 (ledger A-2): three
syscalls resolved their object arguments either way while their NOTIFICATION
argument stayed handle-only, so each trade swapped a handle-namespace consumer
for a dual one on the same argument.  The dual resolver's handle leg is deleted
wholesale when the namespace retires.

(`kslab_alloc` is Stage 6's inventory, not Stage 4's — the authoritative count
lives in `scripts/purity_allowlist.txt`, which the gate checks exactly, rather
than in a number here that drifts.)

### Step 4 — the CSpace root stops being a handle  ✅ DONE

`KProcess.cspace_root_h` (a `handle_id_t` into the process's own handle table)
became `KProcess.cspace_root` (a `struct KCNode *`, holding the same lifecycle
+ active ref pair, released in `kprocess_teardown`).  This removed the
namespace inversion at the base of the whole stage: **every** CPtr resolution
began by looking the root up in the namespace CSpace was built to replace.  It
also ended cross-process handle-table access — `SYS_CSPACE_MINT_INTO`,
`SYS_PROC_CSPACE_MINT`, retype2's `dest_cnode == 0`, `SYS_CNODE_DELETE` and IPC
receive-slot delivery all read the target's root structurally now.

Two userspace consequences, both retiring guesswork rather than adding API:

- `SYS_CNODE_MINT` accepts `arg0 == 0` = "my own root CNode", the convention
  `SYS_CNODE_DELETE` and `SYS_UNTYPED_RETYPE2` already used.  svcmgr and
  `iris_test` used to *probe their own handle table* for the first CNode-typed
  generation-1 id, which only worked because the kernel published the root as
  every process's first handle.  Both probes are deleted.
- userboot's two liveness-only CPtr probes are deleted, and its two founding
  mints for `init` now take CPtr sources (`SYS_CSPACE_MINT_INTO`) instead of a
  `SYS_HANDLE_DUP` + `SYS_CSPACE_RESOLVE` pair — so init's founding caps are
  installed as MDB children of userboot's slots, i.e. revocable, instead of
  handed over forever.

### Step 5 — the productive path leaves the bridge  ✅ DONE

`init` has ZERO uses of the CPtr→handle bridge and zero `SYS_HANDLE_DUP`: its
bootstrap authority, every object it fabricates, its death watch and its
selftest notifications are capabilities in slots.  `svcmgr` is down to one use
(the delivered-cap path, which is the IPC-delivery-without-receive-slot legacy,
not svcmgr's to fix).  `userboot`, `vfs`, `kbd`, `console` and `sh` were already
clean.

Four defects surfaced, none of them the migration's own:

- `SYS_BOOTCAP_RESTRICT` resolved its argument either way but published the
  restricted clone with a handle-table write — a CPtr could never succeed.  It
  derives into a destination slot now, as an MDB child of the source.
- KDEBUG was **ambient**: `SYS_KLOG_DRAIN`/`SYS_SCHED_INFO`/`SYS_POWEROFF`
  scanned the caller's handle table instead of taking a capability.  Retired
  (ledger A-3); it is Stage 5 groundwork, not Stage 4 cleanup.
- Three syscalls (`SYS_PROCESS_WATCH`, `SYS_EXCEPTION_HANDLER`,
  `SYS_IRQ_ROUTE_REGISTER`) had their object arguments migrated and their
  NOTIFICATION argument left behind.  **Any syscall taking a notification
  beside an already-dual argument should be assumed to have this gap until
  checked** (ledger A-2).
- The pager's manifest oracle leaked one handle-table entry per occupied slot,
  per request, with no ceiling.

### Step 6a — CSpace-native introspection  ✅ DONE

The bridge had two distinct users, and only one of them was a test.  Every
remaining PRODUCTIVE use of `SYS_CSPACE_RESOLVE` was asking one of two
questions about a slot the caller already named — *what type is this
capability* (svcmgr, dispatching on a delivered cap) and *is this slot
occupied* (the `pager` and `lifecycle_probe` manifest oracles).  Both were
answered by materialising the slot into a handle and immediately closing it:
asking for authority to learn a fact, and consuming a handle-table entry per
occupied slot on every request.

`SYS_CAP_IDENTIFY` (117) and `SYS_CAP_SAME_OBJECT` (118) answer those two
questions natively — CPtr only, no handle produced, nothing retained past the
call, no dual resolution and no fallback.  They are the CSpace-native
successors of `SYS_HANDLE_TYPE` (52) and `SYS_HANDLE_SAME_OBJECT` (53), which
now retire *with* the namespace instead of blocking it.

This reverses the previous conclusion that the oracles "retire WITH the bridge
rather than before it".  That held while the only way to ask was to mint a
handle.  It does not hold against a primitive that is strictly weaker than the
bridge: identify returns a scalar where resolve returned authority.  Observing
which of your OWN slots are occupied is not a leak — a caller learns the same
thing by invoking any slot and reading `NOT_FOUND`, in IRIS and in seL4 alike,
and a CSpace's layout is not a secret kept from its owner.

Result: **svcmgr, `pager` and `lifecycle_probe` are off the bridge.**  The
suite is now the only consumer left, which is what Step 6b addresses.
Covered by T292/T293 (type per family, no right required, empty slot is
`NOT_FOUND`, identity survives rights reduction and badging, handle value is
`INVALID_ARG` on every argument).

### Step 6b — CSpace stops being ten bits wide  ✅ DONE

Two mechanisms still assumed the pre-`HANDLE_TAG` world, and both of them
capped what a CSpace could be:

**The IPC receive slot was a direct index into the root CNode.**  Declaration
and delivery both open-coded `slot < 1024` and installed straight into
`proc->cspace_root[slot]` — no traversal.  A process whose root CNode is full
therefore could not receive a capability at all, which is not a hypothetical:
this suite's root is ~97% allocated, which is why its fabricated objects
already live in a second-level CNode.  The declaration is a full CPtr now,
resolved by `cspace_resolve_dest_slot` — the destination analogue of
`cspace_resolve_slot`, which allows the terminal slot to be EMPTY (that is the
normal case for an install target) while requiring every intermediate level to
really be a CNode.

**The delivery discriminator was the literal 1024.**  `iris_msg_cap_is_cptr`
was written when a handle was `slot | gen << 10` and so always ≥ 1024.  Handles
carry bit 31 now and CPtrs own the low 31 bits, so a two-level CPtr such as
`(leaf << 8) | 80` — 64080 for leaf 250 — was classified as a *handle* by every
consumer of that helper.  `IRIS_CPTR_LIMIT` is `HANDLE_TAG` now and agrees with
`CSPACE_DIRECT_CPTR_LIMIT`, which `nc/cspace.h` already declared to be the one
definition of the boundary.

**A CPtr addressed more than one capability.**  Resolution consumed radix bits
per level and treated a slot as terminal when the CPtr was exhausted *or* the
slot held a non-CNode — the second clause silently DISCARDED the leftover bits.
With a 256-slot root, CPtr `k`, `k+256`, `k+512` … all resolved to slot `k`:
roughly 2^23 aliases per capability.  A capability address space whose
addresses are not injective cannot be reasoned about — an off-by-one in a
computed CPtr hits a live capability instead of failing, and a value chosen
*because* it is invalid may not be.  The suite's own fuzz constant 4095 aliased
root slot 255, its serial `KIoPort`.  Leftover bits with nothing to descend
into are now `INVALID_ARG` on all three resolvers, which is how seL4 treats the
same shape (depth mismatch).

Covered by T294 (deliver into a second-level slot; the returned value is the
declared CPtr and is classified as a CPtr; occupied deep slot fails fast),
T295 (aliases rejected on invoke / identify / mint-source / receive-slot
paths), and host cases in `tests/kernel/test_cspace.c`.

### Step 6c — the test suite  ← REMAINING

`iris_test` is what keeps Stage 4 open, and it is not one migration.  All 268
tests classified against a single rule — a test whose SUBJECT is the handle
namespace dies with the mechanism; a test asserting an authority property that
survives in a CSpace-only kernel is rewritten, because the property is real and
only the vehicle changes:

| | tests | disposition |
|---|---|---|
| subject is the handle namespace | 57 | deleted WITH the mechanism, not before |
| use the bridge incidentally | 84 | rewritten in CSpace |
| already CSpace-only | 127 | untouched |

T011 (`SYS_HANDLE_TYPE`) and T012 (`SYS_HANDLE_SAME_OBJECT`) are the first
group: migrating them would leave them asserting nothing.  T019 is the second —
"dropping the last capability to an endpoint wakes a blocked receiver" is as
true in seL4, it was merely spelled `SYS_HANDLE_CLOSE`.

Migration is per-test and cannot be batched blindly: a sweep over every
`it_ep_create()` inside tests that also call `it_register_ep` broke eight at
once, because those tests use the same endpoint for other things and their
`it_close()` calls also close process and thread handles that are not moving.

It CAN be batched behind the runtime gate, which is how the bulk moved: convert
a group, run the suite, and read the failures as a map of what the group was
really doing.  Every failure so far was a place where an operation had a CSpace
form nobody had switched to — `SYS_CNODE_MINT` (handle-only source) where
`SYS_CSPACE_MINT` belonged, a handle-to-handle identity comparison where
`SYS_CAP_SAME_OBJECT` belonged, `SYS_HANDLE_DUP` where a slot-to-slot derive
belonged — not a place where the handle was load-bearing.  `it_cs_reduce` is
the last of those: the CSpace form of "a rights-reduced copy", which is also
strictly better than the dup it replaces, because the reduced cap is an MDB
child of its source and therefore revocable.

**Progress.**  Bridge uses inside `iris_test`, counted as occurrences of
`it_ep_create_h` / `it_notify_create_h` / `it_retype_handle` /
`SYS_HANDLE_DUP` / `SYS_HANDLE_TYPE` / `SYS_HANDLE_SAME_OBJECT` /
`SYS_HANDLE_CLOSE` / `SYS_CSPACE_RESOLVE` / `SYS_CNODE_MINT`:

| point | uses |
|---|---|
| close of Step 5 | 197 |
| after Step 6a/6b (lookups, liveness probes, self-proc, vestigial KDEBUG staging) | 178 |
| after the endpoint/notification fixture migration | 137 |
| after the retyped-object fixture migration | 118 |
| after the VMO fixture migration | 111 |
| after retiring the cross-process producers and legacy retype | 104 |
| after retiring handle delivery in IPC | 99 |
| after the loader workspace (processes and VSpaces born in CSpace) | 85 |
| after VMO/initrd/self-VSpace destinations | 81 |
| after the liveness, identity and second-holder probes | 60 |

What is left is dominated by fixtures the suite cannot yet fabricate into a
slot: `SYS_HANDLE_DUP` on VMO / KProcess / KVSpace / KFrame caps.  Those retire
as their CREATORS gain CSpace destinations — the same move the object-cap
accessors just made — not by rewriting the tests around them.

Two tests keep a handle deliberately, and are the pattern for the rest of the
"dies with the mechanism" group.  T073's third leg asserts that a HANDLE value
as an IPC transfer source is `INVALID_ARG` with no fallback, which needs a real
handle to hold wrong.  T127 and T130 assert that a copy made with
`SYS_CNODE_MINT` is an independent reference and NOT a derivation child, so it
survives a revoke of its source — that is the LEGACY_ROOT behaviour the ledger
tracks to zero, and rewriting it with `SYS_CSPACE_MINT` would assert the
opposite of what it exists to pin.  T125 now splits deliberately: it identifies
the four families with a CSpace birth through their slots and the two without
two through the handles the LEGACY `SYS_UNTYPED_RETYPE` produced.  That leg is
not an oversight: 87 is still live for `KFrame` / `KUntyped` / `KSchedContext`,
so something has to keep exercising it until it retires.  `RETYPE2` accepts
both types into a slot already, so that leg is a deletion when 87 goes, not a
rewrite.

Nothing remains of the bridge outside the suite: svcmgr's delivered-cap path
and the `pager` / `lifecycle_probe` manifest oracles all moved to
`SYS_CAP_IDENTIFY` in Step 6a.  Probing by attempting a mint was considered
and rejected first: it requires `RIGHT_DUPLICATE` on the source, which several
of those slots lack, so it would report absent for capabilities that are
present.

**Second-order benefit, not just hygiene.** The `<1024` split caps the whole
CPtr namespace at 10 bits.  A root CNode of 256 slots therefore consumes most
of the addressable space, and multi-level CSpace resolution — which
`cspace_resolve_slot` already implements as a radix walk — is effectively
unusable because only 2 bits remain for deeper levels.  The symptom is
concrete: `iris_test`'s root CNode is ~97% allocated, with six free slots
left, and three separate bring-up failures during Phase S4 were slot
collisions.  Removing the split frees the full 64-bit CPtr space and makes
real CSpace hierarchies possible.

## Stage 5 — seL4-like bootstrap  ✅ CLOSED

Precondition: Stage 4 (the initial caps can only be CSpace now).
Design: `docs/architecture/stage5-root-task-bootinfo.md`.

- Replace the monolithic `KBootstrapCap` with structured BootInfo.
- Root task with: root CNode, initial TCB, initial VSpace, IRQ control cap,
  ASID/PCID control, Untyped list, fine-grained per-device caps.
- TCB_CONFIGURE/TCB_WRITE_REGS (execution of retyped TCBs) is defined here
  because its arguments (CSpace root, VSpace, fault EP) now exist as caps.

**Closing criterion**: the root task receives a structured BootInfo and
fine-grained capabilities, with no monolithic bootstrap object left to
restrict, and the executing TCB is a retyped object configured through
capabilities.

### Step 1 — the root task is told what it holds  ✅ DONE

The kernel writes a structured BootInfo region (`struct iris_root_bootinfo`,
`kernel/include/iris/root_bootinfo.h`) describing the initial capabilities by
CPtr, the shape of the root CNode, and every boot Untyped with its physical
region; it maps the region read-only / non-executable into the root task and
passes its address in RBX — the register that carried a bootstrap HANDLE until
Stage 4 deleted that namespace and left it carrying 0.

What retires is GUESSING.  The root task used to know its capabilities by
compile-time constants shared with the kernel (`BOOT_CPTR_BOOTSTRAP_CAP`,
`BOOT_CPTR_UNTYPED_START`) and count its untypeds by invoking slots until one
answered `NOT_FOUND`; its one liveness "probe" invoked a slot and ignored the
answer.  userboot now validates the description against the CSpace it describes
— every untyped must answer from its slot with the physical region the page
claims — and halts the boot with a serial diagnostic on disagreement.

The page is not authority (charter §3.5): it is read-only and every CPtr in it
names a slot the kernel had already populated.  What bounds it is the converse
rule — a grant that cannot be described is not made, so the untyped drain stops
where the description stops, and the region is two pages so that "describable"
covers every one of a 256-slot root CNode's 240 untyped slots (static-asserted).

Covered by RBI-1..RBI-10 (`tests/kernel/test_root_bootinfo.c`) for the builder,
and by the boot itself for the contract: an unreadable or untrue BootInfo is
fatal in userboot, so a healthy `make smoke-runtime` is the runtime witness.

### Step 2a — device control is its own authority  ✅ DONE

`IRIS_BOOTCAP_HW_ACCESS` — one bit authorising BOTH interrupt-line and I/O-port
capability creation, on an object that also carried spawn, debug and
framebuffer authority — is replaced by two capabilities the kernel matches
EXACTLY.  init printing a boot line to COM1 no longer holds the authority to
claim any IRQ, spawn processes and power the machine off.

Each is published into its own root-CNode slot, recorded in BootInfo v2, and
delegated down the chain as a CPtr source (so every grant is an MDB child of
the granter's slot and stays revocable).  svcmgr renounces hardware authority
by DELETING those two slots once it has claimed the catalog's devices —
previously a `SYS_BOOTCAP_RESTRICT` derive-then-delete whose first half was
load-bearing only because the authority was a bit on a shared object.

Covered by T296 (each control capability authorises its own syscall, neither
authorises the other's, the capability they were split from authorises
neither, an empty slot authorises nothing); T069 and T291 re-anchored.

### Step 2b — debug is its own authority  ✅ DONE

`IRIS_BOOTCAP_KDEBUG` — kernel-log drain, scheduler statistics, poweroff — is
now a capability of its own (`BOOT_CPTR_DEBUG_CONTROL`, BootInfo v3), matched
exactly and delegated to the two processes that use it: svcmgr and the suite.
The child-side slot reuses the retired `IRIS_CPTR_SVC_REPLY` constant, dead
since KChannel was removed, because root CNodes are 256 slots and the suite's
is full.  T296 gained a third leg; T291's oracle moved to the framebuffer bit.

### Step 2c — the monolith is gone  ✅ DONE

The last three authorities split: `SPAWN_SERVICE` became TWO capabilities
(process control and initrd control — one bit was authorising both spawning a
service and reading a boot image, which is why vfs, a file server, held the
authority to create processes), and `FRAMEBUFFER` became the framebuffer
control capability.

`SYS_BOOTCAP_RESTRICT` (45) is RETIRED with its number reserved, and the
monolith is unrepresentable rather than merely unused: `kbootcap_alloc` refuses
a zero or multi-bit kind, every kernel check is exact equality, and
`kbootcap_allows` / `kbootcap_clone_restricted` are deleted.  Slot 1
(`BOOT_CPTR_BOOTSTRAP_CAP`) stays reserved and permanently empty.

The loader API carries the split into userland: `svc_load_minted_ws` takes a
process capability and an initrd capability, so "can read images, cannot spawn"
is expressible in the signature.  T291 died with its mechanism (its subject was
the retired syscall); T148 pins 45; T296 covers what replaced it.  Suite:
269/269.

### Step 3 — the root task's own objects  ✅ DONE

The root task holds capabilities to its own root CNode and its initial thread
(`BOOT_CPTR_CNODE`, `BOOT_CPTR_TCB`, BootInfo v5), validated by userboot.  The
one process those objects belong to was the only one that could not name them:
the CNode was reachable structurally plus the `arg0 == 0` convention, the
thread only through `SYS_TCB_SELF`.

The self-capability makes the CSpace reachable from itself, so
`kprocess_teardown` empties the root CNode's slots before dropping its
references — a cycle cannot be collected by a refcount the cycle is holding up.
BC-11..BC-13 pin it, negative control included.  ASID/PCID control is
deliberately NOT added: no operation exists for it to authorise until VSpaces
are retyped from Untyped (Stage 6).

### Step 4 — a retyped TCB executes  ✅ DONE

`RETYPE2(KOBJ_TCB)` produced inactive threads from Phase S2 onward; what was
missing was not code but ARGUMENTS — a thread runs in a CSpace and a VSpace,
and neither was addressable as a capability until Stages 3–5.  Three CPtr-only
syscalls close it: `SYS_CSPACE_SELF` (119, a capability to the caller's own
root CNode — the CNode counterpart of `SYS_TCB_SELF`), `SYS_TCB_CONFIGURE`
(120) and `SYS_TCB_WRITE_REGS` (121).

`SYS_THREAD_CREATE` (48) is RETIRED with its number reserved: it carved a
thread from the kernel's static pool and returned a global thread id — no
capability authorised it, no Untyped paid for the storage, and the identity was
an array index (charter §3.4/§3.5).  Every in-tree thread is now retyped,
configured with capabilities, and started; creation returns a capability in a
slot.  `SYS_THREAD_START` (a spawned process's FIRST thread) remains the last
pool-born execution path and is Stage 7 work — a spawner cannot yet name its
child's CSpace and VSpace.

Two lifecycle defects surfaced and were fixed: the kernel stack was keyed by
the task's position in the static pool (a retyped TCB has none — it is recorded
per task now and keyed by the registry slot), and teardown released the
registry slot before freeing the stack, so a new thread could map its stack
over a range the dying one still unmapped afterwards.  Covered by T297 plus
every threaded test in the suite.

**Stage 5 closing criterion met**: the root task receives a structured BootInfo
and fine-grained capabilities, no monolithic bootstrap object remains to
restrict, and the executing TCB is a retyped object configured through
capabilities.

## Stage 6 — Remaining memory and objects  ✅ CLOSED

Precondition: Stage 1 (ownership/derivation); may overlap with 5.
Design: `docs/architecture/stage6-memory-from-untyped.md`.

- Page-table objects retyped from Untyped (retires the paging_map PMM
  reserve).
- Canonical VSpace from Untyped; Frame headers inside the region.
- Retire the remaining object kslab paths (ledger list).
- Convert or retire KVMO; separate file-backed and anonymous memory in user
  services (the pager/VFS already provide the base).

**Closing criterion**: no kernel object and no page of user-visible memory is
created from kernel-private storage.  Stage 5 finished the authority story;
this stage answers *who pays for memory*, which is still "the kernel,
invisibly" in four places — page tables on map, frame headers, the VSpace and
its PML4, and sixteen `kslab_alloc` consumers.

### Step 6 — the last runtime allocations  ✅ DONE

Mapping records (one per mapped page, recycled through a per-VSpace free list),
VMO page frame headers and device capabilities — the three paths that still
reached the kernel slab on every use — are charged to a budget.

The purity gate refused the first attempt, correctly: routing mapping records
through the VSpace MOVED a `kslab_alloc` from one file to another, and the
allowlist may only shrink.  Removing it instead — the root task, the one
address space with no budget, uses a fixed 64-entry bootstrap arena — made
`scripts/purity_allowlist.txt` shrink for the first time since Stage 4.

**Stage 6 closing criterion met**: no kernel object and no page of user-visible
memory is created from kernel-private storage after boot.  What remains on the
slab is the root task (built before any Untyped exists) and subsystems that
retire whole in Stage 7 (KVMO, the initrd store, the boot authority); ledger
D-5 records the divergence that stays — IRIS CHARGES these objects to a budget
where seL4 has the user RETYPE them.

### Step 5 — user memory comes out of a named budget  ✅ DONE

A VMO's pages, its page-address array and its header come from an Untyped, and
`SYS_VMO_CREATE` / `SYS_INITRD_VMO` take that budget as a CPtr — a process
holds several and they are not interchangeable.  Anonymous memory was the last
allocation obtainable without a capability behind it.

Charging alone would have made consumption monotonic (a bump allocator does not
rewind), so reclamation is part of the step: the loader recycles a budget per
LIVE child and a scratch budget for image copies, bounding cost by what is
alive rather than by what has ever run.  Covered by T300.

### Step 4 — a process's kernel state comes out of the budget  ✅ DONE

`KProcess`, the child's 256-slot root CNode (the largest single per-process
allocation) and a sub-untyped's own header are carved from the budget instead
of the kernel slab.  The last one closes a circularity: delegating a budget
used to cost kernel memory, because a sub-untyped took its region from the
parent and its header from the slab.

What stays kernel-funded is the root task (built before any Untyped exists) and
the boot Untypeds (created from raw PMM blocks, with no parent to charge).

### Step 3 — the address space itself comes from the budget  ✅ DONE

The PML4 and the KVSpace header follow the page tables into the Untyped: one
budget pays for a whole address space, and a spawn that names none builds
nothing.  A pooled PML4 is never returned to the PMM (the page belongs to the
Untyped), and teardown returns page children, then the header block, then the
pool retain — in that order, because the header block lives in the region the
pool owns.  The root task keeps the kernel-funded path: its address space is
built before any Untyped exists.

### Step 2 — page tables are charged to a budget  ✅ DONE

Mapping user memory needed page tables and took them from the kernel's PMM
reserve: unbudgeted, unauthorised, and drivable from ring 3 by mapping at
scattered addresses.  Every user address space now names the Untyped that pays
for its levels at `SYS_PROCESS_CREATE` — required, `RIGHT_WRITE`, retained by
the VSpace — and the carve fails rather than falling back to kernel memory.
Each table counts as a child of that Untyped, so `SYS_UNTYPED_RESET` cannot
reclaim a region whose pages are somebody's live page tables; the address space
returns them all at teardown.

Kernel mappings and the root task (built before any Untyped exists) stay
kernel-funded — bounded and stated, like the idle task.

Two pre-existing defects surfaced and were fixed: page-aligned carves aligned
the offset rather than the absolute address (a frame retyped from a sub-untyped
got a paddr the mapper masked DOWN, overlapping earlier carves), and a process
created but never started could not be reclaimed (kill found no threads and
dropped nothing, pinning its address space forever).  Covered by T299.

### Step 1 — the Untyped pays for its objects' headers  ✅ DONE

A frame retyped from an Untyped carved its PAGE from that Untyped and its
header from the kslab heap: the caller paid for the page, the kernel quietly
paid for the rest.  `KUntyped` now carves from **both ends** — page-aligned
regions from the bottom (`used`), object headers from the top (`used_top`) —
and a retyped frame's header is a child block of the same Untyped.

The direction is the point: a 160-byte header taken from the bottom would push
the next page-aligned carve onto the following page and cost almost 4 KiB per
frame.  From the top it costs its own size, and consecutive frames stay
page-dense.  The header is also, structurally, never inside the frame's own
page — that page is mapped into ring 3, where kernel bookkeeping would be
readable and writable by the process that received it.

`child_count` accounting is unchanged in shape (one child per frame, held by
the header block as for every other retyped family), and `SYS_UNTYPED_RESET`
reclaims both ends.  Frames with no Untyped to charge — VMO pages and a
spawning process's bootstrap frames — keep their kslab header and are what
Etapas 3 and 5 retire.

Covered by UT-TOP-1..5 (`tests/kernel/test_kuntyped.c`) and T298 (a frame costs
page + header out of one Untyped, four frames stay page-dense, and the frame
still maps, reads clean and writes).

## Stage 6-pure — the user retypes what the kernel charged  ✅ CLOSED

Stage 6 closed on "no kernel object and no page of user-visible memory is
created from kernel-private storage".  That answered *who pays*.  It did not
answer the question seL4 answers, which ledger D-5 records: the kernel still
decided *when* each object existed and *where* it went.  A holder paid for
page tables it could not name, count, delegate or reclaim.

**Closing criterion**: every object the kernel charges to a budget today is
instead RETYPED by the holder and installed by an explicit invocation, or the
row is argued down to something that cannot be user-driven.

### Step 1 — the page table becomes a capability  ✅ DONE

`IRIS_KOBJ_PAGE_TABLE` is retyped from an Untyped like every other object; its
4 KiB region IS the hardware table, and its header is a top-carved block of the
same Untyped (a header inside the region would be walked by the MMU).
`SYS_VSPACE_MAP_TABLE` installs it — seL4's `seL4_X86_PageTable_Map` — and the
kernel's only contribution is the walk, because which level is missing for an
address is a fact about the address space rather than a choice the holder
makes.

The VSpace retains every table installed in it and returns them at teardown,
so a region cannot be RESET while a live walk stands on it — the guarantee
`child_count` gave the charged path, now carried by a real capability.  The
address is validated as authority: every user address space shares the higher
half with the kernel, so an install outside the user private window is refused
rather than spliced into the kernel's own walk.

Three paging primitives that allocate nothing back this: report the missing
level, install a supplied table at it, and map a leaf without ever creating a
level.  Tests: T302 (new) — the object, its one-page size rule, one level per
invocation, double-install refused, kernel-address refused, the budget charged,
and a frame mapped through the walk the holder built.  T148/T251 (the syscall
surface and canonical-type manifests) grew by one member each.

### Step 2 — userland supplies its own levels  ✅ DONE

The kernel no longer creates page tables for any address space whose holder
has a budget.  A map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE`
and names nothing else; the holder retypes a level, installs it, and retries.
The one address space still mapped the old way is the root task's, built
before any Untyped exists — the documented bootstrap exception, and now the
only implicit page-table allocation left in IRIS.

Userland gained one client-side rule, in one place (`services/common/
iris_vspace.h`): ask, be told exactly which level is missing, supply it, retry.
It is a retry and not a pre-pass because how deep a walk already is depends on
what the address space mapped before — two addresses a gigabyte apart can share
a PDPT.  Services apply it at their syscall wrapper rather than at each map
site: iris_test alone has 111 of those, and it is one rule about the address
space, not 111 decisions.

**A task that maps must hold a budget.**  That is the real content of this
step and it fell out of the design rather than being chosen: retyping a level
needs untyped memory, so a service that maps needs some.  `svc_load_minted_ws`
takes the SLOT to mint the child's own address-space budget into, and the
spawner decides — vfs and the pager get one, console and kbd do not, and
lifecycle_probe gets one only in the spawn where it acts as a pager.  The same
image, two roles, different authority: what a task may do follows from what it
was handed.  The authority-audit tests (T162/T177/T201/T215/T217) caught every
place this was granted too widely, which is what they are for.

A SLOT and not a flag because there is no number free in every image: 12 is
lifecycle_probe's target-process capability, 16 is init's vfs.ep receive slot,
29 is the suite's scratch pool, 23 is a pager target slot.  A 256-slot
namespace shared by nine images has per-image maps, not spare numbers.

Two kernel-side corrections the client loop forced: installing a table that is
already installed answers `BUSY` while a walk that is already complete answers
`ALREADY_EXISTS` — one code for both left a client unable to tell "this object
is spent" from "there is nothing to do" — and `SYS_VSPACE_MAP_TABLE` resolves
its VSpace argument with the same resolver and right as `SYS_VMO_MAP_PAGE`, so
a pager holding exactly what it needs to map can also supply what mapping now
requires.

Tests: `tests/kernel/test_pagetable.c` (new, PT-1..PT-7) drives the walk
exhaustively on the host, where level modelling is opt-in so the suites that
predate paging levels keep testing what they were written for.

### Step 3 — the bootstrap exception ends  ✅ DONE

`pt_pool` was answering three different questions at once — is this map strict,
was the PML4 pooled, and where do mapping records come from — which is why the
root task could not be strict without also being charged, or charged without
also being strict.  Three facts, three fields now: `kernel_funded`,
`pml4_from_pool`, and `pt_pool` for storage only.  `pt_count` retires with the
kernel-carved tables it counted; since the kernel stopped carving them it only
ever held 0 or 1, and that 1 was the PML4.

The exception is now bounded to what genuinely has no alternative: the root
task's text, stack and BootInfo, mapped before it exists.  The moment it can
speak for itself — it holds boot untypeds and a capability to its own VSpace —
`kvspace_end_bootstrap` ends it, and the first boot block becomes its own
budget.  From that line on **no address space in IRIS is implicitly funded
while anybody is running.**

Verified rather than assumed: instrumenting the kernel-funded path shows
exactly six carves, all before the root task runs (three levels for its text,
two for its stack, one more for the BootInfo window) and none afterwards.  The
same suite passing without that check would have looked identical if the
exception had never ended, which is why the check was worth making.

The bootstrap arena stops being a guess.  It was sized 64, then 512, both
times for an estimate of what the root task maps; it now serves only the
pre-boot maps, every one of which is registered in `KProcess.bootstrap_frames[]`
— a capped array — so its bound is that cap plus a margin, and a static assert
holds it there.

Tests: PT-8 (host) pins the exception's shape — kernel-funded is the root
task's constructor and nothing else's, and ending it is one-way.  It is worth
a test because the flag is invisible from userland: a kernel that kept funding
the root task forever would look exactly like one that stopped.

### Step 4 — the address space is retyped, not carved  ✅ DONE

`SYS_PROCESS_CREATE` used to take a BUDGET and build an address space out of
it — carving a PML4, a KVSpace header, and every level underneath.  The holder
paid for a walk it could not name until the process existed, could not inspect,
and could not have built differently.

It takes an ADDRESS SPACE now.  `IRIS_KOBJ_VSPACE` is retyped from the caller's
own Untyped like every other object — the PML4 from the bottom, the header from
the top, exactly as a page table is carved, because the top level of a walk is
a page like any other.  What makes it a VSpace rather than a `KPageTable` is
what the kernel writes into that page (`paging_init_user_pml4`: the shared low
window and the higher half) and the bookkeeping the header carries for the
levels the holder will hang under it.  A process is COMPOSED from objects its
creator made.

Two consequences worth naming.  The Untyped a VSpace was retyped from becomes
its pool, so an address space's mapping records come from the region the
address space itself lives in — one region per child, and RESETting it returns
all of the child with no second budget to remember.  And binding is one-way and
exclusive (`IRIS_ERR_BUSY`): teardown is per-process, so two processes sharing
a walk would each tear down the other's.

The teardown order had to be corrected with it.  The PML4 is the VSpace
object's own storage now, so releasing the VSpace can be what returns that
page's accounting to its Untyped — and walking `cr3` afterwards would be
walking a region whose holder has already been told it is free to RESET.  The
walk is destroyed first and the object released after: the reverse of how it
reads, and the only order that is true.

T301 moved with the carves it pins.  Its target — a budget large enough for one
carve and not the other — is now in `retype_vspace`, and it no longer needs the
sub-page sweep it was built around: these two carves differ by a page, so the
band where one fits and the other does not is a page wide and a one-page budget
lands in it by construction.  Verified to fail (`refused retype left a child`)
against the reversed carve order.

### Step 5 — the CSpace is retyped too  ✅ DONE

The root CNode was the other half of a process's kernel footprint, and the
bigger one: 256 slots with their MDB links, carved by the kernel at a width the
kernel picked for everyone.  `SYS_PROCESS_CREATE` takes it as an argument now,
retyped by the spawner alongside the address space — which is the shape seL4
gives `seL4_TCB_Configure(tcb, cspace_root, vspace_root)`, and which also means
the spawner chooses how wide its child's CSpace is.

Binding is exclusive for the same reason it is on a VSpace, and the reason is
worth stating because it is not symmetry for its own sake: `kprocess_teardown`
empties a root's slots before dropping its refs, because a CSpace may name
itself.  A CNode shared by two processes would have its slots emptied by the
first one's death, out from under the second.

`svc_loader` retypes both objects from the child's budget before the spawn, so
a child now costs exactly one region and one syscall's worth of composition.

### Closing criterion — met

**Every object that constitutes an address space or a CSpace is retyped by its
holder and handed over.**  The kernel creates none of them — not a page table,
not a PML4, not a CNode — and a map that needs a level says so instead of
quietly making one.

What the kernel still funds, and why each is not a gap this stage could close:

| still kernel-funded | why |
|---|---|
| the ROOT TASK's KProcess, root CNode and VSpace (`kslab_alloc` ×3, the purity allowlist's floor) | built before any Untyped exists; there is no holder to ask |
| the KERNEL's own address space (kstack region, physmap) | it has no holder at all |
| the root task's pre-run maps — six levels, measured | mapped before it exists; ended by `kvspace_end_bootstrap` the moment it can speak |

What is still CHARGED rather than retyped, and needs a later stage:

- the `KProcess` object itself, and VMO pages, metadata and mapping records —
  charged to a budget the holder named, which is Stage 6's answer, not seL4's.
  (`KProcess` was FROZEN here and expected to retire with a process server;
  Stage 7-proc **deleted** it instead, and no server was needed.)
- frames, IRQ handlers and I/O ports having a kernel object at all.  That is a
  change to what a capability IS, not to who pays for it, and it retires with
  the memory server.

Ledger D-5 records both, and is no longer a single row about who pays: the half
about who CREATES is closed.

## Stage 7 — KProcess retirement  ✅ CLOSED

Precondition: Stages 5–6 (a process = TCB+CSpace+VSpace composition).  Met by
Stage 6-pure: a spawner retypes its child's address space and CSpace and hands
both over.

Stated at the time as: *process server in user space; process creation and
policy outside the kernel; PID stops conferring authority; per-domain quotas
become the process server's policy.*

Three of those four landed, and the fourth turned out to be unnecessary:
process creation and policy ARE outside the kernel, a PID confers no authority
(the global thread lookup is deleted), and the per-domain quotas are not the
server's policy — they are **gone**, because a budget is a capability.  The
server itself was never built; see Stage 7-proc.

### Step 1 — the last pool-born thread retires  ✅ DONE

`SYS_THREAD_START` carved a spawned process's FIRST thread out of the kernel's
static task pool.  It outlived `SYS_THREAD_CREATE` by two stages for one
recorded reason — a spawner could not name the CSpace and VSpace its child
would run in — and Stage 6-pure removed that reason.  58 answers
`NOT_SUPPORTED`, `task_thread_create` is deleted, and `svc_loader` composes the
child's first thread the way any thread is composed: retype the TCB from the
child's budget, configure it with the child's CSpace and VSpace, write its
registers, resume it.  **No path remains by which a thread exists because the
kernel had a free slot**; the root and idle tasks still come from the pool,
both built before any Untyped exists.

The path exposed a lifecycle bug latent since Phase S2: `ktcb_configure` never
took the scheduler's EXECUTION reference on a retyped TCB, so the CSpace slot
was the object's only owner and deleting it freed a running thread's storage.
Invisible while the only caller kept its slot forever; a spawner does not.
T303 pins it and reproduces the page fault when the reference is removed.

### Step 2 — a ceiling nobody granted  ✅ DONE

The per-process PAGE quota retires.  Stage 6 Step 5 moved VMO pages onto a
named budget precisely because they had been "bounded only by a per-process
quota the kernel invented"; the quota was then left standing beside the budget
that replaced it, and since Stage 6-pure it contradicts the model — a holder
handed a large Untyped still stopped at 8 MB nobody granted.  `pages_limit`
reports 0, as the notification quota has since Phase S1; the counters remain as
instrumentation.

### Step 3 — a ceiling nobody granted, again  ✅ DONE

`KPROCESS_MAX_LIVE` (64) retires.  It was the same class as the page quota of
Step 2 and the notification quota of Phase S1 — a number the kernel invented,
refused at, and could not be asked to raise — and since Stage 6 Step 4 a
spawned `KProcess` and its root CNode are child blocks of an Untyped its
creator NAMED, so the memory somebody delegated already bounded how many could
exist.  Refusing at 64 on top of that told a holder with a large budget it had
run out when it had not, and a holder with a tiny one nothing at all.

What bounds a spawn now is derived rather than declared: the creator's Untyped
(which pays for the KProcess header, the root CNode, the PML4 and every level,
out of a bump allocator that does not rewind, so the holder can measure it),
`TASK_MAX` for a process that runs a thread, and the PCID pool for a process
that needs an address-space tag.  `kprocess_live_count()` survives as
instrumentation, exactly as the retired quotas' counters did.

The two things the entry above said retiring the number would require were
re-derived rather than deleted:

- the PCID allocator's exhaustion branch was commented "cannot happen with
  KPROCESS_MAX_LIVE=64".  It is now reachable, so it is documented as the real
  hardware bound and its unwind is exact — the block goes back to the Untyped
  and the gauge was never bumped, because the gauge moved to the end of a
  successful construction and the reserve-then-roll-back dance (which existed
  only to close a TOCTOU on the ceiling check) is gone with the check.
- T240 claimed to show "the real ceiling is the documented KPROCESS_MAX_LIVE",
  which it never measured — it caps at 48, below the number that refused.  It
  now asserts the property that survives whichever bound is reached first, and
  T304 pins the retirement directly: more than 64 live processes out of one
  budget, a clean error whenever the budget does run out, and a RESET of that
  region afterwards, which only succeeds once every KProcess, root CNode,
  VSpace header and PML4 has gone back.

### Step 4 — a thread resolves CPtrs in its own CSpace  ✅ DONE

`SYS_TCB_CONFIGURE` has taken the CSpace as a CAPABILITY since Stage 5 Step 4,
and the kernel then resolved every CPtr through `t->process->cspace_root`
anyway: the argument described the truth without being it, and a thread's most
basic authority — what its capability addresses mean — was a property of a
shared object it did not hold.  Every CSpace resolver took a `struct KProcess *`
and used it for one thing, to reach `proc->cspace_root`; they take the root
itself now, and the capability travels from the syscall into the thread, which
holds the same lifecycle+active pair KProcess holds.  Threads of one process
still share one CNode, so what a CPtr resolves to is unchanged; **resolving one
no longer reads KProcess**, which was most of what KProcess did on the hot path.

### Step 5 — a thread runs in its own address space  ✅ DONE

The same defect on the other capability `SYS_TCB_CONFIGURE` names: the
scheduler loaded CR3 out of `chosen->process`.  The thread holds the VSpace
now.  The PCID moved with it — a PCID is x86's ASID, it tags TLB entries with
the WALK they belong to, and it was allocated per KProcess out of a pool whose
allocation loop was written twice, once in each KProcess constructor.  It is
one loop on `KVSpace` now, claimed by whichever constructor established cr3.
`KProcess.user_cr3` and `KProcess.pcid` are gone; `cr3` remains as an explicit
cache for the teardown gate and the reap, and says so.

### Step 6 — a fault belongs to the thread that took it  ✅ DONE

The fault record lived on KProcess, one copy per process, and the code named
the cost in its own comment: "the per-process record is last-writer-wins".
Two threads faulting before the handler ran left one record describing the
other's vector, rip and CR2; the Phase 25 generation counter made the RESUME
safe against that but could not make the READ safe, because there was one
record.  It is on the execution now.  KProcess keeps what is genuinely
process-scoped — the handler to signal, the generation sequence, and a RETAINED
reference to whoever faulted last so the process-scoped read still answers.

### Step 7 — a fault names the thread by capability  ✅ DONE

`SYS_EXCEPTION_RESUME` took a task id.  Authority came from the process
capability and the id was checked against it, so the number conferred nothing —
but it SELECTED, which charter §3.4/§3.5 forbid, and a supervisor could not
hold, delegate or revoke "that thread" the way it holds everything else.

Closing it needed fault DELIVERY to hand a TCB capability over, and the first
attempt at that was reverted because it delivered into the registrant's own
CSpace.  **The principal that REGISTERS a fault handler is not the principal
that HANDLES the fault**: `iris_test` arms each target's handler and mints the
target's process capability into the PAGER, which is what answers.  Delivering
into the registrant's CSpace puts the capability where nobody reads it.

So the registration NAMES the destination: `SYS_EXCEPTION_HANDLER` gained a
`dest` in `cnode|slot<<32` form, whose CNode half is resolved in the
registrant's CSpace.  A supervisor delivers into a mailbox CNode it shares with
the handler; a handler arming its own faults names its own root.  The kernel
picks neither — which is the point, because which arrangement is right is
supervision policy and is what this stage exists to move out.

Three properties fell out of building it, each of which the tests now pin:

- **The mailbox delegates nothing.**  A delivered capability carries
  `RIGHT_READ | RIGHT_WRITE` and no `DUPLICATE` or `TRANSFER`: it is the
  authority to answer the fault, not to pass the thread on.  T144 asserts the
  capability cannot be minted.
- **Delivery precedes the signal.**  A handler woken by the notification finds
  the mailbox already filled; the other order is a race a handler could only
  paper over by retrying.
- **Re-aiming carries the outstanding fault.**  Re-registering with the same
  notification moves the destination and re-publishes the fault currently in
  flight.  Without it a supervisor taking over from a DEAD handler is told a
  fault is pending and holds nothing to resolve it with — a deadlock dressed as
  a working restart, which is exactly what the pager-restart tests
  (T204/T209/T210) reproduced.

`kprocess_fault_clear` takes the thread its caller already resolved rather than
comparing ids, and `task_find_by_id` is deleted with its last caller — nothing
in the kernel turns a number into a thread any more.

### Step 8 — a fault handler holds nothing but the thread  ✅ DONE

Step 7 left one reason a handler still needed a PROCESS capability: reading the
record.  `SYS_TCB_FAULT_INFO(tcb_cptr, out)` reads it off the thread, with
`RIGHT_READ` on that thread as the whole authority — so the pager's manifest
drops the target process capability entirely.  **A pager now holds no authority
over the processes it serves**: it maps (their VSpace) and answers faults (the
threads it is handed), and neither names a process.  The manifest oracle's bit
20 is gone from every expectation, which is the assertion that it is really
gone rather than merely unused.

`SYS_PROCESS_FAULT_INFO` is KEPT, and the first attempt at this step retired it
— wrongly.  "What faulted last in this process" is a different question asked
by a different principal: a SUPERVISOR watching a child it does not resolve
for, holding `RIGHT_READ` on the process and no capability to any of its
threads.  `iris_test` is exactly that supervisor for every pager suite, and
retiring the process view left it unable to ask.  Two operations on two
objects, each authorised by a capability to the object it names, is not the
dual-namespace shape the charter forbids — it is what having two objects means.

### Step 9 — a supervisor names the thing, not the process holding it  ✅ DONE

Three operations reached into another task by naming its PROCESS and letting
the kernel read the real target out of it: `SYS_VMO_MAP_INTO` (→ `proc->vspace`),
`SYS_PROC_CSPACE_MINT` and `SYS_CSPACE_MINT_INTO` (→ `proc->cspace_root`).  So
a caller that already held the address space or the CSpace it meant had to hold
authority over the whole process as well, and the process capability was doing
nothing but carrying a pointer to something the caller was entitled to name
directly.

All three now name the target:

- `SYS_VMO_MAP_INTO(vmo, vspace_cptr, vaddr, flags)` — the shape
  `SYS_VMO_MAP_PAGE` and `SYS_FRAME_MAP` have had since Phase 25/26.
- `SYS_PROC_CSPACE_MINT` (104) and `SYS_CSPACE_MINT_INTO` (116) RETIRE.
  `SYS_CSPACE_MINT` has taken a destination CNode since Phase S3, dest_cnode 0
  meaning the caller's own root; minting into a child is the same call with the
  child's root CNode as the destination.  **Whether a mint is cross-task is a
  fact about which capability is in `dest_cnode`, not about which syscall is
  called.**

A spawner HAS both: it retyped the child's VSpace and CSpace (Stage 6-pure
Steps 4/5) and handed them to `SYS_PROCESS_CREATE`.  `svc_load_minted_ws` gained
`keep_cnode_dest` so a spawner that means to keep delegating keeps the root,
and one that does not holds no authority over its child's namespace at all —
a distinction the process-shaped forms could not express, because holding the
process WAS holding the CSpace.

The tests re-derived rather than lost their subjects.  "A dead destination
fails" was a property of naming a process; a CNode outlives the process whose
root it was for as long as somebody holds it, so the mint now lands in a CSpace
no thread resolves in — and what teardown actually guarantees is asserted
instead, which is stronger: the child's own slots were EMPTIED, and the
supervisor holding the root can see it.

### Step 10 — a death is observed on the thread that dies  DONE

`SYS_PROCESS_WATCH` and `SYS_PROCESS_EXIT_CODE` (29, 71) RETIRE.  A supervisor
needed authority over a PROCESS to learn about an execution it had started
itself — and it HAS that execution: it retyped the TCB and configured it.
`SYS_TCB_WATCH(tcb, notif, bits)` and `SYS_TCB_EXIT_CODE(tcb)` name the thread,
with `RIGHT_READ` as the whole authority, because learning that something died
confers nothing over it.  Every service in the tree is single-threaded, so this
is not an approximation of the process event: it is that event, named by the
thing that produces it.

The watch array went with it.  KProcess kept room for several unrelated holders
to watch one process; a thread is watched by whoever holds its TCB, and a
second watcher is a second capability rather than a second slot.

`svc_load_minted_ws` gained `keep_tcb_dest`, so a spawner that means to wait for
its child keeps the thread — and the suite grew the child table a process server
keeps, mapping each process capability to the thread it was started with.

**Keeping a TCB keeps the corpse**, and that is the lifecycle fact the step
surfaced: a thread object's storage is a child of its budget, so svcmgr holding
a dead service's thread made that service's budget un-RESET-able and the restart
found no memory.  A supervisor holding a dead child's thread is holding the
memory it is about to need, so it drops it before respawning.  Nothing had held
a thread across a death before, so nothing had said so.

### Step 11 — an address space ends when its last capability does  DONE

The walk came down in `kprocess_reap_address_space` — which is to say when the
PROCESS died.  A walk's lifetime was therefore a property of an object that is
not the walk, and an address space that outlived its process (because a holder
kept a capability) kept a live walk nobody could reach.

It comes down in the VSpace's own destructor now, which runs when the last
capability to it goes.  For a spawned process that is the moment its last
thread releases the VSpace it was configured with (Stage 7 Step 5) and no
holder kept one — so reclamation is driven by capabilities rather than by a
death event, which is what it means in a capability system.

`kvspace_invalidate` shrank to what its name says: `valid = 0` and the frame
sweep.  It used to zero `cr3` as well, and that was the reason only the
declarer of the death could tear the walk down — the destructor would have had
nothing left to walk.  Every reader checks `valid` before touching `cr3`, so
keeping it costs nothing and buys the separation.

### Step 12 — a fault is armed on the execution that takes it  DONE

`SYS_EXCEPTION_HANDLER` armed a PROCESS.  Every thread in it faulted into one
mailbox, signalled one notification with one set of bits, and a handler holding
that registration could not tell two executions apart except by reading an id
out of the record.  That is the shape the charter forbids, one level up: the
process was standing in for the thread.

`SYS_TCB_SET_FAULT_HANDLER` (126) arms the thread, named by capability.  The
registration state moved onto `struct task` with the record Step 6 put there —
`fault_notif`, `fault_bits`, `fault_cspace`, `fault_slot`, `fault_seq_counter`
— so two threads of one process can have two handlers, or one and none.
Everything Step 7 established carries over unchanged: the mailbox is named by
the REGISTRANT (a supervisor arming a target's faults delivers into a CNode it
shares with the pager that answers), the TCB capability is published
`RIGHT_READ|RIGHT_WRITE` BEFORE the signal, a dead target fails `NOT_FOUND`,
and re-registration with the same notification carries an outstanding fault
across a handover.

Two syscalls retired with it, and the second one is the interesting one:

- **`SYS_EXCEPTION_HANDLER` (47)** — replaced outright.
- **`SYS_PROCESS_FAULT_INFO` (105)** — Step 8 KEPT this against the first
  attempt to retire it, because a real principal needed it: a supervisor
  watching a child it does not resolve for, holding `RIGHT_READ` on the process
  and no capability to any of its threads.  Step 12 removed the principal
  rather than the question.  Arming faults is a thread operation, so a spawner
  that supervises keeps its child's first thread — `svc_load_minted_ws`'s
  `keep_tcb_dest`, `RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE` — and the
  supervisor that had only a process capability now holds a thread capability
  and asks the thread.  Nothing called it any more; retiring it was bookkeeping
  by then.

The clearing moved with the state.  `kprocess_teardown` used to clear the
process's fault record so a late read honestly answered `WOULD_BLOCK`; a
terminating thread clears its own in `task_execution_teardown_off_cpu`, and
releases its handler notification and mailbox CNode there too.  The counter
that made this observable (`kfault_cleanup`) still counts exactly the records
actually cleared, from the one place that now does the clearing.

`DUPLICATE` on the kept thread capability is not incidental: a supervisor that
delegates part of the supervising role hands out a REDUCED copy — read-only to
a monitor — and minting one is how rights get given away without giving away
the rest.  T184 asserts precisely that split on the thread now: `READ` reads
the fault record and nothing else, resuming takes `WRITE`, and a process
capability, reduced or full, is not a thread and is refused outright.

### Step 13 — killing is stopping the executions you hold  DONE

`SYS_PROCESS_KILL` did two things, and Stage 7 took them apart one at a time.

The first was **stopping the executions**: `task_kill_process` swept the whole
task registry for threads whose process pointer matched.  Nothing else called
it and nothing could — naming "every thread of that process" without holding
any of them requires scanning the kernel's registry, which is the shape Stage 7
spent its length removing.  A supervisor stops the threads it holds
(`SYS_TCB_EXIT`), and `task_kill_process` is deleted.

The second was **reclamation**, and that is the half that took three steps.
Step 11 moved address-space teardown into the VSpace's destructor, so a walk
comes down when its last capability does.  What was left was the case the
roadmap named as still-to-be-decided: a process created and NEVER STARTED.
Kill found no threads, and its special case tore the object down because
nothing else could — `SYS_PROCESS_CREATE` kept a reference on the kernel's own
behalf that only the last thread's exit released, and there was no last thread.

The decision: **there is no creation reference.**  It is dropped as soon as
`SYS_PROCESS_CREATE` has published a capability, and joining a process takes a
real reference to it (`kprocess_attach_thread`).  A process therefore lives
exactly as long as capabilities and threads reference it, like every other
kernel object — a running one held up by its executions, a never-started one by
whoever holds a capability.  Deleting the last capability to a process that
never ran destroys it, which is the only thing killing it could have meant.
T304 asserts it directly: 80 never-started processes, no kill in the loop, and
the budget still RESETs.

`SYS_PROCESS_STATUS` went with it.  "Is it alive" about a process is derived
from its threads (`thread_count != 0`), so asking the derived object meant
holding a process capability for a question its threads already answer — and
answering with one bit where `SYS_TCB_GET_INFO` reports the state.

Both numbers (26, 35) are permanently reserved and answer `NOT_SUPPORTED`.

Two loader rights changed, for the same reason in both cases: the kept CNode
and the kept TCB gained `RIGHT_DUPLICATE`.  A supervisor that delegates part of
its role hands out a REDUCED copy — read-only to a monitor — and minting one is
how rights are given away without giving away the rest.

One suite-shaped consequence worth recording, because it is evidence rather
than bookkeeping: the child table needed its own CNode.  While `PROCESS_KILL`
existed, a test that spawned more children than the table held still killed
them all, because it named the process and every process capability is in the
root CSpace.  Killing names the thread now, so **a child the table has evicted
is a child nothing can stop** — and T240 holds 48 at once.  That is the
supervision cost of the model, paid in the place a process server would pay it.

### Step 14 — the budget is named, never assumed  DONE

Six syscalls allocated kernel memory out of `t->process->mem_pool`: the Untyped
the kernel remembered as "this process's", used whenever the caller had not
said which of its budgets should pay.  Stage 6 Step 5 removed that guess
everywhere a syscall had an argument to spare; these were the sites that did
not have one, and the field survived on that technicality.

Each one got an argument, and each argument is REQUIRED — a default is the
kernel making the choice again with extra steps:

| syscall | how |
|---|---|
| `SYS_VMO_CREATE` (16) | had the argument since Stage 6 Step 5; `0` stopped meaning "my own" |
| `SYS_INITRD_VMO` (55) | same — the image copy's budget is stated |
| `SYS_CAP_CREATE_IRQCAP` (39) | arg2 was unused; it names the budget now |
| `SYS_CAP_CREATE_IOPORT` (40) | had no free argument, so `base` and `count` share arg1 (`base \| count << 16`) — one range in one word, which is what they always were — and arg2 names the budget |
| `SYS_VMO_CREATE_FOR` (109) | arg3 names the budget, resolved in the CALLER's CSpace |

`SYS_VMO_CREATE_FOR` is the one worth reading twice.  It charged the PAYER's
default budget, so a caller spent an Untyped it did not hold and could not see:
the loader asked for a child's image VMO and the kernel quietly took the pages
from the child's pool.  The budget is the caller's argument now, and the loader
passes the child's pool because it holds it — the same memory, said instead of
inferred.  That the OBJECT QUOTA still goes to the payer while the MEMORY comes
from a named budget is KVMO's owner/payer split, which retires with the object
(ledger: FROZEN, memory-server), not with this step.

`mem_pool` is renamed `storage_pool`, because that is all it does now: the
`KProcess` block lives inside the region that Untyped owns, so the pool must
outlive the block.  Nothing reads it to decide whose memory pays.

One userland consequence, recorded because it is a real loss of a fallback:
`svc_loader` used to set `pool = 0` when its scratch ELF budget could not be
reset or re-carved, letting the kernel charge the caller's own pool.  With no
default there is no fallback, and a spawn that cannot get a scratch budget
fails — which is the honest outcome, because the alternative was spending the
caller's whole pool one image at a time, silently.

The device-authority probes in `lifecycle_probe` improved as a side effect:
they used to pass a bogus authority AND a zero destination slot, so what
refused them was the malformed slot, not the missing capability.  They are
well-formed in every argument but the authority now.

### Step 15 — the last things a process was standing in for  DONE

Four removals, each the same shape: an object was reached, scoped or identified
through `KProcess` when the thing it actually concerns was something else.

- **`SYS_PROCESS_VSPACE` (107)** — "give me that process's address space",
  answered by the kernel reading `child->vspace`.  Phase 25 introduced it to
  make map-into-target a first-class delegable capability instead of a
  process-cap side effect, which was the right direction and stopped one step
  short.  The spawner already HAS the address space: since Stage 6-pure Step 4
  the loader retypes it and holds it through the whole spawn, then threw the
  capability away.  It hands it over now — `svc_load_minted_ws`'s
  `keep_vspace_dest`, opt-in — and the self case is `SYS_VSPACE_SELF`, which
  this syscall's `HANDLE_INVALID` was always documented as equivalent to.
- **`SYS_PROCESS_SELF` (28)** — a capability to your own KProcess, for the
  things that used to need one: minting into your own CSpace, mapping into your
  own address space, being named as a payer, watching yourself.  Every one of
  those was re-aimed at the object it concerns in Steps 9-13.  Nothing called
  it.
- **`KProcess.exit_code`** — the code belongs to the execution that produced
  it, and `SYS_PROCESS_EXIT_CODE`, the only reader of the process copy, retired
  in Step 10.
- **the futex's owner field** — the waiter's `KProcess`, used for exactly one
  thing: refusing to wake a waiter that hashed to the same user address in a
  different address space.  The process was standing in for the ADDRESS SPACE,
  which is what actually makes two identical addresses different futexes.  It
  is `t->vspace` now — same scoping, named after the thing it scopes.

Keeping a child's address space is opt-in for a reason the tests enforce rather
than a preference: a VSpace capability keeps that address space, and every page
table in it, alive past the child's death, blocking the RESET of the budget
those tables are charged to.  The suite's drift checks are the auditor, and
made this concrete — an early version kept one for every spawn and 43 tests
failed on `vspace drift`.  The suite now asks for the child's address space
only where it maps into one, and gives it back when done.

### What Stage 7 still needed after Step 15 — and how it was answered

**Historical, resolved in Stage 7-proc.**  At this point the answer looked like
`KProcess` itself plus a user-space process server to carry the policy it held.
It turned out there was no policy left to carry: Steps 4-15 had moved every
piece to the object it concerned, so the object could simply be deleted.  See
*Stage 7-proc* below.  The inventory that follows is the measurement that made
that visible.

The inventory, measured rather than described — every remaining kernel read
through `t->process`, and where it comes from:

| what | reads | where |
|---|---|---|
| `cspace_root` | 9 | `kernel_main.c` — the boot path |
| `vspace` | 8 | `kernel_main.c` — the boot path |
| `storage_pool` | 1 | a comment in `syscall_cap.c` recording what was removed |

Every one of the live reads is the BOOT PATH: the root task's CSpace and
address space are built before anything exists that could name them.  That
exception is recorded in the ledger and does NOT retire with the process
server — it retires when the root task can speak for itself.

What still held a `struct KProcess *` outside the boot path at Step 15, and
why — **every entry below is now closed**, by Stage 7-mem and Stage 7-proc:

- **`SYS_PROCESS_CREATE` and `SYS_TCB_CONFIGURE`.**  The constructor, and the
  check that a thread is configured with the CSpace and VSpace its process was
  composed from.  The expectation here was that a user-space process server
  would replace them outright rather than convert them, because what they
  create IS the policy container: in seL4 there is no process object, and
  `seL4_TCB_Configure` binds a CNode and a VSpace to a thread with nothing to
  agree with.  ✅ That is exactly what happened, minus the server:
  `SYS_PROCESS_CREATE` is retired and `SYS_TCB_CONFIGURE`'s identity check is
  gone, so it IS `seL4_TCB_Configure`.
- **the KVmo owner/payer relation** (`kvmo_bind_owner`, `kvmo_owner`, the
  `owned_vmos` and page counters).  A VMO is charged to a process, and that
  process is its resource domain.  This retires with the KVMO OBJECT, per the
  ledger (FROZEN, memory-server) — seL4 has Frames and no owner.  Step 14 took
  the memory side of it out (a VMO's pages come from a budget the caller
  names).  ✅ Stage 7-mem took the accounting identity too: the owner relation
  and the VMO-count quota are DELETED.
- **`irq_routing`'s owner.**  Which principal a device route belongs to, so
  teardown can clear it.  ✅ Stage 7-mem gave it the right owner without
  waiting for a device server: a route belongs to the **notification it is
  bound to**, which is the object whose lifetime the teardown actually cares
  about.
- **`SYS_RESOURCE_INFO`** and the diagnostic gauges, which report per-process
  accounting and retire with the accounting.  ✅ Retired in Stage 7-mem; the
  three gauges that were never per-process moved to `SYS_UNTYPED_QUERY`'s
  GLOBAL kind.

So the honest boundary at Step 15 was this: Stage 7's stated goal — *retire
everything a process capability was standing in for* — was DONE.  Fifteen
syscalls retired, the object carried no authority a capability to something
else could not express, and no hot path read it.  What remained was not
authority but IDENTITY: a process was still the name of a resource domain.

Dissolving that name took one more increment rather than two.  Stage 7-mem
removed the resource domain — a VMO's accounting is the Untyped it came from —
and once that was gone, Stage 7-proc found nothing left for a process server to
own, and deleted the object.

Retired across Steps 3-15, all permanently reserved and answering
`NOT_SUPPORTED`:

| number | syscall | step |
|---|---|---|
| 26 | `SYS_PROCESS_STATUS` | 13 |
| 28 | `SYS_PROCESS_SELF` | 15 |
| 29 | `SYS_PROCESS_WATCH` | 10 |
| 35 | `SYS_PROCESS_KILL` | 13 |
| 47 | `SYS_EXCEPTION_HANDLER` | 12 |
| 58 | `SYS_THREAD_START` | (Stage 7 opening) |
| 71 | `SYS_PROCESS_EXIT_CODE` | 10 |
| 104 | `SYS_PROC_CSPACE_MINT` | 9 |
| 105 | `SYS_PROCESS_FAULT_INFO` | 12 |
| 107 | `SYS_PROCESS_VSPACE` | 15 |
| 116 | `SYS_CSPACE_MINT_INTO` | 9 |

Added in their place, each naming the object it acts on: `SYS_TCB_FAULT_INFO`
(123), `SYS_TCB_WATCH` (124), `SYS_TCB_EXIT_CODE` (125),
`SYS_TCB_SET_FAULT_HANDLER` (126) — alongside `SYS_CSPACE_SELF` (119),
`SYS_TCB_CONFIGURE` (120), `SYS_TCB_WRITE_REGS` (121) and
`SYS_VSPACE_MAP_TABLE` (122) from Stages 5 and 6-pure.  `KPROCESS_MAX_LIVE`
retired in Step 3 and the global thread identifier in Step 7.

One smaller item was recorded here as NOT Stage 7 work:

- **The VMO-count quota** retires with the `KVMO` object (memory server), per
  the ledger.  ✅ Done in Stage 7-mem: it went with the owner relation, and a
  VMO's accounting is the Untyped it was carved from.

## Stage 7-mem — the memory server  ✅ CLOSED

Precondition: Stage 7's authority work (done).

`KVmo` is a kernel-side memory abstraction with an owner, a quota and a page
array.  seL4 has Frames and nothing else.  This stage deletes the object, and
with it the last things `KProcess` is for.

- **Retire `KVmo`.**  A "VMO" becomes what it already almost is: a set of
  Frames the holder retyped from an Untyped, mapped through capabilities.
  `SYS_VMO_CREATE`/`CREATE_FOR`/`MAP`/`MAP_INTO`/`MAP_PAGE`/`SIZE`/`SHARE`
  collapse into the Frame family.  Step 14 already made the memory come from a
  budget the caller names, so what dies here is the OBJECT and its identity,
  not the accounting.
- ✅ **The VMO OWNER relation is retired** (`kvmo_bind_owner`, `kvmo_owner`,
  `KVmo.owner`), and with it `SYS_VMO_CREATE_FOR` (109): it named a PAYER on
  top of a budget the caller already names and holds, for a per-process count
  that no longer exists.  A loader that wants a child's image charged to the
  child carves it from the child's budget.
- ✅ **The VMO-count quota** (`owned_vmos`, ceiling 32) and the page counters
  are gone.  A budget is the Untyped; a second ceiling the kernel invented is
  the same mistake Step 2 removed for pages and Step 3 for live processes.
- ✅ **`SYS_RESOURCE_INFO` (110) is retired** with the domain it reported on.
  Its three GLOBAL gauges — kernel-slab occupancy, failed charges, rollbacks —
  were never per-process and moved to `SYS_UNTYPED_QUERY`'s GLOBAL kind.  The
  live-VMO count joined the per-type gauges in `SYS_SCHED_INFO`, which makes
  the suite's leak checks GLOBAL where the per-process form only ever caught
  the caller's own.
- **`KIrqCap` and `KIoPort` stop being objects.**  seL4 has no kernel object
  behind an interrupt or an I/O port — an IRQHandler capability names a line,
  and x86 I/O port access is a capability over a port RANGE with no allocation
  at all.  This is D-5's deeper half and it removes the last two `kslab`
  producers outside the boot path.
- **Delete `KProcess`.**  What is left of it after the above is the
  constructor (`SYS_PROCESS_CREATE`) and `SYS_TCB_CONFIGURE`'s identity check.
  In seL4 a process is a TCB plus a CNode plus a VSpace, and
  `seL4_TCB_Configure` binds them with nothing to agree with.  The IRQ-route
  owner becomes the capability holder rather than a process.

**Exit criterion:** charter §4's last unchecked box — *all canonical objects
born from Untyped* — is checkable, and `scripts/purity_allowlist.txt` contains
only the boot path.

## Stage 7-proc — KProcess deleted  ✅ CLOSED

`struct KProcess` no longer exists.  Nothing in the kernel allocates, owns or
names a process; `SYS_PROCESS_CREATE` (25) answers `NOT_SUPPORTED` and
`KOBJ_PROCESS` is a reserved enumerator no live capability carries.

What a "process" is, is **threads configured with the same CSpace and the same
VSpace** — a fact about two capabilities rather than a third object to point
at.  That is seL4's model exactly, and the steps that got here were each about
moving one thing off the object:

| what moved | to | step |
|---|---|---|
| CSpace root, address space | the thread | 4, 5 |
| fault record, fault handler | the thread | 6, 12 |
| death, exit code | the thread | 10 |
| kill, liveness | the thread | 13 |
| the creation reference | nobody — a process lived as long as its capabilities | 13 |
| the default budget | a required argument | 14 |
| the VMO owner and its quota | the Untyped a VMO is carved from | 7-mem |
| the IRQ route owner | the notification the route is bound to | 7-mem |
| address-space reclamation | the address space's own close and destroy | 7-proc |
| the root CSpace's cycle break | the CNode's own reference count | 7-proc |
| the spawn's handle | the child's first thread | 7-proc |

Three things had to be fixed before the object could go, and each was a real
bug rather than bookkeeping:

1. **A thread held no ACTIVE reference on its address space** — only a
   lifecycle one.  So a spawner deleting its own capability at the end of a
   spawn invalidated the space its child was about to run in.  Every spawned
   thread faulted on its own entry point until that was fixed.
2. **A slot naming its own CNode took an active reference**, which is a
   reference-counting lie: an object reachable only from itself is reachable by
   nobody.  The count never fell to zero, so a self-naming CSpace never emptied
   and `KProcess` had to break the cycle pre-emptively at a moment it knew
   because it counted threads.
3. **53 syscall guards asked `!t->process`** when what they meant was "can this
   task name anything".

The **user-space process server is not needed** and is not scheduled.  It was
in this roadmap as the thing that would replace KProcess's policy; there turned
out to be no policy left to replace once every piece was moved to the object it
concerned.  A supervisor keeps its children's threads (and, when it maps into
them, their address spaces), which `svc_load_minted_ws` hands over — that IS
the child table a process server would have kept, in the place seL4 puts it.

One thing this leaves behind, recorded rather than hidden: `svc_load_minted_ws`
still takes `proc_c`, the spawn AUTHORITY, and ignores it.  There is nothing to
authorise — a child is a TCB, a CNode and a VSpace retyped from a budget the
spawner holds, and holding that budget is the authority, as it is in seL4.
Retiring the argument belongs with retiring `IRIS_CPTR_PROC_CONTROL`, which is
Stage 10-abi's business.

## Stage 8-mcs — Full MCS scheduling  ✅ CLOSED

Precondition: Stages 0–2 (canonical SC/TCB + CSpace-only IPC).

What the stage asked for, and what landed:

| asked | landed |
|---|---|
| replenishment | **sporadic**: every tick consumed returns exactly one period after it was spent, so a thread can never spend more than its budget in any window of its period.  Host R-1..R-8 |
| timeouts | **timeout faults**: budget exhaustion suspends the thread and tells a temporal supervisor, which decides.  A SEPARATE registration from the exception handler, because the principal answering "this overran" is not the pager.  `SYS_TCB_SET_TIMEOUT_HANDLER` (128), T307 |
| SC delegation and donation during IPC | **donation to passive servers**: a thread with no SC of its own runs on the requester's time, recorded on the reply object and returned by every path that ends a binding.  `SYS_REPLY_RECV` (129) closes the window where the server would otherwise be runnable with no SC at all.  T308, T309 |
| revisit "no combined ReplyRecv" | done — it is implemented, not merely revisited |

Three defects surfaced doing it, each of which had been silent:

1. **Budget was a leaky bucket that only refilled when empty.**  The single
   refill site was the exhaustion branch, so a thread that BLOCKED before
   exhausting carried its remainder forward for ever.  A server handling a
   request in 2 of its 5 ticks and waiting on its endpoint kept 3, then 1, then
   stalled — its bandwidth fell the more often it did the right thing.
2. **A thread with no scheduling context was never charged at all**, so it ran
   with unlimited time.  Donation is what makes an SC-less thread mean
   "passive" rather than "exempt".
3. **Donation was wired into two of the three rendezvous paths**, so a server
   ran budgeted or not depending on which side of the rendezvous arrived
   first.  All three now go through one helper.

Not seL4's yet: `refill_max` is a compile-time constant (8 entries) rather than
a per-SC configuration chosen at retype.  At tick granularity with a coalescing
flush it has not been reachable; recorded rather than claimed closed.

## Stage 8-cap — the capability model's last gaps  ← IN PROGRESS (D-2 and D-8 closed; D-4's mechanism landed, its adoption waits on the memory server; D-3 is a decision still owed)

Four items, each a registered divergence or a measured hole.  All are additive:
none of them is a rewrite, which is why they are grouped rather than staged
separately.

**Landed: CNode guards below the root (D-2).**  A CNode CAPABILITY carries a
guard — `SYS_CSPACE_SET_GUARD` (127), `KCSlot.guard`/`guard_bits`, checked by
the walk.  Additive by construction: `guard_bits == 0` is every slot's initial
state and resolves exactly as the pre-guard kernel did, which is why 273
runtime tests and 18738 host assertions passed unchanged on the landing commit.
Capability-local, not object-local — two capabilities to one CNode can be
guarded differently, which is the property that makes it seL4's guard rather
than a lookalike (host G-7).  Pinned by T306 and `test_cnode_guard` G-1..G-8.

**D-2 is CLOSED: the ROOT guard landed too.**  A thread reaches its root CNode
through a structural pointer rather than a slot, so its guard lives on the
thread, installed by `SYS_TCB_CONFIGURE`'s arg3 — seL4's `cspace_root_data`,
the same argument in the same position of the same operation.  It cost no
churn in the resolvers: the walk picks the guard up only when the CNode it is
walking IS the running thread's root, which is the only capability the guard
belongs to.  T312 pins the property that makes it seL4's guard rather than a
lookalike — a parent and a child sharing ONE root CNode object address it
differently.

- **D-8 — revoke is preemptible.**  ✅ **CLOSED (Stage 9-evt).**  Bounded
  slices plus the restart machinery step 1 built; no cursor and no zombie
  capabilities needed, because each slice destroys whole capabilities and
  leaves nothing half-deleted to name.  T311.

- **A9 / D-6 — LEGACY_ROOTs to zero.**  ✅ **The defect class is CLOSED.**
  T305 measures 43 live roots of 335 MDB nodes at Stage 7 close, and the
  fault-delivery class — the one that was a defect — is fixed.  What remains is
  two legitimate classes: the boot path (permanent — seL4's BootInfo
  capabilities are roots too) and KVmo publishes, which disappear with the
  memory server.  The absolute count moves with what is alive; T305 asserts on
  the delta across a spawn/kill and a mint/revoke cycle, which is the shape a
  new productive producer would have.  Nothing is left for this stage to do on
  D-6.
- **D-4 — a per-thread IPC buffer.**  🔶 **MECHANISM LANDED; BLOCKED ON D-5/D-6.**
  `SYS_TCB_SET_IPC_BUFFER` is seL4's `seL4_TCB_SetIPCBuffer`: a thread registers
  a FRAME it retyped and mapped, and the kernel moves payloads between the two
  ends' frames through its own physical window.  The size stops being a kernel
  constant, no user pointer is named on either side, and the buffer is a
  capability refused like one when it is not a writable frame (T313, host TB-9).
  It is also less work than the path it replaces: three copies and two pointer
  validations become one copy.  What remains is not kernel work.  The first
  blocker is gone — every service now holds a capability to the Untyped its
  address space was already charged to, so it can retype a frame at all, which
  it could not before — and console is migrated.  Two things are still owed:
  an IPC buffer is per-THREAD, so a service with several IPC threads needs one
  page each, and vfs composes its reply in a second buffer while the request is
  still live, which one shared page cannot do without reading its protocol
  first.  Until every service is across, `ipc_kbuf` stays.
- **D-2 — CNode guards.**  Resolution is a pure radix walk, so a CPtr's meaning
  is fixed by the CNode sizes along the path: no sparse layouts, no
  depth-limited lookup, and a two-level CSpace costs the full radix of each
  level.  Additive (a guard field per CNode plus the resolution change), with
  Stage 4 Step 6b's injectivity rule re-derived.  Stage 7 paid for its absence
  repeatedly — leaf exhaustion, the "a mint source must be a root CPtr" rule,
  and a whole extra CNode for a supervisor's child table.
- **D-3 — the rights set.**  ✅ **DECIDED: kept, and now PERMANENT.**  The two
  choices were to adopt `Read/Write/Grant/GrantReply` and move
  non-re-delegation into the derivation tree, or to keep the current set and
  say why.  The first is not actually available: the tree RECORDS what was
  derived, it does not PREVENT deriving, so moving `RIGHT_DUPLICATE` there
  would delete the property rather than relocate it.  seL4 lets any holder copy
  anything it holds, on the reasoning that copying grants no authority the
  holder did not already have — true of authority, false of confinement, since
  a child that can copy its endpoint capability can seed a third party with it.
  IRIS refuses the copy.  `RIGHT_TRANSFER` is seL4's `Grant` moved from the
  endpoint to the capability being sent, and `GrantReply` needs no equivalent
  because the reply capability is a separate object.  The cost is stated rather
  than hidden: **"IRIS uses seL4's rights" is never a correct sentence**, and
  the set is pinned by host RG-1..RG-5 so a permanent declaration is about
  something that cannot drift.

**Exit criterion:** `mdb_legacy_roots` is a bounded, named inventory with no
defect class in it, and every §6 row is either permanent-deliberate or has a
live trigger.

## Stage 9-evt — the event kernel  ← STEPS 1 AND 2 CLOSED; STEP 3 OPEN

This is ledger **D-1**, which carried "ACTIVE_LEGACY — no stage assigned" from
Stage 5 until this stage was opened.  The ledger's own words are that it "is
the structural reason seL4 can bound in-kernel latency (and be verified) while
IRIS cannot claim either".

It decomposes into three steps whose ORDER is forced rather than preferred.

**Step 1 — blocking handlers are RESTART-SAFE. ✅ CLOSED.**  No syscall holds a
live C local across a block.  Each one expresses its continuation in thread
state and is proven by actually being re-executed, not by inspection: the
dispatcher re-enters the handler with the same arguments and the handler must
reach the same answer.  Getting there found real defects, all of the same
shape — state that looked like a local and was actually a decision already
taken.  A restarted `SYS_SLEEP` recomputed its deadline and slept forever; a
restarted `NOTIFY_WAIT` re-resolved a CPtr whose object had since closed and
reported NOT_FOUND where the caller was owed CLOSED; a restarted `REPLY_RECV`
re-ran both halves.  The dispatcher-owned `sc_reentry` flag is what
distinguishes "first entry" from "re-entry" without any handler having to
invent its own marker.

**Step 2 — the syscall frame is ABANDONED, not parked. ✅ CLOSED.**  A parking
thread now has its outgoing integer context thrown into a discard buffer, its
kernel stack reset to the top, and its resume RIP set to a trampoline: a
blocked thread's kernel stack holds NOTHING.  It resumes on a fresh stack,
re-runs the syscall from thread state, and returns to ring 3 through an iretq
built entirely from the TCB.  The whole user context is saved at syscall entry
now — including the six callee-saved registers, which a procedural kernel
preserves for free by obeying the C ABI and an event kernel must save
explicitly, because the frame those spills lived on is exactly what step 2
throws away.  That is the bill seL4 pays on every entry, and there is no
cheaper version of it.  There is one honest fallback: when nobody else can run,
the park declines and yields through its own frame as step 1 did — abandoning
buys nothing when the alternative is idling on the same stack.  `syscall_abandons`
counts only real abandonment, separately from `syscall_restarts`, because the
two are indistinguishable from ring 3 and only one is what D-1 is about.

**Step 2 already paid for itself**: it is what made ledger **D-8** closable.  A
preemptible revoke needs somewhere to park a continuation, and step 1 built it.

**Step 3 — ONE kernel stack per core. 🔶 HALF DONE.**  What remains, and the
first analysis of this stage missed it: it is not enough for syscalls to stop
keeping state on the stack.  A timer interrupt fires while a task runs in USER mode and
lands on the kernel stack named by `TSS.RSP0`; if that stack is per-core and
the ISR then preempts to another task, the outgoing task's interrupt frame sits
on a stack the incoming task is about to use.  So step 3 additionally requires
the IRQ path to save the full user context into the TCB rather than leave it on
a kernel stack — a second conversion, of the preemption path, comparable in
size to the first.  Syscalls themselves are not the obstacle: `SFMASK` clears
IF, so no syscall is ever preempted mid-flight, and step 1's restart points are
the only places a thread gives up the CPU inside the kernel.

That conversion is now half done.  `struct iris_user_ctx` is a field of
`struct task`, and every ring-3 kernel entry saves the thread's whole register
state into its own TCB while every ring-3 exit rebuilds the frame from the TCB
of whatever thread is current by then.  With per-thread stacks still in place
the two are an identity — deliberately, because doing the move while the frame
is still authoritative means the rest of step 3 changes where execution
RESUMES and not what gets RESTORED.  It is measured rather than assumed
(`irq_ctx_saves`, and T314's register-integrity spin across thousands of
preemptions), because a path whose effect is currently invisible is a path
that rots.

What is left: `TSS.RSP0` becomes a per-core stack, and the reschedule taken
inside the timer ISR stops going through `context_switch` — which exists to
swap kernel stacks — and becomes a choice of which TCB the exit path restores.
Two cases, and steps 1 and 2 are what make both expressible: a thread
preempted in ring 3 resumes through that restore, and a thread parked in a
syscall resumes at its restart trampoline on the core's stack, holding
nothing.

Then, and only then, the per-thread stacks can go: kernel memory stops scaling
with thread count, and a retyped TCB stops costing memory its payer did not pay
for.

What the whole stage buys, and why it is not optional for a serious product:

- **A bounded in-kernel latency claim.**  Without it, the longest a thread can
  be kept out of the CPU is "however long the longest kernel path takes", which
  is not a number anyone can state.  Every real-time microkernel competitor
  states one.
- **The precondition for any verification work at all**, if that is ever
  wanted.  It is not scheduled here, but a multi-stack blocking kernel forecloses
  it entirely.
- It must land **before Stage 9 (SMP)**: SMP re-derives every atomicity
  property, and re-deriving them twice — once for a blocking kernel, once for
  an event kernel — is the kind of work that gets done badly the second time.

## Stage 9 — SMP

Hard precondition: single authority namespace (4), CDT (1), lifecycle (0),
CSpace-only IPC (2), a documented locking model, **and Stage 9-evt**.

- Re-derive EVERY atomicity property that today depends on the
  non-preemptive uniprocessor kernel (catalog: IPC staging, RETYPE2, reply
  bind, teardown). Per-CPU run-queue ownership. No correctness may still be
  argued "because the kernel is non-preemptive".

The 9-evt precondition is new and it is not a preference: every one of those
properties is re-derived against the kernel's execution model, and doing it
once against a blocking multi-stack kernel and again against an event kernel
means doing it twice, with the second pass carrying the assumptions of the
first.

## Stage 10-dma — device authority must be containable  ← NOT STARTED

This is a SECURITY hole in the capability model, not a platform feature, which
is why it is pulled out of Stage 10's list and given a stage of its own.

There is no IOMMU support in the tree (measured: zero references to IOMMU,
VT-d or DMAR anywhere in `kernel/`).  A driver holding an I/O port or IRQ
capability can program a DMA-capable device to read or write ANY physical
address — including the kernel's own memory and every other task's.  Every
guarantee the rest of this roadmap builds is void against such a driver.

The capability model makes this worse rather than better in one specific way:
the whole point of user-space drivers is that a compromised driver is
contained by the capabilities it holds.  Without an IOMMU that containment is
fiction, and IRIS's driver-isolation document says so only implicitly.

- DMAR/VT-d table parsing; per-device domains.
- A device's DMA reach becomes a CAPABILITY: the frames it may target, named
  by whoever grants them, revocable.  This is seL4's shape (`seL4_X86_IOSpace`)
  and it is the only thing that makes an ioport or IRQ capability safe to
  delegate.
- **Retire the kernel's I/O port whitelist** (charter A5/P2, both PARTIAL for
  this reason).  It exists because the kernel cannot otherwise bound what a
  port grant can reach; with per-device domains the bound is a capability and
  the hardcoded table — kernel policy, charter P3 — goes.

## Stage 10-abi — freeze the ABI  ← NOT STARTED

A product that other people build on has a versioned, stable ABI.  IRIS today
has 71 live syscalls and **43 retired-but-reserved numbers**, which is the
correct state for a system in convergence and the wrong state to ship.

- A declared 1.0 syscall surface, with the reserved numbers either reclaimed
  or documented as permanently dead.
- A compatibility policy: what may change in a minor version, what may not,
  and how a caller detects the difference.  `SYS_UNTYPED_QUERY`'s versioned
  struct is the pattern that already exists; it should be the rule.
- The `handle_id_t` typedef and the `HANDLE_INVALID` spelling survive in
  userland as naming residue from a namespace that no longer exists.  A 1.0
  ABI should say `iris_cptr_t` everywhere or explain why not.

## Stage 10 — General-purpose platform

Precondition: consolidated microkernel (0–9 as applicable), 10-dma, 10-abi.

- User-space drivers; PCI/ACPI; storage; persistent FS; networking;
  optional POSIX personality via servers/libraries; performance; real
  hardware. None of this lands earlier: charter §5.

---

## The ceiling: what this roadmap does NOT reach, and why

A roadmap that ends without stating its ceiling invites the reading that
finishing it produces seL4.  It does not, and two of the three reasons are
deliberate.

**1. The ABI shape.**  seL4 has roughly a dozen syscalls and expresses every
other operation as an INVOCATION on a capability carrying a method label.  IRIS
has numbered syscalls, each resolving its own arguments and checking its own
rights.  The charter registers this as permanent and deliberate, and that is
defensible — but the consequence should be stated plainly: in an invocation
model, a new operation is capability-gated BY CONSTRUCTION, while here it is
gated by a check the author has to write correctly every time.  That is a
structural guarantee traded for a per-syscall discipline, and the discipline
has failed before (Stage 7 alone found a rights check on the wrong object, a
dual-namespace argument, and two writers of one field under two different
locks).  The mitigation is the review gates, not the type system.

**2. Formal verification.**  Out of scope, per the charter.  Worth stating
without euphemism: *the proof is seL4's identity*.  A system that converges on
seL4's model without it has converged on the design, not on the guarantee.
Stage 9-evt is the only item here that would even make the question askable.

**3. Everything else in this document is reachable.**  Stages 7-mem through
10-abi close every measured divergence: the object model becomes Frames and
Untypeds, the derivation tree has no unparented capabilities, the kernel stops
blocking, device authority becomes containable, and the ABI becomes something
to build on.  What remains after that is a microkernel with seL4's authority
model, seL4's object model, seL4's execution model and its own ABI — which is
an honest and defensible thing to be, and is what this project should claim.

---

## Entry contract for the CDT/MDB increment (Stage 1)

What the CDT increment had to implement, defined so that Stage 0 would not
leave it any ambiguity. Delivered in Phase S3; kept here as the historical
contract:

**Structures.** Per-CNode-SLOT derivation metadata (not per handle): a link
to the parent slot + a list/ring of children (seL4 MDB style: a
doubly-linked list ordered by depth, or an explicit tree). The metadata
storage lives inside the slot itself (a CNode is already born from Untyped —
no kslab).

**Relationships.** Original (retype/mint from Untyped) vs derived
(copy/mint/transfer). Derivation crosses CNodes and processes. The parent
Untyped is the root ancestor of every retyped object (child_count integrates
or reconciles with the tree).

**Operations.** `copy` (same rights), `mint` (rights↓ + badge once), `move`
(relocates the slot preserving its position in the tree), `delete` (single
slot; if it is the object's last cap, destroy), `revoke` (recursively removes
ALL descendants of the slot, in any CSpace; the revoked slot survives).

**Invariants.** (1) a child's rights ⊆ parent's rights; (2) badge immutable
after the first badging; (3) deleting a parent does NOT orphan the tree
(reparent or sweep, choose and document — seL4 uses the MDB for this);
(4) revoke is atomic w.r.t. staged IPC: a cap in peek staging that is revoked
is not delivered (commit fails cleanly); (5) exact rollback if a tree
operation fails partway.

**CNode integration.** `kcnode_mint*/fetch/delete/swap` maintain the tree;
tearing down a CNode (close) deletes each slot through the tree, not just a
refcount release.

**IPC integration.** Receive-slot delivery registers the delivered cap as a
child of the source cap (prepares Stage 2).

**Untyped integration.** retype registers the destination slots as originals
of the Untyped; RESET requires an empty tree (replaces/refines child_count);
revoking the Untyped = revoking all its originals.

**Teardown integration.** A process's death deletes all its slots through the
tree; caps that OTHER processes derived from its own remain where the chosen
model defines (documented: seL4 keeps them — derivation does not impose the
holder's lifetime).

**Required tests.** Cross-process chain A→B→C + revoke at A; revoke during a
staged transfer; deleting the intermediate; mint with rights↓ and re-badge
denied; death of the intermediate holder; retype/revoke/reset stress with
gauge verification and no-UAF; a guard that `legacy_handle_derivation_migrated`
→ 0.
