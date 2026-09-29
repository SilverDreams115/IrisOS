/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_prog_mem.c — where a program's next mapping goes.
 *
 * The arithmetic behind `brk`, `mmap` and `mprotect`, asserted on the host.
 * It is separated from the invocations for exactly this: a mis-ordered cursor
 * hands out an address inside the stack guard, an off-by-one in the slot pool
 * hands out a slot another mapping is already using, and a `find` that accepts
 * a partial overlap lets `mprotect` change the protection of a page its caller
 * did not name.  None of those fault where the mistake is, and all three are
 * one comparison each.
 */
#include "framework.h"
#include "../../services/common/prog_mem.h"
#include <iris/program_abi.h>
#include <iris/paging.h>

void test_prog_mem(void);

static struct prog_mem g_m;

static void setup(void) {
    prog_mem_setup(&g_m, USER_PRIVATE_BASE, /*untyped=*/12u, /*vspace=*/18u,
                   /*slot_lo=*/128u, /*slot_hi=*/255u, /*pt_slot=*/32u);
}

void test_prog_mem(void) {
    TEST_SUITE("a program's memory plan");

    /* The regions are the contract's, and they are where the contract says. */
    setup();
    ASSERT_EQ((long)g_m.brk_base, (long)(USER_PRIVATE_BASE + IRIS_PROG_HEAP_OFF));
    ASSERT_EQ((long)g_m.brk_cur,  (long)g_m.brk_base);
    ASSERT_EQ((long)g_m.heap_top, (long)g_m.brk_base);
    ASSERT_EQ((long)g_m.brk_end,  (long)(USER_PRIVATE_BASE + IRIS_PROG_HEAP_END_OFF));
    ASSERT_EQ((long)g_m.mmap_cur, (long)(USER_PRIVATE_BASE + IRIS_PROG_MMAP_OFF));
    /* ...and the mmap ceiling stops one page short of the stack, so the guard
     * page is not something any allocator can hand out. */
    ASSERT_EQ((long)(USER_STACK_BASE - g_m.mmap_end), 4096L);

    /* Rounding saturates instead of wrapping.  A size within 4095 of 2^64 is
     * refused; a wrap would return a tiny number and allocate nothing while
     * reporting success. */
    ASSERT_EQ((long)prog_mem_pages(1u), 4096L);
    ASSERT_EQ((long)prog_mem_pages(4096u), 4096L);
    ASSERT_EQ((long)prog_mem_pages(4097u), 8192L);
    ASSERT_EQ((long)prog_mem_pages(0u), 0L);
    ASSERT_EQ((long)prog_mem_pages((uint64_t)-1), 0L);

    /* ── the cursors hand out disjoint, ordered addresses ── */
    {
        uint64_t a = 0, b = 0, c = 0;
        setup();
        ASSERT_TRUE(prog_mem_reserve(&g_m, 1u, /*heap=*/0, &a));
        ASSERT_TRUE(prog_mem_reserve(&g_m, 4096u, /*heap=*/0, &b));
        ASSERT_TRUE(prog_mem_reserve(&g_m, 4097u, /*heap=*/0, &c));
        ASSERT_EQ((long)a, (long)(USER_PRIVATE_BASE + IRIS_PROG_MMAP_OFF));
        ASSERT_EQ((long)(b - a), 4096L);
        ASSERT_EQ((long)(c - b), 4096L);
        ASSERT_EQ((long)(g_m.mmap_cur - c), 8192L);
        /* The heap cursor did not move: they are two regions, not one. */
        ASSERT_EQ((long)g_m.heap_top, (long)g_m.brk_base);
    }

    /* A reservation bigger than what is left is refused, and refusing leaves
     * the cursor where it was — a half-advanced cursor would strand the
     * addresses it skipped. */
    {
        uint64_t va = 0;
        setup();
        ASSERT_TRUE(!prog_mem_reserve(&g_m, IRIS_PROG_HEAP_END_OFF, 1, &va));
        ASSERT_EQ((long)g_m.heap_top, (long)g_m.brk_base);
        /* ...and a size that would WRAP the test is refused too. */
        ASSERT_TRUE(!prog_mem_reserve(&g_m, (uint64_t)-4096, 0, &va));
        ASSERT_EQ((long)g_m.mmap_cur, (long)(USER_PRIVATE_BASE + IRIS_PROG_MMAP_OFF));
    }

    /* ── the slot pool is finite and says so ── */
    {
        uint32_t s = 0, first = 0, last = 0;
        uint32_t n = 0;
        setup();
        ASSERT_TRUE(prog_mem_take_slot(&g_m, &first));
        ASSERT_EQ((long)first, 128L);
        while (prog_mem_take_slot(&g_m, &s)) { last = s; n++; }
        /* 128..255 inclusive: one taken above, 127 in the loop. */
        ASSERT_EQ((long)n, 127L);
        ASSERT_EQ((long)last, 255L);
        /* And it stays refused rather than wrapping to slot 0 — which is the
         * root CNode itself. */
        ASSERT_TRUE(!prog_mem_take_slot(&g_m, &s));
    }

    /* ── the heap grows geometrically, so the BUDGET runs out first ── */
    {
        uint64_t c0, c1, c2;
        setup();
        /* The first growth is the floor, however little was asked for. */
        c0 = prog_mem_chunk(&g_m, 8u);
        ASSERT_EQ((long)c0, 65536L);
        /* ...and a request bigger than the floor gets what it asked for. */
        ASSERT_EQ((long)prog_mem_chunk(&g_m, 200000u), 200704L);  /* 49 pages */
        /* Once the heap has size, the next chunk is at least that size again:
         * 32 records therefore cover a heap no budget in this system could
         * ever pay for, which is the point. */
        g_m.heap_top = g_m.brk_base + 65536u;
        c1 = prog_mem_chunk(&g_m, 8u);
        ASSERT_EQ((long)c1, 65536L);
        g_m.heap_top = g_m.brk_base + (1u << 20);
        c2 = prog_mem_chunk(&g_m, 8u);
        ASSERT_EQ((long)c2, (long)(1u << 20));
        /* Near the ceiling it is trimmed to what fits, not refused... */
        g_m.heap_top = g_m.brk_end - 8192u;
        ASSERT_EQ((long)prog_mem_chunk(&g_m, 4096u), 8192L);
        /* ...and refused when even the need does not fit. */
        g_m.heap_top = g_m.brk_end - 4096u;
        ASSERT_EQ((long)prog_mem_chunk(&g_m, 8192u), 0L);
    }

    /* ── find covers, and only covers ── */
    {
        int i;
        setup();
        ASSERT_TRUE(prog_mem_record(&g_m, 0x1000u, 0x2000u, 130u, PROG_PROT_RW, 0));
        ASSERT_TRUE(prog_mem_record(&g_m, 0x8000u, 0x1000u, 131u, PROG_PROT_R, 0));
        ASSERT_EQ((long)g_m.nmaps, 2L);

        i = prog_mem_find(&g_m, 0x1000u, 0x2000u);   /* exactly the first    */
        ASSERT_EQ((long)i, 0L);
        i = prog_mem_find(&g_m, 0x1800u, 0x800u);    /* inside the first     */
        ASSERT_EQ((long)i, 0L);
        i = prog_mem_find(&g_m, 0x8000u, 0x1000u);   /* exactly the second   */
        ASSERT_EQ((long)i, 1L);
        /* A range that starts inside and runs past the end belongs to no
         * single mapping, and saying "the first one" would let mprotect
         * change a page its caller never named. */
        ASSERT_EQ((long)prog_mem_find(&g_m, 0x2000u, 0x2000u), -1L);
        /* The gap between them is nobody's. */
        ASSERT_EQ((long)prog_mem_find(&g_m, 0x4000u, 0x1000u), -1L);
        /* Zero bytes names nothing. */
        ASSERT_EQ((long)prog_mem_find(&g_m, 0x1000u, 0u), -1L);
        /* A range that would wrap is refused rather than computed. */
        ASSERT_EQ((long)prog_mem_find(&g_m, 0x1000u, (uint64_t)-1), -1L);
    }

    /* ── forgetting keeps every other record findable ── */
    {
        setup();
        ASSERT_TRUE(prog_mem_record(&g_m, 0x1000u, 0x1000u, 130u, PROG_PROT_RW, 0));
        ASSERT_TRUE(prog_mem_record(&g_m, 0x2000u, 0x1000u, 131u, PROG_PROT_RW, 0));
        ASSERT_TRUE(prog_mem_record(&g_m, 0x3000u, 0x1000u, 132u, PROG_PROT_RW, 1));
        prog_mem_forget(&g_m, 0);                 /* the last moves into 0 */
        ASSERT_EQ((long)g_m.nmaps, 2L);
        ASSERT_EQ((long)prog_mem_find(&g_m, 0x1000u, 0x1000u), -1L);
        ASSERT_TRUE(prog_mem_find(&g_m, 0x2000u, 0x1000u) >= 0);
        ASSERT_TRUE(prog_mem_find(&g_m, 0x3000u, 0x1000u) >= 0);
        /* ...and the record that moved kept everything it carried, including
         * the slot, which is what the frame is deleted by. */
        {
            int i = prog_mem_find(&g_m, 0x3000u, 0x1000u);
            ASSERT_EQ((long)g_m.maps[i].slot, 132L);
            ASSERT_EQ((long)g_m.maps[i].heap, 1L);
        }
        /* An index nobody has is ignored rather than corrupting the count. */
        prog_mem_forget(&g_m, 7);
        ASSERT_EQ((long)g_m.nmaps, 2L);
        prog_mem_forget(&g_m, -1);
        ASSERT_EQ((long)g_m.nmaps, 2L);
    }

    /* ── the record table is finite and refuses rather than overwriting ── */
    {
        uint32_t i;
        setup();
        for (i = 0; i < PROG_MEM_MAPS; i++)
            ASSERT_TRUE(prog_mem_record(&g_m, 0x10000u + (uint64_t)i * 0x1000u,
                                        0x1000u, 130u + i, PROG_PROT_RW, 0));
        ASSERT_TRUE(!prog_mem_record(&g_m, 0x99000u, 0x1000u, 200u, PROG_PROT_RW, 0));
        ASSERT_EQ((long)g_m.nmaps, (long)PROG_MEM_MAPS);
        /* The one that was refused is not findable: a full table that silently
         * dropped the record would leak a frame nothing could ever unmap. */
        ASSERT_EQ((long)prog_mem_find(&g_m, 0x99000u, 0x1000u), -1L);
    }
}
