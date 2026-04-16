#ifndef IRIS_NC_RIGHTS_H
#define IRIS_NC_RIGHTS_H

#include <stdint.h>

typedef uint32_t iris_rights_t;

#define RIGHT_NONE        ((iris_rights_t)0x00000000)
#define RIGHT_READ        ((iris_rights_t)0x00000001)
#define RIGHT_WRITE       ((iris_rights_t)0x00000002)
#define RIGHT_DUPLICATE   ((iris_rights_t)0x00000004)
#define RIGHT_TRANSFER    ((iris_rights_t)0x00000008)  /* move handle to another process */
#define RIGHT_WAIT        ((iris_rights_t)0x00000010)
#define RIGHT_ROUTE       ((iris_rights_t)0x00000020)  /* authorize hardware route registration */
#define RIGHT_MANAGE      ((iris_rights_t)0x00000040)

/* Duplication request flag — never store in HandleEntry.rights */
#define RIGHT_SAME_RIGHTS ((iris_rights_t)0x80000000)

/*
 * Invariants:
 *   - Rights can only be reduced on duplication, never elevated
 *   - Closing a handle requires no rights
 *   - RIGHT_SAME_RIGHTS is an operation flag, not persistent state
 *   - No kernel path ever elevates the rights of an existing HandleEntry
 */

static inline int rights_check(iris_rights_t rights, iris_rights_t required) {
    return (rights & required) == required;
}

/* Returns intersection — never elevates */
static inline iris_rights_t rights_reduce(iris_rights_t base,
                                          iris_rights_t requested) {
    if (requested & RIGHT_SAME_RIGHTS) return base & ~RIGHT_SAME_RIGHTS;
    return base & requested;
}

#endif
