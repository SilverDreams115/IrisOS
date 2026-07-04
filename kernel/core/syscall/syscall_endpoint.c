#include "syscall_priv.h"
#include <iris/nc/kendpoint.h>
#include <iris/nc/kreply.h>
#include <iris/ipc_msg.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

/* IrisMsg = 72 bytes = 9 × uint64_t (Fase 9: +sender_badge) — word copy
 * avoids byte-loop overhead. */
static inline void irismsg_copy64(struct IrisMsg *dst, const struct IrisMsg *src) {
    const uint64_t *s = (const uint64_t *)src;
    uint64_t       *d = (uint64_t *)dst;
    d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
    d[4]=s[4]; d[5]=s[5]; d[6]=s[6]; d[7]=s[7];
    d[8]=s[8]; d[9]=s[9];          /* Fase 11: sender_badge + attached_cap pair */
    _Static_assert(sizeof(struct IrisMsg) == 10u * sizeof(uint64_t),
                   "irismsg_copy64 word count");
}

static inline void copy_kbuf(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

/* ep_get removed — use cspace_or_handle_resolve_endpoint (Fase 3.2) */

/*
 * syscall_ipc_stage_cap — validate and detach an attached cap from the sender's
 * handle table, storing the kobject + rights for later delivery to a receiver.
 * On success: sender's handle is closed; *out_obj holds the ownership ref.
 * On failure: *out_obj = NULL; caller's handle is untouched; returns error.
 * Shared by EP_SEND / EP_NB_SEND / SYS_REPLY (declared in syscall_priv.h).
 */
iris_error_t syscall_ipc_stage_cap_badged(struct task *t, uint32_t src_h,
                                          uint32_t requested_rights,
                                          struct KObject **out_obj,
                                          uint32_t *out_rights,
                                          uint64_t *out_badge) {
    struct KObject *xo;
    iris_rights_t   xr;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)src_h, &xo, &xr);
    if (r != IRIS_OK) return r;
    if (!rights_check(xr, RIGHT_TRANSFER)) { kobject_release(xo); return IRIS_ERR_ACCESS_DENIED; }

    iris_rights_t cap_rights = rights_reduce(xr, (iris_rights_t)requested_rights);
    if (cap_rights == RIGHT_NONE) { kobject_release(xo); return IRIS_ERR_INVALID_ARG; }

    /* Fase 9: the transferred cap keeps its badge across the transfer. */
    if (out_badge)
        *out_badge = handle_table_get_badge(&t->process->handle_table,
                                            (handle_id_t)src_h);

    /* Close sender's handle; the get_object lifecycle ref becomes the staging ref. */
    handle_table_close(&t->process->handle_table, (handle_id_t)src_h);
    *out_obj    = xo;
    *out_rights = (uint32_t)cap_rights;
    return IRIS_OK;
}

iris_error_t syscall_ipc_stage_cap(struct task *t, uint32_t src_h,
                                   uint32_t requested_rights,
                                   struct KObject **out_obj,
                                   uint32_t *out_rights) {
    return syscall_ipc_stage_cap_badged(t, src_h, requested_rights,
                                        out_obj, out_rights, 0);
}

/*
 * syscall_ipc_deliver_cap — install a staged cap into the receiver's handle
 * table. Releases the staging ref regardless of success.
 * Returns HANDLE_INVALID on failure (table full); caller should treat it as
 * a soft error and deliver the message without the capability.
 * Shared by EP_SEND / EP_NB_SEND / SYS_REPLY (declared in syscall_priv.h).
 */
uint32_t syscall_ipc_deliver_cap_badged(struct task *receiver,
                                        struct KObject *xo,
                                        uint32_t cap_rights, uint64_t badge) {
    if (!xo) return IRIS_MSG_NO_CAP;
    handle_id_t new_h = handle_table_insert_badged(
        &receiver->process->handle_table, xo, (iris_rights_t)cap_rights, badge);
    kobject_release(xo); /* release staging ref; table holds its own ref */
    return (uint32_t)new_h;
}

uint32_t syscall_ipc_deliver_cap(struct task *receiver,
                                 struct KObject *xo, uint32_t cap_rights) {
    return syscall_ipc_deliver_cap_badged(receiver, xo, cap_rights, 0);
}

/* ── A1.5: receive-slot support ──────────────────────────────────────── */

/* Resolve a process's root CNode (lifecycle ref only; release with a single
 * kobject_release).  NULL if the process has no root CNode (OOM-degraded). */
static struct KCNode *ipc_root_cnode_of(struct KProcess *proc) {
    struct KObject *obj;
    iris_rights_t   r;
    if (!proc || proc->cspace_root_h == HANDLE_INVALID) return 0;
    if (handle_table_get_object(&proc->handle_table, proc->cspace_root_h,
                                &obj, &r) != IRIS_OK) return 0;
    if (obj->type != KOBJ_CNODE) { kobject_release(obj); return 0; }
    return (struct KCNode *)obj;
}

/*
 * syscall_ipc_recv_slot_declare — validate + record a receive-slot declared
 * by a recv-family syscall (EP_RECV / EP_NB_RECV / EP_CALL).
 *
 * declared == 0 or >= 1024: no declaration (legacy).  Values >= 1024 are
 * IGNORED, not rejected: receivers that reuse a msg buffer without zeroing
 * carry a stale *output* value in the hint field, and outputs are always 0
 * or a handle >= 1024 — never 1..1023 — so no legacy pattern can
 * accidentally declare a slot.  (EP_CALL rejects >= 1024 itself, keeping
 * its historical INVALID_ARG contract for that field.)
 *
 * Fail-fast contract: on error the endpoint has NOT been touched, so a
 * queued sender keeps its staged cap and nothing is consumed.
 *   - out-of-range direct slot        → IRIS_ERR_INVALID_ARG
 *   - slot already occupied           → IRIS_ERR_ALREADY_EXISTS
 *   - process has no root CNode       → IRIS_ERR_NOT_FOUND
 */
iris_error_t syscall_ipc_recv_slot_declare(struct task *t, uint32_t declared) {
    t->ep_recv_slot = 0;
    if (declared == 0u || declared >= 1024u) return IRIS_OK;

    struct KCNode *root = ipc_root_cnode_of(t->process);
    if (!root) return IRIS_ERR_NOT_FOUND;

    /* Occupancy probe: fetch returns NOT_FOUND for an empty in-range slot.
     * TOCTOU between here and delivery is handled by the exclusive install
     * (kcnode_mint_excl_badged) + handle fallback in the routed delivery. */
    struct KObject *probe;
    iris_rights_t   pr;
    iris_error_t e = kcnode_fetch(root, declared, &probe, &pr);
    kobject_release(&root->base);
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
 * < 1024 = CSpace slot CPtr, >= 1024 = handle.  Reply caps never come
 * through here — their handle_table_insert sites are untouched (A1.5).
 */
uint32_t syscall_ipc_deliver_cap_routed(struct task *receiver,
                                        struct KObject *xo,
                                        uint32_t cap_rights, uint64_t badge) {
    if (!xo) return IRIS_MSG_NO_CAP;

    uint32_t slot = receiver->ep_recv_slot;
    if (slot != 0u && slot < 1024u) {
        receiver->ep_recv_slot = 0;   /* one delivery consumes the declaration */
        struct KCNode *root = ipc_root_cnode_of(receiver->process);
        if (root) {
            iris_error_t e = kcnode_mint_excl_badged(root, slot, xo,
                                                     (iris_rights_t)cap_rights,
                                                     badge);
            kobject_release(&root->base);
            if (e == IRIS_OK) {
                kobject_release(xo);  /* staging ref; the slot holds its own */
                return slot;
            }
        }
        /* raced/occupied/no-root → legacy handle materialization below */
    }
    return syscall_ipc_deliver_cap_badged(receiver, xo, cap_rights, badge);
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

uint64_t sys_endpoint_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep = kendpoint_alloc();
    if (!ep) return syscall_err(IRIS_ERR_NO_MEMORY);

    handle_id_t h = handle_table_insert(&t->process->handle_table, &ep->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    kobject_release(&ep->base);
    if (h == HANDLE_INVALID)
        return syscall_err(IRIS_ERR_TABLE_FULL);

    return (uint64_t)h;
}

/* ── SYS_EP_SEND ─────────────────────────────────────────────────────── */

uint64_t sys_ep_send(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    uint64_t ep_badge = 0;
    iris_error_t err = cspace_or_handle_resolve_endpoint_badged(
        t->process, (iris_cptr_t)arg0, RIGHT_WRITE, &ep, &_ep_r, &ep_badge);
    if (err != IRIS_OK) return syscall_err(err);

    /* Copy sender's message. */
    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Fase 9: STAMP the sender badge from the invoked capability — whatever
     * the sender wrote in the field is discarded (anti-spoofing). */
    t->ipc_msg.sender_badge = ep_badge;

    /* Fastpath: no cap, no bulk buffer, receiver already waiting. */
    if (t->ipc_msg.attached_handle == IRIS_MSG_NO_CAP && t->ipc_msg.buf_len == 0)
        if (ep_send_fastpath(t, ep))
            return syscall_ok_u64(0);

    /* Ph69: stage bulk payload in kernel buffer. */
    t->ipc_kbuf_len = 0;
    if (t->ipc_msg.buf_len > 0 && t->ipc_msg.buf_uptr != 0) {
        uint32_t n = t->ipc_msg.buf_len;
        if (n > IRIS_IPC_BUF_SIZE) n = IRIS_IPC_BUF_SIZE;
        if (!user_range_readable(t->ipc_msg.buf_uptr, n) ||
            !copy_from_user_checked(t->ipc_kbuf, t->ipc_msg.buf_uptr, n)) {
            kobject_release(&ep->base);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }
        t->ipc_kbuf_len  = n;
        t->ipc_msg.buf_len = n;
    }

    /* Ph68: validate and stage attached cap before taking the spinlock. */
    struct KObject *xfer_obj    = 0;
    uint32_t        xfer_rights = 0;
    uint64_t        xfer_badge  = 0;
    if (t->ipc_msg.attached_handle != IRIS_MSG_NO_CAP) {
        iris_error_t cr = syscall_ipc_stage_cap_badged(t, t->ipc_msg.attached_handle,
                                         t->ipc_msg.attached_rights,
                                         &xfer_obj, &xfer_rights, &xfer_badge);
        if (cr != IRIS_OK) {
            kobject_release(&ep->base);
            return syscall_err(cr);
        }
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP; /* cleared in staged msg */
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) kobject_release(xfer_obj);
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

        /* Ph69: copy kbuf to receiver's staging. */
        if (t->ipc_kbuf_len > 0) {
            copy_kbuf(receiver->ipc_kbuf, t->ipc_kbuf, t->ipc_kbuf_len);
            receiver->ipc_kbuf_len = t->ipc_kbuf_len;
        } else {
            receiver->ipc_kbuf_len = 0;
        }

        irq_spinlock_unlock(&ep->lock, flags);

        /* Ph68: install cap (outside lock).  A1.5: routed — lands in the
         * receiver's declared receive-slot (CPtr) or its handle table. */
        if (xfer_obj) {
            uint32_t new_h = syscall_ipc_deliver_cap_routed(receiver, xfer_obj,
                                                            xfer_rights, xfer_badge);
            receiver->ipc_msg.attached_handle = new_h;
        }

        /* Wake receiver only after all data is consistent. */
        task_wakeup(receiver);
        kobject_release(&ep->base);
        return syscall_ok_u64(0);
    }

    /* No receiver: stage cap in task and block. */
    ep->ep_state     = EP_STATE_SEND;
    t->ep_next       = 0;
    t->blocking_ep   = ep;
    t->ipc_msg_ready = 0;
    t->ipc_ep_closed = 0;
    t->ep_cap_obj    = xfer_obj;    /* staging ref; released by receiver or cancel */
    t->ep_cap_rights = xfer_rights;
    t->ep_cap_badge  = xfer_badge;

    if (ep->queue_tail) { ep->queue_tail->ep_next = t; ep->queue_tail = t; }
    else                { ep->queue_head = t; ep->queue_tail = t; }

    t->state = TASK_BLOCKED_SEND;
    irq_spinlock_unlock(&ep->lock, flags);

    task_yield();

    kobject_release(&ep->base);

    if (t->ipc_ep_closed) { t->ipc_ep_closed = 0; return syscall_err(IRIS_ERR_CLOSED); }
    return syscall_ok_u64(0);
}

/* ── SYS_EP_RECV ─────────────────────────────────────────────────────── */

uint64_t sys_ep_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    iris_error_t err = cspace_or_handle_resolve_endpoint(t->process, (iris_cptr_t)arg0,
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

    /* Fastpath: no-cap, no-bulk, non-EP_CALL sender already waiting. */
    if (ep_recv_fastpath(t, ep)) {
        t->ep_recv_slot = 0;   /* no cap on the fastpath — drop the declaration */
        kobject_release(&ep->base);
        if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
            return syscall_err(IRIS_ERR_INVALID_ARG);
        return syscall_ok_u64(0);
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state == EP_STATE_SEND) {
        /* Rendezvous: a sender is already waiting. */
        struct task *sender = ep->queue_head;
        ep->queue_head = sender->ep_next;
        if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
        sender->ep_next     = 0;
        sender->blocking_ep = 0;

        irismsg_copy64(&t->ipc_msg, &sender->ipc_msg);
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP; /* will update after unlock */

        /* Ph69: receiver is in their own CR3 — copy kbuf directly to user space. */
        uint64_t recv_buf = t->ep_recv_buf_uptr;
        uint32_t kbuf_n   = sender->ipc_kbuf_len;
        if (kbuf_n > 0 && recv_buf != 0 &&
            user_range_writable(recv_buf, kbuf_n) &&
            copy_to_user_checked(recv_buf, sender->ipc_kbuf, kbuf_n)) {
            t->ipc_msg.buf_len  = kbuf_n;
            t->ipc_msg.buf_uptr = recv_buf;
        } else {
            t->ipc_msg.buf_len  = kbuf_n; /* report available bytes even if not written */
            t->ipc_msg.buf_uptr = 0;
        }
        sender->ipc_kbuf_len = 0;

        /* Ph68: take sender's staged cap. */
        struct KObject *xfer_obj    = sender->ep_cap_obj;
        uint32_t        xfer_rights = sender->ep_cap_rights;
        uint64_t        xfer_badge  = sender->ep_cap_badge;
        sender->ep_cap_obj    = 0;
        sender->ep_cap_rights = 0;
        sender->ep_cap_badge  = 0;

        irq_spinlock_unlock(&ep->lock, flags);

        /* Ph68/Fase11: install sender's transferred cap.  For an EP_CALL the
         * reply cap takes attached_handle, so the transferred cap is delivered
         * into the separate attached_cap field; EP_SEND keeps attached_handle.
         * A1.5: routed — honours our declared receive-slot (CPtr < 1024). */
        t->ipc_msg.attached_cap = IRIS_MSG_NO_CAP;
        if (xfer_obj) {
            uint32_t new_h = syscall_ipc_deliver_cap_routed(t, xfer_obj,
                                                            xfer_rights, xfer_badge);
            if (sender->ep_call_mode) {
                t->ipc_msg.attached_cap        = new_h;
                t->ipc_msg.attached_cap_rights = xfer_rights;
            } else {
                t->ipc_msg.attached_handle = new_h;
            }
        }
        t->ep_recv_slot = 0;   /* declaration is per-recv; never outlives it */

        /* Ph85: if sender used EP_CALL, create KReply and keep sender blocked. */
        if (sender->ep_call_mode) {
            sender->ep_call_mode = 0u;
            struct KReply *rp = kreply_alloc(sender);
            if (rp) {
                kobject_retain(&rp->base);         /* sender's own lifecycle ref */
                sender->pending_kreply = rp;
                handle_id_t rh = handle_table_insert(&t->process->handle_table,
                                                      &rp->base,
                                                      RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER);
                kobject_release(&rp->base);        /* drop alloc ref; HT holds its own */
                if (rh != HANDLE_INVALID) {
                    t->ipc_msg.attached_handle = (uint32_t)rh;
                    sender->state = TASK_BLOCKED_REPLY;
                } else {
                    kobject_release(&sender->pending_kreply->base);
                    sender->pending_kreply = 0;
                    sender->ipc_ep_closed  = 1u;
                    task_wakeup(sender);
                }
            } else {
                sender->ipc_ep_closed = 1u;
                task_wakeup(sender);
            }
        } else {
            task_wakeup(sender);
        }

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

    task_yield();

    kobject_release(&ep->base);

    /* A1.5: any routed delivery already consumed the declaration from the
     * sender's context; make sure it never survives this recv either way. */
    t->ep_recv_slot = 0;

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
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    uint64_t ep_badge = 0;
    iris_error_t err = cspace_or_handle_resolve_endpoint_badged(
        t->process, (iris_cptr_t)arg0, RIGHT_WRITE, &ep, &_ep_r, &ep_badge);
    if (err != IRIS_OK) return syscall_err(err);

    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Fase 9: stamp the sender badge from the invoked cap (anti-spoofing). */
    t->ipc_msg.sender_badge = ep_badge;

    /* Ph69: stage kbuf. */
    t->ipc_kbuf_len = 0;
    if (t->ipc_msg.buf_len > 0 && t->ipc_msg.buf_uptr != 0) {
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

    /* Ph68: stage cap before taking lock. */
    struct KObject *xfer_obj    = 0;
    uint32_t        xfer_rights = 0;
    uint64_t        xfer_badge  = 0;
    if (t->ipc_msg.attached_handle != IRIS_MSG_NO_CAP) {
        iris_error_t cr = syscall_ipc_stage_cap_badged(t, t->ipc_msg.attached_handle,
                                         t->ipc_msg.attached_rights,
                                         &xfer_obj, &xfer_rights, &xfer_badge);
        if (cr != IRIS_OK) { kobject_release(&ep->base); return syscall_err(cr); }
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) kobject_release(xfer_obj);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state != EP_STATE_RECV) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) kobject_release(xfer_obj);
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

    if (t->ipc_kbuf_len > 0) {
        copy_kbuf(receiver->ipc_kbuf, t->ipc_kbuf, t->ipc_kbuf_len);
        receiver->ipc_kbuf_len = t->ipc_kbuf_len;
    } else {
        receiver->ipc_kbuf_len = 0;
    }

    irq_spinlock_unlock(&ep->lock, flags);

    /* A1.5: routed — receiver's declared receive-slot or handle table. */
    if (xfer_obj) {
        uint32_t new_h = syscall_ipc_deliver_cap_routed(receiver, xfer_obj,
                                                        xfer_rights, xfer_badge);
        receiver->ipc_msg.attached_handle = new_h;
    }

    task_wakeup(receiver);
    kobject_release(&ep->base);
    return syscall_ok_u64(0);
}

/* ── SYS_EP_NB_RECV ──────────────────────────────────────────────────── */

uint64_t sys_ep_nb_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep; iris_rights_t _ep_r;
    iris_error_t err = cspace_or_handle_resolve_endpoint(t->process, (iris_cptr_t)arg0,
                                                          RIGHT_READ, &ep, &_ep_r);
    if (err != IRIS_OK) return syscall_err(err);

    /* Ph69: read receiver's hint.  A1.5: attached_cap declares a receive-slot. */
    uint64_t recv_buf = 0;
    t->ep_recv_slot = 0;
    {
        struct IrisMsg hints;
        if (user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)) &&
            copy_from_user_checked(&hints, arg1, (uint32_t)sizeof(struct IrisMsg))) {
            recv_buf = hints.buf_uptr;
            err = syscall_ipc_recv_slot_declare(t, hints.attached_cap);
            if (err != IRIS_OK) {
                kobject_release(&ep->base);
                return syscall_err(err);
            }
        }
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        t->ep_recv_slot = 0;
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state != EP_STATE_SEND) {
        irq_spinlock_unlock(&ep->lock, flags);
        t->ep_recv_slot = 0;
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_WOULD_BLOCK);
    }

    struct task *sender = ep->queue_head;
    ep->queue_head = sender->ep_next;
    if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
    sender->ep_next     = 0;
    sender->blocking_ep = 0;

    irismsg_copy64(&t->ipc_msg, &sender->ipc_msg);
    t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;

    /* Ph69: direct copy to receiver's user space (correct CR3). */
    uint32_t kbuf_n = sender->ipc_kbuf_len;
    if (kbuf_n > 0 && recv_buf != 0 &&
        user_range_writable(recv_buf, kbuf_n) &&
        copy_to_user_checked(recv_buf, sender->ipc_kbuf, kbuf_n)) {
        t->ipc_msg.buf_len  = kbuf_n;
        t->ipc_msg.buf_uptr = recv_buf;
    } else {
        t->ipc_msg.buf_len  = kbuf_n;
        t->ipc_msg.buf_uptr = 0;
    }
    sender->ipc_kbuf_len = 0;

    /* Ph68: take sender's staged cap. */
    struct KObject *xfer_obj    = sender->ep_cap_obj;
    uint32_t        xfer_rights = sender->ep_cap_rights;
    uint64_t        xfer_badge  = sender->ep_cap_badge;
    sender->ep_cap_obj    = 0;
    sender->ep_cap_rights = 0;
    sender->ep_cap_badge  = 0;

    irq_spinlock_unlock(&ep->lock, flags);

    /* Fase 11: EP_CALL transferred cap → attached_cap; EP_SEND → attached_handle.
     * A1.5: routed — honours our declared receive-slot (CPtr < 1024). */
    t->ipc_msg.attached_cap = IRIS_MSG_NO_CAP;
    if (xfer_obj) {
        uint32_t new_h = syscall_ipc_deliver_cap_routed(t, xfer_obj,
                                                        xfer_rights, xfer_badge);
        if (sender->ep_call_mode) {
            t->ipc_msg.attached_cap        = new_h;
            t->ipc_msg.attached_cap_rights = xfer_rights;
        } else {
            t->ipc_msg.attached_handle = new_h;
        }
    }
    t->ep_recv_slot = 0;   /* declaration is per-recv; never outlives it */

    /* Ph85: ep_call_mode senders block until replied to. */
    if (sender->ep_call_mode) {
        sender->ep_call_mode = 0u;
        struct KReply *rp = kreply_alloc(sender);
        if (rp) {
            kobject_retain(&rp->base);
            sender->pending_kreply = rp;
            handle_id_t rh = handle_table_insert(&t->process->handle_table,
                                                  &rp->base,
                                                  RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER);
            kobject_release(&rp->base);
            if (rh != HANDLE_INVALID) {
                t->ipc_msg.attached_handle = (uint32_t)rh;
                sender->state = TASK_BLOCKED_REPLY;
            } else {
                kobject_release(&sender->pending_kreply->base);
                sender->pending_kreply = 0;
                sender->ipc_ep_closed  = 1u;
                task_wakeup(sender);
            }
        } else {
            sender->ipc_ep_closed = 1u;
            task_wakeup(sender);
        }
    } else {
        task_wakeup(sender);
    }

    kobject_release(&ep->base);

    if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(0);
}
