#ifndef IRIS_PAGER_PROTO_H
#define IRIS_PAGER_PROTO_H

#include <stdint.h>

/*
 * pager_proto.h — wire contract between the pager service and its supervisor
 * (Fase 27 model, own binary since Fase 28, file grants + multi-target since
 * Fase 28.1).  Shared by services/pager/main.c and the iris_test supervisor
 * so the two never drift.
 *
 * The pager is a persistent, supervised userland service.  Its ENTIRE
 * authority is a manifest of caps minted into well-known CSpace slots BEFORE
 * it starts; it never acquires anything at runtime.  The supervisor drives it
 * request/reply over a control endpoint (EP_CALL → SYS_REPLY).
 *
 * Manifest slots (in the pager's own CSpace; all others are absent):
 *   slot 3  PGR_SLOT_CTRL_EP     control endpoint the pager serves (RIGHT_READ)
 *   slot 4  PGR_SLOT_VFS_EP     the pager's VFS SESSION cap: a badged
 *                               (IRIS_BADGE_FILEGRANT_S(session)), WRITE-only
 *                               vfs.ep cap.  Fase 28.1: this is NOT a generic
 *                               VFS client cap — the VFS confines the badge to
 *                               the session-scoped GRANT ops and denies every
 *                               name-based op, so the slot carries exactly
 *                               "the backings my supervisor granted me" and
 *                               nothing else, even if this pager is hostile.
 *   slot 5  PGR_SLOT_FAULT_NOTIF the SHARED fault notification (RIGHT_WAIT).
 *                               One KNotification for ALL targets: the
 *                               supervisor registers each target's exception
 *                               handler on it with signal bit (1 << tidx).
 *                               Fase 28.1: replaces the per-target
 *                               notification column — 16 concurrent targets
 *                               cost ONE notification against the supervisor's
 *                               KPROCESS_NOTIFICATION_QUOTA, not 16.
 *   slot 16/17 PGR_VSLOT(j)     VMO grants (RO cache / private pool, or raw
 *                               Fase 27 VMOs), RIGHT_READ [+ RIGHT_WRITE]
 *   target i (i < PGR_MAX_TARGETS = 16):
 *     proc PGR_TSLOT_PROC(i) = 20 + i*2   RIGHT_READ | RIGHT_MANAGE
 *     vs   PGR_TSLOT_VS(i)   = 21 + i*2   RIGHT_WRITE  (map-into authority)
 *
 * Nothing here is a spawn cap, device cap, untyped, KDEBUG, or a nameable
 * VFS authority.
 */

#define PGR_SLOT_CTRL_EP      3u
#define PGR_SLOT_VFS_EP       4u
#define PGR_SLOT_FAULT_NOTIF  5u

#define PGR_MAX_TARGETS    16u
#define PGR_MAX_VMOS       2u

#define PGR_TGT_BASE       20u
#define PGR_TGT_STRIDE     2u
#define PGR_TSLOT_PROC(i)  (PGR_TGT_BASE + (i) * PGR_TGT_STRIDE + 0u)
#define PGR_TSLOT_VS(i)    (PGR_TGT_BASE + (i) * PGR_TGT_STRIDE + 1u)

#define PGR_VMO_BASE       16u
#define PGR_VSLOT(j)       (PGR_VMO_BASE + (j))

/* Control op codes (msg.words[0] bits [7:0]). */
#define PGR_OP_PING        1u
#define PGR_OP_REPORT      2u
#define PGR_OP_MAP_RESUME  3u   /* Fase 27: raw-VMO map from a VMO grant */
#define PGR_OP_KILL        4u
#define PGR_OP_SHUTDOWN    5u
#define PGR_OP_MAP_REGION  6u   /* Fase 28: resolve a file-backed fault (target tidx) */
#define PGR_OP_REGISTER_BACKING   7u   /* buffer = struct pgr_backing_req */
#define PGR_OP_REGISTER_REGION    8u   /* buffer = struct pgr_region_req */
#define PGR_OP_UNREGISTER_REGION  9u   /* words[1] = region_idx */
#define PGR_OP_REVOKE_BACKING    10u   /* words[1] = backing_idx (local hygiene;
                                        * the AUTHORITY revoke is VFS_EP_OP_GRANT_REVOKE) */
#define PGR_OP_DIAG              11u   /* reply: struct pgr_diag in the REPLY buffer */
#define PGR_OP_TARGET_RESET      12u   /* words[1] = tidx: drop the target's regions
                                        * and its pending fault bit (death cleanup) */

/* ── Fase 28 file-backed subsystem (grant-based since Fase 28.1) ────────────
 * The pager reads file bytes EXCLUSIVELY through VFS file grants
 * (VFS_EP_OP_GRANT_READ_AT over its badged session cap).  A backing is
 * registered by the supervisor as (grant_idx, backing_id, generation,
 * file_size) where backing_id/generation are the VFS-ISSUED identity returned
 * by GRANT_OPEN; the pager cross-checks them against
 * VFS_EP_OP_GRANT_QUERY_IDENTITY before accepting the backing, so a
 * supervisor typo cannot silently bind the wrong file.  No pathname exists
 * anywhere in this contract. */
#define PGR_VMO_CACHE      0u   /* VMO grant 0 (slot 16): RO shared page cache */
#define PGR_VMO_PRIVATE    1u   /* VMO grant 1 (slot 17): private-writable pool */

#define PGR_MAX_BACKINGS   4u
#define PGR_MAX_REGIONS    16u  /* Fase 28.1: one region per possible target */
#define PGR_CACHE_CAP      8u   /* pages in the RO cache VMO */
#define PGR_PRIV_CAP       8u   /* pages in the private-writable pool VMO */

/* Mapping modes. */
#define PGR_MODE_RO_SHARED         0u
#define PGR_MODE_PRIVATE_WRITABLE  1u
#define PGR_MODE_SHARED_WRITABLE   2u   /* rejected NOT_SUPPORTED (B4) */

/* Region protections (bit flags). */
#define PGR_PROT_R  1u
#define PGR_PROT_W  2u
#define PGR_PROT_X  4u

/* Wire structs, passed in the control-message bulk buffer (<= IRIS_IPC_BUF_SIZE). */
struct pgr_backing_req {
    uint32_t backing_idx;
    uint32_t grant_idx;        /* Fase 28.1: VFS file-grant index (session-scoped) */
    uint64_t backing_id;       /* VFS-issued (GRANT_OPEN reply) */
    uint64_t generation;       /* VFS-issued (GRANT_OPEN reply) */
    uint64_t file_size;
};
struct pgr_region_req {
    uint32_t region_idx;
    uint32_t target_idx;
    uint32_t backing_idx;
    uint32_t prot;             /* PGR_PROT_* */
    uint32_t mode;             /* PGR_MODE_* */
    uint32_t reserved;
    uint64_t start_va;
    uint64_t memory_length;
    uint64_t file_offset;
    uint64_t file_length;
    uint64_t backing_generation;
};

/* words[0] field packing. */
#define PGR_OP(w0)         ((uint32_t)((w0) & 0xFFu))
#define PGR_TIDX(w0)       ((uint32_t)(((w0) >> 8) & 0xFFu))
#define PGR_VIDX(w0)       ((uint32_t)(((w0) >> 16) & 0xFFu))
#define PGR_FLAGS(w0)      ((uint32_t)(((w0) >> 24) & 0x3u))
#define PGR_PACK(op, tidx, vidx, flags) \
    ((uint64_t)(op) | ((uint64_t)(tidx) << 8) | ((uint64_t)(vidx) << 16) | \
     ((uint64_t)((flags) & 0x3u) << 24))

/* words[1] = VMO byte offset (page-aligned); words[2] = expected cr2 (0=skip). */

/*
 * PGR_OP_REPORT reply words[0] — manifest presence mask (diagnostic oracle):
 *   bits 0..19  slots 0..19 present (ctrl 3, vfs 4, fault notif 5, VMOs 16/17)
 *   bit  20     any PGR_TSLOT_PROC(i) present
 *   bit  21     any PGR_TSLOT_VS(i) present
 *   bit  24     slot 6 present  (spawn cap — MUST be absent)
 *   bit  26     slot 55 present (untyped   — MUST be absent)
 *   bit  27     slot 56 present (vspace    — MUST be absent)
 */

/* Reply words[0] result: 0 = OK, else (uint32_t)(-err) or a marker below.
 * (Markers are returned negated so a caller can distinguish OK/err by sign.) */
#define PGR_ERR_BADOP      0x60u
#define PGR_ERR_INFO       0x61u
#define PGR_ERR_CR2        0x62u
#define PGR_ERR_NOFAULT    0x63u
#define PGR_ERR_NO_REGION  0x64u   /* fault VA in no registered region */
#define PGR_ERR_STALE_GEN  0x65u   /* region backing_generation != backing's current */
#define PGR_ERR_SHORT_READ 0x66u   /* VFS returned fewer bytes than expected mid-file */
#define PGR_ERR_CACHE_FULL 0x67u   /* no evictable (refcount-0) cache slot */
#define PGR_ERR_MODE       0x68u   /* unsupported mapping mode (shared-writable) */
#define PGR_ERR_ACCESS     0x69u   /* write fault into a read-only region */
#define PGR_ERR_RANGE      0x6Au   /* region validation failure */
#define PGR_ERR_NOBACK     0x6Bu   /* region references an unregistered backing */
#define PGR_ERR_PRIVFULL   0x6Cu   /* no free private-writable page */
#define PGR_ERR_GRANT      0x6Du   /* Fase 28.1: VFS denied the grant (bad index,
                                    * identity mismatch, missing right, revoked) */

/* DIAG reply: words[0]=OK(0); the counters come back in the reply bulk buffer as
 * a struct pgr_diag. */
struct pgr_diag {
    uint32_t backing_live;
    uint32_t region_count;
    uint32_t cache_capacity;
    uint32_t cache_entries;
    uint32_t cache_hit;
    uint32_t cache_miss;
    uint32_t cache_evict;
    uint32_t page_fill;
    uint32_t page_fill_fail;
    uint32_t generation_stale;
    uint32_t grant_revoke;
    uint32_t private_pages;
    /* Fase 28.1: multi-target fault multiplexing. */
    uint32_t notif_waits;      /* SYS_NOTIFY_WAIT_TIMEOUT calls on the shared notif */
    uint32_t notif_wakeups;    /* successful wakeups (bit sets consumed) */
    uint32_t pending_mask;     /* fault bits accumulated but not yet resolved */
    uint32_t target_resets;    /* PGR_OP_TARGET_RESET operations served */
    uint32_t grant_denied;     /* VFS grant-layer denials observed (page fills) */
};

#endif /* IRIS_PAGER_PROTO_H */
