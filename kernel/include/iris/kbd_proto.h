#ifndef IRIS_KBD_PROTO_H
#define IRIS_KBD_PROTO_H

#ifndef __ASSEMBLER__
#include <stdint.h>
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
 *     HISTORICAL (retired in Fase 7.6): IRQ1 was delivered as this KChannel
 *     message. kbd now routes IRQ1 to a KNotification (irq_notify=1) and no
 *     longer dispatches this opcode; the constant stays defined because it
 *     shares its value with the generic IRQ_MSG_TYPE_SIGNAL (irq_routing.h)
 *     still used by channel-routed IRQs.
 *     The kbd service reads the scancode from port 0x60 via its KIoPort cap.
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
 * Ownership rules:
 *   - HELLO and STATUS carry no required attached handles.
 *   - STATUS may optionally attach a one-shot reply channel with RIGHT_WRITE;
 *     sending the request move-consumes that handle on successful SYS_CHAN_SEND.
 *   - SUBSCRIBE requires the client to attach a KChannel write handle with
 *     RIGHT_WRITE | RIGHT_TRANSFER; kbd takes ownership of that attached
 *     handle and replaces any previous subscriber.
 *
 * ── Current status ───────────────────────────────────────────────────────────
 *   The protocol is live and consumed by the current `kbd` service and clients.
 *   `irq_routing.c` uses IRQ_MSG_TYPE_SIGNAL from irq_routing.h.
 */

/* Protocol version */
#define KBD_PROTO_VERSION     1u

/* ── Request opcodes (client → service) ─────────────────────────────────── */
#define KBD_MSG_HELLO         0x00020001u
#define KBD_MSG_STATUS        0x00020002u

/* ── Reply opcodes (service → client) ───────────────────────────────────── */
#define KBD_MSG_HELLO_REPLY   0x80020001u
#define KBD_MSG_STATUS_REPLY  0x80020002u

/* ── IRQ channel message (HISTORICAL for kbd — retired in Fase 7.6) ──────
 * This opcode is set by the generic IRQ routing layer (irq_routing_signal)
 * for CHANNEL-routed IRQs.  It is NOT in the 0x0002XXXX namespace because
 * the routing layer is service-agnostic.  The value matches
 * IRQ_MSG_TYPE_SIGNAL (irq_routing.h) and must be kept in sync with it.
 * kbd no longer consumes it: IRQ1 is routed to a KNotification since
 * Fase 7.6 and the kbd server does not dispatch this type anymore.   */
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

/* ── KBD_MSG_SUBSCRIBE (0x00020003) client → service ─────────────────────
 *   Subscribe to raw scancode events.  The client attaches a KChannel write
 *   handle (RIGHT_WRITE | RIGHT_TRANSFER).  kbd stores the handle and
 *   forwards one KBD_MSG_SCANCODE_EVENT per key event until the handle is
 *   replaced by a new SUBSCRIBE or the channel is sealed.
 *   Only one subscriber at a time; a new SUBSCRIBE replaces the old one.
 *   data_len = KBD_MSG_SUBSCRIBE_LEN (0; no payload beyond attached handle).
 *   Receives KBD_MSG_SUBSCRIBE_REPLY.
 */
#define KBD_MSG_SUBSCRIBE         0x00020003u

/* ── KBD_MSG_SUBSCRIBE_REPLY (0x80020003) service → client ──────────────
 *   data[0..3] int32_t err: 0 = OK, <0 = iris_error_t.
 */
#define KBD_MSG_SUBSCRIBE_REPLY   0x80020003u

/* ── KBD_MSG_SCANCODE_EVENT (0x80020004) service → subscriber  [no reply]
 *   Forwarded by kbd to the subscriber channel on every PS/2 scancode.
 *   data[KBD_MSG_OFF_SC_EVENT_CODE] uint8_t: raw PS/2 byte
 *     (same encoding as KBD_MSG_IRQ_SCANCODE: bit 7 = key release).
 */
#define KBD_MSG_SCANCODE_EVENT    0x80020004u

#define KBD_MSG_SUBSCRIBE_LEN           0u
#define KBD_MSG_OFF_SUBSCRIBE_REPLY_ERR 0u
#define KBD_MSG_SUBSCRIBE_REPLY_LEN     4u
#define KBD_MSG_OFF_SC_EVENT_CODE       0u
#define KBD_MSG_SCANCODE_EVENT_LEN      1u

/* ── C-only helpers ─────────────────────────────────────────────────────── */
/* Fase 13/Track G: the KChanMsg-based kbd_proto_* inline helpers (HELLO/
 * STATUS/IRQ_SCANCODE builders+decoders) are retired with the KChannel
 * object — kbd is endpoint + notification only (kbd_ep_proto.h). */

#endif /* IRIS_KBD_PROTO_H */
