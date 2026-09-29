/* SPDX-License-Identifier: Apache-2.0 */
/*
 * string.c — the handful a C program cannot start without.
 *
 * Plain, byte-at-a-time and unhurried on purpose: these exist so that a
 * program compiled against this libc links, not so that it is fast.  The day
 * a measurement says one of them matters is the day it is worth a word-at-a-
 * time version, and that day has not come.
 */
#include <string.h>

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    for (size_t i = 0; i < n; i++) dp[i] = sp[i];
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp == sp || n == 0) return d;
    /* Overlap is the whole reason this is not memcpy: copying forwards through
     * a destination that sits inside the source reads bytes it has already
     * overwritten. */
    if (dp < sp) { for (size_t i = 0; i < n; i++) dp[i] = sp[i]; }
    else         { for (size_t i = n; i-- > 0; )  dp[i] = sp[i]; }
    return d;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *dp = d;
    for (size_t i = 0; i < n; i++) dp[i] = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

char *strcpy(char *d, const char *s) {
    size_t i = 0;
    while ((d[i] = s[i]) != '\0') i++;
    return d;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}
