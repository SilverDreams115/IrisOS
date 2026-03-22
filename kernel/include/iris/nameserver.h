#ifndef IRIS_NAMESERVER_H
#define IRIS_NAMESERVER_H

#include <iris/nc/kobject.h>
#include <iris/nc/rights.h>
#include <iris/nc/handle.h>
#include <iris/nc/error.h>
#include <iris/task.h>
#include <stdint.h>

/*
 * Kernel-side nameserver — maps service names to KObjects.
 *
 * This is a bootstrap registry: the kernel registers well-known services
 * at startup so that user processes can look them up by name.
 *
 * Design:
 *   - ns_register retains the KObject (kobject_retain).
 *   - ns_lookup inserts a NEW handle into the requesting task's table
 *     (kobject_retain again) and returns the handle_id.
 *   - The caller of ns_lookup receives at most the rights registered.
 *   - ns_unregister releases the registered KObject.
 *
 * Invariants:
 *   - Names are null-terminated, max NS_NAME_LEN-1 characters.
 *   - At most NS_MAX_ENTRIES simultaneous registrations.
 *   - All operations are protected by an internal spinlock.
 */

#define NS_MAX_ENTRIES 32
#define NS_NAME_LEN    16   /* max service name length including null */

void         ns_init       (void);

/* Register obj under name with rights.  Retains obj.
 * Returns IRIS_ERR_ALREADY_EXISTS if name is taken. */
iris_error_t ns_register   (const char *name, struct KObject *obj,
                             iris_rights_t rights);

/* Insert a handle to the named service into task t's handle table.
 * Rights are capped to what was registered, intersected with req_rights.
 * Pass RIGHT_SAME_RIGHTS to get all registered rights.
 * Returns HANDLE_INVALID if name not found or table full. */
handle_id_t  ns_lookup     (const char *name, struct task *t,
                             iris_rights_t req_rights);

/* Remove service by name.  Releases the retained KObject. */
iris_error_t ns_unregister (const char *name);

#endif
