#ifndef IRIS_NC_KINITRDENTRY_H
#define IRIS_NC_KINITRDENTRY_H

/*
 * KInitrdEntry — immutable reference to a named ELF image in the kernel initrd.
 *
 * Obtained via SYS_INITRD_LOOKUP(41); used by SYS_SPAWN_ELF(42).
 * Separating the lookup from the spawn allows svcmgr to verify a service
 * exists before allocating channels, and to pass the entry handle to a
 * delegated spawner without handing out the bootstrap capability itself.
 *
 * Invariants:
 *   - data and size point into the kernel-embedded initrd; never freed.
 *   - Multiple concurrent handles to the same name are allowed.
 *   - RIGHT_READ is required to spawn from the entry.
 */

#include <iris/nc/kobject.h>
#include <stdint.h>

#define KINITRDENTRY_POOL_SIZE 8u

struct KInitrdEntry {
    struct KObject  base;
    const void     *data; /* pointer into embedded initrd blob — kernel lifetime */
    uint32_t        size;
};

#ifdef __KERNEL__
struct KInitrdEntry *kinitrdentry_alloc(const void *data, uint32_t size);
void                 kinitrdentry_free(struct KInitrdEntry *e);
#endif

#endif /* IRIS_NC_KINITRDENTRY_H */
