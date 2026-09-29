/* SPDX-License-Identifier: Apache-2.0 */
/*
 * malloc.c — the standard names, over this program's own budget.
 *
 * The allocator itself is `heap_alloc.c`, which knows nothing about IRIS.
 * What is here is the one line that does: where more memory comes from.
 *
 * `__libc_sbrk` moves the break, which retypes frames from
 * `IRIS_CPTR_OWN_UNTYPED` and maps them — two invocations on objects this
 * program already holds, with no server in the path.  So running out of memory
 * is a NULL from `malloc`, on the calling thread, with nothing else on the
 * machine noticing: there was never a shared pool to exhaust.
 */
#include <stdlib.h>
#include <stddef.h>
#include "heap_alloc.h"
#include "libc_internal.h"
#include <errno.h>
#include "libc_lock.h"

static void *libc_grow(void *ctx, size_t bytes) {
    (void)ctx;
    if (bytes > 0x7FFFFFFFu) return 0;       /* `long` is what sbrk takes */
    return __libc_sbrk((long)bytes);
}

/*
 * The process heap.
 *
 * One, because a program has one budget.  It is not locked, and `libc_lock.h`
 * says what that costs and when it stops being true.
 */
static struct iris_heap g_heap = { 0, libc_grow, 0 };
static struct libc_lock g_heap_lock = LIBC_LOCK_INIT;

/*
 * A NULL from any of these means the same thing and says so: the program has
 * spent the budget it was given.  There is no other reason for one here —
 * there was never a shared pool to be evicted from, and nobody else's
 * allocation can affect this answer.
 */
static void *note(void *p) { if (!p) (void)__libc_fail(ENOMEM); return p; }

/*
 * The lock is taken HERE and not inside `heap_alloc.c`, so that the allocator
 * stays a function of its arguments and the host tests can drive it without
 * a scheduler.  Every entry to the free list goes through one of these four.
 */
void *malloc(size_t n) {
    void *p;
    __libc_lock(&g_heap_lock);
    p = iris_heap_malloc(&g_heap, n);
    __libc_unlock(&g_heap_lock);
    return note(p);
}

void free(void *p) {
    __libc_lock(&g_heap_lock);
    iris_heap_free(&g_heap, p);
    __libc_unlock(&g_heap_lock);
}

void *calloc(size_t a, size_t b) {
    void *p;
    __libc_lock(&g_heap_lock);
    p = iris_heap_calloc(&g_heap, a, b);
    __libc_unlock(&g_heap_lock);
    return note(p);
}

/* `realloc(p, 0)` frees and answers NULL by design, so it is not a failure and
 * must not set errno — the one case where a NULL here is an answer. */
void *realloc(void *p, size_t n) {
    void *q;
    __libc_lock(&g_heap_lock);
    q = iris_heap_realloc(&g_heap, p, n);
    __libc_unlock(&g_heap_lock);
    return (n == 0u) ? q : note(q);
}
