# IRIS testing strategy

This document defines the minimum testing baseline that IRIS must keep green on every change.

## Current test layers

Four gates, and a green tree means all four — on **one processor and on four**.

| Layer | Command | Green means |
|---|---|---|
| Host unit tests | `make test-unit` | 27429 assertions across 29 suites, 0 failed |
| Purity gate | `make check-purity` | allowlist respected; the kernel-memory-reachable closure is 26 functions and only ever shrinks |
| Lock-order gate | `make check-locks` | 18 ranked locks, no inversions — it holds SMP roadmap §9.1's hierarchy and follows calls three hops |
| Runtime suite | `make ENABLE_RUNTIME_SELFTESTS=1 smoke-full-selftests` | `SUITE PASS 323/323` plus the P3/P41 markers |
| Persistence | `make smoke-persist` | two boots over one image, then the host reads what IRIS wrote |

### The IOMMU dimension

`IRIS_QEMU_IOMMU=1` attaches an Intel VT-d unit to the machine (Stage 10-dma).
Off by default for the same reason `-smp` defaults to 1: the interesting run is
the one that differs from the ordinary one, and a gate that can only be run one
way proves nothing about the other.

```bash
IRIS_QEMU_IOMMU=1 make ENABLE_RUNTIME_SELFTESTS=1 smoke-full-selftests
```

Both directions are gated: with a unit attached the kernel must FIND it and
must CONTAIN with it (`DMA is contained`), and without one it must still say
so — a kernel that silently found nothing and a kernel that silently skipped
looking read the same from outside.

### The platform, as of Stage 10

Three services the gate now requires, and each is checked by what it DID rather
than by having started:

| line | what it means |
|---|---|
| `[IRIS][ABI] version 1.0 - 4 syscall numbers, 77 invocation labels` | the kernel says which ABI it implements; the root task halts the boot on a major it was not built against |
| `[USERBOOT] ACPI: root pointer reachable from ring 3` | the firmware's tables are named by a capability ring 3 holds |
| `[IRIS][P3] exception table: a kernel fault was survived` | the kernel deliberately faulted at CPL 0 on the one instruction that can take a fault it did not choose -- the store into user memory, whose mapping another CPU can retire between the range check and the write -- and came back from it.  `idt.c` halts on every other ring-0 exception, so before the exception table existed this line could not have been printed: the machine would have stopped inside the store.  Requires `IRIS_QEMU_EXPECT_SELFTESTS` |
| `[USER][INIT] pci: functions N windows M carve 0` | the bus service scanned, and carved a frame over **every** window in the region it owns.  `carve 0` is required: a service that found devices and carved nothing refuses every driver's claim, which from outside is indistinguishable from an empty machine |
| `[USER][INIT] blk: disk 1 sid 0x.. dma contained\|open window N` | a ring-3 AHCI driver claimed a controller, brought a port up and **read a sector**.  `contained` is required with an IOMMU and `open` without one — either word on the wrong machine is a lie the gate catches.  `window N` is how many sectors anything may address on the data disk, which is the IRIS partition and nothing else; it is required NON-ZERO here, and a zero is correct behaviour on a stranger's drive.  `home N` is WHICH disk carries an IRIS partition, asked by identity rather than assumed by index -- under QEMU the answer always matches the old hardcoded constant, so it is printed to keep the lookup observable: a mechanism whose right answer is indistinguishable from its fallback is one nobody can tell has stopped working |
| `[USER][INIT] net: link 1 mac .. dma contained\|open` | a ring-3 e1000 driver brought a network card up |
| `[USER][INIT] fs: mounted gen N ... file 1` | a filesystem on a disk IRIS owns; `gen` is how many boots have mounted it, read from the medium and written back, and `file 1` is a file written and read back through the filesystem, the block driver and the controller |
| `[USER][INIT] net: gateway answered, mac ..` | and a frame went out and one came back.  This is the line that means something: a transmit-only check proves nothing, because the card reports a descriptor done whether or not anything was listening.  An ARP round trip exercises the transmit path, the receive ring, the card's filter and a peer that is not this driver |
| `[USER][INIT] ip: udp round trip ok, tftp data N bytes` | a stack above that driver completed a **TFTP read** against a server that is not this machine.  A peer only answers if it accepted an ARP reply, an IPv4 header whose checksum it recomputed, and a UDP header whose checksum covers a pseudo-header — one of the three wrong and the datagram is dropped in silence.  The byte count is checked because receiving A frame is not receiving THE answer: a TFTP server replies from an ephemeral port of its own, so the reply has to be matched to the port the request went out FROM |

The network card is attached with `-netdev user`, QEMU's own userspace stack,
which answers ARP for the gateway it advertises at 10.0.2.2 and carries a TFTP
server there.  That address and the sender's 10.0.2.15 are part of the test
setup: a request from outside that subnet gets no reply, which would look
exactly like a driver that does not work.

The TFTP server is used because it needs nothing from outside the machine — no
host network, no listener to start, no port to pick — and the file it serves is
written by the runner into `build/tftp/` before QEMU starts, so what comes back
is bytes this repository put there.  A wait of two seconds is not generosity:
slirp runs in QEMU's main loop, so a reply is not scheduled against guest time
at all, and a bound in poll counts was wrong in both directions before it
became a bound in real time.

### The screen, for a machine with no serial port

`make smoke-screen` is the only check here that does not read the serial port,
because what it checks is that the serial port is not NEEDED.

Every other gate in this repository reads `-serial file:`.  That is reasonable
under QEMU and useless on the hardware this system is eventually meant to run
on, where most machines have no serial port at all — and on such a machine
every diagnostic the kernel emits before ring 3 exists was going to a port that
is not there.  A boot that died anywhere in that stretch left a black screen
and no record of how far it got.

`fbcon` paints the kernel log onto the framebuffer instead, and the check is a
real one rather than a screenshot somebody squints at: the console draws an 8x8
bitmap font, so the screen can be READ BACK exactly.  `scripts/fbcon_ocr.py`
parses the font out of `kernel/drivers/fbcon/fbcon.c` and decodes each cell,
which means the check cannot pass against a screen that says something else —
and cannot drift from the kernel, because there is no second copy of the font
to drift from.

| what it requires | why |
|---|---|
| `KFSBPGg` on the top line, whole | the boot markers, on a line that never scrolls.  A PREFIX of them is a boot that stopped, and saying where is the entire point |
| `IRIS KERNEL` | the kernel identified itself |
| `free RAM: N MB` | a NUMBER reached the screen.  Numbers take a different path into the log than strings do, and that path was missed the first time — `free RAM:  MB` is a line that passes a banner check and tells you nothing |
| `virtual memory active` | the boot got as far as paging |

The check reads TWO frames, because two different things have to be proved and
they are never true at the same moment.

The **first** is the last frame that still shows the kernel's banner: proof
that the kernel log reaches a screen at all.  The **last** is what a person
standing at the machine ends up looking at, and by then the kernel's lines have
scrolled away and the ring-3 boot report has arrived:

| what the last frame must carry | why |
|---|---|
| `==== IRIS on this machine ====` and the five lines under it | the kernel log only proves the machine STARTED.  Whether the disk driver found a disk, whether the filesystem mounted, whether a frame left the network card — those answers come from ring 3, and on a machine with no serial port they went nowhere at all |

The ring-3 half works because the `console` service paints too.  It is the
service every `[USER]` line already passes through, and its only output used to
be the UART at 0x3F8.  It now draws the same glyphs the kernel does, from the
same `<iris/font8x8.h>`, because a second copy of that table is the one way the
two halves of one screen could start disagreeing.  Asking where the framebuffer
is IS the handover: the kernel stops painting the moment somebody with
`FB_CONTROL` asks.

The top line never scrolls and neither writer clears it, so the boot markers
survive into ring 3.  What is on that line when a machine stops is the
diagnosis.

And init prints its findings once more, as a block, as the last thing before
the idle loop.  Each service already logged them — but a log that scrolls has
lost what matters by the time anyone reads it, and what a person sees on a
settled machine would otherwise be whatever the supervisor said last.

### Persistence takes two boots

`make smoke-persist` is the only check here that cannot be done in one run,
because "persistent" is a statement about what happens BETWEEN runs.  It throws
the image away, boots (the filesystem formats and reports generation 1), boots
again (it must find the filesystem and report 2), and then reads the image from
the HOST.  That last step is the one that matters: the first two are IRIS
reading back its own writes, which a filesystem that merely remembered things
in RAM would also pass.

The image is a real GPT disk with TWO partitions, and that is what makes the
check mean anything.  A raw image cannot test "stay inside your partition",
because there is nowhere else to go — so the image carries a DECOY partition
full of recognisable bytes alongside the IRIS one, and the host compares the
result against a pristine copy built the same way.  What it requires is not
only that the filesystem is where it should be, but that **every byte outside
the IRIS partition is unchanged**: the GPT, its backup, and the decoy.

A third boot follows, and it protects DATA rather than proving a feature.  This
system used to address disks absolutely: `blk` handed a client's LBA straight
to a `WRITE DMA EXT`, and `fs` wrote its superblock to LBA 0 — the start of a
raw image, and the PARTITION TABLE of a real drive.  The reasoning written
beside that code was that disk 1 is IRIS's own, because the block service
numbers the boot disk 0: true of the script that makes the image, false of a
machine, where disk 1 is whatever SATA device enumerates second.

Now every LBA in the block protocol is relative to a window, the window is the
GPT partition typed `IRISFS-PARTITION`, and there is no way to express an
address outside it.  Writes and reads differ on purpose: a WRITE only ever
happens inside an IRIS partition, and a READ on a disk with no such partition
is allowed, because finding a partition means reading the GPT and proving a
port works means reading a sector off it.

So the third boot hands IRIS a disk with a boot signature, recognisable
payloads and no IRIS partition, and requires three things: `window 0` from the
block service, `fs: foreign disk, refusing to format` from the filesystem —
both layers, because they refuse different things and either alone leaves one
accident in the system — and an image that comes back byte-for-byte unchanged,
compared from the host, because "I did not write anything" is exactly the claim
a broken implementation would also make about itself.

### Numbers

**T356** measures an invocation, an IPC round trip to a real server, and a disk
read, and prints all three whether or not they pass — a ceiling says nothing
got catastrophically worse, and the log says what it actually costs.  The
ceilings are order-of-magnitude guards: this runs under TCG on a machine nobody
controls, so these are not hardware figures and are not presented as any.

And three suite tests carry the end-to-end claims: **T353** a device refused
and then granted, **T354** the ACPI root pointer read and checksummed out of
firmware memory, **T355** sector zero off a real disk with the FAT boot
signature intact.

A DMA-capable device (`-device edu`) is attached on **every** run, with or
without a unit, and T353 is a ring-3 driver for it.  That is what turns the
containment claim from a report into evidence: every other check says what the
KERNEL did, and all of it is consistent with hardware that ignored it.  Each
configuration has its own required marker — `T353 refused sid` / `granted:` /
`revoked:` with a unit, `T353 no unit: the device reached` without one — and
the line naming the device is required on every selftest run, because a
`-device edu` dropped from the command line would otherwise read as a green
run with T353 passing trivially.

**T351** and **T352** are the ring-3 half, and both run on either machine.  On
one with no unit they assert the opposite claim: nothing translating, no
containment reported, and an IOSpace REFUSED rather than handed out.  A
containment gauge that reported success with no hardware to enforce it would be
worse than no gauge.

### The core-count dimension

`IRIS_QEMU_SMP=N` runs the same image on N processors, and the runtime suite
must pass on both 1 and 4:

```
make ENABLE_RUNTIME_SELFTESTS=1 smoke-full-selftests
IRIS_QEMU_SMP=4 make ENABLE_RUNTIME_SELFTESTS=1 smoke-full-selftests
```

On more than one processor the script gates three claims that fail
independently: the MADT reports N, N actually arrived, and T346's
`online=N dispatching=N` — a processor that arrived and then halted is online
too, and would pass the first two.  The QEMU timeout is multiplied by the core
count, because TCG emulates four vCPUs at roughly a third of the speed while
doing strictly more work, and a timeout reads exactly like a hang.

**Tests are waits, and a wait is not a yield count.**  On one processor every
yield is a dispatch, so counting yields and counting other threads' turns are
the same count.  On four, a hundred yields are a hundred fast syscalls on THIS
core that can all complete before the core being waited for has taken a single
timer interrupt.

Everything that waits is bounded in elapsed TIME or on the actual condition:

| Primitive | Waits for |
|---|---|
| `it_settle(n)` | `n` scheduler ticks of real time, with the yields kept underneath |
| `it_quiesce_reaper()` | `deaths_pending` reaching zero, after one tick — the tick being the part yields cannot replace, since a thread that died on another core is not in the reap ring until that core dispatches |
| `it_fault_wait_ep()` | the fault, with a two-second tail after the fast path; T308's fault is a TIMEOUT and cannot arrive until a server has burned a budget measured in ticks |
| `IT_AWAIT(cond, ticks)` | any condition another thread has to make true.  It replaced twenty-nine `for (i = 0; i < N && !flag; i++) yield;` loops |

A new test that waits by counting its own syscalls is a test that will pass on
one processor and flake on four.  Use `IT_AWAIT`.

The suite count moves when a stage retires the mechanism a test was about, or
adds one.  Stage 7 took it from 276 to 273: T144 and T184 lost their "a process
capability is not a thread" checks because a spawn hands back a thread.

The runtime suite is the gate that matters for capability behaviour: it runs in
ring 3 as a real service and observes the kernel only through syscalls.

The three original layers:

1. Static build validation
   - `make clean`
   - `make`
   - `make check`
   - Confirms the UEFI loader, kernel ELF, embedded service ELFs, linker outputs, and ELF layout still build correctly.

2. Reproducible local smoke validation
   - `make smoke`
   - Runs two clean compile passes:
     - default build
     - `ENABLE_RUNTIME_SELFTESTS=1` build
   - This catches regressions where code only compiles in one configuration.

3. Runtime/manual validation
   - `make run`
   - `make run-headless`
   - `make smoke-runtime`
   - `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`
   - Optional deeper local path:
     - `make clean`
     - `make ENABLE_RUNTIME_SELFTESTS=1`
      - `make run`
   - This is the layer that exercises boot, userland bring-up, IRQ delivery, the bootstrap capability flow, and service health end-to-end.
   - Healthy-path serial signature now includes:
     - `[IRIS][BOOT] handoff: kernel -> svcmgr/init`
     - `[SVCMGR] ready`
     - `[USER][INIT][BOOT] healthy path OK`

## CI runtime path

GitHub Actions now uses two runtime boot lanes:

- default lane: `make smoke-runtime`
- selftest lane: `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`

Both lanes:

- boot the built image in headless QEMU
- capture the serial log to a build artifact
- accept QEMU timeout exit `124` because IRIS intentionally keeps running
- fail if the healthy boot signature is missing

The selftest lane additionally asserts:

- `[IRIS][P3] handle/lifecycle selftests OK`
- `[USER][INIT][DIAG] reply`
- `[SVCMGR][DIAG] kbd status OK`

The interactive `make run` path remains local and developer-oriented.

## Minimum expectations for contributors

For changes that touch build files, boot, kernel, services, capability rights, IRQ routing, initrd contents, or protocol headers:

- run `make smoke`
- run `make run` locally if the change can affect boot/runtime behavior
- prefer `make smoke-runtime` when you need a reproducible headless runtime check

For changes that specifically touch lifecycle, diagnostics, IPC, capability transfer, or service bootstrap:

- prefer the selftest-enabled path:
  - `make clean`
  - `make ENABLE_RUNTIME_SELFTESTS=1`
  - `make run`

## Build-configuration toggling guarantee

Toggling `ENABLE_RUNTIME_SELFTESTS` between invocations is safe and must
keep working on the FIRST `make` after the flip. Stale-artifact cleanup runs
at Makefile **parse time** (see the `BUILD_CONFIG_ON_DISK` block at the top
of the Makefile): a recipe-time cleanup used to delete objects make had
already stat-cached, making the first link after a flip fail on missing
`*_bin.o` until a second invocation. That race is fixed; verified in
Phase V1 with four consecutive first-invocation toggles
(0→1, 1→0, 0→1, 1→0 — all RC=0, zero errors). If a first-invocation toggle
ever fails again, treat it as a regression of this guarantee.

## Current gaps worth closing next

The baseline above is the minimum that should stay green. The next additions
should remain small and directly auditable:

1. Add host-side coverage for protocol packing helpers and authority-reduction helpers that do not require QEMU to validate.
2. Decide whether service-side `IRIS_ENABLE_RUNTIME_SELFTESTS` code should be compiled into the selftest lane as well, and document that policy explicitly.
3. If more boot phases become critical, extend the headless assertion set with one marker per phase boundary rather than relying on free-form log inspection.


## What the convergence stages added to the suite

Each stage's invariants are pinned by named runtime tests, so a regression
names itself rather than showing up as a boot hang:

| Test | Pins |
|---|---|
| T351, T352 | Stage 10-dma.  T351: the remapping units found, usable and ENFORCING — `translating == units`, or, on a machine with none, nothing translating AND no containment claimed.  T352: the whole capability arc — retyping an IOSpace (anyone with an Untyped may), binding it to a device (only with IOSpaceControl), installing the three translation levels one at a time out of the holder's own memory, mapping a frame, refusing a second mapping at one address, unmapping, and then destroying the space with a mapping still live so the baseline proves every object came back |
| T353 | Stage 10-dma §10.2 step 6, and the thing T351/T352 cannot do: a DEVICE is watched being refused.  A ring-3 driver finds QEMU's `edu` DMA engine through the `pci` service, takes its BAR as a frame capability, maps it uncached and programs a transfer.  With a unit and no IOSpace mapping the target frame is untouched and the unit's fault record names the device's source-id; with the frame mapped the data arrives; revoked, it is refused again.  On a machine with no unit the same driver reaches memory nobody granted it, which is the other half of the claim and why the device is attached to those runs too |
| T354 | Stage 10.  Ring 3 reads the firmware's own description: the ACPI region is device memory, one frame covers it, and the root pointer inside it is found by searching for the signature — the way every firmware reader does — and validated by its checksum |
| T355 | Stage 10.  Storage, end to end: the ring-3 AHCI driver's read of sector zero carries the FAT boot signature, the controller's DMA is contained exactly when the machine has a unit to contain it with, the buffer arrives READ-ONLY, and a capability from the previous read no longer works — the service revokes before it reuses the frame |
| T347–T350 | SMP roadmap §9.3 step 5, the adversarial phase — four tests that AIM four processors at ONE object rather than merely running on several.  T347: four callers on four cores calling one server, each requiring its own answer, which is how a reply delivered to the wrong caller becomes visible at all.  T348: four cores minting and deleting from one capability while a fifth revokes it.  T349: four cores retyping into the SAME slot, where exactly one may win and the losers must lose cleanly — and the sub-untyped's budget must come all the way back after a RESET, which is the assertion about the ROLLBACK.  T350: four cores killing the same four threads, so one kill always races the thread's own core.  Between them they found four defects — a retype rollback that freed another core's memory, a reference released on the line above the call that used it, a teardown gate that was a plain byte tested unlocked, and a dispatch that overwrote a `Suspend` on a thread already dequeued (that one surfaced in T333, which suspends a thread and then reads its registers).  Each reports how many distinct cores its workers landed on, so a run that was taking turns rather than contending says so |
| T346 | SMP roadmap §9.3 step 4: the other processors SCHEDULE.  On one processor, exactly one has ever dispatched and no tick was broadcast — that zero is not a formality, since the timer ISR calls the broadcast on every tick and a version that did not check would be firing IPIs into an empty destination mask a hundred times a second.  On more than one: every processor that is ONLINE has dispatched a thread (not "at least two" — a machine that brought four up and schedules on three has a quarter of its cores idle for ever and looks healthy from everywhere else), and the tick broadcast is still ADVANCING across real elapsed time, because a processor that stops being told the time never charges its thread's budget and never runs its slice down |
| T345 | SMP roadmap §9.3 step 2, and it asks the machine how many processors it has rather than assuming: always, an unmap still issues its LOCAL `invlpg`; on one processor, zero shootdowns, which is the evidence the target scan skips the CALLING CPU — without that skip the first unmap would IPI itself and spin, with interrupts off, for an acknowledgement it cannot deliver; on several, shootdowns have HAPPENED, and reaching the assertion at all is the ack handshake working, since a core that did not answer would have hung the machine rather than failed a comparison |
| T095, T096 | Stage 4's structural zeros: no handle is live, delivered, or produced by a TOCTOU fallback |
| T292–T295 | CSpace-native introspection; a CPtr addresses exactly one capability |
| T296 | Stage 5: one capability, one authority — each boot control capability authorises its own syscall and nothing else |
| T297 | Stage 5: a retyped TCB executes; an unconfigured one cannot be started, written or exited; foreign CSpace/VSpace refused |
| T298 | Stage 6: an Untyped pays for its frames' headers, and frames stay page-dense |
| T299 | Stage 6: page tables are charged to a named budget, which cannot be RESET while they live |
| T300 | Stage 6: user memory comes out of a named budget, and the region is reclaimable once the VMO is gone |
| T301 | Stage 6: a REFUSED spawn leaves its budget untouched — no stranded children, still RESET-able, swept across the boundary in sub-page steps |
| T302 | Stage 6-pure: a page table is a capability — retyped by the holder, installed one level per invocation, refused at a kernel address, and the walk it builds really maps |
| T303 | Stage 7: a running thread outlives every capability to it — the execution reference a retyped TCB never took |
| T304 | Stage 7: the live-process ceiling is gone — more than 64 children out of one budget, a clean error when that budget ends, and a RESET afterwards that proves nothing leaked |
| T187, T188, T196, T210 (re-derived) | Stage 7-proc: an address space outlives its threads while a capability to it lives, so a late map into a dead target's space SUCCEEDS — seL4's shape, where a page directory outlives its threads.  What the tests were always about (the books return to baseline once the capability is dropped) still holds |
| T239–T250 (re-derived) | Stage 7-mem: budget accounting and reclamation drift, read off `SYS_UNTYPED_QUERY` after `SYS_RESOURCE_INFO` retired with the per-process resource domain |
| T312 | Ledger D-2 closed: the ROOT CSpace capability carries a guard, installed by `SYS_TCB_CONFIGURE`'s arg3 (seL4's `cspace_root_data`).  Asserts the property that would be lost by putting the guard on the KCNode: a parent and a child share ONE root CNode object and address it DIFFERENTLY, because a guard belongs to a capability and not to what it names.  Also that an oversized guard is refused rather than truncated — a root guard meaning something other than what was asked for would change what every CPtr in that thread's CSpace means |
| T311 | Ledger D-8: `SYS_CSPACE_REVOKE` is PREEMPTIBLE.  Builds a derivation subtree wider than one slice and asserts both halves of the claim: the restart counter advances (it really gave the CPU up part-way) and the reported count is the whole job rather than the last slice — the accounting mistake a sliced operation invites.  Then checks every descendant is actually gone, because a preemption point that loses work is worse than none |
| T310 | Stage 9-evt / D-1 step 1: a blocking syscall is RE-EXECUTED, not parked.  From ring 3 a restartable sleep and a stack-parked one are indistinguishable, so the assertion is on the kernel's restart gauge: it must advance across a blocking sleep and must NOT advance for a zero-length one, because a syscall that can complete must never take the slow path |
| T309 | Stage 8-mcs: a passive server serves a LOOP through `SYS_REPLY_RECV` — the donation is re-established on every call, the reply object is re-staged without reallocation, and each answer reaches the right caller.  A server that leaked its donation would stop after one request; one that failed to re-stage would fail the second call.  It also found the footgun the syscall now removes: the buffer arrives holding the kernel's echo of the staged reply CPtr, so a naive reply asks the kernel to transfer away the reply object itself |
| T308 | Stage 8-mcs: a PASSIVE server runs on its client's donated scheduling context.  Discriminating by construction: before donation a thread with no SC was never charged at all, so it ran with unlimited time; the test arms a timeout handler on the SERVER and has it spin, and the fault can only fire if the server was charged against a scheduling context it does not own.  It caught the real bug — donation was wired into two of the three rendezvous paths, so a server ran unbudgeted or not depending on which side arrived first |
| T307 | Stage 8-mcs: budget exhaustion is a FAULT a supervisor can answer.  Starts a thread that only spins, gives it one tick of budget in a long period, arms a timeout handler, and asserts the handler is signalled, the record carries `IRIS_FAULT_VECTOR_TIMEOUT` (so an overrun is distinguishable from a page fault), the thread is really BLOCKED rather than still burning budget, and the supervisor can end it.  Every other test in the suite is the unarmed case, which is what says the change was additive |
| T306 | Stage 8-cap / D-2: a CNode capability carries a GUARD — the default resolves as before (additivity), the guarded address resolves, the plain one stops resolving, a wrong guard fails NOT_FOUND rather than landing elsewhere, width 0 restores the original address, and a guard on a non-CNode is refused.  Ring-3 half of the property; `test_cnode_guard` G-1..G-8 is the host half |
| T305 | Charter A9: every capability is traceable to an ancestor — reads `mdb_legacy_roots`, the gauge the ABI calls "must → 0", and pins it against growth across a spawn/kill and a mint/revoke cycle.  Reports the inventory (43 roots of 335 MDB nodes, max depth 6, at Stage 7 close) so the number is visible rather than assumed; the absolute count moves with what is alive, so the assertion is on the DELTA across the cycle |
| T140–T147, T181–T238 (re-derived) | Stage 7: a fault is answered by naming the faulting THREAD's capability, delivered into a mailbox the registrant declared.  The suite's own targets deliver to the suite; a target handed to a pager is re-aimed to a CNode shared with it; a victim is never re-aimed, which is what makes a cross-target attempt fail for want of a capability rather than by a rejected id.  The pager manifests lost bit 20: it holds no process capability for any target it serves |
| PT-1..PT-11 (host) | Stage 6-pure: the paging walk driven exhaustively — level order, spent-vs-complete, kernel-address refusal, dead VSpace, teardown returning every level, the bootstrap exception being one-way, a reused level entering the walk empty, teardown detaching exactly the holder's levels, and a failed composition giving its bind claim back |

**The syscall layer is under host test as of Stage 8-cap.**  Until then 76% of
the kernel's C had no unit tests and the largest untested piece was the layer
whose entire job is validating arguments and checking authority — covered only
through the syscall boundary, where a rejection and a crash look alike from
ring 3 and where fault injection is unavailable.  That was the wrong way round:
every assertion the object suites make assumes something upstream already
rejected the malformed CPtr, the wrong type, the missing right.
`test_syscall_cspace` (SC-1..SC-10) and `test_syscall_retype` (RT-1..RT-8)
assert the refusals one at a time, each returning the specific error it
promises rather than the nearest plausible one.  Host coverage of kernel `.c`
moved **24% → 72%**, and the syscall layer is **15 of 15 translation units** —
all of it.

What remains outside the host build is hardware and boot: `gdt`, `idt`,
`lapic`, `pic`, `paging`, `pmm`, `kslab`, `kpage`, `kstack`, `serial`, `panic`,
`kernel_main`, the two scheduler files, and `initrd` (whose contents are blobs
objcopy'd into the kernel image).  That is a defensible boundary rather than a
backlog: each of them is about a machine, and a unit test does not have one.

What the host suite deliberately does NOT cover is the real `usercopy`: SMAP
STAC/CLAC, the per-page PRESENT|USER|WRITABLE walk, the overflow check and the
`USER_SPACE_TOP` bound are about an address space that does not exist in a unit
test.  There "user memory" is host memory and the copy helpers are a checked
memcpy, which is what makes the handlers exercisable at all — every one of them
reads its message before doing anything else.  Pretending to test the real
walk against a stub that cannot fail the way it can would be worse than not
covering it; the runtime suite exercises it in a real address space.

**The `struct task` stub is DELETED.**  The host suite compiled against a
hand-written copy of the TCB that omitted whatever the object suites did not
need, so a test could compile different code than the kernel and nobody would
notice — the stub had already drifted three times in one week of this work.
The real `<iris/task.h>` compiles on the host unchanged, so the suite uses it,
and deleting the copy is what unblocked five syscall translation units at
once.

`test_syscall_retype` covers the narrowest place in the system: since Phase S1
`SYS_UNTYPED_RETYPE2` is the only way a kernel object comes into existence, so
everything the kernel will ever hold passes through one switch.  A slot count
accepted there that is not a power of two is a CSpace whose radix walk indexes
past the end of its own array; an unbounded batch count is a ring-3 caller
choosing how long interrupts stay off.  Every case is checked before the source
Untyped is resolved, which is itself the property being pinned — a retype that
fails must fail without having consumed anything, or a caller learns it was
refused by noticing its budget shrank.

Host unit tests cover what a successful boot cannot show: `RBI-1..RBI-10` (the
BootInfo builder's bounds), `UT-TOP-1..5` (the two-ended Untyped carve), and
`BC-11..BC-13` (a CSpace that names itself), `R-1..R-8` (sporadic
replenishment — the conservation law `remaining + consumed + pending ==
budget` re-checked after every operation, the partial-run defect that made a
blocking server starve itself, and the guarantee that no thread gets more than
its budget in a window of its period), and `G-1..G-8` (CNode guards,
including the one property that makes a guard seL4's guard: it is
capability-local, so two capabilities to the same CNode resolve at different
addresses).  `BC-13` changed meaning in
Stage 7-proc and is worth reading for it: a slot naming its own CNode now takes
no ACTIVE reference — an object reachable only from itself is reachable by
nobody — so the case that used to be the negative control ("without the
explicit teardown the object is NOT freed") is now the positive one.
