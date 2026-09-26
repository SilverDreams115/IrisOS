/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_NC_CSPACE_H
#define IRIS_NC_CSPACE_H

#include <iris/nc/kobject.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/nc/cptr.h>
#include <stdint.h>

/*
 * iris_cptr_t — capability pointer (formal type for Phase 3).
 *
 * Encodes a path through a process's CNode tree.  Each CNode level consumes
 * log2(slot_count) bits from the LSB; traversal stops when remaining bits
 * are zero (terminal slot) or a non-CNode object is encountered.
 *
 * Example — two-level tree, both CNodes with 256 slots (8 bits each):
 *   cptr = (leaf_slot << 8) | root_slot
 *
 * IRIS_CPTR_NULL (0) is the null capability.  Slot 0 is the null slot in every
 * CNode by kernel convention and is never populated.  cspace_resolve_cap()
 * rejects IRIS_CPTR_NULL with IRIS_ERR_INVALID_ARG before any traversal.
 *
 * seL4 compatibility note: this matches seL4's CPtr semantics — a raw
 * unsigned index into the process-local CNode tree rooted at the TCB's
 * CSpace root, without guard bits (simplified model, guard bits future).
 */
/*
 * `iris_cptr_t` and `IRIS_CPTR_NULL` come from nc/cptr.h, included above,
 * which is the header ring 3 includes.  This file used to declare the type a
 * second time and to spell the null capability `CPTR_NULL` — two names for one
 * thing, which a 1.0 ABI (Stage 10-abi) does not have.
 */
#define CSPACE_MAX_DEPTH 8u

/*
 * The value-range split between the two authority namespaces: below the limit
 * a syscall argument is a CPtr, at or above it a handle id
 * (slot | gen << 10, generation >= 1).
 *
 * This is the discrimination the dual-namespace retirement deletes, so it has
 * exactly ONE definition — every caller tests it through cspace_value_is_cptr
 * and nobody open-codes `< 1024`.  When the handle namespace goes, this block
 * and its callers are the whole edit.
 */
#define CSPACE_DIRECT_CPTR_LIMIT ((iris_cptr_t)IRIS_CPTR_LIMIT)

static inline int cspace_value_is_cptr(iris_cptr_t v) {
    return v != IRIS_CPTR_NULL && v < CSPACE_DIRECT_CPTR_LIMIT;
}

struct KEndpoint;
struct KReply;
struct KCNode;
struct KNotification;
struct task;
struct KUntyped;
struct KSchedContext;
struct KVSpace;
struct KFrame;

/*
 * cspace_resolve_cap — kernel-internal CSpace traversal.
 *
 * Traverses root's CNode tree using cptr, starting from root —
 * a structural back-reference, not a handle (Stage 4).
 * On success returns IRIS_OK and writes the terminal capability into *obj_out
 * and its effective rights into *rights_out.
 *
 * The returned object carries one kobject_active_retain + one kobject_retain.
 * The caller MUST release both when done:
 *   kobject_active_release(*obj_out);
 *   kobject_release(*obj_out);
 *
 * If required != RIGHT_NONE, the terminal slot's rights are checked against
 * required; IRIS_ERR_ACCESS_DENIED is returned if they are insufficient.
 * Pass RIGHT_NONE to skip the rights check (syscall layer checks separately).
 *
 * Authority invariants upheld by this function:
 *   1. IRIS_CPTR_NULL always fails — null slot is never occupied.
 *   2. Rights are monotonically non-increasing: returned rights ⊆ slot rights.
 *   3. Traversal depth is bounded (CSPACE_MAX_DEPTH levels maximum).
 *   4. Each CNode level is released after descent — no lingering borrows.
 *
 * Errors:
 *   IRIS_ERR_INVALID_ARG   — cptr == IRIS_CPTR_NULL, root NULL, or depth exhausted
 *   IRIS_ERR_NOT_FOUND     — no CSpace root set, or a slot is empty
 *   IRIS_ERR_ACCESS_DENIED — terminal slot rights do not satisfy required
 */
iris_error_t cspace_resolve_cap(struct KCNode     *root,
                                 iris_cptr_t        cptr,
                                 iris_rights_t      required,
                                 struct KObject   **obj_out,
                                 iris_rights_t     *rights_out);

/* Phase 9: like cspace_resolve_cap but also returns the terminal slot's
 * badge (badge_out may be NULL). */
iris_error_t cspace_resolve_cap_badged(struct KCNode     *root,
                                        iris_cptr_t        cptr,
                                        iris_rights_t      required,
                                        struct KObject   **obj_out,
                                        iris_rights_t     *rights_out,
                                        uint64_t          *badge_out);

/* Phase S3: resolve a CPtr to its terminal SLOT LOCATION (CNode + index) —
 * the identity the MDB operates on.  CSpace namespace only (caller guards
 * the <1024 split).  On success the CNode carries active+lifecycle refs
 * (caller releases both); the slot was occupied at resolution time. */
iris_error_t cspace_resolve_slot(struct KCNode   *root, iris_cptr_t cptr,
                                 struct KCNode **cn_out, uint32_t *idx_out);

/* Stage 4: the DESTINATION analogue of cspace_resolve_slot — resolves a CPtr
 * to the terminal (CNode, index) it addresses WITHOUT requiring that slot to
 * be occupied.  Same traversal, same ref contract (the returned CNode carries
 * active+lifecycle refs the caller releases), but the terminal slot may be
 * empty: that is the normal case for an install target.
 *
 * The difference matters at the last level only.  cspace_resolve_slot stops
 * at the first non-CNode it finds and calls it terminal, because a source has
 * to be occupied to be a source.  A destination path is terminal when the
 * CPtr is EXHAUSTED, so an empty intermediate slot is a broken path
 * (NOT_FOUND) rather than an answer, and an occupied non-CNode intermediate
 * is WRONG_TYPE rather than a silent redirect to the wrong slot.
 *
 * This is what lets a receive slot live below the root CNode: a process whose
 * root is full has nowhere to receive a capability otherwise. */
iris_error_t cspace_resolve_dest_slot(struct KCNode   *root, iris_cptr_t cptr,
                                      struct KCNode **cn_out,
                                      uint32_t *idx_out);

/* Phase 9: badge-aware dual endpoint resolver for the EP send/call paths.
 * Same namespace + refcount contract as cspace_resolve_only_endpoint
 * (lifecycle-only ref); additionally returns the badge of the capability
 * that was invoked (slot badge on the CSpace path, handle badge on the
 * handle path; 0 = unbadged). */
iris_error_t cspace_resolve_only_endpoint_badged(struct KCNode    *root,
                                                       iris_cptr_t       cptr,
                                                       iris_rights_t     required,
                                                       struct KEndpoint **out,
                                                       iris_rights_t    *rights_out,
                                                       uint64_t         *badge_out);

/*
 * Typed resolve helpers — call cspace_resolve_cap, validate object type,
 * return cast pointer.  Same ref-count contract as cspace_resolve_cap:
 * caller must kobject_active_release + kobject_release the returned pointer.
 * Return IRIS_ERR_WRONG_TYPE if the resolved capability has a different type.
 */
iris_error_t cspace_resolve_endpoint(struct KCNode    *root, iris_cptr_t cptr,
                                      iris_rights_t       required,
                                      struct KEndpoint  **out, iris_rights_t *rights_out);
iris_error_t cspace_resolve_reply(struct KCNode    *root, iris_cptr_t cptr,
                                   iris_rights_t     required,
                                   struct KReply   **out, iris_rights_t *rights_out);
iris_error_t cspace_resolve_cnode(struct KCNode   *root, iris_cptr_t cptr,
                                   iris_rights_t    required,
                                   struct KCNode  **out, iris_rights_t *rights_out);
iris_error_t cspace_resolve_notification(struct KCNode      *root, iris_cptr_t cptr,
                                          iris_rights_t         required,
                                          struct KNotification **out,
                                          iris_rights_t        *rights_out);
iris_error_t cspace_resolve_tcb(struct KCNode   *root, iris_cptr_t cptr,
                                 iris_rights_t    required,
                                 struct task    **out, iris_rights_t *rights_out);
iris_error_t cspace_resolve_untyped(struct KCNode    *root, iris_cptr_t cptr,
                                     iris_rights_t     required,
                                     struct KUntyped **out, iris_rights_t *rights_out);
iris_error_t cspace_resolve_schedctx(struct KCNode     *root, iris_cptr_t cptr,
                                      iris_rights_t        required,
                                      struct KSchedContext**out,
                                      iris_rights_t       *rights_out);
iris_error_t cspace_resolve_vspace(struct KCNode    *root, iris_cptr_t cptr,
                                    iris_rights_t     required,
                                    struct KVSpace  **out, iris_rights_t *rights_out);
iris_error_t cspace_resolve_frame(struct KCNode   *root, iris_cptr_t cptr,
                                   iris_rights_t    required,
                                   struct KFrame  **out, iris_rights_t *rights_out);

/*
 * cspace_resolve_only_frame — dual-resolution helper for KFrame syscalls.
 *
 * Tries CSpace traversal first (CSpace-first authority).  Falls back to the
 * handle table if CSpace fails with NOT_FOUND or INVALID_ARG (legacy handle).
 * ACCESS_DENIED from CSpace is a hard stop — no fallback.
 *
 * Ref-count contract: active + lifecycle (same as cspace_resolve_only_untyped).
 * KFrame operations do not block, so holding active_refs is safe.
 * Caller MUST release both:
 *   kobject_active_release(&(*out)->base);
 *   kobject_release(&(*out)->base);
 */
iris_error_t cspace_resolve_only_frame(struct KCNode   *root,
                                             iris_cptr_t      cptr,
                                             iris_rights_t    required,
                                             struct KFrame  **out,
                                             iris_rights_t   *rights_out);

/*
 * cspace_resolve_only_vspace — dual resolver for the VSpace argument of
 * SYS_FRAME_MAP/SYS_FRAME_UNMAP (Phase 25).  Same namespace split and
 * active+lifecycle ref contract as cspace_resolve_only_frame; closes the
 * raw-radix handle-masking hazard those two syscalls still carried and lets a
 * supervisor pass a SYS_PROCESS_VSPACE handle directly.
 */
iris_error_t cspace_resolve_only_vspace(struct KCNode   *root,
                                              iris_cptr_t      cptr,
                                              iris_rights_t    required,
                                              struct KVSpace **out,
                                              iris_rights_t   *rights_out);

/*
 * Phase 13: generic dual resolver for device/authority caps (KIoPort, KIrqCap,
 * KBootstrapCap).  Namespace split as usual; LIFECYCLE-ONLY ref contract
 * (lifecycle-only) — release with a single kobject_release.
 * required==RIGHT_NONE defers the rights check to the caller.
 */
iris_error_t cspace_resolve_only_obj(struct KCNode    *root,
                                          iris_cptr_t       cptr,
                                          iris_rights_t     required,
                                          uint32_t          expected_type,
                                          struct KObject  **out,
                                          iris_rights_t    *rights_out);

/*
 * cspace_resolve_only_cnode — resolve a CNode capability.
 *
 * One resolution, not two: the CSpace traversal is the only one there is.  This
 * used to fall back to a handle table when the traversal failed with anything
 * but ACCESS_DENIED, and both the table and the fallback are gone — the helper
 * keeps its shape and its refcount contract, not its second namespace.
 *
 * Both paths return the same ref-count contract as cspace_resolve_cap:
 * one kobject_active_retain + one kobject_retain on *out.
 * The caller MUST release both:
 *   kobject_active_release(&(*out)->base);
 *   kobject_release(&(*out)->base);
 */
iris_error_t cspace_resolve_only_cnode(struct KCNode   *root,
                                             iris_cptr_t      cptr,
                                             iris_rights_t    required,
                                             struct KCNode  **out,
                                             iris_rights_t   *rights_out);

/*
 * cspace_resolve_only_untyped — dual-resolution helper for KUntyped syscalls.
 *
 * One resolution, not two: the CSpace traversal is the only one there is.  This
 * used to fall back to a handle table when the traversal failed with anything
 * but ACCESS_DENIED, and both the table and the fallback are gone — the helper
 * keeps its shape and its refcount contract, not its second namespace.
 *
 * Ref-count contract: ACTIVE + LIFECYCLE (same as cspace_resolve_only_cnode).
 * KUntyped operations (INFO/RETYPE/RESET) never PARK; holding active_refs
 * across one is safe.  (This said "across task_yield()" until the roadmap
 * review: Stage 9-evt step 3 deleted that function, and the boundary an
 * invariant like this is written against is the park point.)  KUntyped's
 * close callback is a no-op, so there is no IPC-style "wake blocked tasks"
 * concern.
 *
 * Caller MUST release both:
 *   kobject_active_release(&(*out)->base);
 *   kobject_release(&(*out)->base);
 *
 * ACCESS_DENIED from CSpace is a hard stop — no fallback to handle table.
 */
iris_error_t cspace_resolve_only_untyped(struct KCNode    *root,
                                               iris_cptr_t       cptr,
                                               iris_rights_t     required,
                                               struct KUntyped **out,
                                               iris_rights_t    *rights_out);

/*
 * IPC dual-resolve helpers — LIFECYCLE-ONLY ref contract.
 *
 * These helpers differ from cspace_resolve_only_cnode in one critical
 * way: they return only a lifecycle retain (kobject_release), NOT an
 * active_retain.
 *
 * Reason: IPC operations (EP_SEND/RECV/CALL, REPLY, NOTIFY_WAIT) can PARK —
 * "block across task_yield()" until Stage 9-evt step 3 deleted that function,
 * the same boundary under a new name.  Holding active_refs > 0 across it would prevent
 * the close callback from firing when the capability is closed.  For KEndpoint,
 * that close callback (kendpoint_obj_close) is the only mechanism that wakes
 * tasks blocked on a destroyed endpoint.  Holding active_retain would stall
 * those tasks permanently.
 *
 * For the CSpace path, cspace_resolve_cap returns active+lifecycle; the active
 * ref is released inside the helper before returning.  The handle fallback path
 * gives lifecycle-only by design — no extra retain.
 *
 * Caller MUST release with:   kobject_release(&(*out)->base);
 * Caller MUST NOT call:       kobject_active_release on the returned pointer.
 *
 * Both CSpace and handle-table paths produce the same lifecycle-only contract.
 * ACCESS_DENIED from CSpace is a hard stop — no fallback to handle table.
 */
iris_error_t cspace_resolve_only_endpoint(struct KCNode     *root,
                                                iris_cptr_t        cptr,
                                                iris_rights_t      required,
                                                struct KEndpoint **out,
                                                iris_rights_t     *rights_out);

iris_error_t cspace_resolve_only_reply(struct KCNode   *root,
                                             iris_cptr_t      cptr,
                                             iris_rights_t    required,
                                             struct KReply  **out,
                                             iris_rights_t   *rights_out);

iris_error_t cspace_resolve_only_notification(struct KCNode      *root,
                                                    iris_cptr_t           cptr,
                                                    iris_rights_t         required,
                                                    struct KNotification **out,
                                                    iris_rights_t        *rights_out);

#endif /* IRIS_NC_CSPACE_H */
