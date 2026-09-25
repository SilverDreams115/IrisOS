/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/klog.h>
#include <iris/fbcon.h>
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
    /*
     * The screen, until ring 3 takes it.
     *
     * This is not a second copy of the ring: the ring is drained by a service
     * that does not exist yet for most of what gets logged here, and on a
     * machine with no serial port that made the whole of early boot
     * unobservable.  `fbcon_write` is a no-op once userspace owns the
     * framebuffer, so this costs nothing after boot.
     *
     * INSIDE the lock, because `fbcon` has a cursor and more than one core
     * calls this.  Two cores painting one screen interleave into a screen
     * that describes neither, and the lock that already serialises the ring
     * is the one that has to cover it -- fbcon owns no lock of its own
     * precisely so that it cannot introduce a second rank into an order this
     * tree checks (`make check-locks`).
     */
    fbcon_write(s);
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
    /* The screen, for the same reason klog_write mirrors, and inside the same
     * lock: this writer bypasses klog_write and pushes bytes into the ring
     * directly, so without it a logged NUMBER was the one thing that did not
     * reach the screen — and "free RAM:  MB" is a line that passes a banner
     * check and tells you nothing. */
    for (uint32_t j = 0; j < len; j++) fbcon_putc(p[j]);
    for (uint32_t j = 0; j < len; j++) {
#ifdef IRIS_KLOG_SERIAL_MIRROR
        klog_dbg_putc(p[j]);
#endif
        uint32_t tail = (klog_head + klog_len) % KLOG_BUF_SIZE;
        klog_buf[tail] = p[j];
        if (klog_len == KLOG_BUF_SIZE)
            klog_head = (klog_head + 1) % KLOG_BUF_SIZE;
        else
            klog_len++;
    }
    irq_spinlock_unlock(&klog_lock, saved);
}

void klog_write_hex(uint64_t n) {
    char buf[17];
    int  i = 0;
    if (n == 0) { klog_write("0"); return; }
    while (n && i < 16) {
        uint8_t d = (uint8_t)(n & 0xFu);
        buf[i++] = (char)(d < 10u ? (uint8_t)('0' + d) : (uint8_t)('a' + (d - 10u)));
        n >>= 4;
    }
    char out[17];
    int  k = 0;
    while (i--) out[k++] = buf[i];
    out[k] = 0;
    klog_write(out);
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
