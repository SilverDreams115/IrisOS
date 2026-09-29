/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fmt.c — the conversion, and nothing else.
 *
 * Pure: it reads its arguments and writes through a sink the caller supplies.
 * `tests/kernel/test_libc_fmt.c` compiles it for the host and checks it
 * against glibc, which is the only way anybody was ever going to notice that
 * `%.*s` printed itself.
 *
 * ── The conversion ─────────────────────────────────────────────────────────
 *
 * A real subset, and the boundary is written down rather than discovered:
 * `%d %i %u %x %X %o %c %s %p %%`; the length modifiers `hh h l ll z t j`; a
 * field width and a precision, either literal or `*`; and the flags `-` `0`
 * `+` space `#`.  No floating point — there is no soft-float here and a
 * program that printed a double would get an answer this file cannot justify
 * — and no `%n`, ever.
 *
 * ── What happens at a conversion this does not implement ───────────────────
 *
 * It STOPS, and says so with a negative return.
 *
 * The old behaviour was to echo the two characters and carry on, which reads
 * like leniency and is not.  A conversion that is not recognised is also not
 * CONSUMED, so every `va_arg` after it takes the wrong argument: `%.*s` — an
 * ordinary thing to write, and unsupported until now — printed itself, left
 * two arguments on the list, and made a `%d` further along the same format
 * print a pointer.  A wrong answer that looks like an answer is worse than no
 * answer.
 *
 * A trailing `%` is the one case still treated leniently, and the distinction
 * is the one above rather than a soft spot: it consumes no argument, so it
 * cannot put the list out of step.
 */
#include "fmt.h"
#include <stdint.h>

/* Local, because this file must not depend on `string.c` to be compiled on its
 * own — and because measuring a string with a BOUND is the thing the precision
 * needs and `strlen` cannot do. */
static size_t fmt_len(const char *s, long prec) {
    size_t n = 0;
    if (prec >= 0) { while (n < (size_t)prec && s[n]) n++; return n; }
    while (s[n]) n++;
    return n;
}

static void out_ch(struct iris_fmt_out *o, char c) {
    o->total++;
    /* One byte of the buffer is never filled: the sink NUL-terminates what it
     * is handed, and a line buffer with no room for that terminator is a line
     * buffer that overruns by one on the very first full line. */
    if (o->n + 1u >= o->cap) {
        if (o->flush) o->flush(o);
        else return;                       /* snprintf truncates, and counts */
    }
    o->buf[o->n++] = c;
    if (o->flush && c == '\n') o->flush(o);
}

static void out_str(struct iris_fmt_out *o, const char *s, size_t n) {
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

static void pad(struct iris_fmt_out *o, char c, long n) {
    while (n-- > 0) out_ch(o, c);
}

int iris_vformat(struct iris_fmt_out *o, const char *f, va_list ap) {
    for (; *f; f++) {
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        long width = 0, prec = -1;
        int lng = 0;                        /* 0=int 1=long 2=long long/size */
        int shrt = 0;                       /* 1=h 2=hh                      */
        char num[24];
        uint32_t nd;
        int neg = 0;
        uint64_t uv = 0;
        uint32_t base = 10;
        int upper = 0;
        const char *pfx = 0;
        size_t pfxn = 0;
        const char *s;

        if (*f != '%') { out_ch(o, *f); continue; }
        f++;

        for (;; f++) {
            if (*f == '-')      left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1;
            else break;
        }

        /* Width.  A NEGATIVE `*` width is a left-justified positive one, which
         * is what C says and is easy to get wrong by clamping it to zero. */
        if (*f == '*') {
            int w = va_arg(ap, int);
            if (w < 0) { left = 1; width = -(long)w; } else width = w;
            f++;
        } else {
            while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        }

        /* Precision.  A bare `.` is a precision of ZERO, not an absent one, and
         * a negative `*` precision is absent rather than zero. */
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') {
                int pv = va_arg(ap, int);
                prec = (pv < 0) ? -1 : (long)pv;
                f++;
            } else {
                while (*f >= '0' && *f <= '9') { prec = prec * 10 + (*f - '0'); f++; }
            }
        }

        while (*f == 'h') { shrt++; f++; }
        while (*f == 'l') { lng++; f++; }
        if (*f == 'z' || *f == 't' || *f == 'j') { lng = 2; f++; }
        if (lng > 2) lng = 2;
        if (shrt > 2) shrt = 2;

        switch (*f) {
            /* A format that ends INSIDE a conversion consumed no argument
             * either, so it is the lenient case too. */
            case '\0': return (int)o->total;
            case '%':  out_ch(o, '%'); continue;
            case 'c':  { char c = (char)va_arg(ap, int);
                         if (!left) pad(o, ' ', width - 1);
                         out_ch(o, c);
                         if (left) pad(o, ' ', width - 1);
                         continue; }
            case 's': {
                size_t sl;
                s = va_arg(ap, const char *);
                /* A null is PRINTED, not followed.  A program that passes one
                 * has a bug, and faulting inside printf tells it nothing. */
                if (!s) s = "(null)";
                /* With a precision the string need not be terminated at all
                 * within it, so the length is MEASURED against the bound
                 * rather than taken from strlen and then clamped. */
                sl = fmt_len(s, prec);
                if (!left) pad(o, ' ', width - (long)sl);
                out_str(o, s, sl);
                if (left) pad(o, ' ', width - (long)sl);
                continue;
            }
            case 'p': {
                void *pv = va_arg(ap, void *);
                long body;
                nd = u_to_text(num, (uint64_t)(uintptr_t)pv, 16u, 0);
                body = (long)nd + 2;
                if (!left) pad(o, ' ', width - body);
                out_str(o, "0x", 2u);
                while (nd) out_ch(o, num[--nd]);
                if (left) pad(o, ' ', width - body);
                continue;
            }
            case 'd': case 'i': {
                long long v = (lng == 0) ? (long long)va_arg(ap, int)
                            : (lng == 1) ? (long long)va_arg(ap, long)
                                         : va_arg(ap, long long);
                if      (shrt == 1) v = (long long)(short)v;
                else if (shrt == 2) v = (long long)(signed char)v;
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
                if      (shrt == 1) uv &= 0xFFFFu;
                else if (shrt == 2) uv &= 0xFFu;
                break;
            default:
                /* See the file header: this cannot be rendered AND cannot be
                 * skipped, because skipping it desynchronises the argument
                 * list.  Everything already written stands; nothing more is. */
                return IRIS_FMT_BAD;
        }

        nd = u_to_text(num, uv, base, upper);
        /* Precision zero and a zero value produce NO characters at all, which
         * is the one conversion in C that renders nothing. */
        if (prec == 0 && uv == 0u) nd = 0;
        {
            long zeros = (prec > (long)nd) ? prec - (long)nd : 0;
            long sign  = (neg || plus || space) ? 1 : 0;
            long body;

            if (alt && uv != 0u) {
                if (base == 16u) { pfx = upper ? "0X" : "0x"; pfxn = 2u; }
                /* An octal gains a leading zero only if it has not got one,
                 * and the leading digit is the LAST one written. */
                else if (base == 8u && zeros == 0 && num[nd - 1u] != '0') zeros = 1;
            }
            body = (long)nd + zeros + sign + (long)pfxn;
            /* An explicit precision makes `0` meaningless, as C requires. */
            if (prec >= 0) zero = 0;

            if (!left && !zero) pad(o, ' ', width - body);
            if (neg) out_ch(o, '-');
            else if (plus) out_ch(o, '+');
            else if (space) out_ch(o, ' ');
            if (pfxn) out_str(o, pfx, pfxn);
            if (!left && zero) pad(o, '0', width - body);
            pad(o, '0', zeros);
            while (nd) out_ch(o, num[--nd]);
            if (left) pad(o, ' ', width - body);
        }
    }
    return (int)o->total;
}

