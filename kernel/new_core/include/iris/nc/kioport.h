#ifndef IRIS_NC_KIOPORT_H
#define IRIS_NC_KIOPORT_H

/*
 * KIoPort — I/O port range capability.
 *
 * A KIoPort authorizes ring-3 processes to perform IN/OUT instructions on a
 * contiguous range of x86 I/O ports via SYS_IOPORT_IN / SYS_IOPORT_OUT.
 * Without a KIoPort handle the kernel refuses all IN/OUT syscalls, making
 * hardware access capability-gated by construction.
 *
 * Fields:
 *   base_port  — first port in the authorized range (e.g. 0x60 for PS/2 data).
 *   count      — number of consecutive ports (must be > 0).
 *
 * Access check at syscall time:
 *   SYS_IOPORT_IN (arg1 = port_offset) and SYS_IOPORT_OUT (arg1 = port_offset)
 *   verify port_offset < count before executing the instruction.
 *
 * Rights model:
 *   RIGHT_READ    — may execute IN  (SYS_IOPORT_IN).
 *   RIGHT_WRITE   — may execute OUT (SYS_IOPORT_OUT).
 *   RIGHT_DUPLICATE / RIGHT_TRANSFER — may copy or move the handle.
 */

#include <iris/nc/kobject.h>
#include <stdint.h>

#define KIOPORT_POOL_SIZE 16u

struct KIoPort {
    struct KObject base;
    uint16_t base_port;  /* first authorized port */
    uint16_t count;      /* number of consecutive ports in the range */
};

#ifdef __KERNEL__
struct KIoPort *kioport_alloc(uint16_t base_port, uint16_t count);
void            kioport_free(struct KIoPort *port);
uint32_t        kioport_live_count(void);
#endif

#endif /* IRIS_NC_KIOPORT_H */
