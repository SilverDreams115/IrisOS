#include "syscall_priv.h"
#include <iris/nc/kendpoint.h>
#include <iris/nc/kreply.h>
#include <iris/ipc_msg.h>
#include <iris/nc/kschedctx.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

/* IrisMsg = 72 bytes = 9 × uint64_t (Phase 9: +sender_badge) — word copy
 * avoids byte-loop overhead. */
static inline void irismsg_copy64(struct IrisMsg *dst, const struct IrisMsg *src) {
    const uint64_t *s = (const uint64_t *)src;
    uint64_t       *d = (uint64_t *)dst;
    d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
    d[4]=s[4]; d[5]=s[5]; d[6]=s[6]; d[7]=s[7];
    d[8]=s[8]; d[9]=s[9];          /* Phase 11: sender_badge + attached_cap pair */
    _Static_assert(sizeof(struct IrisMsg) == 10u * sizeof(uint64_t),
                   "irismsg_copy64 word count");
}

static inline void copy_kbuf(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

/*
 * ── D-4: the bulk payload, when a thread has an IPC BUFFER of its own ──────
 *
 * Three helpers, shared with syscall_reply.c, that put every bulk-payload site
 * behind one decision: does this thread have a registered frame, or is it
 * still on the 256 bytes of kernel staging inside its TCB?
 *
 * The kernel reaches a registered frame through its own physical window, which
 * is mapped in every address space.  That is the whole reason this path is
 * cheaper AND safer than the staging one: no user pointer is named, so there
 * is no window between validating an address and copying through it, and the
 * copy happens once instead of three times.
 */
uint8_t *ipc_buf_kva(struct task *t) {
    if (!t || !t->ipc_buffer) return 0;
    return (uint8_t *)(uintptr_t)PHYS_TO_VIRT(t->ipc_buffer->paddr);
}

uint32_t ipc_buf_capacity(struct task *t) {
    if (!t || !t->ipc_buffer) return IRIS_IPC_BUF_SIZE;
    uint64_t sz = t->ipc_buffer->size;
    return (sz > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)sz;
}

/*
 * Make this thread's outgoing payload available to the kernel.
 *
 * With a registered buffer there is NOTHING to do but agree on a length: the
 * user wrote its bytes into a page it owns and the kernel can already read
 * them.  `msg.buf_uptr` may then be zero or the buffer's own address, and
 * anything else is REFUSED — see below for what the silent version cost.
 */
iris_error_t ipc_stage_out(struct task *t) {
    t->ipc_kbuf_len = 0;
    uint32_t n = t->ipc_msg.buf_len;
    if (n == 0u) return IRIS_OK;

    uint32_t cap = ipc_buf_capacity(t);
    if (n > cap) n = cap;

    if (t->ipc_buffer) {
        /*
         * A thread with a registered buffer sends FROM it.  Naming some other
         * address is refused, not silently ignored.
         *
         * The silent version of this cost a boot's worth of corrupted console
         * output and did not fail a single test: the shared console client
         * marshals into a buffer its caller passes, five services passed their
         * own static array, and the kernel dutifully sent whatever happened to
         * be at offset 0 of their IPC buffer instead.  Every log line came out
         * as the last reply payload the service had composed.  Nothing
         * asserted on log text, so nothing noticed.
         *
         * seL4 has no `buf_uptr` at all — there is one IPC buffer and that is
         * where a message is marshalled.  IRIS keeps the field for threads
         * that have not registered one, and for threads that HAVE, a pointer
         * that disagrees with the buffer is a marshalling mistake.  Refusing
         * it turns a silent corruption into an error at the call site that
         * made it.
         */
        if (t->ipc_msg.buf_uptr != 0u &&
            t->ipc_msg.buf_uptr != t->ipc_buffer_uvaddr)
            return IRIS_ERR_INVALID_ARG;
    } else {
        if (t->ipc_msg.buf_uptr == 0u) return IRIS_OK;
        if (!user_range_readable(t->ipc_msg.buf_uptr, n) ||
            !copy_from_user_checked(t->ipc_kbuf, t->ipc_msg.buf_uptr, n))
            return IRIS_ERR_INVALID_ARG;
    }
    t->ipc_kbuf_len    = n;
    t->ipc_msg.buf_len = n;
    return IRIS_OK;
}

/*
 * Move the sender's payload to where the receiver will look for it, and say
 * where that is.
 *
 * `receiver_current` says whether we are running in the receiver's address
 * space, which is the only condition under which the staging path may write
 * to the buffer the receiver named at EP_RECV.  When it is false the bytes go
 * into the receiver's TCB staging and the wake path presents them later.
 *
 * The registered-buffer case needs neither: the frame is reachable from any
 * address space, so the transfer is the same one copy either way.
 */
void ipc_transfer_bulk(struct task *sender, struct task *receiver,
                       int receiver_current) {
    uint32_t n = sender->ipc_kbuf_len;
    receiver->ipc_kbuf_len = 0u;
    sender->ipc_kbuf_len   = 0u;
    if (n == 0u) return;

    const uint8_t *src = ipc_buf_kva(sender);
    if (!src) src = sender->ipc_kbuf;

    uint8_t *dst = ipc_buf_kva(receiver);
    if (dst) {
        uint32_t cap = ipc_buf_capacity(receiver);
        if (n > cap) n = cap;
        copy_kbuf(dst, src, n);
        receiver->ipc_msg.buf_len  = n;
        receiver->ipc_msg.buf_uptr = receiver->ipc_buffer_uvaddr;
        return;
    }

    if (n > IRIS_IPC_BUF_SIZE) n = IRIS_IPC_BUF_SIZE;

    if (receiver_current) {
        uint64_t recv_buf = receiver->ep_recv_buf_uptr;
        if (recv_buf != 0u && user_range_writable(recv_buf, n) &&
            copy_to_user_checked(recv_buf, src, n)) {
            receiver->ipc_msg.buf_len  = n;
            receiver->ipc_msg.buf_uptr = recv_buf;
        } else {
            /* Report what was available even when it could not be written:
             * a receiver that named no buffer, or too small a one, still has
             * to learn that a payload existed. */
            receiver->ipc_msg.buf_len  = n;
            receiver->ipc_msg.buf_uptr = 0u;
        }
        return;
    }

    copy_kbuf(receiver->ipc_kbuf, src, n);
    receiver->ipc_kbuf_len = n;
}

/*
 * The same transfer, for a REPLY.
 *
 * SYS_REPLY is the one path whose outgoing message is passed by value rather
 * than staged in the sender's TCB: the server hands the kernel a `struct
 * IrisMsg` and the kernel is still in the server's address space, so it can
 * read the server's user memory directly and skip a copy.  That shortcut is
 * why this cannot just call ipc_transfer_bulk — the source is not
 * `server->ipc_kbuf`.
 *
 * A server WITH a registered buffer takes the same shortcut for free and more
 * safely: its payload is already in a frame the kernel can read from any
 * address space, so `reply_msg->buf_uptr` is ignored exactly as it is on a
 * send.
 */
void ipc_transfer_reply(struct task *server, struct task *caller,
                        const struct IrisMsg *reply_msg) {
    caller->ipc_kbuf_len = 0u;
    uint32_t n = reply_msg->buf_len;
    if (n == 0u) return;

    const uint8_t *src = ipc_buf_kva(server);
    uint8_t       *dst = ipc_buf_kva(caller);

    /* Same rule as a send: a server with a registered buffer replies FROM it,
     * and a reply that names some other address is a marshalling mistake. */
    if (src && reply_msg->buf_uptr != 0u &&
        reply_msg->buf_uptr != server->ipc_buffer_uvaddr) {
        caller->ipc_msg.buf_len = 0u;
        return;
    }

    /* Neither end may be asked to exceed what it owns. */
    if (src && n > ipc_buf_capacity(server)) n = ipc_buf_capacity(server);
    if (dst) { if (n > ipc_buf_capacity(caller)) n = ipc_buf_capacity(caller); }
    else     { if (n > IRIS_IPC_BUF_SIZE)        n = IRIS_IPC_BUF_SIZE; }

    uint8_t *into = dst ? dst : caller->ipc_kbuf;

    if (src) {
        copy_kbuf(into, src, n);
    } else {
        if (reply_msg->buf_uptr == 0u ||
            !user_range_readable(reply_msg->buf_uptr, n) ||
            !copy_from_user_checked(into, reply_msg->buf_uptr, n)) {
            caller->ipc_msg.buf_len = 0u;
            return;
        }
    }

    caller->ipc_msg.buf_len = n;
    if (dst) caller->ipc_msg.buf_uptr = caller->ipc_buffer_uvaddr;
    else     caller->ipc_kbuf_len     = n;
}

/* ep_get removed — use cspace_resolve_only_endpoint (Phase 3.2) */

/*
 * A1.9/A1.10 two-phase cap staging — shared by EP_SEND / EP_NB_SEND /
 * EP_CALL / SYS_REPLY (declared in syscall_priv.h).
 *
 * peek: validate + retain WITHOUT consuming the sender's source SLOT.  The
 * returned object ref is the staging ref; on any non-delivery exit (CLOSED /
 * WOULD_BLOCK / endpoint close / waiter cancel / lost one-shot reply race)
 * release it and call _abort, and the sender keeps its cap.
 *
 * commit: consume the source SLOT only once delivery is committed — the
 * receiver is dequeued (immediate rendezvous) or the receiver takes the
 * staged cap from a queued sender.  Blocking paths carry the source slot in
 * task->ep_cap_src_cn / ep_cap_src_idx next to the staged object.
 *
 * Phase S4 (Step 2) ordering rule: DELIVER first, commit second.  The MDB
 * parents the delivered cap to the source slot, which must still be occupied
 * at delivery time; the subsequent delete reparents the delivered cap to the
 * grandparent, preserving move semantics. */
iris_error_t syscall_ipc_stage_cap_peek_badged(struct task *t, uint32_t src_cptr,
                                               uint32_t requested_rights,
                                               struct KObject **out_obj,
                                               uint32_t *out_rights,
                                               uint64_t *out_badge,
                                               struct KCNode **out_src_cn,
                                               uint32_t *out_src_idx) {
    /* Phase S4 (Step 2): CSpace-only source.  A handle value (>=1024) is not
     * a transfer source any more — it fails cleanly, it does NOT fall back
     * (charter §3.7, invariant A6). */
    if (!cspace_only_cptr((uint64_t)src_cptr)) return IRIS_ERR_INVALID_ARG;

    struct KCNode *src_cn;
    uint32_t       src_idx;
    iris_error_t r = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)src_cptr,
                                         &src_cn, &src_idx);
    if (r != IRIS_OK) return r;

    struct KObject *xo;
    iris_rights_t   xr;
    uint64_t        badge = 0;
    r = kcnode_fetch_badged(src_cn, src_idx, &xo, &xr, &badge);
    if (r != IRIS_OK) {
        kobject_active_release(&src_cn->base);
        kobject_release(&src_cn->base);
        return r;
    }
    /* fetch takes retain+active_retain; the staging contract carries a single
     * lifecycle ref (released by one kobject_release on every exit path). */
    kobject_active_release(xo);

    if (!rights_check(xr, RIGHT_TRANSFER)) {
        kobject_release(xo);
        kobject_active_release(&src_cn->base);
        kobject_release(&src_cn->base);
        return IRIS_ERR_ACCESS_DENIED;
    }

    iris_rights_t cap_rights = rights_reduce(xr, (iris_rights_t)requested_rights);
    if (cap_rights == RIGHT_NONE) {
        kobject_release(xo);
        kobject_active_release(&src_cn->base);
        kobject_release(&src_cn->base);
        return IRIS_ERR_INVALID_ARG;
    }

    /* Phase 9: the transferred cap keeps its badge across the transfer. */
    if (out_badge) *out_badge = badge;

    *out_obj     = xo;
    *out_rights  = (uint32_t)cap_rights;
    *out_src_cn  = src_cn;   /* active+lifecycle held until commit/abort */
    *out_src_idx = src_idx;
    return IRIS_OK;
}

/* Consume the peeked source slot once delivery is committed (move semantics).
 * kcnode_slot_delete is idempotent on an empty slot, so a source revoked
 * while staged is a benign no-op here — the delivery itself already failed
 * closed in _deliver_cap_routed. */
void syscall_ipc_stage_cap_commit(struct task *t, struct KCNode *src_cn,
                                  uint32_t src_idx) {
    (void)t;
    if (!src_cn) return;
    (void)kcnode_slot_delete(src_cn, src_idx);
    kobject_active_release(&src_cn->base);
    kobject_release(&src_cn->base);
}

/* Non-delivery exit: the sender keeps its cap — release the CNode refs only. */
void syscall_ipc_stage_cap_abort(struct KCNode *src_cn) {
    if (!src_cn) return;
    kobject_active_release(&src_cn->base);
    kobject_release(&src_cn->base);
}

/* A1.10: the consume-at-stage wrappers (stage_cap / stage_cap_badged =
 * peek + immediate commit) are retired — every transfer path now commits
 * only at its delivery point. */

/* A1.7 diagnostic counters (relaxed atomics; no behavior depends on them).
 * slot/handle/toctou partition every transferred-cap delivery.  Since Stage 4
 * only the SLOT partition can be non-zero: handle materialization is retired
 * (see below) and the TOCTOU degradation went in Step 2, so both are
 * structural zeros kept as retirement witnesses.
 * reply_caps counts successful KReply bindings.  Read by sys_sched_info
 * (extended layout). */
uint32_t iris_ipc_stat_slot_deliveries   = 0u;
uint32_t iris_ipc_stat_handle_deliveries = 0u;
uint32_t iris_ipc_stat_toctou_fallbacks  = 0u;
uint32_t iris_ipc_stat_reply_caps        = 0u;

static void ipc_stat_bump(uint32_t *c) {
    __atomic_fetch_add(c, 1u, __ATOMIC_RELAXED);
}

/*
 * syscall_ipc_deliver_cap_badged — RETIRED (Stage 4).
 *
 * It installed a transferred capability into the RECEIVER'S HANDLE TABLE when
 * the receiver had declared no receive slot.  That was the last place in the
 * kernel where a capability entered a process through the handle namespace,
 * and it was not a fallback the sender or receiver chose: it happened because
 * the receiver said nothing.
 *
 * Charter I1 says a capability transfer uses CSpace as source AND destination.
 * The source became CSpace-only in Step 2; this is the destination half.  A
 * receive with no declared slot now delivers the MESSAGE without the
 * capability, which is the same fail-closed shape a raced or occupied slot has
 * had since Step 2 — the sender's source slot is left intact by its abort
 * path, so nothing is consumed and no authority is lost.  A receiver that
 * wants the capability declares where to put it.
 */

/* ── A1.5: receive-slot support ──────────────────────────────────────── */

/* Resolve a process's root CNode (lifecycle ref only; release with a single
 * kobject_release).  NULL if the process has no root CNode (OOM-degraded). */
/* ipc_root_cnode_of is gone with the direct-index receive slot: both the
 * declaration and the delivery resolve a full CPtr through
 * cspace_resolve_dest_slot, which starts from the structural root itself. */

/*
 * syscall_ipc_recv_slot_declare — validate + record a receive-slot declared
 * by a recv-family syscall (EP_RECV / EP_NB_RECV / EP_CALL).
 *
 * declared == 0 or a handle value: no declaration (legacy).  Handle values are
 * IGNORED, not rejected: receivers that reuse a msg buffer without zeroing
 * carry a stale *output* value in the hint field, and a handle output is
 * never a valid declaration — so no legacy pattern can accidentally declare a
 * slot.  (EP_CALL rejects handle values itself, keeping its historical
 * INVALID_ARG contract for that field.)
 *
 * Stage 4: `declared` is a full CPtr, not a direct index into the root CNode.
 * A process whose root is full — which iris_test and every spawner reach —
 * could otherwise not receive a capability at all; now it declares a slot in
 * a second-level CNode and the traversal finds it.
 *
 * Fail-fast contract: on error the endpoint has NOT been touched, so a
 * queued sender keeps its staged cap and nothing is consumed.
 *   - unresolvable / out-of-range CPtr → IRIS_ERR_INVALID_ARG
 *   - slot already occupied            → IRIS_ERR_ALREADY_EXISTS
 *   - process has no root CNode        → IRIS_ERR_NOT_FOUND
 */
iris_error_t syscall_ipc_recv_slot_declare(struct task *t, uint32_t declared) {
    t->ep_recv_slot = 0;
    if (!cspace_value_is_cptr((iris_cptr_t)declared)) return IRIS_OK;

    struct KCNode *cn; uint32_t idx;
    iris_error_t e = cspace_resolve_dest_slot(t->cspace_root, (iris_cptr_t)declared,
                                              &cn, &idx);
    if (e == IRIS_ERR_NOT_FOUND && !t->cspace_root)
        return IRIS_ERR_NOT_FOUND;
    if (e != IRIS_OK) return IRIS_ERR_INVALID_ARG;

    /* Occupancy probe: fetch returns NOT_FOUND for an empty in-range slot.
     * TOCTOU between here and delivery is handled by the exclusive install
     * in the routed delivery, which fails the delivery closed. */
    struct KObject *probe;
    iris_rights_t   pr;
    e = kcnode_fetch(cn, idx, &probe, &pr);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    if (e == IRIS_OK) {
        kobject_active_release(probe);
        kobject_release(probe);
        return IRIS_ERR_ALREADY_EXISTS;
    }
    if (e != IRIS_ERR_NOT_FOUND) return IRIS_ERR_INVALID_ARG;

    t->ep_recv_slot = declared;
    return IRIS_OK;
}

/*
 * syscall_ipc_deliver_cap_routed — deliver a staged cap honouring the
 * receiver's declared receive-slot.  Consumes the declaration.  If the slot
 * install races (filled since declaration) or the root CNode is gone, the
 * cap falls back to handle materialization — a delivery-location
 * degradation that loses no authority and installs no partial state.
 * Returns the msg discriminator: 0 = no cap (or destroyed on soft failure),
 * a CPtr (handle tag bit clear) = CSpace slot, a handle value = handle.
 * Reply caps never come through here — an EP_CALL's reply capability is the
 * CPtr the RECEIVER passed to EP_RECV, echoed back to it (Phase S1), so it was
 * never a handle in the first place.
 */
uint32_t syscall_ipc_deliver_cap_routed(struct task *receiver,
                                        struct KObject *xo,
                                        uint32_t cap_rights, uint64_t badge,
                                        struct KCNode *src_cn, uint32_t src_idx) {
    if (!xo) return IRIS_MSG_NO_CAP;

    uint32_t slot = receiver->ep_recv_slot;
    if (cspace_value_is_cptr((iris_cptr_t)slot)) {
        receiver->ep_recv_slot = 0;   /* one delivery consumes the declaration */
        /* Stage 4: the declaration is a CPtr, so the destination is found by
         * traversal — it need not be a direct slot of the root CNode. */
        struct KCNode *cn; uint32_t idx;
        iris_error_t e = cspace_resolve_dest_slot(receiver->cspace_root,
                                                  (iris_cptr_t)slot, &cn, &idx);
        if (e == IRIS_OK) {
            /* Phase S4 (Step 2): install as an MDB CHILD of the sender's
             * source slot — real CSpace ancestry, no LEGACY_ROOT.  The
             * source must still be occupied: a cap revoked while staged
             * makes this fail, and the cap is NOT delivered (roadmap
             * invariant 4).  The TOCTOU slot→handle degradation is gone
             * (charter §3.7): an occupied/raced destination slot now fails
             * the delivery instead of silently landing in the handle table. */
            e = kcnode_slot_install_linked(
                    cn, idx, xo, (iris_rights_t)cap_rights,
                    badge, src_cn, src_idx,
                    /*exclusive*/1, /*legacy*/0);
            kobject_active_release(&cn->base);
            kobject_release(&cn->base);
            if (e == IRIS_OK) {
                kobject_release(xo);  /* staging ref; the slot holds its own */
                ipc_stat_bump(&iris_ipc_stat_slot_deliveries);
                kcnode_cdt_note_ipc_transfer();
                return slot;
            }
        }
        /* No usable destination: fail closed.  The staged ref is dropped and
         * the message is delivered without a capability; the sender's source
         * slot is left intact by its abort path. */
        kobject_release(xo);
        return IRIS_MSG_NO_CAP;
    }
    /* Stage 4: no declaration means no destination.  The staged reference is
     * dropped and the message is delivered without a capability. */
    kobject_release(xo);
    return IRIS_MSG_NO_CAP;
}

/*
 * ep_send_fastpath — no-cap, no-bulk rendezvous fast path.
 * Precondition: t->ipc_msg already read from user; buf_len==0 and
 * attached_handle==IRIS_MSG_NO_CAP verified by caller.
 * Returns 1 (IPC done, ep released) or 0 (fall through to slow path).
 */
static int ep_send_fastpath(struct task *t, struct KEndpoint *ep) {
    uint64_t fl = irq_spinlock_lock(&ep->lock);
    if (ep->closed || ep->ep_state != EP_STATE_RECV) {
        irq_spinlock_unlock(&ep->lock, fl);
        return 0;
    }
    struct task *receiver = ep->queue_head;
    ep->queue_head = receiver->ep_next;
    if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
    receiver->ep_next = 0;
    receiver->blocking_ep = 0;

    irismsg_copy64(&receiver->ipc_msg, &t->ipc_msg);
    receiver->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
    receiver->ipc_msg_ready           = 1;
    receiver->ipc_kbuf_len            = 0;

    irq_spinlock_unlock(&ep->lock, fl);
    task_wakeup(receiver);
    kobject_release(&ep->base);
    return 1;
}

/*
 * ep_recv_fastpath — no-cap, no-bulk, non-EP_CALL rendezvous fast path.
 * Returns 1 and fills t->ipc_msg if a matching sender is ready;
 * does NOT release ep (caller handles it). Returns 0 to fall through.
 */
static int ep_recv_fastpath(struct task *t, struct KEndpoint *ep) {
    uint64_t fl = irq_spinlock_lock(&ep->lock);
    if (ep->closed || ep->ep_state != EP_STATE_SEND) {
        irq_spinlock_unlock(&ep->lock, fl);
        return 0;
    }
    struct task *sender = ep->queue_head;
    if (sender->ep_cap_obj || sender->ipc_kbuf_len || sender->ep_call_mode) {
        irq_spinlock_unlock(&ep->lock, fl);
        return 0;
    }
    ep->queue_head = sender->ep_next;
    if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
    sender->ep_next = 0;
    sender->blocking_ep = 0;

    irismsg_copy64(&t->ipc_msg, &sender->ipc_msg);
    t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
    t->ipc_kbuf_len            = 0;
    sender->ipc_kbuf_len       = 0;

    irq_spinlock_unlock(&ep->lock, fl);
    task_wakeup(sender);
    return 1;
}

/* ── SYS_ENDPOINT_CREATE ─────────────────────────────────────────────── */

/*
 * Phase S1: SYS_ENDPOINT_CREATE (74) is RETIRED — endpoints are created ONLY
 * via SYS_UNTYPED_RETYPE2 (storage inside the source Untyped, capability
 * directly in CSpace).  The number stays reserved; the path creates nothing.
 */
uint64_t sys_endpoint_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

/* ── SYS_EP_SEND ─────────────────────────────────────────────────────── */

static uint64_t ep_send_complete(struct task *t);

uint64_t sys_ep_send(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Stage 9-evt Step 1: a re-execution runs only the completion — the
     * message is delivered or the endpoint closed, and every effect this
     * syscall had is already done. */
    if (t->sc_reentry) return ep_send_complete(t);

    if (!user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    uint64_t ep_badge = 0;
    iris_error_t err = cspace_resolve_only_endpoint_badged(t->cspace_root, (iris_cptr_t)arg0, RIGHT_WRITE, &ep, &_ep_r, &ep_badge);
    if (err != IRIS_OK) return syscall_err(err);

    /* Copy sender's message. */
    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Phase 9: STAMP the sender badge from the invoked capability — whatever
     * the sender wrote in the field is discarded (anti-spoofing). */
    t->ipc_msg.sender_badge = ep_badge;

    /* Fastpath: no cap, no bulk buffer, receiver already waiting. */
    if (t->ipc_msg.attached_handle == IRIS_MSG_NO_CAP && t->ipc_msg.buf_len == 0)
        if (ep_send_fastpath(t, ep))
            return syscall_ok_u64(0);

    /* Ph69/D-4: make the bulk payload readable by the kernel. */
    if (ipc_stage_out(t) != IRIS_OK) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Ph68: validate and stage attached cap before taking the spinlock.
     * A1.10: PEEK only (two-phase, same as EP_NB_SEND since A1.9) — the
     * sender's handle is consumed at a commit point only once a receiver
     * is determined: here for an immediate rendezvous, or by the receiver
     * when it takes the staged cap from a queued sender.  CLOSED before
     * delivery / endpoint close / cancel leave the sender's cap untouched. */
    struct KObject *xfer_obj    = 0;
    uint32_t        xfer_rights = 0;
    uint64_t        xfer_badge  = 0;
    struct KCNode  *xfer_src_cn = 0;
    uint32_t        xfer_src_idx = 0;
    if (t->ipc_msg.attached_handle != IRIS_MSG_NO_CAP) {
        iris_error_t cr = syscall_ipc_stage_cap_peek_badged(t, t->ipc_msg.attached_handle,
                                         t->ipc_msg.attached_rights,
                                         &xfer_obj, &xfer_rights, &xfer_badge,
                                         &xfer_src_cn, &xfer_src_idx);
        if (cr != IRIS_OK) {
            kobject_release(&ep->base);
            return syscall_err(cr);
        }
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP; /* cleared in staged msg */
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) {                            /* source slot NOT consumed */
            kobject_release(xfer_obj);
            syscall_ipc_stage_cap_abort(xfer_src_cn);
        }
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state == EP_STATE_RECV) {
        /* Rendezvous: a receiver is already waiting. */
        struct task *receiver = ep->queue_head;
        ep->queue_head = receiver->ep_next;
        if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
        receiver->ep_next     = 0;
        receiver->blocking_ep = 0;

        irismsg_copy64(&receiver->ipc_msg, &t->ipc_msg);
        receiver->ipc_msg.attached_handle = IRIS_MSG_NO_CAP; /* will update after unlock */
        receiver->ipc_msg_ready           = 1;

        /* Ph69/D-4: hand the payload over.  The receiver is not current, so
         * its own address space is unreachable — a registered frame is not
         * (the kernel window is mapped everywhere), which is why this is one
         * call rather than two cases. */
        ipc_transfer_bulk(t, receiver, 0);

        irq_spinlock_unlock(&ep->lock, flags);

        /* Ph68: install cap (outside lock).  A1.5: routed — lands in the
         * receiver's declared receive-slot (CPtr) or its handle table.
         * Phase S4 (Step 2): DELIVER FIRST, then commit — the MDB parenting
         * requires the source slot to still be occupied, so the source is
         * consumed only after the child cap exists (move semantics
         * preserved: delete reparents the delivered cap to the grandparent). */
        if (xfer_obj) {
            uint32_t new_h = syscall_ipc_deliver_cap_routed(receiver, xfer_obj,
                                                            xfer_rights, xfer_badge,
                                                            xfer_src_cn, xfer_src_idx);
            receiver->ipc_msg.attached_handle = new_h;
            if (new_h != IRIS_MSG_NO_CAP)
                syscall_ipc_stage_cap_commit(t, xfer_src_cn, xfer_src_idx);
            else
                syscall_ipc_stage_cap_abort(xfer_src_cn);
        }

        /* Wake receiver only after all data is consistent. */
        task_wakeup(receiver);
        kobject_release(&ep->base);
        return syscall_ok_u64(0);
    }

    /* No receiver: stage cap in task and block.  A1.10 / Phase S4: the source
     * SLOT rides along un-consumed (with its CNode refs); the receiver
     * commits it at take time, cancel paths abort it. */
    ep->ep_state     = EP_STATE_SEND;
    t->ep_next       = 0;
    t->blocking_ep   = ep;
    t->ipc_msg_ready = 0;
    t->ipc_ep_closed = 0;
    t->ep_cap_obj    = xfer_obj;    /* staging ref; released by receiver or cancel */
    t->ep_cap_rights = xfer_rights;
    t->ep_cap_badge  = xfer_badge;
    t->ep_cap_src_cn  = xfer_obj ? xfer_src_cn : 0;
    t->ep_cap_src_idx = xfer_obj ? xfer_src_idx : 0;

    if (ep->queue_tail) { ep->queue_tail->ep_next = t; ep->queue_tail = t; }
    else                { ep->queue_head = t; ep->queue_tail = t; }

    t->state = TASK_BLOCKED_SEND;
    irq_spinlock_unlock(&ep->lock, flags);

    /*
     * Stage 9-evt Step 1 — park and ask to be RE-EXECUTED (ledger D-1).
     *
     * A queued sender's whole continuation is already thread state: the
     * message, the staged capability and its source slot, the bulk payload,
     * and the closed marker.  The receiver takes them from the thread at
     * rendezvous, not from this frame — which is why the frame only ever held
     * the endpoint reference, and why moving that to the thread is the entire
     * conversion.
     */
    t->sc_held = &ep->base;
    syscall_request_restart(t);
    return 0;
}

/*
 * The half of SYS_EP_SEND that runs after the block.
 *
 * Note what it does NOT do: re-stage the capability, re-copy the message, or
 * re-queue.  A restartable handler must not repeat the part of itself that had
 * effects, and a send that got as far as queueing has already consumed the
 * sender's source slot on the delivery path or left it for this function to
 * abort.  Repeating any of it would transfer a capability twice.
 */
static uint64_t ep_send_complete(struct task *t) {
    if (t->sc_held) { kobject_release(t->sc_held); t->sc_held = 0; }

    /* Phase S4 (Step 2): if the endpoint closed under us, kendpoint_obj_close
     * left our source-slot refs for us to drop (it could not release them
     * under ep->lock).  Nothing was delivered — the slot itself survives. */
    if (t->ep_cap_src_cn) {
        syscall_ipc_stage_cap_abort(t->ep_cap_src_cn);
        t->ep_cap_src_cn  = 0;
        t->ep_cap_src_idx = 0;
    }

    if (t->ipc_ep_closed) { t->ipc_ep_closed = 0; return syscall_err(IRIS_ERR_CLOSED); }
    return syscall_ok_u64(0);
}

/* ── SYS_EP_RECV ─────────────────────────────────────────────────────── */

/*
 * Phase S1 — explicit reply staging (receiver side).
 *
 * ep_recv_reply_stage: resolve the reply CPtr the receiver passed as arg2
 * (RIGHT_WRITE) and take the object's exclusive staged claim.  The task keeps
 * the resolve lifecycle ref in t->ep_reply_obj until the recv either binds
 * the object to an EP_CALL caller (ref transferred to pending_kreply) or
 * concludes without a call (ep_recv_reply_unstage).
 */
static iris_error_t ep_recv_reply_stage(struct task *t, uint64_t reply_arg) {
    t->ep_reply_obj = 0;
    t->ep_reply_val = 0;
    if (reply_arg == 0u) return IRIS_OK;

    struct KReply *rp; iris_rights_t rp_r;
    iris_error_t err = cspace_resolve_only_reply(t->cspace_root,
                            (iris_cptr_t)reply_arg, RIGHT_WRITE, &rp, &rp_r);
    if (err != IRIS_OK) return err;
    err = kreply_stage(rp);
    if (err != IRIS_OK) {
        kobject_release(&rp->base);
        return err;
    }
    t->ep_reply_obj = rp;
    t->ep_reply_val = (uint32_t)reply_arg;
    return IRIS_OK;
}

static void ep_recv_reply_unstage(struct task *t) {
    struct KReply *rp = t->ep_reply_obj;
    if (!rp) return;
    t->ep_reply_obj = 0;
    t->ep_reply_val = 0;
    kreply_unstage(rp);
    kobject_release(&rp->base);
}

/*
 * ep_bind_call_reply — rendezvous helper: bind the receiver's staged reply
 * object to the call-mode sender.  Returns 1 on success (sender must stay
 * blocked as TASK_BLOCKED_REPLY; receiver's msg.attached_handle = the reply
 * CPtr it passed), 0 when no usable reply object is staged (implicit KReply
 * fabrication is RETIRED — the kernel never allocates one here).
 */
static int ep_bind_call_reply(struct task *receiver, struct task *sender,
                              uint32_t *attached_out) {
    struct KReply *rp = receiver->ep_reply_obj;
    if (!rp) return 0;
    if (kreply_bind_caller(rp, sender) != IRIS_OK) return 0;

    /*
     * Stage 8-mcs — DONATE the caller's scheduling context to a PASSIVE
     * server.  A server with no SC of its own runs on the requester's time,
     * which is seL4's model: time is an authority a client delegates, not a
     * property the server was born with.  A passive server therefore cannot be
     * starved into uselessness, and cannot be spun by a client with no budget
     * to give.
     *
     * Only when the receiver has none: a server already holding a donation (an
     * earlier call it has not answered) keeps it, so a second client can never
     * displace the first client's time.  Nothing is double-spent — budget is
     * charged only to a RUNNING thread and the caller is about to block in
     * TASK_BLOCKED_REPLY for the whole window.
     *
     * The record lives on the reply object because the reply is what ends the
     * loan, and every way it can end goes through kreply_return_donation.
     */
    kreply_donate_on_call(rp, sender, receiver);

    /* Staging lifecycle ref transfers to sender->pending_kreply. */
    sender->pending_kreply = rp;
    *attached_out          = receiver->ep_reply_val;
    receiver->ep_reply_obj = 0;
    receiver->ep_reply_val = 0;
    return 1;
}

/* Forward: the post-block half, defined with the parking path it belongs to. */
static uint64_t ep_recv_complete(struct task *t, uint64_t arg1);

uint64_t sys_ep_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    /*
     * Stage 9-evt Step 1: a re-execution after the park runs ONLY the
     * completion.  Re-doing the setup would re-stage a reply object that is
     * already staged (IRIS_ERR_BUSY) and re-declare a receive slot the
     * rendezvous has already consumed — a restartable handler must not repeat
     * the part of itself that had effects.
     */
    if (t->sc_reentry)
        return ep_recv_complete(t, arg1);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    iris_error_t err = cspace_resolve_only_endpoint(t->cspace_root, (iris_cptr_t)arg0,
                                                          RIGHT_READ, &ep, &_ep_r);
    if (err != IRIS_OK) return syscall_err(err);

    /* Ph69: read receiver's hints (buf_uptr = where to put bulk data).
     * A1.5: attached_cap is a second hint — the receive-slot declaration. */
    t->ep_recv_buf_uptr = 0;
    t->ipc_kbuf_len     = 0;
    t->ep_recv_slot     = 0;
    {
        struct IrisMsg hints;
        if (user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)) &&
            copy_from_user_checked(&hints, arg1, (uint32_t)sizeof(struct IrisMsg))) {
            t->ep_recv_buf_uptr = hints.buf_uptr;
            /* Fail-fast: a bad slot declaration fails BEFORE the endpoint is
             * touched — a queued sender keeps its staged cap untouched. */
            err = syscall_ipc_recv_slot_declare(t, hints.attached_cap);
            if (err != IRIS_OK) {
                kobject_release(&ep->base);
                return syscall_err(err);
            }
        }
    }

    /* Phase S1: stage the explicit reply object named by arg2 (0 = none).
     * Fail-fast: a bad reply CPtr fails BEFORE the endpoint is touched. */
    err = ep_recv_reply_stage(t, arg2);
    if (err != IRIS_OK) {
        kobject_release(&ep->base);
        return syscall_err(err);
    }

    /* Fastpath: no-cap, no-bulk, non-EP_CALL sender already waiting. */
    if (ep_recv_fastpath(t, ep)) {
        t->ep_recv_slot = 0;   /* no cap on the fastpath — drop the declaration */
        ep_recv_reply_unstage(t);
        kobject_release(&ep->base);
        if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
            return syscall_err(IRIS_ERR_INVALID_ARG);
        return syscall_ok_u64(0);
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        ep_recv_reply_unstage(t);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state == EP_STATE_SEND) {
        /* Rendezvous: a sender is already waiting. */
        struct task *sender = ep->queue_head;

        /* Phase S1: a call-mode sender REQUIRES an explicit reply object.
         * Refuse the recv before anything is consumed — the sender stays
         * queued and keeps its staged cap; the receiver is told to supply
         * reply authority (implicit KReply fabrication is retired). */
        if (sender->ep_call_mode && !t->ep_reply_obj) {
            irq_spinlock_unlock(&ep->lock, flags);
            kobject_release(&ep->base);
            return syscall_err(IRIS_ERR_NOT_SUPPORTED);
        }

        ep->queue_head = sender->ep_next;
        if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
        sender->ep_next     = 0;
        sender->blocking_ep = 0;

        irismsg_copy64(&t->ipc_msg, &sender->ipc_msg);
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP; /* will update after unlock */

        /* Ph69/D-4: the receiver is in its own CR3, so the staging path may
         * write to the buffer it named at EP_RECV. */
        ipc_transfer_bulk(sender, t, 1);

        /* Ph68: take sender's staged cap. */
        struct KObject *xfer_obj     = sender->ep_cap_obj;
        uint32_t        xfer_rights  = sender->ep_cap_rights;
        uint64_t        xfer_badge   = sender->ep_cap_badge;
        struct KCNode  *xfer_src_cn  = sender->ep_cap_src_cn;
        uint32_t        xfer_src_idx = sender->ep_cap_src_idx;
        sender->ep_cap_obj     = 0;
        sender->ep_cap_rights  = 0;
        sender->ep_cap_badge   = 0;
        sender->ep_cap_src_cn  = 0;
        sender->ep_cap_src_idx = 0;

        irq_spinlock_unlock(&ep->lock, flags);

        /* Ph68/Fase11: install sender's transferred cap.  For an EP_CALL the
         * reply cap takes attached_handle, so the transferred cap is delivered
         * into the separate attached_cap field; EP_SEND keeps attached_handle.
         * A1.5: routed — honours our declared receive-slot (CPtr < 1024).
         * Phase S4 (Step 2): deliver first (MDB child of the sender's source
         * slot), then consume that slot.  Outside ep->lock: slot delete can
         * fire object close callbacks that take endpoint locks (cn->lock →
         * ep->lock ordering must not invert). */
        t->ipc_msg.attached_cap = IRIS_MSG_NO_CAP;
        if (xfer_obj) {
            uint32_t new_h = syscall_ipc_deliver_cap_routed(t, xfer_obj,
                                                            xfer_rights, xfer_badge,
                                                            xfer_src_cn, xfer_src_idx);
            if (sender->ep_call_mode) {
                t->ipc_msg.attached_cap        = new_h;
                t->ipc_msg.attached_cap_rights = xfer_rights;
            } else {
                t->ipc_msg.attached_handle = new_h;
            }
            if (new_h != IRIS_MSG_NO_CAP)
                syscall_ipc_stage_cap_commit(sender, xfer_src_cn, xfer_src_idx);
            else
                syscall_ipc_stage_cap_abort(xfer_src_cn);
        }
        t->ep_recv_slot = 0;   /* declaration is per-recv; never outlives it */

        /* Ph85/Phase S1: if sender used EP_CALL, bind the receiver's staged
         * explicit reply object and keep the sender blocked.  The kernel no
         * longer fabricates a KReply here — the availability of the staged
         * object was verified before the sender was dequeued, so the bind
         * cannot fail on this non-preemptive path (defensive fallback wakes
         * the sender with CLOSED). */
        if (sender->ep_call_mode) {
            sender->ep_call_mode = 0u;
            uint32_t reply_attach = IRIS_MSG_NO_CAP;
            if (ep_bind_call_reply(t, sender, &reply_attach)) {
                ipc_stat_bump(&iris_ipc_stat_reply_caps);
                t->ipc_msg.attached_handle = reply_attach;
                sender->state = TASK_BLOCKED_REPLY;
            } else {
                sender->ipc_ep_closed = 1u;
                task_wakeup(sender);
            }
        } else {
            task_wakeup(sender);
        }

        ep_recv_reply_unstage(t);   /* plain send: staged reply stays unused */
        kobject_release(&ep->base);

        if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
            return syscall_err(IRIS_ERR_INVALID_ARG);
        return syscall_ok_u64(0);
    }

    /* No sender: enqueue receiver and block. */
    ep->ep_state     = EP_STATE_RECV;
    t->ep_next       = 0;
    t->blocking_ep   = ep;
    t->ipc_msg_ready = 0;
    t->ipc_ep_closed = 0;

    if (ep->queue_tail) { ep->queue_tail->ep_next = t; ep->queue_tail = t; }
    else                { ep->queue_head = t; ep->queue_tail = t; }

    t->state = TASK_BLOCKED_RECV;
    irq_spinlock_unlock(&ep->lock, flags);

    /*
     * Stage 9-evt Step 1 — park and ask to be RE-EXECUTED (ledger D-1).
     *
     * Everything the completion below needs is already thread state: the
     * delivered message, the staged reply object, the receive-slot
     * declaration, the bulk-payload staging and the closed marker.  The
     * handler was keeping only ONE thing in a C local that mattered — the
     * endpoint reference — because releasing it here would let the object be
     * freed while the endpoint's queue still points at this thread.
     *
     * So the reference moves to the thread and the frame becomes disposable.
     * `blocking_ep` cannot hold it: the wakers clear that field by design.
     */
    t->sc_held = &ep->base;
    syscall_request_restart(t);
    return 0;
}

/*
 * The half of SYS_EP_RECV that runs after the block.
 *
 * Reads nothing but thread state, which is what made EP_RECV convertible at
 * all: the sender writes the message into the receiver's `ipc_msg` at
 * rendezvous, so a receiver's continuation was already a fact about the
 * thread rather than about its stack — the parked frame was holding a
 * reference, not information.
 */
static uint64_t ep_recv_complete(struct task *t, uint64_t arg1) {
    if (t->sc_held) { kobject_release(t->sc_held); t->sc_held = 0; }

    /* A1.5: any routed delivery already consumed the declaration from the
     * sender's context; make sure it never survives this recv either way. */
    t->ep_recv_slot = 0;

    /* Phase S1: if the rendezvous did not consume the staged reply object
     * (plain send, closed endpoint, cancel) release the claim now. */
    ep_recv_reply_unstage(t);

    if (t->ipc_ep_closed) { t->ipc_ep_closed = 0; return syscall_err(IRIS_ERR_CLOSED); }

    /* Ph69: copy staged kbuf to receiver's user space now that we're in receiver's CR3. */
    if (t->ipc_kbuf_len > 0 && t->ep_recv_buf_uptr != 0 &&
        user_range_writable(t->ep_recv_buf_uptr, t->ipc_kbuf_len) &&
        copy_to_user_checked(t->ep_recv_buf_uptr, t->ipc_kbuf, t->ipc_kbuf_len)) {
        t->ipc_msg.buf_len  = t->ipc_kbuf_len;
        t->ipc_msg.buf_uptr = t->ep_recv_buf_uptr;
    } else if (t->ipc_kbuf_len > 0) {
        t->ipc_msg.buf_len = t->ipc_kbuf_len; /* report available, even if unwritten */
    }
    t->ipc_kbuf_len     = 0;
    t->ep_recv_buf_uptr = 0;

    if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(0);
}

/* ── SYS_EP_NB_SEND ──────────────────────────────────────────────────── */

uint64_t sys_ep_nb_send(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    uint64_t ep_badge = 0;
    iris_error_t err = cspace_resolve_only_endpoint_badged(t->cspace_root, (iris_cptr_t)arg0, RIGHT_WRITE, &ep, &_ep_r, &ep_badge);
    if (err != IRIS_OK) return syscall_err(err);

    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Phase 9: stamp the sender badge from the invoked cap (anti-spoofing). */
    t->ipc_msg.sender_badge = ep_badge;

    /* Ph69/D-4: make the bulk payload readable by the kernel. */
    if (ipc_stage_out(t) != IRIS_OK) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Ph68: stage cap before taking lock.  A1.9: PEEK only — the sender's
     * handle is consumed at the commit point below, so a send that fails
     * with CLOSED / WOULD_BLOCK leaves the sender's cap untouched (A1.5
     * atomicity rule: the source cap is never consumed by a failed
     * delivery).  The peeked object ref is released on those exits. */
    struct KObject *xfer_obj     = 0;
    uint32_t        xfer_rights  = 0;
    uint64_t        xfer_badge   = 0;
    struct KCNode  *xfer_src_cn  = 0;
    uint32_t        xfer_src_idx = 0;
    if (t->ipc_msg.attached_handle != IRIS_MSG_NO_CAP) {
        iris_error_t cr = syscall_ipc_stage_cap_peek_badged(t, t->ipc_msg.attached_handle,
                                         t->ipc_msg.attached_rights,
                                         &xfer_obj, &xfer_rights, &xfer_badge,
                                         &xfer_src_cn, &xfer_src_idx);
        if (cr != IRIS_OK) { kobject_release(&ep->base); return syscall_err(cr); }
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) {                            /* source slot NOT consumed */
            kobject_release(xfer_obj);
            syscall_ipc_stage_cap_abort(xfer_src_cn);
        }
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state != EP_STATE_RECV) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) {                            /* source slot NOT consumed */
            kobject_release(xfer_obj);
            syscall_ipc_stage_cap_abort(xfer_src_cn);
        }
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_WOULD_BLOCK);
    }

    struct task *receiver = ep->queue_head;
    ep->queue_head = receiver->ep_next;
    if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
    receiver->ep_next     = 0;
    receiver->blocking_ep = 0;

    irismsg_copy64(&receiver->ipc_msg, &t->ipc_msg);
    receiver->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
    receiver->ipc_msg_ready           = 1;

    ipc_transfer_bulk(t, receiver, 0);

    irq_spinlock_unlock(&ep->lock, flags);

    /* A1.5: routed — receiver's declared receive-slot or handle table.
     * Phase S4 (Step 2): deliver first (MDB child of the source slot), then
     * consume the source slot (move semantics preserved). */
    if (xfer_obj) {
        uint32_t new_h = syscall_ipc_deliver_cap_routed(receiver, xfer_obj,
                                                        xfer_rights, xfer_badge,
                                                        xfer_src_cn, xfer_src_idx);
        receiver->ipc_msg.attached_handle = new_h;
        if (new_h != IRIS_MSG_NO_CAP)
            syscall_ipc_stage_cap_commit(t, xfer_src_cn, xfer_src_idx);
        else
            syscall_ipc_stage_cap_abort(xfer_src_cn);
    }

    task_wakeup(receiver);
    kobject_release(&ep->base);
    return syscall_ok_u64(0);
}

/* ── SYS_EP_NB_RECV ──────────────────────────────────────────────────── */

uint64_t sys_ep_nb_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    iris_error_t err = cspace_resolve_only_endpoint(t->cspace_root, (iris_cptr_t)arg0,
                                                          RIGHT_READ, &ep, &_ep_r);
    if (err != IRIS_OK) return syscall_err(err);

    /* Ph69: read receiver's hint.  A1.5: attached_cap declares a receive-slot.
     * D-4: the hint lands in `ep_recv_buf_uptr` rather than a local, because
     * that is the one field ipc_transfer_bulk consults for a thread with no
     * registered IPC buffer — a receiver should not name its destination two
     * different ways depending on which recv syscall it used. */
    t->ep_recv_buf_uptr = 0;
    t->ep_recv_slot = 0;
    {
        struct IrisMsg hints;
        if (user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)) &&
            copy_from_user_checked(&hints, arg1, (uint32_t)sizeof(struct IrisMsg))) {
            t->ep_recv_buf_uptr = hints.buf_uptr;
            err = syscall_ipc_recv_slot_declare(t, hints.attached_cap);
            if (err != IRIS_OK) {
                kobject_release(&ep->base);
                return syscall_err(err);
            }
        }
    }

    /* Phase S1: stage the explicit reply object named by arg2 (0 = none). */
    err = ep_recv_reply_stage(t, arg2);
    if (err != IRIS_OK) {
        kobject_release(&ep->base);
        return syscall_err(err);
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        t->ep_recv_slot = 0;
        ep_recv_reply_unstage(t);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state != EP_STATE_SEND) {
        irq_spinlock_unlock(&ep->lock, flags);
        t->ep_recv_slot = 0;
        ep_recv_reply_unstage(t);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_WOULD_BLOCK);
    }

    struct task *sender = ep->queue_head;

    /* Phase S1: a call-mode sender REQUIRES an explicit reply object (see
     * sys_ep_recv) — refuse before anything is consumed. */
    if (sender->ep_call_mode && !t->ep_reply_obj) {
        irq_spinlock_unlock(&ep->lock, flags);
        t->ep_recv_slot = 0;
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }

    ep->queue_head = sender->ep_next;
    if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
    sender->ep_next     = 0;
    sender->blocking_ep = 0;

    irismsg_copy64(&t->ipc_msg, &sender->ipc_msg);
    t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;

    /* Ph69/D-4: same as above — correct CR3, so the staging path may write. */
    ipc_transfer_bulk(sender, t, 1);

    /* Ph68: take sender's staged cap. */
    struct KObject *xfer_obj     = sender->ep_cap_obj;
    uint32_t        xfer_rights  = sender->ep_cap_rights;
    uint64_t        xfer_badge   = sender->ep_cap_badge;
    struct KCNode  *xfer_src_cn  = sender->ep_cap_src_cn;
    uint32_t        xfer_src_idx = sender->ep_cap_src_idx;
    sender->ep_cap_obj     = 0;
    sender->ep_cap_rights  = 0;
    sender->ep_cap_badge   = 0;
    sender->ep_cap_src_cn  = 0;
    sender->ep_cap_src_idx = 0;

    irq_spinlock_unlock(&ep->lock, flags);

    /* Phase 11: EP_CALL transferred cap → attached_cap; EP_SEND → attached_handle.
     * A1.5: routed — honours our declared receive-slot (CPtr < 1024).
     * Phase S4 (Step 2): deliver first, then consume the source slot
     * (outside ep->lock — see sys_ep_recv). */
    t->ipc_msg.attached_cap = IRIS_MSG_NO_CAP;
    if (xfer_obj) {
        uint32_t new_h = syscall_ipc_deliver_cap_routed(t, xfer_obj,
                                                        xfer_rights, xfer_badge,
                                                        xfer_src_cn, xfer_src_idx);
        if (sender->ep_call_mode) {
            t->ipc_msg.attached_cap        = new_h;
            t->ipc_msg.attached_cap_rights = xfer_rights;
        } else {
            t->ipc_msg.attached_handle = new_h;
        }
        if (new_h != IRIS_MSG_NO_CAP)
            syscall_ipc_stage_cap_commit(sender, xfer_src_cn, xfer_src_idx);
        else
            syscall_ipc_stage_cap_abort(xfer_src_cn);
    }
    t->ep_recv_slot = 0;   /* declaration is per-recv; never outlives it */

    /* Ph85/Phase S1: ep_call_mode senders block until replied to — bind the
     * receiver's staged explicit reply object (no implicit KReply). */
    if (sender->ep_call_mode) {
        sender->ep_call_mode = 0u;
        uint32_t reply_attach = IRIS_MSG_NO_CAP;
        if (ep_bind_call_reply(t, sender, &reply_attach)) {
            ipc_stat_bump(&iris_ipc_stat_reply_caps);
            t->ipc_msg.attached_handle = reply_attach;
            sender->state = TASK_BLOCKED_REPLY;
        } else {
            sender->ipc_ep_closed = 1u;
            task_wakeup(sender);
        }
    } else {
        task_wakeup(sender);
    }

    ep_recv_reply_unstage(t);   /* plain send: staged reply stays unused */
    kobject_release(&ep->base);

    if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(0);
}
