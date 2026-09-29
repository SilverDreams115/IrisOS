/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stdio.c — printf, and where the bytes go.
 *
 * They go to the CONSOLE ENDPOINT, which is a capability the spawner minted
 * into slot 3 before this program existed.  There is no file, no descriptor
 * table and no `open`: a program that was given no console simply cannot
 * print, and says so by the write returning short rather than by faulting.
 * Descriptors are step 7, where one is a CPtr and `open` is a VFS grant.
 *
 * ── The conversion ─────────────────────────────────────────────────────────
 *
 * A real subset, and the boundary is written down rather than discovered:
 * `%d %i %u %x %X %o %c %s %p %%`, the length modifiers `l` and `ll` and `z`,
 * a field width, `-` for left, `0` for zero-fill, and `+`/space for a sign.
 * No floating point — there is no soft-float here and a program that printed a
 * double would get an answer this file cannot justify — and no `%n`, ever.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "libc_internal.h"

/* One line at a time.  The console takes a message per call, so buffering to a
 * newline is the difference between one IPC per line and one per character. */
#define OUT_BUF 512u

struct out {
    char    *buf;      /* where characters go: the line buffer, or a caller's */
    size_t   cap;      /* its size                                            */
    size_t   n;        /* how many are in it                                  */
    size_t   total;    /* how many the caller asked for, including dropped    */
    int      to_console;
};

static void out_flush(struct out *o) {
    if (o->to_console && o->n) {
        o->buf[o->n] = '\0';
        __libc_console_write(o->buf, o->n);
        o->n = 0;
    }
}

static void out_ch(struct out *o, char c) {
    o->total++;
    if (o->n + 1u >= o->cap) {
        if (o->to_console) out_flush(o);
        else return;                       /* snprintf truncates, and counts */
    }
    o->buf[o->n++] = c;
    if (o->to_console && c == '\n') out_flush(o);
}

static void out_str(struct out *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) out_ch(o, s[i]);
}

/* Unsigned, any base up to 16, written backwards into a caller's scratch. */
static uint32_t u_to_text(char *b, uint64_t v, uint32_t base, int upper) {
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *d = upper ? up : lo;
    uint32_t n = 0;
    if (v == 0u) b[n++] = '0';
    while (v) { b[n++] = d[v % base]; v /= base; }
    return n;
}

static void pad(struct out *o, char c, long n) {
    while (n-- > 0) out_ch(o, c);
}

static int do_fmt(struct out *o, const char *f, va_list ap) {
    for (; *f; f++) {
        int left = 0, zero = 0, plus = 0, space = 0;
        long width = 0;
        int lng = 0;                        /* 0=int 1=long 2=long long/size */
        char num[24];
        uint32_t nd;
        int neg = 0;
        uint64_t uv = 0;
        uint32_t base = 10;
        int upper = 0;
        const char *s;

        if (*f != '%') { out_ch(o, *f); continue; }
        f++;
        for (;; f++) {
            if (*f == '-')      left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1;
            else break;
        }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        while (*f == 'l') { lng++; f++; }
        if (*f == 'z' || *f == 't') { lng = 2; f++; }
        if (lng > 2) lng = 2;

        switch (*f) {
            case '\0': return (int)o->total;   /* a trailing % is not a crash */
            case '%':  out_ch(o, '%'); continue;
            case 'c':  { char c = (char)va_arg(ap, int);
                         if (!left) pad(o, ' ', width - 1);
                         out_ch(o, c);
                         if (left) pad(o, ' ', width - 1);
                         continue; }
            case 's': {
                s = va_arg(ap, const char *);
                /* A null is PRINTED, not followed.  A program that passes one
                 * has a bug, and faulting inside printf tells it nothing. */
                if (!s) s = "(null)";
                size_t sl = strlen(s);
                if (!left) pad(o, ' ', width - (long)sl);
                out_str(o, s, sl);
                if (left) pad(o, ' ', width - (long)sl);
                continue;
            }
            case 'p': {
                void *p = va_arg(ap, void *);
                out_str(o, "0x", 2u);
                nd = u_to_text(num, (uint64_t)(uintptr_t)p, 16u, 0);
                while (nd) out_ch(o, num[--nd]);
                continue;
            }
            case 'd': case 'i': {
                long long v = (lng == 0) ? (long long)va_arg(ap, int)
                            : (lng == 1) ? (long long)va_arg(ap, long)
                                         : va_arg(ap, long long);
                if (v < 0) { neg = 1; uv = (uint64_t)(-(v + 1)) + 1u; }
                else uv = (uint64_t)v;
                break;
            }
            case 'u': base = 10u; goto unsigned_arg;
            case 'o': base = 8u;  goto unsigned_arg;
            case 'X': upper = 1;  /* fall through */
            case 'x': base = 16u;
            unsigned_arg:
                uv = (lng == 0) ? (uint64_t)va_arg(ap, unsigned)
                   : (lng == 1) ? (uint64_t)va_arg(ap, unsigned long)
                                : va_arg(ap, unsigned long long);
                break;
            default:
                /* An unknown conversion is echoed, so a typo shows up in the
                 * output instead of eating the argument list silently. */
                out_ch(o, '%'); out_ch(o, *f); continue;
        }

        nd = u_to_text(num, uv, base, upper);
        {
            long sign = (neg || plus || space) ? 1 : 0;
            long body = (long)nd + sign;
            if (!left && !zero) pad(o, ' ', width - body);
            if (neg) out_ch(o, '-');
            else if (plus) out_ch(o, '+');
            else if (space) out_ch(o, ' ');
            if (!left && zero) pad(o, '0', width - body);
            while (nd) out_ch(o, num[--nd]);
            if (left) pad(o, ' ', width - body);
        }
    }
    return (int)o->total;
}

int vprintf(const char *f, va_list ap) {
    static char line[OUT_BUF];
    struct out o = { line, OUT_BUF, 0, 0, 1 };
    int n = do_fmt(&o, f, ap);
    out_flush(&o);
    return n;
}

int printf(const char *f, ...) {
    va_list ap; int n;
    va_start(ap, f);
    n = vprintf(f, ap);
    va_end(ap);
    return n;
}

int vsnprintf(char *b, size_t cap, const char *f, va_list ap) {
    char sink;
    struct out o = { cap ? b : &sink, cap ? cap : 1u, 0, 0, 0 };
    int n = do_fmt(&o, f, ap);
    if (cap) b[o.n] = '\0';
    return n;                    /* what it WOULD have been, as C requires */
}

int snprintf(char *b, size_t cap, const char *f, ...) {
    va_list ap; int n;
    va_start(ap, f);
    n = vsnprintf(b, cap, f, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) {
    size_t n = strlen(s);
    __libc_console_write(s, n);
    __libc_console_write("\n", 1u);
    return (int)n + 1;
}

int putchar(int c) {
    char b = (char)c;
    __libc_console_write(&b, 1u);
    return c;
}

/* Nothing is held back: a line is written when it ends and `printf` without a
 * newline writes when the buffer fills.  `fflush` is here so that a program
 * that calls it links, and it tells the truth by doing nothing. */
int fflush(void *f) { (void)f; return 0; }
