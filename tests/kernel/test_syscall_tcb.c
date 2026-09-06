/*
 * test_syscall_tcb.c — the THREAD authority layer, under host unit test.
 *
 * Since Stage 7 a thread capability is what a supervisor names for everything
 * a process capability used to be named for: configuring what CSpace and
 * address space it runs in, arming its fault handlers, watching it die,
 * reading its exit code, killing it.  That makes this file's refusals the
 * whole of thread authority, and they had never been enumerated anywhere.
 *
 * Two of them carry more weight than they look:
 *
 * The fault and TIMEOUT handlers are separate registrations, and the tests
 * below check they are refused independently.  Folding them would give a pager
 * temporal authority over every thread it serves and give a scheduler the
 * power to resume a thread out of a page fault.
 *
 * The root CSpace GUARD (arg3, seL4's cspace_root_data) is validated before it
 * is stored.  A guard that silently means something other than what was asked
 * for does not corrupt one lookup — it changes what every CPtr in that
 * thread's CSpace means, for the life of the thread.
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/cspace.h>
#include <iris/nc/rights.h>
#include <iris/task.h>
#include <iris/syscall.h>
#include <iris/kpage.h>
#include <string.h>

void test_set_current_task(struct task *t);

uint64_t sys_tcb_configure(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_tcb_set_fault_handler(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_tcb_set_timeout_handler(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_tcb_watch(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_tcb_exit_code(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_tcb_set_ipc_buffer(uint64_t a0, uint64_t a1, uint64_t a2);

static long tb_err(uint64_t r) { return (long)(int64_t)r; }

static void tb_destroy(struct KObject *o) { (void)o; }
static const struct KObjectOps tb_ops = { .close = NULL, .destroy = tb_destroy };

/* A caller with a root CSpace, plus a TCB capability at `slot` naming a second
 * (inactive) task — which is what a supervisor holds for a child. */
static struct task *tb_caller(struct KCNode **out_root, uint32_t slot,
                              struct task **out_target) {
    struct task *t = (struct task *)kpage_alloc((uint32_t)sizeof(struct task));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    struct KCNode *root = kcnode_alloc(256);
    if (!root) return NULL;
    kobject_active_retain(&root->base);
    t->cspace_root = root;

    struct task *target = (struct task *)kpage_alloc((uint32_t)sizeof(struct task));
    if (!target) return NULL;
    memset(target, 0, sizeof(*target));
    kobject_init(&target->base, KOBJ_TCB, &tb_ops);
    if (kcnode_mint(root, slot, &target->base,
                    RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE) != IRIS_OK)
        return NULL;

    test_set_current_task(t);
    if (out_root)   *out_root   = root;
    if (out_target) *out_target = target;
    return t;
}

void test_syscall_tcb(void) {
    TEST_SUITE("thread authority (TCB syscalls)");

    const uint32_t TCB_SLOT = 6;

    /* ── TB-1: no caller, or a caller with no CSpace ─────────────────────*/
    {
        test_set_current_task(NULL);
        ASSERT_EQ(tb_err(sys_tcb_configure(TCB_SLOT, 1, 2, 0)),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(tb_err(sys_tcb_set_fault_handler(TCB_SLOT, 1, 1, 1)),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(tb_err(sys_tcb_set_timeout_handler(TCB_SLOT, 1, 1, 1)),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(tb_err(sys_tcb_watch(TCB_SLOT, 1, 1)),
                  (long)IRIS_ERR_INVALID_ARG);
    }

    /* ── TB-2: CONFIGURE takes CAPABILITIES, and a handle value is not one ─
     * The CSpace and VSpace arguments are CPtrs.  Stage 4 made a value in the
     * retired handle range a malformed argument rather than an address in
     * another table, and this is where a thread's most basic authority — what
     * namespace it resolves in — would be established from one. */
    {
        struct KCNode *root; struct task *target;
        struct task *t = tb_caller(&root, TCB_SLOT, &target);
        ASSERT_NOT_NULL(t);
        const uint64_t handle_shaped = (uint64_t)1u << 31;
        ASSERT_EQ(tb_err(sys_tcb_configure(TCB_SLOT, handle_shaped, 2, 0)),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(tb_err(sys_tcb_configure(TCB_SLOT, 1, handle_shaped, 0)),
                  (long)IRIS_ERR_INVALID_ARG);
        test_set_current_task(NULL);
    }

    /* ── TB-3: the ROOT GUARD is validated before it is stored ────────────
     * arg3 is seL4's cspace_root_data.  A guard wider than the CPtr space, or
     * a value that does not fit the width it declares, is refused — not
     * truncated.  A truncated root guard does not corrupt one lookup: it
     * changes what every CPtr in that thread's CSpace means, permanently. */
    {
        struct KCNode *root; struct task *target;
        struct task *t = tb_caller(&root, TCB_SLOT, &target);
        ASSERT_NOT_NULL(t);
        /* width beyond the 31-bit CPtr space */
        ASSERT_EQ(tb_err(sys_tcb_configure(TCB_SLOT, 1, 2,
                         ((uint64_t)(KCNODE_GUARD_BITS_MAX + 1u) << 32))),
                  (long)IRIS_ERR_INVALID_ARG);
        /* a 3-valued guard declared as 2 bits wide */
        ASSERT_EQ(tb_err(sys_tcb_configure(TCB_SLOT, 1, 2,
                         (((uint64_t)2 << 32) | 0x7u))),
                  (long)IRIS_ERR_INVALID_ARG);
        /* and the thread was NOT given a guard by a call that failed */
        ASSERT_EQ(target->cspace_root_guard_bits, 0u);
        test_set_current_task(NULL);
    }

    /* ── TB-4: arming a handler needs signal bits and a mailbox ───────────
     * Zero signal bits is a notification that can never say anything; a zero
     * destination is a fault with nowhere to deliver the faulting thread's
     * capability.  Either one produces a handler that looks armed and is a
     * deadlock — the kernel would suspend the thread and nobody would ever be
     * told. */
    {
        struct KCNode *root; struct task *target;
        struct task *t = tb_caller(&root, TCB_SLOT, &target);
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(tb_err(sys_tcb_set_fault_handler(TCB_SLOT, 1, 0, 1)),
                  (long)IRIS_ERR_INVALID_ARG);      /* no signal bits */
        ASSERT_EQ(tb_err(sys_tcb_set_fault_handler(TCB_SLOT, 1, 1, 0)),
                  (long)IRIS_ERR_INVALID_ARG);      /* no mailbox     */
        /* The timeout handler is a SEPARATE registration and refuses on its
         * own terms — it is not a flag on the fault one. */
        ASSERT_EQ(tb_err(sys_tcb_set_timeout_handler(TCB_SLOT, 1, 0, 1)),
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(tb_err(sys_tcb_set_timeout_handler(TCB_SLOT, 1, 1, 0)),
                  (long)IRIS_ERR_INVALID_ARG);
        test_set_current_task(NULL);
    }

    /* ── TB-5: the handler mailbox must be a NOTIFICATION ────────────────
     * WRONG_TYPE travels rather than being flattened: "that is not a
     * notification" is what a supervisor needs to hear, and the family has
     * reported it that way since Stage 7 Step 4. */
    {
        struct KCNode *root; struct task *target;
        struct task *t = tb_caller(&root, TCB_SLOT, &target);
        ASSERT_NOT_NULL(t);
        /* slot 9 holds an endpoint, not a notification */
        struct KObject *ep = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
        ASSERT_NOT_NULL(ep);
        kobject_init(ep, KOBJ_ENDPOINT, &tb_ops);
        ASSERT_EQ(kcnode_mint(root, 9, ep, RIGHT_READ | RIGHT_WRITE), IRIS_OK);

        ASSERT_EQ(tb_err(sys_tcb_set_fault_handler(TCB_SLOT, 9, 1,
                         ((uint64_t)4 << 32))),
                  (long)IRIS_ERR_WRONG_TYPE);
        ASSERT_EQ(tb_err(sys_tcb_set_timeout_handler(TCB_SLOT, 9, 1,
                         ((uint64_t)4 << 32))),
                  (long)IRIS_ERR_WRONG_TYPE);
        /* watch flattens it to INVALID_ARG, which is its documented contract */
        ASSERT_EQ(tb_err(sys_tcb_watch(TCB_SLOT, 9, 1)),
                  (long)IRIS_ERR_INVALID_ARG);
        test_set_current_task(NULL);
    }

    /* ── TB-6: the TARGET must be a TCB ──────────────────────────────────
     * Naming an endpoint where a thread belongs is INVALID_ARG for this whole
     * family — it maps WRONG_TYPE deliberately, to keep one error code for
     * "that is not the thread you meant". */
    {
        struct KCNode *root; struct task *target;
        struct task *t = tb_caller(&root, TCB_SLOT, &target);
        ASSERT_NOT_NULL(t);
        struct KObject *ep = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
        ASSERT_NOT_NULL(ep);
        kobject_init(ep, KOBJ_ENDPOINT, &tb_ops);
        ASSERT_EQ(kcnode_mint(root, 12, ep, RIGHT_READ | RIGHT_WRITE), IRIS_OK);

        ASSERT_EQ(tb_err(sys_tcb_configure(12, 1, 2, 0)), (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(tb_err(sys_tcb_exit_code(12, 0, 0)),    (long)IRIS_ERR_INVALID_ARG);
        test_set_current_task(NULL);
    }

    /* ── TB-7: an empty slot names no thread ─────────────────────────────*/
    {
        struct KCNode *root; struct task *target;
        struct task *t = tb_caller(&root, TCB_SLOT, &target);
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(tb_err(sys_tcb_configure(200, 1, 2, 0)), (long)IRIS_ERR_NOT_FOUND);
        ASSERT_EQ(tb_err(sys_tcb_watch(200, 1, 1)),        (long)IRIS_ERR_NOT_FOUND);
        test_set_current_task(NULL);
    }

    /* TB-8 RETIRED with SYS_TCB_SELF (ledger A-18 / charter A5).  Its subject
     * was that the syscall needs a destination slot; the syscall is gone,
     * because handing a thread a capability to itself for the asking is
     * ambient authority.  A thread is told which thread it is by whoever
     * created it — in the entry register, the one per-thread channel a freshly
     * started thread has.  The surviving property, that every capability is
     * created INTO a slot, is asserted by every other creator here. */

    /* ── TB-9: the IPC buffer is a capability, checked like one (D-4) ─────
     * A thread's IPC buffer replaces 256 bytes of kernel staging with a frame
     * the user retyped and mapped.  That only holds if the kernel refuses
     * everything that is not such a frame — otherwise a "buffer" is whatever
     * the caller says it is, and the kernel writes message payloads into it.
     *
     * The address is validated for the same reason a mapping address is: the
     * kernel does not use it for the transfer, but it hands it back to the
     * RECEIVER as where its message landed, so a nonsense value here becomes
     * a nonsense pointer in somebody else's message. */
    {
        struct KCNode *root; struct task *target;
        struct task *t = tb_caller(&root, TCB_SLOT, &target);
        ASSERT_NOT_NULL(t);

        const uint64_t VA = 0x8000400000ULL;

        /* Not a TCB in arg0 — the CNode's own slot names the root CNode. */
        ASSERT_EQ(tb_err(sys_tcb_set_ipc_buffer(999, TCB_SLOT, VA)),
                  (long)IRIS_ERR_NOT_FOUND);
        /* A TCB in arg1, where a frame belongs. */
        ASSERT_EQ(tb_err(sys_tcb_set_ipc_buffer(TCB_SLOT, TCB_SLOT, VA)),
                  (long)IRIS_ERR_INVALID_ARG);
        /* An unpopulated slot is not a frame either. */
        ASSERT_EQ(tb_err(sys_tcb_set_ipc_buffer(TCB_SLOT, 77, VA)),
                  (long)IRIS_ERR_NOT_FOUND);
        /* Unregistering may not smuggle an address in: "give it back" and
         * "move it" must not be the same call. */
        ASSERT_EQ(tb_err(sys_tcb_set_ipc_buffer(TCB_SLOT, 0, VA)),
                  (long)IRIS_ERR_INVALID_ARG);
        /* Unregistering a thread that has no buffer is a no-op, not an error:
         * a server tearing down should not have to remember whether it ever
         * registered one. */
        ASSERT_EQ(tb_err(sys_tcb_set_ipc_buffer(TCB_SLOT, 0, 0)), 0L);
        ASSERT_NULL(target->ipc_buffer);

        test_set_current_task(NULL);
    }

    test_set_current_task(NULL);
}
