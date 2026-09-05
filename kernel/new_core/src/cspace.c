#include <iris/nc/cspace.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kreply.h>
#include <iris/nc/knotification.h>
#include <iris/nc/ktcb.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kschedctx.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kobject.h>
#include <iris/nc/rights.h>
#include <iris/task.h>

iris_error_t cspace_resolve_cap_badged(struct KCNode     *root,
                                        iris_cptr_t        cptr,
                                        iris_rights_t      required,
                                        struct KObject   **obj_out,
                                        iris_rights_t     *rights_out,
                                        uint64_t          *badge_out)
{
    if (!obj_out || !rights_out) return IRIS_ERR_INVALID_ARG;
    if (cptr == CPTR_NULL) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    /* Stage 4: structural root — no handle-table lookup to start the walk.
     * Stage 7 Step 4: and the root the caller passes is the THREAD's, so a
     * resolution no longer reads KProcess either. */
    struct KObject *root_obj = &root->base;
    iris_error_t    err;

    /* Take the retain the handle read used to yield, plus an active retain, so
     * the loop can uniformly call both kobject_active_release + kobject_release
     * for every level including root, without triggering premature close of the
     * root CNode's slots. */
    kobject_retain(root_obj);
    kobject_active_retain(root_obj);
    struct KCNode *cur = (struct KCNode *)root_obj;

    /*
     * Stage 8-cap / D-2 — the guard of the capability the walk ENTERED `cur`
     * through.  Below the root that is the slot it descended from; at the root
     * it is the thread, because a thread reaches its root CSpace through a
     * structural pointer and there is no slot to carry a guard.
     *
     * The root condition is exact rather than convenient: the guard applies
     * only when the CNode being walked IS the running thread's root, which is
     * the only capability it belongs to.  A walk rooted at some OTHER CNode —
     * a destination the caller named, a child's root a spawner mints into — is
     * a different capability and must not inherit it.  In the host suite
     * task_current() is NULL, so this is inert and the resolver behaves
     * exactly as it did before guards existed.
     */
    uint64_t pending_guard      = 0;
    uint8_t  pending_guard_bits = 0;
    {
        struct task *self = task_current();
        if (self && self->cspace_root == root && self->cspace_root_guard_bits) {
            pending_guard      = self->cspace_root_guard;
            pending_guard_bits = self->cspace_root_guard_bits;
        }
    }

    for (uint32_t depth = 0; depth < CSPACE_MAX_DEPTH; depth++) {
        uint32_t radix = (uint32_t)__builtin_ctzll((uint64_t)cur->slot_count);
        uint32_t idx   = (uint32_t)(cptr & ((uint64_t)cur->slot_count - 1u));
        cptr >>= radix;

        /*
         * Consume and check the guard AFTER the index, which puts it in the
         * more significant half of the level: MSB..LSB the level reads
         * [guard][index], the same relative order seL4 resolves in.  A
         * mismatch is a hard failure, never a fallthrough to another slot —
         * that is the whole point of a guard.
         */
        if (pending_guard_bits) {
            uint64_t mask = (pending_guard_bits >= 64u)
                          ? ~(uint64_t)0
                          : (((uint64_t)1 << pending_guard_bits) - 1u);
            if ((cptr & mask) != pending_guard) {
                kobject_active_release(&cur->base);
                kobject_release(&cur->base);
                return IRIS_ERR_NOT_FOUND;
            }
            cptr >>= pending_guard_bits;
        }

        struct KObject *slot_obj;
        iris_rights_t   slot_rights;
        uint64_t slot_badge = 0;
        uint64_t next_guard = 0;
        uint8_t  next_guard_bits = 0;
        err = kcnode_fetch_guarded(cur, idx, &slot_obj, &slot_rights,
                                   &slot_badge, &next_guard, &next_guard_bits);
        kobject_active_release(&cur->base);
        kobject_release(&cur->base);
        cur = 0;

        if (err != IRIS_OK) return err;

        if (cptr != 0 && slot_obj->type != KOBJ_CNODE) {
            /* Bits remain but there is nothing left to descend into: the CPtr
             * is malformed, not an address of this capability.
             *
             * Accepting it — which is what "terminal := exhausted OR not a
             * CNode" used to do — silently DISCARDED the leftover bits, so
             * every capability had ~2^23 aliases in a 256-slot root: CPtr k,
             * k + 256, k + 512 … all resolved to slot k.  A capability
             * address space whose addresses are not injective cannot be
             * reasoned about: "the cap at X" stops being a fact, an
             * off-by-one in a CPtr computation silently hits a live
             * capability instead of failing, and a value chosen to be
             * INVALID (the fuzz suite's 4095) quietly aliases a real one.
             * seL4 rejects the same shape as a depth mismatch. */
            kobject_active_release(slot_obj);
            kobject_release(slot_obj);
            return IRIS_ERR_INVALID_ARG;
        }

        if (cptr == 0) {
            /* Terminal capability found — check rights if required. */
            if (required != RIGHT_NONE && !rights_check(slot_rights, required)) {
                kobject_active_release(slot_obj);
                kobject_release(slot_obj);
                return IRIS_ERR_ACCESS_DENIED;
            }
            *obj_out    = slot_obj;
            *rights_out = slot_rights;
            if (badge_out)
                *badge_out = slot_badge;
            return IRIS_OK;
        }

        /* Intermediate CNode — descend one level, carrying that capability's
         * guard so the next iteration checks it before indexing. */
        cur                = (struct KCNode *)slot_obj;
        pending_guard      = next_guard;
        pending_guard_bits = next_guard_bits;
    }

    /* Depth limit exhausted without reaching a terminal. */
    if (cur) {
        kobject_active_release(&cur->base);
        kobject_release(&cur->base);
    }
    return IRIS_ERR_INVALID_ARG;
}

iris_error_t cspace_resolve_cap(struct KCNode     *root,
                                 iris_cptr_t        cptr,
                                 iris_rights_t      required,
                                 struct KObject   **obj_out,
                                 iris_rights_t     *rights_out)
{
    return cspace_resolve_cap_badged(root, cptr, required, obj_out,
                                     rights_out, 0);
}

/*
 * Phase S3 — resolve a CPtr to its terminal SLOT LOCATION (CNode + index)
 * instead of the object it contains.  This is what gives the MDB a source
 * identity: derivation (SYS_CSPACE_MINT / MINT_INTO), revocation
 * (SYS_CSPACE_REVOKE) and retype ancestry operate on slots, not objects.
 *
 * Same radix walk and namespace rules as cspace_resolve_cap (CPtr < 1024
 * territory only; the caller guards the namespace split).  On success the
 * containing CNode is returned with active+lifecycle refs (caller releases
 * both) and *idx_out is the slot index.  The terminal slot is guaranteed
 * occupied at resolution time (an empty slot fails NOT_FOUND).
 */
iris_error_t cspace_resolve_slot(struct KCNode   *root, iris_cptr_t cptr,
                                 struct KCNode **cn_out, uint32_t *idx_out)
{
    if (!cn_out || !idx_out) return IRIS_ERR_INVALID_ARG;
    if (cptr == CPTR_NULL) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    /* Stage 4: the root is a structural back-reference — resolving a CPtr no
     * longer begins with a handle-table lookup.  We take the same retain +
     * active_retain pair the handle read used to yield, so every existing
     * release path downstream is unchanged. */
    struct KObject *root_obj = &root->base;
    iris_error_t    err;
    kobject_retain(root_obj);
    kobject_active_retain(root_obj);
    struct KCNode *cur = (struct KCNode *)root_obj;

    for (uint32_t depth = 0; depth < CSPACE_MAX_DEPTH; depth++) {
        uint32_t radix = (uint32_t)__builtin_ctzll((uint64_t)cur->slot_count);
        uint32_t idx   = (uint32_t)(cptr & ((uint64_t)cur->slot_count - 1u));
        cptr >>= radix;

        struct KObject *slot_obj;
        iris_rights_t   slot_rights;
        err = kcnode_fetch(cur, idx, &slot_obj, &slot_rights);
        if (err != IRIS_OK) {
            kobject_active_release(&cur->base);
            kobject_release(&cur->base);
            return err;
        }

        if (cptr != 0 && slot_obj->type != KOBJ_CNODE) {
            /* Leftover bits with nothing to descend into — malformed CPtr.
             * Same rule as cspace_resolve_cap_badged; see the note there on
             * why silently dropping the remainder is not acceptable. */
            kobject_active_release(slot_obj);
            kobject_release(slot_obj);
            kobject_active_release(&cur->base);
            kobject_release(&cur->base);
            return IRIS_ERR_INVALID_ARG;
        }

        if (cptr == 0) {
            /* Terminal slot: (cur, idx).  Keep cur's refs for the caller;
             * drop the probe refs on the slot object. */
            kobject_active_release(slot_obj);
            kobject_release(slot_obj);
            *cn_out  = cur;
            *idx_out = idx;
            return IRIS_OK;
        }

        /* Intermediate CNode — descend (transfer refs to the child). */
        kobject_active_release(&cur->base);
        kobject_release(&cur->base);
        cur = (struct KCNode *)slot_obj;
    }

    kobject_active_release(&cur->base);
    kobject_release(&cur->base);
    return IRIS_ERR_INVALID_ARG;
}


iris_error_t cspace_resolve_dest_slot(struct KCNode   *root, iris_cptr_t cptr,
                                      struct KCNode **cn_out, uint32_t *idx_out)
{
    if (!cn_out || !idx_out) return IRIS_ERR_INVALID_ARG;
    if (cptr == CPTR_NULL) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    struct KObject *root_obj = &root->base;
    kobject_retain(root_obj);
    kobject_active_retain(root_obj);
    struct KCNode *cur = (struct KCNode *)root_obj;

    for (uint32_t depth = 0; depth < CSPACE_MAX_DEPTH; depth++) {
        uint32_t radix = (uint32_t)__builtin_ctzll((uint64_t)cur->slot_count);
        uint32_t idx   = (uint32_t)(cptr & ((uint64_t)cur->slot_count - 1u));
        cptr >>= radix;

        if (cptr == 0) {
            /* Path exhausted: (cur, idx) is the destination, occupied or not. */
            *cn_out  = cur;
            *idx_out = idx;
            return IRIS_OK;
        }

        /* More path left — this level must hold a CNode to descend into. */
        struct KObject *slot_obj;
        iris_rights_t   slot_rights;
        iris_error_t err = kcnode_fetch(cur, idx, &slot_obj, &slot_rights);
        if (err != IRIS_OK) {
            kobject_active_release(&cur->base);
            kobject_release(&cur->base);
            return err;                      /* empty intermediate: broken path */
        }
        if (slot_obj->type != KOBJ_CNODE) {
            /* Malformed CPtr — one answer across all three resolvers. */
            kobject_active_release(slot_obj);
            kobject_release(slot_obj);
            kobject_active_release(&cur->base);
            kobject_release(&cur->base);
            return IRIS_ERR_INVALID_ARG;
        }

        kobject_active_release(&cur->base);
        kobject_release(&cur->base);
        cur = (struct KCNode *)slot_obj;
    }

    kobject_active_release(&cur->base);
    kobject_release(&cur->base);
    return IRIS_ERR_INVALID_ARG;
}


/* Typed resolve helper — validates object type after generic traversal. */
#define TYPED_RESOLVE(fn, member_type, kobj_tag)                         \
iris_error_t fn(struct KCNode   *root, iris_cptr_t cptr,                  \
                 iris_rights_t required,                                   \
                 member_type **out, iris_rights_t *rights_out)             \
{                                                                          \
    struct KObject *obj; iris_rights_t r;                                  \
    iris_error_t err = cspace_resolve_cap(root, cptr, required, &obj, &r);\
    if (err != IRIS_OK) return err;                                        \
    if (obj->type != (kobj_tag)) {                                         \
        kobject_active_release(obj); kobject_release(obj);                 \
        return IRIS_ERR_WRONG_TYPE;                                        \
    }                                                                      \
    *out = (member_type *)obj; *rights_out = r;                            \
    return IRIS_OK;                                                        \
}

TYPED_RESOLVE(cspace_resolve_endpoint,    struct KEndpoint,    KOBJ_ENDPOINT)
TYPED_RESOLVE(cspace_resolve_reply,       struct KReply,       KOBJ_REPLY)
TYPED_RESOLVE(cspace_resolve_cnode,       struct KCNode,       KOBJ_CNODE)
TYPED_RESOLVE(cspace_resolve_notification,struct KNotification,KOBJ_NOTIFICATION)
TYPED_RESOLVE(cspace_resolve_tcb,         struct task,         KOBJ_TCB)
TYPED_RESOLVE(cspace_resolve_untyped,     struct KUntyped,     KOBJ_UNTYPED)
TYPED_RESOLVE(cspace_resolve_schedctx,    struct KSchedContext,KOBJ_SCHED_CONTEXT)
TYPED_RESOLVE(cspace_resolve_vspace,      struct KVSpace,      KOBJ_VSPACE)
TYPED_RESOLVE(cspace_resolve_frame,       struct KFrame,       KOBJ_FRAME)

/*
 * Phase 8: CPtr/handle namespace split for the DUAL resolvers.
 *
 * handle_ids are slot | generation<<HANDLE_GEN_SHIFT with generation >= 1,
 * so every live handle is >= 1024 and every direct CPtr argument is < 1024.
 * Before this split the dual resolvers fed handle values straight into
 * cspace_resolve_cap, whose radix walk MASKS the index (cptr & slot_count-1)
 * — a handle like 1027 silently aliased root slot 3 once Phase 8 populated
 * the low slots (wrong-object IPC / WRONG_TYPE hard stops).  The split makes
 * the documented ABI real:
 *   value <  1024 → CSpace namespace ONLY (no handle-table fallback; a
 *                   missing slot fails cleanly, ACCESS_DENIED stays a
 *                   hard stop);
 *   value >= 1024 → handle-table namespace ONLY (never walks the CSpace).
 * Deep multi-level CPtr paths (>= 1024) remain reachable through the pure
 * CSpace syscalls (SYS_CSPACE_RESOLVE, CNode ops), which take unambiguous
 * CPtr arguments.
 *
 * A1 closeout — authority namespace contract (docs/architecture/
 * a1-authority-namespace-endgame.md):
 *   value <  1024 → CPtr: the CSpace is the CANONICAL namespace for
 *                   persistent, delegable authority.
 *   value >= 1024 → handle: a per-process EPHEMERAL materialization
 *                   (working set), never a second canonical namespace.
 * Rules that follow from it:
 *   - Every object type that carries persistent authority resolves through
 *     a dual resolver in its syscalls; NEW persistent object types MUST be
 *     CSpace-invocable from day one (add a dual resolver call, not a
 *     handle-table lookup).
 *   - Reply caps are the intentional ephemeral exception: one-shot,
 *     delivered by EP_RECV as a handle, never minted into a CNode.
 *   - SYS_CSPACE_RESOLVE / SYS_CNODE_FETCH materialize handles on purpose —
 *     that is the sanctioned CSpace→handle bridge, not a leak.
 *   - ACCESS_DENIED from the CSpace leg is a hard stop; there is NO
 *     fallback to the handle table (and none in the other direction).
 *   - Handles may only be created by the closed producer list documented
 *     in the A1 design doc; extending that list is a design decision.
 */
/* CSPACE_DIRECT_CPTR_LIMIT / cspace_value_is_cptr moved to <iris/nc/cspace.h>:
 * the namespace split has one definition, so the retirement is one edit. */

iris_error_t cspace_resolve_only_cnode(struct KCNode   *root,
                                             iris_cptr_t      cptr_or_handle,
                                             iris_rights_t    required,
                                             struct KCNode  **out,
                                             iris_rights_t   *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!out || !rights_out) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    /* CPtr namespace (< 1024): CSpace only — no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        /* TYPE before RIGHTS, which is the order seL4 checks them in and the
         * order that answers the caller's actual question.  A resolve that
         * checked rights first reported ACCESS_DENIED for a NOTIFICATION passed
         * where a frame goes — "you lack rights on that frame", about something
         * that is not a frame — and the resolver knew.  Rights are a property
         * OF a type; asking about them before knowing the type is asking the
         * wrong question first. */
        err = cspace_resolve_cap(root, cptr_or_handle, RIGHT_NONE, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_CNODE) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        if (required != RIGHT_NONE && !rights_check(r, required)) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_ACCESS_DENIED;
        }
        *out = (struct KCNode *)obj;
        *rights_out = r;
        return IRIS_OK;
    }

    /* Stage 4: there is no second namespace.  A value that is not a CPtr is a
     * malformed argument, not an address in another table. */
    return IRIS_ERR_INVALID_ARG;
}

/*
 * DUAL_RESOLVE_IPC — generates cspace_resolve_only_{endpoint,reply,notification}.
 *
 * Returns lifecycle-only ref.  The CSpace path calls cspace_resolve_cap (which
 * gives active+lifecycle), then releases the active ref before returning — IPC
 * callers must not hold active_refs across task_yield() because that would
 * suppress the close callback that wakes blocked tasks.  The handle-table fallback
 * gives lifecycle-only by construction; no active_retain is added.
 *
 * Caller releases with: kobject_release(&(*out)->base)
 */
#define DUAL_RESOLVE_IPC(fn, member_type, kobj_tag)                               \
iris_error_t fn(struct KCNode   *root, iris_cptr_t cptr_or_handle,                \
                 iris_rights_t required,                                           \
                 member_type **out, iris_rights_t *rights_out)                     \
{                                                                                  \
    struct KObject *obj; iris_rights_t r; iris_error_t err;                       \
    if (!out || !rights_out) return IRIS_ERR_INVALID_ARG;                \
    if (!root) return IRIS_ERR_NOT_FOUND;                \
    /* CPtr namespace (< 1024): CSpace only, no handle-table fallback. */         \
    if (cspace_value_is_cptr(cptr_or_handle)) {                                   \
        /* TYPE before RIGHTS — see cspace_resolve_only_cnode. */                 \
        err = cspace_resolve_cap(root, cptr_or_handle, RIGHT_NONE, &obj, &r);     \
        if (err != IRIS_OK) return err;                                            \
        if (obj->type != (kobj_tag)) {                                            \
            kobject_active_release(obj); kobject_release(obj);                    \
            return IRIS_ERR_WRONG_TYPE;                                           \
        }                                                                          \
        if (required != RIGHT_NONE && !rights_check(r, required)) {               \
            kobject_active_release(obj); kobject_release(obj);                    \
            return IRIS_ERR_ACCESS_DENIED;                                        \
        }                                                                          \
        kobject_active_release(obj); /* IPC: must not hold active ref */          \
        *out = (member_type *)obj; *rights_out = r;                               \
        return IRIS_OK;                                                            \
    }                                                                              \
    /* Stage 4: there is no second namespace — a non-CPtr is malformed. */    \
    return IRIS_ERR_INVALID_ARG;                                              \
}

DUAL_RESOLVE_IPC(cspace_resolve_only_endpoint,    struct KEndpoint,    KOBJ_ENDPOINT)
DUAL_RESOLVE_IPC(cspace_resolve_only_reply,       struct KReply,       KOBJ_REPLY)
DUAL_RESOLVE_IPC(cspace_resolve_only_notification,struct KNotification,KOBJ_NOTIFICATION)

/*
 * cspace_resolve_only_untyped — active+lifecycle ref contract.
 *
 * KUntyped operations never block (no task_yield inside INFO/RETYPE/RESET), so
 * holding active_refs for the duration of the syscall is safe.  The KUntyped
 * close callback is a no-op — there are no blocked tasks to wake.
 *
 * Caller releases with:
 *   kobject_active_release(&(*out)->base);
 *   kobject_release(&(*out)->base);
 */
iris_error_t cspace_resolve_only_untyped(struct KCNode    *root,
                                               iris_cptr_t       cptr_or_handle,
                                               iris_rights_t     required,
                                               struct KUntyped **out,
                                               iris_rights_t    *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!out || !rights_out) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    /* CPtr namespace (< 1024): CSpace only — no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        /* TYPE before RIGHTS, which is the order seL4 checks them in and the
         * order that answers the caller's actual question.  A resolve that
         * checked rights first reported ACCESS_DENIED for a NOTIFICATION passed
         * where a frame goes — "you lack rights on that frame", about something
         * that is not a frame — and the resolver knew.  Rights are a property
         * OF a type; asking about them before knowing the type is asking the
         * wrong question first. */
        err = cspace_resolve_cap(root, cptr_or_handle, RIGHT_NONE, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_UNTYPED) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        if (required != RIGHT_NONE && !rights_check(r, required)) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_ACCESS_DENIED;
        }
        *out = (struct KUntyped *)obj;
        *rights_out = r;
        return IRIS_OK;
    }

    /* Stage 4: there is no second namespace.  A value that is not a CPtr is a
     * malformed argument, not an address in another table. */
    return IRIS_ERR_INVALID_ARG;
}

/*
 * cspace_resolve_only_frame — active+lifecycle ref contract.
 *
 * KFrame is a Phase 5 object; no IPC blocking occurs in frame operations.
 * CSpace-first; ACCESS_DENIED is a hard stop.  Handle-table fallback adds
 * kobject_active_retain to match the cspace_resolve_cap return contract.
 */
iris_error_t cspace_resolve_only_frame(struct KCNode   *root,
                                             iris_cptr_t      cptr_or_handle,
                                             iris_rights_t    required,
                                             struct KFrame  **out,
                                             iris_rights_t   *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!out || !rights_out) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    /* CPtr namespace (< 1024): CSpace only — no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        /* TYPE before RIGHTS, which is the order seL4 checks them in and the
         * order that answers the caller's actual question.  A resolve that
         * checked rights first reported ACCESS_DENIED for a NOTIFICATION passed
         * where a frame goes — "you lack rights on that frame", about something
         * that is not a frame — and the resolver knew.  Rights are a property
         * OF a type; asking about them before knowing the type is asking the
         * wrong question first. */
        err = cspace_resolve_cap(root, cptr_or_handle, RIGHT_NONE, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_FRAME) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        if (required != RIGHT_NONE && !rights_check(r, required)) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_ACCESS_DENIED;
        }
        *out = (struct KFrame *)obj;
        *rights_out = r;
        return IRIS_OK;
    }

    /* Stage 4: there is no second namespace.  A value that is not a CPtr is a
     * malformed argument, not an address in another table. */
    return IRIS_ERR_INVALID_ARG;
}

/*
 * cspace_resolve_only_vspace — dual resolver for the VSpace argument of
 * SYS_FRAME_MAP/SYS_FRAME_UNMAP (Phase 25).  Same namespace split and
 * active+lifecycle ref contract as cspace_resolve_only_frame.  Before
 * Phase 25 those syscalls fed the VSpace value straight into the raw radix
 * walk, where a handle (>= 1024) was masked into low root slots — the exact
 * aliasing hazard the Phase 8 split closed for every other capability
 * argument.  The handle namespace now resolves honestly, which is what lets
 * a supervisor drive map-into-target with the SYS_PROCESS_VSPACE handle
 * directly (no permanent CSpace slot pin).
 */
iris_error_t cspace_resolve_only_vspace(struct KCNode   *root,
                                              iris_cptr_t      cptr_or_handle,
                                              iris_rights_t    required,
                                              struct KVSpace **out,
                                              iris_rights_t   *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!out || !rights_out) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    /* CPtr namespace (< 1024): CSpace only — no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        /* TYPE before RIGHTS, which is the order seL4 checks them in and the
         * order that answers the caller's actual question.  A resolve that
         * checked rights first reported ACCESS_DENIED for a NOTIFICATION passed
         * where a frame goes — "you lack rights on that frame", about something
         * that is not a frame — and the resolver knew.  Rights are a property
         * OF a type; asking about them before knowing the type is asking the
         * wrong question first. */
        err = cspace_resolve_cap(root, cptr_or_handle, RIGHT_NONE, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_VSPACE) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        if (required != RIGHT_NONE && !rights_check(r, required)) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_ACCESS_DENIED;
        }
        *out = (struct KVSpace *)obj;
        *rights_out = r;
        return IRIS_OK;
    }

    /* Stage 4: there is no second namespace.  A value that is not a CPtr is a
     * malformed argument, not an address in another table. */
    return IRIS_ERR_INVALID_ARG;
}

/*
 * Phase 13: generic dual resolver for device/authority caps (KIoPort, KIrqCap,
 * KBootstrapCap, …).  Same namespace split as the typed resolvers (CPtr < 1024
 * → CSpace only; >= 1024 → handle table only) but **lifecycle-only** ref
 * contract — lifecycle-only, matching what the retired handle lookup gave —
 * so callers that already
 * use that helper can switch with no change to their kobject_release path.
 * This lets device caps be CPtr-minted into a child's CNode and invoked by
 * CPtr, removing the last reason device caps had to travel over KChannel.
 * required==RIGHT_NONE leaves the rights check to the caller (preserving each
 * device syscall's existing rights logic).
 */
iris_error_t cspace_resolve_only_obj(struct KCNode    *root,
                                          iris_cptr_t       cptr_or_handle,
                                          iris_rights_t     required,
                                          uint32_t          expected_type,
                                          struct KObject  **out,
                                          iris_rights_t    *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!out || !rights_out) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    if (cspace_value_is_cptr(cptr_or_handle)) {
        /* TYPE before RIGHTS — see cspace_resolve_only_cnode. */
        err = cspace_resolve_cap(root, cptr_or_handle, RIGHT_NONE, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != expected_type) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        if (required != RIGHT_NONE && !rights_check(r, required)) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_ACCESS_DENIED;
        }
        /* Drop the traversal's active ref → lifecycle-only (handle contract). */
        kobject_active_release(obj);
        *out = obj;
        *rights_out = r;
        return IRIS_OK;
    }

    /* Stage 4: there is no second namespace.  A value that is not a CPtr is a
     * malformed argument, not an address in another table. */
    return IRIS_ERR_INVALID_ARG;
}

/*
 * Phase 9: badge-aware dual endpoint resolver for the EP send/call paths.
 * Same namespace split and lifecycle-only refcount contract as the
 * DUAL_RESOLVE_IPC endpoint resolver; additionally returns the badge of
 * the capability that was invoked (slot badge on the CSpace path, handle
 * badge on the handle path; 0 = unbadged).
 */
iris_error_t cspace_resolve_only_endpoint_badged(struct KCNode    *root,
                                                       iris_cptr_t       cptr_or_handle,
                                                       iris_rights_t     required,
                                                       struct KEndpoint **out,
                                                       iris_rights_t    *rights_out,
                                                       uint64_t         *badge_out)
{
    struct KObject *obj; iris_rights_t r; iris_error_t err;
    uint64_t badge = 0;

    if (!out || !rights_out) return IRIS_ERR_INVALID_ARG;
    if (!root) return IRIS_ERR_NOT_FOUND;

    /* CPtr namespace (< 1024): CSpace only, no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        /* TYPE before RIGHTS — see cspace_resolve_only_cnode. */
        err = cspace_resolve_cap_badged(root, cptr_or_handle, RIGHT_NONE,
                                        &obj, &r, &badge);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_ENDPOINT) {
            kobject_active_release(obj); kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        if (required != RIGHT_NONE && !rights_check(r, required)) {
            kobject_active_release(obj); kobject_release(obj);
            return IRIS_ERR_ACCESS_DENIED;
        }
        kobject_active_release(obj); /* IPC: must not hold active ref */
        *out = (struct KEndpoint *)obj;
        *rights_out = r;
        if (badge_out) *badge_out = badge;
        return IRIS_OK;
    }

    /* Stage 4: there is no second namespace.  A value that is not a CPtr is a
     * malformed argument, not an address in another table. */
    return IRIS_ERR_INVALID_ARG;
}
