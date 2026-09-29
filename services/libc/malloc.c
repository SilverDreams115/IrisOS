/* SPDX-License-Identifier: Apache-2.0 */
/*
 * malloc.c — a heap, out of the program's own budget and nothing else.
 *
 * `__libc_sbrk` moves the break, which retypes frames from
 * `IRIS_CPTR_OWN_UNTYPED` and maps them — two invocations on objects this
 * program already holds, with no server in the path.  So running out of memory
 * is a NULL from `malloc`, on the calling thread, with nothing else on the
 * machine noticing: there was never a shared pool to exhaust.
 *
 * ── What kind of allocator ─────────────────────────────────────────────────
 *
 * First fit over a singly-linked free list, with splitting and forward
 * coalescing.  It is not a good allocator and does not pretend to be; it is a
 * correct one, small enough to read in one sitting, and its failure mode is
 * fragmentation rather than corruption.  A program whose allocation pattern
 * this hurts is the reason to write a better one, and there is no such program
 * yet.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "libc_internal.h"

struct blk {
    size_t      size;     /* payload bytes, not counting this header */
    struct blk *next;     /* free list only                          */
    uint64_t    free;
};

#define ALIGN_UP(n) (((n) + 15u) & ~(size_t)15u)
#define HDR ALIGN_UP(sizeof(struct blk))
#define GROW_MIN (64u * 1024u)

static struct blk *g_free;

static struct blk *blk_of(void *p) {
    return (struct blk *)(void *)((uint8_t *)p - HDR);
}
static void *pay_of(struct blk *b) {
    return (void *)((uint8_t *)b + HDR);
}

void *malloc(size_t n) {
    struct blk *b, *prev = 0;
    size_t want;

    if (n == 0u) n = 1u;
    want = ALIGN_UP(n);
    if (want < n) return 0;                    /* the round-up wrapped */

    for (b = g_free; b; prev = b, b = b->next) {
        if (!b->free || b->size < want) continue;
        /* Split when what is left could hold a block of its own; otherwise the
         * remainder is given away with the allocation, because a free block
         * too small to ever satisfy anything is a leak with a header on it. */
        if (b->size >= want + HDR + 16u) {
            struct blk *rest = (struct blk *)(void *)((uint8_t *)b + HDR + want);
            rest->size = b->size - want - HDR;
            rest->free = 1u;
            rest->next = b->next;
            b->size = want;
            b->next = rest;
        }
        b->free = 0u;
        if (prev) prev->next = b->next; else g_free = b->next;
        b->next = 0;
        return pay_of(b);
    }

    /* Nothing fit: take more from the budget.  In chunks, because every
     * growth costs a frame and a CSpace slot, both finite. */
    {
        size_t grow = HDR + want;
        void *mem;
        if (grow < GROW_MIN) grow = GROW_MIN;
        mem = __libc_sbrk((long)grow);
        if (!mem) return 0;
        b = (struct blk *)mem;
        b->size = grow - HDR;
        b->free = 1u;
        b->next = g_free;
        g_free  = b;
        return malloc(n);               /* now it fits, by construction */
    }
}

void free(void *p) {
    struct blk *b;
    if (!p) return;
    b = blk_of(p);
    if (b->free) return;                /* a double free is ignored, not fatal */
    b->free = 1u;
    b->next = g_free;
    g_free  = b;
    /* Forward coalescing only, and only with the block physically next to this
     * one when it happens to be at the head of the list.  A real allocator
     * keeps the list in address order and merges both ways; this one is honest
     * about being simpler. */
    {
        struct blk *nxt = (struct blk *)(void *)((uint8_t *)b + HDR + b->size);
        struct blk **pp = &g_free;
        while (*pp) {
            if (*pp == nxt && nxt->free) {
                *pp = nxt->next;
                b->size += HDR + nxt->size;
                break;
            }
            pp = &(*pp)->next;
        }
    }
}

void *calloc(size_t a, size_t b) {
    size_t n;
    void *p;
    if (a && b > (size_t)-1 / a) return 0;     /* the multiply would wrap */
    n = a * b;
    p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n) {
    struct blk *b;
    void *q;
    if (!p) return malloc(n);
    if (n == 0u) { free(p); return 0; }
    b = blk_of(p);
    if (b->size >= n) return p;
    q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, b->size);
    free(p);
    return q;
}
