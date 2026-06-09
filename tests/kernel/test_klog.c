/*
 * test_klog.c — unit tests for klog ring buffer.
 *
 * Verifies that klog_write/klog_write_dec/klog_get_buf/klog_clear are
 * self-consistent and that the ring-buffer overflow policy (overwrite oldest)
 * works correctly.  The irq_spinlock stubs in tests/kernel/include make the
 * locking calls no-ops on the host, so only buffer logic is exercised.
 */
#include "framework.h"
#include <iris/klog.h>
#include <string.h>
#include <stdio.h>

void test_klog(void) {
    TEST_SUITE("klog");

    /* ── basic write + get_buf ─────────────────────────────────────── */
    {
        klog_clear();
        klog_write("hello");
        uint32_t n = 0;
        const char *buf = klog_get_buf(&n);
        ASSERT_EQ(n, 5u);
        ASSERT_TRUE(memcmp(buf, "hello", 5) == 0);
    }

    /* ── accumulate multiple writes ────────────────────────────────── */
    {
        klog_clear();
        klog_write("foo");
        klog_write("bar");
        uint32_t n = 0;
        const char *buf = klog_get_buf(&n);
        ASSERT_EQ(n, 6u);
        ASSERT_TRUE(memcmp(buf, "foobar", 6) == 0);
    }

    /* ── klog_clear resets length to zero ──────────────────────────── */
    {
        klog_write("data");
        klog_clear();
        uint32_t n = 99u;
        (void)klog_get_buf(&n);
        ASSERT_EQ(n, 0u);
    }

    /* ── klog_write_dec basic ──────────────────────────────────────── */
    {
        klog_clear();
        klog_write_dec(42u);
        uint32_t n = 0;
        const char *buf = klog_get_buf(&n);
        ASSERT_EQ(n, 2u);
        ASSERT_TRUE(memcmp(buf, "42", 2) == 0);
    }

    /* ── klog_write_dec zero ───────────────────────────────────────── */
    {
        klog_clear();
        klog_write_dec(0u);
        uint32_t n = 0;
        const char *buf = klog_get_buf(&n);
        ASSERT_EQ(n, 1u);
        ASSERT_TRUE(buf[0] == '0');
    }

    /* ── klog_write_dec large value ────────────────────────────────── */
    {
        klog_clear();
        klog_write_dec(1234567890u);
        uint32_t n = 0;
        const char *buf = klog_get_buf(&n);
        ASSERT_EQ(n, 10u);
        ASSERT_TRUE(memcmp(buf, "1234567890", 10) == 0);
    }

    /* ── ring overflow: oldest bytes overwritten ───────────────────── */
    {
        klog_clear();
        /* Write KLOG_BUF_SIZE 'A' bytes then one 'B'. */
        char filler[KLOG_BUF_SIZE];
        memset(filler, 'A', KLOG_BUF_SIZE);
        /* Write in chunks since filler is not NUL-terminated as a string;
         * klog_write stops at NUL, so write byte by byte using write_dec
         * would be slow — instead write "A" string KLOG_BUF_SIZE times.
         * Use a NUL-terminated 1-char buffer. */
        klog_clear();
        for (uint32_t i = 0; i < KLOG_BUF_SIZE; i++) {
            char c[2];
            c[0] = 'A'; c[1] = '\0';
            klog_write(c);
        }
        /* Buffer is now full. Write one more byte: should overwrite oldest. */
        klog_write("B");
        uint32_t n = 0;
        const char *buf = klog_get_buf(&n);
        /* Length stays at KLOG_BUF_SIZE (ring is full). */
        ASSERT_EQ(n, KLOG_BUF_SIZE);
        /* Last byte must be 'B'. */
        ASSERT_TRUE(buf[KLOG_BUF_SIZE - 1] == 'B');
        /* First byte must be 'A' (oldest overwritten was 'A', all remaining are 'A'). */
        ASSERT_TRUE(buf[0] == 'A');
    }

    /* ── get_buf with NULL out_len does not crash ──────────────────── */
    {
        klog_clear();
        klog_write("x");
        const char *buf = klog_get_buf(NULL);
        ASSERT_NOT_NULL(buf);
    }

    /* ── write empty string is a no-op ────────────────────────────── */
    {
        klog_clear();
        klog_write("");
        uint32_t n = 99u;
        (void)klog_get_buf(&n);
        ASSERT_EQ(n, 0u);
    }

    /* ── write NULL is a no-op ─────────────────────────────────────── */
    {
        klog_clear();
        klog_write(NULL);
        uint32_t n = 99u;
        (void)klog_get_buf(&n);
        ASSERT_EQ(n, 0u);
    }
}
