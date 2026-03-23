#ifndef IRIS_KBD_PROTO_H
#define IRIS_KBD_PROTO_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#endif

/*
 * IRIS Keyboard Service Protocol
 *
 * ── Overview ─────────────────────────────────────────────────────────────────
 * The keyboard service (kbd) is a ring-3 process that:
 *   1. Receives raw PS/2 scancodes from the kernel IRQ routing layer via a
 *      dedicated KChannel (service/IRQ channel).
 *   2. Accepts request messages from clients on the same service channel.
 *   3. Replies to clients over a separate reply channel whose handle is
 *      delivered to the client by svcmgr at discovery time.
 *
 * Service startup is handled by svcmgr using SVCMGR_MSG_BOOTSTRAP_HANDLE
 * (defined in svcmgr_proto.h).  kbd itself does not participate in discovery;
 * it only waits for two handle deliveries over its private bootstrap channel
 * and then enters its main service loop.
 *
 * ── KChanMsg wire layout (84 bytes total) ────────────────────────────────────
 *   offset  0: uint32_t type             → KBD_MSG_* opcode
 *   offset  4: uint32_t sender_id        → 0 = kernel, N = task_id
 *   offset  8: uint8_t  data[64]         → payload (layout per opcode below)
 *   offset 72: uint32_t data_len         → payload bytes used
 *   offset 76: handle_id_t attached_handle
 *   offset 80: iris_rights_t attached_rights
 *
 * ── Opcode namespace ─────────────────────────────────────────────────────────
 *   Requests  (client → service): 0x0002XXXX
 *   Replies   (service → client): 0x8002XXXX
 *   IRQ notif (kernel  → service): defined by irq_routing layer (see below)
 *
 *   VFS uses 0x0001XXXX / 0x8001XXXX — no collision.
 *   svcmgr uses 0x000X / 0x800X     — no collision.
 *
 * ── Message table ────────────────────────────────────────────────────────────
 *
 *   KBD_MSG_HELLO        (0x00020001) client → service
 *     Zero-payload probe.  Confirms the service is alive and accepting
 *     requests.  Receives KBD_MSG_HELLO_REPLY.
 *
 *   KBD_MSG_HELLO_REPLY  (0x80020001) service → client
 *     data[0..3] int32_t err: 0 = OK, <0 = iris_error_t.
 *
 *   KBD_MSG_STATUS       (0x00020002) client → service
 *     Zero-payload status query.  Receives KBD_MSG_STATUS_REPLY.
 *     May optionally carry a transferred one-shot reply channel handle with
 *     RIGHT_WRITE; otherwise the service replies on its shared reply channel.
 *
 *   KBD_MSG_STATUS_REPLY (0x80020002) service → client
 *     data[0..3] int32_t  err:   0 = OK, <0 = iris_error_t.
 *     data[4..7] uint32_t flags: KBD_STATUS_* bits.
 *
 *   KBD_MSG_IRQ_SCANCODE (0x00000004) kernel → service  [no reply]
 *     Sent by irq_routing_signal() when hardware IRQ1 fires.
 *     Value matches IRQ_MSG_TYPE_SIGNAL (irq_routing.h); must stay in sync.
 *     data[0] uint8_t: raw PS/2 scancode byte.
 *       Bit 7 set: key release (scancode & 0x7F = base key code).
 *       Bit 7 clr: key press.
 *     No reply is sent; the service processes and loops.
 *
 * ── Status flag bits (KBD_STATUS_*) ─────────────────────────────────────────
 *   KBD_STATUS_READY   (bit 0): controller initialized, accepting input
 *   KBD_STATUS_PS2_OK  (bit 1): PS/2 port I/O confirmed responding
 *   KBD_STATUS_NORMAL          both bits set — expected healthy-path value
 *
 * ── Protocol version ─────────────────────────────────────────────────────────
 *   KBD_PROTO_VERSION 1
 *   Bump when any wire-level field layout changes.
 *
 * ── Phase status ─────────────────────────────────────────────────────────────
 *   Phase 10/current: protocol formalized.  kbd_server.S and user_init.S
 *   both consume this header and use named constants throughout.
 *   irq_routing.c uses IRQ_MSG_TYPE_SIGNAL from irq_routing.h.
 */

/* Protocol version */
#define KBD_PROTO_VERSION     1u

/* ── Request opcodes (client → service) ─────────────────────────────────── */
#define KBD_MSG_HELLO         0x00020001u
#define KBD_MSG_STATUS        0x00020002u

/* ── Reply opcodes (service → client) ───────────────────────────────────── */
#define KBD_MSG_HELLO_REPLY   0x80020001u
#define KBD_MSG_STATUS_REPLY  0x80020002u

/* ── IRQ notification (kernel → service, via irq_routing layer) ──────────
 * This opcode is set by the generic IRQ routing layer (irq_routing_signal).
 * It is NOT in the 0x0002XXXX namespace because the routing layer is service-
 * agnostic.  The value matches IRQ_MSG_TYPE_SIGNAL (irq_routing.h) and must
 * be kept in sync with it.  kbd_server validates type == KBD_MSG_IRQ_SCANCODE
 * to distinguish hardware events from client requests.               */
#define KBD_MSG_IRQ_SCANCODE  0x00000004u

/* ── KBD_MSG_HELLO payload ──────────────────────────────────────────────── */
#define KBD_MSG_HELLO_LEN           0u   /* no payload */

/* ── KBD_MSG_HELLO_REPLY payload ────────────────────────────────────────── */
#define KBD_MSG_OFF_HELLO_REPLY_ERR 0u   /* int32_t: 0=OK, <0=iris_error_t */
#define KBD_MSG_HELLO_REPLY_LEN     4u

/* ── KBD_MSG_STATUS payload ─────────────────────────────────────────────── */
#define KBD_MSG_STATUS_LEN            0u   /* no payload */

/* ── KBD_MSG_STATUS_REPLY payload ───────────────────────────────────────── */
#define KBD_MSG_OFF_STATUS_REPLY_ERR    0u   /* int32_t:  0=OK, <0=iris_error_t */
#define KBD_MSG_OFF_STATUS_REPLY_FLAGS  4u   /* uint32_t: KBD_STATUS_* bits     */
#define KBD_MSG_STATUS_REPLY_LEN        8u

/* Status flag bits */
#define KBD_STATUS_READY    0x01u  /* controller initialized, accepting input */
#define KBD_STATUS_PS2_OK   0x02u  /* PS/2 port I/O confirmed responding      */
/* Combined healthy-path value: both READY and PS2_OK bits set */
#define KBD_STATUS_NORMAL   0x03u

/* ── KBD_MSG_IRQ_SCANCODE payload ───────────────────────────────────────── */
#define KBD_MSG_OFF_IRQ_SCANCODE  0u   /* uint8_t: raw PS/2 scancode byte */
#define KBD_MSG_IRQ_SCANCODE_LEN  1u

/* ── C-only helpers ─────────────────────────────────────────────────────── */
#ifndef __ASSEMBLER__

static inline void kbd_proto_write_u32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >>  8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static inline uint32_t kbd_proto_read_u32(const uint8_t *src) {
    return ((uint32_t)src[0])          |
           ((uint32_t)src[1] <<  8)    |
           ((uint32_t)src[2] << 16)    |
           ((uint32_t)src[3] << 24);
}

/* Validate an inbound KBD_MSG_HELLO request. */
static inline int kbd_proto_hello_valid(const struct KChanMsg *msg) {
    return msg != 0 &&
           msg->type     == KBD_MSG_HELLO &&
           msg->data_len == KBD_MSG_HELLO_LEN;
}

/* Initialise a KBD_MSG_HELLO_REPLY message in *msg. */
static inline void kbd_proto_hello_reply_init(struct KChanMsg *msg, int32_t err) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = KBD_MSG_HELLO_REPLY;
    kbd_proto_write_u32(&msg->data[KBD_MSG_OFF_HELLO_REPLY_ERR], (uint32_t)err);
    msg->data_len        = KBD_MSG_HELLO_REPLY_LEN;
    msg->attached_handle = HANDLE_INVALID;
    msg->attached_rights = RIGHT_NONE;
}

/* Validate an inbound KBD_MSG_STATUS request. */
static inline int kbd_proto_status_valid(const struct KChanMsg *msg) {
    return msg != 0 &&
           msg->type     == KBD_MSG_STATUS &&
           msg->data_len == KBD_MSG_STATUS_LEN &&
           (msg->attached_handle == HANDLE_INVALID ||
            (msg->attached_rights & RIGHT_WRITE) != 0);
}

/* Initialise a KBD_MSG_STATUS_REPLY message in *msg. */
static inline void kbd_proto_status_reply_init(struct KChanMsg *msg,
                                               int32_t err,
                                               uint32_t flags) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = KBD_MSG_STATUS_REPLY;
    kbd_proto_write_u32(&msg->data[KBD_MSG_OFF_STATUS_REPLY_ERR],   (uint32_t)err);
    kbd_proto_write_u32(&msg->data[KBD_MSG_OFF_STATUS_REPLY_FLAGS], flags);
    msg->data_len        = KBD_MSG_STATUS_REPLY_LEN;
    msg->attached_handle = HANDLE_INVALID;
    msg->attached_rights = RIGHT_NONE;
}

/* Validate an inbound KBD_MSG_IRQ_SCANCODE notification. */
static inline int kbd_proto_irq_scancode_valid(const struct KChanMsg *msg) {
    return msg != 0 &&
           msg->type     == KBD_MSG_IRQ_SCANCODE &&
           msg->data_len == KBD_MSG_IRQ_SCANCODE_LEN;
}

#endif /* !__ASSEMBLER__ */

#endif /* IRIS_KBD_PROTO_H */
