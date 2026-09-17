# IRIS testing strategy

This document defines the minimum testing baseline that IRIS must keep green on every change.

## Current test layers

Four gates, and a green tree means all four — on **one processor and on four**.

| Layer | Command | Green means |
|---|---|---|
| Host unit tests | `make test-unit` | 27414 assertions across 28 suites, 0 failed |
| Purity gate | `make check-purity` | allowlist respected; the kernel-memory-reachable closure is 26 functions and only ever shrinks |
| Lock-order gate | `make check-locks` | 18 ranked locks, no inversions — it holds SMP roadmap §9.1's hierarchy and follows calls three hops |
| Runtime suite | `make ENABLE_RUNTIME_SELFTESTS=1 smoke-full-selftests` | `SUITE PASS 319/319` plus the P3/P41 markers |

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
| T351, T352 | Stage 10-dma.  T351: the remapping units found, usable and ENFORCING — `translating == units`, or, on a machine with none, nothing translating AND no containment claimed.  T352: the whole capability arc — retyping an IOSpace (anyone with an Untyped may), binding it to a device (only with IOSpaceControl), installing the three translation levels one at a time out of the holder's own memory, mapping a frame, refusing a second mapping at one address, unmapping, and then destroying the space with a mapping still live so the baseline proves every object came back.  Neither watches a device be refused — there is no DMA engine under IRIS's control here, which is §10.2 step 6 |
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
