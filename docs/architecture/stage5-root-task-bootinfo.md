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
| 2 | The monolith splits: fine-grained boot authority replaces the permission bitmask; `SYS_BOOTCAP_RESTRICT` and `KBootstrapCap` retire | pending |
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

## Etapa 2 — the monolith splits (planned)

`KBootstrapCap`'s four permission bits become separate capabilities, each
published into its own BootInfo slot, each delegated by handing it over:

| Bit today | Authorises | Fine-grained successor |
|---|---|---|
| `HW_ACCESS` | `SYS_CAP_CREATE_IRQCAP`, `SYS_CAP_CREATE_IOPORT` | IRQ control cap, IOPort control cap |
| `FRAMEBUFFER` | `SYS_FRAMEBUFFER_VMO` | device-memory cap for the framebuffer region |
| `SPAWN_SERVICE` | `SYS_PROCESS_CREATE`, `SYS_INITRD_COUNT/VMO` | process-control cap, initrd cap |
| `KDEBUG` | `SYS_KLOG_DRAIN`, `SYS_SCHED_INFO`, `SYS_POWEROFF` | debug cap |

`SYS_BOOTCAP_RESTRICT` retires with the object: narrowing a bitmask by cloning
an object is replaced by delegating the one capability meant, and revoking it
through the CDT.  The ioport whitelist (`kioport_whitelist`, ledger P3) is the
policy that should follow the IOPort control cap out of the kernel.

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
