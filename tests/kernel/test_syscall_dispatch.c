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
#include <iris/invoke.h>
#include <iris/nc/error.h>

void test_set_current_task(struct task *t);
uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3, uint64_t a4);

static long ds(uint64_t num) {
    return (long)(int64_t)syscall_dispatch(num, 0, 0, 0, 0, 0);
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
     *   55  SYS_INITRD_VMO         — a boot image as a KVMO (Stage 6, D-5)
     */
    {
        /* Ledger A-22 adds two: 66 (SYS_EXCEPTION_RESUME) and 123
         * (SYS_TCB_FAULT_INFO).  A fault is answered by REPLYING to it and
         * read out of the message it arrived in, so both numbers are gone —
         * and gone means NOT_SUPPORTED for everyone, not repurposed. */
        /* Ledger A-24 adds three: 8 (SYS_SLEEP), 64 (SYS_NOTIFY_WAIT_TIMEOUT)
         * and 70 (SYS_CLOCK_NANOSLEEP).  A kernel that can block a thread on
         * time owns a policy about time; waiting is a service now. */
        /* Ledger A-27 adds two: 2 (SYS_GETPID) and 49 (SYS_THREAD_EXIT).  A
         * thread's own id answered from nothing, and an exit that recorded no
         * code where SYS_EXIT records one. */
        const uint64_t retired[] = { 2, 8, 15, 19, 25, 49, 55, 56, 58, 64, 66,
                                     70, 104, 109, 123 };
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
     * 3.  If this fails, check that the addition was deliberate.
     *
     * It fired once, for A-32: 144 became SYS_INVOKE, the door every other
     * number is being folded into.  That is the last number this table is
     * expected to gain — the conversion SHRINKS it. */
    {
        ASSERT_EQ(ds(SYS_INVOKE + 1u), (long)IRIS_ERR_NOT_SUPPORTED);
    }

    /* ── DS-6: the numbered table is CLOSED (ledger A-32) ────────────────
     * Every number from 0 to 400 answers NOT_SUPPORTED except four: the
     * invocation door, and the three calls that invoke nothing and therefore
     * could never be methods of anything.
     *
     * This is the assertion the whole conversion was for, and it is made here
     * rather than from ring 3 because only here can every number be tried —
     * including the ones a live handler would act on.  It subsumes DS-2: a
     * retired number is refused because NO number reaches a method any more.
     */
    {
        for (uint64_t n = 0; n <= 400u; n++) {
            if (n == SYS_INVOKE || n == SYS_EXIT || n == SYS_YIELD ||
                n == SYS_CLOCK_GET)
                continue;
            if (ds(n) != (long)IRIS_ERR_NOT_SUPPORTED) {
                /* Name the survivor rather than only failing: a number that
                 * still reaches a method is a family that was missed. */
                ASSERT_EQ((long)n, (long)IRIS_ERR_NOT_SUPPORTED);
            }
        }
        ASSERT_EQ(ds(87), (long)IRIS_ERR_NOT_SUPPORTED);  /* UNTYPED_RETYPE */
        ASSERT_EQ(ds(111), (long)IRIS_ERR_NOT_SUPPORTED); /* UNTYPED_RETYPE2 */
        ASSERT_EQ(ds(97), (long)IRIS_ERR_NOT_SUPPORTED);  /* TCB_SUSPEND */
    }

    /* ── DS-5: the invocation door is wired to the label table ───────────
     * A real label must REACH its method.  With no current task the method
     * refuses on its first line with INVALID_ARG — which is the point: an
     * unassigned number and an unassigned label both answer NOT_SUPPORTED, so
     * only a method that was actually entered tells them apart.
     *
     * Label 0 names nothing on purpose, and gets the same answer an
     * unassigned syscall number does. */
    {
        ASSERT_EQ((long)(int64_t)syscall_dispatch(SYS_INVOKE, 0, INV_CAP_IDENTIFY,
                                                  0, 0, 0),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ((long)(int64_t)syscall_dispatch(SYS_INVOKE, 0, INV_INVALID,
                                                  0, 0, 0),
                  (long)IRIS_ERR_NOT_SUPPORTED);
        ASSERT_EQ((long)(int64_t)syscall_dispatch(SYS_INVOKE, 0,
                                                  (uint64_t)INV_LABEL_COUNT, 0, 0, 0),
                  (long)IRIS_ERR_NOT_SUPPORTED);
    }

    test_set_current_task(NULL);
}
