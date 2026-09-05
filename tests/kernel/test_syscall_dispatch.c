/*
 * test_syscall_dispatch.c — the syscall table itself.
 *
 * The dispatcher is the one place where a number becomes an operation, so two
 * properties of it are worth asserting directly rather than inferring from the
 * handlers:
 *
 *   - a RETIRED number stays retired.  IRIS never reuses a syscall number; a
 *     stale caller must get a refusal rather than somebody else's operation,
 *     and that promise is only as good as the table.
 *   - the match is on the FULL 64-bit value.  A switch on a truncated number
 *     would let 0x1_0000_0002 reach SYS_GETPID, which is a way to invoke a
 *     live handler with a number the ABI never assigned.
 *
 * The runtime suite's T148 fuzzes the same ground from ring 3 and is the
 * authority on which numbers exist.  What it cannot do is call the dispatcher
 * with a value the syscall instruction cannot carry, which is exactly where
 * the truncation bug would live.
 */
#include "framework.h"
#include <iris/task.h>
#include <iris/syscall.h>
#include <iris/nc/error.h>

void test_set_current_task(struct task *t);
uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);

static long ds(uint64_t num) {
    return (long)(int64_t)syscall_dispatch(num, 0, 0, 0, 0);
}

void test_syscall_dispatch(void) {
    TEST_SUITE("syscall dispatch table");

    /* No current task: every handler reached below refuses on its first line,
     * so nothing here can park in the restart loop. */
    test_set_current_task(NULL);

    /* ── DS-1: numbers that were never assigned ─────────────────────────*/
    {
        ASSERT_EQ(ds(9),   (long)IRIS_ERR_NOT_SUPPORTED);
        ASSERT_EQ(ds(10),  (long)IRIS_ERR_NOT_SUPPORTED);
        ASSERT_EQ(ds(11),  (long)IRIS_ERR_NOT_SUPPORTED);
        ASSERT_EQ(ds(400), (long)IRIS_ERR_NOT_SUPPORTED);
    }

    /* ── DS-2: the retired families stay retired ────────────────────────
     * One number from each retirement, because they were retired for different
     * reasons and a table edit that resurrected any of them would reopen a
     * closed convergence stage:
     *   15  SYS_HANDLE_INSERT      — the handle namespace (Stage 4)
     *   19  SYS_NOTIFY_CREATE      — fabricating creators (Phase S1)
     *   25  SYS_NS_LOOKUP          — the kernel name service (pre-capability)
     *   56  SYS_PROCESS_CREATE     — the process object (Stage 7-proc)
     *   58  SYS_THREAD_START       — pool-born threads (Stage 7)
     *  104  SYS_PROC_CSPACE_MINT   — minting through a process (Step 9)
     *  109  SYS_RESOURCE_INFO      — the per-process resource domain (7-mem)
     */
    {
        const uint64_t retired[] = { 15, 19, 25, 56, 58, 104, 109 };
        for (unsigned i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
            ASSERT_EQ(ds(retired[i]), (long)IRIS_ERR_NOT_SUPPORTED);
    }

    /* ── DS-3: the match is on the full 64-bit value ─────────────────────
     * SYS_GETPID (2) is the probe because a live dispatch returns a
     * non-negative id, so a truncating switch is visible as success.  If any
     * of these ever stops being NOT_SUPPORTED, a caller can reach a live
     * handler through a number the ABI never assigned. */
    {
        ASSERT_EQ(ds(0x100000002ULL), (long)IRIS_ERR_NOT_SUPPORTED);
        ASSERT_EQ(ds(0x200000002ULL), (long)IRIS_ERR_NOT_SUPPORTED);
        ASSERT_EQ(ds(~(uint64_t)0),   (long)IRIS_ERR_NOT_SUPPORTED);
        /* and the same for a live number in the high range */
        ASSERT_EQ(ds(0x100000000ULL | SYS_CSPACE_REVOKE),
                  (long)IRIS_ERR_NOT_SUPPORTED);
    }

    /* ── DS-4: the first unassigned number is where the ABI says it is ───
     * A guard against growing the syscall surface silently: adding a number
     * must break this and be re-stated, the same way T148 forces it from ring
     * 3.  If this fails, check that the addition was deliberate. */
    {
        ASSERT_EQ(ds(SYS_TCB_SET_IPC_BUFFER + 1u), (long)IRIS_ERR_NOT_SUPPORTED);
    }

    test_set_current_task(NULL);
}
