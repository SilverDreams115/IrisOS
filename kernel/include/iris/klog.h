#ifndef IRIS_KLOG_H
#define IRIS_KLOG_H

#include <stdint.h>

/*
 * klog — kernel boot-log ring buffer.
 *
 * Ring-0 callers write via klog_write() / klog_write_dec() during boot.
 * Ring-3 drains via SYS_KLOG_DRAIN(65) (KDEBUG-gated) into a user buffer.
 * The drain is destructive: the buffer is cleared after each call.
 *
 * Buffer: 4096-byte ring (klog_buf + klog_out).  Overflow overwrites oldest bytes.
 * IRQ-safe spinlock: safe from early boot, IRQ context, and SMP syscall paths.
 */

#define KLOG_BUF_SIZE 4096u

void           klog_write(const char *s);
void           klog_write_dec(uint64_t n);
/* Hexadecimal, no "0x" prefix and no padding.  Added for the IOMMU's
 * capability registers (Stage 10-dma §10.2 step 2): a 64-bit bitfield read as
 * a decimal number is a number nobody can check against a specification. */
void           klog_write_hex(uint64_t n);
const char    *klog_get_buf(uint32_t *out_len);
void           klog_clear(void);

#endif /* IRIS_KLOG_H */
