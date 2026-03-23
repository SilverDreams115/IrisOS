#ifndef IRIS_NAMESERVER_H
#define IRIS_NAMESERVER_H

#include <iris/nc/kobject.h>
#include <iris/nc/rights.h>
#include <iris/nc/handle.h>
#include <iris/nc/error.h>
#include <iris/task.h>
#include <stdint.h>

struct KProcess;

/*
 * Bootstrap service registry — transicional.
 *
 * ── Current role ─────────────────────────────────────────────────
 * Flat kernel-resident table that maps string names to KObjects so
 * that user processes can obtain handles to well-known services by
 * name at bootstrap time.
 *
 *   - ns_register: reached through SYS_NS_REGISTER by the bootstrap
 *     service manager.  The kernel no longer chooses public service
 *     names for compiled-in services; svcmgr owns that policy in
 *     userland and uses this table only as the transitional bootstrap
 *     publication backend.
 *   - ns_lookup: compatibility/bootstrap lookup path for experiments
 *     that still need a kernel-resident flat registry.
 *   - ns_unregister_owner: called on KProcess teardown to clean up
 *     entries owned by the dying process.
 *
 * ── What this is NOT ─────────────────────────────────────────────
 *   - Not a service supervisor: no restart, health-check, or policy.
 *   - Not open-access: SYS_NS_REGISTER is restricted at the syscall
 *     layer to the process holding ns_authority (set on svcmgr at
 *     bootstrap).  This is transitional kernel-enforced policy; the
 *     final architecture moves all registration policy out of the
 *     kernel.  See KProcess.ns_authority and kprocess_has_ns_authority().
 *   - Not a capability discovery protocol: lookup returns a handle
 *     but there is no typing or interface versioning.
 *   - Not the final naming architecture: NS_NAME_LEN=16 and the flat
 *     structure are intentionally minimal for the bootstrap phase.
 *
 * ── Evolution path ───────────────────────────────────────────────
 * The long-term target is a privileged user-space service manager
 * that owns service registration and lifecycle policy.  Phase 5 moved
 * service naming policy there.  A later pass moved normal runtime
 * lookup of live services (`kbd`, `vfs`) into svcmgr IPC with attached
 * handle transfer.  The current bootstrap path also no longer uses
 * SYS_NS_LOOKUP to find svcmgr: the first client receives an explicit
 * bootstrap handle from the kernel.  What remains here is therefore
 * compatibility/transitional registry code, not the healthy-path first
 * discovery authority.
 *
 * ── Implementation invariants (stable) ──────────────────────────
 *   - ns_register retains the KObject (kobject_retain).
 *   - ns_lookup inserts a NEW handle into the requesting task's
 *     handle table and returns the handle_id.
 *   - The caller of ns_lookup receives at most the rights registered.
 *   - ns_unregister and ns_unregister_owner release the retained ref.
 *   - Names are null-terminated, max NS_NAME_LEN-1 usable chars.
 *   - At most NS_MAX_ENTRIES simultaneous registrations.
 *   - All operations are protected by an internal spinlock.
 */

#define NS_MAX_ENTRIES 32
#define NS_NAME_LEN    16   /* max service name length including null */

void         ns_init       (void);

/* Register obj under name with rights.  Retains obj.
 * Returns IRIS_ERR_ALREADY_EXISTS if name is taken. */
iris_error_t ns_register   (const char *name, struct KObject *obj,
                             iris_rights_t rights, struct KProcess *owner);

/* Insert a handle to the named service into task t's handle table.
 * Rights are capped to what was registered, intersected with req_rights.
 * Pass RIGHT_SAME_RIGHTS to get all registered rights.
 * Returns IRIS_ERR_NOT_FOUND or IRIS_ERR_TABLE_FULL on failure. */
iris_error_t ns_lookup     (const char *name, struct task *t,
                             iris_rights_t req_rights, handle_id_t *out_handle);

/* Remove service by name.  Releases the retained KObject. */
iris_error_t ns_unregister (const char *name);
void         ns_unregister_owner(struct KProcess *owner);

#endif
