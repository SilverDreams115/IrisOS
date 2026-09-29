/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stdio.c — printf, and where the bytes go.
 *
 * The CONVERSION is `fmt.c`, which is pure and host-tested.  What is here is
 * the part that cannot be: the console sink, which writes over an endpoint
 * capability the spawner minted into slot 3 before this program existed.
 *
 * There is no file behind `stdout`: a program that was given no console simply
 * cannot print, and says so by the write returning short rather than by
 * faulting.  `open` and descriptors are `fd.c`, where one is a CPtr.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "fmt.h"
#include "libc_internal.h"
#include "libc_lock.h"
#include <errno.h>

/* One line at a time.  The console takes a message per call, so buffering to a
 * newline is the difference between one IPC per line and one per character. */
#define OUT_BUF 512u

static void console_flush(struct iris_fmt_out *o) {
    if (!o->n) return;
    o->buf[o->n] = '\0';
    __libc_console_write(o->buf, o->n);
    o->n = 0;
}

/* The line buffer is one buffer, so two threads rendering into it interleave
 * character by character.  The lock makes a `printf` atomic against another
 * `printf`, which is the guarantee a program actually relies on. */
static struct libc_lock g_out_lock = LIBC_LOCK_INIT;

int vprintf(const char *f, va_list ap) {
    static char line[OUT_BUF];
    struct iris_fmt_out o = { line, OUT_BUF, 0, 0, console_flush, 0 };
    int n;
    __libc_lock(&g_out_lock);
    n = iris_vformat(&o, f, ap);
    /* Whatever was rendered before the refusal is still WRITTEN.  Dropping it
     * would hide where the format went wrong, which is the one thing a caller
     * looking at this output needs to see. */
    console_flush(&o);
    __libc_unlock(&g_out_lock);
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
    struct iris_fmt_out o = { cap ? b : &sink, cap ? cap : 1u, 0, 0, 0, 0 };
    int n = iris_vformat(&o, f, ap);
    /* Terminated either way: a caller that ignores the negative return must
     * still find a string rather than whatever was in the buffer. */
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



/*
 * perror — the message, then what went wrong, on the console.
 *
 * It writes to `stderr`, which on this system is the same console endpoint
 * `stdout` is: there is one place for a program's output because the spawner
 * hands it one capability.  A program that wants them separated is asking for
 * two capabilities, and that is a decision at the spawn rather than here.
 */
void perror(const char *what) {
    const char *why = strerror(errno);
    if (what && what[0]) {
        __libc_console_write(what, strlen(what));
        __libc_console_write(": ", 2u);
    }
    __libc_console_write(why, strlen(why));
    __libc_console_write("\n", 1u);
}
