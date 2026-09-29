/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_LIBC_HEAP_ALLOC_H
#define IRIS_LIBC_HEAP_ALLOC_H

#include <stddef.h>

/*
 * heap_alloc.h — the allocator, with the system taken out of it.
 *
 * `malloc.c` used to be the allocator AND its connection to this machine, and
 * the connection is one line: where more memory comes from.  Here that line is
 * a callback, so everything else — the free list, the split, the coalesce, the
 * overflow checks — is a function of its arguments and can be driven from the
 * host, where a wrong pointer is a failed assertion instead of a page fault
 * inside a virtual machine that then reports something unrelated.
 *
 * On IRIS `grow` retypes frames out of the program's own budget and maps them.
 * In `tests/kernel/test_libc_malloc.c` it is `malloc` on the host.  The
 * allocator cannot tell, which is the point.
 */

struct iris_blk;

struct iris_heap {
    struct iris_blk *freelist;
    /*
     * More memory, CONTIGUOUS with nothing in particular.
     *
     * It returns a fresh region of at least `bytes`, or NULL when there is no
     * more — which on IRIS means the program has spent the budget it was
     * given, on its own thread, with nothing else on the machine affected.
     * Regions need not be adjacent: each becomes its own free block.
     */
    void *(*grow)(void *ctx, size_t bytes);
    void  *ctx;
};

/* The smallest region worth asking `grow` for.  Every growth costs a frame and
 * a CSpace slot, both finite, so the floor is here rather than at the caller. */
#define IRIS_HEAP_GROW_MIN (64u * 1024u)

void *iris_heap_malloc(struct iris_heap *h, size_t n);
void  iris_heap_free(struct iris_heap *h, void *p);
void *iris_heap_calloc(struct iris_heap *h, size_t a, size_t b);
void *iris_heap_realloc(struct iris_heap *h, void *p, size_t n);

/* The payload size a pointer was given.  Exposed for the tests, which cannot
 * otherwise check that a split left the block it claimed. */
size_t iris_heap_usable(const void *p);

#endif /* IRIS_LIBC_HEAP_ALLOC_H */
