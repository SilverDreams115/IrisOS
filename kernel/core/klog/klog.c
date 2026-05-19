#include <iris/klog.h>
#include <iris/nc/spinlock.h>
#include <stdint.h>

static char     klog_buf[KLOG_BUF_SIZE];
static uint32_t klog_len = 0;

/* IRQ-disabling spinlock: klog_write is called from both kernel_main (before
 * scheduler) and from SYS_KLOG_DRAIN (syscall path, IRQs masked by SFMASK).
 * The lock is taken in the drain path so concurrent readers see a consistent
 * buffer snapshot, and protects against SMP callers when APs are brought up. */
static irq_spinlock_t klog_lock;

void klog_write(const char *s) {
    if (!s) return;
    uint64_t saved = irq_spinlock_lock(&klog_lock);
    while (*s && klog_len < KLOG_BUF_SIZE)
        klog_buf[klog_len++] = *s++;
    irq_spinlock_unlock(&klog_lock, saved);
}

void klog_write_dec(uint64_t n) {
    char tmp[20];
    uint32_t i = 20u;
    if (n == 0) { klog_write("0"); return; }
    while (n > 0u && i > 0u) {
        tmp[--i] = (char)('0' + (int)(n % 10u));
        n /= 10u;
    }
    uint32_t len = 20u - i;
    const char *p = tmp + i;

    uint64_t saved = irq_spinlock_lock(&klog_lock);
    for (uint32_t j = 0; j < len && klog_len < KLOG_BUF_SIZE; j++)
        klog_buf[klog_len++] = p[j];
    irq_spinlock_unlock(&klog_lock, saved);
}

const char *klog_get_buf(uint32_t *out_len) {
    uint64_t saved = irq_spinlock_lock(&klog_lock);
    if (out_len) *out_len = klog_len;
    const char *buf = klog_buf;
    irq_spinlock_unlock(&klog_lock, saved);
    return buf;
}

void klog_clear(void) {
    uint64_t saved = irq_spinlock_lock(&klog_lock);
    klog_len = 0;
    irq_spinlock_unlock(&klog_lock, saved);
}
