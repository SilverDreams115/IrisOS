/*
 * vfs_ep.c — VFS endpoint-protocol dispatcher (Fase 7.1; grants Fase 28.1).
 *
 * Pure request → reply logic for the IrisMsg-based VFS protocol
 * (iris/vfs_ep_proto.h). No syscalls, no globals: unit-testable on the host
 * (tests/kernel/test_vfs_ep.c) and wrapped by the drain loop in vfs.c.
 *
 * Invariants enforced here:
 *   - exactly one reply per request (caller sends it; we always fill it);
 *   - unknown opcode → IRIS_EP_REPLY_ERR / IRIS_ERR_NOT_SUPPORTED;
 *   - malformed payload (empty, oversized, non-NUL-terminated, undelivered)
 *     → IRIS_EP_REPLY_ERR / IRIS_ERR_INVALID_ARG;
 *   - reads are clamped to VFS_EP_DATA_MAX; offset >= size is EOF, not error.
 *
 * Fase 28.1 — the file-grant trust boundary lives HERE, in the VFS, not in
 * any client:
 *   - the caller class comes from req->sender_badge (kernel-stamped from the
 *     invoked capability — a client cannot write it);
 *   - a SESSION badge may invoke only the session-scoped grant ops, only on
 *     grants of ITS session; every name-based op is ACCESS_DENIED, so a
 *     compromised holder of a session cap cannot read/stat/enumerate any
 *     export by name no matter what message it constructs;
 *   - grant validity (backing generation) is re-checked on EVERY operation:
 *     a revoked backing fails CLOSED even for a holder that kept the index,
 *     the name, an old message, or its own private table;
 *   - rights are monotonic: GRANT_DERIVE may only shrink the mask.
 */

#include "vfs_ep.h"
#include <iris/endpoint_proto.h>
#include <iris/nc/error.h>

static void vfs_ep_msg_clear(struct IrisMsg *m) {
    uint8_t *p = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) p[i] = 0;
}

static void vfs_ep_reply_err(struct IrisMsg *reply, iris_error_t err) {
    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_ERR;
    reply->words[0]   = (uint64_t)(uint32_t)err;
    reply->word_count = 1u;
}

/*
 * Validate and extract the request path from the bulk payload.
 * Returns NULL if the payload is malformed (caller replies INVALID_ARG).
 */
static const char *vfs_ep_req_path(const struct IrisMsg *req,
                                   const uint8_t *req_buf) {
    if (!req_buf) return 0;                       /* payload not delivered */
    if (req->buf_len == 0u) return 0;             /* short payload */
    if (req->buf_len > VFS_EP_PATH_MAX) return 0; /* long payload */
    if (req_buf[req->buf_len - 1u] != 0u) return 0; /* not NUL-terminated */
    if (req_buf[0] == 0u) return 0;               /* empty name */
    return (const char *)req_buf;
}

static int vfs_ep_name_equal(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static const struct vfs_export *vfs_ep_find_export(const struct vfs_export *exports,
                                                   uint32_t export_count,
                                                   const char *path) {
    for (uint32_t i = 0; i < export_count; i++) {
        if (!exports[i].ready) continue;
        if (vfs_ep_name_equal(exports[i].name, path)) return &exports[i];
    }
    return 0;
}

static const struct vfs_export *vfs_ep_export_at_index(const struct vfs_export *exports,
                                                       uint32_t export_count,
                                                       uint64_t index) {
    uint64_t visible = 0;
    for (uint32_t i = 0; i < export_count; i++) {
        if (!exports[i].ready) continue;
        if (visible == index) return &exports[i];
        visible++;
    }
    return 0;
}

static const uint8_t *vfs_ep_export_bytes(const struct vfs_export *exp) {
    if (exp->is_mapped)
        return (const uint8_t *)(uintptr_t)exp->virt_base;
    return exp->data;
}

static void vfs_ep_handle_list(const struct vfs_export *exports,
                               uint32_t export_count,
                               const struct IrisMsg *req,
                               struct IrisMsg *reply, uint8_t *reply_buf) {
    if (req->word_count < 1u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    const struct vfs_export *exp =
        vfs_ep_export_at_index(exports, export_count, req->words[0]);
    if (!exp) {
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_FOUND);
        return;
    }

    uint32_t name_len = 0;
    while (name_len + 1u < VFS_EP_PATH_MAX && exp->name[name_len]) name_len++;
    for (uint32_t i = 0; i < name_len; i++) reply_buf[i] = (uint8_t)exp->name[i];
    reply_buf[name_len] = 0u;

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = exp->size;
    reply->words[2]   = name_len;
    reply->word_count = 3u;
    reply->buf_len    = name_len + 1u;
    reply->buf_uptr   = (uint64_t)(uintptr_t)reply_buf;
}

static void vfs_ep_handle_stat(const struct vfs_export *exports,
                               uint32_t export_count,
                               const struct IrisMsg *req,
                               const uint8_t *req_buf,
                               struct IrisMsg *reply) {
    const char *path = vfs_ep_req_path(req, req_buf);
    if (!path) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    const struct vfs_export *exp = vfs_ep_find_export(exports, export_count, path);
    if (!exp) {
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_FOUND);
        return;
    }

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = exp->size;
    reply->word_count = 2u;
}

/* Common read body shared by the named READ_AT and GRANT_READ_AT paths: the
 * authority decision was already made by the caller; this only moves bytes. */
static void vfs_ep_read_reply(const struct vfs_export *exp,
                              uint64_t offset, uint64_t len,
                              struct IrisMsg *reply, uint8_t *reply_buf) {
    if (len > VFS_EP_DATA_MAX) len = VFS_EP_DATA_MAX;

    uint64_t bytes = 0;
    if (offset < exp->size) {
        uint64_t available = (uint64_t)exp->size - offset;
        bytes = (len < available) ? len : available;
        const uint8_t *src = vfs_ep_export_bytes(exp) + offset;
        for (uint64_t i = 0; i < bytes; i++) reply_buf[i] = src[i];
    }

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = bytes;
    reply->words[2]   = exp->size;
    reply->word_count = 3u;
    if (bytes > 0) {
        reply->buf_len  = (uint32_t)bytes;
        reply->buf_uptr = (uint64_t)(uintptr_t)reply_buf;
    }
}

static void vfs_ep_handle_read_at(const struct vfs_export *exports,
                                  uint32_t export_count,
                                  const struct IrisMsg *req,
                                  const uint8_t *req_buf,
                                  struct IrisMsg *reply, uint8_t *reply_buf) {
    if (req->word_count < 2u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    const char *path = vfs_ep_req_path(req, req_buf);
    if (!path) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    const struct vfs_export *exp = vfs_ep_find_export(exports, export_count, path);
    if (!exp) {
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_FOUND);
        return;
    }

    vfs_ep_read_reply(exp, req->words[0], req->words[1], reply, reply_buf);
}

static void vfs_ep_handle_status(const struct vfs_export *exports,
                                 uint32_t export_count,
                                 const struct IrisMsg *req,
                                 struct IrisMsg *reply) {
    if (req->buf_len > 0u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    uint64_t ready = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < export_count; i++) {
        if (!exports[i].ready) continue;
        ready++;
        bytes += exports[i].size;
    }

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = ready;
    reply->words[2]   = bytes;
    reply->word_count = 3u;
}

/* ── Fase 28.1: file-grant layer ────────────────────────────────────────── */

void vfs_ep_grants_init(struct vfs_ep_state *st, uint64_t epoch) {
    struct vfs_grant_table *gt = st->grants;
    uint8_t *p = (uint8_t *)gt;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*gt); i++) p[i] = 0;
    gt->epoch   = epoch;
    gt->gen_seq = 1u;
    /* VFS-issued identities: backing_id is stable per (instance, slot);
     * generation starts at (epoch<<16)|1 and only ever moves forward within
     * the instance.  A restarted VFS gets a fresh epoch, so no pre-restart
     * generation can ever validate again (A12). */
    for (uint32_t i = 0; i < st->export_count; i++) {
        if (!st->exports[i].ready) continue;
        st->exports[i].backing_id = 0xB1000000ull | (epoch << 8) | (uint64_t)i;
        st->exports[i].generation = (epoch << 16) | (uint64_t)gt->gen_seq;
    }
}

/* Resolve a session grant reference: badge class, session scope, slot
 * existence.  Liveness (generation) is NOT checked here — QUERY/STAT/READ
 * decide how staleness surfaces.  Returns the grant or NULL with *err set. */
static struct vfs_grant *vfs_ep_grant_ref(struct vfs_ep_state *st,
                                          const struct IrisMsg *req,
                                          iris_error_t *err) {
    int s = iris_badge_filegrant_session(req->sender_badge);
    if (s < 0) { *err = IRIS_ERR_ACCESS_DENIED; return 0; }
    if (req->word_count < 1u) { *err = IRIS_ERR_INVALID_ARG; return 0; }
    uint64_t idx = req->words[0];
    if (idx >= VFS_GRANTS_PER_SESSION) { *err = IRIS_ERR_NOT_FOUND; return 0; }
    struct vfs_grant *g = &st->grants->g[s][idx];
    if (!g->used) { *err = IRIS_ERR_NOT_FOUND; return 0; }
    return g;
}

/* Liveness: a grant is live iff its snapshotted generation is the export's
 * CURRENT generation (and the export is still ready).  Everything else —
 * revoked backing, superseded generation, re-seeded export — fails CLOSED. */
static const struct vfs_export *vfs_ep_grant_live(struct vfs_ep_state *st,
                                                  const struct vfs_grant *g,
                                                  iris_error_t *err) {
    const struct vfs_export *exp;
    if (g->export_slot >= st->export_count) { *err = IRIS_ERR_CLOSED; return 0; }
    exp = &st->exports[g->export_slot];
    if (!exp->ready || exp->backing_id != g->backing_id ||
        exp->generation != g->generation) {
        *err = IRIS_ERR_CLOSED;
        return 0;
    }
    return exp;
}

static int vfs_ep_grant_alloc(struct vfs_grant_table *gt, uint32_t session) {
    for (uint32_t i = 0; i < VFS_GRANTS_PER_SESSION; i++)
        if (!gt->g[session][i].used) return (int)i;
    return -1;
}

/* GRANT_OPEN — admin only.  The ONLY place a pathname meets the grant layer,
 * presented by the admin identity, never by the session holder. */
static void vfs_ep_handle_grant_open(struct vfs_ep_state *st,
                                     const struct IrisMsg *req,
                                     const uint8_t *req_buf,
                                     struct IrisMsg *reply) {
    if (req->sender_badge != IRIS_BADGE_FILEGRANT_ADMIN) {
        vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
        return;
    }
    if (req->word_count < 2u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }
    uint64_t session = req->words[0];
    uint64_t rights  = req->words[1];
    if (session >= VFS_GRANT_SESSIONS ||
        rights == 0u || (rights & ~(uint64_t)VFS_FILE_RIGHT_ALL) != 0u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }
    const char *path = vfs_ep_req_path(req, req_buf);
    if (!path) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }
    const struct vfs_export *exp = vfs_ep_find_export(st->exports,
                                                      st->export_count, path);
    if (!exp) {
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_FOUND);
        return;
    }
    int idx = vfs_ep_grant_alloc(st->grants, (uint32_t)session);
    if (idx < 0) {
        vfs_ep_reply_err(reply, IRIS_ERR_TABLE_FULL);
        return;
    }
    struct vfs_grant *g = &st->grants->g[session][idx];
    g->used        = 1u;
    g->export_slot = (uint8_t)(exp - st->exports);
    g->rights      = (uint32_t)rights;
    g->backing_id  = exp->backing_id;
    g->generation  = exp->generation;

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = (uint64_t)idx;
    reply->words[2]   = g->backing_id;
    reply->words[3]   = g->generation;
    reply->word_count = 4u;
}

/* GRANT_STAT / GRANT_QUERY_IDENTITY — session-scoped introspection. */
static void vfs_ep_handle_grant_stat(struct vfs_ep_state *st,
                                     const struct IrisMsg *req,
                                     struct IrisMsg *reply, int query_only) {
    iris_error_t err = IRIS_ERR_INTERNAL;
    struct vfs_grant *g = vfs_ep_grant_ref(st, req, &err);
    if (!g) { vfs_ep_reply_err(reply, err); return; }
    if (!query_only && !(g->rights & VFS_FILE_RIGHT_STAT)) {
        vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
        return;
    }
    const struct vfs_export *exp = vfs_ep_grant_live(st, g, &err);
    if (!exp) { vfs_ep_reply_err(reply, err); return; }

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    if (query_only) {
        reply->words[1] = g->backing_id;
        reply->words[2] = g->generation;
        reply->words[3] = (uint64_t)g->rights;
    } else {
        reply->words[1] = exp->size;
        reply->words[2] = g->backing_id;
        reply->words[3] = g->generation;
    }
    reply->word_count = 4u;
}

/* GRANT_READ_AT — the only byte-moving grant op.  No pathname in the request;
 * the grant (validated against VFS state) IS the authority. */
static void vfs_ep_handle_grant_read_at(struct vfs_ep_state *st,
                                        const struct IrisMsg *req,
                                        struct IrisMsg *reply,
                                        uint8_t *reply_buf) {
    iris_error_t err = IRIS_ERR_INTERNAL;
    struct vfs_grant *g = vfs_ep_grant_ref(st, req, &err);
    if (!g) { vfs_ep_reply_err(reply, err); return; }
    if (req->word_count < 3u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }
    if (!(g->rights & VFS_FILE_RIGHT_READ)) {
        vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
        return;
    }
    const struct vfs_export *exp = vfs_ep_grant_live(st, g, &err);
    if (!exp) { vfs_ep_reply_err(reply, err); return; }

    vfs_ep_read_reply(exp, req->words[1], req->words[2], reply, reply_buf);
}

/* GRANT_DERIVE — monotonic reduced-rights copy within the session. */
static void vfs_ep_handle_grant_derive(struct vfs_ep_state *st,
                                       const struct IrisMsg *req,
                                       struct IrisMsg *reply) {
    iris_error_t err = IRIS_ERR_INTERNAL;
    struct vfs_grant *g = vfs_ep_grant_ref(st, req, &err);
    if (!g) { vfs_ep_reply_err(reply, err); return; }
    if (req->word_count < 2u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }
    uint64_t rights = req->words[1];
    if (rights == 0u || (rights & ~(uint64_t)VFS_FILE_RIGHT_ALL) != 0u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }
    if (!(g->rights & VFS_FILE_RIGHT_DUPLICATE)) {
        vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
        return;
    }
    /* Monotonicity: every requested right must already be held.  Requesting
     * a right the source lacks is a recovery attempt — denied, not clamped,
     * so the attack is visible. */
    if ((rights & ~(uint64_t)g->rights) != 0u) {
        vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
        return;
    }
    const struct vfs_export *exp = vfs_ep_grant_live(st, g, &err);
    if (!exp) { vfs_ep_reply_err(reply, err); return; }

    int s = iris_badge_filegrant_session(req->sender_badge);
    int idx = vfs_ep_grant_alloc(st->grants, (uint32_t)s);
    if (idx < 0) {
        vfs_ep_reply_err(reply, IRIS_ERR_TABLE_FULL);
        return;
    }
    struct vfs_grant *ng = &st->grants->g[s][idx];
    ng->used        = 1u;
    ng->export_slot = g->export_slot;
    ng->rights      = (uint32_t)rights;
    ng->backing_id  = g->backing_id;
    ng->generation  = g->generation;

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = (uint64_t)idx;
    reply->word_count = 2u;
}

/* Bump an export's generation: every grant snapshotting the old value is now
 * stale and fails CLOSED at its next use, in the VFS, whatever any holder
 * believes.  Returns the new generation. */
static uint64_t vfs_ep_backing_bump(struct vfs_ep_state *st,
                                    struct vfs_export *exp) {
    st->grants->gen_seq++;
    exp->generation = (st->grants->epoch << 16) | (uint64_t)st->grants->gen_seq;
    return exp->generation;
}

/* GRANT_REVOKE — admin by name, or session via a FILE_RIGHT_REVOKE grant. */
static void vfs_ep_handle_grant_revoke(struct vfs_ep_state *st,
                                       const struct IrisMsg *req,
                                       const uint8_t *req_buf,
                                       struct IrisMsg *reply) {
    struct vfs_export *exp = 0;

    if (req->sender_badge == IRIS_BADGE_FILEGRANT_ADMIN) {
        const char *path = vfs_ep_req_path(req, req_buf);
        if (!path) {
            vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
            return;
        }
        exp = (struct vfs_export *)vfs_ep_find_export(st->exports,
                                                      st->export_count, path);
        if (!exp) {
            vfs_ep_reply_err(reply, IRIS_ERR_NOT_FOUND);
            return;
        }
    } else {
        iris_error_t err = IRIS_ERR_INTERNAL;
        struct vfs_grant *g = vfs_ep_grant_ref(st, req, &err);
        if (!g) { vfs_ep_reply_err(reply, err); return; }
        if (!(g->rights & VFS_FILE_RIGHT_REVOKE)) {
            vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
            return;
        }
        const struct vfs_export *live = vfs_ep_grant_live(st, g, &err);
        if (!live) { vfs_ep_reply_err(reply, err); return; }
        exp = (struct vfs_export *)live;
    }

    uint64_t newgen = vfs_ep_backing_bump(st, exp);
    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = newgen;
    reply->word_count = 2u;
}

/* GRANT_SESSION_RESET — admin only; the pager-restart protocol step. */
static void vfs_ep_handle_session_reset(struct vfs_ep_state *st,
                                        const struct IrisMsg *req,
                                        struct IrisMsg *reply) {
    if (req->sender_badge != IRIS_BADGE_FILEGRANT_ADMIN) {
        vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
        return;
    }
    if (req->word_count < 1u || req->words[0] >= VFS_GRANT_SESSIONS) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }
    struct vfs_grant *row = st->grants->g[req->words[0]];
    for (uint32_t i = 0; i < VFS_GRANTS_PER_SESSION; i++) {
        struct vfs_grant *g = &row[i];
        uint8_t *p = (uint8_t *)g;
        for (uint32_t k = 0; k < (uint32_t)sizeof(*g); k++) p[k] = 0;
    }
    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->word_count = 1u;
}

void vfs_ep_dispatch(struct vfs_ep_state *st,
                     const struct IrisMsg *req, const uint8_t *req_buf,
                     struct IrisMsg *reply, uint8_t *reply_buf) {
    if (!reply) return;
    if (!req || !reply_buf || !st || !st->grants ||
        (!st->exports && st->export_count > 0u)) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    /* Payload announced but not delivered (receiver buffer copy failed). */
    if (req->buf_len > 0u && !req_buf) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    /* Fase 28.1 containment: a SESSION badge gets the grant ops and PING —
     * nothing else.  The check runs BEFORE the opcode switch so no name-based
     * path is reachable from a session cap, present or future. */
    if (iris_badge_filegrant_session(req->sender_badge) >= 0) {
        switch (req->label) {
        case VFS_EP_OP_GRANT_STAT:
            vfs_ep_handle_grant_stat(st, req, reply, 0);
            return;
        case VFS_EP_OP_GRANT_READ_AT:
            vfs_ep_handle_grant_read_at(st, req, reply, reply_buf);
            return;
        case VFS_EP_OP_GRANT_QUERY_IDENTITY:
            vfs_ep_handle_grant_stat(st, req, reply, 1);
            return;
        case VFS_EP_OP_GRANT_DERIVE:
            vfs_ep_handle_grant_derive(st, req, reply);
            return;
        case VFS_EP_OP_GRANT_REVOKE:
            vfs_ep_handle_grant_revoke(st, req, req_buf, reply);
            return;
        case IRIS_EP_OP_PING:
            break;                      /* falls through to the common PING */
        default:
            vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
            return;
        }
    }

    switch (req->label) {
    case VFS_EP_OP_LIST:
        vfs_ep_handle_list(st->exports, st->export_count, req, reply, reply_buf);
        return;
    case VFS_EP_OP_STAT:
        vfs_ep_handle_stat(st->exports, st->export_count, req, req_buf, reply);
        return;
    case VFS_EP_OP_READ_AT:
        vfs_ep_handle_read_at(st->exports, st->export_count, req, req_buf,
                              reply, reply_buf);
        return;
    case VFS_EP_OP_STATUS:
        vfs_ep_handle_status(st->exports, st->export_count, req, reply);
        return;
    case VFS_EP_OP_GRANT_OPEN:
        vfs_ep_handle_grant_open(st, req, req_buf, reply);
        return;
    case VFS_EP_OP_GRANT_REVOKE:
        vfs_ep_handle_grant_revoke(st, req, req_buf, reply);
        return;
    case VFS_EP_OP_GRANT_SESSION_RESET:
        vfs_ep_handle_session_reset(st, req, reply);
        return;
    case VFS_EP_OP_GRANT_STAT:
    case VFS_EP_OP_GRANT_READ_AT:
    case VFS_EP_OP_GRANT_QUERY_IDENTITY:
    case VFS_EP_OP_GRANT_DERIVE:
        /* Session-scoped ops from a non-session badge: there is no session
         * to address — denied, not NOT_SUPPORTED, so probes are visible. */
        vfs_ep_reply_err(reply, IRIS_ERR_ACCESS_DENIED);
        return;
    case IRIS_EP_OP_PING:
        vfs_ep_msg_clear(reply);
        reply->label      = IRIS_EP_REPLY_OK;
        reply->words[0]   = 0u;
        /* Fase 9 PING convention: echo the kernel-stamped sender badge. */
        reply->words[1]   = req->sender_badge;
        reply->word_count = 2u;
        return;
    default:
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_SUPPORTED);
        return;
    }
}
