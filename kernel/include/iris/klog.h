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
 * Buffer: 4096-byte flat BSS array.  Overflow is silently dropped.
 * No locking: klog_write is called before ring-3 is up (single-threaded).
 */

#define KLOG_BUF_SIZE 4096u

void           klog_write(const char *s);
void           klog_write_dec(uint64_t n);
const char    *klog_get_buf(uint32_t *out_len);
void           klog_clear(void);

#endif /* IRIS_KLOG_H */
