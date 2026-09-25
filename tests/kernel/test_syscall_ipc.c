/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_syscall_ipc.c — the IPC authority layer, under host unit test.
 *
 * Endpoint IPC is where capabilities cross between principals, so its refusals
 * are the ones that decide whether authority can leak.  Three of them are
 * load-bearing enough to be worth naming:
 *
 *   - RIGHT_WRITE to send and RIGHT_READ to receive.  Reversed or omitted, a
 *     client could serve an endpoint it was only meant to call, and read the
 *     requests other clients send to it.
 *   - RIGHT_TRANSFER to hand a capability along.  Without it, delegation stops
 *     being a decision the grantor makes: anything a sender can name, it could
 *     give away.
 *   - The one-shot reply.  A KReply answers exactly once; a second SYS_REPLY
 *     on a freed object must fail rather than answer whoever happens to be
 *     bound to it now, which after re-staging is a different caller entirely.
 *
 * These are argument-and-authority tests.  A completed rendezvous needs two
 * runnable threads and a scheduler, so the delivery paths belong to the runtime
 * suite; what belongs here is that nothing gets as far as delivery without the
 * right to do it.
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kreply.h>
#include <iris/nc/cspace.h>
#include <iris/nc/rights.h>
#include <iris/task.h>
#include <iris/syscall.h>
#include <iris/ipc_msg.h>
#include <iris/kpage.h>
#include <string.h>

void test_set_current_task(struct task *t);

uint64_t sys_ep_send(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_ep_recv(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_ep_nb_send(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_ep_call(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_reply(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_reply_recv(uint64_t a0, uint64_t a1, uint64_t a2);

static long ip_err(uint64_t r) { return (long)(int64_t)r; }

static void ip_destroy(struct KObject *o) { (void)o; }
static const struct KObjectOps ip_ops = { .close = NULL, .destroy = ip_destroy };

/* Slots used throughout: an endpoint with full rights, one with READ only, one
 * with WRITE only, a non-endpoint, and a reply object. */
enum { S_EP_FULL = 4, S_EP_RD = 5, S_EP_WR = 6, S_NOTEP = 7, S_REPLY = 8,
       S_XFER_NOTRANSFER = 9 };

static struct task *ip_caller(void) {
    struct task *t = (struct task *)kpage_alloc((uint32_t)sizeof(struct task));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    struct KCNode *root = kcnode_alloc(256);
    if (!root) return NULL;
    kobject_active_retain(&root->base);
    t->cspace_root = root;

    /* One endpoint object, three capabilities to it with different rights —
     * which is the shape the rights tests need: the OBJECT is identical, so a
     * refusal can only be about the capability that was invoked. */
    void *epm = kpage_alloc((uint32_t)sizeof(struct KEndpoint));
    if (!epm) return NULL;
    memset(epm, 0, sizeof(struct KEndpoint));
    struct KEndpoint *ep = kendpoint_alloc_at(epm);
    if (!ep) return NULL;
    if (kcnode_mint(root, S_EP_FULL, &ep->base,
                    RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER) != IRIS_OK) return NULL;
    if (kcnode_mint(root, S_EP_RD, &ep->base, RIGHT_READ) != IRIS_OK) return NULL;
    if (kcnode_mint(root, S_EP_WR, &ep->base, RIGHT_WRITE) != IRIS_OK) return NULL;

    struct KObject *nt = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (!nt) return NULL;
    kobject_init(nt, KOBJ_NOTIFICATION, &ip_ops);
    if (kcnode_mint(root, S_NOTEP, nt, RIGHT_READ | RIGHT_WRITE) != IRIS_OK) return NULL;

    void *rpm = kpage_alloc((uint32_t)sizeof(struct KReply));
    if (!rpm) return NULL;
    memset(rpm, 0, sizeof(struct KReply));
    struct KReply *rp = kreply_alloc_at(rpm);
    if (!rp) return NULL;
    if (kcnode_mint(root, S_REPLY, &rp->base, RIGHT_READ | RIGHT_WRITE) != IRIS_OK)
        return NULL;

    /* A capability the caller holds but may NOT hand on. */
    struct KObject *x = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (!x) return NULL;
    kobject_init(x, KOBJ_NOTIFICATION, &ip_ops);
    if (kcnode_mint(root, S_XFER_NOTRANSFER, x, RIGHT_READ | RIGHT_WRITE) != IRIS_OK)
        return NULL;

    test_set_current_task(t);
    return t;
}

/*
 * A-33: a message is the syscall's ARGUMENT WORDS, so arming one means
 * setting them.  There is no struct to point at and no pointer to pass —
 * which is why IP-2, "the message pointer is required", is gone below.
 */
static void ip_msg(struct task *t) {
    for (unsigned i = 0; i < 9u; i++) t->sc_arg[i] = 0;
}

/* ...and one carrying a capability: the MessageInfo says a capability travels
 * and with which rights, and the capability word says which one. */
static void ip_msg_cap(struct task *t, uint64_t cptr, uint32_t rights) {
    ip_msg(t);
    t->sc_arg[1 + IRIS_MSGA_INFO] = iris_mi(0, 0, rights, 0);
    t->sc_arg[1 + IRIS_MSGA_CAP]  = iris_capw(cptr, rights);
}

void test_syscall_ipc(void) {
    TEST_SUITE("IPC authority (endpoint + reply syscalls)");

    /* ── IP-1: no caller, or a caller with no CSpace ─────────────────────*/
    {
        test_set_current_task(NULL);
        ASSERT_EQ(ip_err(sys_ep_send(S_EP_FULL, 0, 0)), (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(ip_err(sys_ep_recv(S_EP_FULL, 0, 0)), (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(ip_err(sys_ep_call(S_EP_FULL, 0, 0)), (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(ip_err(sys_reply(S_REPLY, 0, 0)),     (long)IRIS_ERR_INVALID_ARG);
    }

    /* ── IP-2 is RETIRED (ledger A-33) ───────────────────────────────────
     * It asserted that a message pointer of zero was refused — the check that
     * stood between the kernel and a user address it was about to dereference.
     * A message has no address: it is a MessageInfo word and message
     * registers, so there is nothing to validate and nothing for a second
     * thread to unmap between the check and the copy.  What replaced it is
     * an absence that can be checked once instead of per path: the kernel has
     * no user-pointer READ at all now — `user_range_readable` and
     * `copy_from_user_checked` are deleted, and every surviving access to user
     * memory is a write-back. */

    /* ── IP-3: SEND needs WRITE, RECV needs READ ─────────────────────────
     * The same endpoint OBJECT through three capabilities, so a refusal can
     * only be about the capability invoked.  Reversed or omitted, a client
     * could serve an endpoint it was only meant to call — and read the
     * requests every other client sends to it. */
    {
        struct task *t = ip_caller();
        ASSERT_NOT_NULL(t);
        ip_msg(t);

        /* read-only capability cannot send */
        ASSERT_EQ(ip_err(sys_ep_send(S_EP_RD, 0, 0)), (long)IRIS_ERR_ACCESS_DENIED);
        ASSERT_EQ(ip_err(sys_ep_nb_send(S_EP_RD, 0, 0)), (long)IRIS_ERR_ACCESS_DENIED);
        ASSERT_EQ(ip_err(sys_ep_call(S_EP_RD, 0, 0)), (long)IRIS_ERR_ACCESS_DENIED);
        /* write-only capability cannot receive */
        ASSERT_EQ(ip_err(sys_ep_recv(S_EP_WR, 0, 0)), (long)IRIS_ERR_ACCESS_DENIED);
        test_set_current_task(NULL);
    }

    /* ── IP-4: an endpoint syscall on a non-endpoint is WRONG_TYPE ───────*/
    {
        struct task *t = ip_caller();
        ASSERT_NOT_NULL(t);
        ip_msg(t);
        ASSERT_EQ(ip_err(sys_ep_send(S_NOTEP, 0, 0)), (long)IRIS_ERR_WRONG_TYPE);
        ASSERT_EQ(ip_err(sys_ep_recv(S_NOTEP, 0, 0)), (long)IRIS_ERR_WRONG_TYPE);
        test_set_current_task(NULL);
    }

    /* ── IP-5: a handle-shaped value is not an endpoint address ──────────*/
    {
        struct task *t = ip_caller();
        ASSERT_NOT_NULL(t);
        ip_msg(t);
        const uint64_t handle_shaped = (uint64_t)1u << 31;
        ASSERT_EQ(ip_err(sys_ep_send(handle_shaped, 0, 0)) < 0, 1);
        ASSERT_EQ(ip_err(sys_ep_recv(handle_shaped, 0, 0)) < 0, 1);
        test_set_current_task(NULL);
    }

    /* ── IP-6: handing a capability along needs RIGHT_TRANSFER ───────────
     * Without this check delegation stops being a decision the grantor makes:
     * anything a sender can NAME, it could give away.  The message names a
     * capability the caller genuinely holds — the refusal is about the right,
     * not about the slot. */
    {
        struct task *t = ip_caller();
        ASSERT_NOT_NULL(t);
        ip_msg_cap(t, S_XFER_NOTRANSFER, RIGHT_READ);  /* held, not transferable */
        ASSERT_EQ(ip_err(sys_ep_send(S_EP_FULL, 0, 0)),
                  (long)IRIS_ERR_ACCESS_DENIED);
        test_set_current_task(NULL);
    }

    /* ── IP-7: a capability the caller does NOT hold cannot be attached ──*/
    {
        struct task *t = ip_caller();
        ASSERT_NOT_NULL(t);
        ip_msg_cap(t, 199, RIGHT_READ);             /* an empty slot */
        ASSERT_EQ(ip_err(sys_ep_send(S_EP_FULL, 0, 0)),
                  (long)IRIS_ERR_NOT_FOUND);
        test_set_current_task(NULL);
    }

    /* ── IP-8: the reply object is ONE-SHOT ─────────────────────────────
     * A KReply with no bound caller has already been answered, or was never
     * bound.  Replying anyway must fail: after re-staging, the object may be
     * bound to a DIFFERENT caller, and a second reply would answer somebody
     * else's call with this one's payload. */
    {
        struct task *t = ip_caller();
        ASSERT_NOT_NULL(t);
        ip_msg(t);
        ASSERT_EQ(ip_err(sys_reply(S_REPLY, 0, 0)), (long)IRIS_ERR_NOT_FOUND);
        /* and a reply CPtr of zero is not an address */
        ASSERT_EQ(ip_err(sys_reply(0, 0, 0)), (long)IRIS_ERR_INVALID_ARG);
        /* the object named must actually be a reply */
        ASSERT_EQ(ip_err(sys_reply(S_NOTEP, 0, 0)) < 0, 1);
        test_set_current_task(NULL);
    }

    /* ── IP-9: REPLY_RECV does not receive when the reply half failed ────
     * The composition rule: a syscall made of two halves must not run the
     * second when the first did not happen.  Here the reply object is unbound,
     * so the call must report that and NOT go on to block the caller on an
     * endpoint — a server that believed it had answered would wait for the
     * next request while its client waited for an answer that never came. */
    {
        struct task *t = ip_caller();
        ASSERT_NOT_NULL(t);
        ip_msg(t);
        ASSERT_EQ(ip_err(sys_reply_recv(S_REPLY, 0, S_EP_FULL)),
                  (long)IRIS_ERR_NOT_FOUND);
        ASSERT_EQ(t->sc_restart, 0u);      /* it did not park on the endpoint */
        test_set_current_task(NULL);
    }

    test_set_current_task(NULL);
}
