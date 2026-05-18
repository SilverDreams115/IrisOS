#include <iris/klog.h>
#include <stdint.h>

static char     klog_buf[KLOG_BUF_SIZE];
static uint32_t klog_len = 0;

void klog_write(const char *s) {
    if (!s) return;
    while (*s && klog_len < KLOG_BUF_SIZE)
        klog_buf[klog_len++] = *s++;
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
    uint32_t j;
    for (j = 0; j < len && klog_len < KLOG_BUF_SIZE; j++)
        klog_buf[klog_len++] = p[j];
}

const char *klog_get_buf(uint32_t *out_len) {
    if (out_len) *out_len = klog_len;
    return klog_buf;
}

void klog_clear(void) {
    klog_len = 0;
}
