/*
 * syscall_reply.c — Block 7 (Ph85-87): seL4-style reply capabilities.
 *
 * SYS_EP_CALL: send message on endpoint + block waiting for reply via KReply.
 *   - If a receiver is already waiting: rendezvous immediately, deliver KReply
 *     to receiver's handle table, then transition caller to TASK_BLOCKED_REPLY.
 *   - If no receiver: block as TASK_BLOCKED_SEND (ep_call_mode=1); EP_RECV code
 *     detects the flag and performs the KReply delivery at rendezvous time.
 *   - EP_CALL does NOT support simultaneous capability transfer (use EP_SEND for that).
 *
 * SYS_REPLY: invoke a KReply handle to unblock its caller with a reply message.
 *   - Stages the reply message + bulk payload in the caller's ipc_kbuf.
 *   - Clears caller->pending_kreply (releases task's own KReply ref).
 *   - Transitions caller to TASK_READY.
 *   - Returns IRIS_ERR_NOT_FOUND if the KReply was already invoked.
 *
 * Reply-cap transfer (Phase 7.1): a reply MAY carry one capability in
 * msg.attached_handle / msg.attached_rights, with EP_SEND staging semantics:
 *   - The server handle must have RIGHT_TRANSFER; it is consumed on success.
 *   - The cap is installed in the EP_CALL caller's handle table; the caller
 *     sees the new handle id in msg.attached_handle (IRIS_MSG_NO_CAP if the
 *     caller's table is full — soft failure, message still delivered).
 *   - If SYS_REPLY fails before staging (bad KReply handle, unreadable msg,
 *     stage validation error) the server handle is NOT consumed.
 *   - A1.10: if the KReply was already invoked (IRIS_ERR_NOT_FOUND) nothing
 *     is delivered and the server handle is NOT consumed either — the source
 *     cap is consumed only when a blocked caller is determined (two-phase
 *     staging; before A1.10 this path destroyed the staged cap).
 */
#include "syscall_priv.h"
#include <iris/nc/kreply.h>
#include <iris/nc/kendpoint.h>
#include <iris/ipc_msg.h>

static inline void copy_irismsg_r(struct IrisMsg *dst, const struct IrisMsg *src) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < (uint32_t)sizeof(struct IrisMsg); i++) d[i] = s[i];
}

static inline void copy_kbuf_r(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0u; i < n; i++) dst[i] = src[i];
}

/* ep_get_r removed — use cspace_resolve_only_endpoint (Phase 3.2) */

/* ── SYS_EP_CALL ──────────────────────────────────────────────────────── */

uint64_t sys_ep_call(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* msg is both send (input) and reply (output) — must be readable and writable. */
    if (!user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)) ||
        !user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    uint64_t ep_badge = 0;
    iris_error_t err = cspace_resolve_only_endpoint_badged(t->cspace_root, (iris_cptr_t)arg0, RIGHT_WRITE, &ep, &_ep_r, &ep_badge);
    if (err != IRIS_OK) return syscall_err(err);

    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Phase 9: stamp the caller badge from the invoked cap (anti-spoofing);
     * the server observes it on EP_RECV / EP_NB_RECV. */
    t->ipc_msg.sender_badge = ep_badge;

    /* attached_handle is reserved for the reply cap on EP_CALL.  A1.5: a CPtr
     * declares the caller's receive-slot for a cap the REPLY transfers back
     * (the KReply itself always stays a handle); a handle VALUE keeps the
     * historical INVALID_ARG contract, so legacy callers (forced to pass 0)
     * are unaffected.  Stage 4: the boundary is the handle tag bit, not the
     * literal 1024 — a multi-level CPtr is a legitimate receive slot. */
    if (t->ipc_msg.attached_handle != 0u &&
        !cspace_value_is_cptr((iris_cptr_t)t->ipc_msg.attached_handle)) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    {
        iris_error_t se = syscall_ipc_recv_slot_declare(t, t->ipc_msg.attached_handle);
        if (se != IRIS_OK) {
            kobject_release(&ep->base);
            return syscall_err(se);
        }
    }
    t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;

    /* Phase 11: stage a transferred cap from attached_cap (separate field so the
     * reply cap and the transferred cap never collide).  Staging validates the
     * caller really holds it and reduces to the requested rights; the raw
     * attached_cap number is then cleared so it can never be delivered as-is.
     * A1.10: PEEK only — the caller's handle is consumed at a commit point
     * (immediate rendezvous here, or the receiver's take of a queued caller),
     * so CLOSED / endpoint close / cancel before delivery leave it intact. */
    struct KObject *xfer_obj     = 0;
    uint32_t        xfer_rights  = 0;
    uint64_t        xfer_badge   = 0;
    struct KCNode  *xfer_src_cn  = 0;
    uint32_t        xfer_src_idx = 0;
    if (t->ipc_msg.attached_cap != IRIS_MSG_NO_CAP) {
        iris_error_t cr = syscall_ipc_stage_cap_peek_badged(
            t, t->ipc_msg.attached_cap, t->ipc_msg.attached_cap_rights,
            &xfer_obj, &xfer_rights, &xfer_badge, &xfer_src_cn, &xfer_src_idx);
        if (cr != IRIS_OK) { kobject_release(&ep->base); return syscall_err(cr); }
    }
    t->ipc_msg.attached_cap = IRIS_MSG_NO_CAP;

    /* Save reply bulk destination before staging the send bulk (same buf_uptr field). */
    uint64_t reply_buf_uptr = t->ipc_msg.buf_uptr;

    /* Stage send bulk payload. */
    t->ipc_kbuf_len = 0u;
    if (t->ipc_msg.buf_len > 0u && t->ipc_msg.buf_uptr != 0u) {
        uint32_t n = t->ipc_msg.buf_len;
        if (n > IRIS_IPC_BUF_SIZE) n = IRIS_IPC_BUF_SIZE;
        if (!user_range_readable(t->ipc_msg.buf_uptr, n) ||
            !copy_from_user_checked(t->ipc_kbuf, t->ipc_msg.buf_uptr, n)) {
            kobject_release(&ep->base);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }
        t->ipc_kbuf_len    = n;
        t->ipc_msg.buf_len = n;
    }

    t->ep_call_mode     = 1u;
    t->ep_recv_buf_uptr = reply_buf_uptr; /* where reply bulk should land on wake-up */
    t->ipc_ep_closed    = 0u;

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        t->ep_call_mode = 0u;
        /* A1.10: nothing delivered — drop the staging ref; the caller's
         * source handle was never consumed (also fixes the pre-A1.10 leak
         * of the staged ref on this exit). */
        if (xfer_obj) {
            kobject_release(xfer_obj);
            syscall_ipc_stage_cap_abort(xfer_src_cn);
        }
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state == EP_STATE_RECV) {
        /* Immediate rendezvous: a receiver is already waiting. */
        struct task *receiver = ep->queue_head;

        /* Phase S1: a CALL needs the receiver's explicit reply object.  If the
         * blocked receiver staged none, fail the call BEFORE consuming
         * anything — the receiver stays queued, the caller keeps its staged
         * cap (implicit KReply fabrication is retired). */
        if (!receiver->ep_reply_obj) {
            irq_spinlock_unlock(&ep->lock, flags);
            t->ep_call_mode = 0u;
            if (xfer_obj) {
                kobject_release(xfer_obj);
                syscall_ipc_stage_cap_abort(xfer_src_cn);
            }
            kobject_release(&ep->base);
            return syscall_err(IRIS_ERR_NOT_SUPPORTED);
        }

        ep->queue_head = receiver->ep_next;
        if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
        receiver->ep_next     = 0;
        receiver->blocking_ep = 0;

        copy_irismsg_r(&receiver->ipc_msg, &t->ipc_msg);
        receiver->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
        receiver->ipc_msg.attached_cap    = IRIS_MSG_NO_CAP;
        receiver->ipc_msg_ready           = 1u;

        if (t->ipc_kbuf_len > 0u) {
            copy_kbuf_r(receiver->ipc_kbuf, t->ipc_kbuf, t->ipc_kbuf_len);
            receiver->ipc_kbuf_len = t->ipc_kbuf_len;
        } else {
            receiver->ipc_kbuf_len = 0u;
        }

        t->ep_call_mode = 0u;
        irq_spinlock_unlock(&ep->lock, flags);

        /* Phase 11: deliver the staged transferred cap into the receiver's
         * attached_cap (the reply cap below takes attached_handle).
         * A1.5: routed — lands in the receiver's declared receive-slot
         * (CPtr) or its handle table.  A1.10: receiver dequeued → delivery
         * committed; consume the caller's source handle now. */
        if (xfer_obj) {
            uint32_t nh = syscall_ipc_deliver_cap_routed(receiver, xfer_obj,
                                                         xfer_rights, xfer_badge,
                                                         xfer_src_cn, xfer_src_idx);
            receiver->ipc_msg.attached_cap        = nh;
            receiver->ipc_msg.attached_cap_rights = xfer_rights;
            if (nh != IRIS_MSG_NO_CAP)
                syscall_ipc_stage_cap_commit(t, xfer_src_cn, xfer_src_idx);
            else
                syscall_ipc_stage_cap_abort(xfer_src_cn);
        }

        /* Phase S1: bind the receiver's staged explicit reply object to this
         * caller (verified non-NULL before the receiver was dequeued; the
         * kernel is non-preemptive between that check and this bind).  The
         * receiver's staging lifecycle ref transfers to t->pending_kreply;
         * the receiver observes its own reply CPtr in msg.attached_handle. */
        {
            struct KReply *r = receiver->ep_reply_obj;
            if (kreply_bind_caller(r, t) == IRIS_OK) {
                /* Stage 8-mcs: lend this caller's time to a passive server.
                 * This is the sender-side rendezvous — the receiver was
                 * already waiting — and it needs the donation exactly as much
                 * as the receiver-side ones do. */
                kreply_donate_on_call(r, t, receiver);
                t->pending_kreply = r;   /* staging ref transferred */
                receiver->ipc_msg.attached_handle = receiver->ep_reply_val;
                receiver->ep_reply_obj = 0;
                receiver->ep_reply_val = 0;
                __atomic_fetch_add(&iris_ipc_stat_reply_caps, 1u, __ATOMIC_RELAXED);
            } else {
                /* Defensive: claim lost (last cap dropped while blocked).
                 * Wake receiver (message already delivered) but fail the call. */
                task_wakeup(receiver);
                kobject_release(&ep->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
        }

        /* Receiver is ready; caller blocks waiting for reply. */
        task_wakeup(receiver);
        t->state        = TASK_BLOCKED_REPLY;
        task_yield(); /* resumes when sys_reply (or kreply_obj_close) sets TASK_READY */

    } else {
        /* No receiver: queue sender and block as TASK_BLOCKED_SEND (ep_call_mode=1).
         * sys_ep_recv detects the flag at rendezvous time and creates the KReply. */
        ep->ep_state     = EP_STATE_SEND;
        t->ep_next       = 0;
        t->blocking_ep   = ep;
        t->ipc_msg_ready = 0u;
        /* Phase 11: carry the staged transferred cap to the receiver (delivered
         * into attached_cap by sys_ep_recv / sys_ep_nb_recv).  A1.10 / S4: the
         * source SLOT rides along un-consumed; the receiver commits it at
         * take time, and close/cancel paths abort it without consuming. */
        t->ep_cap_obj     = xfer_obj;
        t->ep_cap_rights  = xfer_rights;
        t->ep_cap_badge   = xfer_badge;
        t->ep_cap_src_cn  = xfer_obj ? xfer_src_cn : 0;
        t->ep_cap_src_idx = xfer_obj ? xfer_src_idx : 0;

        if (ep->queue_tail) { ep->queue_tail->ep_next = t; ep->queue_tail = t; }
        else                { ep->queue_head = t; ep->queue_tail = t; }

        t->state = TASK_BLOCKED_SEND;
        irq_spinlock_unlock(&ep->lock, flags);

        task_yield(); /* stays blocked through SEND→REPLY; wakes at READY (sys_reply) */
    }

    /* Phase S4 (Step 2): endpoint close leaves our source-slot refs for us to
     * drop (kendpoint_obj_close cannot release them under ep->lock).  Nothing
     * was delivered on that path — the source slot itself survives. */
    if (t->ep_cap_src_cn) {
        syscall_ipc_stage_cap_abort(t->ep_cap_src_cn);
        t->ep_cap_src_cn  = 0;
        t->ep_cap_src_idx = 0;
    }

    kobject_release(&ep->base);

    /* ── Wake-up handling ─────────────────────────────────────────────── */

    if (t->pending_kreply) {
        kobject_release(&t->pending_kreply->base);
        t->pending_kreply = 0;
    }

    /* A1.5: a reply cap delivery already consumed the declaration from the
     * server's context; make sure it never survives this call either way. */
    t->ep_recv_slot = 0;

    if (t->ipc_ep_closed) {
        t->ipc_ep_closed    = 0u;
        t->ep_recv_buf_uptr = 0u;
        return syscall_err(IRIS_ERR_CLOSED);
    }

    /* Deliver reply bulk from ipc_kbuf to caller's user space. */
    if (t->ipc_kbuf_len > 0u && t->ep_recv_buf_uptr != 0u &&
        user_range_writable(t->ep_recv_buf_uptr, t->ipc_kbuf_len) &&
        copy_to_user_checked(t->ep_recv_buf_uptr, t->ipc_kbuf, t->ipc_kbuf_len)) {
        t->ipc_msg.buf_len  = t->ipc_kbuf_len;
        t->ipc_msg.buf_uptr = t->ep_recv_buf_uptr;
    } else if (t->ipc_kbuf_len > 0u) {
        t->ipc_msg.buf_len = t->ipc_kbuf_len;
    }
    t->ipc_kbuf_len     = 0u;
    t->ep_recv_buf_uptr = 0u;

    if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    return syscall_ok_u64(0);
}

/* ── SYS_REPLY ────────────────────────────────────────────────────────── */

uint64_t sys_reply(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    iris_cptr_t kreply_cptr = (iris_cptr_t)arg0;
    if (!kreply_cptr) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KReply *rp; iris_rights_t rp_rights;
    iris_error_t err = cspace_resolve_only_reply(t->cspace_root, kreply_cptr,
                                                       RIGHT_WRITE, &rp, &rp_rights);
    if (err != IRIS_OK) return syscall_err(err);

    /* Read reply message from server before taking the kreply lock. */
    struct IrisMsg reply_msg;
    if (!copy_from_user_checked(&reply_msg, arg1, (uint32_t)sizeof(reply_msg))) {
        kobject_release(&rp->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Stage attached reply cap (if any) before consuming the one-shot KReply,
     * so staging errors leave the reply invocable and the handle untouched.
     * A1.10: PEEK only — the server's handle is consumed just below, once
     * the caller is determined (delivery committed).  A reply that loses the
     * one-shot race (NOT_FOUND) no longer destroys the server's cap. */
    struct KObject *xfer_obj     = 0;
    uint32_t        xfer_rights  = 0;
    uint64_t        xfer_badge   = 0;
    struct KCNode  *xfer_src_cn  = 0;
    uint32_t        xfer_src_idx = 0;
    if (reply_msg.attached_handle != IRIS_MSG_NO_CAP) {
        iris_error_t cr = syscall_ipc_stage_cap_peek_badged(t, reply_msg.attached_handle,
                                                reply_msg.attached_rights,
                                                &xfer_obj, &xfer_rights, &xfer_badge,
                                                &xfer_src_cn, &xfer_src_idx);
        if (cr != IRIS_OK) {
            kobject_release(&rp->base);
            return syscall_err(cr);
        }
        reply_msg.attached_handle = IRIS_MSG_NO_CAP;
    }

    uint64_t flags  = irq_spinlock_lock(&rp->lock);
    struct task *caller = rp->caller;
    rp->caller          = 0;
    irq_spinlock_unlock(&rp->lock, flags);

    if (!caller) {
        /* Already replied / caller gone: nothing delivered, so the peeked
         * staging ref is dropped and the server KEEPS its source handle. */
        if (xfer_obj) {
            kobject_release(xfer_obj);
            syscall_ipc_stage_cap_abort(xfer_src_cn);
        }
        kobject_release(&rp->base);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    /* Deliver reply message into caller's staging (caller is blocked — safe). */
    copy_irismsg_r(&caller->ipc_msg, &reply_msg);
    caller->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
    /* Phase 9: replies carry NO sender identity — the kernel forces badge 0
     * so a server cannot spoof a badge into its caller (reply identity is
     * implied by the one-shot KReply itself). */
    caller->ipc_msg.sender_badge = 0u;

    /* Install the staged reply cap.  A1.5: routed — lands in the caller's
     * receive-slot declared at EP_CALL entry (CPtr) or its handle table. */
    if (xfer_obj) {
        uint32_t new_h = syscall_ipc_deliver_cap_routed(caller, xfer_obj,
                                                        xfer_rights, xfer_badge,
                                                        xfer_src_cn, xfer_src_idx);
        caller->ipc_msg.attached_handle = new_h;
        /* Phase S4 (Step 2): caller determined and still blocked — delivery
         * committed, so consume the server's source slot (outside rp->lock). */
        if (new_h != IRIS_MSG_NO_CAP)
            syscall_ipc_stage_cap_commit(t, xfer_src_cn, xfer_src_idx);
        else
            syscall_ipc_stage_cap_abort(xfer_src_cn);
    }

    /* Stage reply bulk directly into caller's ipc_kbuf (server's CR3 → kernel memory). */
    caller->ipc_kbuf_len = 0u;
    if (reply_msg.buf_len > 0u && reply_msg.buf_uptr != 0u) {
        uint32_t n = reply_msg.buf_len;
        if (n > IRIS_IPC_BUF_SIZE) n = IRIS_IPC_BUF_SIZE;
        if (user_range_readable(reply_msg.buf_uptr, n) &&
            copy_from_user_checked(caller->ipc_kbuf, reply_msg.buf_uptr, n)) {
            caller->ipc_kbuf_len    = n;
            caller->ipc_msg.buf_len = n;
        } else {
            caller->ipc_msg.buf_len = 0u;
        }
    }

    /*
     * Stage 8-mcs — the lent scheduling context goes home BEFORE the caller
     * runs again.  Ordering matters: waking the caller first would leave a
     * window where it is runnable with no SC while the server still holds it,
     * so the client would be scheduled on nothing and the server would keep
     * spending time that is no longer on loan.
     */
    kreply_return_donation(rp, caller);

    /* Release task's own KReply lifecycle ref. */
    if (caller->pending_kreply) {
        kobject_release(&caller->pending_kreply->base);
        caller->pending_kreply = 0;
    }

    /* Unblock caller. */
    task_wakeup(caller);

    kobject_release(&rp->base); /* drop resolve lifecycle ref */
    return syscall_ok_u64(0);
}
