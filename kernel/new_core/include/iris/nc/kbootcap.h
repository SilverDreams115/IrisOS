#ifndef IRIS_NC_KBOOTCAP_H
#define IRIS_NC_KBOOTCAP_H

#include <iris/nc/kobject.h>
#include <stdint.h>

#define KBOOTCAP_POOL_SIZE 4u

#define IRIS_BOOTCAP_NONE          0u
#define IRIS_BOOTCAP_SPAWN_SERVICE (1u << 0)
#define IRIS_BOOTCAP_HW_ACCESS     (1u << 1)  /* may request IRQ/ioport caps via syscall */

struct KBootstrapCap {
    struct KObject base;
    uint32_t permissions;
};

struct KBootstrapCap *kbootcap_alloc(uint32_t permissions);
void                  kbootcap_free(struct KBootstrapCap *cap);

static inline int kbootcap_allows(const struct KBootstrapCap *cap, uint32_t permissions) {
    return cap && (cap->permissions & permissions) == permissions;
}

#endif
