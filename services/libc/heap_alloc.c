/* SPDX-License-Identifier: Apache-2.0 */
/*
 * heap_alloc.c — a heap, over whatever `grow` hands it.
 *
 * ── What kind of allocator ─────────────────────────────────────────────────
 *
 * First fit over a singly-linked free list, with splitting and forward
 * coalescing.  It is not a good allocator and does not pretend to be; it is a
 * correct one, small enough to read in one sitting, and its failure mode is
 * fragmentation rather than corruption.  A program whose allocation pattern
 * this hurts is the reason to write a better one, and there is no such program
 * yet.
 *
 * What changed when it was carved out of `malloc.c` is not the algorithm: it
 * is that the algorithm is now reachable from a test.  See `heap_alloc.h`.
 */
#include "heap_alloc.h"
#include <stdint.h>

struct iris_blk {
    size_t           size;     /* payload bytes, not counting this header */
    struct iris_blk *next;     /* free list only                          */
    uint64_t         free;
};

#define ALIGN_UP(n) (((n) + 15u) & ~(size_t)15u)
#define HDR ALIGN_UP(sizeof(struct iris_blk))

static struct iris_blk *blk_of(const void *p) {
    return (struct iris_blk *)(void *)((uint8_t *)(uintptr_t)p - HDR);
}
static void *pay_of(struct iris_blk *b) {
    return (void *)((uint8_t *)b + HDR);
}

size_t iris_heap_usable(const void *p) {
    return p ? blk_of(p)->size : 0u;
}

/* First fit, unlinking and splitting.  Returns NULL when nothing fits. */
static void *heap_take(struct iris_heap *h, size_t want) {
    struct iris_blk *b, *prev = 0;

    for (b = h->freelist; b; prev = b, b = b->next) {
        if (!b->free || b->size < want) continue;
        /* Split when what is left could hold a block of its own; otherwise the
         * remainder is given away with the allocation, because a free block
         * too small to ever satisfy anything is a leak with a header on it. */
        if (b->size >= want + HDR + 16u) {
            struct iris_blk *rest = (struct iris_blk *)(void *)
                                        ((uint8_t *)b + HDR + want);
            rest->size = b->size - want - HDR;
            rest->free = 1u;
            rest->next = b->next;
            b->size = want;
            b->next = rest;
        }
        b->free = 0u;
        if (prev) prev->next = b->next; else h->freelist = b->next;
        b->next = 0;
        return pay_of(b);
    }
    return 0;
}

void *iris_heap_malloc(struct iris_heap *h, size_t n) {
    size_t want, grow;
    void *p, *mem;
    struct iris_blk *b;

    if (!h) return 0;
    if (n == 0u) n = 1u;
    want = ALIGN_UP(n);
    if (want < n) return 0;                    /* the round-up wrapped */

    p = heap_take(h, want);
    if (p) return p;

    /* Nothing fit: ask for more.  ONCE — the second attempt is against a block
     * that satisfies `want` by construction, so a loop here could only spin,
     * and the recursion this replaced could only have done the same. */
    if (!h->grow) return 0;
    grow = HDR + want;
    if (grow < HDR) return 0;                  /* the header pushed it over  */
    if (grow < IRIS_HEAP_GROW_MIN) grow = IRIS_HEAP_GROW_MIN;
    mem = h->grow(h->ctx, grow);
    if (!mem) return 0;

    b = (struct iris_blk *)mem;
    b->size = grow - HDR;
    b->free = 1u;
    b->next = h->freelist;
    h->freelist = b;
    return heap_take(h, want);
}

void iris_heap_free(struct iris_heap *h, void *p) {
    struct iris_blk *b;
    if (!h || !p) return;
    b = blk_of(p);
    if (b->free) return;                /* a double free is ignored, not fatal */
    b->free = 1u;
    b->next = h->freelist;
    h->freelist = b;
    /*
     * Forward coalescing only, and only with the block physically next to this
     * one when that block is already on the list.  A real allocator keeps the
     * list in address order and merges both ways; this one is honest about
     * being simpler.
     *
     * The neighbour is only DEREFERENCED once the list says it exists, which
     * is what keeps this from reading past the end of the last region: the
     * address is computed, then compared, and read only if it matched.
     */
    {
        struct iris_blk *nxt = (struct iris_blk *)(void *)
                                   ((uint8_t *)b + HDR + b->size);
        struct iris_blk **pp = &h->freelist;
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

void *iris_heap_calloc(struct iris_heap *h, size_t a, size_t b) {
    size_t n;
    void *p;
    if (a && b > (size_t)-1 / a) return 0;     /* the multiply would wrap */
    n = a * b;
    p = iris_heap_malloc(h, n);
    if (p) { uint8_t *d = p; for (size_t i = 0; i < n; i++) d[i] = 0u; }
    return p;
}

void *iris_heap_realloc(struct iris_heap *h, void *p, size_t n) {
    struct iris_blk *b;
    void *q;
    if (!p) return iris_heap_malloc(h, n);
    if (n == 0u) { iris_heap_free(h, p); return 0; }
    b = blk_of(p);
    if (b->size >= n) return p;
    q = iris_heap_malloc(h, n);
    if (!q) return 0;
    { const uint8_t *s = p; uint8_t *d = q;
      for (size_t i = 0; i < b->size; i++) d[i] = s[i]; }
    iris_heap_free(h, p);
    return q;
}
