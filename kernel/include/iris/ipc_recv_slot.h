/*
 * ipc_recv_slot.h — userland helpers for the IPC receive-slot protocol
 * (A1.5 kernel mechanism, A1.6 in-tree adoption).
 *
 * A receiver may declare, per receive operation, an empty slot of its CSpace:
 * "if this receive delivers a transferred cap, install it there."  Since
 * Stage 4 the declaration is a full CPtr, not a direct root index, so a
 * process whose root CNode is full can still receive capabilities — into a
 * second-level CNode, the way a real CSpace hierarchy works.
 *
 * A-33 gave the declaration an argument register of its own.  It used to ride
 * in two dead message fields whose identity depended on the operation
 * (`attached_cap` for a receive, `attached_handle` for a Call), which is why
 * the old text here said "no ABI change" and why there used to be two helpers
 * to choose between.  There is one place to put it now: `msg.recv_slot`.
 *
 * What a receiver gets back is `msg.got_cap` — a CPtr, or `IRIS_MSG_NO_CAP`
 * when nothing travelled.  There is no second outcome any more: handle
 * materialization was the fallback for a receiver that declared nothing, and
 * Stage 4 retired it along with the handle table.  A receive that declares no
 * slot is delivered the MESSAGE without the capability, which is the same
 * fail-closed shape an occupied or broken slot has.
 *
 * The classifier below survives because the boundary it tests is still real:
 * a value with the handle TAG BIT set is not a CPtr.  It used to be tested as
 * the literal 1024, which was correct only while handles were encoded as
 * `slot | gen << 10`; handles carry bit 31 now (see nc/handle.h) and CPtrs own
 * the whole low 31 bits, so a two-level CPtr such as (leaf << 8) | 80 is
 * routinely above 1024 and is NOT a handle.  Keeping the old test would have
 * classified every multi-level delivery as a handle.
 *
 * These helpers only read outputs; they never bypass kernel validation
 * (occupied slot → IRIS_ERR_ALREADY_EXISTS fail-fast, broken/occupied
 * destination at delivery → no cap delivered, fail closed).
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

/* Is this a delivered capability?  A receive reports `IRIS_MSG_NO_CAP` when
 * nothing came, and a CPtr when something did. */
static inline int iris_msg_cap_is_cptr(uint32_t v) {
    return v != (uint32_t)IRIS_MSG_NO_CAP && v < IRIS_CPTR_LIMIT;
}
static inline int iris_msg_cap_is_handle(uint32_t v) {
    return v >= IRIS_CPTR_LIMIT;
}

#endif /* IRIS_IPC_RECV_SLOT_H */
