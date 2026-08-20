# IRIS — Stage 5: seL4-like bootstrap (BootInfo + fine-grained boot authority)

Status: **CLOSED — all four Etapas landed**.
Precondition: [Stage 4](sel4-convergence-roadmap.md#stage-4--dual-namespace-retirement--closed)
(closed — the initial capabilities can only be CSpace now).
Normative frame: [purity charter](iris-sel4-purity-charter.md) §2.1 (A5),
§4 (mandatory end state, "bootstrap with fine-grained capabilities"), and the
[ledger](sel4-convergence-ledger.md) entry `KBootstrapCap`.

**Closing criterion**: the root task receives a structured BootInfo and a set
of fine-grained capabilities, and there is no monolithic bootstrap object left
to restrict — `KBootstrapCap` and `SYS_BOOTCAP_RESTRICT` are retired, and the
executing TCB is a retyped object configured through capabilities rather than
one drawn from a static pool.

## Where the bootstrap stood at the end of Stage 4

Stage 4 made the boot path CSpace-only: the bootstrap capability and every boot
Untyped are published into the root task's root CNode and nowhere else, RBX
carries 0, and a failed publish is fatal instead of "non-fatal because the
legacy handle still works".  What it did NOT change is what that authority *is*
and how the root task *learns* about it:

- **The inventory was a convention, not a message.**  The root task knew its
  bootstrap capability was in slot 1 and its untypeds started at slot 16
  because both sides were compiled against the same constants
  (`BOOT_CPTR_BOOTSTRAP_CAP`, `BOOT_CPTR_UNTYPED_START` in
  `kernel/include/iris/boot_info.h`), and it discovered *how many* untypeds it
  had by invoking slots until one answered `NOT_FOUND`.  A layout two
  separately-compiled artifacts agree on is not a contract; it holds until one
  of them changes, and nothing detects the moment it stops holding.
- **The authority was one object with a bitmask.**  `KBootstrapCap` carries
  `SPAWN_SERVICE | HW_ACCESS | KDEBUG | FRAMEBUFFER`, and delegating any one of
  those means minting the whole object and narrowing it with
  `SYS_BOOTCAP_RESTRICT`.  Restriction-by-cloning is not how a capability
  system delegates: the seL4 shape is separate objects (IRQControl, IOPort
  control, ASID control) that are delegated by handing over the one you mean.
- **The executing thread is not a retyped object.**  `RETYPE2(KOBJ_TCB)`
  produces a cap-complete but inactive TCB; the thread that actually runs still
  comes from the static `struct task` pool through `SYS_THREAD_CREATE`
  (ledger: "executable thread-create via pool + handle").  Its replacement
  needs CSpace/VSpace/fault-EP capabilities as arguments, which is why the
  roadmap parks it here.

## Etapas

| Step | Subject | State |
|---|---|---|
| 1 | The root task is TOLD what it holds: structured BootInfo | ✅ DONE |
| 1b | ...and stops assuming which slots are free | ✅ DONE |
| 2a | Device control splits off: IRQ and ioport control are their own capabilities | ✅ DONE |
| 2b | Debug splits off | ✅ DONE |
| 2c | Spawn, initrd and framebuffer split too; `SYS_BOOTCAP_RESTRICT` and the monolith retire | ✅ DONE |
| 3 | The root task's own objects are named caps (root CNode, initial TCB) | ✅ DONE |
| 4 | `TCB_CONFIGURE` / `TCB_WRITE_REGS`: a retyped TCB executes | ✅ DONE |

## Step 1 — the root task is told what it holds  ✅ DONE

The kernel writes a structured, self-describing **BootInfo** region and maps it
into the root task read-only and non-executable before the task ever runs; the
address arrives in RBX.

ABI: `kernel/include/iris/root_bootinfo.h` (`struct iris_root_bootinfo`).
Builder: `kernel/new_core/src/root_bootinfo.c`.  Producer: `kernel_main.c`
section 8.  Consumer: `services/userboot/main.c`.

```text
magic "IRISROOT" · version · header_bytes · total_bytes
cap_bootstrap · cap_vspace                     ← initial caps, by CPtr
cnode_slots · empty_slot_first · empty_slot_end ← the CSpace as handed over
untyped_count · untyped[] { cptr, paddr, size_bytes, is_device }
```

Not to be confused with `<iris/boot_info.h>`, which is the FIRMWARE→KERNEL
handoff (memory map + framebuffer, written by the UEFI loader).  This is the
KERNEL→ROOT-TASK handoff.

**The page is not authority.**  Charter §3.5 forbids an address standing in for
a capability, and nothing here does: every `cptr` field names a slot the kernel
had already populated, the region is read-only, and a root task that ignores it
holds exactly the same authority as one that reads it.  Fabricating a CPtr out
of the page gets whatever that slot really contains — nothing, unless the
kernel put something there.  seL4's BootInfo frame is the same shape and makes
the same non-claim.

What it replaces is **guessing**: the retired Phase 3.4 probe invoked
`BOOT_CPTR_UNTYPED_START`, ignored the answer, and documented itself as
something boot was not gated on.  A probe that cannot fail proves nothing.  In
its place userboot validates the description against the CSpace it describes —
every untyped the page claims must answer from its slot with the physical
region the page states — and a disagreement halts the boot with a serial
diagnostic instead of surfacing three services later as a missing capability.

Design decisions worth keeping:

- **The description bounds the grant.**  The drain stops when the BootInfo runs
  out of room, because a capability the root task cannot be told about is one
  it cannot name.  To keep that from silently capping the machine's memory, the
  region is two pages: a 256-slot root CNode has 240 untyped slots, one page
  describes 126, and `root_bootinfo.c` static-asserts that the region covers
  every describable slot.  Growing the root CNode or the descriptor is a build
  failure, not a boot-time surprise.
- **A root task that cannot be told is not created.**  The region is allocated
  and initialised *before* `task_spawn_user`, and a failure to allocate, write
  or map it aborts the spawn — the same fatality rule Stage 4 applied to the
  bootstrap-cap publish, for the same reason: the alternative is a root task
  that has to rediscover its own CSpace.
- **The builder is pure and host-tested.**  It touches no PMM, CSpace or task,
  so both failure modes that a successful boot cannot reveal — overrunning a
  buffer that is about to be mapped into ring 3, and describing a CSpace that
  is not the one that was built — are covered by unit tests (RBI-1..RBI-10,
  `tests/kernel/test_root_bootinfo.c`) rather than inferred from this machine's
  memory map.
- **The page is bootstrap memory of an existing category.**  It is mapped as
  KFrames registered with `kprocess_register_bootstrap_frame`, exactly like the
  root task's text and stack pages, so teardown releases it with them and no
  new lifetime rule is introduced.

**Invariants introduced**

- **B1 — the inventory is described, not conventional**: every capability the
  kernel installs in the root task's CSpace before it starts appears in
  BootInfo.  A grant that is not described is a bug; the drain is bounded by
  the description for that reason.
- **B2 — BootInfo confers nothing**: the region is read-only and names CPtrs
  that are already populated.  No syscall accepts "the BootInfo said so" as
  authority.
- **B3 — the root task validates and refuses**: magic and version are checked,
  the descriptors are checked against the CSpace, and a mismatch is fatal.  No
  fallback to constants, in either direction.
- **B4 — one writer**: only `kernel_main` writes BootInfo, and only before the
  root task runs; the region is never updated afterwards.

**Gates**: `make`, `make test-unit` (18683 pass), `make check-purity`,
`make smoke-runtime` (269/269), `make ENABLE_RUNTIME_SELFTESTS=1
smoke-runtime-selftests` (269/269 + phase-3/41 markers).

**Follow-up recorded, not done here**: `svc_loader`'s API still spells its
bootstrap-capability parameter `handle_id_t spawn_cap_h` across init, svcmgr
and vfs.  The value has been a CPtr since Stage 4; only the type name is
residue.  Renaming it is a cross-service change with no behavioural content, so
it is deliberately not mixed into an increment that changes the boot contract.

## Step 1b — the root task stops assuming which slots are free  ✅ DONE

userboot needs two slots of its own: one for the loader workspace CNode (a
spawn needs eleven capabilities alive at once, which no root CNode has room
for directly) and one for the serial `KIoPort` it mints to print a boot
diagnostic.  Both were constants chosen by reading the boot code — "slot 3 is
free in the reserved boot range", "slot 40 is free" — and a constant that is
true only until someone moves something is the same defect BootInfo exists to
remove.  Three of Phase S4's bring-up failures were slot collisions.

They now come from `empty_slot_first` / `empty_slot_end`, taken from opposite
ends of the declared free range so they cannot collide with each other, with a
refusal if the kernel left fewer than two free slots.  One constant survives:
the panic path that fires when the BootInfo itself is unreadable has nothing to
consult, and a diagnostic that guesses wrong prints nothing rather than
corrupting a slot.

## Step 2 — the monolith splits

`KBootstrapCap`'s permission bits become separate capabilities, each published
into its own BootInfo slot, each delegated by handing it over:

| Bit | Authorises | Successor | State |
|---|---|---|---|
| `HW_ACCESS` | `SYS_CAP_CREATE_IRQCAP` | IRQ control capability | ✅ 2a |
| `HW_ACCESS` | `SYS_CAP_CREATE_IOPORT` | ioport control capability | ✅ 2a |
| `FRAMEBUFFER` | `SYS_FRAMEBUFFER_VMO` | framebuffer control capability | ✅ 2c |
| `SPAWN_SERVICE` | `SYS_PROCESS_CREATE` | process control capability | ✅ 2c |
| `SPAWN_SERVICE` | `SYS_INITRD_COUNT/VMO` | initrd control capability | ✅ 2c |
| `KDEBUG` | `SYS_KLOG_DRAIN`, `SYS_SCHED_INFO`, `SYS_POWEROFF` | debug control capability | ✅ 2b |

`SYS_BOOTCAP_RESTRICT` retired with the last bit: narrowing a bitmask by
cloning an object is replaced by delegating the one capability meant and
revoking it through the CDT.  The ioport whitelist (`kioport_whitelist`, ledger
P3) is the policy that should follow the ioport control capability out of the
kernel.

### Step 2a — device control is its own authority  ✅ DONE

`IRIS_BOOTCAP_HW_ACCESS` was one bit authorising BOTH interrupt-line and
I/O-port capability creation, on an object that also carried spawn, debug and
framebuffer authority.  Concretely: **init printing a boot line to COM1 held
the authority to claim any IRQ, spawn processes and power the machine off**,
and the only way to give up one of those was `SYS_BOOTCAP_RESTRICT` — cloning
a narrowed copy of the whole object and deleting the original.

Now there are two capabilities, `IRIS_BOOTCAP_IRQ_CONTROL` and
`IRIS_BOOTCAP_IOPORT_CONTROL`, and the kernel matches them **exactly**
(`kbootcap_is`, not the subset test `kbootcap_allows`).  Exactness is what
makes the split real: a monolith that merely includes the bit does not pass, so
the only capability that authorises a syscall is the one that exists for it.

- **Publication**: `kernel_main` publishes each into its own root-CNode slot
  (`BOOT_CPTR_IRQ_CONTROL`, `BOOT_CPTR_IOPORT_CONTROL`) and records both in
  BootInfo v2 (`cap_irq_control`, `cap_ioport_control`).  Failure is fatal,
  like every other boot grant.
- **Delegation**: userboot → init → svcmgr and the suite, as CPtr sources, so
  each grant is an MDB child of the granter's slot and stays revocable.
- **Renunciation gets simpler and stronger**: svcmgr used to strip hardware
  authority with `SYS_BOOTCAP_RESTRICT` + delete — a derive-then-delete dance
  that existed only because the authority was a bit on a shared object, and
  whose "derive" half silently left the original wide if the delete was
  skipped.  It now **deletes the two control slots**.  The device caps it
  already claimed are MDB children of those slots, so they reparent onto init's
  slots and survive: svcmgr keeps its hardware and loses the authority to claim
  more.
- **Slot arithmetic**: svcmgr's own device-cap bookkeeping slots moved to
  96..127 (its registration pool now starts at 128) so the low, well-known part
  of every root CNode can hold boot control capabilities.  The suite's root
  CNode is full, so the ioport control capability took over slot 26 — formerly
  a second copy of the monolith that existed to prove an authority capability
  resolves by CPtr (T069).  That test proves the same thing with a capability
  that authorises exactly one syscall, which is what its name always claimed.

Covered by **T296**: each control capability authorises its own syscall,
neither authorises the other's, the capability they were split from authorises
neither, and an empty slot authorises nothing (which is what makes svcmgr's
delete a real loss of authority).  **T291** was re-anchored: its behavioural
oracle for `SYS_BOOTCAP_RESTRICT` moved from device creation — no longer a mask
bit — to `SYS_INITRD_COUNT` vs `SYS_SCHED_INFO`, two authorities that still
share the remaining monolith.  It dies with the mechanism in Step 2b.

### Step 2b — debug is its own authority  ✅ DONE

`IRIS_BOOTCAP_KDEBUG` authorised draining the kernel log, reading scheduler
statistics and powering the machine off — and rode on the same object as spawn
and framebuffer authority.  It is now `IRIS_BOOTCAP_DEBUG_CONTROL`, a
capability of its own, matched exactly, published at `BOOT_CPTR_DEBUG_CONTROL`
and recorded in BootInfo v3 (`cap_debug_control`).

Delegation reaches svcmgr (kernel-log drain for its diagnostics endpoint) and
the suite (~20 tests read scheduler and IPC statistics).  Neither holds spawn
authority *because of* it any more, and svcmgr's remaining mask narrowing drops
to `SPAWN_SERVICE` alone.

The child-side slot is 9, formerly `IRIS_CPTR_SVC_REPLY` — half of the legacy
service/reply KChannel pair, dead since KChannel was removed and every catalog
service became endpoint-only.  Reusing a retired constant is deliberate: root
CNodes hold 256 slots and the suite's is full, so an authority split that grew
every CSpace would stall on slot arithmetic rather than on design.  Slot 8 (the
other half) is reserved for the next split.

T296 grew a third leg: debug authority is denied to both device control
capabilities and to what is left of the monolith, and grants no device
creation itself.  T291's oracle moved again — from device creation (Step 2a)
to the framebuffer bit — because each re-anchoring is forced by the same
progress: the authority it probed with stopped being a bit and became a
capability.

### Step 2c — the monolith is gone  ✅ DONE

The last three authorities split, and with them the object itself:

| Was | Is |
|---|---|
| `IRIS_BOOTCAP_SPAWN_SERVICE` on the monolith | `IRIS_BOOTCAP_PROC_CONTROL` (`SYS_PROCESS_CREATE`) **and** `IRIS_BOOTCAP_INITRD_CONTROL` (`SYS_INITRD_COUNT/VMO`) — two authorities that were one bit |
| `IRIS_BOOTCAP_FRAMEBUFFER` on the monolith | `IRIS_BOOTCAP_FB_CONTROL` |
| `SYS_BOOTCAP_RESTRICT` (45) | RETIRED, number reserved, `NOT_SUPPORTED` |

**The monolith is not merely unused — it is unrepresentable.**
`kbootcap_alloc` refuses a kind that is zero or has more than one bit set, so a
capability that could be narrowed cannot be constructed, and every kernel check
is exact equality.  `kbootcap_allows` (the subset test) and
`kbootcap_clone_restricted` are deleted.  `BOOT_CPTR_BOOTSTRAP_CAP` (slot 1)
stays reserved and permanently empty.

Splitting `SPAWN_SERVICE` in two is the concrete least-authority result: **vfs
is a file server that held the authority to create processes**, because reading
a boot image and spawning a service were the same bit.  It now holds the initrd
capability alone.  The loader API says the same thing — `svc_load_minted_ws`
takes a process capability and an initrd capability, not one "spawn cap" — so a
caller that can read images but not spawn is expressible in the signature.

The other retirement is a simplification: `init_spawn_fb` was a
restrict-into-scratch, mint, delete dance; fb now receives the framebuffer
control capability from init's own slot, as an MDB child, in one mint.

Slots, again constrained by 256-slot root CNodes: slot 6 (`IRIS_CPTR_SPAWN_CAP`)
became `IRIS_CPTR_PROC_CONTROL`, slot 8 (the retired `IRIS_CPTR_SVC_CHAN`)
became `IRIS_CPTR_INITRD_CONTROL`, and slot 99 — kept empty in the suite for a
`NOT_FOUND` probe — became `IRIS_CPTR_FB_CONTROL`, with T134's probe moved onto
a scratch slot it deletes itself (a guarantee by construction rather than by
comment).

**T291 dies with its mechanism** — its subject was `SYS_BOOTCAP_RESTRICT`, and
what it asserted cannot be expressed once a restrictable capability cannot
exist.  T148 pins number 45 as `NOT_SUPPORTED`; T296 carries the property that
replaced it.  This is the Stage 4 rule applied unchanged: a test whose subject
is the retired mechanism dies with it; one asserting a surviving property is
rewritten.

**Boot marker**: the smoke gate stops requiring `boot bootstrap cap CSpace
grants OK` and requires `boot control caps CSpace grants OK` — a boot that
publishes only some of the six aborts the root task instead of continuing with
partial authority.

## Step 3 — the root task's own objects  ✅ DONE

The root task holds a capability to its own root CNode (`BOOT_CPTR_CNODE`) and
to its initial thread (`BOOT_CPTR_TCB`), both described in BootInfo v5
(`cap_cnode`, `cap_tcb`) and validated by userboot before it delegates
anything.  Previously the root CNode was reachable only structurally
(`proc->cspace_root`) plus the `arg0 == 0` convention, and the thread only by
asking `SYS_TCB_SELF` — so the one process those objects belonged to was the
one process that could not name them.  seL4's root task finds
`seL4_CapInitThreadCNode` and `seL4_CapInitThreadTCB` in its CSpace.

**The lifecycle consequence, and the fix.**  The root CNode capability lives
inside the CNode it names, so the CSpace is reachable from itself and the slot
holds references on it.  Nothing outside can drop them: the close callback that
empties a CNode's slots runs when its references reach zero, and the
self-reference is one of those references — the object would keep its entire
CSpace alive after the process died.  `kprocess_teardown` therefore calls
`kcnode_teardown_slots(root)` BEFORE dropping the structural references:
emptying the slots is what breaks the cycle, and doing it explicitly does not
depend on a refcount the cycle is holding up.  It is idempotent, so the close
callback afterwards finds nothing to do.

Covered by BC-11..BC-13 (`tests/kernel/test_boot_cspace.c`): a CNode can name
itself and the self-capability resolves; teardown-then-release frees it; and —
the negative control, so the suite proves the fix rather than the absence of a
symptom — releasing WITHOUT the explicit teardown leaves the object live.
userboot's validation (fatal on a wrong or missing type) is the runtime
witness that the boot path really publishes them.

**Deeper cycles are pre-existing, documented debt.**  A process can already
mint a second-level CNode into itself, and that cycle is not broken by the
root's teardown — its objects are freed only when the root's slots release
them, which the self-reference prevents.  seL4 solves the general case with
zombie capabilities during recursive delete; IRIS has no equivalent yet.  The
ledger records it; Stage 5 fixes exactly the case it introduces.

**ASID/PCID control is deliberately not added.**  The roadmap lists it because
seL4's root task holds `seL4_CapASIDControl` to make ASID pools for new
VSpaces.  In IRIS a VSpace is still created by the kernel inside process
creation and PCIDs are assigned there; there is no operation such a capability
could authorise, and adding one would be speculative surface for a mechanism
that does not exist yet.  It belongs with retypable VSpaces — Stage 6, ledger
entry M1.

## Step 4 — a retyped TCB executes  ✅ DONE

`RETYPE2(KOBJ_TCB)` has produced cap-complete but INACTIVE threads since
Phase S2: no registry slot, no kernel stack, no address space, refused by every
execution syscall.  What was missing was not the code but **the arguments** — a
thread runs in a CSpace and a VSpace, and neither was addressable as a
capability until Stages 3–5 made them so.

| Syscall | Meaning |
|---|---|
| `SYS_CSPACE_SELF` (119) | a capability to the caller's own root CNode — the CNode counterpart of `SYS_TCB_SELF` |
| `SYS_TCB_CONFIGURE` (120) | give an inactive TCB its execution state, naming the CSpace and VSpace as capabilities |
| `SYS_TCB_WRITE_REGS` (121) | say where a configured, not-yet-started thread starts |

`SYS_THREAD_CREATE` (48) is **RETIRED**, number reserved.  It carved a thread
out of the kernel's static task pool and returned a global thread id: no
capability authorised it, no Untyped paid for the storage, and the identity it
handed back was an index into a kernel array — the shape charter §3.4/§3.5
forbid.  Every in-tree thread is now created the seL4 way: retype a TCB from an
Untyped you hold, configure it with CSpace/VSpace capabilities, write its
registers, resume it.  What comes back is a capability in a slot.

`SYS_THREAD_START` (58) remains, and is Stage 7 work: it starts the FIRST
thread of a freshly created process, which still comes from the pool because a
spawner cannot yet name its child's CSpace and VSpace.

### Why `SYS_CSPACE_SELF` exists

`SYS_TCB_CONFIGURE` takes the CSpace as a capability.  Only the root task held
one to its own root CNode (Step 3); every other process reached its CSpace
through the `arg0 == 0` convention.  A syscall whose signature says *capability*
must not be satisfiable only by a convention, so processes can now ask for a
capability to their own root CNode — self-introspection, no delegation implied,
no way to name anyone else's CSpace, and no handle produced.  It is what will
let a supervisor hand a child its CSpace root once processes are composed
rather than created (Stage 7).

### Both capabilities must be the caller's own

A thread in a foreign address space is process-server work.  Accepting foreign
capabilities and running the thread somewhere else anyway would be a lie in the
signature, so the check is by object identity against `proc->cspace_root` and
`proc->vspace` — and it is a real check on real capabilities: the caller must
HOLD them, of the right type, or be refused.

### Two lifecycle defects the migration exposed, both fixed

- **The kernel stack was keyed by pool position.**  `kstack_alloc(t, idx)` used
  the task's index in the static backing array, which a TCB living inside an
  Untyped does not have.  The slot is recorded in the task now
  (`t->kstack_slot`) and keyed by the registry index, which every executing
  thread has by definition.
- **The stack outlived its key.**  Teardown released the registry slot and
  freed the kernel stack later, so a thread created in between could claim the
  slot, map its stack over the same range, and then have it unmapped by the
  first thread's teardown.  This is not theoretical: it wedged the machine
  during bring-up.  Both now happen together, IRQ-off, in the order
  *free stack, then release slot*.

Also fixed while proving it: `SYS_TCB_CONFIGURE` initially released an ACTIVE
reference on capabilities resolved with `cspace_resolve_only_obj`, which hands
back a lifecycle-only reference.  On an ordinary object that is a refcount
underflow; on the ROOT CNode it is a demolition — reaching zero active
references runs the close callback, which empties every slot of the CSpace the
caller is using.

Covered by **T297** (the gate: the retired syscall, an unconfigured TCB that
cannot be started/written/exited, wrong-type and foreign capabilities refused,
no double configure, no register rewrite after start) and by the whole suite
plus init's exception test, every thread of which is now born this way.  Three
tests changed what they compare — `it_thread_create` returns a CAPABILITY, not
a global id — which is the point: identity now comes from asking the object.
