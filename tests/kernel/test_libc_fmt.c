/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_libc_fmt.c — the C library's conversion, against a mature one.
 *
 * The defect this exists for produced output that looked like a typo.  An
 * unparsed precision fell through to a branch that echoed the two characters
 * and did NOT consume the argument, so `printf("%.*s", 3, s)` printed `%.*s`
 * and left two arguments on the list — and a `%d` further along the same
 * format then rendered a pointer.  Nothing in a running system reports that.
 * A program prints something slightly wrong and everybody reads past it.
 *
 * So the engine is compiled for the host and compared against glibc, which is
 * the only oracle available that nobody has to trust this project for.  Where
 * IRIS deliberately differs — an unsupported conversion is a refusal rather
 * than undefined behaviour — the test asserts the difference instead of
 * comparing, and says so.
 *
 * `iris_vformat` is named so it cannot collide with the host's `printf`, which
 * is what makes the comparison possible at all.
 */
#include "framework.h"
#include "../../services/libc/fmt.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

void test_libc_fmt(void);

/* A varargs shim, so a test can drive a sink it built itself. */
int iris_vformat_shim(struct iris_fmt_out *o, const char *f, ...);

/* A sink that never flushes: truncates and keeps counting, like snprintf. */
static int fmt_into(char *buf, size_t cap, const char *f, ...) {
    struct iris_fmt_out o = { buf, cap, 0, 0, 0, 0 };
    va_list ap;
    int n;
    va_start(ap, f);
    n = iris_vformat(&o, f, ap);
    va_end(ap);
    if (cap) buf[o.n] = '\0';
    return n;
}

/* A sink that DOES flush, so the console path's buffering is exercised too. */
struct catch_ctx { char out[4096]; size_t n; unsigned flushes; };
static void catch_flush(struct iris_fmt_out *o) {
    struct catch_ctx *c = o->ctx;
    if (!o->n) return;
    o->buf[o->n] = '\0';
    for (size_t i = 0; i < o->n && c->n + 1u < sizeof c->out; i++)
        c->out[c->n++] = o->buf[i];
    c->out[c->n] = '\0';
    o->n = 0;
    c->flushes++;
}

#define CK(fmt, ...) do {                                                     \
    char want[256], got[256];                                                 \
    int rw = snprintf(want, sizeof want, fmt, __VA_ARGS__);                   \
    int rg = fmt_into(got, sizeof got, fmt, __VA_ARGS__);                     \
    if (strcmp(want, got) != 0 || rw != rg) {                                 \
        fprintf(stderr, "  FAIL %s:%d: \"%s\" glibc=[%s]%d iris=[%s]%d\n",    \
                __FILE__, __LINE__, fmt, want, rw, got, rg);                  \
        g_fail++;                                                             \
    } else { g_pass++; }                                                      \
} while (0)

void test_libc_fmt(void) {
    printf("test_libc_fmt: the conversion, against glibc\n");

    /* ── integers, every flag that can apply to one ── */
    CK("%d", -42);            CK("%d", 0);             CK("%d", 2147483647);
    CK("%5d", 42);            CK("%-5d|", 42);         CK("%05d", 42);
    CK("%+d", 7);             CK("% d", 7);            CK("%+d", -7);
    CK("%u", 4294967295u);    CK("%x", 0xbeefu);       CK("%X", 0xbeefu);
    CK("%#x", 0xbeefu);       CK("%#X", 0xbeefu);      CK("%#x", 0u);
    CK("%o", 64u);            CK("%#o", 64u);          CK("%#o", 0u);
    CK("%08x", 0x1fu);        CK("%-8x|", 0x1fu);      CK("%#010x", 0xabu);

    /* ── lengths.  A truncating modifier that is ignored is a wrong number,
     *    not a formatting nicety. ── */
    CK("%ld", -1234567890123L);
    CK("%lld", -1234567890123LL);
    CK("%llu", 18446744073709551615ULL);
    CK("%lx", 0xdeadbeefcafeUL);
    CK("%zu", (size_t)4096);
    CK("%hd", (int)-70000);   CK("%hhd", (int)300);
    CK("%hx", (unsigned)0x12345u);
    CK("%hhx", (unsigned)0x1234u);

    /* ── strings, and the precision that used to desynchronise ── */
    CK("%s", "hola");         CK("%10s|", "hola");     CK("%-10s|", "hola");
    CK("%.2s", "hola");       CK("%.0s|", "hola");     CK("%.10s", "hola");
    CK("%.*s", 3, "abcdef");  CK("%.*s", 0, "abcdef"); CK("%.*s", -1, "abcdef");
    CK("%*d", 6, 42);         CK("%*d", -6, 42);
    CK("%.*d", 5, 42);        CK("%.3d", 7);           CK("%.0d|", 0);
    CK("%.5x", 0xabu);        CK("%8.3d|", 7);         CK("%-8.3d|", 7);
    /* The `0` a precision makes meaningless.  gcc warns about writing it,
     * which is fair advice and exactly the case that has to be checked. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    CK("%08.3d|", 7);
#pragma GCC diagnostic pop
    CK("%.4s|", "ab");

    /* ── characters, and a format with several conversions in it ── */
    CK("%c", 'Z');            CK("%3c|", 'Z');         CK("%-3c|", 'Z');
    CK("%s/%d", "libc", 6);
    CK("%d %u %x %-6s|%6s| %+d %05d", -42, 42u, 0xBEEFu, "left", "right", 7, 99);

    /* A literal percent consumes nothing, so it is compared like the rest. */
    { char want[16], got[16];
      int rw = snprintf(want, sizeof want, "100%%");
      int rg = fmt_into(got, sizeof got, "100%%");
      ASSERT_TRUE(strcmp(want, got) == 0 && rw == rg); }

    /* ── truncation: the RETURN is what it would have been ── */
    { char b[6]; int n = fmt_into(b, sizeof b, "%s", "abcdefghij");
      ASSERT_EQ(n, 10); ASSERT_TRUE(strcmp(b, "abcde") == 0); }
    /* ...and a zero-capacity sink still counts, which is how snprintf is used
     * to MEASURE a string before allocating room for it. */
    { char sink; ASSERT_EQ(fmt_into(&sink, 0u, "%d-%s", 123, "xy"), 6); }

    /* ── where IRIS differs on purpose ──
     * An unsupported conversion cannot be rendered AND cannot be skipped,
     * because skipping it puts the argument list out of step.  It refuses. */
    { char b[64]; ASSERT_EQ(fmt_into(b, sizeof b, "%f", 1.5), IRIS_FMT_BAD); }
    { char b[64]; ASSERT_EQ(fmt_into(b, sizeof b, "%e", 1.5), IRIS_FMT_BAD); }
    { char b[64]; ASSERT_EQ(fmt_into(b, sizeof b, "a%nb", (int *)0),
                            IRIS_FMT_BAD); }
    /* Everything rendered BEFORE the refusal still stands: a caller looking at
     * the output needs to see where the format went wrong. */
    { char b[64]; (void)fmt_into(b, sizeof b, "ok=%d then %f", 5, 1.5);
      ASSERT_TRUE(strncmp(b, "ok=5 then ", 10) == 0); }
    /* A trailing `%` is the one lenient case, and the reason is the rule
     * above rather than a soft spot: it consumes no argument. */
    { char b[64]; ASSERT_EQ(fmt_into(b, sizeof b, "x%"), 1); }

    /* ── a null string is printed, not followed ── */
    { char b[64]; (void)fmt_into(b, sizeof b, "%s", (char *)0);
      ASSERT_TRUE(strcmp(b, "(null)") == 0); }

    /* ── the flushing sink: every byte arrives, and the NUL never overruns ── */
    {
        struct catch_ctx c = { { 0 }, 0, 0 };
        char line[8];
        struct iris_fmt_out o = { line, sizeof line, 0, 0, catch_flush, &c };
        int n;
        /* Thirty characters through an eight-byte buffer.  `out_ch` keeps one
         * byte back for the terminator the sink writes; without that reserve
         * the first full line writes one past the end of `line`. */
        n = iris_vformat_shim(&o, "%s\n%s\n", "abcdefghijkl", "mno");
        if (o.n) catch_flush(&o);
        ASSERT_EQ(n, 17);
        ASSERT_TRUE(strcmp(c.out, "abcdefghijkl\nmno\n") == 0);
        ASSERT_TRUE(c.flushes >= 2u);
    }
}

int iris_vformat_shim(struct iris_fmt_out *o, const char *f, ...) {
    va_list ap; int n;
    va_start(ap, f);
    n = iris_vformat(o, f, ap);
    va_end(ap);
    return n;
}
