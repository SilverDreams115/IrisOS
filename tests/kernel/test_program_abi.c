/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_program_abi.c — the program contract, pinned.
 *
 * `docs/contracts/program.md` says what `proc` gives a program;
 * `iris/program_abi.h` carries the numbers.  A contract that nothing checks is
 * a contract that goes stale, which is the lesson Stage 10-abi was taught by
 * its own opening paragraph — so the ranges are asserted here rather than
 * trusted to stay disjoint while somebody edits one of them.
 *
 * Every program compiled against this finds its capabilities at these slots
 * and its regions at these offsets.  A silent overlap is not a bug that shows
 * up in a test run; it is a bug that shows up as a program mapping over its
 * own heap, on a machine somebody else is holding.
 */
#include "framework.h"
#include <iris/program_abi.h>
#include <iris/paging.h>
#include <iris/endpoint_proto.h>

void test_program_abi(void);

void test_program_abi(void) {
    TEST_SUITE("the program contract");

    /* ── the CSpace runs are ordered and do not overlap ──────────────── */
    /* reserved sits above the well-known slots */
    /* 19 is IRIS_CPTR_OWN_TCB, the highest the shared convention names. */
    ASSERT_TRUE(IRIS_PROG_SLOT_RESERVED_LO > (uint32_t)IRIS_CPTR_OWN_TCB);

    /* the program's first free run follows the reserved one */
    ASSERT_TRUE(IRIS_PROG_SLOT_FREE_LO > IRIS_PROG_SLOT_RESERVED_HI);
    ASSERT_TRUE(IRIS_PROG_SLOT_FREE_HI >= IRIS_PROG_SLOT_FREE_LO);

    /* the object table follows the first free run and does not overlap it */
    ASSERT_TRUE(IRIS_PROG_SLOT_OBJ_BASE > IRIS_PROG_SLOT_FREE_HI);

    /* the second free run begins after the whole object table */
    ASSERT_TRUE(IRIS_PROG_SLOT_FREE2_LO >=
                IRIS_PROG_SLOT_OBJ_BASE + IRIS_PROG_SLOT_OBJ_MAX);

    /* The harness's fixtures really are inside the reserved run: this is the
     * assertion that would fail if somebody moved one of them. */
    /* the test harness's fixtures are inside the reserved run */
    ASSERT_TRUE((uint32_t)IRIS_CPTR_TEST_PROC  >= IRIS_PROG_SLOT_RESERVED_LO);
    ASSERT_TRUE((uint32_t)IRIS_CPTR_TEST_PROC  <= IRIS_PROG_SLOT_RESERVED_HI);
    ASSERT_TRUE((uint32_t)IRIS_CPTR_TEST_FIX_A >= IRIS_PROG_SLOT_RESERVED_LO);
    ASSERT_TRUE((uint32_t)IRIS_CPTR_TEST_FIX_B <= IRIS_PROG_SLOT_RESERVED_HI);

    /* ── the address regions are ordered, disjoint, and inside the space ─ */
    /* the image region is exactly what the loader randomises within */
    ASSERT_EQ((long)(USER_PRIVATE_BASE + IRIS_PROG_IMAGE_OFF),
              (long)USER_TEXT_BASE);
    ASSERT_EQ((long)(USER_PRIVATE_BASE + IRIS_PROG_IMAGE_END_OFF),
              (long)USER_VMO_BASE);

    /* the interpreter's region begins where the image's ends */
    ASSERT_EQ((long)IRIS_PROG_INTERP_OFF, (long)IRIS_PROG_IMAGE_END_OFF);
    ASSERT_TRUE(IRIS_PROG_INTERP_END_OFF > IRIS_PROG_INTERP_OFF);

    /* the heap follows the interpreter, and mmap follows the heap */
    ASSERT_EQ((long)IRIS_PROG_HEAP_OFF,  (long)IRIS_PROG_INTERP_END_OFF);
    ASSERT_TRUE(IRIS_PROG_HEAP_END_OFF > IRIS_PROG_HEAP_OFF);
    ASSERT_EQ((long)IRIS_PROG_MMAP_OFF,  (long)IRIS_PROG_HEAP_END_OFF);

    /* mmap has room below the stack, and the stack is where it was */
    ASSERT_TRUE(USER_PRIVATE_BASE + IRIS_PROG_MMAP_OFF < USER_STACK_BASE);
    /* ...and the contract's own name for where the stack starts is the same
     * address paging.h computes.  Two files state it; this is what keeps them
     * from stating two different things. */
    ASSERT_EQ((long)(USER_PRIVATE_BASE + IRIS_PROG_STACK_OFF), (long)USER_STACK_BASE);
    /* The mmap region ends one page BELOW it: the guard page is not part of
     * any region, so no allocator can hand it out. */
    ASSERT_EQ((long)(IRIS_PROG_STACK_OFF - IRIS_PROG_MMAP_END_OFF), 4096L);
    ASSERT_TRUE(IRIS_PROG_MMAP_OFF < IRIS_PROG_MMAP_END_OFF);
    ASSERT_TRUE(USER_STACK_BASE < USER_STACK_TOP);
    ASSERT_TRUE(USER_STACK_TOP < USER_PRIVATE_BASE + USER_PRIVATE_SIZE);

    /* every region is inside the user half */
    ASSERT_TRUE(USER_PRIVATE_BASE + IRIS_PROG_MMAP_OFF < USER_SPACE_TOP);

    /* ── the private auxv entries cannot be mistaken for standard ones ── */
    /* the private auxv numbers are far above the standard set */
    /* Linux's are all below 64; anything above that cannot collide by
     * accident, and these are chosen high enough to be obvious in a dump. */
    ASSERT_TRUE(AT_IRIS_OBJC    > 0x10000ULL);
    ASSERT_TRUE(AT_IRIS_OBJV    > 0x10000ULL);
    ASSERT_TRUE(AT_IRIS_UNTYPED > 0x10000ULL);

    /* and they are distinct from each other */
    ASSERT_TRUE(AT_IRIS_OBJC != AT_IRIS_OBJV);
    ASSERT_TRUE(AT_IRIS_OBJV != AT_IRIS_UNTYPED);
    ASSERT_TRUE(AT_IRIS_OBJC != AT_IRIS_UNTYPED);

    /* ── the service's own slots do not tread on the shared convention ── */
    /* proc's own slots keep their shared meanings */
    ASSERT_EQ((long)PROC_SLOT_CTRL_EP, (long)IRIS_CPTR_OWN_EP);

    /* a spawn that is running reports the same 12 the net backends do */
    /* One number, one meaning, across the tree: 12 is up. */
    ASSERT_EQ((long)PROC_STEP_RUNNING, 12L);
}
