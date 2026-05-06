#ifndef IRIS_NC_KIRQCAP_H
#define IRIS_NC_KIRQCAP_H

/*
 * KIrqCap — IRQ routing capability.
 *
 * A KIrqCap authorizes the holder to register a specific hardware IRQ line
 * for delivery into a KChannel via SYS_IRQ_ROUTE_REGISTER.  The irq_num field
 * is immutable after allocation; it is set by the kernel at boot and embedded
 * in the object so callers cannot forge a different vector.
 *
 * Rights model:
 *   RIGHT_ROUTE   — may call SYS_IRQ_ROUTE_REGISTER with this cap as arg0.
 *   RIGHT_DUPLICATE / RIGHT_TRANSFER — may copy or move the handle.
 *
 * Allocation:
 *   KIrqCap objects are kpage-backed; there is no fixed allocator pool cap.
 */

#include <iris/nc/kobject.h>
#include <stdint.h>

#define KIRQCAP_POOL_SIZE 0u

struct KIrqCap {
    struct KObject base;
    uint8_t irq_num;  /* hardware IRQ vector — immutable after alloc */
};

#ifdef __KERNEL__
struct KIrqCap *kirqcap_alloc(uint8_t irq_num);
void            kirqcap_free(struct KIrqCap *cap);
uint32_t        kirqcap_live_count(void);
#endif

#endif /* IRIS_NC_KIRQCAP_H */
