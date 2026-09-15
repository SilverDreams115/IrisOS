#ifndef IRIS_SMP_H
#define IRIS_SMP_H

/*
 * smp.h — bringing application processors up (SMP roadmap §9.3 step 3).
 *
 * What this step delivers and what it deliberately does not: the APs are
 * started, taken to 64-bit long mode, given their own GDT, TSS, per-CPU block
 * and GS base — and then they PARK.  They schedule nothing, take no
 * interrupts, run no threads.
 *
 * That is not an unfinished implementation, it is the checkpoint.  "N
 * processors are up" is a claim that can fail entirely on its own, and keeping
 * it separate from "N processors are running threads" means a failure in
 * either is legible.  Letting them schedule is step 4.
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

#endif /* IRIS_SMP_H */
