/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_LIBC_INTERNAL_H
#define IRIS_LIBC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * What this libc holds, and where it got it.
 *
 * Everything here arrived as a CAPABILITY minted into a fixed CSpace slot
 * before the program existed (docs/contracts/program.md §2), or as a number on
 * the initial stack.  There is no global namespace to look anything up in and
 * no environment variable that changes any of it: what a program can reach is
 * what its spawner decided, visibly, at launch.
 */
struct libc_state {
    uint64_t untyped;      /* AT_IRIS_UNTYPED: the budget, and the whole bound */
    uint64_t objc, objv;   /* the object table the spawner filled              */
    uint64_t prog_base;    /* where the program was loaded                     */
    uint64_t interp_base;  /* ...and where this libc was                       */
    int      console;      /* 1 once the console endpoint has answered         */
};

extern struct libc_state __libc;

/* One write to the console endpoint.  Short writes are silent: a program given
 * no console cannot print, and faulting over it would be worse. */
void __libc_console_write(const char *s, size_t n);

/* This thread's registered IPC buffer, or NULL.  A bulk payload must be
 * marshalled into it and nowhere else — the kernel refuses a send that names
 * any other address — so every caller that stages one asks here. */
uint8_t *__libc_ipc_buf(void);

/* Set up before the program's first instruction. */
void __libc_console_init(void);
void __libc_heap_init(void);
int  __libc_tls_init(void);

/*
 * Resolve and apply the program's relocations against the objects the spawner
 * gave it.  Returns 0, or negative.  `dynlink.c`.
 */
long __libc_link_program(uint64_t prog_base, const void *phdrs,
                         uint64_t phnum, uint64_t phent);

/* The heap, for malloc.  It is the program's own budget and nothing else. */
void *__libc_sbrk(long delta);

/*
 * ── The one place a refusal becomes a number ───────────────────────────────
 *
 * Every function in this library that can fail returns through one of these.
 * A library that sets `errno` at each call site sets it at MOST of them, and
 * the ones it misses report whatever failed last — which is worse than
 * reporting nothing, because it looks like an answer.
 *
 * Both return -1 as a `long`, so a caller writes `return (int)__libc_fail(E);`
 * and the cast is where the reader sees what the function's type is.
 */
long __libc_fail(int err);            /* set errno to this, return -1     */
long __libc_fail_iris(long iris_err); /* ...translating an IRIS_ERR_* first */
int  __libc_errno_from(long iris_err);

#endif /* IRIS_LIBC_INTERNAL_H */
