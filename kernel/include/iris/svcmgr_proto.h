#ifndef IRIS_SVCMGR_PROTO_H
#define IRIS_SVCMGR_PROTO_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#endif

/*
 * IRIS Service Manager Bootstrap Protocol
 *
 * ── Overview ─────────────────────────────────────────────────────
 * Messages flow over the svcmgr bootstrap KChannel between the kernel
 * and the ring-3 service manager process.
 *
 *   kernel  → svcmgr : SVCMGR_MSG_PHASE3_PROBE
 *   kernel  → svcmgr : PROC_EVENT_MSG_EXIT (via SYS_PROCESS_WATCH)
 *   client  → svcmgr : SVCMGR_MSG_LOOKUP
 *   client  → svcmgr : SVCMGR_MSG_LOOKUP_NAME
 *   service → svcmgr : SVCMGR_MSG_REGISTER
 *   service → svcmgr : SVCMGR_MSG_UNREGISTER
 *   client  → svcmgr : SVCMGR_MSG_DIAG
 *   svcmgr  → client : SVCMGR_MSG_LOOKUP_REPLY
 *   svcmgr  → client : SVCMGR_MSG_DIAG_REPLY
 *   svcmgr  → service: SVCMGR_MSG_BOOTSTRAP_HANDLE
 *   svcmgr  → kernel : SVCMGR_MSG_ACK (phase 2)
 *
 * Healthy-path bootstrap no longer uses kernel → svcmgr spawn requests
 * for compiled-in services.  The kernel now bootstraps only svcmgr and
 * the first client capability; svcmgr consumes the declarative service catalog for
 * compiled-in services and reads selftest/bootstrap traffic, process-exit
 * watch events, and client lookup requests from its bootstrap channel.
 *
 * ── KChanMsg wire layout (84 bytes) ─────────────────────────────
 *   offset  0: uint32_t type      → SVCMGR_MSG_* opcode
 *   offset  4: uint32_t sender_id → 0 = kernel, N = task_id
 *   offset  8: uint8_t  data[64]  → payload (see below)
 *   offset 72: uint32_t data_len  → payload length in bytes
 *   offset 76: handle_id_t attached_handle  → optional moved handle
 *   offset 80: iris_rights_t attached_rights → rights delivered to receiver
 *
 * ── SVCMGR_MSG_SPAWN_SERVICE (transitional compatibility) ────────
 * Legacy/bootstrap compatibility message kept for staged evolution.
 * Healthy-path boot no longer uses it for compiled-in service startup.
 *
 *   data[SVCMGR_SPAWN_OFF_SERVICE_ID] uint32_t    service kind selector.
 *                                                 svcmgr resolves this to
 *                                                 entry point + nameservice
 *                                                 metadata in userland.
 *   data[SVCMGR_SPAWN_OFF_REG_CHAN]   handle_id_t pre-created channel handle
 *                                                 already inserted into svcmgr's
 *                                                 handle table by the kernel.
 *                                                 svcmgr publishes it under
 *                                                 the userland-owned service
 *                                                 name for this service kind.
 *   data[SVCMGR_SPAWN_OFF_IRQ]        uint8_t     hardware IRQ line to route
 *                                                 into the service channel, or
 *                                                 0xFF for no IRQ route.
 *   data[SVCMGR_SPAWN_OFF_FLAGS]      uint8_t     bootstrap flags (0 = none).
 *
 * ── SVCMGR_MSG_ACK (svcmgr → kernel, phase 2) ───────────────────
 *   data[SVCMGR_ACK_OFF_TASK_ID]  uint32_t  spawned task id (0 = failed)
 *   data[SVCMGR_ACK_OFF_ERR]      int32_t   0 = OK, negative = iris_error_t

 * ── SVCMGR_MSG_BOOTSTRAP_HANDLE (svcmgr → child service) ─────────
 * Delivers one public service handle to a freshly spawned child over the
 * child's private bootstrap channel.  The handle is attached to the message
 * and is MOVE-only from svcmgr to the child:
 *
 *   data[SVCMGR_BOOTSTRAP_OFF_KIND] uint32_t bootstrap role selector
 *                                   (service inbox, reply channel,
 *                                   or bootstrap capability).
 *   attached_handle                 duplicated temp handle in svcmgr,
 *                                   consumed by SYS_CHAN_SEND and installed
 *                                   into the child on SYS_CHAN_RECV.
 *   attached_rights                exact rights granted to the child.

 * ── SVCMGR_MSG_LOOKUP (client → svcmgr) ──────────────────────────
 * Normal runtime service discovery path.  The client no longer asks the
 * kernel registry for "kbd" or "vfs"; it bootstrap-lookups "svcmgr",
 * creates a one-shot reply channel locally, duplicates that channel down
 * to WRITE|TRANSFER, and attaches it here.
 *
 *   data[SVCMGR_LOOKUP_OFF_ENDPOINT] uint32_t requested endpoint id.
 *   data[SVCMGR_LOOKUP_OFF_RIGHTS]   uint32_t requested rights cap.
 *   attached_handle                  reply channel handle moved into svcmgr.
 *   attached_rights                  RIGHT_WRITE for that reply channel copy.

 * ── SVCMGR_MSG_LOOKUP_REPLY (svcmgr → client) ────────────────────
 * Lookup result sent over the transferred reply channel.
 *
 *   data[SVCMGR_LOOKUP_REPLY_OFF_ERR]      int32_t 0 = OK, <0 = iris_error_t
 *   data[SVCMGR_LOOKUP_REPLY_OFF_ENDPOINT] uint32_t echoed endpoint id
 *   attached_handle                        looked-up service handle, if err=0
 *   attached_rights                        exact rights granted to the client
 *
 * ── SVCMGR_MSG_LOOKUP_NAME (client → svcmgr) ──────────────────────
 * Name-based runtime discovery. Fixed catalog services and runtime-registered
 * services share the same namespace. The client attaches a one-shot reply
 * channel with RIGHT_WRITE.
 *
 *   data[SVCMGR_LOOKUP_NAME_OFF_NAME]      fixed-size NUL-terminated service name
 *   data[SVCMGR_LOOKUP_NAME_OFF_RIGHTS]    uint32_t requested rights cap
 *   attached_handle                        reply channel handle moved into svcmgr
 *   attached_rights                        RIGHT_WRITE for that reply channel copy
 *
 * ── SVCMGR_MSG_REGISTER (service → svcmgr) ────────────────────────
 * First-cut dynamic publication path. A publisher moves one master handle into
 * svcmgr and binds it to a runtime `(endpoint, name)` entry plus an allowed
 * client-rights mask.
 *
 *   data[SVCMGR_REGISTER_OFF_ENDPOINT]     uint32_t published endpoint id
 *   data[SVCMGR_REGISTER_OFF_RIGHTS]       uint32_t allowed client-rights mask
 *   data[SVCMGR_REGISTER_OFF_NAME]         fixed-size NUL-terminated service name
 *   attached_handle                        service master handle moved into svcmgr
 *   attached_rights                        must include RIGHT_DUPLICATE
 *
 * This first cut does not define a secure unregister path. sender_id is not
 * treated as an authority signal.
 *
 * ── SVCMGR_MSG_UNREGISTER (service → svcmgr) ──────────────────────
 * First-cut withdrawal path for runtime-published channel endpoints.
 * The publisher moves a proof handle for the same published channel object;
 * svcmgr compares object identity against the registered master handle.
 *
 *   data[SVCMGR_UNREGISTER_OFF_ENDPOINT]   uint32_t published endpoint id
 *   attached_handle                        proof handle for the same channel object
 *   attached_rights                        must include RIGHT_WRITE
 *
 * On success, svcmgr seals the published channel before dropping its master
 * reference. That makes future lookups fail and forces already-distributed
 * channel duplicates to observe CLOSED.
 *
 * ── SVCMGR_MSG_DIAG (client → svcmgr) ─────────────────────────────
 * One-shot consolidated diagnostics query. The client attaches a reply
 * channel handle with RIGHT_WRITE; svcmgr gathers:
 *   - kernel-owned snapshot counts via SYS_DIAG_SNAPSHOT
 *   - svcmgr-local supervision summary from its live state
 *   - vfs status via VFS_MSG_STATUS on a temporary reply channel
 *   - kbd status via KBD_MSG_STATUS on a temporary reply channel
 *
 * The reply carries a compact global-health summary. Subsystem-local status
 * providers remain the source of truth; svcmgr only aggregates their views.
 *
 * ── SVCMGR_MSG_DIAG_REPLY (svcmgr → client) ───────────────────────
 * data[0..3]   int32_t  err: 0 = OK, <0 = iris_error_t
 * data[4..7]   uint32_t version: SVCMGR_DIAG_VERSION
 * data[8..11]  uint32_t tasks_live
 * data[12..15] uint32_t kproc_live
 * data[16..19] uint32_t irq_routes_active
 * data[20..23] uint32_t ticks_lo
 * data[24..27] uint32_t ticks_hi
 * data[28..31] uint32_t catalog entries
 * data[32..35] uint32_t ready services
 * data[36..39] uint32_t active tracked slots
 * data[40..43] uint32_t service catalog version
 * data[44..47] uint32_t vfs exports ready
 * data[48..51] uint32_t vfs open files
 * data[52..55] uint32_t vfs open capacity
 * data[56..59] uint32_t vfs exported bytes
 * data[60..63] uint32_t kbd status flags
 *
 * ── SVCMGR_MSG_PHASE3_PROBE (kernel → svcmgr, phase 3 validation) ──
 * Bounded supervision self-check used to prove proc_h retirement and
 * slot reuse deterministically under the real svcmgr ownership path.
 *
 *   data[SVCMGR_P3_OFF_ENTRY]  uint64_t  ring-3 entry_vaddr for a child
 *                                        that exits quickly.
 *
 * ── Phase status ─────────────────────────────────────────────────
 * Phase 8/current: svcmgr is the userland-authoritative discovery service
 * for normal clients and consumes a declarative catalog for compiled-in
 * services (`kbd`, `vfs`).  The kernel bootstrap registry is transitional
 * only and no longer participates in the healthy path to reach svcmgr.
 * Other well-known service handles are kept in svcmgr's runtime registry
 * and are returned over IPC with attached handle transfer. Service exit on the
 * healthy path is supervised by PROC_EVENT_MSG_EXIT over svcmgr's bootstrap
 * channel; svcmgr itself no longer polls for child lifecycle.
 * IRQ routing stays kernel-side (permanent kernel concern). irq_routing owner
 * = child service KProcess after SYS_IRQ_ROUTE_REGISTER; auto-cleanup fires
 * when a service exits via kprocess_teardown → irq_routing_unregister_owner.
 */

#define SVCMGR_MSG_SPAWN_SERVICE  0x0001u
/* 0x0002 retired (Phase 19: SVCMGR_MSG_PHASE3_PROBE removed with SYS_SPAWN) */
#define SVCMGR_MSG_LOOKUP         0x0003u
#define SVCMGR_MSG_STATUS         0x0004u
#define SVCMGR_MSG_DIAG           0x0005u
#define SVCMGR_MSG_LOOKUP_NAME    0x0006u
#define SVCMGR_MSG_REGISTER       0x0007u
#define SVCMGR_MSG_UNREGISTER     0x0008u
#define SVCMGR_MSG_ACK            0x8001u
#define SVCMGR_MSG_BOOTSTRAP_HANDLE 0x8002u
#define SVCMGR_MSG_LOOKUP_REPLY   0x8003u
#define SVCMGR_MSG_STATUS_REPLY   0x8004u
#define SVCMGR_MSG_DIAG_REPLY     0x8005u

#define SVCMGR_SERVICE_NAME_CAP     16u

#define SVCMGR_SERVICE_NONE       0u
#define SVCMGR_SERVICE_KBD        1u
#define SVCMGR_SERVICE_VFS        2u

#define SVCMGR_ENDPOINT_NONE        0u
#define SVCMGR_ENDPOINT_KBD         1u
#define SVCMGR_ENDPOINT_KBD_REPLY   2u
#define SVCMGR_ENDPOINT_VFS         3u
#define SVCMGR_ENDPOINT_VFS_REPLY   4u

#define SVCMGR_BOOTSTRAP_KIND_NONE      0u
#define SVCMGR_BOOTSTRAP_KIND_SERVICE   1u
#define SVCMGR_BOOTSTRAP_KIND_REPLY     2u
#define SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP 3u
#define SVCMGR_BOOTSTRAP_KIND_IRQ_CAP   4u  /* KIrqCap capability for IRQ routing */
#define SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP 5u /* KIoPort capability for I/O port access */

/* Byte offsets within KChanMsg.data[64] */
#define SVCMGR_SPAWN_OFF_SERVICE_ID 0  /* uint32_t:    service kind selector       */
#define SVCMGR_SPAWN_OFF_REG_CHAN   4  /* handle_id_t: pre-created channel handle
                                        *   in svcmgr's table; published under the
                                        *   service name chosen by svcmgr.         */
#define SVCMGR_SPAWN_OFF_IRQ        8  /* uint8_t:     hardware IRQ line to route
                                        *   into the service channel, or 0xFF for
                                        *   no IRQ route.  svcmgr calls
                                        *   SYS_IRQ_ROUTE_REGISTER(irqcap_h, chan_h, proc_h)
                                        *   after spawn to transfer route ownership
                                        *   from svcmgr to the child process, so that
                                        *   kprocess_teardown auto-clears the route
                                        *   when the service exits.
                                        *   irqcap_h is received at bootstrap time via
                                        *   SVCMGR_BOOTSTRAP_KIND_IRQ_CAP.         */
#define SVCMGR_SPAWN_OFF_FLAGS      9  /* uint8_t:     bootstrap flags; 0 = none   */

/* Additional byte offsets for SVCMGR_BOOTSTRAP_KIND_IRQ_CAP messages */
#define SVCMGR_BOOTSTRAP_OFF_IRQ_NUM     4   /* uint8_t: hardware IRQ number (0-15) */
#define SVCMGR_BOOTSTRAP_IRQ_CAP_MSG_LEN 5u  /* kind(4 bytes) + irq_num(1 byte)    */

/* Additional byte offsets for SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP messages */
#define SVCMGR_BOOTSTRAP_OFF_IOPORT_SVC  4   /* uint8_t: service_id that owns this cap */
#define SVCMGR_BOOTSTRAP_IOPORT_CAP_MSG_LEN 5u /* kind(4 bytes) + service_id(1 byte) */

#define SVCMGR_ACK_OFF_TASK_ID    0    /* uint32_t: spawned task id        */
#define SVCMGR_ACK_OFF_ERR        4    /* int32_t:  0=OK, <0=iris_error_t  */
#define SVCMGR_BOOTSTRAP_OFF_KIND 0    /* uint32_t: endpoint role selector */
#define SVCMGR_LOOKUP_OFF_ENDPOINT 0   /* uint32_t: requested endpoint id  */
#define SVCMGR_LOOKUP_OFF_RIGHTS   4   /* uint32_t: requested rights cap   */
#define SVCMGR_LOOKUP_NAME_OFF_NAME   0  /* char[16]: requested service name */
#define SVCMGR_LOOKUP_NAME_OFF_RIGHTS 16 /* uint32_t: requested rights cap   */
#define SVCMGR_REGISTER_OFF_ENDPOINT  0  /* uint32_t: published endpoint id  */
#define SVCMGR_REGISTER_OFF_RIGHTS    4  /* uint32_t: allowed client rights  */
#define SVCMGR_REGISTER_OFF_NAME      8  /* char[16]: published service name */
#define SVCMGR_UNREGISTER_OFF_ENDPOINT 0 /* uint32_t: published endpoint id  */
#define SVCMGR_LOOKUP_REPLY_OFF_ERR      0 /* int32_t: 0=OK, <0=iris_error_t */
#define SVCMGR_LOOKUP_REPLY_OFF_ENDPOINT 4 /* uint32_t: echoed endpoint id   */
#define SVCMGR_STATUS_REPLY_OFF_ERR      0 /* int32_t: 0=OK, <0=iris_error_t */
#define SVCMGR_STATUS_REPLY_OFF_VERSION  4 /* uint32_t: protocol version      */
#define SVCMGR_STATUS_REPLY_OFF_MANIFEST 8 /* uint32_t: catalog entries       */
#define SVCMGR_STATUS_REPLY_OFF_READY   12 /* uint32_t: ready services        */
#define SVCMGR_STATUS_REPLY_OFF_SLOTS   16 /* uint32_t: tracked live slots    */
#define SVCMGR_STATUS_REPLY_OFF_CATALOG 20 /* uint32_t: service catalog version */
#define SVCMGR_DIAG_REPLY_OFF_ERR       0 /* int32_t: 0=OK, <0=iris_error_t */
#define SVCMGR_DIAG_REPLY_OFF_VERSION   4 /* uint32_t: diagnostics summary version */
#define SVCMGR_DIAG_REPLY_OFF_TASKS     8 /* uint32_t: live scheduler tasks */
#define SVCMGR_DIAG_REPLY_OFF_KPROC    12 /* uint32_t: live KProcess slots */
#define SVCMGR_DIAG_REPLY_OFF_IRQ      16 /* uint32_t: active IRQ routes */
#define SVCMGR_DIAG_REPLY_OFF_TICKS_LO 20 /* uint32_t: scheduler ticks low */
#define SVCMGR_DIAG_REPLY_OFF_TICKS_HI 24 /* uint32_t: scheduler ticks high */
#define SVCMGR_DIAG_REPLY_OFF_MANIFEST 28 /* uint32_t: service catalog entries */
#define SVCMGR_DIAG_REPLY_OFF_READY    32 /* uint32_t: ready services */
#define SVCMGR_DIAG_REPLY_OFF_SLOTS    36 /* uint32_t: tracked live slots */
#define SVCMGR_DIAG_REPLY_OFF_CATALOG 40 /* uint32_t: service catalog version */
#define SVCMGR_DIAG_REPLY_OFF_VFS_EXPORTS 44 /* uint32_t: ready exports */
#define SVCMGR_DIAG_REPLY_OFF_VFS_OPENS   48 /* uint32_t: live open files */
#define SVCMGR_DIAG_REPLY_OFF_VFS_CAP     52 /* uint32_t: open-file capacity */
#define SVCMGR_DIAG_REPLY_OFF_VFS_BYTES   56 /* uint32_t: exported bytes */
#define SVCMGR_DIAG_REPLY_OFF_KBD_FLAGS   60 /* uint32_t: KBD_STATUS_* bits */

/* data_len values */
#define SVCMGR_SPAWN_MSG_LEN      10u  /* 4 + 4 + 1 + 1 */
#define SVCMGR_ACK_MSG_LEN        8u   /* 4 + 4 */
#define SVCMGR_BOOTSTRAP_MSG_LEN  4u   /* kind only (SPAWN_CAP, SERVICE, REPLY) */
/* SVCMGR_BOOTSTRAP_IRQ_CAP_MSG_LEN = 5u is defined above with its offset constants */
#define SVCMGR_LOOKUP_MSG_LEN     8u   /* endpoint + rights */
#define SVCMGR_LOOKUP_NAME_MSG_LEN 20u /* name + rights */
#define SVCMGR_REGISTER_MSG_LEN   24u  /* endpoint + rights + name */
#define SVCMGR_UNREGISTER_MSG_LEN 4u   /* endpoint only */
#define SVCMGR_LOOKUP_REPLY_MSG_LEN 8u /* err + endpoint */
#define SVCMGR_STATUS_MSG_LEN     0u
#define SVCMGR_STATUS_REPLY_MSG_LEN 24u
#define SVCMGR_DIAG_MSG_LEN       0u
#define SVCMGR_DIAG_REPLY_MSG_LEN 64u

#define SVCMGR_PROTO_VERSION      3u
#define SVCMGR_DIAG_VERSION       2u
#define IRIS_SERVICE_CATALOG_VERSION 1u

#ifndef __ASSEMBLER__
static inline void svcmgr_proto_write_u32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static inline uint32_t svcmgr_proto_read_u32(const uint8_t *src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static inline uint64_t svcmgr_proto_read_u64(const uint8_t *src) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8u; i++) v |= ((uint64_t)src[i]) << (i * 8u);
    return v;
}

static inline int svcmgr_proto_lookup_valid(const struct KChanMsg *msg) {
    return msg && msg->type == SVCMGR_MSG_LOOKUP &&
           msg->data_len == SVCMGR_LOOKUP_MSG_LEN &&
           msg->attached_handle != HANDLE_INVALID &&
           (msg->attached_rights & RIGHT_WRITE) != 0;
}

static inline int svcmgr_proto_service_name_valid_bytes(const uint8_t *src) {
    if (!src) return 0;
    for (uint32_t i = 0; i < SVCMGR_SERVICE_NAME_CAP; i++) {
        if (src[i] == '\0')
            return i != 0u;
    }
    return 0;
}

static inline void svcmgr_proto_lookup_decode(const struct KChanMsg *msg,
                                              uint32_t *endpoint,
                                              iris_rights_t *rights) {
    if (endpoint) *endpoint = svcmgr_proto_read_u32(&msg->data[SVCMGR_LOOKUP_OFF_ENDPOINT]);
    if (rights) *rights = (iris_rights_t)svcmgr_proto_read_u32(&msg->data[SVCMGR_LOOKUP_OFF_RIGHTS]);
}

static inline int svcmgr_proto_lookup_name_valid(const struct KChanMsg *msg) {
    return msg && msg->type == SVCMGR_MSG_LOOKUP_NAME &&
           msg->data_len == SVCMGR_LOOKUP_NAME_MSG_LEN &&
           msg->attached_handle != HANDLE_INVALID &&
           (msg->attached_rights & RIGHT_WRITE) != 0 &&
           svcmgr_proto_service_name_valid_bytes(&msg->data[SVCMGR_LOOKUP_NAME_OFF_NAME]);
}

static inline void svcmgr_proto_lookup_name_decode(const struct KChanMsg *msg,
                                                   char *name_out,
                                                   iris_rights_t *rights) {
    if (name_out) {
        for (uint32_t i = 0; i < SVCMGR_SERVICE_NAME_CAP; i++)
            name_out[i] = (char)msg->data[SVCMGR_LOOKUP_NAME_OFF_NAME + i];
    }
    if (rights)
        *rights = (iris_rights_t)svcmgr_proto_read_u32(&msg->data[SVCMGR_LOOKUP_NAME_OFF_RIGHTS]);
}

static inline void svcmgr_proto_lookup_reply_init(struct KChanMsg *msg,
                                                  uint32_t endpoint,
                                                  int32_t err,
                                                  handle_id_t attached_h,
                                                  iris_rights_t attached_rights) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = SVCMGR_MSG_LOOKUP_REPLY;
    svcmgr_proto_write_u32(&msg->data[SVCMGR_LOOKUP_REPLY_OFF_ERR], (uint32_t)err);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_LOOKUP_REPLY_OFF_ENDPOINT], endpoint);
    msg->data_len = SVCMGR_LOOKUP_REPLY_MSG_LEN;
    msg->attached_handle = attached_h;
    msg->attached_rights = attached_rights;
}

static inline int svcmgr_proto_status_valid(const struct KChanMsg *msg) {
    return msg && msg->type == SVCMGR_MSG_STATUS &&
           msg->data_len == SVCMGR_STATUS_MSG_LEN &&
           msg->attached_handle != HANDLE_INVALID &&
           (msg->attached_rights & RIGHT_WRITE) != 0;
}

static inline int svcmgr_proto_register_valid(const struct KChanMsg *msg) {
    return msg && msg->type == SVCMGR_MSG_REGISTER &&
           msg->data_len == SVCMGR_REGISTER_MSG_LEN &&
           msg->attached_handle != HANDLE_INVALID &&
           svcmgr_proto_service_name_valid_bytes(&msg->data[SVCMGR_REGISTER_OFF_NAME]);
}

static inline void svcmgr_proto_register_decode(const struct KChanMsg *msg,
                                                uint32_t *endpoint,
                                                iris_rights_t *rights,
                                                char *name_out) {
    if (endpoint)
        *endpoint = svcmgr_proto_read_u32(&msg->data[SVCMGR_REGISTER_OFF_ENDPOINT]);
    if (rights)
        *rights = (iris_rights_t)svcmgr_proto_read_u32(&msg->data[SVCMGR_REGISTER_OFF_RIGHTS]);
    if (name_out) {
        for (uint32_t i = 0; i < SVCMGR_SERVICE_NAME_CAP; i++)
            name_out[i] = (char)msg->data[SVCMGR_REGISTER_OFF_NAME + i];
    }
}

static inline int svcmgr_proto_unregister_valid(const struct KChanMsg *msg) {
    return msg && msg->type == SVCMGR_MSG_UNREGISTER &&
           msg->data_len == SVCMGR_UNREGISTER_MSG_LEN &&
           msg->attached_handle != HANDLE_INVALID &&
           (msg->attached_rights & RIGHT_WRITE) != 0;
}

static inline uint32_t svcmgr_proto_unregister_endpoint(const struct KChanMsg *msg) {
    return msg ? svcmgr_proto_read_u32(&msg->data[SVCMGR_UNREGISTER_OFF_ENDPOINT]) : 0u;
}

static inline int svcmgr_proto_diag_valid(const struct KChanMsg *msg) {
    return msg && msg->type == SVCMGR_MSG_DIAG &&
           msg->data_len == SVCMGR_DIAG_MSG_LEN &&
           msg->attached_handle != HANDLE_INVALID &&
           (msg->attached_rights & RIGHT_WRITE) != 0;
}

static inline void svcmgr_proto_status_reply_init(struct KChanMsg *msg,
                                                  int32_t err,
                                                  uint32_t manifest_count,
                                                  uint32_t ready_services,
                                                  uint32_t active_slots,
                                                  uint32_t catalog_version) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = SVCMGR_MSG_STATUS_REPLY;
    svcmgr_proto_write_u32(&msg->data[SVCMGR_STATUS_REPLY_OFF_ERR], (uint32_t)err);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_STATUS_REPLY_OFF_VERSION], SVCMGR_PROTO_VERSION);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_STATUS_REPLY_OFF_MANIFEST], manifest_count);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_STATUS_REPLY_OFF_READY], ready_services);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_STATUS_REPLY_OFF_SLOTS], active_slots);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_STATUS_REPLY_OFF_CATALOG], catalog_version);
    msg->data_len = SVCMGR_STATUS_REPLY_MSG_LEN;
    msg->attached_handle = HANDLE_INVALID;
    msg->attached_rights = RIGHT_NONE;
}

static inline void svcmgr_proto_diag_reply_init(struct KChanMsg *msg,
                                                int32_t err,
                                                uint32_t tasks_live,
                                                uint32_t kproc_live,
                                                uint32_t irq_routes_active,
                                                uint32_t ticks_lo,
                                                uint32_t ticks_hi,
                                                uint32_t manifest_count,
                                                uint32_t ready_services,
                                                uint32_t active_slots,
                                                uint32_t catalog_version,
                                                uint32_t vfs_exports_ready,
                                                uint32_t vfs_open_files,
                                                uint32_t vfs_open_capacity,
                                                uint32_t vfs_exported_bytes,
                                                uint32_t kbd_flags) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = SVCMGR_MSG_DIAG_REPLY;
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_ERR], (uint32_t)err);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_VERSION], SVCMGR_DIAG_VERSION);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_TASKS], tasks_live);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_KPROC], kproc_live);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_IRQ], irq_routes_active);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_TICKS_LO], ticks_lo);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_TICKS_HI], ticks_hi);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_MANIFEST], manifest_count);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_READY], ready_services);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_SLOTS], active_slots);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_CATALOG], catalog_version);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_VFS_EXPORTS], vfs_exports_ready);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_VFS_OPENS], vfs_open_files);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_VFS_CAP], vfs_open_capacity);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_VFS_BYTES], vfs_exported_bytes);
    svcmgr_proto_write_u32(&msg->data[SVCMGR_DIAG_REPLY_OFF_KBD_FLAGS], kbd_flags);
    msg->data_len = SVCMGR_DIAG_REPLY_MSG_LEN;
    msg->attached_handle = HANDLE_INVALID;
    msg->attached_rights = RIGHT_NONE;
}

static inline int svcmgr_proto_proc_exit_valid(const struct KChanMsg *msg) {
    return msg && msg->type == PROC_EVENT_MSG_EXIT && msg->data_len == PROC_EVENT_MSG_LEN;
}

static inline void svcmgr_proto_proc_exit_decode(const struct KChanMsg *msg,
                                                 handle_id_t *watched_h,
                                                 uint32_t *cookie) {
    if (watched_h) *watched_h = (handle_id_t)svcmgr_proto_read_u32(&msg->data[PROC_EVENT_OFF_HANDLE]);
    if (cookie) *cookie = svcmgr_proto_read_u32(&msg->data[PROC_EVENT_OFF_COOKIE]);
}
#endif

#endif
