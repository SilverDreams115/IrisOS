/*
 * init_bootstrap.c — initial-authority wiring for init (Fase 14 extraction).
 *
 * Everything here is MOVED VERBATIM from main.c (no functional change):
 *   - spawn/bootstrap KBootstrapCap acquisition (the IRIS_CPTR_SPAWN_CAP
 *     pre-start mint from userboot, Fase 13/Track I);
 *   - the early-serial UART (the only pre-console.ep log fallback);
 *   - EP_LOOKUP_NAME service discovery over svcmgr.ep, including the A1.6
 *     reply receive-slot variant;
 *   - the S5/S6 VFS endpoint boot-health validation (LIST / STAT / READ_AT)
 *     with their retry waits.
 *
 * S5/S6 live HERE, not in init_test.c (Fase 14 decision): they are
 * boot-GATING checks — main.c exits (codes 9/10) if they fail, and their
 * retry loops double as the "wait until VFS is up" boot synchronization —
 * while init_test.c holds post-healthy-path probes that never gate boot.
 * They also share the VFS EP client (init_vfs_ep_call / g_init_ep_buf)
 * with the discovery lookups above; moving them would split that client
 * across modules for no boundary gain.
 */

#include "init.h"
#include <iris/endpoint_proto.h>
#include <iris/ipc_recv_slot.h>
#include <iris/vfs_ep_proto.h>

/* ── Early serial (pre-console.ep log fallback) ─────────────────────────── */

static handle_id_t g_init_early_serial_h = HANDLE_INVALID;

void init_early_serial_write(const char *s) {
    if (g_init_early_serial_h == HANDLE_INVALID || !s) return;
    while (*s) {
        long v;
        do {
            v = init_sys2(SYS_IOPORT_IN, (long)g_init_early_serial_h, 5);
        } while (v < 0 || !((uint8_t)v & 0x20u));
        (void)init_sys3(SYS_IOPORT_OUT, (long)g_init_early_serial_h, 0,
                        (long)(uint8_t)*s++);
    }
}

/* Fase S4: the serial KIoPort is published into a CSpace slot as an MDB child
 * of the spawn-cap slot; the authority argument must be a CPtr.  Slot 40 is
 * free in init's root CNode (1..15 well-known). */
#define INIT_EARLY_SERIAL_SLOT 40u
void init_early_serial_start(handle_id_t spawn_cap_h) {
    (void)spawn_cap_h;
    if (g_init_early_serial_h != HANDLE_INVALID) return;
    if (init_sys4(SYS_CAP_CREATE_IOPORT, (long)IRIS_CPTR_SPAWN_CAP,
                  0x3F8, 8, (long)INIT_EARLY_SERIAL_SLOT) != 0) return;
    g_init_early_serial_h = (handle_id_t)INIT_EARLY_SERIAL_SLOT;
}

void init_early_serial_stop(void) {
    init_close(&g_init_early_serial_h);
}

void init_retry_pause(void) {
    (void)init_sys1(SYS_SLEEP, INIT_RETRY_SLEEP_TICKS);
}

/* Fase 13 (Track I): init's spawn/bootstrap KBootstrapCap arrives as the
 * IRIS_CPTR_SPAWN_CAP (slot 6) pre-start mint from userboot — not over a
 * bootstrap KChannel.
 *
 * Etapa 4: init_recv_spawn_cap is RETIRED.  It materialised that slot into a
 * handle because init used to DUP and restrict the cap for its children, and
 * both of those needed a handle-table entry.  Neither does any more — the
 * duplicates are gone and restriction derives slot-to-slot — so every consumer
 * names slot 6 directly and the bridge had nothing left to bridge. */

/* ── VFS endpoint client (Fase 7.2) ─────────────────────────────────────── */

/* EP_CALL bulk buffer: the request path and the reply data share this buffer
 * (EP_CALL reuses buf_uptr in both directions). +1 for a guard NUL. */
static uint8_t g_init_ep_buf[VFS_EP_DATA_MAX + 1u];

static void init_imsg_zero(struct IrisMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

/*
 * Resolve a service name (e.g. "vfs.ep") through the svcmgr discovery endpoint:
 * EP_CALL(svcmgr_ep, IRIS_SVCMGR_EP_LOOKUP_NAME, name).  The reply carries the
 * endpoint cap via SYS_REPLY cap transfer.  Returns HANDLE_INVALID on any
 * failure (caller retries / fails fast).  Fase 13/Track I: this EP_LOOKUP_NAME
 * path replaces the retired legacy KChannel LOOKUP_NAME (init_lookup_name).
 *
 * A1.6: reply_slot != 0 declares a receive-slot for the looked-up cap — it
 * lands in init's CSpace and the return value is the CPtr (< 1024), directly
 * invocable through the dual resolvers.  reply_slot = 0 keeps the legacy
 * handle delivery; the supervisor lookups that feed SYS_PROC_CSPACE_MINT use
 * it deliberately (mint sources are handle-layer working set by design). */
handle_id_t init_ep_lookup_name_slot(handle_id_t svcmgr_ep_h,
                                     const char *name,
                                     uint32_t reply_slot) {
    struct IrisMsg msg;
    uint32_t n = 0;

    if (svcmgr_ep_h == HANDLE_INVALID || !name) return HANDLE_INVALID;
    while (name[n] && n + 1u < (uint32_t)sizeof(g_init_ep_buf)) {
        g_init_ep_buf[n] = (uint8_t)name[n];
        n++;
    }
    g_init_ep_buf[n] = 0u;

    init_imsg_zero(&msg);
    msg.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)g_init_ep_buf;
    msg.buf_len  = n + 1u;  /* includes NUL */
    iris_msg_declare_reply_slot(&msg, reply_slot);

    if (init_sys2(SYS_EP_CALL, (long)svcmgr_ep_h, (long)&msg) != IRIS_OK)
        return HANDLE_INVALID;
    if (msg.label != IRIS_EP_REPLY_OK)
        return HANDLE_INVALID;
    if (msg.attached_handle == (uint32_t)IRIS_MSG_NO_CAP)
        return HANDLE_INVALID;
    return (handle_id_t)msg.attached_handle;   /* CPtr or handle — dual-invocable */
}

handle_id_t init_ep_lookup_name(handle_id_t svcmgr_ep_h, const char *name) {
    return init_ep_lookup_name_slot(svcmgr_ep_h, name, 0u);
}

/*
 * One VFS endpoint round trip. The path (when non-NULL) is staged into
 * g_init_ep_buf; reply bulk data lands in the same buffer.
 */
static int init_vfs_ep_call(handle_id_t vfs_ep_h, struct IrisMsg *msg,
                            const char *path) {
    msg->buf_uptr = (uint64_t)(uintptr_t)g_init_ep_buf;
    if (path) {
        uint32_t plen = 0;
        while (path[plen]) plen++;
        if (plen + 1u > VFS_EP_PATH_MAX) return (int)IRIS_ERR_INVALID_ARG;
        for (uint32_t i = 0; i < plen; i++) g_init_ep_buf[i] = (uint8_t)path[i];
        g_init_ep_buf[plen] = 0u;
        msg->buf_len = plen + 1u;
    }
    return (int)init_sys2(SYS_EP_CALL, (long)vfs_ep_h, (long)msg);
}

/* ── VFS EP LIST check (S5) ─────────────────────────────────────────────── */

static int init_check_vfs_list_ep(handle_id_t vfs_ep_h) {
    struct IrisMsg msg;

    /* indices 0..2 — expect OK with a non-empty name */
    for (uint64_t idx = 0; idx < 3u; idx++) {
        init_imsg_zero(&msg);
        msg.label      = VFS_EP_OP_LIST;
        msg.words[0]   = idx;
        msg.word_count = 1u;
        if (init_vfs_ep_call(vfs_ep_h, &msg, 0) != IRIS_OK) return 0;
        if (msg.label != IRIS_EP_REPLY_OK) return 0;
        if (msg.words[2] == 0u) return 0;  /* name length */
    }

    /* index 100 — expect NOT_FOUND (end-of-listing semantics) */
    init_imsg_zero(&msg);
    msg.label      = VFS_EP_OP_LIST;
    msg.words[0]   = 100u;
    msg.word_count = 1u;
    if (init_vfs_ep_call(vfs_ep_h, &msg, 0) != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_ERR) return 0;
    if (msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) return 0;

    return 1;
}

int init_wait_vfs_list_ep(handle_id_t vfs_ep_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_check_vfs_list_ep(vfs_ep_h)) return 1;
        init_retry_pause();
    }
    return 0;
}

/* ── VFS EP STAT / READ_AT check (S6) ───────────────────────────────────── */

static int init_check_vfs_rw_ep(handle_id_t vfs_ep_h) {
    struct IrisMsg msg;
    uint64_t size;

    /* STAT of the boot file — expect OK with a sane size */
    init_imsg_zero(&msg);
    msg.label = VFS_EP_OP_STAT;
    if (init_vfs_ep_call(vfs_ep_h, &msg, "iris.txt") != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_OK) return 0;
    size = msg.words[1];
    if (size == 0u || size > VFS_EP_DATA_MAX) return 0;

    /* READ_AT offset 0 — full content in one reply */
    init_imsg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0u;
    msg.words[1]   = VFS_EP_DATA_MAX;
    msg.word_count = 2u;
    if (init_vfs_ep_call(vfs_ep_h, &msg, "iris.txt") != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_OK) return 0;
    if (msg.words[1] != size || msg.words[2] != size) return 0;
    if (msg.buf_len != (uint32_t)size) return 0;

    /* READ_AT offset == size — EOF (0 bytes), not an error */
    init_imsg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = size;
    msg.words[1]   = VFS_EP_DATA_MAX;
    msg.word_count = 2u;
    if (init_vfs_ep_call(vfs_ep_h, &msg, "iris.txt") != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_OK) return 0;
    if (msg.words[1] != 0u || msg.words[2] != size) return 0;

    /* READ_AT of a missing export — NOT_FOUND */
    init_imsg_zero(&msg);
    msg.label      = VFS_EP_OP_READ_AT;
    msg.words[0]   = 0u;
    msg.words[1]   = VFS_EP_DATA_MAX;
    msg.word_count = 2u;
    if (init_vfs_ep_call(vfs_ep_h, &msg, "no-such-file") != IRIS_OK) return 0;
    if (msg.label != IRIS_EP_REPLY_ERR) return 0;
    if (msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) return 0;

    return 1;
}

int init_wait_vfs_rw_ep(handle_id_t vfs_ep_h) {
    for (uint32_t attempt = 0; attempt < INIT_RETRY_LIMIT; attempt++) {
        if (init_check_vfs_rw_ep(vfs_ep_h)) return 1;
        init_retry_pause();
    }
    return 0;
}
