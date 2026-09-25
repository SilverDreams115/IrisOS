/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_abi.c — the 1.0 surface, asserted rather than described.
 *
 * `iris/abi.h` says what IRIS offers ring 3: four syscall numbers, a
 * contiguous label space, and everything else refused.  A contract that only
 * exists as prose is a contract until the first person adds a label and
 * forgets the paragraph.  These assertions are what make it one, and each is
 * written so that the thing it would catch is the thing somebody would
 * actually do:
 *
 *   - add a label and not extend the declared range      (AB-2, AB-3)
 *   - leave a hole in the label space                     (AB-3)
 *   - let a retired number reach a method again           (AB-1)
 *   - grow BootInfo without saying which ABI it is        (AB-4)
 *
 * The runtime suite's T148 fuzzes the same ground from ring 3.  What it cannot
 * do is call the dispatcher with values the syscall instruction cannot carry,
 * or read a struct the kernel writes before any task exists.
 */
#include "framework.h"
#include <iris/task.h>
#include <iris/abi.h>
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/root_bootinfo.h>
#include <iris/nc/error.h>
#include <string.h>

void test_set_current_task(struct task *t);

static long ds(uint64_t num) {
    return (long)(int64_t)syscall_dispatch(num, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
static long inv(uint64_t label) {
    return (long)(int64_t)syscall_dispatch(SYS_INVOKE, 0, label,
                                           0, 0, 0, 0, 0, 0, 0);
}

void test_abi(void) {
    TEST_SUITE("ABI 1.0 surface");

    test_set_current_task(NULL);

    /* ── AB-1: the numbered door is exactly four wide ────────────────────
     *
     * Over every value the dispatcher can see, not over a list — a list would
     * have to be maintained alongside the thing it describes, which is the
     * failure this suite exists to prevent.  1024 is far past the largest
     * number IRIS ever exported (144), so it covers every retired number, the
     * gaps between them, and a margin of numbers that never existed. */
    {
        uint32_t live = 0;
        for (uint64_t n = 0; n < 1024u; n++) {
            int is_live = (n == SYS_INVOKE || n == SYS_EXIT ||
                           n == SYS_YIELD  || n == SYS_CLOCK_GET);
            if (is_live) { live++; continue; }
            if (ds(n) != (long)IRIS_ERR_NOT_SUPPORTED) {
                /* Name the survivor.  A bare failure here would say the
                 * surface is wrong without saying which number made it so. */
                ASSERT_EQ((long)n, (long)IRIS_ERR_NOT_SUPPORTED);
            }
        }
        ASSERT_EQ((int)live, (int)IRIS_ABI_SYSCALL_COUNT);
    }

    /* ── AB-2: the declared label range agrees with the label table ──────*/
    ASSERT_EQ((int)INV_LABEL_COUNT, (int)IRIS_ABI_LABEL_MAX + 1);

    /* ── AB-3: the label space is contiguous, and ends where it says ─────
     *
     * With no current task an ASSIGNED label reaches its method and is refused
     * on its first line with INVALID_ARG; an UNASSIGNED one never reaches a
     * method and falls to the dispatcher's default with NOT_SUPPORTED.  That
     * difference is the only way to tell "this label exists and I called it
     * wrongly" from "this label does not exist", and a frozen ABI must not let
     * the two be confused.
     *
     * Label 0 names nothing on purpose and answers like a number past the end.
     */
    ASSERT_EQ(inv(INV_INVALID), (long)IRIS_ERR_NOT_SUPPORTED);
    for (uint64_t l = 1; l <= IRIS_ABI_LABEL_MAX; l++) {
        if (inv(l) == (long)IRIS_ERR_NOT_SUPPORTED) {
            /* A hole: a label inside the declared range that reaches nothing. */
            ASSERT_EQ((long)l, 0L);
        }
    }
    /* ...and one past the end is refused, as is a value far beyond it. */
    ASSERT_EQ(inv(IRIS_ABI_LABEL_MAX + 1u), (long)IRIS_ERR_NOT_SUPPORTED);
    ASSERT_EQ(inv(0xFFFFFFFFull),           (long)IRIS_ERR_NOT_SUPPORTED);

    /* ── AB-4: BootInfo says which ABI the kernel implements ─────────────
     *
     * The only way a caller can detect the kernel it is on, so a zero here
     * would read as "version 0.0" rather than as "nobody said" — which is why
     * it is written by root_bootinfo_init and not by the boot path. */
    {
        static uint8_t buf[IRIS_ROOT_BOOTINFO_BYTES];
        memset(buf, 0xA5, sizeof(buf));
        ASSERT_EQ((int)root_bootinfo_init(buf, sizeof(buf), 1u, 2u, 3u, 256u),
                  (int)IRIS_OK);
        const struct iris_root_bootinfo *bi =
            (const struct iris_root_bootinfo *)buf;
        ASSERT_EQ((int)bi->version,   (int)IRIS_ROOT_BOOTINFO_VERSION);
        ASSERT_EQ((int)bi->abi_major, (int)IRIS_ABI_VERSION_MAJOR);
        ASSERT_EQ((int)bi->abi_minor, (int)IRIS_ABI_VERSION_MINOR);
        /* 1.0 is the frozen surface.  A major of 0 would mean this file is
         * describing something that was never frozen. */
        ASSERT_EQ((int)bi->abi_major, 1);
    }

    test_set_current_task(NULL);
}
