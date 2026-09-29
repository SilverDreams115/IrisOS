/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stdio_file.c — `FILE`, and the three a program does not open.
 *
 * ── Why this is thin ───────────────────────────────────────────────────────
 *
 * A `FILE` on a POSIX system is a descriptor, a buffer, a lock, a pushback
 * byte, an orientation and a state machine over all of them.  Here it is a
 * descriptor, a read buffer and two flags — because a descriptor on IRIS is
 * already a capability and everything `FILE` classically adds for SAFETY (who
 * may touch it, whether it is still valid) is a property of the capability
 * rather than of this structure.
 *
 * What is left for `FILE` to be is the thing it actually is in a C program:
 * somewhere to put the bytes so that reading a line does not cost one IPC per
 * character.
 *
 * ── stdout and stderr are the same place ───────────────────────────────────
 *
 * Both are the console endpoint the spawner minted into slot 3, because that
 * is how many capabilities a program is given.  A program that wants them
 * separated is asking for two capabilities, and that is a decision made at the
 * spawn, where it can be seen — not something this library can manufacture.
 *
 * `stdin` is the EOF descriptor: this system has no path from a keyboard to a
 * program that did not ask the keyboard service itself, so every read returns
 * end of file rather than an error, and a program that reads until zero
 * terminates normally.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "fmt.h"
#include "libc_internal.h"
#include "libc_lock.h"

struct _IRIS_FILE {
    int      fd;
    unsigned eof   : 1;
    unsigned err   : 1;
    unsigned owned : 1;   /* fclose closes the descriptor, and frees this */
    /* The read buffer.  Writes are not buffered here: `printf` already
     * buffers by line, and a second buffer would only add a way for the two
     * to come out in the wrong order. */
    unsigned char buf[256];
    size_t   have, at;
};

static FILE f_stdin  = { 0, 0, 0, 0, { 0 }, 0, 0 };
static FILE f_stdout = { 1, 0, 0, 0, { 0 }, 0, 0 };
static FILE f_stderr = { 2, 0, 0, 0, { 0 }, 0, 0 };

FILE *const stdin  = &f_stdin;
FILE *const stdout = &f_stdout;
FILE *const stderr = &f_stderr;

int fileno(FILE *f) { return f ? f->fd : (int)__libc_fail(EBADF); }
int feof(FILE *f)   { return f && f->eof; }
int ferror(FILE *f) { return f && f->err; }
void clearerr(FILE *f) { if (f) { f->eof = 0; f->err = 0; } }

/*
 * `fflush` tells the truth by doing nothing.
 *
 * Nothing is held back on the way out: `printf` writes a line when it ends and
 * when its buffer fills, and `fwrite` goes straight to the descriptor.  A
 * flush that had work to do would mean output could be lost at `exit`, and
 * there is no `atexit` here to lose it in.
 */
int fflush(FILE *f) { (void)f; return 0; }

/* ── writing ─────────────────────────────────────────────────────────────── */

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    size_t want;
    ssize_t got;
    if (!f || !p || sz == 0u || n == 0u) return 0;
    if (n > (size_t)-1 / sz) { f->err = 1; (void)__libc_fail(EINVAL); return 0; }
    want = sz * n;
    got = write(f->fd, p, want);
    if (got < 0) { f->err = 1; return 0; }
    /* The count is in ITEMS, so a short write reports whole items only —
     * a caller cannot act on half of one. */
    return (size_t)got / sz;
}

int fputs(const char *s, FILE *f) {
    size_t n;
    if (!s || !f) return (int)__libc_fail(EINVAL);
    n = strlen(s);
    if (n == 0u) return 0;
    return (fwrite(s, 1u, n, f) == n) ? 0 : -1;
}

int fputc(int c, FILE *f) {
    unsigned char b = (unsigned char)c;
    if (!f) return (int)__libc_fail(EINVAL);
    return (fwrite(&b, 1u, 1u, f) == 1u) ? (int)b : -1;
}

/*
 * A sink that empties into a FILE.
 *
 * It is the same engine `printf` uses, with a different place for the bytes to
 * go — which is the whole reason `fmt.c` takes a callback.
 */
struct file_sink { struct iris_fmt_out o; FILE *f; char line[512]; };

static void file_flush(struct iris_fmt_out *o) {
    struct file_sink *s = o->ctx;
    if (!o->n) return;
    o->buf[o->n] = '\0';
    (void)fwrite(o->buf, 1u, o->n, s->f);
    o->n = 0;
}

/* One lock for every formatted write, so that two threads' lines do not
 * interleave character by character.  `printf` has its own, and they are
 * different buffers; see libc_lock.h. */
static struct libc_lock g_file_lock = LIBC_LOCK_INIT;

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    struct file_sink s;
    int n;
    if (!f || !fmt) return (int)__libc_fail(EINVAL);
    s.f = f;
    s.o.buf = s.line; s.o.cap = sizeof s.line; s.o.n = 0; s.o.total = 0;
    s.o.flush = file_flush; s.o.ctx = &s;
    __libc_lock(&g_file_lock);
    n = iris_vformat(&s.o, fmt, ap);
    file_flush(&s.o);
    __libc_unlock(&g_file_lock);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

/* ── reading ─────────────────────────────────────────────────────────────── */

/* Refill from the descriptor.  Returns bytes available, or 0 at EOF/error. */
static size_t file_fill(FILE *f) {
    ssize_t got;
    if (f->at < f->have) return f->have - f->at;
    f->at = 0; f->have = 0;
    got = read(f->fd, f->buf, sizeof f->buf);
    if (got < 0) { f->err = 1; return 0; }
    if (got == 0) { f->eof = 1; return 0; }
    f->have = (size_t)got;
    return f->have;
}

int fgetc(FILE *f) {
    if (!f) return (int)__libc_fail(EINVAL);
    if (!file_fill(f)) return EOF;
    return (int)f->buf[f->at++];
}

size_t fread(void *p, size_t sz, size_t n, FILE *f) {
    unsigned char *d = p;
    size_t want, done = 0;
    if (!f || !p || sz == 0u || n == 0u) return 0;
    if (n > (size_t)-1 / sz) { f->err = 1; (void)__libc_fail(EINVAL); return 0; }
    want = sz * n;
    while (done < want) {
        size_t avail = file_fill(f);
        size_t take;
        if (!avail) break;
        take = want - done;
        if (take > avail) take = avail;
        memcpy(d + done, f->buf + f->at, take);
        f->at += take;
        done += take;
    }
    return done / sz;               /* whole items, as above */
}

/*
 * `fgets` stops at a newline, KEEPS it, and always terminates — the three
 * things about it that are easy to get wrong and that a caller depends on.
 * `n` counts the terminator, so at most n-1 bytes are read.
 */
char *fgets(char *dst, int n, FILE *f) {
    int i = 0;
    if (!dst || n <= 0 || !f) { (void)__libc_fail(EINVAL); return 0; }
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        dst[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;           /* nothing at all: EOF or error */
    dst[i] = '\0';
    return dst;
}

/* ── opening ─────────────────────────────────────────────────────────────── */

/*
 * `fopen`, read-only, because that is what this system has.
 *
 * A mode asking to write is refused with EROFS rather than accepted and then
 * failing at the first `fwrite` — what a file's bytes ARE is a memory question
 * for a real filesystem, and a mode string that lied about it would be the
 * same mistake `<fcntl.h>` refuses to make with its flags.
 */
FILE *fopen(const char *path, const char *mode) {
    FILE *f;
    int fd;
    if (!path || !mode) { (void)__libc_fail(EINVAL); return 0; }
    if (mode[0] != 'r' || strchr(mode, '+')) { (void)__libc_fail(EROFS); return 0; }
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;                  /* open already set errno */
    f = malloc(sizeof *f);
    if (!f) { (void)close(fd); return 0; } /* malloc already set ENOMEM */
    f->fd = fd; f->eof = 0; f->err = 0; f->owned = 1;
    f->have = 0; f->at = 0;
    return f;
}

int fclose(FILE *f) {
    int r = 0;
    if (!f) return (int)__libc_fail(EINVAL);
    if (!f->owned) return 0;               /* stdin/stdout/stderr are not ours */
    r = close(f->fd);
    free(f);
    return r;
}
