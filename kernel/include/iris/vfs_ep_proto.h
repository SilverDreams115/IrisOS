/*
 * vfs_ep_proto.h — VFS service protocol over KEndpoint (Fase 7.1).
 *
 * Wire format: struct IrisMsg (iris/ipc_msg.h) following the conventions in
 * iris/endpoint_proto.h. All operations are EP_CALL + SYS_REPLY round trips.
 *
 * Design notes:
 *   - The endpoint protocol is STATELESS: there is no open-file table on the
 *     EP path. Reads carry an explicit (path, offset, len) triple, so a dead
 *     client leaves no server-side state behind and no sender identity is
 *     required (IrisMsg carries no kernel-stamped sender id / badge yet).
 *   - This is the ONLY VFS protocol (Fase 7.5): the legacy stateful KChannel
 *     open/read/close protocol (iris/vfs_proto.h) was removed with its last
 *     clients; VFS no longer owns a legacy service channel.
 *   - Requests never transfer capabilities (EP_CALL forbids request-side cap
 *     transfer); replies never transfer capabilities either.
 *
 * Reply convention (see endpoint_proto.h):
 *   reply.label == IRIS_EP_REPLY_OK  → words[0] = 0, payload in words[1..]/kbuf
 *   reply.label == IRIS_EP_REPLY_ERR → words[0] = (uint64_t)(uint32_t)iris_error_t
 *
 * Error semantics (all ops):
 *   IRIS_ERR_INVALID_ARG   — malformed request (missing/oversized/non-NUL
 *                            path, missing words, undeliverable payload)
 *   IRIS_ERR_NOT_FOUND     — no export with that name / index out of range
 *   IRIS_ERR_NOT_SUPPORTED — unknown opcode
 */

#ifndef IRIS_VFS_EP_PROTO_H
#define IRIS_VFS_EP_PROTO_H

#include <stdint.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>   /* Fase 28.1: IRIS_BADGE_FILEGRANT_* */

/* Service opcode range 0x0100–0xEFFF (endpoint_proto.h); VFS owns 0x01xx. */

/*
 * VFS_EP_OP_LIST — enumerate exports by visible index.
 *   Request:  words[0] = index, word_count >= 1. No bulk payload.
 *   Reply OK: words[1] = export size in bytes
 *             words[2] = name length (excluding NUL)
 *             kbuf     = NUL-terminated export name (buf_len = name_len + 1)
 *   Reply ERR: IRIS_ERR_NOT_FOUND when index >= number of ready exports
 *              (this is the normal end-of-listing condition).
 */
#define VFS_EP_OP_LIST     UINT64_C(0x0101)

/*
 * VFS_EP_OP_STAT — look up an export by name.
 *   Request:  kbuf = NUL-terminated path; 1 <= buf_len <= VFS_EP_PATH_MAX
 *             (buf_len includes the NUL).
 *   Reply OK: words[1] = export size in bytes.
 *   Reply ERR: IRIS_ERR_NOT_FOUND / IRIS_ERR_INVALID_ARG.
 */
#define VFS_EP_OP_STAT     UINT64_C(0x0102)

/*
 * VFS_EP_OP_READ_AT — stateless positional read.
 *   Request:  kbuf     = NUL-terminated path (as STAT)
 *             words[0] = byte offset
 *             words[1] = requested length (server clamps to VFS_EP_DATA_MAX)
 *             word_count >= 2.
 *   Reply OK: words[1] = bytes read (0 = EOF; offset >= size is EOF, not error)
 *             words[2] = total export size in bytes
 *             kbuf     = data (buf_len = bytes read)
 *   Reply ERR: IRIS_ERR_NOT_FOUND / IRIS_ERR_INVALID_ARG.
 *
 *   Note (EP_CALL buffer reuse): msg.buf_uptr is both the request payload
 *   (path) and the reply bulk destination (data) — the client must re-stage
 *   the path before every call.
 */
#define VFS_EP_OP_READ_AT  UINT64_C(0x0103)

/*
 * VFS_EP_OP_STATUS — service health/diagnostics summary (Fase 7.5).
 *   Request:  no words, no bulk payload required (extra words are ignored;
 *             a bulk payload is rejected as IRIS_ERR_INVALID_ARG).
 *   Reply OK: words[1] = number of ready exports
 *             words[2] = total exported bytes across ready exports
 *   The stateless protocol has no open-file table, so the legacy
 *   opens/capacity counters do not exist on this path.
 */
#define VFS_EP_OP_STATUS   UINT64_C(0x0104)

/* IRIS_EP_OP_PING (0xFF01, endpoint_proto.h) is also served: reply OK. */

/* ── Fase 28.1: VFS-enforced file grants ─────────────────────────────────────
 *
 * A file grant is an UNFORGEABLE, per-backing authority validated by the VFS
 * itself on every operation — a pathname is never authority.  The construction
 * composes three existing kernel guarantees:
 *
 *   1. sender_badge is KERNEL-STAMPED from the invoked capability (Fase 9);
 *      a client cannot write it.
 *   2. A badged endpoint cap can NEVER be re-badged (SYS_PROC_CSPACE_MINT);
 *      only a holder of an UNBADGED duplicable vfs.ep cap (a supervisor, by
 *      the Fase 10 grant-tightening rule) can mint session identities.
 *   3. Rights reduce monotonically on every mint.
 *
 * Roles, by badge class:
 *   IRIS_BADGE_FILEGRANT_ADMIN      grant administrator (a pager SUPERVISOR).
 *                                   May OPEN grants into any session, REVOKE
 *                                   backings by name, and RESET sessions.
 *   IRIS_BADGE_FILEGRANT_S(s)       grant SESSION s (a pager instance).  May
 *                                   invoke ONLY the session-scoped grant ops
 *                                   below, ONLY on grants of session s.  All
 *                                   name-based ops (LIST/STAT/READ_AT/STATUS)
 *                                   are ACCESS_DENIED for session badges: a
 *                                   compromised pager holding only its session
 *                                   cap cannot read, stat, or enumerate ANY
 *                                   file it was not granted, no matter what
 *                                   bytes it puts in a message.
 *   any other badge                 ordinary named-export client (unchanged);
 *                                   every GRANT_* op is ACCESS_DENIED.
 *
 * Grant record: (backing_id, generation, rights) bound to one export at open
 * time.  backing_id and generation are VFS-ISSUED (namespaced by the service
 * instance epoch), never caller-chosen.  Revoking a backing bumps the export
 * generation: every grant snapshotting an older generation fails CLOSED on
 * its next use — enforcement is in the VFS, regardless of any state the
 * holder keeps.  A VFS restart re-seeds exports under a fresh epoch and an
 * empty grant table, so pre-restart grants can never revive (A11/A12).
 *
 * Error semantics (grant ops):
 *   IRIS_ERR_ACCESS_DENIED — wrong badge class, session mismatch, missing
 *                            right, or a derive requesting rights beyond the
 *                            source grant (monotonicity).
 *   IRIS_ERR_NOT_FOUND     — no such grant slot / no such export (open).
 *   IRIS_ERR_CLOSED        — grant is stale: its backing was revoked or its
 *                            generation superseded.
 *   IRIS_ERR_INVALID_ARG   — malformed request.
 *   IRIS_ERR_TABLE_FULL    — session grant table exhausted (open/derive).
 */

/* Grant rights (bit flags).  Monotonic: a derive may only shrink the mask. */
#define VFS_FILE_RIGHT_STAT       1u   /* GRANT_STAT / size queries */
#define VFS_FILE_RIGHT_READ       2u   /* GRANT_READ_AT */
#define VFS_FILE_RIGHT_DUPLICATE  4u   /* GRANT_DERIVE (reduced-rights copies) */
#define VFS_FILE_RIGHT_REVOKE     8u   /* GRANT_REVOKE via the grant itself */
#define VFS_FILE_RIGHT_ALL        0xFu

/* The file-grant badge identities (IRIS_BADGE_FILEGRANT_*, session count and
 * the badge→session helper) live with the other well-known badges in
 * iris/endpoint_proto.h (included above via this header's users; included
 * here for standalone use). */
#define VFS_GRANT_SESSIONS          IRIS_FILEGRANT_SESSIONS

/* Grants per session (per pager instance). */
#define VFS_GRANTS_PER_SESSION      8u

/*
 * VFS_EP_OP_GRANT_OPEN — create a grant (ADMIN badge only).
 *   Request:  words[0] = session index, words[1] = rights mask (nonempty,
 *             subset of VFS_FILE_RIGHT_ALL); kbuf = NUL-terminated export name.
 *             The pathname appears HERE ONLY — at grant creation, presented by
 *             the admin; the session holder never sends a name again.
 *   Reply OK: words[1] = grant index (session-scoped),
 *             words[2] = backing_id (VFS-issued),
 *             words[3] = generation (VFS-issued).
 */
#define VFS_EP_OP_GRANT_OPEN          UINT64_C(0x0110)

/*
 * VFS_EP_OP_GRANT_STAT — size/identity via a grant (SESSION badge;
 * FILE_RIGHT_STAT).
 *   Request:  words[0] = grant index.
 *   Reply OK: words[1] = file size, words[2] = backing_id, words[3] = generation.
 */
#define VFS_EP_OP_GRANT_STAT          UINT64_C(0x0111)

/*
 * VFS_EP_OP_GRANT_READ_AT — positional read via a grant (SESSION badge;
 * FILE_RIGHT_READ).  No pathname anywhere in the request.
 *   Request:  words[0] = grant index, words[1] = byte offset,
 *             words[2] = length (clamped to VFS_EP_DATA_MAX).
 *   Reply OK: words[1] = bytes read (0 = EOF), words[2] = file size,
 *             kbuf = data.
 */
#define VFS_EP_OP_GRANT_READ_AT       UINT64_C(0x0112)

/*
 * VFS_EP_OP_GRANT_QUERY_IDENTITY — introspect a live grant (SESSION badge;
 * any rights).  Lets a pager verify a supervisor-registered backing identity
 * against the VFS-issued one before trusting it.
 *   Request:  words[0] = grant index.
 *   Reply OK: words[1] = backing_id, words[2] = generation, words[3] = rights.
 */
#define VFS_EP_OP_GRANT_QUERY_IDENTITY UINT64_C(0x0113)

/*
 * VFS_EP_OP_GRANT_DERIVE — reduced-rights copy (SESSION badge;
 * FILE_RIGHT_DUPLICATE on the source).  words[1] must be a nonempty SUBSET of
 * the source rights — requesting any right the source lacks is ACCESS_DENIED
 * (monotonic derivation; rights can never be recovered).
 *   Request:  words[0] = source grant index, words[1] = rights mask.
 *   Reply OK: words[1] = new grant index (same session, same backing+gen).
 */
#define VFS_EP_OP_GRANT_DERIVE        UINT64_C(0x0114)

/*
 * VFS_EP_OP_GRANT_REVOKE — revoke a BACKING (all grants on it, all sessions).
 * Bumps the export generation; existing grants fail CLOSED from now on.
 *   ADMIN badge:   kbuf = NUL-terminated export name.
 *   SESSION badge: words[0] = grant index; requires FILE_RIGHT_REVOKE.
 *   Reply OK: words[1] = new generation.
 */
#define VFS_EP_OP_GRANT_REVOKE        UINT64_C(0x0115)

/*
 * VFS_EP_OP_GRANT_SESSION_RESET — drop EVERY grant of a session (ADMIN badge).
 * Part of the supervisor's pager-restart protocol: reset the session BEFORE
 * re-minting the session cap into a fresh pager instance, so a restarted
 * pager can never reuse its predecessor's grants (A11).
 *   Request:  words[0] = session index.
 */
#define VFS_EP_OP_GRANT_SESSION_RESET UINT64_C(0x0116)

/* Maximum path length including NUL (matches legacy VFS_MAX_NAME). */
#define VFS_EP_PATH_MAX    64u

/* Boot contract: number of exports seeded before "[VFS] ep ready" (the
 * static boot exports; initrd exports come on top). Checked by init's diag
 * invariant. Moved here from iris/vfs_proto.h (removed, Fase 7.5). */
#define VFS_BOOT_EXPORT_COUNT 4u

/* Maximum data bytes per READ_AT reply (one IPC bulk buffer). */
#define VFS_EP_DATA_MAX    IRIS_IPC_BUF_SIZE

/* Service name under which svcmgr publishes the VFS endpoint. */
#define VFS_EP_SVC_NAME    "vfs.ep"

#endif /* IRIS_VFS_EP_PROTO_H */
