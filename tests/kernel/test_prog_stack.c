/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_prog_stack.c — the initial stack a program finds, read back byte for
 * byte.
 *
 * This is the part of a spawn most likely to be subtly wrong and least likely
 * to say so.  Every pointer in it is an address in the CHILD's address space
 * while the memory is mapped in the SPAWNER's, so a mistake does not fail: the
 * program reads somebody else's memory, or faults somewhere unrelated to the
 * error.  On the host both address spaces are this one, so the difference
 * between them can be checked — which is the whole reason the builder takes
 * both and lives in a file of its own.
 */
#include "framework.h"
#include "../../services/common/prog_stack.h"

void test_prog_stack(void);

/* A child base that is nowhere near the host mapping, so a pointer that was
 * written with the wrong one is off by a number nothing could produce by
 * accident. */
#define CHILD_BASE 0x0000008077000000ULL

static uint8_t g_stack[4096];

void test_prog_stack(void) {
    TEST_SUITE("the initial stack of a program");

    static const char *const argv[] = { "/bin/hello", "one", "two" };
    static const char *const envp[] = { "PATH=/bin", "IRIS=1" };
    struct prog_auxv aux[3] = {
        { 6u,  4096u },              /* AT_PAGESZ */
        { 9u,  0x8000300000ULL },    /* AT_ENTRY  */
        { 0x49520001ULL, 2u },       /* AT_IRIS_OBJC */
    };

    for (uint32_t i = 0; i < sizeof(g_stack); i++) g_stack[i] = 0xEE;

    uint64_t rsp = prog_stack_build(g_stack, CHILD_BASE, sizeof(g_stack),
                                    argv, 3u, envp, 2u, aux, 3u);

    /* it built at all */
    ASSERT_TRUE(rsp != 0);

    /* the entry pointer is inside the region and 16-byte aligned */
    ASSERT_TRUE(rsp >= CHILD_BASE);
    ASSERT_TRUE(rsp < CHILD_BASE + sizeof(g_stack));
    ASSERT_EQ((long)(rsp & 15u), 0L);

    uint64_t off = rsp - CHILD_BASE;
    const uint64_t *w = (const uint64_t *)(g_stack + off);

    /* argc is what %rsp points at */
    ASSERT_EQ((long)w[0], 3L);

    /* argv, then its NULL terminator */
    ASSERT_TRUE(w[1] != 0 && w[2] != 0 && w[3] != 0);
    ASSERT_EQ((long)w[4], 0L);

    /* envp, then its NULL terminator */
    ASSERT_TRUE(w[5] != 0 && w[6] != 0);
    ASSERT_EQ((long)w[7], 0L);

    /* the auxiliary vector, in order, terminated by AT_NULL */
    ASSERT_EQ((long)w[8],  6L);
    ASSERT_EQ((long)w[9],  4096L);
    ASSERT_EQ((long)w[10], 9L);
    ASSERT_EQ((long)w[11], (long)0x8000300000ULL);
    ASSERT_EQ((long)w[12], (long)0x49520001ULL);
    ASSERT_EQ((long)w[13], 2L);
    ASSERT_EQ((long)w[14], 0L);
    ASSERT_EQ((long)w[15], 0L);

    /*
     * EVERY pointer is a CHILD address.
     *
     * This is the assertion the file exists for.  A builder that wrote its own
     * mapping's addresses would pass every check above — the strings would be
     * there, the vectors would be shaped right — and hand the program five
     * pointers into the spawner.
     */
    for (uint32_t i = 1; i <= 3; i++) {
        ASSERT_TRUE(w[i] >= CHILD_BASE);
        ASSERT_TRUE(w[i] < CHILD_BASE + sizeof(g_stack));
    }
    for (uint32_t i = 5; i <= 6; i++) {
        ASSERT_TRUE(w[i] >= CHILD_BASE);
        ASSERT_TRUE(w[i] < CHILD_BASE + sizeof(g_stack));
    }

    /* and the strings really are there, where the pointers say */
    {
        const char *a0 = (const char *)(g_stack + (w[1] - CHILD_BASE));
        ASSERT_EQ((int)a0[0], (int)'/');
        ASSERT_EQ((int)a0[5], (int)'h');
        const char *a2 = (const char *)(g_stack + (w[3] - CHILD_BASE));
        ASSERT_EQ((int)a2[0], (int)'t');
        ASSERT_EQ((int)a2[3], 0);
        const char *e1 = (const char *)(g_stack + (w[6] - CHILD_BASE));
        ASSERT_EQ((int)e1[0], (int)'I');
        ASSERT_EQ((int)e1[6], 0);
    }

    /* the strings sit ABOVE the vectors and do not overlap them */
    ASSERT_TRUE(w[1] > rsp + 15u * 8u);

    /*
     * It refuses rather than overflowing.
     *
     * A stack too small for what it was asked to carry is the case a spawner
     * must be told about: the alternative is a program whose argv runs off the
     * end of its own stack into its guard page, at its first instruction.
     */
    ASSERT_EQ((long)prog_stack_build(g_stack, CHILD_BASE, 64u,
                                     argv, 3u, envp, 2u, aux, 3u), 0L);
    ASSERT_EQ((long)prog_stack_build(NULL, CHILD_BASE, sizeof(g_stack),
                                     argv, 3u, envp, 2u, aux, 3u), 0L);
    /* a count with no array is a caller bug, not an empty vector */
    ASSERT_EQ((long)prog_stack_build(g_stack, CHILD_BASE, sizeof(g_stack),
                                     NULL, 3u, envp, 2u, aux, 3u), 0L);

    /* the degenerate case a first program actually uses: no args, no env */
    {
        uint64_t r0 = prog_stack_build(g_stack, CHILD_BASE, sizeof(g_stack),
                                       NULL, 0u, NULL, 0u, aux, 1u);
        ASSERT_TRUE(r0 != 0);
        ASSERT_EQ((long)(r0 & 15u), 0L);
        const uint64_t *v = (const uint64_t *)(g_stack + (r0 - CHILD_BASE));
        ASSERT_EQ((long)v[0], 0L);   /* argc  */
        ASSERT_EQ((long)v[1], 0L);   /* argv NULL */
        ASSERT_EQ((long)v[2], 0L);   /* envp NULL */
        ASSERT_EQ((long)v[3], 6L);   /* AT_PAGESZ */
        ASSERT_EQ((long)v[5], 0L);   /* AT_NULL   */
    }
}
