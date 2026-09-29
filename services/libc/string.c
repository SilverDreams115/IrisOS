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

/*
 * ── The rest of what a C program assumes is there ──────────────────────────
 *
 * Added because "an ordinary C program, unmodified" was the close condition of
 * Stage 10-run step 6, and a program that cannot call `strstr` is a program
 * somebody had to modify.  Same discipline as above: plain, byte-at-a-time,
 * and correct — including at the edges, which is where these are usually
 * wrong.  `strncat` writing n+1 bytes and `strcspn` walking off a string with
 * no match are the two classic ones, and both are avoided by construction
 * rather than by testing for them.
 */

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

char *strcat(char *d, const char *s) {
    char *at = d + strlen(d);
    strcpy(at, s);
    return d;
}

/*
 * `strncat` bounds the SOURCE, not the result, and always terminates — so it
 * writes at most n+1 bytes past the existing end.  That asymmetry with
 * `strncpy` is a fact about C rather than a choice here, and stating it is the
 * only defence against the buffer being sized as if it were n.
 */
char *strncat(char *d, const char *s, size_t n) {
    char *at = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) at[i] = s[i];
    at[i] = '\0';
    return d;
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

void *memchr(const void *p, int c, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i] == (unsigned char)c) return (void *)(b + i);
    return 0;
}

/* The empty needle matches at the start, which is what C says and what a naive
 * loop gets wrong by returning NULL. */
char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (needle[i] && hay[i] == needle[i]) i++;
        if (!needle[i]) return (char *)hay;
    }
    return 0;
}

static int str_in(const char *set, char c) {
    for (; *set; set++) if (*set == c) return 1;
    return 0;
}

size_t strspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && str_in(set, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && !str_in(set, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *set) {
    for (; *s; s++) if (str_in(set, *s)) return (char *)s;
    return 0;
}

/*
 * `strtok_r` is the real one and `strtok` is a wrapper over a static.
 *
 * That order matters: the static is the only thread-unsafe thing in this file,
 * and keeping it out of the implementation means a program that cares can use
 * the reentrant form and pay nothing.  See `libc_lock.h` for what else still
 * assumes one thread.
 */
char *strtok_r(char *s, const char *sep, char **save) {
    char *start;
    if (!s) s = *save;
    if (!s) return 0;
    s += strspn(s, sep);
    if (!*s) { *save = 0; return 0; }
    start = s;
    s += strcspn(s, sep);
    if (*s) { *s = '\0'; *save = s + 1; }
    else    { *save = 0; }
    return start;
}

char *strtok(char *s, const char *sep) {
    static char *save;
    return strtok_r(s, sep, &save);
}
