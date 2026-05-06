#ifndef IRIS_NC_KBOOTCAP_H
#define IRIS_NC_KBOOTCAP_H

#include <iris/nc/kobject.h>
#include <stdint.h>

#define KBOOTCAP_POOL_SIZE 0u

#define IRIS_BOOTCAP_NONE          0u
#define IRIS_BOOTCAP_SPAWN_SERVICE (1u << 0)
#define IRIS_BOOTCAP_HW_ACCESS     (1u << 1)  /* may request IRQ/ioport caps via syscall */
#define IRIS_BOOTCAP_KDEBUG        (1u << 2)  /* may call SYS_WRITE for serial debug output */

struct KBootstrapCap {
    struct KObject base;
    uint32_t permissions;
};

struct KBootstrapCap *kbootcap_alloc(uint32_t permissions);
void                  kbootcap_free(struct KBootstrapCap *cap);
struct KBootstrapCap *kbootcap_clone_restricted(const struct KBootstrapCap *src,
                                                uint32_t new_permissions);

static inline int kbootcap_allows(const struct KBootstrapCap *cap, uint32_t permissions) {
    return cap && (cap->permissions & permissions) == permissions;
}

#endif
