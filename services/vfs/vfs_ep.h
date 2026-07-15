/*
 * vfs_ep.h — VFS endpoint-protocol dispatcher (Fase 7.1; grants Fase 28.1).
 *
 * The dispatcher is a pure function over (state, request) → reply with no
 * syscalls inside, so the host unit-test harness (tests/kernel) can exercise
 * the full opcode/validation surface without a kernel. vfs.c wraps it with
 * the EP_RECV / SYS_REPLY drain loop.
 *
 * Fase 28.1: the dispatcher also owns the FILE-GRANT layer — the VFS-enforced
 * authority table that makes a pathname worthless as authority.  The caller
 * class is derived from req->sender_badge (kernel-stamped, unforgeable);
 * session badges are confined to their own grants and denied every name-based
 * op.  See iris/vfs_ep_proto.h for the full contract.
 */
#ifndef IRIS_SERVICES_VFS_EP_H
#define IRIS_SERVICES_VFS_EP_H

#include <stdint.h>
#include <iris/ipc_msg.h>
#include <iris/vfs_ep_proto.h>

/* One exported file. Shared by vfs.c (owner) and the dispatcher (reader).
 * is_mapped exports read through virt_base (initrd VMO mappings); inline
 * exports read from data[].
 * Fase 28.1: every ready export carries a VFS-ISSUED backing identity —
 * backing_id (stable for this service instance) and generation (bumped on
 * revoke; namespaced by the instance epoch so a restarted VFS never reissues
 * an old value). */
struct vfs_export {
    char     name[VFS_EP_PATH_MAX];
    uint8_t  data[512];
    uint32_t size;
    uint8_t  ready;
    uint8_t  is_mapped;
    uint64_t virt_base;
    uint64_t backing_id;    /* Fase 28.1: VFS-issued, never caller-chosen */
    uint64_t generation;    /* Fase 28.1: bumps on GRANT_REVOKE */
};

/* One file grant: an unforgeable authority over exactly one backing at one
 * generation, held by one session.  The record is VFS state — nothing the
 * holder stores or forges can widen it. */
struct vfs_grant {
    uint8_t  used;
    uint8_t  export_slot;   /* index into exports[] */
    uint32_t rights;        /* VFS_FILE_RIGHT_* mask (monotonic) */
    uint64_t backing_id;    /* snapshot at open/derive */
    uint64_t generation;    /* snapshot at open/derive; stale ⇒ CLOSED */
};

/* Grant table: VFS_GRANT_SESSIONS × VFS_GRANTS_PER_SESSION.  A session is
 * addressed ONLY by the kernel-stamped badge of the invoking cap. */
struct vfs_grant_table {
    struct vfs_grant g[VFS_GRANT_SESSIONS][VFS_GRANTS_PER_SESSION];
    uint32_t         gen_seq;   /* per-instance revoke sequence (low half) */
    uint64_t         epoch;     /* instance epoch (high half of generations) */
};

/* Dispatcher state: the export table plus the grant table.  Exports are
 * mutable because GRANT_REVOKE bumps an export's generation. */
struct vfs_ep_state {
    struct vfs_export      *exports;
    uint32_t                export_count;
    struct vfs_grant_table *grants;
};

/* Seed VFS-issued identities onto every ready export and initialize the grant
 * table under `epoch` (the service-instance epoch, e.g. the svcmgr restart
 * generation).  Idempotent per boot; called once after export seeding. */
void vfs_ep_grants_init(struct vfs_ep_state *st, uint64_t epoch);

/*
 * vfs_ep_dispatch — handle one endpoint request.
 *
 *   st:        export + grant tables (see above).
 *   req:       received IrisMsg (label, words, buf_len, sender_badge as
 *              delivered by the kernel).
 *   req_buf:   bulk payload bytes for req (valid for req->buf_len bytes when
 *              the payload was delivered; pass NULL if it was not).
 *   reply:     fully written by the dispatcher (label/words/word_count/buf_*).
 *   reply_buf: VFS_EP_DATA_MAX-byte buffer for reply bulk data; on return
 *              reply->buf_uptr points at it iff reply->buf_len > 0.
 *
 * Exactly one reply is produced for every request, including malformed ones.
 */
void vfs_ep_dispatch(struct vfs_ep_state *st,
                     const struct IrisMsg *req, const uint8_t *req_buf,
                     struct IrisMsg *reply, uint8_t *reply_buf);

#endif /* IRIS_SERVICES_VFS_EP_H */
