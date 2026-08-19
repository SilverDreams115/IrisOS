# IRIS — Stage 5: seL4-like bootstrap (BootInfo + fine-grained boot authority)

Status: **OPEN — Etapa 1 landed**.
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

| Etapa | Subject | State |
|---|---|---|
| 1 | The root task is TOLD what it holds: structured BootInfo | ✅ DONE |
| 1b | ...and stops assuming which slots are free | ✅ DONE |
| 2a | Device control splits off: IRQ and ioport control are their own capabilities | ✅ DONE |
| 2b | Debug splits off | ✅ DONE |
| 2c | Spawn, initrd and framebuffer split too; `SYS_BOOTCAP_RESTRICT` and the monolith retire | pending |
| 3 | The root task's own objects are named caps (root CNode, initial TCB, ASID/PCID control) | pending |
| 4 | `TCB_CONFIGURE` / `TCB_WRITE_REGS`: a retyped TCB executes | pending |

## Etapa 1 — the root task is told what it holds  ✅ DONE

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

What it replaces is **guessing**: the retired Fase 3.4 probe invoked
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

## Etapa 1b — the root task stops assuming which slots are free  ✅ DONE

userboot needs two slots of its own: one for the loader workspace CNode (a
spawn needs eleven capabilities alive at once, which no root CNode has room
for directly) and one for the serial `KIoPort` it mints to print a boot
diagnostic.  Both were constants chosen by reading the boot code — "slot 3 is
free in the reserved boot range", "slot 40 is free" — and a constant that is
true only until someone moves something is the same defect BootInfo exists to
remove.  Three of Fase S4's bring-up failures were slot collisions.

They now come from `empty_slot_first` / `empty_slot_end`, taken from opposite
ends of the declared free range so they cannot collide with each other, with a
refusal if the kernel left fewer than two free slots.  One constant survives:
the panic path that fires when the BootInfo itself is unreadable has nothing to
consult, and a diagnostic that guesses wrong prints nothing rather than
corrupting a slot.

## Etapa 2 — the monolith splits

`KBootstrapCap`'s permission bits become separate capabilities, each published
into its own BootInfo slot, each delegated by handing it over:

| Bit | Authorises | Successor | State |
|---|---|---|---|
| `HW_ACCESS` | `SYS_CAP_CREATE_IRQCAP` | IRQ control capability | ✅ 2a |
| `HW_ACCESS` | `SYS_CAP_CREATE_IOPORT` | ioport control capability | ✅ 2a |
| `FRAMEBUFFER` | `SYS_FRAMEBUFFER_VMO` | device-memory cap for the framebuffer | 2c |
| `SPAWN_SERVICE` | `SYS_PROCESS_CREATE`, `SYS_INITRD_COUNT/VMO` | process-control cap, initrd cap | 2c |
| `KDEBUG` | `SYS_KLOG_DRAIN`, `SYS_SCHED_INFO`, `SYS_POWEROFF` | debug control capability | ✅ 2b |

`SYS_BOOTCAP_RESTRICT` retires with the last bit: narrowing a bitmask by
cloning an object is replaced by delegating the one capability meant and
revoking it through the CDT.  The ioport whitelist (`kioport_whitelist`, ledger
P3) is the policy that should follow the ioport control capability out of the
kernel.

### Etapa 2a — device control is its own authority  ✅ DONE

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
share the remaining monolith.  It dies with the mechanism in Etapa 2b.

### Etapa 2b — debug is its own authority  ✅ DONE

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
creation itself.  T291's oracle moved again — from device creation (Etapa 2a)
to the framebuffer bit — because each re-anchoring is forced by the same
progress: the authority it probed with stopped being a bit and became a
capability.

## Etapa 3 — the root task's own objects (planned)

Root CNode self-capability, initial TCB capability, and ASID/PCID control, each
named in BootInfo.  Today the root CNode is reachable structurally
(`proc->cspace_root`, Stage 4 Etapa 4) and by the `arg0 == 0` convention, and
the initial TCB is reachable only through `SYS_TCB_SELF`.

## Etapa 4 — a retyped TCB executes (planned)

`TCB_CONFIGURE` / `TCB_WRITE_REGS` over a `RETYPE2(KOBJ_TCB)` object, taking
CSpace root, VSpace and fault endpoint as capability arguments — the ledger's
"executable thread-create via pool + handle" entry, whose replacement was
parked here because those arguments only became caps in Stages 3–4.
