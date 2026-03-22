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
 *   svcmgr_get_process() returns the KProcess* of the service manager.
 *   sys_ns_register uses this to restrict SYS_NS_REGISTER to svcmgr
 *   only.  This is a kernel-enforced transitional policy — the kernel
 *   knows about svcmgr specifically.  In the final architecture, the
 *   service manager enforces registration policy outside the kernel.
 *
 *   Returns NULL before svcmgr_bootstrap_init() completes.
 */

struct KProcess;

void             svcmgr_bootstrap_init(void);
struct KProcess *svcmgr_get_process   (void);

#endif
