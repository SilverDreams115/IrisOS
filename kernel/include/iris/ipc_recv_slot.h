/*
 * ipc_recv_slot.h — userland helpers for the IPC receive-slot protocol
 * (A1.5 kernel mechanism, A1.6 in-tree adoption).
 *
 * A receiver may declare, per receive operation, an empty direct slot of its
 * root CNode: "if this receive delivers a transferred cap, install it there."
 * The declaration reuses two previously-dead IrisMsg input values (no ABI
 * change — see docs/architecture/a1-5-ipc-receive-slot.md):
 *
 *   SYS_EP_RECV / SYS_EP_NB_RECV : input hint msg.attached_cap
 *   SYS_EP_CALL                  : input field msg.attached_handle
 *                                  (the slot for a cap the REPLY transfers)
 *
 * Output discriminator (both attached_handle and attached_cap), shared with
 * the Fase 8 CPtr/handle namespace split:
 *
 *   0                      no cap delivered
 *   1..IRIS_CPTR_LIMIT-1   cap installed in the receiver's CSpace (CPtr)
 *   >= IRIS_CPTR_LIMIT     cap materialized as a handle (legacy / fallback)
 *
 * Declaring slot 0 (or not declaring) keeps bit-for-bit legacy behavior.
 * These helpers only write input fields and read outputs; they never bypass
 * kernel validation (occupied slot → IRIS_ERR_ALREADY_EXISTS fail-fast,
 * TOCTOU-filled slot → documented handle fallback).
 */

#ifndef IRIS_IPC_RECV_SLOT_H
#define IRIS_IPC_RECV_SLOT_H

#include <stdint.h>
#include <iris/ipc_msg.h>

/* Fase 8 namespace boundary: values below this are CPtrs (CSpace-resolved),
 * values at or above it are handle ids (handle-table-resolved). */
#define IRIS_CPTR_LIMIT 1024u

/* Declare a receive-slot for the cap a sender attaches (EP_RECV/EP_NB_RECV).
 * slot = 0 keeps the legacy attached-handle delivery. */
static inline void iris_msg_declare_recv_slot(struct IrisMsg *m, uint32_t slot) {
    m->attached_cap = slot;
}

/* Declare a receive-slot for a cap the REPLY transfers back (EP_CALL).
 * slot = 0 keeps the legacy delivery; the KReply cap itself is unaffected
 * (it is ephemeral by design and always arrives as a handle). */
static inline void iris_msg_declare_reply_slot(struct IrisMsg *m, uint32_t slot) {
    m->attached_handle = slot;
}

/* Discriminate a delivered-cap output field (attached_cap or, after EP_CALL,
 * attached_handle).  Exactly one of these is true when a cap was delivered. */
static inline int iris_msg_cap_is_cptr(uint32_t v) {
    return v != (uint32_t)IRIS_MSG_NO_CAP && v < IRIS_CPTR_LIMIT;
}
static inline int iris_msg_cap_is_handle(uint32_t v) {
    return v >= IRIS_CPTR_LIMIT;
}

#endif /* IRIS_IPC_RECV_SLOT_H */
