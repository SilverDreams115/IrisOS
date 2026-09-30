/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_VERIFIED_CONFIG_H
#define IRIS_VERIFIED_CONFIG_H

/*
 * verified_config.h — what IRIS-V IS, and what it is not.
 *
 * ── Why a configuration and not a kernel ───────────────────────────────────
 *
 * A proof is not made about a project.  It is made about ONE artefact with a
 * declared boundary, and until that boundary exists the sentence "verify IRIS"
 * has no meaning — there is nothing for a proof obligation to be ABOUT.  So
 * the first act of the verification work is not code: it is cutting the
 * system in two and saying which half is the subject.
 *
 *   IRIS-V  — the subject.  Uniprocessor, x86-64, a frozen object set and a
 *             frozen ABI.  This is what a refinement proof would be about.
 *   IRIS-P  — the platform.  SMP, IOMMU, everything the tree has today.  It
 *             shares its source with IRIS-V wherever it can and it is NOT
 *             verified, which the README says in those words.
 *
 * Nothing here is verified TODAY.  This header defines the target, not a
 * claim: `IRIS_VERIFIED_CONFIG` selects a build whose proof obligation is
 * small enough to be stated.  Anyone reading it should take it as the
 * definition of future work.
 *
 * ── What IRIS-V gives up, stated where it costs something ──────────────────
 *
 * **SMP.**  Verified multiprocessor seL4 does not exist — not as an option,
 * not anywhere.  A kernel with 32 ranked locks and threads on four cores is
 * a proof obligation nobody has discharged for any microkernel.  So IRIS-V is
 * `MAX_CPUS 1`, and Stage 9 stays in IRIS-P.  This is the single largest
 * reduction in the obligation and it is also the largest thing surrendered:
 * for as long as this work runs, the BETTER kernel is the unverified one.
 *
 * **The IOMMU.**  Device-DMA containment is authority over a device, not
 * authority in the object model, and 795 lines of table walking is not where
 * a first proof should be spent.  IRIS-V behaves exactly as a machine with no
 * VT-d unit — a path the tree already supports and already gates, so nothing
 * else in the kernel changes shape to accommodate it.
 *
 * ── What is still inside the boundary and should not be ────────────────────
 *
 * Honest, because a boundary drawn generously is not a boundary.  ACPI (314
 * lines) is in, because the LAPIC base comes from the MADT and ripping it out
 * is a boot-path change rather than a scoping decision.  The initrd catalogue
 * (280) and `klog` (125) are in for the same reason.  Each must leave before
 * the refinement proof of §4, and the ledger records that as owed work rather
 * than letting the boundary quietly include them.
 *
 * Size, measured: the core IRIS-V would have to prove is the object model
 * (~6.1k lines), the syscall layer (~7.7k), the scheduler (~2.7k) and the
 * architecture code (~2.8k) — about 19k, against seL4's ~10k verified.  The
 * syscall layer is where the reduction has to come from: 7.7k for 77
 * invocation labels is 100 lines each.
 */

#ifndef IRIS_VERIFIED_CONFIG
#define IRIS_VERIFIED_CONFIG 0
#endif

#if IRIS_VERIFIED_CONFIG
/* One processor.  Not "one by default" — one by construction, so that every
 * `for (i = 0; i < MAX_CPUS; i++)` in the tree collapses and no proof has to
 * reason about a second core that cannot exist. */
#define IRIS_V_MAX_CPUS 1u
#else
#define IRIS_V_MAX_CPUS 8u
#endif

#endif /* IRIS_VERIFIED_CONFIG_H */
