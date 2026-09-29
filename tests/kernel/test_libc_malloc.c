/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_libc_malloc.c — the allocator, driven from the host.
 *
 * An allocator is the piece whose bugs are furthest from their symptom.  A
 * split that hands out four bytes it does not own, a coalesce that merges a
 * live block, an overflow in the round-up: none of them fail where they
 * happen.  They fail later, in whatever wrote through the pointer, and on IRIS
 * that is a page fault in a virtual machine reporting an address that names
 * nothing.
 *
 * `heap_alloc.c` takes its memory through a callback, so here it takes it from
 * the host's `malloc` and every one of those mistakes is an assertion instead.
 */
#include "framework.h"
#include "../../services/libc/heap_alloc.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void test_libc_malloc(void);

/* What the heap grows from, with a ceiling so exhaustion can be tested. */
struct grow_ctx {
    size_t budget;      /* bytes left to hand out */
    unsigned calls;
    unsigned char *arena;
    size_t used, cap;
};

static void *grow_from_arena(void *vctx, size_t bytes) {
    struct grow_ctx *c = vctx;
    unsigned char *p;
    c->calls++;
    if (bytes > c->budget) return 0;
    if (c->used + bytes > c->cap) return 0;
    p = c->arena + c->used;
    c->used += bytes;
    c->budget -= bytes;
    return p;
}

void test_libc_malloc(void) {
    printf("test_libc_malloc: the allocator, on a budget it can exhaust\n");

    {
        struct grow_ctx c;
        struct iris_heap h;
        void *a, *b, *d;

        c.cap = 1u << 20;
        c.arena = aligned_alloc(64u, c.cap);
        c.used = 0; c.calls = 0; c.budget = c.cap;
        h.freelist = 0; h.grow = grow_from_arena; h.ctx = &c;
        ASSERT_NOT_NULL(c.arena);

        /* ── the first allocation grows once, and only once ── */
        a = iris_heap_malloc(&h, 32);
        ASSERT_NOT_NULL(a);
        ASSERT_EQ(c.calls, 1u);
        /* A second one fits in what the first growth left over. */
        b = iris_heap_malloc(&h, 32);
        ASSERT_NOT_NULL(b);
        ASSERT_EQ(c.calls, 1u);
        ASSERT_NE(a, b);

        /* ── alignment: a payload must suit any object the program stores ── */
        ASSERT_EQ((unsigned long)((uintptr_t)a & 15u), 0ul);
        ASSERT_EQ((unsigned long)((uintptr_t)b & 15u), 0ul);

        /* ── the blocks do not overlap ── */
        ASSERT_TRUE((unsigned char *)a + iris_heap_usable(a) <= (unsigned char *)b ||
                    (unsigned char *)b + iris_heap_usable(b) <= (unsigned char *)a);

        /* ── a request is rounded UP, never down ── */
        ASSERT_TRUE(iris_heap_usable(a) >= 32u);
        d = iris_heap_malloc(&h, 1);
        ASSERT_TRUE(iris_heap_usable(d) >= 1u);

        /* ── writing the whole payload touches nothing else ── */
        memset(a, 0xAA, iris_heap_usable(a));
        memset(b, 0xBB, iris_heap_usable(b));
        for (size_t i = 0; i < iris_heap_usable(a); i++)
            if (((unsigned char *)a)[i] != 0xAA) { g_fail++; break; }
        g_pass++;

        /* ── free then malloc the same size reuses the block ── */
        iris_heap_free(&h, b);
        {
            void *again = iris_heap_malloc(&h, 32);
            ASSERT_EQ(again, b);
            ASSERT_EQ(c.calls, 1u);
            iris_heap_free(&h, again);
        }

        /* ── a double free is ignored, not fatal, and does not corrupt ── */
        iris_heap_free(&h, a);
        iris_heap_free(&h, a);
        {
            void *x = iris_heap_malloc(&h, 16);
            void *y = iris_heap_malloc(&h, 16);
            ASSERT_NOT_NULL(x);
            ASSERT_NOT_NULL(y);
            ASSERT_NE(x, y);          /* the list did not hand one block twice */
        }

        /* ── calloc zeroes, and refuses a product that would wrap ── */
        {
            unsigned char *z = iris_heap_calloc(&h, 16, 4);
            int clean = 1;
            ASSERT_NOT_NULL(z);
            for (int i = 0; i < 64; i++) if (z[i]) clean = 0;
            ASSERT_TRUE(clean);
            ASSERT_NULL(iris_heap_calloc(&h, (size_t)-1 / 2u, 4u));
        }

        /* ── realloc keeps the bytes, and its NULL/zero cases are malloc/free ── */
        {
            char *p = iris_heap_malloc(&h, 8);
            char *q;
            ASSERT_NOT_NULL(p);
            memcpy(p, "1234567", 8);
            q = iris_heap_realloc(&h, p, 64);
            ASSERT_NOT_NULL(q);
            ASSERT_TRUE(memcmp(q, "1234567", 8) == 0);
            /* Shrinking in place is allowed to return the same pointer. */
            ASSERT_EQ(iris_heap_realloc(&h, q, 4), q);
            ASSERT_NULL(iris_heap_realloc(&h, q, 0));
            ASSERT_NOT_NULL(iris_heap_realloc(&h, 0, 16));
        }

        /* ── a zero-byte request is still a distinct, writable allocation ── */
        {
            char *z0 = iris_heap_malloc(&h, 0);
            char *z1 = iris_heap_malloc(&h, 0);
            ASSERT_NOT_NULL(z0);
            ASSERT_NE(z0, z1);
            z0[0] = 1;                 /* at least one byte, as C requires */
        }

        free(c.arena);
    }

    /* ── exhaustion is a NULL, on this thread, and nothing else ── */
    {
        struct grow_ctx c;
        struct iris_heap h;
        void *p;
        c.cap = 128u * 1024u;
        c.arena = aligned_alloc(64u, c.cap);
        c.used = 0; c.calls = 0; c.budget = 96u * 1024u;
        h.freelist = 0; h.grow = grow_from_arena; h.ctx = &c;

        ASSERT_NOT_NULL(iris_heap_malloc(&h, 1024));      /* grows 64 KiB */
        p = iris_heap_malloc(&h, 200u * 1024u);           /* over budget  */
        ASSERT_NULL(p);
        /* ...and the heap still works afterwards: a refusal is not a wound. */
        ASSERT_NOT_NULL(iris_heap_malloc(&h, 64));
        free(c.arena);
    }

    /* ── a heap with no way to grow refuses instead of faulting ── */
    {
        struct iris_heap h = { 0, 0, 0 };
        ASSERT_NULL(iris_heap_malloc(&h, 16));
        iris_heap_free(&h, 0);          /* free(NULL) is a no-op */
        ASSERT_NULL(iris_heap_realloc(&h, 0, 16));
    }

    /* ── the round-up must not wrap a size back to something small ── */
    {
        struct grow_ctx c;
        struct iris_heap h;
        c.cap = 4096; c.arena = aligned_alloc(64u, c.cap);
        c.used = 0; c.calls = 0; c.budget = c.cap;
        h.freelist = 0; h.grow = grow_from_arena; h.ctx = &c;
        ASSERT_NULL(iris_heap_malloc(&h, (size_t)-1));
        ASSERT_NULL(iris_heap_malloc(&h, (size_t)-8));
        ASSERT_EQ(c.calls, 0u);         /* refused before asking for memory */
        free(c.arena);
    }
}
