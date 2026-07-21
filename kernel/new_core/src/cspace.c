#include <iris/nc/cspace.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/handle_table.h>
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

iris_error_t cspace_resolve_cap_badged(struct KProcess   *proc,
                                        iris_cptr_t        cptr,
                                        iris_rights_t      required,
                                        struct KObject   **obj_out,
                                        iris_rights_t     *rights_out,
                                        uint64_t          *badge_out)
{
    if (!proc || !obj_out || !rights_out) return IRIS_ERR_INVALID_ARG;
    if (cptr == CPTR_NULL) return IRIS_ERR_INVALID_ARG;

    handle_id_t root_h = proc->cspace_root_h;
    if (root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;

    struct KObject *root_obj;
    iris_rights_t   root_rights;
    iris_error_t err = handle_table_get_object(&proc->handle_table, root_h,
                                               &root_obj, &root_rights);
    if (err != IRIS_OK) return err;
    if (root_obj->type != KOBJ_CNODE) {
        kobject_release(root_obj);
        return IRIS_ERR_INVALID_ARG;
    }

    /* handle_table_get_object adds only a lifecycle retain, not an active retain.
     * Add one active retain here so the loop can uniformly call both
     * kobject_active_release + kobject_release for every level including root,
     * without triggering premature close of the root CNode's slots. */
    kobject_active_retain(root_obj);
    struct KCNode *cur = (struct KCNode *)root_obj;

    for (uint32_t depth = 0; depth < CSPACE_MAX_DEPTH; depth++) {
        uint32_t radix = (uint32_t)__builtin_ctzll((uint64_t)cur->slot_count);
        uint32_t idx   = (uint32_t)(cptr & ((uint64_t)cur->slot_count - 1u));
        cptr >>= radix;

        struct KObject *slot_obj;
        iris_rights_t   slot_rights;
        uint64_t slot_badge = 0;
        err = kcnode_fetch_badged(cur, idx, &slot_obj, &slot_rights,
                                  &slot_badge);
        kobject_active_release(&cur->base);
        kobject_release(&cur->base);
        cur = 0;

        if (err != IRIS_OK) return err;

        if (cptr == 0 || slot_obj->type != KOBJ_CNODE) {
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

        /* Intermediate CNode — descend one level. */
        cur = (struct KCNode *)slot_obj;
    }

    /* Depth limit exhausted without reaching a terminal. */
    if (cur) {
        kobject_active_release(&cur->base);
        kobject_release(&cur->base);
    }
    return IRIS_ERR_INVALID_ARG;
}

iris_error_t cspace_resolve_cap(struct KProcess   *proc,
                                 iris_cptr_t        cptr,
                                 iris_rights_t      required,
                                 struct KObject   **obj_out,
                                 iris_rights_t     *rights_out)
{
    return cspace_resolve_cap_badged(proc, cptr, required, obj_out,
                                     rights_out, 0);
}

/*
 * Fase S3 — resolve a CPtr to its terminal SLOT LOCATION (CNode + index)
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
iris_error_t cspace_resolve_slot(struct KProcess *proc, iris_cptr_t cptr,
                                 struct KCNode **cn_out, uint32_t *idx_out)
{
    if (!proc || !cn_out || !idx_out) return IRIS_ERR_INVALID_ARG;
    if (cptr == CPTR_NULL) return IRIS_ERR_INVALID_ARG;

    handle_id_t root_h = proc->cspace_root_h;
    if (root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;

    struct KObject *root_obj;
    iris_rights_t   root_rights;
    iris_error_t err = handle_table_get_object(&proc->handle_table, root_h,
                                               &root_obj, &root_rights);
    if (err != IRIS_OK) return err;
    if (root_obj->type != KOBJ_CNODE) {
        kobject_release(root_obj);
        return IRIS_ERR_INVALID_ARG;
    }
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

        if (cptr == 0 || slot_obj->type != KOBJ_CNODE) {
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


/* Typed resolve helper — validates object type after generic traversal. */
#define TYPED_RESOLVE(fn, member_type, kobj_tag)                         \
iris_error_t fn(struct KProcess *proc, iris_cptr_t cptr,                  \
                 iris_rights_t required,                                   \
                 member_type **out, iris_rights_t *rights_out)             \
{                                                                          \
    struct KObject *obj; iris_rights_t r;                                  \
    iris_error_t err = cspace_resolve_cap(proc, cptr, required, &obj, &r);\
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
 * Fase 8: CPtr/handle namespace split for the DUAL resolvers.
 *
 * handle_ids are slot | generation<<HANDLE_GEN_SHIFT with generation >= 1,
 * so every live handle is >= 1024 and every direct CPtr argument is < 1024.
 * Before this split the dual resolvers fed handle values straight into
 * cspace_resolve_cap, whose radix walk MASKS the index (cptr & slot_count-1)
 * — a handle like 1027 silently aliased root slot 3 once Fase 8 populated
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
 *     handle_table_get_object call).
 *   - Reply caps are the intentional ephemeral exception: one-shot,
 *     delivered by EP_RECV as a handle, never minted into a CNode.
 *   - SYS_CSPACE_RESOLVE / SYS_CNODE_FETCH materialize handles on purpose —
 *     that is the sanctioned CSpace→handle bridge, not a leak.
 *   - ACCESS_DENIED from the CSpace leg is a hard stop; there is NO
 *     fallback to the handle table (and none in the other direction).
 *   - Handles may only be created by the closed producer list documented
 *     in the A1 design doc; extending that list is a design decision.
 */
#define CSPACE_DIRECT_CPTR_LIMIT ((iris_cptr_t)1u << HANDLE_GEN_SHIFT)

static inline int cspace_value_is_cptr(iris_cptr_t v) {
    return v != CPTR_NULL && v < CSPACE_DIRECT_CPTR_LIMIT;
}

iris_error_t cspace_or_handle_resolve_cnode(struct KProcess *proc,
                                             iris_cptr_t      cptr_or_handle,
                                             iris_rights_t    required,
                                             struct KCNode  **out,
                                             iris_rights_t   *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!proc || !out || !rights_out) return IRIS_ERR_INVALID_ARG;

    /* CPtr namespace (< 1024): CSpace only — no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        if (proc->cspace_root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;
        err = cspace_resolve_cap(proc, cptr_or_handle, required, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_CNODE) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        *out = (struct KCNode *)obj;
        *rights_out = r;
        return IRIS_OK;
    }

    /* Handle namespace (>= 1024): handle table only — never walks CSpace.
     * handle_table_get_object gives lifecycle retain only; we add
     * kobject_active_retain to match the cspace_resolve_cap return contract
     * (caller must release both). */
    err = handle_table_get_object(&proc->handle_table,
                                   (handle_id_t)cptr_or_handle, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != KOBJ_CNODE) {
        kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    if (required != RIGHT_NONE && !rights_check(r, required)) {
        kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    kobject_active_retain(obj);
    *out = (struct KCNode *)obj;
    *rights_out = r;
    return IRIS_OK;
}

/*
 * DUAL_RESOLVE_IPC — generates cspace_or_handle_resolve_{endpoint,reply,notification}.
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
iris_error_t fn(struct KProcess *proc, iris_cptr_t cptr_or_handle,                \
                 iris_rights_t required,                                           \
                 member_type **out, iris_rights_t *rights_out)                     \
{                                                                                  \
    struct KObject *obj; iris_rights_t r; iris_error_t err;                       \
    if (!proc || !out || !rights_out) return IRIS_ERR_INVALID_ARG;                \
    /* CPtr namespace (< 1024): CSpace only, no handle-table fallback. */         \
    if (cspace_value_is_cptr(cptr_or_handle)) {                                   \
        if (proc->cspace_root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;     \
        err = cspace_resolve_cap(proc, cptr_or_handle, required, &obj, &r);       \
        if (err != IRIS_OK) return err;                                            \
        if (obj->type != (kobj_tag)) {                                            \
            kobject_active_release(obj); kobject_release(obj);                    \
            return IRIS_ERR_WRONG_TYPE;                                           \
        }                                                                          \
        kobject_active_release(obj); /* IPC: must not hold active ref */          \
        *out = (member_type *)obj; *rights_out = r;                               \
        return IRIS_OK;                                                            \
    }                                                                              \
    /* Handle namespace (>= 1024): handle table only. */                          \
    err = handle_table_get_object(&proc->handle_table,                            \
                                   (handle_id_t)cptr_or_handle, &obj, &r);       \
    if (err != IRIS_OK) return err;                                                \
    if (obj->type != (kobj_tag)) { kobject_release(obj); return IRIS_ERR_WRONG_TYPE; } \
    if (required != RIGHT_NONE && !rights_check(r, required)) {                   \
        kobject_release(obj); return IRIS_ERR_ACCESS_DENIED;                      \
    }                                                                              \
    *out = (member_type *)obj; *rights_out = r;                                   \
    return IRIS_OK;                                                                \
}

DUAL_RESOLVE_IPC(cspace_or_handle_resolve_endpoint,    struct KEndpoint,    KOBJ_ENDPOINT)
DUAL_RESOLVE_IPC(cspace_or_handle_resolve_reply,       struct KReply,       KOBJ_REPLY)
DUAL_RESOLVE_IPC(cspace_or_handle_resolve_notification,struct KNotification,KOBJ_NOTIFICATION)

/*
 * cspace_or_handle_resolve_untyped — active+lifecycle ref contract.
 *
 * KUntyped operations never block (no task_yield inside INFO/RETYPE/RESET), so
 * holding active_refs for the duration of the syscall is safe.  The KUntyped
 * close callback is a no-op — there are no blocked tasks to wake.
 *
 * Caller releases with:
 *   kobject_active_release(&(*out)->base);
 *   kobject_release(&(*out)->base);
 */
iris_error_t cspace_or_handle_resolve_untyped(struct KProcess  *proc,
                                               iris_cptr_t       cptr_or_handle,
                                               iris_rights_t     required,
                                               struct KUntyped **out,
                                               iris_rights_t    *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!proc || !out || !rights_out) return IRIS_ERR_INVALID_ARG;

    /* CPtr namespace (< 1024): CSpace only — no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        if (proc->cspace_root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;
        err = cspace_resolve_cap(proc, cptr_or_handle, required, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_UNTYPED) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        *out = (struct KUntyped *)obj;
        *rights_out = r;
        return IRIS_OK;
    }

    /* Handle namespace (>= 1024): lifecycle retain only; add active_retain to match contract. */
    err = handle_table_get_object(&proc->handle_table,
                                   (handle_id_t)cptr_or_handle, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != KOBJ_UNTYPED) {
        kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    if (required != RIGHT_NONE && !rights_check(r, required)) {
        kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    kobject_active_retain(obj);
    *out = (struct KUntyped *)obj;
    *rights_out = r;
    return IRIS_OK;
}

/*
 * cspace_or_handle_resolve_frame — active+lifecycle ref contract.
 *
 * KFrame is a Fase 5 object; no IPC blocking occurs in frame operations.
 * CSpace-first; ACCESS_DENIED is a hard stop.  Handle-table fallback adds
 * kobject_active_retain to match the cspace_resolve_cap return contract.
 */
iris_error_t cspace_or_handle_resolve_frame(struct KProcess *proc,
                                             iris_cptr_t      cptr_or_handle,
                                             iris_rights_t    required,
                                             struct KFrame  **out,
                                             iris_rights_t   *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!proc || !out || !rights_out) return IRIS_ERR_INVALID_ARG;

    /* CPtr namespace (< 1024): CSpace only — no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        if (proc->cspace_root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;
        err = cspace_resolve_cap(proc, cptr_or_handle, required, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_FRAME) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        *out = (struct KFrame *)obj;
        *rights_out = r;
        return IRIS_OK;
    }

    err = handle_table_get_object(&proc->handle_table,
                                   (handle_id_t)cptr_or_handle, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != KOBJ_FRAME) {
        kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    if (required != RIGHT_NONE && !rights_check(r, required)) {
        kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    kobject_active_retain(obj);
    *out = (struct KFrame *)obj;
    *rights_out = r;
    return IRIS_OK;
}

/*
 * cspace_or_handle_resolve_vspace — dual resolver for the VSpace argument of
 * SYS_FRAME_MAP/SYS_FRAME_UNMAP (Fase 25).  Same namespace split and
 * active+lifecycle ref contract as cspace_or_handle_resolve_frame.  Before
 * Fase 25 those syscalls fed the VSpace value straight into the raw radix
 * walk, where a handle (>= 1024) was masked into low root slots — the exact
 * aliasing hazard the Fase 8 split closed for every other capability
 * argument.  The handle namespace now resolves honestly, which is what lets
 * a supervisor drive map-into-target with the SYS_PROCESS_VSPACE handle
 * directly (no permanent CSpace slot pin).
 */
iris_error_t cspace_or_handle_resolve_vspace(struct KProcess *proc,
                                              iris_cptr_t      cptr_or_handle,
                                              iris_rights_t    required,
                                              struct KVSpace **out,
                                              iris_rights_t   *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!proc || !out || !rights_out) return IRIS_ERR_INVALID_ARG;

    /* CPtr namespace (< 1024): CSpace only — no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        if (proc->cspace_root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;
        err = cspace_resolve_cap(proc, cptr_or_handle, required, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_VSPACE) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        *out = (struct KVSpace *)obj;
        *rights_out = r;
        return IRIS_OK;
    }

    err = handle_table_get_object(&proc->handle_table,
                                   (handle_id_t)cptr_or_handle, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != KOBJ_VSPACE) {
        kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    if (required != RIGHT_NONE && !rights_check(r, required)) {
        kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    kobject_active_retain(obj);
    *out = (struct KVSpace *)obj;
    *rights_out = r;
    return IRIS_OK;
}

/*
 * Fase 13: generic dual resolver for device/authority caps (KIoPort, KIrqCap,
 * KBootstrapCap, …).  Same namespace split as the typed resolvers (CPtr < 1024
 * → CSpace only; >= 1024 → handle table only) but **lifecycle-only** ref
 * contract — identical to handle_table_get_object — so callers that already
 * use that helper can switch with no change to their kobject_release path.
 * This lets device caps be CPtr-minted into a child's CNode and invoked by
 * CPtr, removing the last reason device caps had to travel over KChannel.
 * required==RIGHT_NONE leaves the rights check to the caller (preserving each
 * device syscall's existing rights logic).
 */
iris_error_t cspace_or_handle_resolve_obj(struct KProcess  *proc,
                                          iris_cptr_t       cptr_or_handle,
                                          iris_rights_t     required,
                                          uint32_t          expected_type,
                                          struct KObject  **out,
                                          iris_rights_t    *rights_out)
{
    struct KObject *obj;
    iris_rights_t   r;
    iris_error_t    err;

    if (!proc || !out || !rights_out) return IRIS_ERR_INVALID_ARG;

    if (cspace_value_is_cptr(cptr_or_handle)) {
        if (proc->cspace_root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;
        err = cspace_resolve_cap(proc, cptr_or_handle, required, &obj, &r);
        if (err != IRIS_OK) return err;
        if (obj->type != expected_type) {
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        /* Drop the traversal's active ref → lifecycle-only (handle contract). */
        kobject_active_release(obj);
        *out = obj;
        *rights_out = r;
        return IRIS_OK;
    }

    err = handle_table_get_object(&proc->handle_table,
                                   (handle_id_t)cptr_or_handle, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != expected_type) {
        kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    if (required != RIGHT_NONE && !rights_check(r, required)) {
        kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    *out = obj;
    *rights_out = r;
    return IRIS_OK;
}

/*
 * Fase 9: badge-aware dual endpoint resolver for the EP send/call paths.
 * Same namespace split and lifecycle-only refcount contract as the
 * DUAL_RESOLVE_IPC endpoint resolver; additionally returns the badge of
 * the capability that was invoked (slot badge on the CSpace path, handle
 * badge on the handle path; 0 = unbadged).
 */
iris_error_t cspace_or_handle_resolve_endpoint_badged(struct KProcess  *proc,
                                                       iris_cptr_t       cptr_or_handle,
                                                       iris_rights_t     required,
                                                       struct KEndpoint **out,
                                                       iris_rights_t    *rights_out,
                                                       uint64_t         *badge_out)
{
    struct KObject *obj; iris_rights_t r; iris_error_t err;
    uint64_t badge = 0;

    if (!proc || !out || !rights_out) return IRIS_ERR_INVALID_ARG;

    /* CPtr namespace (< 1024): CSpace only, no handle-table fallback. */
    if (cspace_value_is_cptr(cptr_or_handle)) {
        if (proc->cspace_root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;
        err = cspace_resolve_cap_badged(proc, cptr_or_handle, required,
                                        &obj, &r, &badge);
        if (err != IRIS_OK) return err;
        if (obj->type != KOBJ_ENDPOINT) {
            kobject_active_release(obj); kobject_release(obj);
            return IRIS_ERR_WRONG_TYPE;
        }
        kobject_active_release(obj); /* IPC: must not hold active ref */
        *out = (struct KEndpoint *)obj;
        *rights_out = r;
        if (badge_out) *badge_out = badge;
        return IRIS_OK;
    }

    /* Handle namespace (>= 1024): handle table only. */
    err = handle_table_get_object(&proc->handle_table,
                                   (handle_id_t)cptr_or_handle, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != KOBJ_ENDPOINT) {
        kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    if (required != RIGHT_NONE && !rights_check(r, required)) {
        kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    *out = (struct KEndpoint *)obj;
    *rights_out = r;
    if (badge_out)
        *badge_out = handle_table_get_badge(&proc->handle_table,
                                            (handle_id_t)cptr_or_handle);
    return IRIS_OK;
}
