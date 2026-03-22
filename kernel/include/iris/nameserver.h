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
 *   - ns_register: currently called by the kernel in boot sequence
 *     to publish services (e.g. "kbd").  A future service manager
 *     process will own this instead.
 *   - ns_lookup: consumed by user processes via SYS_NS_LOOKUP to
 *     obtain a capability handle to a named service.
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
 * that owns service registration and lifecycle policy.  The kernel
 * side retains only the capability-transfer mechanism (handle table
 * + KChannel); name resolution moves out of the kernel entirely.
 * When that transition happens, ns_register (kernel-side direct
 * call) disappears; SYS_NS_REGISTER may be removed or restricted to
 * the service manager process.  SYS_NS_LOOKUP may evolve into a
 * request sent over a well-known bootstrap channel.
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
