/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stdlib.c — the conversions, the searches, and the environment.
 *
 * Nothing here talks to the system except `getenv`, and what that has to say
 * about this system is the interesting part: see below.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include "libc_internal.h"

int  abs(int n)   { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

static int is_space(int c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static int digit_of(int c, int base) {
    int v;
    if (c >= '0' && c <= '9')      v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return -1;
    return (v < base) ? v : -1;
}

/*
 * ── One accumulate, two functions over it ──────────────────────────────────
 *
 * `strtol` must NOT be written in terms of `strtoul`.  It looks like it can
 * be — the parsing is identical — but `strtoul` has already applied the sign
 * by the time it returns, so a `strtol` that re-examines its result is
 * checking a negated value against a magnitude's limits.  `strtol("-1234")`
 * then saturates to LONG_MIN, which is a wrong answer that no test of the
 * unsigned form can see.
 *
 * So the shared half returns the MAGNITUDE and the sign separately, and each
 * public function applies its own range rule to them.
 */
static unsigned long str_acc(const char *s, char **end, int base,
                             int *neg_out, int *over_out) {
    const char *p = s;
    unsigned long acc = 0;
    int any = 0;

    *neg_out = 0;
    *over_out = 0;
    while (is_space((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') { *neg_out = (*p == '-'); p++; }
    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X') && digit_of((unsigned char)p[2], 16) >= 0) {
        p += 2; base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        if (end) *end = (char *)s;
        (void)__libc_fail(EINVAL);
        return 0;
    }

    for (;;) {
        int d = digit_of((unsigned char)*p, base);
        if (d < 0) break;
        any = 1;
        /* Checked BEFORE the multiply, because after it the evidence is gone. */
        if (acc > ((unsigned long)-1 - (unsigned long)d) / (unsigned long)base)
            *over_out = 1;
        else
            acc = acc * (unsigned long)base + (unsigned long)d;
        p++;
    }
    /* No digits at all: nothing was converted, and `end` must say so by
     * pointing at where the CALLER started rather than where we stopped. */
    if (end) *end = (char *)(any ? p : s);
    return acc;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg, over;
    unsigned long v = str_acc(s, end, base, &neg, &over);
    if (over) { (void)__libc_fail(ERANGE); return (unsigned long)-1; }
    /* A negated unsigned conversion wraps, which is what C says it does. */
    return neg ? (unsigned long)(-(long)v) : v;
}

long strtol(const char *s, char **end, int base) {
    int neg, over;
    unsigned long v = str_acc(s, end, base, &neg, &over);
    const unsigned long lim = neg ? 0x8000000000000000UL : 0x7FFFFFFFFFFFFFFFUL;

    if (over || v > lim) {
        (void)__libc_fail(ERANGE);
        return neg ? (long)0x8000000000000000UL : (long)0x7FFFFFFFFFFFFFFFUL;
    }
    return neg ? -(long)v : (long)v;
}

int  atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

char *strdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return 0;
    n = strlen(s) + 1u;
    p = malloc(n);
    if (!p) return 0;                 /* malloc already set ENOMEM */
    memcpy(p, s, n);
    return p;
}

/*
 * ── Sorting and searching ──────────────────────────────────────────────────
 *
 * Insertion sort, and that is a deliberate answer rather than a placeholder.
 * `qsort` is required to work; it is not required to be quick, and the
 * alternative here is a quicksort whose recursion depth is a stack this
 * program did not size for it.  A program that sorts enough elements for the
 * difference to matter is the reason to write a better one, and there is no
 * such program yet — the same sentence `malloc.c` makes, for the same reason.
 */
void qsort(void *base, size_t n, size_t sz,
           int (*cmp)(const void *, const void *)) {
    unsigned char *a = base;
    if (!a || !cmp || sz == 0u) return;
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && cmp(a + (j - 1) * sz, a + j * sz) > 0; j--) {
            unsigned char *x = a + (j - 1) * sz, *y = a + j * sz;
            for (size_t k = 0; k < sz; k++) {
                unsigned char t = x[k]; x[k] = y[k]; y[k] = t;
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *)) {
    const unsigned char *a = base;
    size_t lo = 0, hi = n;
    if (!key || !a || !cmp || sz == 0u) return 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;      /* cannot overflow */
        int r = cmp(key, a + mid * sz);
        if (r == 0) return (void *)(a + mid * sz);
        if (r < 0) hi = mid; else lo = mid + 1u;
    }
    return 0;
}

/*
 * ── The environment ────────────────────────────────────────────────────────
 *
 * `environ` is whatever the spawner put on the initial stack, and on IRIS that
 * is EMPTY — `proc` passes an argument vector and no environment at all.
 *
 * That is worth stating rather than leaving to be discovered, because the
 * environment is exactly the mechanism charter §6 objected to in a POSIX
 * personality: a name-value table, inherited invisibly, that changes what a
 * program can reach.  `LD_LIBRARY_PATH` is the canonical example and it is the
 * one this system's dynamic linker refuses to have.  So `getenv` here is not
 * unimplemented — it is implemented, over a table that is empty because
 * nothing in this system has a reason to fill it, and a program that needs to
 * be told something is told it in `argv` where the spawner can be seen saying
 * it.
 */
char **environ;

char *getenv(const char *name) {
    size_t n;
    if (!name || !*name || !environ) return 0;
    n = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    }
    return 0;
}
