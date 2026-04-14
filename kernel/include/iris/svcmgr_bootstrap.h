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
 * Bootstrap capability model:
 *   svcmgr_bootstrap_init() injects an explicit bootstrap capability handle
 *   into svcmgr over its private bootstrap channel.
 *
 *   That handle authorizes only SYS_SPAWN_SERVICE. IRQ route installation is
 *   authorized by the target child proc_handle carrying RIGHT_ROUTE.
 */

void svcmgr_bootstrap_init(void);
iris_error_t svcmgr_bootstrap_attach_client(struct task *client,
                                            iris_rights_t rights,
                                            handle_id_t *out_handle);

#endif
