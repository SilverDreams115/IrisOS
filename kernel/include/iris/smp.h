/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_SMP_H
#define IRIS_SMP_H

/*
 * smp.h — application processors: bringing them up (§9.3 step 3), and letting
 * them schedule (§9.3 step 4).
 *
 * Step 3 started the APs, took them to 64-bit long mode, and gave each its own
 * GDT, TSS, per-CPU block and GS base — and then they PARKED.  Keeping "N
 * processors are up" separate from "N processors are running threads" was the
 * point: either claim can fail on its own, and a failure in one is legible
 * only if the other is not entangled with it.
 *
 * Step 4 is the second claim.  An AP now loads the IDT, enables its LAPIC,
 * writes its own syscall MSRs and enters the dispatcher, which is the same
 * dispatcher the boot processor runs.  There is no AP idle loop and no AP
 * scheduler: a core with nothing to run halts inside `core_dispatch`, exactly
 * as the BSP does, and threads reach it because `home_cpu` sends them there.
 */

#include <stdint.h>

/* Start every processor the MADT reported except this one.  Returns how many
 * arrived — checked, not assumed: a processor that does not answer is left
 * alone and reported rather than waited on for ever.
 *
 * The memory map is a PARAMETER because the trampoline has to live below 1 MiB
 * and which page is free there is a fact about this machine.  Taking it rather
 * than reaching for the kernel's copy keeps this file's inputs visible. */
struct iris_boot_info;
uint32_t smp_start_aps(const struct iris_boot_info *bi);

/* How many processors are running IRIS code, including the BSP.  1 before
 * `smp_start_aps`, and 1 afterwards on a machine with one processor. */
uint32_t smp_online_count(void);

/* How far the last processor that failed to arrive got through the
 * trampoline, or 0.  A processor that does not start is otherwise completely
 * silent, and the difference between "the far jump was wrong" and "the page
 * tables were wrong" is the difference between an afternoon and a week. */
uint32_t smp_last_progress(void);

/* Where an application processor arrives, in 64-bit long mode on its own
 * stack.  Never returns.  Named here because the trampoline needs its
 * address and the BSP is what writes it in. */
void ap_main(void);

/*
 * The tick IPI (§9.3 step 4).
 *
 * The PIT interrupts one processor.  The other three have a running thread
 * whose budget has to be charged and whose time slice has to run down, and
 * this is how they are told a tick happened.  See the long note above
 * `sched_tick_global` in scheduler.c for why the tick has one owner rather
 * than one timer per core.
 */
#define SCHED_TICK_IPI_VECTOR 0xF2u

/* Does this processor own the machine's clock?
 *
 * The PIT interrupts one core, and that core does the timekeeping half of the
 * tick.  Anything else that MOVES the clock rather than reading it belongs to
 * the same core, for the same reason — the idle fast-forward most of all: three
 * idle processors each advancing the tick counter to the nearest replenishment
 * would hand the threads running on the fourth a clock that jumps. */
int smp_is_timekeeper(void);

/* Did this processor arrive?  The only honest test: the count says how many
 * came, and the cpu ids come from the MADT's order, so a machine where the
 * third entry failed to start has processors 0 and 1 — or 0 and 2.  Set by the
 * arriving processor itself, which is the only code that knows for certain. */
int smp_is_online(uint32_t cpu_id);

/* Send the tick to every processor that is online but us.  No-op on a machine
 * with one processor, which is what makes the timer ISR identical there to
 * what it was before this existed. */
void smp_tick_others(void);

/* Ask every other online processor to re-run its dispatcher.  Used when a
 * decision made here invalidates what another core is running — a domain
 * switch, which is machine-wide by definition. */
void smp_reschedule_others(void);

/* Ask ONE processor to re-run its dispatcher.  A no-op for this processor and
 * for one that never arrived.  Every cross-CPU reschedule goes through here so
 * that the gauge below sees all of them, not just the broadcasts. */
void smp_send_reschedule(uint32_t cpu_id);

/* Evidence, for the diagnostic block and the tests that read it.  How many
 * ticks were broadcast, how many reschedules, and how many processors have
 * ever dispatched a thread — which is what "the APs schedule" means, as
 * distinct from "the APs are up". */
uint32_t smp_tick_ipi_count(void);
uint32_t smp_reschedule_ipi_count(void);
uint32_t smp_dispatching_count(void);

#endif /* IRIS_SMP_H */
