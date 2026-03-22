#ifndef IRIS_SVCMGR_BOOTSTRAP_H
#define IRIS_SVCMGR_BOOTSTRAP_H

/*
 * Kernel-side service manager bootstrap.
 *
 * Spawns the ring-3 service manager process, gives it its bootstrap
 * KChannel, and retains the kernel-side channel reference for future
 * service spawn requests.
 *
 * svcmgr self-registers as "svcmgr" in the nameserver — the kernel
 * does NOT call ns_register for svcmgr.  This is the intended pattern:
 * services register themselves; the kernel is not the registrar.
 *
 * NS Authority model (transitional):
 *   svcmgr_bootstrap_init() sets KProcess.ns_authority = 1 on the
 *   spawned process via kprocess_set_ns_authority().  sys_ns_register
 *   checks kprocess_has_ns_authority(t->process) to restrict the
 *   syscall to svcmgr only.
 *
 *   Authority is a property of the KProcess object itself, not of an
 *   external global pointer.  This means:
 *     - Authority does not become stale if svcmgr is restarted.
 *     - The syscall layer has no coupling to a named svcmgr identity.
 *     - Any process granted the flag gains authority (by design, only
 *       svcmgr receives it at bootstrap time).
 *
 *   In the final architecture, service registration policy moves
 *   entirely outside the kernel.
 *
 * Note: svcmgr_get_process() has been intentionally removed.
 *   It is no longer needed; callers that previously used it to check
 *   NS authority should use kprocess_has_ns_authority() instead.
 */

void svcmgr_bootstrap_init(void);

#endif
