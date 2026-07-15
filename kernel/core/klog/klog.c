#include <iris/klog.h>
#include <iris/nc/spinlock.h>
#include <stdint.h>

/*
 * Ring buffer: klog_buf[KLOG_BUF_SIZE] with klog_head / klog_len.
 * Writes overwrite the oldest byte when the ring is full — no drops.
 * klog_get_buf linearizes the ring into klog_out and returns a flat pointer;
 * the caller (sys_klog_drain) copies that to userspace and then calls klog_clear.
 */
static char     klog_buf[KLOG_BUF_SIZE];
static char     klog_out[KLOG_BUF_SIZE];
static uint32_t klog_head = 0;
static uint32_t klog_len  = 0;

static irq_spinlock_t klog_lock;

#ifdef IRIS_KLOG_SERIAL_MIRROR
static inline void klog_dbg_putc(char c) {
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)c), "Nd"((uint16_t)0x3F8));
}
#endif

void klog_write(const char *s) {
    if (!s) return;
    uint64_t saved = irq_spinlock_lock(&klog_lock);
    while (*s) {
#ifdef IRIS_KLOG_SERIAL_MIRROR
        if (*s == '\n') klog_dbg_putc('\r');
        klog_dbg_putc(*s);
#endif
        uint32_t tail = (klog_head + klog_len) % KLOG_BUF_SIZE;
        klog_buf[tail] = *s++;
        if (klog_len == KLOG_BUF_SIZE)
            klog_head = (klog_head + 1) % KLOG_BUF_SIZE;
        else
            klog_len++;
    }
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
    for (uint32_t j = 0; j < len; j++) {
        uint32_t tail = (klog_head + klog_len) % KLOG_BUF_SIZE;
        klog_buf[tail] = p[j];
        if (klog_len == KLOG_BUF_SIZE)
            klog_head = (klog_head + 1) % KLOG_BUF_SIZE;
        else
            klog_len++;
    }
    irq_spinlock_unlock(&klog_lock, saved);
}

const char *klog_get_buf(uint32_t *out_len) {
    uint64_t saved = irq_spinlock_lock(&klog_lock);
    uint32_t n = klog_len;
    uint32_t h = klog_head;
    for (uint32_t i = 0; i < n; i++)
        klog_out[i] = klog_buf[(h + i) % KLOG_BUF_SIZE];
    if (out_len) *out_len = n;
    irq_spinlock_unlock(&klog_lock, saved);
    return klog_out;
}

void klog_clear(void) {
    uint64_t saved = irq_spinlock_lock(&klog_lock);
    klog_head = 0;
    klog_len  = 0;
    irq_spinlock_unlock(&klog_lock, saved);
}
