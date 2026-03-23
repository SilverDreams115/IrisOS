#ifndef IRIS_SVCMGR_BOOTSTRAP_H
#define IRIS_SVCMGR_BOOTSTRAP_H

#include <iris/nc/error.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>

struct task;

/*
 * Kernel-side service manager bootstrap.
 *
 * Spawns the ring-3 service manager process, gives it its bootstrap
 * KChannel, and retains the kernel-side channel reference for future
 * service spawn requests.
 *
 * Phase 8/current bootstrap model:
 *   - The kernel spawns svcmgr and gives it its private bootstrap
 *     KChannel handle via the task arg0 bootstrap contract.
 *   - The kernel may also install a narrow client handle to that same
 *     channel into the first user task that must talk to svcmgr.
 *   - Normal healthy boot no longer requires SYS_NS_LOOKUP("svcmgr")
 *     to find the service manager.
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
iris_error_t svcmgr_bootstrap_attach_client(struct task *client,
                                            iris_rights_t rights,
                                            handle_id_t *out_handle);

#endif
