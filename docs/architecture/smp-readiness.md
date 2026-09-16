# IRIS — the scheduler on more than one processor

This file used to be called SMP *readiness*, and it described a migration
(Phase S2) that was a prerequisite: getting the scheduler's identity out of a
static `tasks[TASK_MAX]` array and into pointers, so that a TCB could be an
object carved from Untyped rather than a slot.  That migration is finished —
the array is gone, the run queue is pointer-based, the registry that replaced
the array is itself gone in favour of an intrusive list — and readiness stopped
being the question when the processors started scheduling.

**The detailed account is SMP roadmap §9.1–§9.4**
(`sel4-convergence-roadmap.md`): the lock hierarchy with every edge at
`file:line`, the catalog of shared mutable state, the five steps and what the
stage deliberately cannot prove.  This file is the short version of where
things stand and where to look.

## Where it stands

Four processors dispatch threads on a four-CPU machine; one processor behaves
exactly as it did before any of it existed.  The full suite is green on both,
and `make check-locks` enforces the hierarchy statically.

| | |
|---|---|
| Discovery | ACPI MADT, out of an RSDP the bootloader forwards from the EFI configuration table — that table stops existing at ExitBootServices |
| Bring-up | a real-mode trampoline in a page CLAIMED from the PMM, INIT-SIPI-SIPI, serialised so two processors never share one stack |
| What an AP adopts | its own GDT/TSS/GS, the IDT (the table is shared, IDTR is not), its LAPIC, its syscall MSRs, its core stack — and the BSP's **CR0 and CR4**, copied rather than re-derived, because an AP leaves INIT at reset values and would otherwise have no SSE, no PCIDE, no SMEP and no SMAP |
| The tick | the PIT interrupts one processor, which does the MACHINE's half (clock, domain schedule, replenishment sweep, idle fast-forward) and then IPIs the others, which each do their own CORE's half (budget, preemption, time slice) |
| Where threads run | round-robin over the processors that are online, chosen once at TCB configure.  Nothing migrates: a kernel that moves threads has to decide when, and "when" is a ring-3 policy |
| Handing a thread over | `task->on_cpu`, raised by the dispatcher that commits to a thread and lowered by the one that has finished releasing it.  A thread becomes wakeable the instant it blocks — several hundred instructions before its core is done with it |
| Stopping a remote thread | `Suspend` stalls until the thread is off its processor; a kill marks it and leaves, because a killer is often inside an endpoint and the dispatch it would wait for takes endpoint locks |
| TLB | shootdown IPIs, targeted at the processors whose `current_task` names the VSpace, spinning for an acknowledgement with no timeout |
| Proof | **T346**: every processor that is online has dispatched a thread, and the tick broadcast is still advancing.  `online=4 dispatching=4` is the claim; a machine that brought four up and schedules on one would read `online=4 dispatching=1` and look healthy everywhere else |

## The adversarial phase

"The suite passes on four processors" was worth less than it sounded: its
threads happened to be spread across cores, so it exercised whatever
interleavings fell out.  Four tests (§9.3 step 5) AIM four processors at one
object instead — one endpoint with four callers, one capability being minted
from while it is revoked, one slot being retyped into, one thread being killed
by four cores at once.

They found four defects, none of them in the code written for SMP: a retype
rollback that un-bumped a carve window covering another core's allocation, a
reference released on the line above the call that used the pointer, a teardown
gate that was a plain byte tested by an unlocked read so four cores entered one
thread's teardown, and a dispatch that overwrote a `Suspend` on a thread that
had already been dequeued — the fourth surfacing not in the new tests but in
T333, which suspends a thread and then reads its registers.

## What is not done

The model-based fuzzer is not extended to N cores.  It RUNS there and passes;
it does not aim several cores at one object the way those four do.

There is no `TCB_SetAffinity`.  Nothing migrates a thread, so a test cannot
choose which processors contend — it reads where the round robin put them
(`iris_tcb_info.home_cpu`) and says so.  Adding affinity means deciding when a
thread may move, which is a scheduling policy and belongs in ring 3.

And §9.4's limit stands: QEMU's TCG interleaves, it does not reorder.  This
method finds logic races; it will not find a wrong `memory_order` on a relaxed
atomic.  Four defects found this way is evidence the method works, not
evidence the kernel is free of the other kind.

Per-core APIC timers are the other open item, and they are a performance
question rather than a correctness one: they would remove three interrupts per
tick, and they cost four independent calibrations of a quantity that MCS
deadlines are counted in.
