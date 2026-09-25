# IRIS — seL4 Convergence Roadmap (normative, by dependencies, no dates)

Orders the stages toward the [purity charter](iris-sel4-purity-charter.md).
Each stage declares its **technical precondition** (what must be closed
first) and its **closing criterion** (what must be demonstrable when it ends).
The [ledger](sel4-convergence-ledger.md) maps every transitional mechanism to
its retirement stage. No stage may be declared closed while its productive
path still depends on the mechanism it retires (charter §3.10).

## Where this is, in one paragraph

**Every convergence stage in this document is closed.**  The authority model,
the object model and the kernel architecture are seL4's; the last four
differences that were about SHAPE rather than substance turned out to be three
pieces of substance (closed: A-21, A-22, A-24) and one decision (the ABI).
The review that followed them found five more things and all five are closed
too: IPC capability transfer is a COPY as seL4's is, and the delivered
capability is a revocable derivation child of the sender's slot (A-29); a
capability of the wrong type is answered as `WRONG_TYPE` rather than flattened
to `INVALID_ARG`, with `ACCESS_DENIED` kept for the right type without the
authority (A-30); two files whose names had outlived their subjects are
renamed and two orphaned headers deleted, and the "123 dead #defines" that
audit reported turned out to be two (A-31 notes); `SYS_CNODE_SWAP` has ring-3
coverage for the first time (T336); and the 26,572-line test suite is twelve
files with an interface (A-31).
What is left on this roadmap is not convergence — it is FORWARD work on a
system that has arrived: SMP (prepared, not started — per-CPU run queues and
stacks exist, no AP is brought up), DMA containment (**a security hole, not a
feature**: zero IOMMU references in the tree, so a driver holding an ioport or
IRQ capability can program a device to write any physical address, and the
containment user-space drivers are supposed to give is fiction until
`seL4_X86_IOSpace`'s shape exists), and an ABI freeze.

ONE thing no stage closes, and it was always going to be that way: formal
verification, which is seL4's identity.  A system that converges on seL4's
model without the proof has converged on the design, not on the guarantee, and
this document says so wherever it is tempted to claim otherwise.  This
paragraph named two until A-32 — the other was the ABI shape, carried as a
permanent decision on a cost estimate that was right and a gain estimate that
was not.

Measured, not recalled (recounted at the file-by-file audit): **one**
invocation door and three syscalls that invoke nothing, 67 methods reached by
label — **and every generic seL4 invocation now exists**, the last five closed
after the file-by-file audit named them — a message that is a MessageInfo word
and message registers with no pointer anywhere on the path, and no pointer the
KERNEL reads either, since the audit found the user-copy read side had lost its
last caller; 11 retypeable object types all of them seL4's, scheduling domains
partitioning time above priority, 322 runtime tests, 29 host suites, 27429 host
assertions, 37 of 37 charter invariants MET, and a purity gate that now follows
BOTH kernel allocators over the transitive closure with zero exemptions.  See [the audit](sel4-purity-audit.md) for what reading every file
against seL4 turned up, including one A9 defect it fixed.

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
| 13-form — the four FORM divergences (A-20's audit) | ✅ 3 of 4 CLOSED, the fourth decided.  **A-21** address-space identity is `ASIDControl`/`ASIDPool`; **A-22** a fault is an IPC message on an endpoint answered by a reply capability; **A-24** the kernel cannot block a thread on time — waiting is a ring-3 service — with **A-23** (`seL4_TCB_BindNotification`) as its enabler and **A-25** (`CancelBadgedSends`) closing the audit's last item.  The fourth, the ABI SHAPE, is a permanent deliberate divergence (charter §4) |
| 9 — SMP | ✅ **All 5 steps done.**  §9.1 hierarchy and §9.2 catalog written and enforced (`make check-locks`); step 1 (the one-core kernel made SMP-correct), step 2 (TLB shootdown), step 3 (APs discovered and started), step 4 (they schedule — `online=4 dispatching=4`), step 5 (the adversarial phase — four tests aiming four cores at one object, which found four real defects: a rollback that freed another core's memory, a release-then-use, a teardown gate that was not atomic, and a dispatch that overwrote a Suspend).  Full suite green on `-smp 1` and `-smp 4`.  What remains is NOT mechanism: the model-based fuzzer is not yet aimed at N cores, and §9.4's limit stands — TCG interleaves, it does not reorder |
| 10-dma — device authority must be containable | ✅ **All 6 steps done**, a device is watched being refused.  The DMAR is parsed and the units probed; translation is ENABLED with every device blocked; `KIOSpace` and `KIOPageTable` are retyped objects and `IOSpaceControl` a BootInfo authority; a frame mapped into an IOSpace is what a device may reach, and unmapping or destroying the space takes it back — from the unit's translation cache as well as the table.  **T351** pins containment, **T352** the whole arc, and **T353** is a ring-3 driver for a real bus master that is refused without a mapping, reaches exactly the frame it is granted, and is refused again when it is revoked — on a machine with no unit the same driver reaches memory nobody granted it.  The driver cost three pre-existing defects: an NX bit riding in every physical address `paging_virt_to_phys` returned, a port ABI with no width above a byte, and no way to map a BAR uncached |
| 10-abi — freeze the ABI | ✅ **CLOSED.**  The surface is four syscall numbers and 77 contiguous invocation labels, declared in `iris/abi.h` and ASSERTED by `tests/kernel/test_abi.c` over every number the dispatcher can see — a description nothing checks is a description that goes stale, which is the lesson the stage was taught by its own opening paragraph.  BootInfo names the ABI and the root task refuses a major it was not built for.  The naming residue of the retired handle namespace is gone, and removing it found a capability argument being truncated to 32 bits |
| 10 — General-purpose platform | ◐ **8 of 9 settled.**  Delivered and gated: `pci` (the bus is a service and the only task that reaches configuration space), ACPI reachable from ring 3, `blk` (an AHCI driver whose controller's DMA is contained, with a write path and FLUSH CACHE), `fs` (a filesystem on a disk IRIS owns, proven by booting twice and reading the image from the host), `net` + `ip` (an e1000 driver and, above it, ARP/IPv4/UDP — gated by a TFTP read against a server that is not this machine), and **T356**, which measures the system and fails on order-of-magnitude regressions.  POSIX is DECLINED on the record (charter §6).  **Real hardware is no longer untried**: IRIS booted a real desktop on 2026-09-25, found its disks and wrote to its own partition — but that is one machine observed once, not support, and every automated gate still runs under QEMU |

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

**Every item A-20's file-by-file audit found is closed.**  It found six things
no row had named — three authority holes (`SchedControl`, MCP, ambient
priority; closed in A-20 itself), the ASID capability model (A-21), faults as
IPC (A-22), the bound notification (A-23), timed blocking (A-24) and
`CancelBadgedSends` (A-25).  The last of them was the ABI shape, which was a
decision before it was a finding and stopped being either at **A-32**: a method
is named by sending a label to the capability it acts on, and the numbered
table is three calls that invoke nothing.

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
because a roadmap that grades itself is not evidence.

**The authority model is done.  So is the kernel architecture.  So, now, is the
FORM.**

All eight dimensions below are met.  The eighth was the ABI shape, carried for
most of this project's life as a recorded permanent decision; A-32 closed it.
What is left is not a dimension of this table — it is the proof, which is
seL4's identity and was never on it.

Until the A-20 audit, "IRIS has seL4's semantics in a different shape" was a
claim with four unexamined items behind it, and three of them turned out to be
substance rather than shape:

  - **Address-space identity** (A-21) was not a shape difference at all.  The
    kernel handed every retyped VSpace a hardware identifier out of a global
    bitmap nobody could name, be refused from, or account for.  It is
    `ASIDControl` and `ASIDPool` now, and a thread cannot be bound to an
    unnamed address space.
  - **Faults** (A-22) were three mechanisms where seL4 reuses one — a
    notification, a mailbox the kernel minted a TCB capability into on every
    fault, and a generation number standing in for a one-shot token.  A fault
    is an IPC message on an endpoint, answered by replying to it.
  - **Timed blocking** (A-24) was the last product living in the kernel.
    Three syscalls parked a thread with a deadline and had the scheduler wake
    it — a kernel deciding how long a thread may wait and whose waiting is
    worth a data structure.  Waiting is a ring-3 service; the kernel keeps the
    timer interrupt for preemption and MCS accounting, as seL4's does.

Two smaller absences from the same audit closed with them, and neither was
small.  `seL4_TCB_BindNotification` (A-23) is what lets ONE thread be a driver:
without it, a thread blocked receiving on an endpoint is deaf to signals, so
both drivers in this system busy-waited on a kernel timeout to serve an
interrupt and a request queue at once.  And `CancelBadgedSends` (A-25) is the
half of revocation that was missing — revoking a badged capability stopped a
client sending anything new and left whatever it had already queued to be
delivered afterwards.

**The fourth item was the ABI shape, and it is closed (A-32).**  It used to
read: 62 live numbered syscalls, each taking CPtrs and checking rights itself,
where seL4 has a handful and expresses every other operation as an invocation
on a capability carrying a method label.  The authority SEMANTICS were already
equivalent — nothing reachable without naming a capability, every operation
checking rights on the object it acts on, and a syscall number selecting a
METHOD and never an object — but a rule the kernel obeyed and nothing checked
is a different thing from a rule the shape enforces, and that is what changed.
`SYS_INVOKE(cptr, label, …)` is the surface; three numbers survive, each
because it invokes nothing.  What remains that no convergence work changes is
the verification surface, and that was never this row.

| Dimension | State | Evidence |
|---|---|---|
| Object model and creation | **met** | every canonical object is retyped from Untyped; address spaces and CSpaces are retyped by their HOLDER (Stage 6-pure).  `KProcess` — the largest of the four object types seL4 has no equivalent for — is DELETED (Stage 7-proc), and `KVmo` is DELETED with it (D-5): a grant is a run of frame capabilities, one per page, so which page a pager may install is which capability it holds and the kernel owns no memory on anybody's behalf.  MMIO is handed over as a DEVICE Untyped the way seL4's BootInfo does it (D-9, D-10, T316/T317).  **A-21 ADDED one**, and it is seL4's: `KAsidPool`.  Address-space identity used to come from a kernel-global bitmap nobody could name; it is now a pool retyped from an Untyped by a holder of `ASIDControl`, and a thread cannot be bound to an address space that has not been assigned an identifier from one.  What remains that seL4 has no equivalent for is `KInitrdEntry` and `KBootstrapCap`: neither costs kernel memory, neither is how anything is reached, and seL4 would express both as capability TYPES with no backing object — a change to how a CNode slot is represented, not to what the system can do |
| Capabilities (CSpace, CDT, revoke) | **close** | native CDT/MDB, recursive cross-process revoke, one namespace, and CNode GUARDS on the capability rather than the object — the root CSpace included, which is where a guard is load-bearing and where it was missed first (D-2, closed).  Revoke is preemptible (D-8, closed).  The rights set is different from seL4's and now permanently so (D-3, decided): `RIGHT_DUPLICATE` makes a delegation non-re-delegable, which seL4 cannot express — its derivation tree records what was derived, it does not prevent deriving.  Pinned by host RG-1..RG-5.  **A-25 closed the last operation seL4 had and IRIS did not**: `SYS_EP_CANCEL_BADGED_SENDS` cancels the in-flight sends of one badge, because revoking a badged delegation used to stop new sends and leave whatever was already queued to be delivered afterwards — revocation with a tail.  One divergence in this dimension went unwritten until the A-26 review and was closed by **A-29**: IPC capability transfer used to be a MOVE (the sender's slot was emptied) where seL4's is a COPY.  It is a copy now, and the delivered capability is a revocable MDB child of the sender's slot — which is what the kernel had been recording all along while deleting the parent.  **No open gap**, one registered permanent divergence (the rights set) |
| IPC | **met** | endpoints, badges, reply objects, receive slots, no handle fallback, and `SYS_REPLY_RECV` — seL4's combined `ReplyRecv`, which a passive server needs so it never crosses the gap between returning its donated time and blocking again (Stage 8-mcs, T309).  D-4 is CLOSED: `SYS_TCB_SET_IPC_BUFFER` is seL4's `seL4_TCB_SetIPCBuffer`, `ipc_kbuf` is deleted, and a payload with no registered buffer is an error.  **A-22 made FAULTS use it**: a faulting thread CALLS an endpoint, the handler receives the record as an ordinary message with a reply capability, and replying resumes it — where there used to be a notification, a mailbox the kernel minted a TCB capability into on every fault, and a generation number standing in for a one-shot token.  **A-23 added `seL4_TCB_BindNotification`**, without which a thread blocked receiving on an endpoint is deaf to signals and no server can take both an interrupt and a request queue on one thread |
| No ambient authority | **met** | boot authority is one capability per authority, every per-process quota is gone (Stage 7), and the kernel's hardcoded ioport whitelist is REMOVED (Stage 5): the range a holder may claim travels on the `IOPORT_CONTROL` capability, narrowed by derivation (`SYS_IOPORT_CONTROL_NARROW`, T164/T171).  The kernel decides no device policy at all.  A-18 removed the LAST ambient authority: `SYS_VSPACE_SELF`, `SYS_CSPACE_SELF` and `SYS_TCB_SELF` handed a thread capabilities to its own address space, CSpace and thread asking for no capability at all.  All three are RETIRED — delegated at `IRIS_CPTR_OWN_VSPACE`/`OWN_CSPACE`/`OWN_TCB` for services, in BootInfo for the root task (which is seL4's arrangement), and for a thread the loader never saw, in the ENTRY REGISTER: the trampoline delivers the thread argument in `rdi` as well as `rbx`, so a thread written in C reads its own TCB capability as a parameter.  **A-21 removed the last ambient RESOURCE** — the kernel-global PCID bitmap that named every address space for free — and **A-24 the last ambient SERVICE**: `SYS_SLEEP`, `SYS_CLOCK_NANOSLEEP` and `SYS_NOTIFY_WAIT_TIMEOUT` let any thread ask the kernel to hold a deadline for it, and waiting is now a capability to a ring-3 timer service that can also be refused.  `mdb_legacy_roots` 32 → 25 (A-20 and A-21 each add one permanent boot-path root, `SchedControl` and `ASIDControl`) |
| No kernel heap | **met** | The kernel's slab is a BOOT ARENA and it is SEALED at the end of boot: allocating from it afterwards panics.  seL4 has no kernel heap because its boot code carves the root task's initial objects from a statically-known region and describes everything else as Untyped — which is exactly what this is, now that the door shuts behind it.  The purity gate's reachability check runs with ZERO exemptions and over the TRANSITIVE closure (A-16): no syscall handler can reach the allocator through any chain of calls, not merely by naming its caller, and T318 reads the seal from ring 3 so the property cannot stop being true unobserved |
| MCS scheduling | **met** | all four pillars are in as of Stage 8-mcs, and A-22 changed how the fourth is DELIVERED: a timeout fault is an IPC message on its own endpoint, like every other fault, so a temporal supervisor is a server.  Budget and period are enforced; **sporadic replenishment** returns every tick consumed exactly one period later, so a thread can never spend more than its budget in any window of its period (host R-1..R-8); **timeout faults** make an overrun a policy decision a temporal supervisor takes rather than an invisible stall (`SYS_TCB_SET_TIMEOUT_HANDLER`, T307); and **SC donation** lends a client's scheduling context to a PASSIVE server for the duration of a Call, so an SC-less thread runs on the requester's time instead of — as it did before — running unbudgeted (T308).  `SYS_REPLY_RECV` closes the last of them (T309): without it a passive server is, between reply and receive, runnable with no scheduling context — and an SC-less thread is not charged, so it runs unbudgeted for exactly as long as the second syscall takes.  `refill_max` is now the SC's own, chosen at RETYPE and sizing the object (T315): a passive server woken per request needs a deep replenishment queue and a periodic task needs two, and the memory is charged to whoever asked for the depth instead of every SC paying for the worst case out of the kernel.  **`SchedControl` landed with the A-20 audit that found it missing.**  A budget and a period reach a scheduling context only through `IRIS_BOOTCAP_SCHED_CONTROL` — a boot capability carried in BootInfo the way seL4 carries `seL4_CapSchedControl` — so holding the SC says WHICH context to configure and holding this says you may configure one at all.  Priority is bounded the same way: `SYS_TCB_SET_PRIORITY` takes an AUTHORITY and refuses above its ceiling, a thread inherits the ceiling of whoever configured it, and the capability-free `SYS_THREAD_PRIORITY` is retired (T327) |
| ABI shape | **met** | `SYS_INVOKE(cptr, label, …)` — one door, and a method cannot be named without naming the capability it acts on.  **A-33 finished the other half**: a message is a MessageInfo word and message registers, `struct IrisMsg` is deleted, and there is no address on the message path — nothing to validate, nothing for a second thread to unmap between the check and the copy, and a short message never touches memory at either end.  A receive reports the RIGHTS a delivered capability landed with, which is more than seL4's `extraCaps` says and is recorded as a deliberate difference.  The numbered table is three calls that invoke nothing (`EXIT`, `YIELD`, `CLOCK_GET`), which is why seL4 keeps `seL4_Yield`.  Labels are one flat list, as seL4's are; the type is checked inside the method by the resolver that asks for what it needs, answering WRONG_TYPE (A-30) where seL4 answers IllegalOperation.  Closed at **A-32** after five stages with both doors open and a counter of numbered calls that had to reach zero; it did.  Pinned by T337 and by `test_syscall_dispatch` DS-6, which tries every number from 0 to 400.  Two divergences recorded rather than rounded away: IRIS folds seL4's IPC syscalls into the same door, and the slot methods hang off the slot rather than off a CNode.  Binary compatibility was never sought and still is not |
| Object lifetime | **close** | seL4 has no per-object reference count: an object exists while a capability to it exists, and `cteDelete`/`finaliseCap` walk the derivation tree.  IRIS reaches the same ANSWER through two counters, and that is now measured rather than asserted — T322 checks the rule for every retypeable type, T323 over generated derivation shapes, T321 through a CSpace cycle (where IRIS and seL4 behave identically: neither collects it idly, both reclaim it when the Untyped is revoked).  The mechanism difference is registered and permanent (D-7).  What it cost is recorded too: a donated scheduling context was released twice because the loan moved a pointer and not a reference, and the object hit refcount 0 with a slot still naming it — found by T324, which reads every pool slot because nothing else ever reads an idle one |
| Kernel architecture | **met** | D-1, the only one of these that was a rewrite rather than an increment, is CLOSED.  IRIS has ONE kernel stack per core and no thread blocks inside the kernel.  No blocking syscall keeps live state across its block (step 1); a parked one abandons its frame (step 2, T310); the whole ring-3 register context lives in the TCB (step 3, T314); and `TSS.RSP0` is set once and never changes, because a DISPATCHER on the core's stack replaced `context_switch` — which is deleted, along with `task_yield`, `scheduler_sleep_current`, the idle task and `kstack_alloc`.  T318 measures the consequence from ring 3: eight threads, and the kernel's physical reserve does not move, where the old per-thread stacks would have cost two pages each |

### What no further stage closes

**The proof, and it is the only one left.**  This section has named four things
over its life and three of them turned out to be work rather than identity.

The first was D-1 — the event-kernel rewrite, described here as "the reason
seL4 can bound in-kernel latency and be verified, and converting to it is a
rewrite of every blocking path... not an increment".  It was a rewrite, it was
done (Stage 9-evt), and the text describing it as open outlived it by several
stages.

The second was the ABI shape, carried as a permanent decision on the grounds
that converting would rewrite every caller to gain nothing measurable.  The
cost estimate was right; the gain was undervalued, and A-32 closed it.

The third was the message — `struct IrisMsg`, split out of the ABI row by A-32
and priced the same way, at 339 ring-3 sites "for nothing this charter measures".
A-33 closed that too, and the gain it had not counted was that a message with
no address is a message the kernel never dereferences.

Both are worth leaving on the record for the same reason: a roadmap's stalest
paragraph is usually the one that was written most confidently, and twice now
it has been the paragraph explaining why something would not be done.

### If it needs a number

**Capability semantics: done, with two registered permanent divergences (the
rights set, D-3, and refcount lifetime, D-7).  Kernel architecture: done.  ABI
shape: done (A-32).  Message ABI: done (A-33).**

The old figure here — 75% on semantics, 25% on architecture, with the advice
not to average them — was honest when D-1 was open and is not a description of
anything now.  A single number was the wrong instrument then and there is
nothing left for it to measure.

### What this review found — all of it now closed

A file-by-file re-read after the form divergences closed (ledger A-26).  None
of these was a hole in the authority model; all are kept here with what closing
them found, because the finding and the fix are more useful together than the
fix alone.  Two of the five turned out to be wrong as stated: the transfer
divergence was recorded as permanent and should not have been (A-29), and the
"123 dead `#define`s" was a count of unreferenced table entries rather than of
dead code (A-31 notes, item 5).

1. **IPC capability transfer is a MOVE.**  ***Closed by ledger A-29 — transfer
   is a COPY now.***  Sending a capability over an endpoint deleted the
   sender's source slot.  seL4 COPIES: the sender keeps its capability, gated
   by the Grant right.  Both are coherent — IRIS's was strictly more
   conservative — but a client that wanted to keep what it sent had to derive a
   copy per send, and nothing in the documentation said so until the timer
   client (A-24) ran into it.  What settled it was not the seL4 comparison but
   the kernel's own tree: the delivered capability was already installed as an
   MDB child of the sender's slot, and then the parent was deleted.
2. **Two live syscalls are leftovers.**  ***Closed: both retired (A-27), and
   T001 pins that they answer NOT_SUPPORTED.***  `SYS_GETPID` handed a thread
   its own id for the asking — ambient INFORMATION rather than authority, so no
   invariant is violated, but seL4 has no equivalent and nothing productive
   uses it.  `SYS_THREAD_EXIT` duplicates `SYS_EXIT`, which also records the
   exit code.  Both are small retirements nobody has needed yet.
3. **`SYS_CLOCK_GET` is an ambient read of the clock.**  ***Closed by being
   ANSWERED rather than retired (A-27).***  Retiring it buys nothing: `rdtsc`
   is unprivileged on x86 and IRIS never sets `CR4.TSD`, so any task can read a
   monotonic counter with one instruction whether the syscall exists or not.
   The call is now a capability question — ask the clock's owner — and T002
   pins that a granted clock answers and advances.
4. **Four seL4 invocations have no equivalent.**  ***Closed: five now exist
   (A-28), pinned by T333.***  None was load-bearing for anything IRIS did,
   which is exactly why they went unnoticed: `seL4_TCB_ReadRegisters`/`CopyRegisters` (a supervisor can
   write a thread's registers but not read them),
   `seL4_SchedContext_YieldTo`/`Consumed`, `seL4_IRQHandler_Clear`, and
   cross-CNode `seL4_CNode_Move` (IRIS moves within a CNode with
   `SYS_CNODE_SWAP`; across CNodes it is a mint-then-delete, which reaches the
   same place with a different derivation shape).
5. **`kprocess.c` is misnamed.**  ***Closed: it is `kfault.c` now, and
   `context_switch.S` — which has held only the FPU save/restore since Stage
   9-evt deleted the switch — is `fpu_switch.S`.***  `struct KProcess` was
   deleted in Stage 7-proc and the file was fault delivery and its counters
   for four stages under the old name.

   The same audit listed "123 dead `#define`s" alongside these, and that count
   did not survive being looked at.  Measured properly there are 87
   unreferenced object-like macros, and almost none of them are dead code: 83
   belong to `svcmgr_proto.h` and `kbd_proto.h`, two headers describing RETIRED
   KChannel protocols whose own text says they are kept as historical wire
   records; the rest are permanently-reserved syscall numbers, entries of the
   `SYS_CAP_IDENTIFY` wire-type table, `*_POOL_SIZE 0u` markers stating per
   type that nothing is slab-allocated, and halves of pairs (`USER_SPACE_BASE`
   with `_TOP`, `TASK_PRIORITY_MIN` with `_MAX`, two of seven named badges).
   Pruning a table to its referenced entries makes a specification worse, not
   smaller.  Exactly two were genuinely dead — `KSTACK_PAGE_SIZE` and
   `PGR_FSLOT` — and are gone.  If the retired protocol headers should go, they
   go whole, and that is a separate decision from this one.

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
- Order: DELIVER, then release the staging refs.  MDB parenting needs the
  source slot occupied — and since ledger A-29 it stays occupied, because the
  transfer is a COPY (seL4's semantics).  There is no longer a commit/abort
  distinction: the sender keeps its capability whether the message landed or
  not.  A cap revoked while staged is never delivered (entry invariant 4).
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

## Stage 8-cap — the capability model's last gaps  ← CLOSED (D-2, D-8 and D-4 closed; D-3 decided and registered as a permanent divergence)

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

## Stage 9-evt — the event kernel  ← CLOSED (all three steps)

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

**Step 3 — ONE kernel stack per core. ✅ CLOSED.**  The paragraphs that used
to stand here described the state mid-stage and outlived it; they are replaced
rather than kept, because a roadmap that says a closed step is half done is
worse than one that says nothing.

What it needed, and the first analysis of this stage missed it: it is not
enough for syscalls to stop keeping state on the stack.  A timer interrupt
fires while a task runs in USER mode and lands on the kernel stack named by
`TSS.RSP0`; if that stack is per-core and the ISR then preempts to another
task, the outgoing task's interrupt frame sits on a stack the incoming task is
about to use.  So step 3 additionally required converting the PREEMPTION path
to save the full user context into the TCB — a second conversion, comparable
in size to the first.  Syscalls were never the obstacle: `SFMASK` clears IF, so
no syscall is preempted mid-flight, and step 1's restart points are the only
places a thread gives up the CPU inside the kernel.

All of it landed.  `struct iris_user_ctx` is a field of `struct task`; every
ring-3 entry saves the thread's whole register state into its own TCB and every
ring-3 exit rebuilds the frame from the TCB of whatever thread is current by
then.  `TSS.RSP0` and the syscall stack pointer are set ONCE, in
`core_dispatch_init`, and are the core's for its lifetime — they used to be
rewritten on every context switch, because the stack belonged to whichever
thread was about to run.  `context_switch` is deleted: a reschedule is a choice
of which TCB the exit path restores, not a swap of kernel stacks.  And the
per-thread stacks went with it, so kernel memory stopped scaling with thread
count and a retyped TCB stopped costing memory its payer did not pay for.

Measured rather than assumed (`irq_ctx_saves`, and T314's register-integrity
spin across thousands of preemptions), because a path whose effect is invisible
is a path that rots.

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

## Stage 13-form — the four FORM divergences  ✅ 3 of 4 CLOSED, the fourth decided

The A-20 audit read the whole kernel and the whole test suite against seL4's
actual API and asked what was MISSING, rather than whether each recorded item
was done.  It found six things no row had named.  Three authority holes closed
in A-20 itself.  The rest were called "form divergences" — differences in shape
rather than in what the system can express — and three of them turned out not
to be about shape at all.

### Step 1 — address-space identity is a capability  ✅ DONE (A-21)

`ASIDControl` carves POOLS; an `ASIDPool` is a retyped object owning a range of
identifiers; `SYS_ASID_POOL_ASSIGN` issues one; `ktcb_configure` refuses a
VSpace that has not been given one.  Before this the kernel handed every
retyped VSpace an identifier out of a global bitmap: creating an address space
required no authority beyond the memory, how many could exist was a constant
compiled into the kernel, and when it ran out nothing in ring 3 could see it
coming.

The check is deliberately independent of `iris_pcid_enabled`.  A machine with
no tag register allocates the same identifiers from the same pools and drops
them on the way to CR3, so the rule a program obeys is the same everywhere —
the alternative would have made the model decorative on exactly the
configurations where it is cheapest to be wrong about it.

**Gauge**: T328, whose first claim is the load-bearing one — a freshly retyped
VSpace is refused by `TCB_CONFIGURE` — plus `IRIS_ASID_POOL_SIZE + 8`
build-and-destroy rounds through one pool, which can only finish if every
identifier came back.

### Step 2 — a fault is an IPC message  ✅ DONE (A-22)

The faulting thread CALLS an endpoint.  The handler receives the record as an
ordinary message, gets a reply capability with it, and replying resumes the
thread.  `SYS_EXCEPTION_RESUME` and `SYS_TCB_FAULT_INFO` are retired.

What went with them is the point.  The mailbox was a hand-rolled capability
delivery, with its own parent tracking so revoke could reach the copies the
kernel handed out, and it meant a pager held a TCB capability — authority over
everything a thread can be made to do — for every target it served.  The
generation number was a hand-rolled one-shot token.  A badge does the first and
a reply capability is the second.

One property was genuinely lost, and it is the point: a supervisor could PEEK
at a fault it intended somebody else to serve, because observation and delivery
were separate mechanisms.  They are one now.

**Gauges**: T329 (a badged fault on a shared endpoint, `RIGHT_READ` as the
authority to take delivery), and T185, which keeps a COPY of a live reply
capability, spends the original, lets the thread refault, and shows the copy
answers neither fault.

### Step 3 — the kernel cannot block a thread on time  ✅ DONE (A-24, A-23)

`SYS_SLEEP`, `SYS_CLOCK_NANOSLEEP` and `SYS_NOTIFY_WAIT_TIMEOUT` are retired,
and with them `TASK_SLEEPING`, `timed_out`, both timed notification waits, the
expiry sweep in the tick and the sleeper half of the idle fast-forward.
`wake_tick` survives with one meaning instead of three: when a scheduling
context's budget comes back.

Waiting is a ring-3 SERVICE.  It holds the timer interrupt — the kernel offers
the tick through the ordinary IRQ routing, keeping it for preemption and MCS
accounting as seL4's kernel does with its own — takes "signal this notification
in N nanoseconds" over an endpoint, and signals.  A task that holds no timer
capability cannot wait on time, which was never true of a syscall number.

The service is single-threaded and could not have been written at all before
`SYS_TCB_BIND_NOTIFICATION` (A-23): a thread blocked receiving on an endpoint
was deaf to signals, so both drivers in this system busy-waited on a kernel
timeout to serve an interrupt and a request queue at once.  Both lost their
timeout with the bind, which is the second-order proof that it was the missing
piece rather than a convenience.

**Gauges**: T331 (the service fires, the endpoint is the authority, all three
numbers answer NOT_SUPPORTED), T330 (a signal reaches a thread blocked on an
endpoint, and a pending one is consulted on the way in), and T310, whose
restartable-syscall claim moved from `SYS_SLEEP` to `SYS_NOTIFY_WAIT`
unchanged.

### Step 4 — the ABI shape  ✅ DONE (A-32)

It read: 62 live numbered syscalls against seL4's handful of invocations on
capabilities; the authority semantics are equivalent, the verification surface
is not; what an invocation ABI would buy is a smaller kernel entry surface and
one place to check authority instead of sixty, and what it would cost is
rewriting every caller in the system to gain nothing this charter measures.

The cost was as advertised: five stages, every service, the assembly driver and
1,139 sites in the suite.  The gain was not what the row predicted.  A smaller
entry surface was the smaller half; the larger half is that "a syscall selects
a method and never an object" stopped being a rule the kernel obeyed and
nothing checked, and became the only thing the ABI can express.

### And the audit's last item  ✅ DONE (A-25)

`seL4_CNode_CancelBadgedSends`.  Revoking a badged capability stopped a client
sending anything NEW and left whatever it had already queued to be delivered
afterwards, to a server that had just been told this client no longer exists.
`SYS_EP_CANCEL_BADGED_SENDS` dequeues them and returns a count, so a supervisor
can tell a revoke that had a tail from one that did not.  The capability must
be UNBADGED: a badged one names a client, and cancelling by badge through it
would let that client silence any other by naming their number.

**Gauge**: T332.


## Stage 9 — SMP

Hard precondition: single authority namespace (4), CDT (1), lifecycle (0),
CSpace-only IPC (2), **a documented locking model**, and Stage 9-evt.

The 9-evt precondition was not a preference: every atomicity property here is
re-derived against the kernel's execution model, and doing it once against a
blocking multi-stack kernel and again against an event kernel means doing it
twice, with the second pass carrying the assumptions of the first.  That is
closed.

The LOCKING MODEL precondition was not, and this section is it.  What follows
was measured against the tree, not designed for it: the hierarchy is the order
the code already takes locks in, and the catalog is what a grep for shared
mutable state actually returns.  A plan that invented either would be worse
than none, because it would disagree with the code in ways nobody would notice
until a deadlock.

### 9.0 — What is already there

More than the "pending" label suggests, and it changes the shape of the work:

| Present | State |
|---|---|
| Spinlocks | REAL — `atomic_flag` with acquire/release, not uniprocessor no-ops |
| Per-CPU data | `cpu_local[MAX_CPUS]`, GS-relative with a self-pointer, offsets pinned by `syscall_entry.S` |
| Per-CPU TSS + IST stacks | Arrays already indexed by `cpu_id` and sized `MAX_CPUS` |
| Per-CPU run queues | `cpu_rqs[MAX_CPUS]`, each with its own lock; threads carry `home_cpu` |
| Per-CPU kernel stacks | `core_stacks[MAX_CPUS]` — Stage 9-evt's whole point |
| IPI | `lapic_send_ipi` and a reschedule vector wired into the IDT |
| AP bring-up recipe | Written down in `gdt.c`, four steps, never executed |

This table is what §9.0 found when the stage opened.  Every row but the last is
now closed; it is kept in the shape it was written because the point of §9.0 is
what the stage STARTED from.

| Absent, when the stage opened | Consequence, then | Now |
|---|---|---|
| AP startup | No MADT parse, no trampoline, no INIT-SIPI-SIPI.  `lapic_send_ipi` does FIXED delivery only | step 3 |
| TLB shootdown | `paging.c` says so in as many words.  An unmap is one `invlpg` on the CPU that ran it | step 2 |
| Per-CPU timer | The tick is the PIT: one global source, one CPU | **still the PIT, on purpose** — step 4 split the tick instead and IPIs the other cores, so the machine keeps ONE clock.  Per-core APIC timers need four calibrations of a quantity MCS deadlines are counted in |
| A lock on `sched_thread_list` | There is none, and it is walked twice per idle | step 1 |

### 9.1 — The lock hierarchy

**Every acquisition must go down this list, never up.**  The order is not
chosen: each edge below was read out of a call path that exists today, and the
file:line is given so a future change can check whether its claim is still
true.

| # | Lock | Scope |
|---|---|---|
| 1 | `mdb_lock` | global — the one derivation tree |
| 2 | `KEndpoint.lock` | per object |
| 3 | `KVSpace.lock` | per object |
| 4 | `live_lock` (knotification registry) | global |
| 5 | `sched_list_lock` | global — the scheduler's list of live threads |
| 6 | `KCNode.lock`, `KObject.lock`, `KAsidPool.lock`, `KSchedContext.lock`, `task.obj_lock` | per object |
| 7 | `dom_lock` | global — the domain schedule's cursor |
| 8 | `CpuRunQueue.lock` | per CPU — **leaf, nothing may be taken under it** |

**Enforced**: `make check-locks` (`scripts/check_lock_order.py`) holds the
table above as data and reports any edge that goes up it, following calls three
hops deep.  It runs in CI next to the purity gate.  A static check rather than
a test, for the reason the whole section exists — an inversion cannot happen on
one core, so there is nothing for a test to observe until the day it is a hang.
Changing the order means changing that table and saying why.

Observed edges, all of them:

| Order | Where |
|---|---|
| `mdb_lock` → `KCNode.lock` | `kcnode.c` 381→401, and `mdb_relocate` 488/493 under a caller's `mdb_lock` |
| `KEndpoint.lock` → `task.obj_lock` | `kendpoint.c:54`, via `kfault_resolve` |
| `KEndpoint.lock` → `CpuRunQueue.lock` | `kendpoint.c:57`, via `task_wakeup` → `rq_enqueue` |
| `KVSpace.lock` → `KAsidPool.lock` | `kvspace.c:63→67`, via `kasidpool_take` |
| `live_lock` → `KObject.lock` | `knotification.c:166` |
| `sched_list_lock` → `CpuRunQueue.lock` | `scheduler.c`, both walks, via `task_wakeup` |

Three locks are BOOT-ONLY and cannot contend once the system is running, which
is worth stating because it removes them from the analysis rather than leaving
them to be reasoned about every time:

- `kslab_lock` — the arena is SEALED after boot (`kslab_seal`), and an
  allocation after that asserts;
- `pmm_lock` — every `pmm_alloc_*` caller is on the boot path, and
  `check_purity` now proves no syscall handler can reach one;
- `kvspace_boot_lock` — the pre-boot mapping arena.

The rest (`klog_lock`, `irq_lock`, `reap_queue_lock`) are leaves.

### 9.2 — The catalog: shared mutable state

This is what Stage 9's one-line "re-derive EVERY atomicity property" expands
to.  The old catalog named four items (IPC staging, RETYPE2, reply bind,
teardown); it was not wrong, it was a quarter of the list.

**Unprotected, and that is only safe on one core** — ✅ **all closed, step 1**:

| State | Was | Now |
|---|---|---|
| `sched_thread_list` | plain pointer, no lock; walked three times in `scheduler.c` and mutated on every create/destroy | `sched_list_lock`, IRQ-off because the TICK is one of the walkers |
| `scheduler_ticks`, `wall_ticks` | `volatile uint64_t` — `volatile` orders nothing and is not atomic | `_Atomic`, relaxed.  The idle fast-forward became a monotonic MAX via compare-exchange: it was a read, a decision and a write with the clock free to move between them |
| `iris_cur_domain` | plain, read by every dispatcher on every dispatch | `_Atomic uint8_t`, relaxed — nothing is published through it, the queues it selects have their own lock |
| `dom_sched_idx`, `dom_ticks_left` | plain | `dom_lock`.  Advancing the cursor is a read-modify-write across two variables that must happen ONCE per tick however many CPUs tick, or a domain's slot ends at twice the rate the schedule says |
| `next_id` | plain `uint32_t`, three incrementing call sites | `_Atomic`.  A diagnostic that hands two threads the same id is a diagnostic that lies |
| `reap_queue_hwm` | plain | **the catalog was wrong**: the write was always under `reap_queue_lock` and only the read was bare.  Made `_Atomic` so the type says what the code does |

One more found while closing these, which the catalog had missed and which is
now ✅ closed: the whole sporadic-replenishment family — `refill_reset`,
`charge_tick`, `flush_run`, `apply_refills` — mutated a scheduling context's
ring buffer without taking `sc->lock`.  The object HAD a lock and three
functions used it (configure, bind, unbind); the half a RUNNING system touches
did not.  On one core the tick, the dispatcher and the idle walk cannot
interleave; on two they are three CPUs writing one ring.

The `_locked` split it needed is forced rather than stylistic: `configure`
already holds the lock when it resets the ring, and these spinlocks are not
reentrant, so a single locking version would have deadlocked the configure
path against itself on its first call.

**Protected, but by an argument rather than a lock** — ten sites whose comment
said the kernel is uniprocessor or non-preemptive.  ✅ **All re-derived, step
1.**  The outcome is worth recording as three different outcomes rather than
one, because "we checked them" says less than what checking found:

| Site | Subject | Outcome |
|---|---|---|
| `syscall_untyped.c` | RETYPE2's validate-then-act window | **Comment wrong, code right.**  The occupancy scan is an optimisation; the authoritative check is the exclusive install under the tree's lock, and a lost race rolls back every object and un-bumps the carve exactly |
| `kuntyped.c` | retype's all-or-nothing child allocation | **Comment conflated two exclusions.**  An IRQ-off spinlock excludes other CPUs *and* a handler on this one; only the second ever needed a premise, and it was never "uniprocessor" |
| `kcnode.c` overwrite mint | delete-with-reparent | **REAL DEFECT — fixed.**  See below |
| `kcnode.c` revoke | a case called impossible | **Comment wrong, code already correct.**  A preemptible revoke DROPS the tree lock between batches, so its own root can be deleted underneath it — and the code already answers NOT_FOUND or stops and reports what it revoked |
| `syscall_reply.c` bind | check-then-bind on a reply object | **Structural, not temporal.**  The receiver was dequeued, so nobody else delivers to it, and only the receiver could change its staged object — and it is blocked |
| `syscall_reply.c` ReplyRecv | the composed reply-then-receive | **Half the claim fails, and it is the harmless half.**  "Nothing is scheduled between them" is false with two cores — `sys_reply` marks the client READY and another CPU may dispatch it.  That is fine: the client got its scheduling context back *before* the wakeup, so it runs on its own time.  The property that matters — the server never running unbudgeted in the gap — holds because the server is ON-CPU with IF cleared (`MSR_SFMASK`), which is not about the core count |
| `syscall_endpoint.c` | a bind that "cannot fail" | **Structural.**  The staged object belongs to the receiver, which is the thread executing the syscall |
| `syscall_frame.c`, `syscall.h` | TLB invalidation is one local `invlpg` | **Genuinely step 2.**  Relabelled as such rather than left reading like a closed question |

**The one real defect**: the overwrite mint (`kcnode_slot_install_linked` with
`exclusive == 0`) deleted the old occupant in one lock hold and installed the
new capability in another.  The comment said no mutator could slip between
them because the kernel is uniprocessor — adding that if one ever could, the
install would fail cleanly with `ALREADY_EXISTS`.

That second half is not clean.  The delete has already happened, so a caller
whose install loses the race is left with a slot holding NEITHER capability:
it destroyed the occupant, installed nothing, and got back an error saying
"occupied", which is the one thing the slot is not.  Delete and install now
happen under ONE hold of `mdb_lock`, so the slot goes from the old capability
to the new one with nothing observable in between.

**Already SMP-shaped**, and worth recording so the audit does not revisit them:
`cpu_local[].current_task` is per-CPU; the `_Atomic` statistics counters are
relaxed and independent; `kernel_cr3` is write-once at boot.

### 9.3 — The steps, and why this order

**Step 1 — make the one-core kernel SMP-correct, before a second core exists.**
✅ **DONE.**  Every change here was made, reviewed and tested on one core;
doing it after bring-up would mean debugging races and bring-up at once, with
no way to tell which was lying.

What it came to:

- `sched_thread_list` gets `sched_list_lock`, IRQ-off because the TICK is one
  of its walkers;
- the tick counters become `_Atomic` and the idle fast-forward becomes a
  monotonic MAX rather than a read-decide-write;
- the domain schedule splits: the current domain is an atomic byte every
  dispatcher reads, the CURSOR takes `dom_lock` because advancing it must
  happen once per tick however many CPUs tick;
- the sporadic-replenishment family takes the lock its object already had —
  four functions that the RUNNING system touches and that had none;
- the ten arguments of §9.2 are re-derived: **one was a real defect** (the
  overwrite mint could destroy an occupant and install nothing), four were
  structural with the comment crediting the wrong thing, one was already
  handled, one is half-false in its harmless half, and two are step 2's;
- the reap ring's capacity is DERIVED from `MAX_CPUS` instead of estimated,
  and its refusal is counted and pinned at zero by **T344**.

**Gate**: four gates green — the three that existed plus `make check-locks`,
which did not.  §9.2's "unprotected" table is empty.

What step 1 did NOT do, deliberately: per-CPU dead lists.  They are the better
shape for a reason that is not correctness — they remove a cross-CPU cache
line — so they belong with the per-CPU timer in step 4.

**Step 2 — TLB shootdown.**  ✅ **DONE**, with the honest caveat below.

The design turned out smaller than expected, and the reason is worth keeping.
IRIS loads CR3 on every dispatch with **bit 63 clear**, and with `CR4.PCIDE`
set that invalidates every TLB entry for the PCID being loaded.  So a CPU that
switches INTO an address space throws away what it had cached for it on the
way in, free, on a write it was making anyway.  Without PCID a CR3 write
flushes everything — the same conclusion, more bluntly.

That leaves exactly one case needing an IPI: a CPU running a thread in that
address space RIGHT NOW, which will not reload CR3 until it switches away.  So
the shootdown targets the CPUs whose `current_task` names the VSpace, and on a
machine where nobody else is in it, it sends nothing and takes no lock.

Synchronous, with no timeout and no fallback: the caller's next act is usually
to free the frame, and a CPU that does not answer is a CPU that is wedged with
interrupts off.  Continuing past it would mean freeing memory it can still
write.  Hanging is a worse-looking failure and a better one.

Wired into the two places that remove an entry from a LIVE address space:
`kframe_unmap_all` (which now takes the VSpace as well as the CR3 — teardown
paths pass NULL, because a VSpace dies when its last capability goes and a
running thread holds one through its TCB) and `PageTable_Unmap`, where an
interior entry is exactly what a paging-structure cache holds.

**The caveat as it stood when step 2 closed**: there was one CPU, so the target
set was always empty and the cross-CPU path had never executed.  **Step 4 ran
it** — with threads spread across four processors the shootdown fires, and
T345 now asks the machine how many processors it has and checks the right thing
for the answer.  What T345 pinned then, and still pins on one processor, is
that an unmap still issues its local `invlpg` and that zero IPIs are sent.  The
second is not a formality: the target scan skips the
CALLING CPU, and without that skip a shootdown would IPI itself and spin for an
acknowledgement it cannot deliver, with interrupts off.  The first unmap would
hang the machine.  Zero is the evidence the skip works.

The cross-CPU behaviour is **step 3's to test**, when there is a second CPU to
test it with.

**Step 3 — discover and start the APs.**  ✅ **DONE.**  Four processors come
up on a four-CPU machine and the full suite passes; one CPU is unchanged.

The chain: the bootloader forwards the ACPI RSDP out of the EFI configuration
table (BootInfo 2 → 3), because that table stops existing at ExitBootServices
and nobody can ask afterwards.  The kernel walks RSDP → XSDT → MADT, reads
processor entries, and stops — every other ACPI table describes something that
is a ring-3 concern here.  Then a real-mode trampoline, INIT-SIPI-SIPI, and the
four steps `gdt.c` had written down and never executed.

**The APs park with interrupts off, scheduling nothing**, which is the
checkpoint rather than an unfinished implementation: "N processors are up" can
fail entirely on its own, and keeping it separate from "N processors are
running threads" means a failure in either is legible.

**Gated**: the smoke script takes `IRIS_QEMU_SMP`, checks the MADT count
against it, and checks that every processor actually ARRIVED — a processor that
does not start is reported and left alone rather than hung on, so without the
second gate a machine quietly running on half its cores looks healthy.

**What bring-up cost, recorded because the next architecture will cost it
again.**  Four bugs, none of which produced any output at all:

| Symptom | Cause |
|---|---|
| Boot hangs, no output | The trampoline page was CHOSEN from the memory map rather than CLAIMED from the PMM — which initialises from that same map, so the page was already somebody's.  The copy corrupted live memory |
| APs execute a descriptor table | The trampoline's GDT was written at offset 0x20 — inside its own real-mode entry code |
| Reaches protected mode, dies at `mov cr0` | Physical 0..2 MiB is mapped **NX**, and the instruction after enabling paging fetches from exactly that page |
| Reaches 64-bit, faults entering C | The AP never set **EFER.NXE**.  The BSP did, so the kernel's tables have bit 63 set on every non-executable mapping — and with NXE clear that bit is RESERVED, so the first touch of its own stack is a reserved-bit fault |

Every one was found with `AP_TRAMPOLINE_TRACE`, which writes a stage byte
straight to COM1.  The in-memory progress bytes the BSP reads on failure were
useless for all four, because a faulting AP took the machine down before the
BSP could read them.  The switch stays, off, with the two signatures in its
comment.

**Step 4 — let the APs schedule.**  ✅ **DONE.**  Four processors dispatch
threads on a four-CPU machine and the full suite passes on both `-smp 1` and
`-smp 4`.

**The tick: the PIT stays the single source and IPIs the others.**  The tick
split in two along a line that was always there and never had to be drawn —
what is true of the MACHINE (the clock, the domain schedule, the replenishment
sweep, the idle fast-forward) and what is true of a CORE (charging the running
thread's budget, noticing a higher-priority thread, running a time slice down).
The first has one owner; the second every core does for itself, from a tick IPI
on vector 0xF2.

Per-core APIC timers are what seL4 uses and what this should eventually be.
They are not what this is for one reason worth stating: a per-core timer has to
be CALIBRATED, and four calibrations give four slightly different ideas of how
long a tick is — while `kschedctx_charge_tick` takes the tick NUMBER and every
MCS deadline is expressed in it.  One timer and an IPI has one clock by
construction.  The cost is three interrupts per tick, three hundred a second at
100 Hz: real, bounded, and the thing to fix when there is a reason to.

**Threads distribute** round-robin over the processors that are actually
online, chosen once when a TCB is configured.  The root task and the idle
thread stay on the boot processor.  Nothing MIGRATES — a kernel that moves
threads has to decide when, and "when" is a policy that belongs to ring 3.

**What making them schedule actually cost.**  Six defects, and not one of them
was in the SMP code written for step 4.  Every one was an existing correctness
argument that had "there is one processor" inside it, unstated:

| Symptom | Cause |
|---|---|
| Kernel instruction fetch from inside `core_stacks`, no output | Both ring-3 entry paths wrote `IA32_KERNEL_GS_BASE = &cpu_local[0]` as a link-time constant.  A thread started on CPU 2 took its first syscall with CPU 0's per-CPU block, so `%gs:48` handed it CPU 0's kernel stack — two processors on one stack |
| Threads die with #UD on some cores and not others | An AP leaves INIT with CR0/CR4 at RESET values.  **CR4.OSFXSR** was never set on it, so every SSE instruction a thread executed there was an invalid opcode.  The same register was missing **PCIDE** — and the dispatcher ORs a PCID into CR3's low bits, which without PCIDE are part of the page-table ADDRESS — and **SMEP/SMAP**, absent silently on a machine that reports them on |
| Nondeterministic corruption under load | "Which thread is running" was one global pointer.  The tick charged CPU 1's budget to CPU 0's thread; a teardown asked "is this thread still on a CPU" about the wrong one |
| A thread resumed while another core was still releasing it | A thread becomes wakeable the instant it blocks, which is several hundred instructions before the core it was on has finished saving its FPU and flushing its scheduling context.  Closed with `task->on_cpu`: raised by the dispatcher that commits to a thread, lowered by the one that has finished releasing it, and the picker spins on it holding no lock |
| `Suspend` returned success and the thread kept running; `kill` killed nothing | Changing a thread's state from another core changes nothing about the core executing it.  Both now mark and send a reschedule IPI; the external kill marks DEAD exactly as a self-exit does and the owning core hands it to the reap ring |
| An unlocked list spliced on every thread create and destroy | `task_list_head`'s circular `task->next` list.  It had two uses — a pointer comparison meaning "the idle thread", and a walk to find a dying thread's predecessor — and no readers.  DELETED rather than locked; the list the kernel uses is `sched_thread_list` |

**And three defects in the TEST SUITE, which are worth separating from the
kernel's because they are a different kind of mistake.**  Every one was a wait
that had quietly stopped waiting:

- `it_settle(n)` was `n*16+8` yields.  On one core every yield was a dispatch,
  so counting yields counted other threads' turns.  On four it is that many
  fast syscalls on THIS core, all of which can complete before another core
  takes a single timer interrupt.  It now has a floor of real elapsed TIME.
- `it_quiesce_reaper` was 200 yields, with "on single-CPU" in its own comment.
  It now waits on the condition: `deaths_pending` reaching zero, which counts
  the reap ring's depth PLUS the threads marked DEAD that have not reached it —
  the second half being the one a killed thread on another core sits in.
- `it_fault_wait_ep` was 3000 polls.  T308 waits for a TIMEOUT fault, which
  cannot arrive until a server has burned a budget measured in ticks; a bound
  expressed in this thread's syscalls cannot express "three ticks from now".

Two more assertions were reading a GLOBAL counter to make a claim about the
CALLING thread — the syscall-restart gauge (T310, T311) and the shootdown
gauge (T345).  The first got a per-thread counterpart; the second became a
test that asks the machine how many processors it has and checks the right
thing for the answer.

**Gated**: `IRIS_QEMU_SMP=4` runs the full suite, the smoke script scales its
timeout with the core count (four vCPUs under TCG need it, and a timeout reads
exactly like a hang), and **T346** pins the claim itself — every processor that
is online has dispatched a thread, and the tick broadcast is still advancing.
`online=4 dispatching=4` is what step 4 means; step 3 could have said
`online=4 dispatching=1`.

**Step 5 — the adversarial phase.**  ✅ **DONE.**  Four tests that AIM four
processors at one object and check something only a properly serialised kernel
can satisfy.  **They found four defects, all of them real, none of them in the
code written for SMP.**

The distinction the step exists for: after step 4 the whole suite passed on
four processors, and that is a weaker statement than it sounds.  Its threads
happened to be spread across cores, so it exercised whatever interleavings fell
out — never the ones it did not happen to produce.  These do not merely RUN on
several processors; each is pointed at one piece of machinery.

| Test | Aimed at | Reports |
|---|---|---|
| **T347** | the reply object and the endpoint queue: four callers on four cores calling one server, each requiring ITS OWN answer.  A reply delivered to the wrong caller is the classic multiprocessor IPC defect and is invisible to a test with one client | `cores=4 attempts≈1400 won=1400` |
| **T348** | the derivation tree: four cores minting and deleting from one capability while a fifth revokes it underneath them | `cores=4 attempts≈250000` |
| **T349** | the retype sequence: four cores retyping into the SAME slot, where exactly one may win and the losers must lose cleanly.  §9.2 re-derived that path's atomicity and found the code right for a different reason than its comment gave; this runs it | `cores=4 attempts≈550000 won≈400` |
| **T350** | step 4's death machinery: four cores killing the same four threads, so every thread is killed four times and one of those kills races the thread's own core | `cores=4 attempts≈15000` |

**What they found.**

| Defect | What it was |
|---|---|
| **A rollback that freed another core's memory** | RETYPE2 recorded its carve window with two separate reads of `ut->used`, one on each side of the allocation.  On one processor those bracket exactly this caller's blocks; on four they bracket whatever else was carved in between, and the failure path un-bumped the whole window — handing a live block back to the allocator while another core's object sat in it.  Two cores then built objects in one block, the first destroy zero-filled it, and the second release read a header of zeroes.  **The window is reported by the allocator now**, under the hold that reserved it: only that code can answer the question |
| **Release, then use** | `sys_tcb_exit` dropped the resolve's reference on the line ABOVE `task_kill_external(target)` and then dereferenced the pointer.  Safe on one processor by an argument nobody wrote down — nothing else was running — and on four the other core's kill completes the teardown in that window |
| **A teardown gate that was not a gate** | `task->terminal` was a plain byte, set near the END of teardown and tested by an unlocked read at the top.  Four cores calling Exit on one thread all passed that test, so all four tore the same thread down: the registry slot, the CSpace, the address space and the scheduling context each released four times.  It is `_Atomic` and claimed with an **exchange** now, which makes "am I the one" and "say so" a single act |
| **A dispatch that overwrote a Suspend** | `Suspend` takes a thread out of the run queue — but a thread ALREADY DEQUEUED cannot be taken out of a queue it is no longer in.  Between the dequeue and the commit it belongs to no queue and to no processor, and a `Suspend` landing there set SUSPENDED on a thread the dispatcher then marked RUNNING, clearing `need_resched` on the way: the suspend was simply lost and the caller was told its thread had stopped.  The dispatcher re-checks after the `on_cpu` hand-over now and drops a SUSPENDED choice.  **SUSPENDED and nothing else**, which cost a wrong turn worth recording: the first version dropped anything that was not READY, on the reasoning that a queued thread is a runnable thread.  That is not true of this kernel — a thread is put in a queue and its state written by two different pieces of code, so one can legitimately be queued while BLOCKED_REPLY — and dropping a single such thread wedged the whole system.  Found by **T333**, which suspends a thread and then reads its registers, an operation the kernel refuses for a RUNNING one |

Three of the four present identically — `kobject_retain: resurrect from
refcount 0`, or a refcount underflow, on an object whose `type` field reads 0,
a type nothing creates.  That is the signature of a header that has already been zero-filled
by `kuntyped_release_child`, and it says use-after-free without saying whose.
So the three reference asserts now NAME the object — type, both counters, and
whether its storage came from an Untyped or the slab — on the panic's own
channel rather than through `klog`, which is a ring that ring 3 drains and the
machine halts before anybody asks.  That one line is what turned each of these
from "something is wrong" into a specific path in an afternoon.

**What step 5 could not do.**  There is no `TCB_SetAffinity`: nothing migrates
a thread, so a test cannot CHOOSE which processors contend.  It can only read
where the round robin put them, which `iris_tcb_info.home_cpu` now reports —
so each test says how many distinct cores its workers actually landed on, and a
run that was taking turns rather than contending is legible instead of
indistinguishable.  Adding affinity means deciding when a thread may move,
which is a scheduling policy and belongs to ring 3; it is a genuine seL4
invocation IRIS does not have, and it is recorded as that rather than smuggled
in to make a test more convenient.

**And what it cannot prove**, restated because it is easy to overclaim from a
green run: §9.4.  QEMU's TCG interleaves and does not reorder.  These find
LOGIC races — two cores reaching one structure in an order nobody arranged for
— and they will not find a wrong `memory_order` on a relaxed atomic.  Four
defects found this way is evidence the method works, not evidence the kernel is
now free of the other kind.

**And the rest of the suite's waits were converted rather than left to be found
one at a time.**  Step 4 caught three yield-bounded waits by having them fail
(T083, T118, T308); twenty-nine more of the same shape —
`for (i = 0; i < N && !flag; i++) yield;` — were still there, each one a wait
that had stopped waiting the moment the thread it waits for could be on another
processor.  They are one `IT_AWAIT(cond, ticks)` each now, bounded in elapsed
time.  Fixing them after they flake is how the first three were found; it is
not a method.

**Still open, and named rather than implied**: the model-based fuzzer is not
extended to N cores.  It RUNS there and passes, and T108 already races two of
its workers against one endpoint while a third closes it — what it does not do
is aim its whole operation set at shared objects the way these four tests do.
That is the next thing this file should say is done.

There is also no `TCB_SetAffinity`, so nothing here CHOOSES which processors
contend; each test reads where the round robin put its workers
(`iris_tcb_info.home_cpu`) and reports it.

### 9.4 — What this stage cannot prove

**QEMU/TCG does not reproduce memory-ordering bugs.**  It interleaves, so it
finds logic races, and it will not find a wrong `memory_order` on a relaxed
atomic.  Steps 1–4 are verifiable here; step 5 tells us when the system LOOKS
correct on this emulator, which is not the same claim and should not be written
up as one.  Saying so now is cheaper than discovering it in a ledger row later.

**The deadlock direction is untestable until step 3.**  A lock-order inversion
cannot happen on one core, so §9.1 is enforced by review until there are two.

## Stage 10-dma — device authority must be containable  ← ALL 6 STEPS DONE

This is a SECURITY hole in the capability model, not a platform feature, which
is why it is pulled out of Stage 10's list and given a stage of its own.

A driver holding an I/O port or IRQ capability can program a DMA-capable device
to read or write ANY physical address — including the kernel's own memory and
every other task's.  Every guarantee the rest of this roadmap builds is void
against such a driver.  The capability model makes this worse rather than
better in one specific way: the whole point of user-space drivers is that a
compromised driver is contained by the capabilities it holds, and without an
IOMMU that containment is fiction.

### 10.0 — What exists, and one thing that turns out not to be needed

`kernel/core/acpi/acpi.c` walks RSDP → XSDT/RSDT → MADT and stops, on purpose
(see the note at the top of `iris/acpi.h`).  Finding a second table is an
extension of the walk, not a new parser.  There is no IOMMU code and no PCI
code.

**The kernel does not need PCI enumeration, and that is worth stating before
the steps because the old version of this section said it did.**  To install a
translation for a device you need its source-id — the PCI bus:device:function
that appears on the bus with every DMA request.  You do NOT need to have
DISCOVERED it: the source-id is a PARAMETER of the authority, travelling on the
capability, exactly as an I/O port range travels on an `IOPORT_CONTROL`
derivation (Stage 5).  seL4 is built the same way — its kernel enumerates no
PCI either; a root task that scans the bus asks for an IOSpace for a device it
found.

So enumeration is something ring 3 needs in order to make this USEFUL.  It is
not something the kernel needs in order to make it SAFE, and a kernel that
grew a PCI scanner would be bending charter P1/P2 for no gain.

### 10.1 — The model

Three things, and they are seL4's three:

| Object | Is | Comes from |
|---|---|---|
| `IOSpaceControl` | the authority to name a device at all | BootInfo, once, like SchedControl and ASIDControl |
| `KIOSpace` | ONE device's DMA address space, identified by (remapping unit, source-id) | minted from `IOSpaceControl` with the source-id as the parameter |
| `KIOPageTable` | one level of that address space's translation tables | retyped from an Untyped, like every other object |

And the operation that matters: a FRAME is mapped into an IOSpace at a DMA
address with rights.  A device's reach is then exactly the set of frames
somebody mapped for it — nameable, delegatable, and revocable by the ordinary
CDT, because the mapping is a derivation of the frame capability like any
other.

The property to hold onto, stated so a later step can be checked against it:
**a device whose source-id no capability names must reach nothing.**  Not "the
default is identity" and not "the default is unconfigured" — blocked.

### 10.2 — The steps

**Step 1 — find the remapping units.**  ✅ **DONE.**  The DMAR is parsed: each
DRHD's register base, PCI segment, INCLUDE_PCI_ALL flag and device-scope count,
plus the table's host address width.  Nothing is mapped and nothing is enabled;
a machine with no DMAR says so and runs exactly as it did before.  Same shape
as SMP step 3, and for the same reason: "the hardware is there and I found it"
is a claim that can fail entirely on its own.

`acpi.c`'s MADT-only walk became a walk that takes a signature, because the
second table needs the same four RSDP checks and duplicating them is how the
two would eventually disagree about which RSDP is valid.  The DMAR's LAYOUT is
read in `kernel/core/iommu/iommu.c`, not in `acpi.c` — that file finds bytes
and checksums them, and keeping interpretation out of it is what stops it
becoming the place tables accumulate.

**Measured, and it changes step 3**: QEMU's `-device intel-iommu` emits one
64-byte DRHD with INCLUDE_PCI_ALL **clear** and six explicit device scopes.  So
a source-id no scope names is a source-id no unit claims, and the honest answer
to a request for that device's IOSpace is a refusal — not a translation
installed on the assumption that one unit covers everything.

**Gated** both ways by the smoke script: with `IRIS_QEMU_IOMMU=1` the kernel
must FIND a unit, and without it the kernel must still SAY something — a kernel
that silently found nothing and a kernel that silently skipped looking read the
same from outside.

**Step 2 — read what they can do.**  ✅ **DONE.**  Each unit's VER, CAP and
ECAP are read and decoded: guest address width, the page-table depths it
supports, how many domain ids it has, caching mode, whether the write buffer
must be flushed, whether its page walk is coherent, whether it offers queued
invalidation.  A unit that does not answer — an unmapped register reads as all
ones, and a decoded `~0` claims support for everything — is reported and left
alone rather than believed.

The registers need no new mapping: `paging_init` maps the low 4 GiB and the
unit sits at 0xFED90000, reached through the same physmap window the LAPIC is.

`usable` is decided against what the later steps actually require, and the
refusal names WHICH assumption broke — "the IOMMU is not usable" helps nobody.
Queued invalidation is deliberately NOT required: the register interface is
always present and is what step 5 will use.

**Two measured facts that shape step 3**, both read off QEMU's unit
(`cap 0xd2008c22260206 ecap 0xf00f4a`, decoded and checked by hand against the
specification):

| Fact | Consequence |
|---|---|
| SAGAW offers **3 levels only** (39-bit) | the IO page table is three levels on this hardware, not four.  A walker written for four and run on this would build a table the unit reads as garbage |
| **ECAP.C is 0 — the page walk is NOT coherent** | every root, context and page-table line IRIS writes has to be flushed out of the CPU cache before the unit reads it.  A kernel that skipped that would install translations the hardware never sees, and the symptom is a device that still reaches everything — protection that is not there, reported as present |

**Step 3 — translation ON, and everything blocked.**  ✅ **DONE.**  The root
table is built and the unit switched into translating mode with NO entry
present, so every DMA request from every device is refused by the hardware.
That is §10.1's property standing on its own, before any object exists that
could relax it.

An empty root table is also the cheapest possible default: one 4 KiB page per
unit, no context tables, and a request from any bus faults on the root entry.
Building 256 context tables to hold nothing would be the same answer at a
thousand times the memory.

Three things made it harder than setting a bit, and each is a comment in
`iommu.c` rather than a line of code somebody has to re-derive:

| | |
|---|---|
| GCMD is WRITE-ONLY and one-shot | the register holds a command, not a state; the enabled bits are SHADOWED, because a read-modify-write on a register that cannot be read is a bug that works until the second command |
| The page walk is not cache-coherent | a root table written by the CPU sits in the CPU's cache and the unit reads memory.  Without a flush the unit walks whatever was there before — not "translations that do not work" but translations the hardware never sees, presenting as a device that still reaches everything |
| Both caches hold the firmware's leftovers | the context cache and the IOTLB are emptied, in order, through commands that must be waited on |

It is called LAST in boot, after the first user task is built.  The firmware's
devices DMA, and this is what stops DMA nobody authorised; IRIS does none of
its own after ExitBootServices, but "there is nothing left that needs it" is a
claim about the boot sequence and is made where that sequence is finished.

**Gated** by the smoke script (`DMA is contained` must appear when a unit was
found) and by **T351**, which asserts the property from ring 3 and asserts the
opposite one on a machine with no unit: nothing usable, nothing translating,
and no containment CLAIMED.  A gauge that reported success with no hardware to
enforce it would be worse than no gauge, because every later test reading it
would pass for the wrong reason.

**Step 4 — the objects, and mapping.**  ✅ **DONE.**  `KIOSpace` and
`KIOPageTable` are retypable objects, `IOSpaceControl` is a BootInfo authority,
and four invocations do the work: bind, map a level, map a frame, unmap.

The split that matters: **retyping an IOSpace is paying for an object**, which
anyone holding an Untyped may do, and it names no device.  **Binding it to a
source-id is authority** — IOSPACE_CONTROL — because that is where "which
hardware may reach what" is decided.  Same shape as retyping a scheduling
context and then configuring it against SchedControl.

The holder pays for every page-table level out of its own Untyped, which is
why `MISSING_TABLE` is an answer the kernel gives rather than an allocation it
makes.  A device's rights on a frame are what the caller asked for narrowed by
what the caller HOLDS — a holder that could hand a device more than its own
frame capability carries would be laundering authority through the one grantee
that cannot be asked what it holds.

**Two bounded pools with declared limits, refused by name**, because this runs
on a syscall path and an allocator reachable from a capability invocation is
what charter M1 and the purity gate exist to prevent: eight context tables
(one per (unit, PCI bus) in use) and sixteen mappings per device.  "IRIS
supports eight device buses" is a fact somebody can read rather than a limit
somebody discovers.

**One honest limitation, and it is the shape of the next piece of work.**  A
DRHD without INCLUDE_PCI_ALL says which devices it covers through its device
SCOPES, and IRIS records the scope count but not the paths — so it cannot match
a source-id against them.  With exactly one remapping unit that does not matter
and the code says why: a unit is the hardware every DMA request in its segment
passes through, so attributing a device to the only unit that could translate
it is not a guess.  With MORE than one and no INCLUDE_PCI_ALL it refuses, which
is the only other honest answer — an IOSpace installed on the wrong unit is a
capability promising containment the hardware never enforces.

**Step 5 — revocation, and the IOTLB.**  ✅ **DONE.**  `IOSpace_Unmap` removes
the entry, flushes the line and invalidates the unit's translation cache, in
that order — the other order leaves a window in which the unit refills its
cache from a table line the CPU has not written back, which is a revoke that
undoes itself.

Destroying the IOSpace does the same for everything at once: the context entry
goes FIRST, so the device is blocked whatever the tables still say, and only
then are the levels and the frames released.  Until the context entry is gone
the device has a live translation into tables about to be handed back to an
Untyped, and the order is the whole difference between a teardown and a
use-after-free with a bus master on the other end.

**T352** walks the whole arc and both endings: bind refused without the
authority, rebinding refused, levels installed one at a time, a frame mapped,
mapped twice refused, unmapped, unmapped twice refused — and then a frame
mapped and the SPACE destroyed with the mapping live, where the baseline is
what proves every object came back.

**Step 6 — watch a device be refused.**  ✅ **DONE.**

This step used to be a paragraph saying what the stage could not prove.  It
said: there is no DMA engine under IRIS's control in the test environment, so
nothing here watches a device be refused; every claim is about what the kernel
ACCEPTED and REFUSED and about what the translation tables say, not about a bus
transaction that failed.  That was honest and it was a hole, because every
other check in the stage is consistent with a kernel that programmed the
hardware perfectly and was ignored by it.

There is a device now.  `-device edu` is on the QEMU command line of every
headless run — a PCI function with one MMIO BAR, a four-kilobyte internal
buffer and a DMA engine that copies between that buffer and whatever physical
address a driver writes into its registers — and **T353** is a ring-3 driver
for it.  The driver derives an I/O-port capability for 0xCF8/0xCFC, enumerates
bus 0, finds 1234:11e8, reads the window its BAR decodes, retypes a frame over
that window out of the PCI-hole device Untyped, maps it uncached, checks the
device answers through it, and points the device's DMA engine at a frame it
retyped from its own Untyped.

The proof is one frame with two halves.  The processor writes a pattern into
the first half; the device is told to copy that pattern into its buffer and
copy it back out into the second half, which holds a sentinel.  Reading the
second half afterwards needs no interpretation:

| configuration | what the second half holds |
| --- | --- |
| a unit, no IOSpace mapping | the sentinel, untouched — and the unit's fault record names source-id 0x10 |
| a unit, the frame mapped | the pattern — the device reached exactly what it was granted |
| a unit, the mapping revoked | the sentinel again — and a second fault record |
| **no unit at all** | the pattern — the device reached memory nobody granted it |

The middle rows are what make the first one mean anything.  A sentinel left
standing is also what a DMA that was never issued looks like, and a test that
stopped there would pass just as happily against a device that was not present.
The only thing that differs between the first row and the second is one
`INV_IOSPACE_MAP_FRAME`.  The last row is not a fallback but the other half of
the claim, and it is why the device is attached on runs with no IOMMU too: a
suite that could only be run in the configuration that passes is evidence of
nothing.  `scripts/run_qemu_headless.sh` requires the device line on every
selftest run and each configuration's own markers.

**What the driver cost, which is the part worth keeping.**  Three defects, all
of them pre-existing and none of them findable by anything in steps 1–5:

1. `paging_virt_to_phys` returned `entry & ~0xFFF`, which clears the low flags
   of a page-table entry and keeps every HIGH one — so the "physical address"
   of any non-executable page had **NX set at bit 63**.  Every caller in the
   kernel had got away with it, because they compared the result against zero
   or fed it back into another walk that masked it again.  The first caller to
   hand such an address to HARDWARE was `iommu_context_set`, writing a VT-d
   root entry whose bits 63:39 are reserved: the unit answered with fault
   reason 10 and refused every device on the bus, in both arms of the test.
   Fixed with `PAGE_PA_MASK` / `PAGE_PA_MASK_2M` in `paging.h`, applied to
   every walk; the AP trampoline's CR3 was carrying the same passenger.
2. The I/O-port ABI had **only byte-wide** `IN`/`OUT`.  PCI configuration space
   is reached through a 32-bit index register at 0xCF8 that discards anything
   narrower, so a byte-at-a-time driver writing the four bytes of a config
   address writes four values that are each thrown away: the kernel could not
   host a PCI driver at all.  `INV_IOPORT_IN16/OUT16/IN32/OUT32` are seL4's
   `seL4_X86_IOPort_In8/16/32` family, and the range check had to become
   `offset + width <= count` — a dword read at the last byte of a range reads
   three bytes the capability does not cover.
3. Every frame mapping was **write-back**.  A driver mapping a BAR needs the
   opposite, so `SYS_FRAME_MAP` grew flag bit 2 for an uncached mapping
   (`PAGE_PCD | PAGE_PWT`).  It is a property of the MAPPING, not of the frame.

Plus one addition that is not a defect: `INV_IOSPACE_FAULT`, on the
IOSPACE_CONTROL authority, reporting the unit's fault RECORD — source-id,
address, reason, direction — rather than only the status bit `SYS_SCHED_INFO`
already exposed.  "A fault happened" and "MY device was refused" are different
claims, and only the second is worth anything to a driver: an idle SATA
controller touching memory nobody mapped for it sets the same status bit.

**And what it still does not prove.**  The device is emulated.  What T353
establishes is that IRIS programs a VT-d unit correctly enough that a bus
master behind it reaches exactly the frames somebody mapped and nothing else,
as QEMU implements VT-d.  A real unit has errata, a real machine has devices
behind bridges whose source-id is not their devfn, and neither is exercised
here.  The bus scan is bus 0, function 0, because that is the machine: a
general enumerator would be code no test covers.

### 10.3 — What is NOT in this stage

The I/O port whitelist is already gone (Stage 5): the range a holder may claim
travels on the `IOPORT_CONTROL` capability and is narrowed by derivation.  What
this stage adds is the same bound for DMA, which is the one reach a port or IRQ
capability still cannot express.

Interrupt remapping is not here.  It is a separate VT-d facility, it protects
against a different attack (a device forging an interrupt vector), and mixing
it in would mean two claims failing as one.

## Stage 10-abi — freeze the ABI  ✅ CLOSED

A product that other people build on has a versioned, stable ABI.  The
paragraph that used to open this stage said IRIS had "96 live syscalls", which
was true when it was written and had been wrong since ledger A-32 retired the
numbered door — a sentence in a document that nothing checks is a sentence that
will be wrong, and that is the whole lesson of the stage.

**The surface is declared in one file and asserted by a test.**
`kernel/include/iris/abi.h` is the contract; `tests/kernel/test_abi.c` is what
makes it one.

| | |
|---|---|
| syscall numbers | **four**: `SYS_INVOKE`, plus `SYS_EXIT`/`SYS_YIELD`/`SYS_CLOCK_GET`, which are here for the reason seL4 keeps `seL4_Yield` — they name no capability |
| invocation labels | **77**, numbered 0..76, with no holes.  Label 0 names nothing and is refused exactly as an unassigned number is |
| reserved numbers | **136**, permanently.  A number is never reused, so a caller built against an older kernel gets a refusal rather than somebody else's method |

The test does not check a list — a list would have to be maintained alongside
the thing it describes, which is the failure being fixed.  It walks every
number the dispatcher can see and every label in the declared range, and tells
an ASSIGNED label from an unassigned one by the answer: a real method refuses
with `INVALID_ARG` on its first line, an unassigned label falls through to
`NOT_SUPPORTED`.  Adding a label without extending the declaration fails.

**Versioning rides in BootInfo** (`abi_major` / `abi_minor`, v9), not behind an
invocation.  The version is a fact about the KERNEL, so there is no capability
to invoke it on, and a syscall for it would have been a fifth numbered door in
all but name.  The root task halts the boot on a major it was not built
against — otherwise a caller discovers the mismatch one `NOT_SUPPORTED` at a
time, from a method that used to exist, with no author.

**Three rules make growth safe**, each promoting a precedent that already
existed to a rule: a versioned struct is a PREFIX and fields are only appended;
an unknown flag bit is REFUSED rather than ignored, which is exactly what lets
a later minor version define one; an error code is part of the contract, so
changing which of `WRONG_TYPE` / `ACCESS_DENIED` / `NOT_SUPPORTED` a case
produces is a MAJOR change even though the call still fails.

**The naming residue is gone, and it was not only naming.**  `handle_id_t` and
`HANDLE_INVALID` survived in 1341 places from a namespace Stage 4 deleted; the
type is `iris_cptr_t` everywhere now, `nc/handle.h` is `nc/cptr.h`, and
`CPTR_NULL` and `IRIS_CPTR_NULL` are one name instead of two.  One of those
places was a defect: `sys_sc_configure` truncated a 64-bit capability argument
to 32 bits and widened it again, folding values ABOVE the CPtr boundary back
inside the valid range — the one thing that boundary exists to prevent.

**The §5.1 walk the stage required** found six ledger rows naming a retirement
stage that had closed without anyone returning to them, which is the exact
failure §5.1 was written to stop.  All six are answered in ledger A-35.

## Stage 10 — General-purpose platform  ← PARTLY DELIVERED

Precondition: consolidated microkernel (0–9 as applicable), 10-dma, 10-abi —
all met.

This stage is a LIST rather than a claim, and the honest way to report it is
item by item.  The table below has NINE rows and **eight of them are settled**:
seven delivered and gated, one declined on the record.  The ninth cannot be
done in this environment at all, and no amount of work here changes that.

(The count said seven of eight until networking closed, and it was already one
short of its own table then — `user-space drivers` is a row and was not being
counted.  A total that does not match the list under it is the kind of error
that survives because nobody re-adds it.)

| item | state |
|---|---|
| user-space drivers | ✅ **three of them** — a DMA engine, a disk and a network card — and none holds anything the others do |
| PCI | ✅ `services/pci` — the bus is a service, and the only task that can reach configuration space |
| ACPI | ✅ reachable from ring 3 as capabilities; **T354** reads the root pointer out of firmware memory |
| storage | ✅ `services/blk` — an AHCI driver in ring 3; **T355** reads sector zero and checks the boot signature |
| | |
| persistent FS | ✅ `services/fs` on a disk IRIS owns.  `make smoke-persist` boots twice over one image: the first formats and reports generation 1, the second finds it and reports 2, and then the HOST reads the bytes off the image |
| networking | ✅ **a driver and a stack, as two services.**  `services/net` is an e1000 driver in ring 3 that moves Ethernet frames and parses nothing; `services/ip` is ARP, IPv4 and UDP ABOVE it, holding an endpoint to the driver and no hardware authority of its own.  The gate is a TFTP read completed against QEMU's gateway — a peer that is not this machine accepted the ARP, the IPv4 checksum and the UDP pseudo-header checksum, and answered.  There is no TCP, no fragment reassembly and no sockets, which is stated below rather than rounded |
| POSIX personality | ⊘ **declined, and recorded as a deliberate divergence** in charter §6.  It needs no kernel change — that is the point of the capability model — and all of it is policy.  The sharper objection: POSIX's ambient authority is the thing thirteen stages removed |
| performance | ✅ **T356** measures an invocation, an IPC round trip and a disk read, prints the numbers, and fails on order-of-magnitude regressions.  They are TCG figures and are not presented as hardware ones |
| real hardware | ◐ **it has run on one, once.**  On 2026-09-25 IRIS booted a real x86-64 desktop from a partition on its own disk: the kernel came up, the screen carried the log (there is no serial port on that machine), `pci` enumerated 41 functions across PCI bridges, `blk` found both SATA drives, `fs` recognised the IRIS partition, formatted it, and wrote a report into it — which was then read back from the other operating system, off the medium, with nobody transcribing anything.  What that is NOT is real-hardware SUPPORT: it is one machine, observed once, with no gate.  Every automated check in this repository still runs under QEMU.  Its network card is not an e1000, so `net` found nothing and `ip` had nothing to say.  The honest claim is that the item moved from *cannot be attempted* to *attempted, and here is exactly what happened* |

### What the delivered items are

**`pci`, the bus as a service.**  Configuration space is one pair of I/O ports
through which any device on the machine can be reprogrammed, so a capability
for it is a capability over the bus.  Handing that to every driver would have
undone Stage 10-dma one port range at a time — contain a device's DMA, then
hand out the config space that programs the DMA.  So one task holds those
ports: init derives them once, gives them to `pci`, and **deletes its own
copy**.  `pci` also owns the PCI-hole device Untyped, which is what makes the
restriction enforceable rather than conventional: a driver holds no device
Untyped, so there is no window it could retype a frame over.

A driver asks for its device by identity or by class and gets back a frame over
that device's register window and the source-id the device puts on the bus —
the two things a driver needs and the only two.

**ACPI, reachable.**  The tables live in memory the firmware marked
RECLAIMABLE or NVS, which is neither usable RAM nor unmapped address space, so
no capability in the system named those bytes.  The kernel's refusal to
interpret ACPI was therefore not a delegation but a gap: anything ring 3 wanted
to know about the machine, it could learn only if the kernel had already
decided to tell it.  Those regions are device Untypeds now and BootInfo carries
the RSDP, because a region is not a starting point.

**`fs`, a filesystem that survives the power going off.**  It holds the least
of any service in the tree: an endpoint, a reply object, an endpoint to the
BLOCK service, and memory.  No disk, no controller, no device Untyped, no DMA
authority — the thing that owns your data holds no hardware at all, which is
what the whole stack was built to make possible.

Its format is a superblock, a fixed directory and one sector per file, and that
is deliberate rather than unfinished: it is the smallest thing that makes
"persistent" a TESTABLE claim.  `make smoke-persist` boots twice over one image
and then reads the image from the HOST — which does not depend on IRIS being
self-consistent about anything, and is the only check here that cannot be done
in a single run.

Making it work found the real defect underneath: **the disk driver reported
writes complete that were still in a cache**.  It now issues `FLUSH CACHE EXT`
after every write, because otherwise "persistent" is a claim about a cache.

**`net`, an e1000 driver in ring 3.**  The same five capabilities as the disk
driver, which is the point worth noticing: two drivers for completely
different hardware need exactly the same manifest, because "drive a PCI device
with DMA" is one shape and this system has a name for each part of it.  It is
also the clearest case in the tree for `IOSpaceControl` — a disk controller
reads a command table when told to, but a NIC reads a RING of physical
addresses continuously and nothing tells it to stop.  It moves frames and
parses nothing; the stack above it builds the ARP request, because a driver
that understood ARP would be policy inside a driver.

**`ip`, a stack above that driver and nothing else.**  It holds one capability
that reaches hardware at all — an endpoint to `net` — and no ports, no device
Untyped and no DMA authority, which is the whole reason the driver parses
nothing: ARP, IPv4 and UDP can be replaced without reimplementing an e1000.

Its gate is a TFTP read against the server QEMU's userspace network carries at
the gateway, and every part of that is something only a real peer can confirm.
A server that answers has accepted an ARP reply this stack built, an IPv4
header whose checksum it recomputed, and a UDP header whose checksum covers a
pseudo-header — get any of the three wrong and the datagram is dropped in
silence, which is what makes a transmit-only check worthless.  The reply is
matched to the EPHEMERAL port the request went out from, because a TFTP server
answers from a port of its own, and a stack that ignored ports would read back
whatever happened to arrive first.

Two things fell out of writing it.  The driver's receive buffers were 256
bytes, which is smaller than a datagram — a real reply arrived split across
four descriptors and no amount of correctness above that could reassemble it,
so the buffers are 1024 bytes now.  And the poll loop had to become a
DISPATCHER rather than a filter: an ARP request for our address that arrives
while the stack is waiting for a datagram must be answered, or the peer never
learns where we are and the datagram never comes.  Which also means the thing
being waited for needs a KIND and not just a port, so that a stray ARP reply
does not end a wait for a datagram.

**`blk`, an AHCI driver in ring 3.**  It asks `pci` for a SATA controller by
CLASS code — a driver that matched vendor:device would drive one machine —
maps BAR5 uncached, builds command structures in memory it owns, and issues
`READ DMA EXT`.  It is the driver that most needs Stage 10-dma, because AHCI
takes physical addresses FROM ITS DRIVER: on a machine with a remapping unit it
binds an IOSpace to its controller and maps only its own two buffers, and on a
machine without one the controller reaches all of memory.  It reports which,
and the gate requires the right answer for the machine it is on.

### Preparing for a machine this repository cannot test on

Real hardware stays ⛔, and none of the following changes that.  What it
changes is the cost of the first attempt: a boot that fails on an unknown
machine should produce a diagnosis rather than a black screen, and a boot that
succeeds should not destroy anything.  All three items below are gated under
QEMU like everything else.

**The screen is a diagnostic surface now.**  Every gate here reads the serial
port and every kernel diagnostic goes to 0x3F8 — reasonable under emulation,
nothing at all on a machine with no serial port, which is most of them.
`fbcon` paints the kernel log onto the framebuffer, and `make smoke-screen`
decodes it back to text with the kernel's own font to prove it is legible.  The
boot markers sit on a line that never scrolls, so a boot that stops says where.

**A disk is addressed through a partition, not absolutely.**  `blk` put a
client's LBA straight into a `WRITE DMA EXT`, and `fs` wrote its superblock to
LBA 0 — the start of a test image, and the partition table of a real drive.
Every LBA is now relative to a window, the window is the GPT partition typed
`IRISFS-PARTITION`, and there is no way left to express an address outside it;
a disk with no such partition gets no window and every transfer to it is
refused.  Formatting is separately an authority, granted by a token a host
deliberately wrote.  The test image is a real GPT disk with a decoy partition,
and `make smoke-persist` proves every byte outside IRIS's own — the GPT, its
backup, the decoy — comes back unchanged.

**A missing serial port cannot wedge the boot.**  `console` spun forever
waiting for a UART transmit register to drain.  A floating bus reads 0xFF and
escapes that loop by luck; a port that decodes and never drains does not.  The
wait is bounded and a byte is dropped rather than waited on, because a log that
loses a character is a diagnostic and a log that hangs is an outage.

What is still untouched, and would be the next thing to break on a real
machine: nobody has interrupts, `pci` walks bus 0 only, and `blk` assumes the
first SATA controller answers the way QEMU's does.

### What the first real machine actually taught

Every defect the hardware found was the same shape, and naming the shape is
worth more than the list: **something true of the machine this was developed
on, written down as though it were true of machines.**

| what broke | the assumption under it |
|---|---|
| `pci` saw 24 functions and no storage controller at all | the bus is bus 0.  A desktop keeps storage behind PCI-to-PCI bridges |
| the carve stopped on a gap of 232 pages and 256 bytes | gaps between BAR windows are page-sized.  A PCI BAR is aligned to its own SIZE, so a 256-byte register block is not |
| `fs` reported a disk REFUSED on a machine with no disks | a message has a fifth word.  It has four, and the write went past the array — the same shape the audit had already found in `blk` and fixed |
| the report said `window 0  home 0` | the data disk is disk 1.  On that machine it was disk 0 |
| the scan diagnostic printed nothing in the failing case | a failure worth describing has at least one disk |

None of them is exotic.  All of them passed every gate, on every
configuration, for as long as they existed — because the gates run on the
machine the assumptions came from.  That is not an argument against the gates,
which caught real defects repeatedly; it is the limit of what a uniform test
environment can tell you, and it is the same limit the untyped-allocator
overflow demonstrated from the other direction.

### What is NOT claimed

The device-driver stack is three drivers deep and none has an interrupt: they
all poll.  `pci` walks bus 0 only — no PCI-to-PCI bridges, because this machine has
none and a bridge walk would be code no test covers.  `blk` reads and writes,
and flushes after every write, but has no queueing — one command in flight per
port.  `net` sends one frame at a time and waits for it, and its receive ring
is eight 1024-byte buffers, so a burst longer than eight frames loses the
oldest and a frame larger than 1024 bytes is dropped whole rather than handed
up in pieces.  `ip` speaks ARP, IPv4 and UDP and nothing else: no TCP, no fragment
reassembly, no sockets, no DHCP — the address is a constant — and because it
has no thread of its own it polls only while a request is outstanding, so a
frame that arrives when nobody is asking waits in the card's ring.  `fs` has no journal, no allocator and no directories — a
crash between writing a file's contents and naming it loses the space, which is
the better of the two orders and is not a transaction.  The benchmark's
ceilings are order-of-magnitude guards, not performance targets.  Each of those is named here rather than left for
a reader to discover, because a platform's gaps are part of its description.

---

## The ceiling: what this roadmap does NOT reach, and why

A roadmap that ends without stating its ceiling invites the reading that
finishing it produces seL4.  It does not, and two of the three reasons are
deliberate.

**1. ~~The ABI shape.~~  This item is RETIRED, and it is worth recording what
it used to say and why it was wrong.**

It said: seL4 has roughly a dozen syscalls and expresses every other operation
as an INVOCATION on a capability carrying a method label, IRIS has numbered
syscalls each resolving its own arguments and checking its own rights, the
charter registers that as permanent, and the consequence is that a new
operation is capability-gated BY CONSTRUCTION in seL4 and by a check the author
has to write correctly every time in IRIS.

Every sentence of that was true and stopped being true in ledger A-32, which
converted the whole surface to `SYS_INVOKE(cptr, label, …)`.  Three numbers
survive, each because it invokes nothing.  The paragraph then sat here
unchanged for several stages, describing a kernel that no longer existed — in
the very section that exists to keep this document honest about its limits,
which is the most expensive place for a stale sentence to be.  Stage 10-abi
found it, and `tests/kernel/test_abi.c` is why the replacement cannot go stale
the same way: the surface is now asserted rather than described.

What is left of the item is much smaller and is still real: IRIS's ABI is its
OWN.  Binary compatibility with seL4 was never sought and is not offered — this
is about form, not about linking — so seL4 code does not build against IRIS and
never will.  That is a deliberate divergence, registered in the charter, and it
is a different statement from the one this paragraph used to make.

**2. Formal verification.**  Out of scope, per the charter.  Worth stating
without euphemism: *the proof is seL4's identity*.  A system that converges on
seL4's model without it has converged on the design, not on the guarantee.
Stage 9-evt is the only item here that would even make the question askable.

**3. Everything else in this document has been reached.**  The object model is
Frames and Untypeds, the derivation tree's unparented capabilities are down to
the boot path seL4 has too, the kernel stops blocking, and the form divergences
are closed or decided.  What is here now is a microkernel with seL4's authority
model, seL4's object model, seL4's execution model and its own ABI — which is
an honest and defensible thing to be, and is what this project claims.

The sentence above used to be in the future tense.  Moving it is the whole
result of Stages 9-evt through 13-form.  What remains in the ceiling is now
ONE item rather than two: the proof, which is seL4's identity.  The other —
the ABI shape — closed in A-32 and this document went on listing it, which is
recorded above rather than quietly deleted.

**What A-26 listed as still open is now closed.**  Transfer is a COPY (A-29);
`SYS_GETPID` and `SYS_THREAD_EXIT` are retired and `SYS_CLOCK_GET` was
answered rather than retired, because `rdtsc` is unprivileged on x86 and
removing the syscall would have bought nothing (A-27); the four missing seL4
invocations exist (`TCB_ReadRegisters`, `SchedContext_YieldTo`,
`IRQHandler_Clear`, cross-CNode `CNode_Move` — A-28).  What remains in the
ceiling is what was always going to remain: the proof, which is seL4's
identity.  The ABI shape was listed beside it until Stage 10-abi noticed that
A-32 had closed it.

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
