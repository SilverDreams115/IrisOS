/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_LIBC_FMT_H
#define IRIS_LIBC_FMT_H

#include <stddef.h>
#include <stdarg.h>

/*
 * fmt.h — the conversion engine, with nothing of this system in it.
 *
 * It is separated from `stdio.c` for one reason: everything here is a function
 * of its arguments, so it can be compiled for the HOST and checked against a
 * mature `printf` as an oracle.  That is not a nicety.  The defect this file
 * was carved out of — an unparsed precision falling through to a branch that
 * echoed two characters and did not consume the argument — produced output
 * that looked like a typo and silently made every later conversion in the same
 * format read the wrong argument.  Nothing in a running system would have
 * caught it; fifty formats compared against glibc catch it in a second.
 *
 * The sink is a callback rather than a flag, so the same engine serves the
 * console (flush at a newline and when full) and `snprintf` (never flush,
 * truncate, but keep counting) — and so a test can supply a third.
 */

struct iris_fmt_out {
    char   *buf;     /* where characters go: a line buffer, or a caller's   */
    size_t  cap;     /* its size; at least one byte is kept for a NUL       */
    size_t  n;       /* how many are in it                                  */
    size_t  total;   /* how many the caller asked for, INCLUDING dropped    */
    /* Called when the buffer is full and at every newline.  NULL means the
     * output truncates instead, which is what `snprintf` wants. */
    void  (*flush)(struct iris_fmt_out *o);
    void   *ctx;     /* the sink's own, untouched here */
};

/* A format this cannot render.  Negative, and distinct from any length. */
#define IRIS_FMT_BAD (-1)

/*
 * Render `f` into `o`.  Returns the number of characters the format WOULD have
 * produced — as C requires of `snprintf`, and regardless of truncation — or
 * `IRIS_FMT_BAD` at a conversion this does not implement.
 *
 * It does not flush at the end; the caller owns that, because the caller is
 * the one that knows whether a partial line should be written out.
 */
int iris_vformat(struct iris_fmt_out *o, const char *f, va_list ap);

#endif /* IRIS_LIBC_FMT_H */
