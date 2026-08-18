/*
 * ipc_recv_slot.h — userland helpers for the IPC receive-slot protocol
 * (A1.5 kernel mechanism, A1.6 in-tree adoption).
 *
 * A receiver may declare, per receive operation, an empty slot of its CSpace:
 * "if this receive delivers a transferred cap, install it there."  Since
 * Stage 4 the declaration is a full CPtr, not a direct root index, so a
 * process whose root CNode is full can still receive capabilities — into a
 * second-level CNode, the way a real CSpace hierarchy works.
 * The declaration reuses two previously-dead IrisMsg input values (no ABI
 * change — see docs/architecture/a1-5-ipc-receive-slot.md):
 *
 *   SYS_EP_RECV / SYS_EP_NB_RECV : input hint msg.attached_cap
 *   SYS_EP_CALL                  : input field msg.attached_handle
 *                                  (the slot for a cap the REPLY transfers)
 *
 * Output discriminator (both attached_handle and attached_cap), shared with
 * the CPtr/handle namespace split:
 *
 *   0                      no cap delivered
 *   HANDLE_TAG bit clear   cap installed in the receiver's CSpace (CPtr)
 *   HANDLE_TAG bit set     cap materialized as a handle (legacy / fallback)
 *
 * The boundary is the handle TAG BIT, not a magnitude.  It used to be the
 * literal 1024, which was correct only while handles were encoded as
 * `slot | gen << 10` and therefore always >= 1024.  Handles carry bit 31 now
 * (see nc/handle.h), and CPtrs own the whole low 31 bits — a two-level CPtr
 * such as (leaf << 8) | 80 is routinely above 1024 and is NOT a handle.
 * Keeping the old test would have classified every multi-level delivery as a
 * handle.
 *
 * Declaring slot 0 (or not declaring) keeps bit-for-bit legacy behavior.
 * These helpers only write input fields and read outputs; they never bypass
 * kernel validation (occupied slot → IRIS_ERR_ALREADY_EXISTS fail-fast,
 * broken/occupied destination at delivery → no cap delivered, fail closed).
 */

#ifndef IRIS_IPC_RECV_SLOT_H
#define IRIS_IPC_RECV_SLOT_H

#include <stdint.h>
#include <iris/ipc_msg.h>
#include <iris/nc/handle.h>   /* HANDLE_TAG — the one namespace boundary */

/* Namespace boundary: a value with the handle tag bit set is a handle id
 * (handle-table-resolved); anything else non-zero is a CPtr (CSpace-resolved).
 * IRIS_CPTR_LIMIT is the first value that is NOT a CPtr, i.e. the tag bit
 * itself — it keeps its name so existing `v < IRIS_CPTR_LIMIT` call sites stay
 * correct, and it agrees with CSPACE_DIRECT_CPTR_LIMIT in nc/cspace.h, which
 * is the kernel-side definition of the same boundary. */
#define IRIS_CPTR_LIMIT ((uint32_t)HANDLE_TAG)

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
